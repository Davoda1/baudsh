#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
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
// HPCON is an opaque handle type used by ConPTY APIs.
#ifndef HPCON
typedef void* HPCON;
#endif

// PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE is the attribute used with UpdateProcThreadAttribute.
// Many references (including Microsoft examples) use value 0x00020016.
#ifndef PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE
#define PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE 0x00020016
#endif

#ifndef PSEUDOCONSOLE_INHERIT_CURSOR
#define PSEUDOCONSOLE_INHERIT_CURSOR 0x1
#endif

#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0200
#endif


// -------------------- ConPTY typedefs --------------------

typedef HRESULT (WINAPI *CreatePseudoConsole_t)(COORD, HANDLE, HANDLE, DWORD, HPCON*);
typedef void    (WINAPI *ClosePseudoConsole_t)(HPCON);
typedef HRESULT (WINAPI *ResizePseudoConsole_t)(HPCON, COORD);

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
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring out((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), n);
    return out;
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
            m &= ~(ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT);
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


// Windows CreateProcess quoting rules (simple safe quoting)
static std::wstring quote_arg(const std::wstring& a) {
    std::wstring out;
    out.push_back(L'"');
    for (wchar_t ch : a) {
        if (ch == L'"') out.push_back(L'\\');
        out.push_back(ch);
    }
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
        "  ~/.config/baudsh/baudsh.conf\n"
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
            out.baud = std::atoi(argv[++i]);
        } else if (a == "--data-bits") {
            if (i + 1 >= argc) return false;
            out.data_bits = std::atoi(argv[++i]);
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
            out.stop_bits = std::atof(argv[++i]);
            if (!(out.stop_bits == 1.0 || out.stop_bits == 1.5 || out.stop_bits == 2.0)) return false;
        } else if (a == "--burst-ms") {
            if (i + 1 >= argc) return false;
            out.burst_ms = std::atoi(argv[++i]);
            if (out.burst_ms < 1) out.burst_ms = 1;
        } else if (a == "--exit-key") {
            if (i + 1 >= argc) return false;
            out.exit_key = argv[++i];
        } else if (a == "--exit-arm-ms") {
            if (i + 1 >= argc) return false;
            out.exit_arm_ms = std::atoi(argv[++i]);
            if (out.exit_arm_ms < 0) out.exit_arm_ms = 0;
        } else if (a == "--cmd") {
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
    return out.baud > 0 || out.print_effective;
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
    // early flags: --version / --print-default / --autocomp
    baudsh::handle_early_flags(argc, argv);

    OutputCPGuard cp_guard;
/*
    // Save original console output codepage
    UINT original_cp = GetConsoleOutputCP();

    // Force UTF-8 so NBSP (C2 A0) renders correctly
    SetConsoleOutputCP(65001);
*/
    ConsoleModeGuard cmg;
    cmg.apply_raw_vt();
    if (!load_conpty_symbols()) return 1;

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
        } else if (ek.rfind("raw:", 0) == 0) {
            exit_seq = parse_raw_seq(args.exit_key.substr(4));
            if (exit_seq.empty()) exit_enabled = false;
        } else {
            exit_seq.assign(args.exit_key.begin(), args.exit_key.end());
            if (exit_seq.empty()) exit_enabled = false;
        }
    }

    // Build child argv:
    // - no --cmd: interactive cmd.exe
    // - --cmd single string: cmd.exe /S /C "<string>"
    // - --cmd multi args: direct CreateProcess of argv[0] + rest
    std::vector<std::string> child_argv;
    if (!args.has_cmd || args.cmd.empty()) {
        child_argv = {"cmd.exe"};
    } else if (args.cmd.size() == 1) {
        child_argv = {"cmd.exe", "/S", "/C", args.cmd[0]};
    } else {
        child_argv = args.cmd;
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

    HPCON hPcon = nullptr;
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
    std::wstring cmdline = build_cmdline_from_utf8_argv(child_argv);

    BOOL ok = CreateProcessW(
        nullptr,
        cmdline.data(),
        nullptr,
        nullptr,
        TRUE,
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

    // Input thread: stdin -> pty (with escape prefix handling + throttling)
    std::thread t_in([&]() {
        const size_t BUF_SZ = 1u << 14;
        std::vector<unsigned char> buf(BUF_SZ);

        auto start = std::chrono::steady_clock::now();
        bool armed = (args.exit_arm_ms == 0);
        size_t prefix_i = 0;

        bool in_escape = false;
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
            if (!write_all(hPipeToPty_Write, &b, 1)) return false;
            in_bucket.consume(1);
            return true;
        };

        auto send_bytes = [&](const unsigned char* p, size_t n) -> bool {
            for (size_t i = 0; i < n; ++i) {
                if (!send_one(p[i])) return false;
            }
            return true;
        };

        auto process_byte = [&](unsigned char c) {
            maybe_arm();

            if (exit_enabled && armed) {
                if (!in_escape) {
                    if (!exit_seq.empty() && c == exit_seq[prefix_i]) {
                        prefix_i++;
                        if (prefix_i == exit_seq.size()) {
                            in_escape = true;
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
                    if (c == '?' || c == 'h') {
                        print_escape_help(args.exit_key, args.exit_arm_ms);
                        in_escape = false;
                        esc_prefix_i = 0;
                        return;
                    }
                    if (c == 'q') {
                        TerminateProcess(pi.hProcess, 1);
                        stop.store(true);
                        in_escape = false;
                        esc_prefix_i = 0;
                        return;
                    }
                    if (!exit_seq.empty() && c == exit_seq[esc_prefix_i]) {
                        esc_prefix_i++;
                        if (esc_prefix_i == exit_seq.size()) {
                            (void)send_bytes(exit_seq.data(), exit_seq.size());
                            in_escape = false;
                            esc_prefix_i = 0;
                        }
                        return;
                    }
                    in_escape = false;
                    esc_prefix_i = 0;
                    return;
                }
            }

            (void)send_one(c);
        };

        if (stdin_is_console) {
            INPUT_RECORD rec{};
            DWORD nread = 0;

            while (!stop.load()) {
                if (!ReadConsoleInputW(hStdin, &rec, 1, &nread)) break;
                if (nread == 0) continue;
                if (rec.EventType != KEY_EVENT) continue;

                const KEY_EVENT_RECORD& ke = rec.Event.KeyEvent;
                if (!ke.bKeyDown) continue;

                if (exit_enabled && exit_is_ctrl_f7 && armed) {
                    const bool ctrl = (ke.dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0;
                    if (ctrl && ke.wVirtualKeyCode == VK_F7) {
                        in_escape = !in_escape;
                        esc_prefix_i = 0;
                        prefix_i = 0;
                        continue;
                /*if (exit_enabled && armed) {
                    std::string ek = args.exit_key;
                    for (auto& ch : ek) ch = (char)std::tolower((unsigned char)ch);
                    if (ek == "ctrl+f7") {
                        const bool ctrl = (ke.dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0;
                        if (ctrl && ke.wVirtualKeyCode == VK_F7) {
                            in_escape = !in_escape;
                            esc_prefix_i = 0;
                            prefix_i = 0;
                            continue;
                        }*/
                    }
                }

                wchar_t wc = ke.uChar.UnicodeChar;

                if (wc == 0) {
                    switch (ke.wVirtualKeyCode) {
                        case VK_RETURN:
                            process_byte('\r');
                            process_byte('\n');
                            break;
                        case VK_TAB:
                            process_byte('	');
                            break;
                        case VK_BACK:
                            process_byte('');
                            break;
                        default:
                            break;
                    }
                    continue;
                }

                char out8[8];
                int n = WideCharToMultiByte(CP_UTF8, 0, &wc, 1, out8, (int)sizeof(out8), nullptr, nullptr);
                if (n > 0) {
                    for (int i = 0; i < n; ++i) process_byte((unsigned char)out8[i]);
                }
            }
            return;
        }

        while (!stop.load()) {
            DWORD got = 0;
            if (!ReadFile(hStdin, buf.data(), (DWORD)BUF_SZ, &got, nullptr)) break;
            if (got == 0) break;
            for (DWORD bi = 0; bi < got; ++bi) process_byte(buf[bi]);
        }
    });


    // Output thread: pty -> stdout (throttled)
    std::thread t_out([&]() {
        const size_t BUF_SZ = 1u << 14;
        std::vector<unsigned char> buf(BUF_SZ);

        while (!stop.load()) {
            DWORD got = 0;
            if (!ReadFile(hPipeFromPty_Read, buf.data(), (DWORD)BUF_SZ, &got, nullptr)) break;
            if (got == 0) break;

            size_t off = 0;
            while (off < (size_t)got && !stop.load()) {
                out_bucket.refill();
                size_t can = out_bucket.available((size_t)got - off);
                if (can == 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    continue;
                }
                if (!write_all(hStdout, buf.data() + off, can)) { stop.store(true); break; }
                out_bucket.consume(can);
                off += can;
            }
        }
    });

    // Wait for child
    WaitForSingleObject(pi.hProcess, INFINITE);
    stop.store(true);

    // Close pipes to unblock threads
    close_handle(hPipeToPty_Write);
    close_handle(hPipeFromPty_Read);

    if (t_in.joinable()) t_in.join();
    if (t_out.joinable()) t_out.join();

    DWORD exit_code = 0;
    GetExitCodeProcess(pi.hProcess, &exit_code);

    close_handle(pi.hThread);
    close_handle(pi.hProcess);

    pClosePseudoConsole(hPcon);
/*  // Replaced by OutputCPGuard
    // Restore original console output codepage
    SetConsoleOutputCP(original_cp);
*/
    return (int)exit_code;
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
}

#endif // _WIN32