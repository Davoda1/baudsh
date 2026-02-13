# baudsh

*A slightly ridiculous but technically correct serial‑baud simulator for shells.*

## What is this?

**baudsh** is a PTY wrapper that simulates old serial baud rates by throttling bytes in both directions between your terminal and a spawned shell.

It is not meant to be a production-grade terminal multiplexer.
It is not meant to replace `screen`, `tmux`, or real serial tools.

It exists because:

- Simulating slow terminals is fun.
- Testing legacy / embedded behavior is interesting.
- Watching a shell crawl at 300 baud is strangely satisfying.

## Project Status

- Linux version: stable and unchanged
- Windows version: ported and functional, but subject to Windows console limitations

The original implementation targets Linux PTY semantics and remains the reference behavior.

The Windows version exists for portability and experimentation — it is not a perfect POSIX equivalent (because Windows is not POSIX).

## Platform Support

| Platform | Status | Backend                            |
| -------- | ------ | ---------------------------------- |
| Linux    | Stable | `openpty()` + `fork()` + `termios` |
| Windows  | Ported | ConPTY                             |

### Important Differences

| Feature      | Linux            | Windows                      |
| ------------ | ---------------- | ---------------------------- |
| PTY model    | Native POSIX PTY | ConPTY                       |
| Job control  | Full             | Not truly supported          |
| Signals      | POSIX signals    | Limited                      |
| Line endings | LF               | May involve CRLF translation |

If you see oddities on Windows such as:

- Double line spacing
- Missing newlines
- Slightly different shell nesting behavior

Those are console/ConPTY characteristics, not throttling logic bugs.

## Building

This project uses a **Makefile**.

### Linux

Typical build:

```
make
```

This compiles the Linux PTY implementation.

Depending on configuration, it may link with `-lutil` and use static or dynamic flags as defined in the Makefile.

To clean:

```
make clean
```

### Windows

The Windows port does **not** use the Linux Makefile directly.
It must be built using either:

- MSVC (`cl`)
- MinGW

Example (MinGW):

```
g++ -std=c++17 -O2 baudsh.cpp -o baudsh.exe
```

Windows requires:

- Windows 10 1809+ (ConPTY support)

## What It Actually Does

At runtime, baudsh:

1. Creates a PTY (platform specific)
2. Spawns a shell attached to the PTY slave
3. Relays data between:
   - Your real terminal
   - The spawned shell
4. Throttles the byte stream to simulate a configured baud rate

The throttling applies in both directions.

It behaves like a “transparent slow serial cable” between you and your shell.

## What It Is NOT

- Not a secure sandbox
- Not a terminal emulator
- Not a full TTY virtualization framework
- Not a serious production serial testing suite

It is intentionally minimal and focused on one idea: **slow down the bytes**.

## Known Limitations

### Linux

- Requires proper PTY support
- Static linking increases binary size

### Windows

- No real POSIX job control
- Console behavior differs from Linux
- CR/LF translation may occur
- Some nested shells behave slightly differently

## When Is This Useful?

- Testing legacy software assumptions
- Debugging interactive timing issues
- Demonstrating historical terminal speeds
- Fun experimentation

## License

MIT

---

> ## Final Note
> 
> The Linux version is the canonical implementation.
> 
> The Windows port is a best-effort adaptation to a fundamentally different console architecture.
> 
> If something behaves differently between platforms, that is expected — especially around signals and job control.
> 
> If something behaves incorrectly on Linux, that is likely a real bug.
