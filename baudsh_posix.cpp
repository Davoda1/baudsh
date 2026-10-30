/*
    baudsh — throttle bytes between a terminal and a child PTY.
    Build: g++ -std=c++17 -O2 baudsh_main.cpp baudsh_posix.cpp -o baudsh -lutil
*/

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <climits>
#include <ctime>
#include <string>
#include <vector>

#include "baudsh_posix_features.hpp"

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#ifdef __linux__
#include <pty.h>
#else
#include <util.h>
#endif

// -------------------- signals --------------------

static volatile sig_atomic_t g_winch_pending = 1;
static volatile sig_atomic_t g_term_pending  = 0;
static volatile sig_atomic_t g_suspend_pending = 0;

static void on_winch(int) { g_winch_pending = 1; }
static void on_term(int sig)  { g_term_pending = sig; }
static void on_suspend(int) { g_suspend_pending = 1; }

// -------------------- time --------------------

static double monotonic_seconds() {
    struct timespec ts;
#if defined(CLOCK_MONOTONIC)
    clock_gettime(CLOCK_MONOTONIC, &ts);
#else
    clock_gettime(CLOCK_REALTIME, &ts);
#endif
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

// -------------------- fd helpers --------------------

static void set_nonblocking(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0) return;
    (void)fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

// Nonblocking read result:
//  >0  bytes read
//   0  EOF
//  -2  would block
//  -1  fatal
static ssize_t read_nb(int fd, void* buf, size_t n) {
    for (;;) {
        ssize_t r = ::read(fd, buf, n);
        if (r > 0) return r;
        if (r == 0) return 0;
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return -2;
#ifdef __linux__
        if (errno == EIO) return 0; // PTY master: treat as EOF
#endif
        return -1;
    }
}

// Nonblocking write result:
//  >0 bytes written
//  -2 would block
//  -1 fatal
static ssize_t write_nb(int fd, const void* buf, size_t n) {
    for (;;) {
        ssize_t w = ::write(fd, buf, n);
        if (w > 0) return w;
        if (w == 0) return -2;
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return -2;
        return -1;
    }
}

// -------------------- winsize --------------------

static bool get_winsize(int fd, int& rows, int& cols) {
    struct winsize ws;
    if (ioctl(fd, TIOCGWINSZ, &ws) != 0) return false;
    if (ws.ws_row <= 0 || ws.ws_col <= 0) return false;
    rows = (int)ws.ws_row;
    cols = (int)ws.ws_col;
    return true;
}

static void set_winsize(int fd, int rows, int cols) {
    struct winsize ws;
    std::memset(&ws, 0, sizeof(ws));
    ws.ws_row = (unsigned short)rows;
    ws.ws_col = (unsigned short)cols;
    (void)ioctl(fd, TIOCSWINSZ, &ws);
}

// -------------------- token bucket --------------------

struct TokenBucket {
    double rate_bps;
    double capacity;
    double tokens = 0.0;
    double last = 0.0;

    TokenBucket(double rate, double cap) : rate_bps(rate), capacity(cap) {}

    void init(double now) { tokens = capacity; last = now; }

    void refill(double now) {
        double dt = now - last;
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

// -------------------- buffer --------------------

struct Buf {
    std::vector<unsigned char> data;
    size_t off = 0;

    size_t size() const { return (off <= data.size()) ? (data.size() - off) : 0; }
    bool empty() const { return size() == 0; }
    const unsigned char* ptr() const { return data.data() + off; }

    void append(const unsigned char* p, size_t n) {
        if (n == 0) return;
        if (off > 0 && off >= 65536 && off * 2 >= data.size()) {
            std::vector<unsigned char> tmp(data.begin() + (long)off, data.end());
            data.swap(tmp);
            off = 0;
        }
        data.insert(data.end(), p, p + n);
    }

    void consume(size_t n) {
        if (n == 0) return;
        size_t sz = size();
        if (n >= sz) { data.clear(); off = 0; return; }
        off += n;
    }
};

// -------------------- exit key parsing --------------------

static std::vector<unsigned char> parse_raw_seq(const std::string& raw) {
    std::vector<unsigned char> out;
    out.reserve(raw.size());

    auto hexval = [](char h) -> int {
        if (h >= '0' && h <= '9') return h - '0';
        if (h >= 'a' && h <= 'f') return 10 + (h - 'a');
        if (h >= 'A' && h <= 'F') return 10 + (h - 'A');
        return -1;
    };

    for (size_t i = 0; i < raw.size(); ++i) {
        unsigned char c = (unsigned char)raw[i];
        if (c != '\\') { out.push_back(c); continue; }
        if (i + 1 >= raw.size()) { out.push_back('\\'); break; }

        char n = raw[++i];
        if (n == 'e') out.push_back(0x1B);
        else if (n == 'n') out.push_back('\n');
        else if (n == 'r') out.push_back('\r');
        else if (n == 't') out.push_back('\t');
        else if (n == '\\') out.push_back('\\');
        else if (n == 'x') {
            if (i + 2 < raw.size()) {
                int h1 = hexval(raw[i + 1]);
                int h2 = hexval(raw[i + 2]);
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



// -------------------- escape help --------------------

static void print_escape_help(const char* prog, const std::string& exit_key_spec, int arm_ms) {
    (void)prog;
    // Printed to stderr so it doesn't mix as badly with PTY stdout.
    dprintf(2,
        "\r\nEscape controls (prefix: %s)\r\n"
        "  %s  ? / h\tShow this help\r\n"
        "  %s  %s   \tSend literal prefix to the child\r\n"
        "  %s  q    \tQuit immediately (SIGHUP child pgrp)\r\n"
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
// -------------------- CLI --------------------

struct Args {
    int baud = -1;
    int data_bits = 8;
    char parity = 'n';
    double stop_bits = 1.0;
    int burst_ms = 200;

    std::string exit_key = "ctrl+f7";
    int exit_arm_ms = 250;

    // Diagnostic: print the final resolved configuration (after defaults + rc + CLI)
    // and exit without spawning the PTY.
    bool print_effective = false;

    bool has_cmd = false;
    std::vector<std::string> cmd;
};

static void print_help(const char* prog) {
    dprintf(1,
        "baudsh — throttle a PTY to simulate serial baud rates\n\n"
        "Usage:\n"
        "  %s --baud <rate> [options] [--cmd <command...>]\n\n"
        "Options:\n"
        "  --baud <rate>          Required, unless specified in config (e.g. 2400)\n"
        "  --data-bits <5|6|7|8>  Default: 8\n"
        "  --parity <n|e|o>       Default: n\n"
        "  --stop-bits <1|1.5|2>  Default: 1\n"
        "  --burst-ms <ms>        Default: 200\n"
        "  --exit-key <spec>      ctrl+f7 | ctrl+<key> | raw:<seq> | none (default: ctrl+f7)\n"
        "  --exit-arm-ms <ms>     Default: 250\n"
        "  --cmd <...>            Run command. If single string, uses /bin/sh -lc.\n"
        "                         [must be last flag if used]\n"
        "  --print-effective      Print resolved settings and exit\n"
        "  --print-default        Print built-in default config template and exit\n"
        "  --autocomp <shell>     Print autocomplete for dash|bash|zsh|fish and exit\n"
        "  --version              Print version and exit\n"
        "  -h, --help             Show help\n\n"
        "Defaults file:\n"
        "  ~/.baudsh.rc or ~/.config/baudsh/baudsh.conf (key=value; keys match flags)\n"
        "  If both files exist the values in ~/.baudsh.rc will be used.\n"
        "  The --cmd flag cannot be used in config if present will be ignored.\n\n"
        "Examples:\n"
        "  %s --baud 2400\n"
        "  %s --baud 2400 --cmd /bin/bash -l\n"
        "  %s --baud 2400 --cmd bash -c 'cat /etc/os-release'\n"
        "  %s --baud 2400 --cmd \"bash -c 'echo hi'\"\n"
        "  %s --baud 2400 --exit-key none --cmd \"echo hi\"\n",
        prog, prog, prog, prog, prog, prog
    );
}

static bool parse_int(const char* text, int& value) {
    char* end = nullptr;
    errno = 0;
    long n = std::strtol(text, &end, 10);
    if (errno || end == text || *end != '\0' || n < INT_MIN || n > INT_MAX) return false;
    value = static_cast<int>(n);
    return true;
}

static bool parse_double(const char* text, double& value) {
    char* end = nullptr;
    errno = 0;
    double n = std::strtod(text, &end);
    if (errno || end == text || *end != '\0') return false;
    value = n;
    return true;
}

static bool parse_args(int argc, char** argv, Args& out) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];

        if (a == "-h" || a == "--help") {
            print_help(argv[0]);
            std::exit(0);
        } else if (a == "--baud") {
            if (i + 1 >= argc) return false;
            if (!parse_int(argv[++i], out.baud) || out.baud <= 0) return false;
        } else if (a == "--data-bits") {
            if (i + 1 >= argc) return false;
            if (!parse_int(argv[++i], out.data_bits)) return false;
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
            if (!parse_double(argv[++i], out.stop_bits)) return false;
            if (!(out.stop_bits == 1.0 || out.stop_bits == 1.5 || out.stop_bits == 2.0)) return false;
        } else if (a == "--burst-ms") {
            if (i + 1 >= argc) return false;
            if (!parse_int(argv[++i], out.burst_ms) || out.burst_ms < 1) return false;
        } else if (a == "--exit-key") {
            if (i + 1 >= argc) return false;
            out.exit_key = argv[++i];
        } else if (a == "--exit-arm-ms") {
            if (i + 1 >= argc) return false;
            if (!parse_int(argv[++i], out.exit_arm_ms) || out.exit_arm_ms < 0) return false;
        } else if (a == "--cmd") {
            if (i + 1 >= argc) return false;
            out.has_cmd = true;
            out.cmd.clear();
            for (int j = i + 1; j < argc; ++j) out.cmd.emplace_back(argv[j]);
            break;
        } else if (a == "--print-effective") {
            out.print_effective = true;
        } else if (a == "--autocomp" || a == "--version" || a == "--print-default") {
            // Handled by baudsh::handle_early_flags() in main.
            // Treat as an argument error here in case parsing is called without it.
            return false;
        } else {
            return false;
        }
    }
    // baud can be provided by config; only require it in the final resolved args.
    return (out.baud > 0 || (out.baud == -1 && out.print_effective)) &&
        out.data_bits >= 5 && out.data_bits <= 8 &&
        (out.parity == 'n' || out.parity == 'e' || out.parity == 'o') &&
        (out.stop_bits == 1.0 || out.stop_bits == 1.5 || out.stop_bits == 2.0) &&
        out.burst_ms >= 1 && out.exit_arm_ms >= 0;
}

// -------------------- spawn PTY (forkpty = python pty.fork semantics) --------------------

struct SpawnResult {
    pid_t pid = -1;      // child PID in parent; 0 in child; -1 on error
    int master_fd = -1;  // valid in parent; undefined in child
};

static SpawnResult spawn_pty_forkpty(const termios& parent_term) {
    SpawnResult res;

    int master_fd = -1;

    // Always provide explicit termios + winsize so forkpty() doesn't try to tcgetattr() something non-tty.
    termios slave_term = parent_term;

    struct winsize ws;
    std::memset(&ws, 0, sizeof(ws));
    // If stdin isn't a tty, this fails; that's fine.
    if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) != 0) {
        std::memset(&ws, 0, sizeof(ws));
    }

    errno = 0;
    pid_t pid = forkpty(&master_fd, nullptr, &slave_term, &ws);
    if (pid < 0) {
        return res; // pid=-1 master=-1
    }

    // forkpty() already created a new session in the child and made the PTY slave the controlling TTY.
    // For interactive shells (bash/dash/fish) to get consistent job control (and for us to be able to
    // SIGHUP the whole foreground job on escape-quit), make the child a process-group leader and
    // foreground that group on the controlling tty.
    if (pid == 0) {
        (void)setpgid(0, 0);
        (void)tcsetpgrp(STDIN_FILENO, getpgrp());
    }

    res.pid = pid;         // 0 in child, >0 in parent
    res.master_fd = (pid == 0) ? -1 : master_fd;
    return res;
}

// -------------------- child exec --------------------

static void child_exec_or_die(const Args& args) {
    auto die = [](const char* msg) {
        dprintf(2, "baudsh: %s: %s\n", msg, strerror(errno));
        _exit(127);
    };

    const char* env_shell = getenv("SHELL");
    const char* default_shell = (env_shell && *env_shell) ? env_shell : "/bin/bash";

    if (!args.has_cmd || args.cmd.empty()) {
        // Interactive login shell
        execlp(default_shell, default_shell, "-l", (char*)nullptr);
        die("exec login shell failed");
    }

    if (args.cmd.size() == 1) {
        execl("/bin/sh", "sh", "-lc", args.cmd[0].c_str(), (char*)nullptr);
        die("exec shell failed");
    }

    std::vector<char*> cargv;
    cargv.reserve(args.cmd.size() + 1);
    for (auto& s : args.cmd) cargv.push_back(const_cast<char*>(s.c_str()));
    cargv.push_back(nullptr);
    execvp(cargv[0], cargv.data());
    die("execvp failed");
}

// -------------------- main --------------------

int baudsh_posix_main(int argc, char** argv) {
    // Handle "print and exit" flags early (no need to read config).
    (void)baudsh::handle_early_flags(argc, argv);

    Args args;

    // Load rc defaults, then let CLI override them.
    baudsh::load_rc_files(args);

    if (!parse_args(argc, argv, args)) {
        dprintf(2, "baudsh: invalid arguments\n\n");
        print_help(argv[0]);
        return 64;
    }

    if (args.print_effective) {
        dprintf(1,
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

    // exit key
    bool exit_enabled = true;
    std::vector<unsigned char> exit_seq;
    {
        std::string ek = args.exit_key;
        for (auto& ch : ek) ch = (char)std::tolower((unsigned char)ch);

        if (ek.empty() || ek == "none") {
            exit_enabled = false;
        } else if (ek == "ctrl+f7") {
            const unsigned char seq[] = {0x1B,'[','1','8',';','5','~'};
            exit_seq.assign(seq, seq + sizeof(seq));
        } else if (ek.size() == 6 && ek.compare(0, 5, "ctrl+") == 0 &&
                   ((ek[5] >= 'a' && ek[5] <= 'z') || ek[5] == ']' || ek[5] == '[' || ek[5] == '\\' || ek[5] == '^' || ek[5] == '_')) {
            exit_seq.push_back(static_cast<unsigned char>(std::toupper(static_cast<unsigned char>(ek[5])) & 0x1f));
        } else if (ek.rfind("raw:", 0) == 0) {
            exit_seq = parse_raw_seq(args.exit_key.substr(4));
            if (exit_seq.empty()) exit_enabled = false;
        } else {
            exit_seq.assign(args.exit_key.begin(), args.exit_key.end());
            if (exit_seq.empty()) exit_enabled = false;
        }
    }

    // signals
    {
        struct sigaction sa;
        std::memset(&sa, 0, sizeof(sa));
        sa.sa_handler = on_winch;
        sigaction(SIGWINCH, &sa, nullptr);
    }
    {
        struct sigaction sa;
        std::memset(&sa, 0, sizeof(sa));
        sa.sa_handler = on_term;
        sigaction(SIGINT, &sa, nullptr);
        sigaction(SIGTERM, &sa, nullptr);
        sigaction(SIGHUP, &sa, nullptr);
        sigaction(SIGQUIT, &sa, nullptr);
    }
    {
        struct sigaction sa{};
        sa.sa_handler = on_suspend;
        sigaction(SIGTSTP, &sa, nullptr);
    }

    // read terminal attributes
    termios old_term;
    if (tcgetattr(STDIN_FILENO, &old_term) != 0) {
        dprintf(2, "baudsh: tcgetattr failed: %s\n", strerror(errno));
        return 1;
    }

    // put local terminal into raw mode (so we forward bytes cleanly)
    termios raw = old_term;
    cfmakeraw(&raw);
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) {
        dprintf(2, "baudsh: tcsetattr failed: %s\n", strerror(errno));
        return 1;
    }

    auto restore_local_tty = [&]() {
        (void)tcsetattr(STDIN_FILENO, TCSANOW, &old_term);
    };

    // spawn PTY (forkpty = python pty.fork semantics; avoids job-control issues in nested shells)
    SpawnResult sp = spawn_pty_forkpty(old_term);

    // IMPORTANT: child path first (do NOT treat master_fd=-1 as an error in child)
    if (sp.pid == 0) {
        child_exec_or_die(args);
        _exit(127);
    }

    // parent: now validate spawn result
    if (sp.pid < 0 || sp.master_fd < 0) {
        dprintf(2, "baudsh: openpty/fork failed: %s\n", strerror(errno));
        restore_local_tty();
        return 1;
    }

    pid_t child = sp.pid;
    int master_fd = sp.master_fd;

    // The child (pid==0 branch in spawn_pty_forkpty) tries to become a process-group leader (PGID=PID).
    // Confirm that so we can reliably SIGHUP the whole job on escape-quit.
    bool child_pgrp_ok = (getpgid(child) == child);

    // set initial winsize on master side too (some systems apply it via slave only; this is still useful)
    {
        int rows = 0, cols = 0;
        if (get_winsize(STDIN_FILENO, rows, cols)) {
            set_winsize(master_fd, rows, cols);
        }
    }

    // Preserve inherited descriptor flags as these can be shared with the parent shell.
    const int stdin_flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    const int stdout_flags = fcntl(STDOUT_FILENO, F_GETFL, 0);
    // Parent handles a closed output pipe and still restores the terminal.
    struct sigaction ignore_pipe{};
    ignore_pipe.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &ignore_pipe, nullptr);

    // nonblocking IO
    set_nonblocking(STDIN_FILENO);
    set_nonblocking(master_fd);
    set_nonblocking(STDOUT_FILENO);

    auto cleanup_all = [&]() {
        if (master_fd >= 0) close(master_fd);
        if (stdin_flags >= 0) (void)fcntl(STDIN_FILENO, F_SETFL, stdin_flags);
        if (stdout_flags >= 0) (void)fcntl(STDOUT_FILENO, F_SETFL, stdout_flags);
        restore_local_tty();
    };

    // throttle
    const double bps = baud_to_bytes_per_sec(args.baud, args.data_bits, args.parity, args.stop_bits);
    const double burst_seconds = std::max(0.01, (double)args.burst_ms / 1000.0);
    const double capacity = std::max(1.0, bps * burst_seconds);

    TokenBucket in_bucket(bps, capacity);
    TokenBucket out_bucket(bps, capacity);

    const double start_time = monotonic_seconds();
    in_bucket.init(start_time);
    out_bucket.init(start_time);

    Buf stdin_buf, pty_buf;

// Escape prefix state machine (for --exit-key)
bool esc_enabled = exit_enabled;
bool esc_armed = false;
double esc_armed_at = 0.0;
const double ESC_TIMEOUT_SEC = 1.5;

// Partial matching for the escape sequence while parsing stdin stream
std::vector<unsigned char> esc_partial;
size_t esc_match = 0;
double esc_partial_at = 0.0;
const double ESC_PARTIAL_TIMEOUT_SEC = 0.1;

std::vector<unsigned char> esc2_partial;
size_t esc2_match = 0;

    bool child_exited = false;
    int child_status = 0;
    bool master_eof = false;
    bool input_closed = false;
    bool io_error = false;
    constexpr size_t BUFFER_LIMIT = 65536;

    auto emergency_exit = [&]() {
        // Immediate local exit: do not wait for child to finish / buffers to drain.
        if (child_pgrp_ok) (void)killpg(child, SIGHUP);
        else (void)kill(child, SIGHUP);
        cleanup_all();
        _exit(0);
    };

    while (true) {
        bool did = false;

        const double now = monotonic_seconds();
        in_bucket.refill(now);
        out_bucket.refill(now);

        if (g_suspend_pending) {
            g_suspend_pending = 0;
            // Restore the invoking terminal before actually stopping this process.
            // The child has a separate PTY session, so suspend and resume its
            // foreground group along with the wrapper when one exists.
            pid_t foreground = tcgetpgrp(master_fd);
            if (foreground <= 0 || foreground == getpgrp()) foreground = child_pgrp_ok ? child : -1;
            if (foreground > 0) (void)killpg(foreground, SIGTSTP);
            if (stdin_flags >= 0) (void)fcntl(STDIN_FILENO, F_SETFL, stdin_flags);
            if (stdout_flags >= 0) (void)fcntl(STDOUT_FILENO, F_SETFL, stdout_flags);
            restore_local_tty();
            (void)kill(getpid(), SIGSTOP);
            // Execution continues here after SIGCONT.
            if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) {
                dprintf(2, "baudsh: failed to restore raw mode after resume: %s\n", strerror(errno));
                if (foreground > 0) (void)killpg(foreground, SIGHUP);
                else (void)kill(child, SIGHUP);
                io_error = true;
                break;
            }
            set_nonblocking(STDIN_FILENO);
            set_nonblocking(STDOUT_FILENO);
            if (foreground > 0) (void)killpg(foreground, SIGCONT);
        }

        if (g_term_pending) {
            int sig = g_term_pending;
            g_term_pending = 0;
            // The foreground job may differ from the shell's process group.
            pid_t foreground = tcgetpgrp(master_fd);
            if (foreground > 0 && foreground != getpgrp()) (void)killpg(foreground, sig);
            else if (child_pgrp_ok) (void)killpg(child, sig);
            else (void)kill(child, sig);
        }

        if (g_winch_pending) {
            g_winch_pending = 0;
            int rows = 0, cols = 0;
            if (get_winsize(STDIN_FILENO, rows, cols)) {
                set_winsize(master_fd, rows, cols);
                killpg(child, SIGWINCH);
            }
        }

        if (!child_exited) {
            pid_t w = waitpid(child, &child_status, WNOHANG);
            if (w == child) child_exited = true;
            else if (w < 0 && errno != EINTR) { child_exited = true; io_error = true; }
        }


// Flush an incomplete key sequence after a short inter-byte timeout.
if (esc_match && now - esc_partial_at >= ESC_PARTIAL_TIMEOUT_SEC) {
    stdin_buf.append(esc_partial.data(), esc_partial.size());
    esc_partial.clear();
    esc_match = 0;
}
if (esc_armed && now - esc_armed_at > ESC_TIMEOUT_SEC) {
    if (!esc2_partial.empty()) stdin_buf.append(esc2_partial.data(), esc2_partial.size());
    esc2_partial.clear();
    esc2_match = 0;
    esc_armed = false;
}

// stdin -> stdin_buf (with escape prefix parsing); stop reading when the queue is full.
while (!input_closed && !master_eof && !child_exited && stdin_buf.size() < BUFFER_LIMIT) {
    unsigned char tmp[8192];
    const size_t room = BUFFER_LIMIT - stdin_buf.size();
    ssize_t r = read_nb(STDIN_FILENO, tmp, std::min(sizeof(tmp), room));
    if (r > 0) {
        did = true;

        const double now2 = monotonic_seconds();
        const double ms_since_start = (now2 - start_time) * 1000.0;

        for (ssize_t i = 0; i < r; ++i) {
            unsigned char b = tmp[i];

            // Timeout: if prefix was pressed but no follow-up arrived, disarm.
            if (esc_armed && (now2 - esc_armed_at) > ESC_TIMEOUT_SEC) {
                esc_armed = false;
                if (!esc2_partial.empty()) stdin_buf.append(esc2_partial.data(), esc2_partial.size());
                esc2_partial.clear();
                esc2_match = 0;
            }

            // Escape prefix is only active after --exit-arm-ms
            bool esc_active = esc_enabled && (ms_since_start >= (double)args.exit_arm_ms) && !exit_seq.empty();

            if (esc_armed) {
                // Prefer detecting prefix+prefix (second prefix), which is a multi-byte sequence.
                if (esc_active) {
                    if (b == exit_seq[esc2_match]) {
                        esc2_partial.push_back(b);
                        esc2_match++;
                        if (esc2_match == exit_seq.size()) {
                            // prefix + prefix => send literal prefix to child
                            stdin_buf.append(exit_seq.data(), exit_seq.size());
                            esc_armed = false;
                            esc2_partial.clear();
                            esc2_match = 0;
                        }
                        continue; // consume byte as part of sequence
                    } else if (esc2_match > 0) {
                        // Mismatch mid-sequence: flush what we buffered and handle current byte normally.
                        stdin_buf.append(esc2_partial.data(), esc2_partial.size());
                        esc2_partial.clear();
                        esc2_match = 0;
                        esc_armed = false;
                        stdin_buf.append(&b, 1);
                        continue;
                    }
                }

                // Single-byte escape commands
                if (b == (unsigned char)'?' || b == (unsigned char)'h' || b == (unsigned char)'H') {
                    print_escape_help(argv[0], args.exit_key, args.exit_arm_ms);
                    esc_armed = false;
                    continue;
                }
                if (b == (unsigned char)'q' || b == (unsigned char)'Q') {
                    emergency_exit();
                }

                // Unknown escape command: forward byte to child and disarm.
                stdin_buf.append(&b, 1);
                esc_armed = false;
                continue;
            }

            // Not armed: try to match prefix sequence (may span reads)
            if (esc_active) {
                if (b == exit_seq[esc_match]) {
                    esc_partial.push_back(b);
                    esc_partial_at = now2;
                    esc_match++;
                    if (esc_match == exit_seq.size()) {
                        // Prefix detected: arm escape, do NOT forward prefix bytes.
                        esc_armed = true;
                        esc_armed_at = now2;
                        esc_partial.clear();
                        esc_match = 0;
                    }
                    continue; // consumed as part of potential prefix
                }

                // Mismatch: flush any partial bytes we buffered
                if (esc_match > 0) {
                    stdin_buf.append(esc_partial.data(), esc_partial.size());
                    esc_partial.clear();
                    esc_match = 0;
                }

                // If current byte could be start of prefix again, start buffering it.
                if (!exit_seq.empty() && b == exit_seq[0]) {
                    esc_partial.push_back(b);
                    esc_partial_at = now2;
                    esc_match = 1;
                    continue;
                }
            }

            // Normal forwarding
            stdin_buf.append(&b, 1);
        }

        continue;
    }
    if (r == -2) break;
    // Raw terminal input has no in-band EOF. Losing the invoking terminal
    // must not leave a detached wrapper waiting forever for more keystrokes.
    input_closed = true;
    io_error = true;
    break;
}
        if (io_error) {
            pid_t foreground = tcgetpgrp(master_fd);
            if (foreground > 0 && foreground != getpgrp()) (void)killpg(foreground, SIGHUP);
            if (!child_exited) (void)kill(child, SIGHUP);
            break;
        }

        // PTY -> pty_buf
        if (!master_eof) {
            while (pty_buf.size() < BUFFER_LIMIT) {
                unsigned char tmp[8192];
                ssize_t r = read_nb(master_fd, tmp, std::min(sizeof(tmp), BUFFER_LIMIT - pty_buf.size()));
                if (r > 0) {
                    did = true;
                    pty_buf.append(tmp, (size_t)r);
                    continue;
                }
                if (r == -2) break;
                master_eof = true;
                stdin_buf.consume(stdin_buf.size());
                esc_partial.clear();
                esc_match = 0;
                esc2_partial.clear();
                esc2_match = 0;
                esc_armed = false;
                break;
            }
        }

        // stdin_buf -> PTY (throttled)
        if (!stdin_buf.empty()) {
            size_t want = std::min<size_t>(stdin_buf.size(), 8192);
            size_t k = in_bucket.available(want);
            if (k > 0) {
                ssize_t w = write_nb(master_fd, stdin_buf.ptr(), k);
                if (w > 0) {
                    did = true;
                    in_bucket.consume((size_t)w);
                    stdin_buf.consume((size_t)w);
                } else if (w == -1) {
                    // No further input can be delivered to this PTY.
                    input_closed = true;
                    stdin_buf.consume(stdin_buf.size());
                }
            }
        }

        // pty_buf -> stdout (throttled)
        if (!pty_buf.empty()) {
            size_t want = std::min<size_t>(pty_buf.size(), 8192);
            size_t k = out_bucket.available(want);
            if (k > 0) {
                ssize_t w = write_nb(STDOUT_FILENO, pty_buf.ptr(), k);
                if (w > 0) {
                    did = true;
                    out_bucket.consume((size_t)w);
                    pty_buf.consume((size_t)w);
                } else if (w == -1) {
                    io_error = true;
                    if (!child_exited) {
                        if (child_pgrp_ok) (void)killpg(child, SIGHUP);
                        else (void)kill(child, SIGHUP);
                    }
                    break;
                }
            }
        }

        // Exit when child is gone AND PTY ended AND buffers drained
        if (child_exited && master_eof && stdin_buf.empty() && pty_buf.empty()) {
            break;
        }

        if (!did) usleep(2000);
    }

    cleanup_all();
    if (io_error) return 1;
    if (WIFEXITED(child_status)) return WEXITSTATUS(child_status);
    if (WIFSIGNALED(child_status)) return 128 + WTERMSIG(child_status);
    return 1;
}
