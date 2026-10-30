/*
    baudsh_posix_features.hpp — small add-ons for baudsh

    Adds:
      1) rc/config file defaults
      2) shell autocomplete emitter
      3) --version output

    Config locations (in this order; later overrides earlier):
      - ~/.config/baudsh/baudsh.conf
      - ~/.baudsh.rc        (PRIMARY if both exist)

    Config format (simple, intentionally strict):
      - Blank lines and lines starting with '#' or ';' are ignored.
      - Key/value are parsed as: key=value OR key value
      - Keys are case-insensitive.
      - Whitespace around keys/values is trimmed.

    Supported keys map 1:1 to CLI flags:
      baud, data-bits, parity, stop-bits, burst-ms, exit-key, exit-arm-ms

    Notes:
      - This does NOT try to parse a "cmd" from the rc file because the CLI
        supports arbitrary argv vectors; you should keep that on the command line.

    License: same as the rest of the project.
*/

#pragma once

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <climits>
#include <string>
#include <vector>

namespace baudsh {

static constexpr const char* VERSION = "1.0.0";
static constexpr const char* COPYRIGHT = "Copyright (c) 2026 davoda1";

// -------------------- tiny string helpers --------------------

static inline std::string ltrim(std::string s) {
    size_t i = 0;
    while (i < s.size() && std::isspace((unsigned char)s[i])) i++;
    s.erase(0, i);
    return s;
}

static inline std::string rtrim(std::string s) {
    while (!s.empty() && std::isspace((unsigned char)s.back())) s.pop_back();
    return s;
}

static inline std::string trim(std::string s) { return rtrim(ltrim(std::move(s))); }

// Remove inline comments. We treat '#' or ';' as a comment starter when it appears
// at the beginning of the line OR is preceded by whitespace. This supports:
//   baud=2400 # comment
// while still allowing values like:
//   exit-key=raw:\\e[18;5~
static inline std::string strip_inline_comment(std::string s) {
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if ((c == '#' || c == ';') && (i == 0 || std::isspace((unsigned char)s[i - 1]))) {
            s.erase(i);
            break;
        }
    }
    return s;
}

static inline std::string tolower_ascii(std::string s) {
    for (auto& ch : s) ch = (char)std::tolower((unsigned char)ch);
    return s;
}

static inline bool split_kv(const std::string& line, std::string& k, std::string& v) {
    // The first separator determines the key; '=' inside a whitespace-style
    // value is data, as in "exit-key raw:=".
    const size_t sep = line.find_first_of("= \t\r\n");
    if (sep == std::string::npos || sep == 0) return false;
    k = trim(line.substr(0, sep));
    v = ltrim(line.substr(sep));
    if (!v.empty() && v[0] == '=') v.erase(0, 1);
    v = trim(std::move(v));
    return !k.empty();
}

static inline std::string expand_home(const std::string& path) {
    if (path.rfind("~/", 0) != 0) return path;
    const char* home = std::getenv("HOME");
    if (!home || !*home) return path; // fallback: leave as-is
    return std::string(home) + path.substr(1);
}

// -------------------- config loader --------------------

template <typename ArgsT>
static inline void apply_cfg_kv(ArgsT& a, const std::string& key_raw, const std::string& val_raw) {
    const std::string key = tolower_ascii(key_raw);
    const std::string val = tolower_ascii(val_raw);

    auto to_int = [&](const std::string& s, int& out) -> bool {
        char* end = nullptr;
        errno = 0;
        long n = std::strtol(s.c_str(), &end, 10);
        if (errno != 0 || end == s.c_str() || *end != '\0' || n < INT_MIN || n > INT_MAX) return false;
        out = (int)n;
        return true;
    };

    auto to_double = [&](const std::string& s, double& out) -> bool {
        char* end = nullptr;
        errno = 0;
        double n = std::strtod(s.c_str(), &end);
        if (errno != 0 || end == s.c_str() || *end != '\0') return false;
        out = n;
        return true;
    };

    if (key == "baud") {
        int n = -1;
        a.baud = (to_int(val, n) && n > 0) ? n : 0;
    } else if (key == "data-bits" || key == "databits" || key == "data_bits") {
        int n = 0;
        a.data_bits = (to_int(val, n) && n >= 5 && n <= 8) ? n : 0;
    } else if (key == "parity") {
        a.parity = (val.size() == 1 && (val[0] == 'n' || val[0] == 'e' || val[0] == 'o')) ? val[0] : 0;
    } else if (key == "stop-bits" || key == "stopbits" || key == "stop_bits") {
        double n = 0.0;
        a.stop_bits = (to_double(val, n) && (n == 1.0 || n == 1.5 || n == 2.0)) ? n : 0.0;
    } else if (key == "burst-ms" || key == "burstms" || key == "burst_ms") {
        int n = 0;
        a.burst_ms = (to_int(val, n) && n >= 1) ? n : 0;
    } else if (key == "exit-key" || key == "exitkey" || key == "exit_key") {
        if (!val_raw.empty()) a.exit_key = trim(val_raw);
    } else if (key == "exit-arm-ms" || key == "exitarmms" || key == "exit_arm_ms") {
        int n = 0;
        a.exit_arm_ms = (to_int(val, n) && n >= 0) ? n : -1;
    }
}

template <typename ArgsT>
static inline void load_rc_files(ArgsT& a) {
    const char* paths[] = {
        // Lower priority first; higher priority last.
        "~/.config/baudsh/baudsh.conf",
        "~/.baudsh.rc",
    };

    for (const char* p0 : paths) {
        const std::string p = expand_home(p0);
        FILE* f = std::fopen(p.c_str(), "rb");
        if (!f) continue; // silent if missing

        char buf[4096];
        bool first_line = true;
        while (std::fgets(buf, (int)sizeof(buf), f)) {
            std::string line(buf);
            // Read the entire logical line so a long value or comment cannot
            // turn its next chunk into an unintended setting.
            while (!line.empty() && line.back() != '\n' && std::fgets(buf, (int)sizeof(buf), f)) line += buf;
            if (first_line && line.compare(0, 3, "\xEF\xBB\xBF") == 0) line.erase(0, 3);
            first_line = false;
            // strip CR/LF
            while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();

            // allow inline comments and flexible whitespace
            line = strip_inline_comment(std::move(line));
            line = trim(std::move(line));
            if (line.empty()) continue;
            if (line[0] == '#' || line[0] == ';') continue;

            std::string k, v;
            if (!split_kv(line, k, v)) continue;
            if (k.empty()) continue;
            apply_cfg_kv(a, k, v);
        }

        std::fclose(f);
    }
}

// -------------------- --version --------------------

static inline void print_version() {
    dprintf(1, "baudsh %s\n%s\n", VERSION, COPYRIGHT);
}

// -------------------- --print-default --------------------

// Prints a minimal config template showing *built-in* defaults.
// Intended to be copy-pasted into ~/.baudsh.rc or ~/.config/baudsh/baudsh.conf.
// Baud is intentionally commented out so the template preserves the built-in
// unspecified state until the user selects a rate or passes it on the CLI.
static inline void print_default_config() {
    dprintf(1,
        "# baud=2400  # Uncomment and choose a rate, or pass --baud on the command line.\n"
        "data-bits=8\n"
        "parity=n\n"
        "stop-bits=1.0\n"
        "burst-ms=200\n"
        "exit-key=ctrl+f7\n"
        "exit-arm-ms=250\n"
    );
}

// -------------------- --autocomp --------------------

static inline bool print_autocomp(const std::string& shell_raw, const char* progname) {
    const std::string shell = tolower_ascii(shell_raw);
    (void)progname;

    if (shell == "fish") {
        dprintf(1,
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
        dprintf(1, "# This shell does not support programmable completion.\n");
        return true;
    }
    if (shell == "zsh") {
        dprintf(1,
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
        dprintf(1,
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

    dprintf(2, "baudsh: unsupported shell for --autocomp: %s\n", shell_raw.c_str());
    return false;
}

// -------------------- early special-flag handling --------------------

// Returns true if the program should exit after handling a special flag.
static inline bool handle_early_flags(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--cmd") break;
        if (a == "--baud" || a == "--data-bits" || a == "--parity" ||
            a == "--stop-bits" || a == "--burst-ms" || a == "--exit-key" ||
            a == "--exit-arm-ms") {
            if (i + 1 < argc) ++i;
            continue;
        }
        if (a == "--print-default") {
            print_default_config();
            std::exit(0);
        }
        if (a == "--version") {
            print_version();
            std::exit(0);
        }
        if (a == "--autocomp") {
            if (i + 1 >= argc) {
                dprintf(2, "baudsh: --autocomp requires a shell name (dash|bash|zsh|fish)\n");
                std::exit(64);
            }
            std::exit(print_autocomp(argv[i + 1], argv[0]) ? 0 : 64);
        }
    }
    return false;
}

} // namespace baudsh
