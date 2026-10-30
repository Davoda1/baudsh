# baudsh

[![CI](https://github.com/Davoda1/baudsh/actions/workflows/ci.yml/badge.svg?branch=master)](https://github.com/Davoda1/baudsh/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

**Simulate a slow serial terminal by throttling a shell's input and output.**

baudsh runs a shell or command in a pseudo-terminal and limits the bytes flowing in each direction. Use it to demonstrate historical terminal speeds, explore interactive timing, or watch a modern shell operate at 300 baud.

It does not open serial ports, change hardware settings, or isolate the command it runs.

## Status and downloads

Version **1.0.0** is being prepared. There are no published binary releases yet; build from source using the instructions below. When available, release packages will appear on the [Releases page](https://github.com/Davoda1/baudsh/releases).

The [CI workflow](https://github.com/Davoda1/baudsh/actions/workflows/ci.yml) is the source of current build and automated test results. Manual console testing is also needed to assess host-specific keyboard behavior and terminal applications.

## Platforms

| Platform | Backend | Requirements |
| --- | --- | --- |
| Linux | `forkpty()` and `termios` | A terminal with PTY support; C++17 compiler and GNU Make to build |
| Windows | ConPTY | Windows 10 version 1809 or later, including Windows 11; MinGW-w64 or MSVC to build |

Linux and Windows share the main options, but their shells, signals, quoting rules and console behavior differ. Windows does not provide POSIX job control. Other operating systems are not currently verified targets.

## Quick start

After building on Linux:

```sh
./baudsh --baud 300
```

After building on Windows, run from PowerShell or Command Prompt:

```powershell
.\baudsh.exe --baud 300
```

Without `--cmd`, Linux starts the shell named by `SHELL` as a login shell, falling back to `/bin/bash`. Windows starts the system `cmd.exe`.

Type `exit` to leave the child shell normally. For an immediate local exit, press **Ctrl+F7**, release it, then press **q**. If your terminal reserves Ctrl+F7, select a different prefix:

```sh
./baudsh --baud 2400 --exit-key 'ctrl+]'
```

## Building

### Linux

Install a C++17 compiler, GNU Make and the standard C/C++ development libraries, then run:

```sh
make
```

This produces `baudsh` in the repository root. Available build modes are:

| Command | Output |
| --- | --- |
| `make` or `make release` | Optimized, stripped executable using the system runtime libraries |
| `make static` | Optimized, stripped executable with static libraries where supported |
| `make debug` | Unstripped executable with debug information |
| `make clean` | Removes the executable and object files |
| `make help` | Shows the detected platform and build targets |

Static builds require the toolchain's static libraries. Run one build mode per invocation; run `make clean` separately from a build. Switching modes rebuilds the output file.

An optional system-wide installation of a locally built binary is:

```sh
sudo install -m 0755 baudsh /usr/local/bin/baudsh
```

To uninstall that copy, remove `/usr/local/bin/baudsh`. Configuration files are kept separately.

### Windows with MinGW-w64

Use a MinGW-w64 environment such as the **MSYS2 UCRT64** shell, with GNU Make and the matching C++ compiler and binutils installed:

```sh
make static
```

This produces `baudsh.exe` in the repository root. The static target includes the MinGW runtime libraries; the program still uses Windows system DLLs.

The equivalent direct compiler command is:

```sh
g++ -std=c++17 -O2 -Wall -Wextra -pedantic baudsh_main.cpp baudsh_win.cpp -o baudsh.exe -static -static-libstdc++ -static-libgcc
```

Keep the executable in a directory of your choice and run it from a terminal. Add that directory to `PATH` if desired. No launcher script is required.

### Windows with MSVC

Open an **x64 Native Tools Command Prompt for Visual Studio** with the C++ build tools and Windows SDK installed. From the repository root, run:

```bat
cl /std:c++17 /EHsc /W4 /O2 /MT /D_CRT_SECURE_NO_WARNINGS baudsh_main.cpp baudsh_win.cpp /Fe:baudsh.exe
```

This produces `baudsh.exe` with the static MSVC runtime. Run the same Windows test command shown below after building.

### Cross-compiling Windows from Linux

With a MinGW-w64 cross-toolchain installed:

```sh
make static IS_WIN=1 CXX=x86_64-w64-mingw32-g++ STRIP=x86_64-w64-mingw32-strip
```

Cross-compilation checks the build, but does not replace testing the executable on Windows.

## Commands and options

Place `--cmd` last: every following argument belongs to the child command, including arguments such as `--help` or `--version`.

| Option | Meaning | Default |
| --- | --- | --- |
| `--baud RATE` | Positive serial bit rate | Required on the command line or in configuration |
| `--data-bits 5\|6\|7\|8` | Data bits used in the timing calculation | `8` |
| `--parity n\|e\|o` | None, even or odd parity for timing | `n` |
| `--stop-bits 1\|1.5\|2` | Stop bits used in the timing calculation | `1` |
| `--burst-ms MS` | Token-bucket burst window; positive integer | `200` |
| `--exit-key SPEC` | Local escape prefix, or `none` to disable | `ctrl+f7` |
| `--exit-arm-ms MS` | Startup delay before the escape prefix becomes active | `250` |
| `--cmd COMMAND...` | Run a command instead of the default shell | — |
| `--print-effective` | Print the resolved settings without starting a command | — |
| `--print-default` | Print a configuration template | — |
| `--autocomp SHELL` | Emit shell completion definitions | — |
| `--version` | Print the program version | — |
| `-h`, `--help` | Show command-line help | — |

Data bits, parity and stop bits affect timing only; baudsh does not truncate bytes or generate parity bits. The nominal rate in each direction is:

```text
bytes per second = baud / (1 + data_bits + parity_bits + stop_bits)
```

Here, `1` is the start bit and `parity_bits` is `0` for none or `1` for even/odd parity. For 8N1 at 300 baud, that is 30 bytes per second. Input and output use independent limits. Scheduling and the initial token-bucket burst mean this is an approximation, not a bit-level hardware simulation. The internal burst window has a 10 ms minimum.

### Running a command

On Linux, multiple arguments after `--cmd` are executed directly:

```sh
./baudsh --baud 9600 --cmd /bin/bash -l
./baudsh --baud 2400 --cmd /bin/sh -c 'printf "Hello from baudsh\n"'
```

A single argument is interpreted by `/bin/sh -lc`:

```sh
./baudsh --baud 2400 --cmd 'printf "Hello\n"; uname -s'
```

On Windows, multiple arguments use Windows process argument quoting. A single argument is interpreted by the system `cmd.exe /D /S /C`:

```powershell
.\baudsh.exe --baud 2400 --cmd cmd.exe /D /C ver
.\baudsh.exe --baud 2400 --cmd 'echo Hello & ver'
```

Shell syntax is intentional in the single-string form. Use trusted command strings. Normal completion returns the child's exit code; on Linux, signal termination is reported as `128 + signal number`. Invalid options return `64`.

## Configuration

Configuration files use `key=value` or `key value`. Keys match the timing and escape options without the leading `--`. The command itself cannot be stored in configuration.

Files are loaded in this order, with later settings taking precedence:

| Platform | First file | Higher-priority file |
| --- | --- | --- |
| Linux | `~/.config/baudsh/baudsh.conf` | `~/.baudsh.rc` |
| Windows | `%APPDATA%\baudsh\baudsh.conf` | `.baudsh.rc` in the home directory |

On Windows, the home directory is resolved from `HOME`, then `USERPROFILE`, then `HOMEDRIVE` plus `HOMEPATH`. Command-line values override both files.

Example:

```ini
baud=2400
# 8N1 framing for timing purposes
data-bits=8
parity=n
stop-bits=1
burst-ms=200
exit-key=ctrl+f7
exit-arm-ms=250
```

Blank lines and lines beginning with `#` or `;` are ignored. Inline comments must be preceded by whitespace. UTF-8 files with or without a byte-order mark are accepted. Unknown keys are ignored; invalid recognized numeric or framing values fail validation unless replaced by a valid higher-priority setting.

Use `--print-default` to obtain a template or see [extras/baudsh.conf](extras/baudsh.conf) for a commented example. The template leaves baud commented out so it does not silently choose a speed. Use `--print-effective` to inspect the settings that will be used.

### Escape controls

After pressing the configured prefix, enter one of the following within 1.5 seconds:

| Follow-up | Action |
| --- | --- |
| `q` | Quit immediately; queued output may be discarded |
| `?` or `h` | Show escape help |
| The prefix again | Forward one literal prefix to the child |

`--exit-arm-ms` controls the delay after program startup, not the follow-up timeout. Prefixes include `ctrl+f7`, `ctrl+]`, `ctrl+q`, a literal string, or `raw:` followed by escaped bytes. For example, in a POSIX shell:

```sh
./baudsh --baud 2400 --exit-key 'raw:\e[18;5~'
```

Supported raw escapes include `\e`, `\n`, `\r`, `\t`, `\\` and `\xHH`. Prefix bytes depend on the terminal. A standalone Escape key is forwarded after a short sequence-detection timeout.

## Tests

Build first, then run the Python standard-library test suite from the repository root:

```sh
python3 -m unittest discover -s tests -v
```

On Windows, use `python` in place of `python3`. The default executable is `baudsh` or `baudsh.exe` in the repository root. To test another build, set `BAUDSH_BINARY` to its path before running the suite.

The suite covers command-line and configuration behavior, with platform-specific integration tests for Linux PTYs and Windows ConPTY. Tests for the other operating system are skipped. See [CONTRIBUTING.md](CONTRIBUTING.md) for development checks and issue-reporting guidance.

## Limitations

- This is a terminal timing tool, not a serial-device driver, network limiter, terminal emulator or security sandbox.
- Linux runtime sessions require terminal input; redirect the child command's input inside the child shell when needed.
- Long outputs at low baud rates take time to drain. Use the escape quit command if you intend to discard the remainder.
- Framing affects the calculated byte rate only. ANSI control sequences and multibyte text are throttled byte by byte.
- Windows behavior depends on ConPTY, the host console and the application inside it. Keyboard shortcuts reserved by the host may not reach baudsh.
- Terminal cleanup cannot run after an uncatchable termination such as `SIGKILL` on Linux.

Unexpected output loss, stuck sessions and incorrect terminal restoration are bugs worth reporting, rather than assumed platform differences.

## License

[MIT](LICENSE). Copyright © 2026 davoda1.
