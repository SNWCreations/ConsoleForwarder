#include "conpty.h"
#include <thread>
#include <atomic>

typedef HRESULT (WINAPI *PFN_CreatePseudoConsole)(COORD, HANDLE, HANDLE, DWORD, HPCON*);
typedef void (WINAPI *PFN_ClosePseudoConsole)(HPCON);
typedef HRESULT (WINAPI *PFN_ResizePseudoConsole)(HPCON, COORD);

static PFN_CreatePseudoConsole pCreatePseudoConsole = nullptr;
static PFN_ClosePseudoConsole pClosePseudoConsole = nullptr;
static PFN_ResizePseudoConsole pResizePseudoConsole = nullptr;
static bool g_ConPTYChecked = false;
static bool g_ConPTYAvailable = false;

bool IsConPTYAvailable() {
    if (g_ConPTYChecked) {
        return g_ConPTYAvailable;
    }

    g_ConPTYChecked = true;
    HMODULE hKernel32 = GetModuleHandleW(L"kernel32.dll");
    if (!hKernel32) {
        return false;
    }

    pCreatePseudoConsole = (PFN_CreatePseudoConsole)GetProcAddress(hKernel32, "CreatePseudoConsole");
    pClosePseudoConsole = (PFN_ClosePseudoConsole)GetProcAddress(hKernel32, "ClosePseudoConsole");
    pResizePseudoConsole = (PFN_ResizePseudoConsole)GetProcAddress(hKernel32, "ResizePseudoConsole");

    g_ConPTYAvailable = (pCreatePseudoConsole && pClosePseudoConsole);
    return g_ConPTYAvailable;
}

void ConPTYHandle::Close() {
    if (hPC && pClosePseudoConsole) {
        pClosePseudoConsole(hPC);
        hPC = nullptr;
    }
    if (hPipeIn != INVALID_HANDLE_VALUE) {
        CloseHandle(hPipeIn);
        hPipeIn = INVALID_HANDLE_VALUE;
    }
    if (hPipeOut != INVALID_HANDLE_VALUE) {
        CloseHandle(hPipeOut);
        hPipeOut = INVALID_HANDLE_VALUE;
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

    // Quote program if needed
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

bool CreateConPTYProcess(
    const std::wstring& program,
    const std::vector<std::wstring>& args,
    bool hideWindow,
    ConPTYHandle& handle
) {
    if (!IsConPTYAvailable()) {
        fwprintf(stderr, L"ConPTY is not available on this system\n");
        return false;
    }

    // Create pipes for ConPTY
    HANDLE hPipeInRead = INVALID_HANDLE_VALUE;
    HANDLE hPipeOutWrite = INVALID_HANDLE_VALUE;

    if (!CreatePipe(&hPipeInRead, &handle.hPipeIn, nullptr, 0)) {
        fwprintf(stderr, L"Failed to create input pipe: %lu\n", GetLastError());
        return false;
    }

    if (!CreatePipe(&handle.hPipeOut, &hPipeOutWrite, nullptr, 0)) {
        CloseHandle(hPipeInRead);
        CloseHandle(handle.hPipeIn);
        fwprintf(stderr, L"Failed to create output pipe: %lu\n", GetLastError());
        return false;
    }

    // Create pseudo console
    COORD consoleSize = { 120, 30 };
    HRESULT hr = pCreatePseudoConsole(consoleSize, hPipeInRead, hPipeOutWrite, 0, &handle.hPC);

    // Close the handles we passed to CreatePseudoConsole
    CloseHandle(hPipeInRead);
    CloseHandle(hPipeOutWrite);

    if (FAILED(hr)) {
        fwprintf(stderr, L"Failed to create pseudo console: 0x%08X\n", hr);
        handle.Close();
        return false;
    }

    // Prepare startup info with pseudo console
    STARTUPINFOEXW siEx = {};
    siEx.StartupInfo.cb = sizeof(siEx);

    SIZE_T attrListSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrListSize);

    siEx.lpAttributeList = (LPPROC_THREAD_ATTRIBUTE_LIST)HeapAlloc(GetProcessHeap(), 0, attrListSize);
    if (!siEx.lpAttributeList) {
        fwprintf(stderr, L"Failed to allocate attribute list\n");
        handle.Close();
        return false;
    }

    if (!InitializeProcThreadAttributeList(siEx.lpAttributeList, 1, 0, &attrListSize)) {
        fwprintf(stderr, L"Failed to initialize attribute list: %lu\n", GetLastError());
        HeapFree(GetProcessHeap(), 0, siEx.lpAttributeList);
        handle.Close();
        return false;
    }

    if (!UpdateProcThreadAttribute(
        siEx.lpAttributeList,
        0,
        PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
        handle.hPC,
        sizeof(handle.hPC),
        nullptr,
        nullptr
    )) {
        fwprintf(stderr, L"Failed to update attribute list: %lu\n", GetLastError());
        DeleteProcThreadAttributeList(siEx.lpAttributeList);
        HeapFree(GetProcessHeap(), 0, siEx.lpAttributeList);
        handle.Close();
        return false;
    }

    if (hideWindow) {
        siEx.StartupInfo.dwFlags |= STARTF_USESHOWWINDOW;
        siEx.StartupInfo.wShowWindow = SW_HIDE;
    }

    // Build command line
    std::wstring cmdLine = BuildCommandLine(program, args);
    std::vector<wchar_t> cmdLineBuf(cmdLine.begin(), cmdLine.end());
    cmdLineBuf.push_back(L'\0');

    // Create process
    PROCESS_INFORMATION pi = {};
    BOOL success = CreateProcessW(
        nullptr,
        cmdLineBuf.data(),
        nullptr,
        nullptr,
        FALSE,
        EXTENDED_STARTUPINFO_PRESENT,
        nullptr,
        nullptr,
        &siEx.StartupInfo,
        &pi
    );

    DeleteProcThreadAttributeList(siEx.lpAttributeList);
    HeapFree(GetProcessHeap(), 0, siEx.lpAttributeList);

    if (!success) {
        fwprintf(stderr, L"Failed to create process: %lu\n", GetLastError());
        handle.Close();
        return false;
    }

    handle.hProcess = pi.hProcess;
    handle.hThread = pi.hThread;

    return true;
}

static std::atomic<bool> g_Running{true};
static HANDLE g_hPipeIn = INVALID_HANDLE_VALUE;
static HANDLE g_hProcess = nullptr;
static HPCON g_hPC = nullptr;

static BOOL WINAPI ConPTYCtrlHandler(DWORD ctrlType) {
    if (ctrlType == CTRL_C_EVENT || ctrlType == CTRL_BREAK_EVENT) {
        // Send Ctrl+C character to the pseudo console input
        if (g_hPipeIn != INVALID_HANDLE_VALUE) {
            char ctrlC = '\x03';
            DWORD written;
            WriteFile(g_hPipeIn, &ctrlC, 1, &written, nullptr);
        }
        return TRUE;  // Signal handled, don't terminate launcher
    }

    if (ctrlType == CTRL_CLOSE_EVENT || ctrlType == CTRL_LOGOFF_EVENT || ctrlType == CTRL_SHUTDOWN_EVENT) {
        // For close/logoff/shutdown, close the pseudo console to signal the child
        if (g_hPC && pClosePseudoConsole) {
            pClosePseudoConsole(g_hPC);
            g_hPC = nullptr;
        }
        // Wait for process to exit
        if (g_hProcess) {
            WaitForSingleObject(g_hProcess, 5000);
        }
        return TRUE;
    }

    return FALSE;
}

static void StdinReaderThread(HANDLE hPipeIn) {
    HANDLE hStdin = GetStdHandle(STD_INPUT_HANDLE);
    char buffer[4096];
    DWORD bytesRead;

    while (g_Running) {
        if (!ReadFile(hStdin, buffer, sizeof(buffer), &bytesRead, nullptr) || bytesRead == 0) {
            break;
        }
        DWORD bytesWritten;
        if (!WriteFile(hPipeIn, buffer, bytesRead, &bytesWritten, nullptr)) {
            break;
        }
    }
}

static bool ShouldEnableStdin(StdinMode mode) {
    if (mode == StdinMode::ForceOn) return true;
    if (mode == StdinMode::ForceOff) return false;

    // Auto mode: check if stdin is a terminal
    HANDLE hStdin = GetStdHandle(STD_INPUT_HANDLE);
    if (hStdin == INVALID_HANDLE_VALUE) return false;

    // Check if it's a console handle
    DWORD consoleMode;
    return GetConsoleMode(hStdin, &consoleMode) != 0;
}

void RunConPTYLoop(ConPTYHandle& handle, StdinMode stdinMode) {
    HANDLE hStdout = GetStdHandle(STD_OUTPUT_HANDLE);
    char buffer[4096];
    DWORD bytesRead;

    // Store handles for console control handler
    g_hPipeIn = handle.hPipeIn;
    g_hProcess = handle.hProcess;
    g_hPC = handle.hPC;

    // Register console control handler
    SetConsoleCtrlHandler(ConPTYCtrlHandler, TRUE);

    // Start stdin reader thread only if needed
    bool enableStdin = ShouldEnableStdin(stdinMode);
    std::thread stdinThread;
    if (enableStdin) {
        stdinThread = std::thread(StdinReaderThread, handle.hPipeIn);
    }

    // Read from pseudo console and write to stdout
    // Use overlapped I/O to allow checking if process has exited
    OVERLAPPED ov = {};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    while (true) {
        // Check if process has exited
        DWORD waitResult = WaitForSingleObject(handle.hProcess, 0);
        if (waitResult == WAIT_OBJECT_0) {
            // Process exited, drain remaining output then exit
            DWORD available = 0;
            while (PeekNamedPipe(handle.hPipeOut, nullptr, 0, nullptr, &available, nullptr) && available > 0) {
                if (ReadFile(handle.hPipeOut, buffer, min((DWORD)sizeof(buffer), available), &bytesRead, nullptr) && bytesRead > 0) {
                    DWORD bytesWritten;
                    WriteFile(hStdout, buffer, bytesRead, &bytesWritten, nullptr);
                } else {
                    break;
                }
            }
            break;
        }

        // Try to read with a short timeout by peeking first
        DWORD available = 0;
        if (!PeekNamedPipe(handle.hPipeOut, nullptr, 0, nullptr, &available, nullptr)) {
            break;
        }

        if (available > 0) {
            if (!ReadFile(handle.hPipeOut, buffer, min((DWORD)sizeof(buffer), available), &bytesRead, nullptr) || bytesRead == 0) {
                break;
            }
            DWORD bytesWritten;
            WriteFile(hStdout, buffer, bytesRead, &bytesWritten, nullptr);
        } else {
            // No data available, sleep briefly to avoid busy loop
            Sleep(10);
        }
    }

    CloseHandle(ov.hEvent);

    g_Running = false;

    // Close the pseudo console to terminate conhost.exe
    if (handle.hPC && pClosePseudoConsole) {
        pClosePseudoConsole(handle.hPC);
        handle.hPC = nullptr;
        g_hPC = nullptr;
    }

    // Close the input pipe to unblock stdin thread's WriteFile
    if (handle.hPipeIn != INVALID_HANDLE_VALUE) {
        CloseHandle(handle.hPipeIn);
        handle.hPipeIn = INVALID_HANDLE_VALUE;
        g_hPipeIn = INVALID_HANDLE_VALUE;
    }

    // Cancel stdin read if blocked and join thread
    if (enableStdin) {
        CancelIoEx(GetStdHandle(STD_INPUT_HANDLE), nullptr);
        if (stdinThread.joinable()) {
            stdinThread.join();
        }
    }

    // Wait for process to exit
    WaitForSingleObject(handle.hProcess, INFINITE);

    // Cleanup
    SetConsoleCtrlHandler(ConPTYCtrlHandler, FALSE);
    g_hPipeIn = INVALID_HANDLE_VALUE;
    g_hProcess = nullptr;
    g_hPC = nullptr;
}
