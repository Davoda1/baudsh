# Contributing

Keep changes focused on terminal behavior, timing, portability and clear documentation. Use `master` as the base for contributions.

## Build and test

From the repository root on Linux:

```sh
make release CXXFLAGS=-Werror
python3 -m unittest discover -s tests -v
```

On Windows, build with MinGW-w64 and run the same test suite with `python`:

```sh
make static CXXFLAGS=-Werror
python -m unittest discover -s tests -v
```

Set `BAUDSH_BINARY` to test a binary outside the repository root. Run only one build mode per invocation. A cross-build proves that the source compiles; platform-specific runtime changes need tests on that operating system too.

For terminal or input changes, check ordinary shell use, the configured escape prefix, literal Escape, navigation keys, output completion and clean exit. On Linux, also check terminal restoration and suspend/resume. On Windows, check the relevant console host, Unicode input and paths, and resizing.

## Changes and commits

- Describe the behavior being fixed and add a regression test where practical.
- Keep commit titles specific and put the problem, solution and relevant validation in the commit body.
- Update the README when options, defaults, build commands or supported behavior change.
- Do not include generated binaries, credentials, local configuration, private logs or unrelated files in source commits.
- Check `git diff --check` and inspect the staged diff before committing.

## Reporting issues

Include the operating system, terminal host, baudsh version, command-line options, expected behavior and the smallest reproducible example. For build failures, include the compiler version and relevant error output. Remove private command arguments, paths, environment values and credentials before sharing logs.

A screenshot can help explain display problems, but text commands and output are preferable when they reproduce the issue.
