#include "legacy_console.h"
#include <thread>
#include <atomic>
#include <chrono>

void LegacyConsoleHandle::Close() {
    running = false;
    if (hStdinWrite != INVALID_HANDLE_VALUE) {
        CloseHandle(hStdinWrite);
        hStdinWrite = INVALID_HANDLE_VALUE;
    }
    if (hThread != INVALID_HANDLE_VALUE) {
        CloseHandle(hThread);
        hThread = INVALID_HANDLE_VALUE;
    }
    if (hProcess != INVALID_HANDLE_VALUE) {
        CloseHandle(hProcess);
        hProcess = INVALID_HANDLE_VALUE;
    }
}

static std::wstring BuildCommandLine(const std::wstring& program, const std::vector<std::wstring>& args) {
    std::wstring cmdLine;
    bool needQuote = program.find(L' ') != std::wstring::npos;
    if (needQuote) cmdLine += L'"';
    cmdLine += program;
    if (needQuote) cmdLine += L'"';

    for (const auto& arg : args) {
        cmdLine += L' ';
        needQuote = arg.find(L' ') != std::wstring::npos || arg.empty();
        if (needQuote) cmdLine += L'"';
        cmdLine += arg;
        if (needQuote) cmdLine += L'"';
    }
    return cmdLine;
}

static HWND FindConsoleWindowForProcess(DWORD processId, int maxAttempts = 50) {
    for (int i = 0; i < maxAttempts; i++) {
        HWND hwnd = nullptr;
        while ((hwnd = FindWindowExW(nullptr, hwnd, L"ConsoleWindowClass", nullptr)) != nullptr) {
            DWORD windowPid = 0;
            GetWindowThreadProcessId(hwnd, &windowPid);
            if (windowPid == processId) {
                return hwnd;
            }
        }
        Sleep(100);
    }
    return nullptr;
}

bool CreateLegacyProcess(
    const std::wstring& program,
    const std::vector<std::wstring>& args,
    bool hideWindow,
    LegacyConsoleHandle& handle
) {
    std::wstring cmdLine = BuildCommandLine(program, args);
    std::vector<wchar_t> cmdLineBuf(cmdLine.begin(), cmdLine.end());
    cmdLineBuf.push_back(L'\0');

    STARTUPINFOW si = {};
    si.cb = sizeof(si);

    if (hideWindow) {
        si.dwFlags |= STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
    }

    PROCESS_INFORMATION pi = {};

    // Create process with CREATE_NEW_CONSOLE so it gets its own console
    BOOL success = CreateProcessW(
        nullptr,
        cmdLineBuf.data(),
        nullptr,
        nullptr,
        FALSE,
        CREATE_NEW_CONSOLE,
        nullptr,
        nullptr,
        &si,
        &pi
    );

    if (!success) {
        fwprintf(stderr, L"Failed to create process: %lu\n", GetLastError());
        return false;
    }

    handle.hProcess = pi.hProcess;
    handle.hThread = pi.hThread;
    handle.running = true;

    // Wait for the process to create its console window
    handle.hConsoleWnd = FindConsoleWindowForProcess(pi.dwProcessId);
    if (!handle.hConsoleWnd) {
        fwprintf(stderr, L"Warning: Could not find console window for process\n");
    }

    return true;
}

struct ConsoleState {
    std::wstring lastContent;
    SHORT lastCursorY = 0;
};

static void ReadConsoleBuffer(HANDLE hConsole, ConsoleState& state, HANDLE hStdout) {
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (!GetConsoleScreenBufferInfo(hConsole, &csbi)) {
        return;
    }

    SHORT startY = state.lastCursorY;
    SHORT endY = csbi.dwCursorPosition.Y;
    SHORT width = csbi.dwSize.X;

    if (endY < startY) {
        // Console was cleared or scrolled, reset
        startY = 0;
    }

    if (endY == startY && state.lastCursorY > 0) {
        // No new lines
        return;
    }

    // Read new lines
    for (SHORT y = startY; y <= endY; y++) {
        std::vector<wchar_t> buffer(width + 1);
        COORD coord = { 0, y };
        DWORD charsRead = 0;

        if (!ReadConsoleOutputCharacterW(hConsole, buffer.data(), width, coord, &charsRead)) {
            continue;
        }

        buffer[charsRead] = L'\0';

        // Trim trailing spaces
        while (charsRead > 0 && buffer[charsRead - 1] == L' ') {
            buffer[--charsRead] = L'\0';
        }

        if (charsRead > 0 || y < endY) {
            // Convert to UTF-8 and write to stdout
            int utf8Len = WideCharToMultiByte(CP_UTF8, 0, buffer.data(), (int)charsRead, nullptr, 0, nullptr, nullptr);
            if (utf8Len > 0) {
                std::vector<char> utf8Buf(utf8Len);
                WideCharToMultiByte(CP_UTF8, 0, buffer.data(), (int)charsRead, utf8Buf.data(), utf8Len, nullptr, nullptr);
                DWORD written;
                WriteFile(hStdout, utf8Buf.data(), utf8Len, &written, nullptr);
            }
            if (y < endY) {
                DWORD written;
                WriteFile(hStdout, "\n", 1, &written, nullptr);
            }
        }
    }

    state.lastCursorY = endY;
}

static std::atomic<bool> g_LegacyRunning{true};

void RunLegacyLoop(LegacyConsoleHandle& handle) {
    // Save stdout handle before detaching - if we're running from a terminal,
    // the handle remains valid even after FreeConsole()
    // But if we're a GUI subsystem or stdin is redirected, we need to handle differently
    HANDLE hStdout = GetStdHandle(STD_OUTPUT_HANDLE);

    // Check if stdout is a valid handle (pipe or file, not console)
    DWORD stdoutType = GetFileType(hStdout);
    bool stdoutIsPipe = (stdoutType == FILE_TYPE_PIPE || stdoutType == FILE_TYPE_DISK);

    ConsoleState state;
    g_LegacyRunning = true;

    // Get process ID
    DWORD targetPid = GetProcessId(handle.hProcess);

    // We need to attach to the target's console to read from it
    // First, free our own console
    FreeConsole();

    // Attach to target's console
    if (!AttachConsole(targetPid)) {
        DWORD err = GetLastError();
        // Re-allocate our console to show error
        AllocConsole();
        fwprintf(stderr, L"Failed to attach to target console: %lu\n", err);
        return;
    }

    // Get handle to target's console output buffer
    HANDLE hTargetConsole = CreateFileW(
        L"CONOUT$",
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        0,
        nullptr
    );

    if (hTargetConsole == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        FreeConsole();
        AllocConsole();
        fwprintf(stderr, L"Failed to open target console buffer: %lu\n", err);
        return;
    }

    // If stdout was a console (not pipe/file), we need to write to a file or use a different method
    // For now, if stdout is not a pipe, we'll write to a temp file or just skip
    HANDLE hOutput = hStdout;
    if (!stdoutIsPipe) {
        // stdout was a console handle, it's now invalid
        // We can't output anywhere useful in this case
        // This is a limitation of legacy mode when not run with redirected output
        hOutput = INVALID_HANDLE_VALUE;
    }

    // Main loop - poll console buffer for changes
    while (handle.running) {
        DWORD exitCode;
        if (!GetExitCodeProcess(handle.hProcess, &exitCode) || exitCode != STILL_ACTIVE) {
            // Process exited, read any remaining output
            if (hOutput != INVALID_HANDLE_VALUE) {
                ReadConsoleBuffer(hTargetConsole, state, hOutput);
            }
            break;
        }

        if (hOutput != INVALID_HANDLE_VALUE) {
            ReadConsoleBuffer(hTargetConsole, state, hOutput);
        }
        Sleep(50); // Poll every 50ms
    }

    g_LegacyRunning = false;

    CloseHandle(hTargetConsole);

    // Detach and re-allocate our console
    FreeConsole();
    AllocConsole();

    if (!stdoutIsPipe) {
        fwprintf(stderr, L"Warning: Legacy mode output was not captured (stdout was not redirected)\n");
        fwprintf(stderr, L"Use --mode inject for better results, or redirect output to a file\n");
    }
}
