#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>

#include "baudsh_win.hpp"

template <typename T>
static inline T load_proc(HMODULE mod, const char* name) {
    FARPROC fp = GetProcAddress(mod, name);
    // C++-correct way: cast through void* (suppresses -Wcast-function-type on GCC/MinGW)
    return reinterpret_cast<T>(reinterpret_cast<void*>(fp));
}

// -------------------- ConPTY compatibility --------------------
// Some MinGW-w64 headers (or older Windows SDKs) may not define ConPTY types/constants.
// We provide minimal fallbacks so the code can compile, then resolve symbols at runtime.
//
// Use a private opaque handle alias: HPCON is a typedef, so #ifndef cannot
// reliably detect it across Windows SDK versions. The APIs are loaded below.
using PseudoConsoleHandle = void*;

// PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE is the attribute used with UpdateProcThreadAttribute.
// Many references (including Microsoft examples) use value 0x00020016.
#ifndef PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE
#define PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE 0x00020016
#endif

#ifndef PSEUDOCONSOLE_INHERIT_CURSOR
#define PSEUDOCONSOLE_INHERIT_CURSOR 0x1
#endif

#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif


// -------------------- ConPTY typedefs --------------------

typedef HRESULT (WINAPI *CreatePseudoConsole_t)(COORD, HANDLE, HANDLE, DWORD, PseudoConsoleHandle*);
typedef void    (WINAPI *ClosePseudoConsole_t)(PseudoConsoleHandle);
typedef HRESULT (WINAPI *ResizePseudoConsole_t)(PseudoConsoleHandle, COORD);

static CreatePseudoConsole_t pCreatePseudoConsole = nullptr;
static ClosePseudoConsole_t  pClosePseudoConsole  = nullptr;
static ResizePseudoConsole_t pResizePseudoConsole = nullptr;

// -------------------- small helpers --------------------

static void win_die(const char* msg) {
    DWORD e = GetLastError();
    std::string m = std::string("baudsh(win): ") + msg + " (GetLastError=" + std::to_string((unsigned long)e) + ")";
    throw std::runtime_error(m);
}

static void close_handle(HANDLE& h) {
    if (h && h != INVALID_HANDLE_VALUE) {
        CloseHandle(h);
        h = nullptr;
    }
}

static std::wstring utf8_to_utf16(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.c_str(), (int)s.size(), nullptr, 0);
    if (n <= 0) throw std::runtime_error("baudsh(win): invalid UTF-8 text");
    std::wstring out((size_t)n, L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.c_str(), (int)s.size(), out.data(), n))
        throw std::runtime_error("baudsh(win): could not convert UTF-8 text");
    return out;
}


static std::string utf16_to_utf8(const wchar_t* s) {
    if (!s || !*s) return {};
    int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) throw std::runtime_error("baudsh(win): invalid Unicode command line");
    std::string out((size_t)n, '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s, -1, out.data(), n, nullptr, nullptr))
        throw std::runtime_error("baudsh(win): could not convert Unicode command line");
    out.pop_back();
    return out;
}

// Obtain native Unicode arguments rather than treating the CRT's current
// system-codepage argv as UTF-8. Resolve shell32 dynamically so existing
// build commands do not need an additional link library.
static std::vector<std::string> unicode_command_line_args() {
    using ParseCommandLine = LPWSTR* (WINAPI *)(LPCWSTR, int*);
    HMODULE shell = LoadLibraryW(L"shell32.dll");
    if (!shell) throw std::runtime_error("baudsh(win): cannot load shell32.dll");
    auto parse = load_proc<ParseCommandLine>(shell, "CommandLineToArgvW");
    if (!parse) { FreeLibrary(shell); throw std::runtime_error("baudsh(win): cannot parse command line"); }
    int count = 0;
    LPWSTR* values = parse(GetCommandLineW(), &count);
    if (!values) { FreeLibrary(shell); throw std::runtime_error("baudsh(win): cannot parse command line"); }
    std::vector<std::string> result;
    try {
        for (int i = 0; i < count; ++i) result.push_back(utf16_to_utf8(values[i]));
    } catch (...) {
        LocalFree(values);
        FreeLibrary(shell);
        throw;
    }
    LocalFree(values);
    FreeLibrary(shell);
    return result;
}


// RAII guard to restore console modes even if we return early.
// NOTE: This only runs on normal returns; hard termination (TerminateProcess/ExitProcess)
// will bypass it, but it fixes the common "console left unusable" issue.
struct ConsoleModeGuard {
    HANDLE hin = nullptr;
    HANDLE hout = nullptr;
    DWORD in_mode_old = 0;
    DWORD out_mode_old = 0;
    bool has_in = false;
    bool has_out = false;

    ConsoleModeGuard() {
        hin = GetStdHandle(STD_INPUT_HANDLE);
        hout = GetStdHandle(STD_OUTPUT_HANDLE);
        if (hin && GetConsoleMode(hin, &in_mode_old)) has_in = true;
        if (hout && GetConsoleMode(hout, &out_mode_old)) has_out = true;
    }

    void apply_raw_vt() {
        if (has_in) {
            DWORD m = in_mode_old;
            m &= ~ENABLE_VIRTUAL_TERMINAL_INPUT;
            // Disable cooked input so keystrokes stream to the child PTY (like POSIX raw mode).
            m &= ~(ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT | ENABLE_PROCESSED_INPUT);
            m |= ENABLE_WINDOW_INPUT;
            SetConsoleMode(hin, m);
        }
        if (has_out) {
            DWORD m = out_mode_old;
            m |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
            SetConsoleMode(hout, m);
        }
    }

    void restore() {
        if (has_in) {
            // Prevent leftover escape bytes (e.g. from function keys) from hitting the shell prompt.
            FlushConsoleInputBuffer(hin);
            SetConsoleMode(hin, in_mode_old);
        }
        if (has_out) {
            SetConsoleMode(hout, out_mode_old);
        }
    }

    ~ConsoleModeGuard() {
        restore();
    }
};

// RAII guard to restore console code pages (fixes "┬á" / NBSP issues)
struct ConsoleCodePageGuard {
    UINT in_cp = 0;
    UINT out_cp = 0;
    bool ok = false;

    ConsoleCodePageGuard() {
        in_cp = GetConsoleCP();
        out_cp = GetConsoleOutputCP();
        ok = (in_cp != 0 && out_cp != 0);
    }

    void apply_utf8() {
        if (!ok) return;
        SetConsoleCP(65001);
        SetConsoleOutputCP(65001);
    }

    ~ConsoleCodePageGuard() {
        if (!ok) return;
        SetConsoleCP(in_cp);
        SetConsoleOutputCP(out_cp);
    }
};


// Escape backslashes preceding quotes and the closing quote according to
// the Windows C runtime argument parsing convention.
static std::wstring quote_arg(const std::wstring& a) {
    std::wstring out(1, L'"');
    size_t slashes = 0;
    for (wchar_t ch : a) {
        if (ch == L'\\') { ++slashes; continue; }
        if (ch == L'"') {
            out.append(slashes * 2 + 1, L'\\');
        } else {
            out.append(slashes, L'\\');
        }
        slashes = 0;
        out.push_back(ch);
    }
    out.append(slashes * 2, L'\\');
    out.push_back(L'"');
    return out;
}

static std::wstring build_cmdline_from_utf8_argv(const std::vector<std::string>& argv_utf8) {
    std::wstring cmd;
    for (size_t i = 0; i < argv_utf8.size(); ++i) {
        if (i) cmd.push_back(L' ');
        cmd += quote_arg(utf8_to_utf16(argv_utf8[i]));
    }
    return cmd;
}

// -------------------- token bucket (same spirit as POSIX) --------------------

struct TokenBucket {
    double rate_bps = 0.0;
    double capacity = 0.0;
    double tokens = 0.0;
    std::chrono::steady_clock::time_point last;

    TokenBucket() : last(std::chrono::steady_clock::now()) {}

    void init(double rate, double cap) {
        rate_bps = rate;
        capacity = cap;
        tokens = cap;
        last = std::chrono::steady_clock::now();
    }

    void refill() {
        auto now = std::chrono::steady_clock::now();
        double dt = std::chrono::duration<double>(now - last).count();
        if (dt < 0.0) dt = 0.0;
        last = now;
        tokens = std::min(capacity, tokens + dt * rate_bps);
    }

    size_t available(size_t want) const {
        if (tokens <= 0.0) return 0;
        return (size_t)std::min(tokens, (double)want);
    }

    void consume(size_t n) {
        tokens -= (double)n;
        if (tokens < 0.0) tokens = 0.0;
    }
};

static double baud_to_bytes_per_sec(int baud, int data_bits, char parity, double stop_bits) {
    const double start_bits = 1.0;
    const double parity_bits = ((parity == 'e' || parity == 'o') ? 1.0 : 0.0);
    const double total_bits = start_bits + (double)data_bits + parity_bits + stop_bits;
    return (double)baud / total_bits;
}

// -------------------- escape sequence parsing (ported from POSIX style) --------

static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    c = std::tolower((unsigned char)c);
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    return -1;
}

// Supports: "1B 5B 31 38 3B 35 7E" and "\x1b[18;5~" and "\e[18;5~"
static std::vector<unsigned char> parse_raw_seq(const std::string& s0) {
    std::string s = s0;
    std::vector<unsigned char> out;
    out.reserve(s.size());

    // If it looks like hex bytes separated by whitespace, parse that.
    bool maybe_hex_bytes = false;
    for (char c : s) {
        if (std::isxdigit((unsigned char)c) || std::isspace((unsigned char)c)) {
            maybe_hex_bytes = true;
        } else {
            maybe_hex_bytes = false;
            break;
        }
    }
    if (maybe_hex_bytes) {
        size_t i = 0;
        while (i < s.size()) {
            while (i < s.size() && std::isspace((unsigned char)s[i])) i++;
            if (i >= s.size()) break;
            if (i + 1 >= s.size()) break;
            int h1 = hexval(s[i]);
            int h2 = hexval(s[i + 1]);
            if (h1 < 0 || h2 < 0) break;
            out.push_back((unsigned char)((h1 << 4) | h2));
            i += 2;
            while (i < s.size() && !std::isspace((unsigned char)s[i])) i++;
        }
        return out;
    }

    // Otherwise parse backslash escapes.
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = (unsigned char)s[i];
        if (c != '\\') {
            out.push_back(c);
            continue;
        }
        if (i + 1 >= s.size()) { out.push_back('\\'); break; }

        char n = s[++i];
        if (n == 'e') out.push_back(0x1B);
        else if (n == 'n') out.push_back('\n');
        else if (n == 'r') out.push_back('\r');
        else if (n == 't') out.push_back('\t');
        else if (n == '\\') out.push_back('\\');
        else if (n == 'x') {
            if (i + 2 < s.size()) {
                int h1 = hexval(s[i + 1]);
                int h2 = hexval(s[i + 2]);
                if (h1 >= 0 && h2 >= 0) {
                    out.push_back((unsigned char)((h1 << 4) | h2));
                    i += 2;
                } else {
                    out.push_back('\\'); out.push_back('x');
                }
            } else {
                out.push_back('\\'); out.push_back('x');
            }
        } else {
            out.push_back((unsigned char)n);
        }
    }

    return out;
}

static void print_escape_help(const std::string& exit_key_spec, int arm_ms) {
    // stderr to avoid mixing with child output too much.
    std::fprintf(stderr,
        "\r\nEscape controls (prefix: %s)\r\n"
        "  %s  ? / h\tShow this help\r\n"
        "  %s  %s   \tSend literal prefix to the child\r\n"
        "  %s  q    \tQuit immediately (TerminateProcess)\r\n"
        "\r\nNotes:\r\n"
        "  • Prefix becomes active after %d ms (see --exit-arm-ms).\r\n"
        "  • Prefix is a byte sequence; it may vary by terminal.\r\n"
        "    Use --exit-key raw:<seq> if Ctrl+F7 is not detected.\r\n\r\n",
        exit_key_spec.c_str(),
        exit_key_spec.c_str(),
        exit_key_spec.c_str(), exit_key_spec.c_str(),
        exit_key_spec.c_str(),
        arm_ms
    );
}

// -------------------- CLI args (mirrors POSIX) --------------------

struct Args {
    int baud = -1;
    int data_bits = 8;
    char parity = 'n';
    double stop_bits = 1.0;
    int burst_ms = 200;

    std::string exit_key = "ctrl+f7";
    int exit_arm_ms = 250;

    bool print_effective = false;

    bool has_cmd = false;
    std::vector<std::string> cmd;
};

static void print_help(const char* prog) {
    std::fprintf(stdout,
        "baudsh — throttle a PTY to simulate serial baud rates (Windows ConPTY)\n\n"
        "Usage:\n"
        "  %s --baud <rate> [options] [--cmd <command...>]\n\n"
        "Options:\n"
        "  --baud <rate>          Required, unless specified in config (e.g. 2400)\n"
        "  --data-bits <5|6|7|8>  Default: 8\n"
        "  --parity <n|e|o>       Default: n\n"
        "  --stop-bits <1|1.5|2>  Default: 1\n"
        "  --burst-ms <ms>        Default: 200\n"
        "  --exit-key <spec>      ctrl+f7 | raw:<seq> | none (default: ctrl+f7)\n"
        "  --exit-arm-ms <ms>     Default: 250\n"
        "  --cmd <...>            Run command. If single string, uses cmd.exe /S /C.\n"
        "                         [must be last flag if used]\n"
        "  --print-effective      Print resolved settings and exit\n"
        "  --print-default        Print built-in default config template and exit\n"
        "  --autocomp <shell>     Print autocomplete for dash|bash|zsh|fish and exit\n"
        "  --version              Print version and exit\n"
        "  -h, --help             Show this help\n\n"
        "Config (same keys as CLI, overrides defaults; CLI overrides config):\n"
        "  %%APPDATA%%/baudsh/baudsh.conf\n"
        "  ~/.baudsh.rc   (higher priority)\n\n"
        "Default shell (Windows): cmd.exe\n",
        prog
    );
}

static bool parse_args(int argc, char** argv, Args& out) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];

        if (a == "-h" || a == "--help") {
            print_help(argv[0]);
            out.baud = -9999; // sentinel: help printed
            return false;
        } else if (a == "--baud") {
            if (i + 1 >= argc) return false;
            if (!baudsh::parse_int(argv[++i], out.baud) || out.baud <= 0) return false;
        } else if (a == "--data-bits") {
            if (i + 1 >= argc) return false;
            if (!baudsh::parse_int(argv[++i], out.data_bits)) return false;
            if (!(out.data_bits == 5 || out.data_bits == 6 || out.data_bits == 7 || out.data_bits == 8)) return false;
        } else if (a == "--parity") {
            if (i + 1 >= argc) return false;
            std::string p = argv[++i];
            if (p.size() != 1) return false;
            char c = (char)std::tolower((unsigned char)p[0]);
            if (!(c == 'n' || c == 'e' || c == 'o')) return false;
            out.parity = c;
        } else if (a == "--stop-bits") {
            if (i + 1 >= argc) return false;
            if (!baudsh::parse_double(argv[++i], out.stop_bits)) return false;
            if (!(out.stop_bits == 1.0 || out.stop_bits == 1.5 || out.stop_bits == 2.0)) return false;
        } else if (a == "--burst-ms") {
            if (i + 1 >= argc) return false;
            if (!baudsh::parse_int(argv[++i], out.burst_ms) || out.burst_ms < 1) return false;
        } else if (a == "--exit-key") {
            if (i + 1 >= argc) return false;
            out.exit_key = argv[++i];
        } else if (a == "--exit-arm-ms") {
            if (i + 1 >= argc) return false;
            if (!baudsh::parse_int(argv[++i], out.exit_arm_ms) || out.exit_arm_ms < 0) return false;
        } else if (a == "--cmd") {
            if (i + 1 >= argc) return false;
            out.has_cmd = true;
            out.cmd.clear();
            for (int j = i + 1; j < argc; ++j) out.cmd.emplace_back(argv[j]);
            break;
        } else if (a == "--print-effective") {
            out.print_effective = true;
        } else if (a == "--autocomp" || a == "--version" || a == "--print-default") {
            // handled by baudsh::handle_early_flags()
            return false;
        } else {
            return false;
        }
    }
    return (out.baud > 0 || (out.baud == -1 && out.print_effective)) &&
        (out.data_bits >= 5 && out.data_bits <= 8) &&
        (out.parity == 'n' || out.parity == 'e' || out.parity == 'o') &&
        (out.stop_bits == 1.0 || out.stop_bits == 1.5 || out.stop_bits == 2.0) &&
        out.burst_ms >= 1 && out.exit_arm_ms >= 0;
}

// -------------------- ConPTY load + spawn --------------------

static bool load_conpty_symbols() {
    HMODULE hKernel = GetModuleHandleW(L"kernel32.dll");
    if (!hKernel) win_die("GetModuleHandleW(kernel32.dll) failed");

    pCreatePseudoConsole = load_proc<CreatePseudoConsole_t>(hKernel, "CreatePseudoConsole");
    pClosePseudoConsole  = load_proc<ClosePseudoConsole_t>(hKernel, "ClosePseudoConsole");
    pResizePseudoConsole = load_proc<ResizePseudoConsole_t>(hKernel, "ResizePseudoConsole");

    if (!pCreatePseudoConsole || !pClosePseudoConsole || !pResizePseudoConsole) {
        std::cerr << "baudsh(win): ConPTY APIs not available. Need Windows 10 1809+.\n";
        return false;
    }
    return true;
}

// Write-all helper
static bool write_all(HANDLE h, const unsigned char* p, size_t n) {
    while (n > 0) {
        DWORD w = 0;
        if (!WriteFile(h, p, (DWORD)std::min(n, (size_t)0x7fffffff), &w, nullptr)) return false;
        if (w == 0) return false;
        p += w;
        n -= w;
    }
    return true;
}

struct OutputCPGuard {
    UINT old_cp;
    OutputCPGuard() {
        old_cp = GetConsoleOutputCP();
        SetConsoleOutputCP(65001);
    }
    ~OutputCPGuard() {
        SetConsoleOutputCP(old_cp);
    }
};

// -------------------- main Windows backend --------------------

int baudsh_windows_main(int argc, char** argv) {
    try {
    std::vector<std::string> unicode_args = unicode_command_line_args();
    std::vector<char*> unicode_argv;
    unicode_argv.reserve(unicode_args.size() + 1);
    for (auto& arg : unicode_args) unicode_argv.push_back(arg.data());
    unicode_argv.push_back(nullptr);
    argc = (int)unicode_args.size();
    argv = unicode_argv.data();
    // early flags: --version / --print-default / --autocomp
    baudsh::handle_early_flags(argc, argv);

    Args args;
    // apply rc defaults
    baudsh::load_rc_files(args);

    if (!parse_args(argc, argv, args)) {
        if (args.baud == -9999) return 0; // help already printed
        print_help(argv[0]);
        return 64;
    }

    if (args.print_effective) {
        std::printf(
            "baud=%d\n"
            "data-bits=%d\n"
            "parity=%c\n"
            "stop-bits=%.1f\n"
            "burst-ms=%d\n"
            "exit-key=%s\n"
            "exit-arm-ms=%d\n",
            args.baud,
            args.data_bits,
            args.parity,
            args.stop_bits,
            args.burst_ms,
            args.exit_key.c_str(),
            args.exit_arm_ms
        );
        return 0;
    }

    if (args.baud <= 0) {
        std::fprintf(stderr, "baudsh(win): --baud is required (or set baud=<n> in rc file)\n");
        return 64;
    }

    if (!load_conpty_symbols()) return 1;
    OutputCPGuard cp_guard;
    ConsoleModeGuard cmg;
    cmg.apply_raw_vt();

    // Resolve exit prefix sequence
    bool exit_enabled = true;
    bool exit_is_ctrl_f7 = false;
    std::vector<unsigned char> exit_seq;
    {
        std::string ek = args.exit_key;
        for (auto& ch : ek) ch = (char)std::tolower((unsigned char)ch);

        if (ek.empty() || ek == "none") {
            exit_enabled = false;
        } else if (ek == "ctrl+f7") {
            exit_is_ctrl_f7 = true;
            const unsigned char seq[] = {0x1B,'[','1','8',';','5','~'};
            exit_seq.assign(seq, seq + sizeof(seq));
        } else if (ek.size() == 6 && ek.rfind("ctrl+", 0) == 0 &&
                   ((ek[5] >= 'a' && ek[5] <= 'z') || (ek[5] >= '[' && ek[5] <= '_'))) {
            exit_seq.push_back((unsigned char)(std::toupper((unsigned char)ek[5]) & 0x1f));
        } else if (ek.rfind("raw:", 0) == 0) {
            exit_seq = parse_raw_seq(args.exit_key.substr(4));
            if (exit_seq.empty()) exit_enabled = false;
        } else {
            exit_seq.assign(args.exit_key.begin(), args.exit_key.end());
            if (exit_seq.empty()) exit_enabled = false;
        }
    }

    // Direct commands use CRT argv quoting. cmd.exe /S /C instead strips the
    // outer pair of quotes and interprets the command inside as shell syntax.
    std::wstring application;
    std::wstring cmdline;
    if (!args.has_cmd || args.cmd.size() == 1) {
        std::vector<wchar_t> system_dir(32768);
        UINT length = GetSystemDirectoryW(system_dir.data(), (UINT)system_dir.size());
        if (!length || length >= system_dir.size()) win_die("GetSystemDirectoryW failed");
        application = std::wstring(system_dir.data(), length) + L"\\cmd.exe";
        cmdline = quote_arg(application);
        if (args.has_cmd) cmdline += L" /D /S /C \"" + utf8_to_utf16(args.cmd[0]) + L"\"";
    } else {
        cmdline = build_cmdline_from_utf8_argv(args.cmd);
    }

    // ConPTY pipes: parent<->pty
    HANDLE hPipeToPty_Read = nullptr;
    HANDLE hPipeToPty_Write = nullptr;
    HANDLE hPipeFromPty_Read = nullptr;
    HANDLE hPipeFromPty_Write = nullptr;

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    if (!CreatePipe(&hPipeToPty_Read, &hPipeToPty_Write, &sa, 0)) win_die("CreatePipe(to-pty) failed");
    if (!CreatePipe(&hPipeFromPty_Read, &hPipeFromPty_Write, &sa, 0)) win_die("CreatePipe(from-pty) failed");

    COORD size{};
    size.X = 120;
    size.Y = 30;
    CONSOLE_SCREEN_BUFFER_INFO initial_csbi{};
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &initial_csbi)) {
        size.X = initial_csbi.srWindow.Right - initial_csbi.srWindow.Left + 1;
        size.Y = initial_csbi.srWindow.Bottom - initial_csbi.srWindow.Top + 1;
    }

    PseudoConsoleHandle hPcon = nullptr;
    HRESULT hr = pCreatePseudoConsole(size, hPipeToPty_Read, hPipeFromPty_Write, 0, &hPcon);
    if (FAILED(hr)) {
        std::cerr << "baudsh(win): CreatePseudoConsole failed (HRESULT=0x" << std::hex << (unsigned long)hr << ")\n";
        return 1;
    }

    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);

    SIZE_T attr_list_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_list_size);
    std::vector<unsigned char> attr_buf(attr_list_size);
    si.lpAttributeList = (PPROC_THREAD_ATTRIBUTE_LIST)attr_buf.data();

    if (!InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &attr_list_size)) {
        win_die("InitializeProcThreadAttributeList failed");
    }

    if (!UpdateProcThreadAttribute(
            si.lpAttributeList,
            0,
            PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
            hPcon,
            sizeof(hPcon),
            nullptr,
            nullptr)) {
        win_die("UpdateProcThreadAttribute(PSEUDOCONSOLE) failed");
    }

    PROCESS_INFORMATION pi{};

    BOOL ok = CreateProcessW(
        application.empty() ? nullptr : application.c_str(),
        cmdline.data(),
        nullptr,
        nullptr,
        FALSE,
        EXTENDED_STARTUPINFO_PRESENT,
        nullptr,
        nullptr,
        &si.StartupInfo,
        &pi
    );

    DeleteProcThreadAttributeList(si.lpAttributeList);

    if (!ok) win_die("CreateProcessW failed");

    // Close PTY ends not used by parent
    close_handle(hPipeToPty_Read);
    close_handle(hPipeFromPty_Write);

    // Throttle setup (same as POSIX)
    const double bps = baud_to_bytes_per_sec(args.baud, args.data_bits, args.parity, args.stop_bits);
    const double burst_seconds = std::max(0.01, (double)args.burst_ms / 1000.0);
    const double capacity = std::max(1.0, bps * burst_seconds);

    TokenBucket in_bucket;
    TokenBucket out_bucket;
    in_bucket.init(bps, capacity);
    out_bucket.init(bps, capacity);

    HANDLE hStdin  = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE hStdout = GetStdHandle(STD_OUTPUT_HANDLE);

    DWORD __tmp_mode = 0;
    const bool stdin_is_console = (GetFileType(hStdin) == FILE_TYPE_CHAR) && GetConsoleMode(hStdin, &__tmp_mode);

    std::atomic<bool> stop{false};
    std::atomic<bool> input_finished{false};
    std::atomic<bool> output_failed{false};
    std::atomic<bool> input_failed{false};
    std::atomic<bool> discard_output{false};

    // Input thread: stdin -> pty (with escape prefix handling + throttling)
    std::thread t_in([&]() {
        struct FinishedGuard { std::atomic<bool>& done; ~FinishedGuard() { done.store(true); } } finished{input_finished};
        const size_t BUF_SZ = 1u << 14;
        std::vector<unsigned char> buf(BUF_SZ);

        auto start = std::chrono::steady_clock::now();
        bool armed = (args.exit_arm_ms == 0);
        size_t prefix_i = 0;
        auto prefix_at = std::chrono::steady_clock::now();

        bool in_escape = false;
        auto escape_at = std::chrono::steady_clock::now();
        size_t esc_prefix_i = 0;

        auto maybe_arm = [&]() {
            if (armed) return;
            auto now = std::chrono::steady_clock::now();
            double ms = std::chrono::duration<double, std::milli>(now - start).count();
            if (ms >= (double)args.exit_arm_ms) armed = true;
        };

        auto send_one = [&](unsigned char b) -> bool {
            in_bucket.refill();
            while (in_bucket.available(1) == 0 && !stop.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                in_bucket.refill();
            }
            if (stop.load()) return false;
            if (!write_all(hPipeToPty_Write, &b, 1)) {
                if (!stop.load()) {
                    input_failed.store(true);
                    discard_output.store(true);
                    stop.store(true);
                    TerminateProcess(pi.hProcess, 1);
                }
                return false;
            }
            in_bucket.consume(1);
            return true;
        };

        auto send_bytes = [&](const unsigned char* p, size_t n) -> bool {
            for (size_t i = 0; i < n; ++i) {
                if (!send_one(p[i])) return false;
            }
            return true;
        };

        auto expire_escape = [&]() {
            if (in_escape && std::chrono::steady_clock::now() - escape_at >= std::chrono::milliseconds(1500)) {
                if (esc_prefix_i) (void)send_bytes(exit_seq.data(), esc_prefix_i);
                esc_prefix_i = 0;
                in_escape = false;
            }
        };

        auto process_byte = [&](unsigned char c) {
            maybe_arm();
            expire_escape();

            if (exit_enabled && armed) {
                // Native console Ctrl+F7 arrives as a key event, not bytes.
                if (!in_escape && exit_is_ctrl_f7 && stdin_is_console) {
                    (void)send_one(c);
                    return;
                }
                if (!in_escape) {
                    if (!exit_seq.empty() && c == exit_seq[prefix_i]) {
                        prefix_i++;
                        prefix_at = std::chrono::steady_clock::now();
                        if (prefix_i == exit_seq.size()) {
                            in_escape = true;
                            escape_at = std::chrono::steady_clock::now();
                            esc_prefix_i = 0;
                            prefix_i = 0;
                        }
                        return;
                    } else {
                        if (prefix_i > 0 && !exit_seq.empty()) {
                            for (size_t k = 0; k < prefix_i; ++k) {
                                (void)send_one(exit_seq[k]);
                            }
                            prefix_i = 0;
                        }
                    }
                } else {
                    // A repeated prefix takes precedence over local commands,
                    // including custom prefixes that begin with q, h or ?.
                    if (!exit_seq.empty() && c == exit_seq[esc_prefix_i]) {
                        esc_prefix_i++;
                        if (esc_prefix_i == exit_seq.size()) {
                            (void)send_bytes(exit_seq.data(), exit_seq.size());
                            in_escape = false;
                            esc_prefix_i = 0;
                        }
                        return;
                    }
                    if (esc_prefix_i) {
                        // A mismatched repeated prefix is input, not a local
                        // command. Preserve both the partial prefix and byte.
                        (void)send_bytes(exit_seq.data(), esc_prefix_i);
                        in_escape = false;
                        esc_prefix_i = 0;
                        (void)send_one(c);
                        return;
                    }
                    if (c == '?' || c == 'h' || c == 'H') {
                        print_escape_help(args.exit_key, args.exit_arm_ms);
                        in_escape = false;
                        esc_prefix_i = 0;
                        return;
                    }
                    if (c == 'q' || c == 'Q') {
                        discard_output.store(true);
                        stop.store(true);
                        TerminateProcess(pi.hProcess, 1);
                        in_escape = false;
                        esc_prefix_i = 0;
                        return;
                    }
                    in_escape = false;
                    esc_prefix_i = 0;
                    (void)send_one(c);
                    return;
                }
            }

            (void)send_one(c);
        };

        auto flush_partial_prefix = [&]() {
            expire_escape();
            if (prefix_i && std::chrono::steady_clock::now() - prefix_at >= std::chrono::milliseconds(100)) {
                (void)send_bytes(exit_seq.data(), prefix_i);
                prefix_i = 0;
            }
        };

        if (stdin_is_console) {
            INPUT_RECORD rec{};
            DWORD nread = 0;
            wchar_t high_surrogate = 0;

            while (!stop.load()) {
                const DWORD wait = WaitForSingleObject(hStdin, 50);
                if (wait == WAIT_TIMEOUT) { flush_partial_prefix(); continue; }
                if (wait != WAIT_OBJECT_0) break;
                if (stop.load() || !ReadConsoleInputW(hStdin, &rec, 1, &nread)) break;
                if (nread == 0) continue;
                if (rec.EventType == WINDOW_BUFFER_SIZE_EVENT) {
                    CONSOLE_SCREEN_BUFFER_INFO csbi{};
                    if (GetConsoleScreenBufferInfo(hStdout, &csbi)) {
                        COORD dims{(SHORT)(csbi.srWindow.Right - csbi.srWindow.Left + 1),
                                   (SHORT)(csbi.srWindow.Bottom - csbi.srWindow.Top + 1)};
                        pResizePseudoConsole(hPcon, dims);
                    }
                    continue;
                }
                if (rec.EventType != KEY_EVENT) continue;

                const KEY_EVENT_RECORD& ke = rec.Event.KeyEvent;
                if (!ke.bKeyDown) continue;

                maybe_arm();
                expire_escape();
                if (exit_enabled && exit_is_ctrl_f7 && armed) {
                    const bool ctrl = (ke.dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0;
                    if (ctrl && ke.wVirtualKeyCode == VK_F7) {
                        if (in_escape) {
                            (void)send_bytes(exit_seq.data(), exit_seq.size());
                            in_escape = false;
                        } else {
                            in_escape = true;
                            escape_at = std::chrono::steady_clock::now();
                        }
                        esc_prefix_i = 0;
                        prefix_i = 0;
                        continue;
                    }
                }

                wchar_t wc = ke.uChar.UnicodeChar;
                const DWORD modifiers = ke.dwControlKeyState;
                const int modifier = 1 + ((modifiers & SHIFT_PRESSED) ? 1 : 0)
                    + ((modifiers & (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED)) ? 2 : 0)
                    + ((modifiers & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) ? 4 : 0);
                if (ke.wVirtualKeyCode == VK_TAB && modifier == 2) {
                    const char seq[] = "\x1b[Z";
                    for (WORD repeat = 0; repeat < ke.wRepeatCount; ++repeat)
                        for (const char* q = seq; *q; ++q) process_byte((unsigned char)*q);
                    continue;
                }

                if (wc == 0) {
                    const char* seq = nullptr;
                    switch (ke.wVirtualKeyCode) {
                        case VK_UP: seq = "\x1b[A"; break;
                        case VK_DOWN: seq = "\x1b[B"; break;
                        case VK_RIGHT: seq = "\x1b[C"; break;
                        case VK_LEFT: seq = "\x1b[D"; break;
                        case VK_HOME: seq = "\x1b[H"; break;
                        case VK_END: seq = "\x1b[F"; break;
                        case VK_INSERT: seq = "\x1b[2~"; break;
                        case VK_DELETE: seq = "\x1b[3~"; break;
                        case VK_PRIOR: seq = "\x1b[5~"; break;
                        case VK_NEXT: seq = "\x1b[6~"; break;
                        case VK_F1: seq = "\x1bOP"; break;
                        case VK_F2: seq = "\x1bOQ"; break;
                        case VK_F3: seq = "\x1bOR"; break;
                        case VK_F4: seq = "\x1bOS"; break;
                        case VK_F5: seq = "\x1b[15~"; break;
                        case VK_F6: seq = "\x1b[17~"; break;
                        case VK_F7: seq = "\x1b[18~"; break;
                        case VK_F8: seq = "\x1b[19~"; break;
                        case VK_F9: seq = "\x1b[20~"; break;
                        case VK_F10: seq = "\x1b[21~"; break;
                        case VK_F11: seq = "\x1b[23~"; break;
                        case VK_F12: seq = "\x1b[24~"; break;
                    }
                    std::string modified;
                    if (seq && modifier > 1) {
                        // xterm-compatible modifier parameter: 1 + shift + 2*alt + 4*ctrl.
                        std::string base(seq);
                        const char last = base.back();
                        if (last == '~') modified = base.substr(0, base.size()-1) + ";" + std::to_string(modifier) + "~";
                        else modified = "\x1b[1;" + std::to_string(modifier) + last;
                        seq = modified.c_str();
                    }
                    if (seq) for (WORD repeat = 0; repeat < ke.wRepeatCount; ++repeat)
                        for (const char* q = seq; *q; ++q) process_byte((unsigned char)*q);
                    continue;
                }

                wchar_t units[2] = {wc, 0};
                int unit_count = 1;
                if (wc >= 0xD800 && wc <= 0xDBFF) {
                    high_surrogate = wc;
                    continue;
                }
                if (wc >= 0xDC00 && wc <= 0xDFFF && high_surrogate) {
                    units[0] = high_surrogate;
                    units[1] = wc;
                    unit_count = 2;
                }
                high_surrogate = 0;
                char out8[8];
                int n = WideCharToMultiByte(CP_UTF8, 0, units, unit_count, out8, (int)sizeof(out8), nullptr, nullptr);
                if (n > 0) {
                    for (WORD repeat = 0; repeat < ke.wRepeatCount; ++repeat)
                        for (int i = 0; i < n; ++i) process_byte((unsigned char)out8[i]);
                }
            }
            return;
        }

        const bool stdin_is_pipe = GetFileType(hStdin) == FILE_TYPE_PIPE;
        while (!stop.load()) {
            if (stdin_is_pipe) {
                DWORD available = 0;
                if (!PeekNamedPipe(hStdin, nullptr, 0, nullptr, &available, nullptr)) break;
                if (!available) {
                    flush_partial_prefix();
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    continue;
                }
            }
            DWORD got = 0;
            if (!ReadFile(hStdin, buf.data(), (DWORD)BUF_SZ, &got, nullptr)) break;
            if (got == 0) break;
            for (DWORD bi = 0; bi < got; ++bi) process_byte(buf[bi]);
        }
        // EOF on redirected input is conveyed to the pseudo console.
        if (!stop.load()) {
            if (prefix_i) (void)send_bytes(exit_seq.data(), prefix_i);
            if (in_escape && esc_prefix_i) (void)send_bytes(exit_seq.data(), esc_prefix_i);
            close_handle(hPipeToPty_Write);
        }
    });


    // Output thread: pty -> stdout (throttled)
    std::thread t_out([&]() {
        const size_t BUF_SZ = 1u << 14;
        std::vector<unsigned char> buf(BUF_SZ);

        while (true) {
            DWORD got = 0;
            if (!ReadFile(hPipeFromPty_Read, buf.data(), (DWORD)BUF_SZ, &got, nullptr)) break;
            if (got == 0) break;

            if (output_failed.load() || discard_output.load()) continue;
            size_t off = 0;
            while (off < (size_t)got) {
                if (discard_output.load()) break;
                out_bucket.refill();
                size_t can = out_bucket.available((size_t)got - off);
                if (can == 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    continue;
                }
                if (!write_all(hStdout, buf.data() + off, can)) {
                    output_failed.store(true);
                    TerminateProcess(pi.hProcess, 1);
                    break; // continue draining and discard until the pipe closes
                }
                out_bucket.consume(can);
                off += can;
            }
        }
    });

    // Stop input after the child exits, but keep reading output until ConPTY
    // closes its pipe. Closing the console while the reader runs prevents the
    // synchronous ClosePseudoConsole call from deadlocking on a full pipe.
    WaitForSingleObject(pi.hProcess, INFINITE);
    stop.store(true);
    while (!input_finished.load()) {
        CancelSynchronousIo(t_in.native_handle());
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (t_in.joinable()) t_in.join();
    close_handle(hPipeToPty_Write);
    pClosePseudoConsole(hPcon);
    hPcon = nullptr;
    if (t_out.joinable()) t_out.join();
    close_handle(hPipeFromPty_Read);

    DWORD exit_code = 0;
    GetExitCodeProcess(pi.hProcess, &exit_code);

    close_handle(pi.hThread);
    close_handle(pi.hProcess);

/*  // Replaced by OutputCPGuard
    // Restore original console output codepage
    SetConsoleOutputCP(original_cp);
*/
    return (output_failed.load() || input_failed.load()) ? 1 : (int)exit_code;
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
}

#endif // _WIN32
