# Changelog

## 1.0.0

The first packaged release for Windows x64 and Linux amd64.

### Features

- Bidirectional terminal throttling using Linux PTYs and Windows ConPTY.
- Configurable baud, framing parameters, burst window and local escape prefix.
- Command execution, configuration files, effective-setting output and shell-completion generation.

### Corrections

- Preserve child exit status and restore Linux terminal settings and descriptor flags on supported exit, suspension and resume paths.
- Handle incomplete escape sequences, queued input after child exit and disconnection of the invoking terminal.
- Validate numeric settings and keep child arguments separate from baudsh options.
- Read commented, BOM-prefixed and long configuration lines consistently.
- Correct Windows shell quoting, Unicode arguments and configuration paths, navigation-key input, console resizing and shutdown handling.
- Rebuild correctly when switching build modes; reject conflicting build targets and surface tool failures.

### Development

- Add automated regression tests and continuous integration for Linux and Windows.
- Replace outdated build instructions with platform-specific usage and configuration documentation.
