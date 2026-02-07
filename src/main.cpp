#include <windows.h>
#include <cstdio>
#include <string>

#include "args.h"
#include "conpty.h"
#include "legacy_console.h"
#include "injector.h"

static std::wstring GetExecutableDirectory() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring pathStr(path);
    size_t pos = pathStr.find_last_of(L"\\/");
    if (pos != std::wstring::npos) {
        return pathStr.substr(0, pos);
    }
    return L".";
}

static CaptureMode SelectBestMode(CaptureMode requested) {
    if (requested != CaptureMode::Auto) {
        return requested;
    }

    // Auto-select: prefer ConPTY if available, otherwise use inject
    if (IsConPTYAvailable()) {
        return CaptureMode::ConPTY;
    }

    // Fall back to inject mode as it's more reliable than legacy
    return CaptureMode::Inject;
}

static void EnableVirtualTerminalProcessing() {
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut == INVALID_HANDLE_VALUE) return;

    DWORD mode = 0;
    if (!GetConsoleMode(hOut, &mode)) return;

    mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    SetConsoleMode(hOut, mode);
}

int wmain(int argc, wchar_t* argv[]) {
    // Set console to UTF-8
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    // Enable ANSI escape sequence processing
    EnableVirtualTerminalProcessing();

    LaunchOptions options;
    if (!ParseArguments(argc, argv, options)) {
        return 1;
    }

    if (options.showHelp) {
        PrintUsage(argv[0]);
        return 0;
    }

    CaptureMode mode = SelectBestMode(options.mode);

    switch (mode) {
    case CaptureMode::ConPTY: {
        fwprintf(stderr, L"Using ConPTY mode\n");
        ConPTYHandle handle;
        if (!CreateConPTYProcess(options.program, options.args, options.hideWindow, handle)) {
            return 1;
        }
        RunConPTYLoop(handle, options.stdinMode);

        DWORD exitCode = 0;
        GetExitCodeProcess(handle.hProcess, &exitCode);
        handle.Close();
        return (int)exitCode;
    }

    case CaptureMode::Legacy: {
        fwprintf(stderr, L"Using Legacy console buffer mode\n");
        LegacyConsoleHandle handle;
        if (!CreateLegacyProcess(options.program, options.args, options.hideWindow, handle)) {
            return 1;
        }
        RunLegacyLoop(handle);

        DWORD exitCode = 0;
        GetExitCodeProcess(handle.hProcess, &exitCode);
        handle.Close();
        return (int)exitCode;
    }

    case CaptureMode::Inject: {
        fwprintf(stderr, L"Using DLL injection mode\n");

        std::wstring exeDir = GetExecutableDirectory();
        std::wstring dllPath = exeDir + L"\\ConsoleHook.dll";

        // Check if DLL exists
        DWORD attrs = GetFileAttributesW(dllPath.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES) {
            fwprintf(stderr, L"Error: ConsoleHook.dll not found at %s\n", dllPath.c_str());
            return 1;
        }

        HANDLE hProcess, hThread;
        if (!CreateInjectedProcess(options.program, options.args, options.hideWindow, dllPath, hProcess, hThread)) {
            return 1;
        }

        // Pipe name matches what the DLL creates
        wchar_t pipeName[64];
        swprintf_s(pipeName, L"ConsoleForwarder_%lu", GetProcessId(hProcess));

        RunInjectedLoop(hProcess, pipeName, options.program, options.hideWindow);

        DWORD exitCode = 0;
        GetExitCodeProcess(hProcess, &exitCode);
        CloseHandle(hThread);
        CloseHandle(hProcess);
        return (int)exitCode;
    }

    default:
        fwprintf(stderr, L"Error: Unknown capture mode\n");
        return 1;
    }
}
