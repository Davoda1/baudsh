/*
    baudsh_win.hpp — Windows-facing shared helpers + Windows backend entry point.

    This header is meant to replace baudsh_features.hpp on Windows builds,
    because baudsh_main.cpp includes ONLY baudsh_win.hpp when _WIN32 is defined.

    Goals:
      - No <windows.h> include required.
      - Provide the same "early flag" helpers as the POSIX build:
            --version
            --print-default
            --autocomp <shell>
      - Provide rc file loading + key=value parsing for the Windows backend.
      - Declare baudsh_windows_main() implemented in baudsh_win.cpp.

    Config locations on Windows:
      1) %APPDATA%/baudsh/baudsh.conf   (if APPDATA is set)
      2) ~/.baudsh.rc                   (HOME -> USERPROFILE -> HOMEDRIVE+HOMEPATH)

    RC parsing:
      - Lines: key=value
      - Whitespace trimmed.
      - Comments: lines starting with '#' or ';' (after leading whitespace).
      - Unknown keys ignored (so forward compatible).
*/

#pragma once

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <climits>
#include <cmath>
#include <string>
#include <vector>

#ifndef BAUDSH_VERSION
    #define BAUDSH_VERSION "1.0.0"
#endif

namespace baudsh {

// ------------------------------
// small string helpers
// ------------------------------

static inline std::string ltrim_copy(std::string s) {
    size_t i = 0;
    while (i < s.size() && std::isspace((unsigned char)s[i])) ++i;
    s.erase(0, i);
    return s;
}

static inline std::string rtrim_copy(std::string s) {
    while (!s.empty() && std::isspace((unsigned char)s.back())) s.pop_back();
    return s;
}

static inline std::string trim_copy(std::string s) {
    return rtrim_copy(ltrim_copy(std::move(s)));
}

static inline bool starts_with(const std::string& s, const char* pfx) {
    const size_t n = std::strlen(pfx);
    return s.size() >= n && s.compare(0, n, pfx) == 0;
}

static inline std::string tolower_copy(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// Strict parsing shared by the command-line and configuration readers.
static inline bool parse_int(const std::string& s, int& value) {
    if (s.empty()) return false;
    char* end = nullptr;
    errno = 0;
    long n = std::strtol(s.c_str(), &end, 10);
    if (errno || end == s.c_str() || *end != '\0' || n < INT_MIN || n > INT_MAX) return false;
    value = static_cast<int>(n);
    return true;
}
static inline bool parse_double(const std::string& s, double& value) {
    if (s.empty()) return false;
    char* end = nullptr;
    errno = 0;
    double n = std::strtod(s.c_str(), &end);
    if (errno || end == s.c_str() || *end != '\0' || !std::isfinite(n)) return false;
    value = n;
    return true;
}

// ------------------------------
// "home" expansion for Windows without <windows.h>
// ------------------------------

static inline std::string get_home_dir() {
    // Prefer HOME if present (works on MSYS2/Cygwin and sometimes native setups).
    if (const char* home = std::getenv("HOME"); home && *home) {
        return std::string(home);
    }

#ifdef _WIN32
    if (const char* up = std::getenv("USERPROFILE"); up && *up) {
        return std::string(up);
    }
    const char* drive = std::getenv("HOMEDRIVE");
    const char* path  = std::getenv("HOMEPATH");
    if (drive && *drive && path && *path) {
        return std::string(drive) + std::string(path);
    }
#endif

    return std::string();
}

static inline std::string expand_home(const std::string& path) {
    // Support "~/" and "~\"
    if (path.size() >= 2 && path[0] == '~' && (path[1] == '/' || path[1] == '\\')) {
        const std::string home = get_home_dir();
        if (home.empty()) return path;
        return home + path.substr(1);
    }
    return path;
}

// ------------------------------
// default config template (for --print-default)
// ------------------------------

static inline void print_default_config(FILE* out = stdout) {
    std::fprintf(out,
        "# baudsh config (Windows)\n"
        "# key=value format; unknown keys are ignored\n"
        "#\n"
        "# baud=<rate>            (required unless passed on CLI)\n"
        "# data-bits=5|6|7|8       (default 8)\n"
        "# parity=n|e|o            (default n)\n"
        "# stop-bits=1|1.5|2       (default 1)\n"
        "# burst-ms=<ms>           (default 200)\n"
        "# exit-key=ctrl+f7|raw:<seq>|none   (default ctrl+f7)\n"
        "# exit-arm-ms=<ms>        (default 250)\n"
        "\n"
        "# baud=2400\n"
        "data-bits=8\n"
        "parity=n\n"
        "stop-bits=1\n"
        "burst-ms=200\n"
        "exit-key=ctrl+f7\n"
        "exit-arm-ms=250\n"
    );
}

// ------------------------------
// autocomplete (for --autocomp)
// ------------------------------

static inline bool print_autocomp(const std::string& shell_raw, FILE* out = stdout) {
    const std::string shell = tolower_copy(shell_raw);

    if (shell == "fish") {
        std::fprintf(out,
            "# fish completion for baudsh\n"
            "complete -c baudsh -l baud -r\n"
            "complete -c baudsh -l data-bits -r -a '5 6 7 8'\n"
            "complete -c baudsh -l parity -r -a 'n e o'\n"
            "complete -c baudsh -l stop-bits -r -a '1 1.5 2'\n"
            "complete -c baudsh -l burst-ms -r\n"
            "complete -c baudsh -l exit-key -r\n"
            "complete -c baudsh -l exit-arm-ms -r\n"
            "complete -c baudsh -l cmd -r\n"
            "complete -c baudsh -l autocomp -r -a 'dash bash zsh fish'\n"
            "complete -c baudsh -l print-default\n"
            "complete -c baudsh -l print-effective\n"
            "complete -c baudsh -l version\n"
            "complete -c baudsh -s h -l help\n"
        );
        return true;
    }

    if (shell == "dash" || shell == "sh") {
        std::fprintf(out, "# This shell does not support programmable completion.\n");
        return true;
    }
    if (shell == "zsh") {
        std::fprintf(out,
            "# Source after compinit to enable baudsh completion.\n"
            "_baudsh_complete() {\n"
            "  _arguments '--baud[Baud rate]:rate:' '--data-bits[Data bits]:bits:(5 6 7 8)' \\\n"
            "    '--parity[Parity]:parity:(n e o)' '--stop-bits[Stop bits]:bits:(1 1.5 2)' \\\n"
            "    '--burst-ms[Burst window]:milliseconds:' '--exit-key[Escape prefix]:key:' \\\n"
            "    '--exit-arm-ms[Escape activation delay]:milliseconds:' '--cmd[Command]:command:_command_names' \\\n"
            "    '--autocomp[Completion shell]:shell:(dash bash zsh fish)' '--print-default[Print defaults]' \\\n"
            "    '--print-effective[Print effective settings]' '--version[Print version]' '--help[Show help]' '-h[Show help]'\n"
            "}\n"
            "if (( $+functions[compdef] )); then compdef _baudsh_complete baudsh; fi\n"
        );
        return true;
    }

    if (shell == "bash") {
        std::fprintf(out,
            "# completion for baudsh (bash).\n"
            "_baudsh_opts='--baud --data-bits --parity --stop-bits --burst-ms --exit-key --exit-arm-ms --cmd --autocomp --print-default --print-effective --version --help -h'\n"
            "_baudsh_complete() {\n"
            "  local cur prev\n"
            "  cur=\"${COMP_WORDS[COMP_CWORD]}\"\n"
            "  prev=\"${COMP_WORDS[COMP_CWORD-1]}\"\n"
            "  case \"$prev\" in\n"
            "    --data-bits) COMPREPLY=( $(compgen -W '5 6 7 8' -- \"$cur\") ) ;;\n"
            "    --parity)    COMPREPLY=( $(compgen -W 'n e o' -- \"$cur\") ) ;;\n"
            "    --stop-bits) COMPREPLY=( $(compgen -W '1 1.5 2' -- \"$cur\") ) ;;\n"
            "    --autocomp)  COMPREPLY=( $(compgen -W 'dash bash zsh fish' -- \"$cur\") ) ;;\n"
            "    *)           COMPREPLY=( $(compgen -W \"$_baudsh_opts\" -- \"$cur\") ) ;;\n"
            "  esac\n"
            "}\n"
            "if command -v complete >/dev/null 2>&1; then\n"
            "  complete -F _baudsh_complete baudsh\n"
            "fi\n"
        );
        return true;
    }

    std::fprintf(stderr, "baudsh: unsupported shell for --autocomp: %s\n", shell_raw.c_str());
    return false;
}


// ------------------------------
// version output (for --version)
// ------------------------------

static inline void print_version(FILE* out = stdout) {
    std::fprintf(out, "baudsh %s\n", BAUDSH_VERSION);
}

// ------------------------------
// rc parsing: write into an Args-like type
// ------------------------------

template <typename ArgsT>
static inline void apply_kv_to_args(ArgsT& a, const std::string& key0, const std::string& val0) {
    const std::string key = tolower_copy(trim_copy(key0));
    const std::string val = trim_copy(val0);

    if (key == "baud") {
        if (!parse_int(val, a.baud) || a.baud <= 0) a.baud = 0;
        return;
    }
    if (key == "data-bits" || key == "databits") {
        if (!parse_int(val, a.data_bits)) a.data_bits = -1;
        return;
    }
    if (key == "parity") {
        a.parity = val.size() == 1 ? (char)std::tolower((unsigned char)val[0]) : '?';
        return;
    }
    if (key == "stop-bits" || key == "stopbits") {
        if (!parse_double(val, a.stop_bits)) a.stop_bits = -1;
        return;
    }
    if (key == "burst-ms" || key == "burstms") {
        if (!parse_int(val, a.burst_ms)) a.burst_ms = -1;
        return;
    }
    if (key == "exit-key" || key == "exitkey") {
        a.exit_key = val;
        return;
    }
    if (key == "exit-arm-ms" || key == "exitarmms") {
        if (!parse_int(val, a.exit_arm_ms)) a.exit_arm_ms = -1;
        return;
    }

    // Unknown keys ignored intentionally.
}

template <typename ArgsT>
static inline void load_kv_file_into_args(FILE* f, ArgsT& a) {
    char buf[4096];
    bool first_line = true;
    while (std::fgets(buf, sizeof(buf), f)) {
        std::string line(buf);
        // Assemble complete lines rather than interpreting 4096-byte fragments
        // as separate settings. Accept the UTF-8 BOM written by common editors.
        while (!line.empty() && line.back() != '\n' && std::fgets(buf, sizeof(buf), f)) line += buf;
        if (first_line && line.compare(0, 3, "\xef\xbb\xbf") == 0) line.erase(0, 3);
        first_line = false;
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
        line = ltrim_copy(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        for (size_t i = 0; i < line.size(); ++i) {
            if ((line[i] == '#' || line[i] == ';') && (i == 0 || std::isspace((unsigned char)line[i-1]))) {
                line.erase(i);
                break;
            }
        }
        const size_t split = line.find_first_of("= \t");
        if (split == std::string::npos) continue;
        const std::string key = line.substr(0, split);
        size_t value_start = line.find_first_not_of(" \t", split);
        if (value_start != std::string::npos && line[value_start] == '=') ++value_start;
        const std::string val = value_start == std::string::npos ? "" : line.substr(value_start);
        apply_kv_to_args(a, key, val);
    }
}

template <typename ArgsT>
static inline void load_rc_files(ArgsT& a) {
    // Lowest priority first, higher priority last. Use wide paths on Windows:
    // environment paths may contain characters outside the active ANSI codepage.
#ifdef _WIN32
    std::vector<std::wstring> paths;
    if (const wchar_t* appdata = _wgetenv(L"APPDATA"); appdata && *appdata)
        paths.emplace_back(std::wstring(appdata) + L"/baudsh/baudsh.conf");
    std::wstring home;
    if (const wchar_t* value = _wgetenv(L"HOME"); value && *value) home = value;
    else if (const wchar_t* value = _wgetenv(L"USERPROFILE"); value && *value) home = value;
    else {
        const wchar_t* drive = _wgetenv(L"HOMEDRIVE");
        const wchar_t* path = _wgetenv(L"HOMEPATH");
        if (drive && *drive && path && *path) home = std::wstring(drive) + path;
    }
    if (!home.empty()) paths.emplace_back(home + L"/.baudsh.rc");
    for (const auto& path : paths) {
        FILE* f = _wfopen(path.c_str(), L"rb");
        if (!f) continue;
        load_kv_file_into_args(f, a);
        std::fclose(f);
    }
#else
    for (const char* path : {"~/.config/baudsh/baudsh.conf", "~/.baudsh.rc"}) {
        FILE* f = std::fopen(expand_home(path).c_str(), "rb");
        if (!f) continue;
        load_kv_file_into_args(f, a);
        std::fclose(f);
    }
#endif
}

// ------------------------------
// early flag handling
//   - call at the very beginning of baudsh_windows_main()
//   - exits the process if a handled early flag is found
// ------------------------------

static inline void handle_early_flags(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];

        if (a == "--cmd") break;

        if (a == "--version") {
            print_version(stdout);
            std::exit(0);
        }

        if (a == "--print-default") {
            print_default_config(stdout);
            std::exit(0);
        }

        if (a == "--autocomp") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "baudsh: --autocomp requires an argument (dash|bash|zsh|fish)\n");
                std::exit(64);
            }
            std::exit(print_autocomp(argv[i + 1], stdout) ? 0 : 64);
        }
        if (a == "--baud" || a == "--data-bits" || a == "--parity" ||
            a == "--stop-bits" || a == "--burst-ms" || a == "--exit-key" ||
            a == "--exit-arm-ms") {
            if (i + 1 < argc) ++i;
        }
    }
}

} // namespace baudsh