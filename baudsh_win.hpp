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
#include <string>
#include <vector>

#ifndef BAUDSH_VERSION
    #define BAUDSH_VERSION "dev"
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
        "baud=2400\n"
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

static inline void print_autocomp(const std::string& shell, FILE* out = stdout) {
    const std::string sh = tolower_copy(shell);

    // Keep this deliberately minimal and robust (no fancy generator).
    // You can expand later, but this is enough to be useful and safe.
    if (sh == "bash" || sh == "zsh" || sh == "dash") {
        std::fprintf(out,
            "# minimal completion for baudsh\n"
            "_baudsh_complete() {\n"
            "  local cur=\"${COMP_WORDS[COMP_CWORD]}\"\n"
            "  local opts=\"--baud --data-bits --parity --stop-bits --burst-ms --exit-key --exit-arm-ms --cmd --print-effective --print-default --autocomp --version -h --help\"\n"
            "  COMPREPLY=( $(compgen -W \"$opts\" -- \"$cur\") )\n"
            "}\n"
            "complete -F _baudsh_complete baudsh\n"
        );
        return;
    }

    if (sh == "fish") {
        std::fprintf(out,
            "# minimal completion for baudsh (fish)\n"
            "complete -c baudsh -l baud -r\n"
            "complete -c baudsh -l data-bits -r\n"
            "complete -c baudsh -l parity -r\n"
            "complete -c baudsh -l stop-bits -r\n"
            "complete -c baudsh -l burst-ms -r\n"
            "complete -c baudsh -l exit-key -r\n"
            "complete -c baudsh -l exit-arm-ms -r\n"
            "complete -c baudsh -l cmd -r\n"
            "complete -c baudsh -l print-effective\n"
            "complete -c baudsh -l print-default\n"
            "complete -c baudsh -l autocomp -r\n"
            "complete -c baudsh -l version\n"
            "complete -c baudsh -s h -l help\n"
        );
        return;
    }

    std::fprintf(out, "Unknown shell for --autocomp: %s\n", shell.c_str());
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
        a.baud = std::atoi(val.c_str());
        return;
    }
    if (key == "data-bits" || key == "databits") {
        a.data_bits = std::atoi(val.c_str());
        return;
    }
    if (key == "parity") {
        if (!val.empty()) a.parity = (char)std::tolower((unsigned char)val[0]);
        return;
    }
    if (key == "stop-bits" || key == "stopbits") {
        a.stop_bits = std::atof(val.c_str());
        return;
    }
    if (key == "burst-ms" || key == "burstms") {
        a.burst_ms = std::atoi(val.c_str());
        return;
    }
    if (key == "exit-key" || key == "exitkey") {
        a.exit_key = val;
        return;
    }
    if (key == "exit-arm-ms" || key == "exitarmms") {
        a.exit_arm_ms = std::atoi(val.c_str());
        return;
    }

    // Unknown keys ignored intentionally.
}

template <typename ArgsT>
static inline void load_kv_file_into_args(FILE* f, ArgsT& a) {
    char buf[4096];

    while (std::fgets(buf, sizeof(buf), f)) {
        std::string line(buf);

        // trim newline
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();

        line = ltrim_copy(line);
        if (line.empty()) continue;
        if (line[0] == '#' || line[0] == ';') continue;

        // key=value
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);

        apply_kv_to_args(a, key, val);
    }
}

template <typename ArgsT>
static inline void load_rc_files(ArgsT& a) {
    // Lowest priority first, higher priority last.
    std::vector<std::string> paths;

#ifdef _WIN32
    if (const char* appdata = std::getenv("APPDATA"); appdata && *appdata) {
        paths.emplace_back(std::string(appdata) + "/baudsh/baudsh.conf");
    }
    paths.emplace_back("~/.baudsh.rc");
#else
    // If this header is ever included on POSIX, keep it sensible.
    paths.emplace_back("~/.config/baudsh/baudsh.conf");
    paths.emplace_back("~/.baudsh.rc");
#endif

    for (const std::string& p0 : paths) {
        const std::string p = expand_home(p0);
        FILE* f = std::fopen(p.c_str(), "rb");
        if (!f) continue;
        load_kv_file_into_args(f, a);
        std::fclose(f);
    }
}

// ------------------------------
// early flag handling
//   - call at the very beginning of baudsh_windows_main()
//   - exits the process if a handled early flag is found
// ------------------------------

static inline void handle_early_flags(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];

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
            print_autocomp(argv[i + 1], stdout);
            std::exit(0);
        }
    }
}

} // namespace baudsh