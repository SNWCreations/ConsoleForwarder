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

static void StdinForwardThread(DWORD targetPid) {
    // Attach to target process's console to send input
    HANDLE hStdin = GetStdHandle(STD_INPUT_HANDLE);
    char buffer[256];
    DWORD bytesRead;

    while (g_LegacyRunning) {
        if (!ReadFile(hStdin, buffer, sizeof(buffer), &bytesRead, nullptr) || bytesRead == 0) {
            break;
        }

        // We need to attach to the target console to send input
        // This is complex - for now we'll skip stdin forwarding in legacy mode
        // The inject mode handles this better
    }
}

void RunLegacyLoop(LegacyConsoleHandle& handle) {
    HANDLE hStdout = GetStdHandle(STD_OUTPUT_HANDLE);
    ConsoleState state;
    g_LegacyRunning = true;

    // Get process ID
    DWORD targetPid = GetProcessId(handle.hProcess);

    // We need to attach to the target's console to read from it
    // First, free our own console
    FreeConsole();

    // Attach to target's console
    if (!AttachConsole(targetPid)) {
        fwprintf(stderr, L"Failed to attach to target console: %lu\n", GetLastError());
        // Re-allocate our console
        AllocConsole();
        return;
    }

    HANDLE hTargetConsole = GetStdHandle(STD_OUTPUT_HANDLE);

    // Main loop - poll console buffer for changes
    while (handle.running) {
        DWORD exitCode;
        if (!GetExitCodeProcess(handle.hProcess, &exitCode) || exitCode != STILL_ACTIVE) {
            // Process exited, read any remaining output
            ReadConsoleBuffer(hTargetConsole, state, hStdout);
            break;
        }

        ReadConsoleBuffer(hTargetConsole, state, hStdout);
        Sleep(50); // Poll every 50ms
    }

    g_LegacyRunning = false;

    // Detach and re-allocate our console
    FreeConsole();
    AllocConsole();
}
