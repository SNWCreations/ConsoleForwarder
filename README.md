# ConsoleForwarder

A Windows utility that captures console output from programs that create their own console window (like Valve's Source Engine Dedicated Server) and forwards it to standard output/error streams.

## Why This Tool?

Some Windows programs, notably Valve's Source Engine Dedicated Server (`srcds.exe`), don't write their output to standard streams (stdout/stderr). Instead, they:

1. Create their own console window using `AllocConsole()`
2. Write directly to the console using `WriteFile()` or `WriteConsole()`

This means **normal output redirection doesn't work**:

```cmd
# This captures NOTHING - srcds output goes to its own console window, not stdout
srcds.exe -game tf +map cp_dustbowl > server.log 2>&1
```

This is a problem when you want to:
- **Run srcds as a background service** and collect logs
- **Monitor server output** from a process manager (systemd, Docker, PM2, etc.)
- **Pipe output** to log aggregation tools
- **Run headless** without a visible console window

ConsoleForwarder solves this by intercepting the console output and forwarding it to standard streams:

```cmd
# This works - output is captured to server.log
ConsoleForwarder --mode inject srcds.exe -game tf +map cp_dustbowl > server.log 2>&1
```

## Features

- Captures output from programs that use `AllocConsole()` and write directly to the console
- Multiple capture methods: ConPTY, Legacy console buffer reading, and DLL injection
- Supports command-line arguments and argument files (`@argfile` syntax like JVM)
- Forwards stdin to the child process (injection mode)
- Separates stdout and stderr streams (injection mode)
- Option to hide the child process console window
- Graceful shutdown handling:
  - Source Engine programs: sends "quit" command automatically
  - Other programs: sends WM_CLOSE to console window
- Supports both 32-bit and 64-bit targets

## Build

Requirements:
- CMake 3.15+
- Visual Studio 2019/2022 or compatible C++17 compiler
- Windows SDK

### Using build.bat (Recommended)

```cmd
build.bat                    # Build Release (both 64-bit and 32-bit)
build.bat debug              # Build Debug (both architectures)
build.bat x64                # Build Release 64-bit only
build.bat x86                # Build Release 32-bit only
build.bat debug 32           # Build Debug 32-bit only
build.bat rebuild            # Clean and build Release (both)
build.bat clean              # Remove all build directories
```

### Using CMake directly

```cmd
# 64-bit
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release

# 32-bit
cmake -B build32 -G "Visual Studio 17 2022" -A Win32
cmake --build build32 --config Release
```

Output files:
- 64-bit: `build\Release\`
- 32-bit: `build32\Release\`

Each contains:
- `ConsoleForwarder.exe` - Main launcher
- `ConsoleHook.dll` - Hook DLL for injection mode

## Usage

```
ConsoleForwarder [options] <program> [program arguments...]
```

### Options

| Option | Description |
|--------|-------------|
| `-h`, `--help` | Show help message |
| `--mode <mode>` | Capture mode: `auto`, `conpty`, `legacy`, `inject` (default: `auto`) |
| `--hide` | Hide the child process console window |
| `--show` | Show the child process console window (default) |

### Argument File

Use `@filename` to read arguments from a file (one argument per line):

```
# server_args.txt
-game
tf
+maxplayers
24
+map
cp_dustbowl
```

### Capture Modes

| Mode | Description | Requirements |
|------|-------------|--------------|
| `auto` | Automatically select best method (prefers ConPTY, falls back to inject) | - |
| `conpty` | Windows Pseudo Console API | Windows 10 1809+ |
| `legacy` | Console buffer polling | Any Windows |
| `inject` | DLL injection to hook WriteConsole/WriteFile | Any Windows |

**Note**: For programs like Source Engine dedicated servers that use `WriteFile()` directly to console handles, use `--mode inject` for best results.

## Examples

Basic usage:
```cmd
ConsoleForwarder srcds.exe -game tf +maxplayers 24
```

Using injection mode with hidden window:
```cmd
ConsoleForwarder --mode inject --hide srcds.exe -game tf
```

Using argument file:
```cmd
ConsoleForwarder srcds.exe @server_args.txt
```

Redirect output to file:
```cmd
ConsoleForwarder --mode inject srcds.exe -game tf > server.log 2>&1
```

Run from PowerShell or cmd (output appears in terminal):
```cmd
.\ConsoleForwarder.exe --mode inject srcds.exe -console -game left4dead2 +map c1m1_hotel
```

## How It Works

### ConPTY Mode (Win10 1809+)

Uses the Windows Pseudo Console API to create a virtual terminal. The child process is started with the pseudo console attached, and all console output is captured through a pipe.

### Legacy Mode

Attaches to the child process's console and periodically reads the console screen buffer using `ReadConsoleOutputCharacter`. This mode has some limitations with real-time output.

### Inject Mode

Injects `ConsoleHook.dll` into the target process using `CreateRemoteThread` + `LoadLibraryW`. The DLL hooks:
- `WriteConsoleA` / `WriteConsoleW` - Direct console write functions
- `WriteFile` - For programs that write to console handles via WriteFile
- `LoadLibraryA/W/ExA/ExW` - To hook dynamically loaded modules

Intercepted output is forwarded through named pipes to the launcher process, which writes to its stdout/stderr. This allows the launcher to be used from terminals (cmd, PowerShell, Windows Terminal) with output appearing directly in the terminal.

The hook also forwards stdin from the launcher to the target process's console input, allowing interactive use.

### Graceful Shutdown

When the launcher receives a termination signal (Ctrl+C, window close, etc.):

- **Source Engine programs** (srcds.exe, hl2.exe, csgo.exe, left4dead2.exe, portal2.exe): Automatically sends the "quit" command through stdin, allowing the server to shut down gracefully and save state.
- **Other programs**: Sends `WM_CLOSE` message to the target's console window, equivalent to clicking the X button.

The launcher then waits for the target process to exit before terminating.

## Use Case

This tool is designed for programs like Valve's Source Engine Dedicated Server (`srcds.exe`) that:
1. Call `AllocConsole()` to create their own console window
2. Write output using `WriteFile()` or `WriteConsole()` directly
3. Don't use standard stdout/stderr streams

These programs cannot be captured using normal pipe redirection. ConsoleForwarder solves this by intercepting the console output at various levels.

## Architecture Notes

- Use 32-bit build (`build32\Release\`) for 32-bit target programs
- Use 64-bit build (`build\Release\`) for 64-bit target programs
- The launcher and hook DLL must match the target's architecture

## License

Copyright (C) 2026 SNWCreations, Licensed under MIT License.
