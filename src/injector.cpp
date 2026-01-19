#include "injector.h"
#include <thread>
#include <atomic>

static HANDLE g_hTargetProcess = nullptr;
static DWORD g_dwTargetPID = 0;
static std::atomic<bool> g_InjectedRunning{true};
static HANDLE g_hPipeIn = INVALID_HANDLE_VALUE;  // For sending commands to target
static bool g_isSourceEngine = false;  // Whether target is Source Engine (srcds)

static void DebugLog(const char* msg) {
    OutputDebugStringA("[ConsoleForwarder] ");
    OutputDebugStringA(msg);
}

static void DebugLogW(const wchar_t* msg) {
    OutputDebugStringA("[ConsoleForwarder] ");
    OutputDebugStringW(msg);
}

// Console control handler to gracefully terminate target process
static BOOL WINAPI ConsoleCtrlHandler(DWORD ctrlType) {
    if (ctrlType == CTRL_C_EVENT || ctrlType == CTRL_BREAK_EVENT ||
        ctrlType == CTRL_CLOSE_EVENT || ctrlType == CTRL_LOGOFF_EVENT ||
        ctrlType == CTRL_SHUTDOWN_EVENT) {

        DebugLog("ConsoleCtrlHandler: Received termination signal\n");

        // For Source Engine, send "quit" command through stdin pipe
        if (g_isSourceEngine && g_hPipeIn != INVALID_HANDLE_VALUE) {
            DebugLog("ConsoleCtrlHandler: Source Engine detected, sending quit command\n");
            const char* quitCmd = "quit\n";
            DWORD written;
            WriteFile(g_hPipeIn, quitCmd, (DWORD)strlen(quitCmd), &written, nullptr);
        } else if (g_dwTargetPID != 0) {
            // For other programs, try to close console window gracefully
            FreeConsole();
            if (AttachConsole(g_dwTargetPID)) {
                HWND hConsoleWnd = GetConsoleWindow();
                FreeConsole();

                if (hConsoleWnd) {
                    DebugLog("ConsoleCtrlHandler: Sending WM_CLOSE to target console window\n");
                    PostMessageW(hConsoleWnd, WM_CLOSE, 0, 0);
                }
            } else {
                DebugLog("ConsoleCtrlHandler: Failed to attach to target console\n");
            }
        }

        // Signal the main loop to stop
        g_InjectedRunning = false;

        // Wait for target to exit
        if (g_hTargetProcess) {
            DebugLog("ConsoleCtrlHandler: Waiting for target to exit\n");
            WaitForSingleObject(g_hTargetProcess, INFINITE);
            DebugLog("ConsoleCtrlHandler: Target exited\n");
        }

        return TRUE;
    }
    return FALSE;
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

bool InjectDLL(HANDLE hProcess, const std::wstring& dllPath) {
    DebugLog("InjectDLL: Starting injection\n");
    DebugLogW(dllPath.c_str());
    DebugLog("\n");

    // Allocate memory in target process for DLL path
    SIZE_T pathSize = (dllPath.length() + 1) * sizeof(wchar_t);
    LPVOID remotePath = VirtualAllocEx(hProcess, nullptr, pathSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remotePath) {
        DebugLog("InjectDLL: VirtualAllocEx failed\n");
        fwprintf(stderr, L"Failed to allocate memory in target process: %lu\n", GetLastError());
        return false;
    }
    DebugLog("InjectDLL: Memory allocated in target process\n");

    // Write DLL path to target process
    if (!WriteProcessMemory(hProcess, remotePath, dllPath.c_str(), pathSize, nullptr)) {
        DebugLog("InjectDLL: WriteProcessMemory failed\n");
        fwprintf(stderr, L"Failed to write DLL path to target process: %lu\n", GetLastError());
        VirtualFreeEx(hProcess, remotePath, 0, MEM_RELEASE);
        return false;
    }
    DebugLog("InjectDLL: DLL path written to target process\n");

    // Get LoadLibraryW address
    HMODULE hKernel32 = GetModuleHandleW(L"kernel32.dll");
    LPTHREAD_START_ROUTINE loadLibraryAddr = (LPTHREAD_START_ROUTINE)GetProcAddress(hKernel32, "LoadLibraryW");
    if (!loadLibraryAddr) {
        DebugLog("InjectDLL: GetProcAddress(LoadLibraryW) failed\n");
        fwprintf(stderr, L"Failed to get LoadLibraryW address\n");
        VirtualFreeEx(hProcess, remotePath, 0, MEM_RELEASE);
        return false;
    }
    DebugLog("InjectDLL: Got LoadLibraryW address\n");

    // Create remote thread to load DLL
    HANDLE hRemoteThread = CreateRemoteThread(hProcess, nullptr, 0, loadLibraryAddr, remotePath, 0, nullptr);
    if (!hRemoteThread) {
        DebugLog("InjectDLL: CreateRemoteThread failed\n");
        fwprintf(stderr, L"Failed to create remote thread: %lu\n", GetLastError());
        VirtualFreeEx(hProcess, remotePath, 0, MEM_RELEASE);
        return false;
    }
    DebugLog("InjectDLL: Remote thread created, waiting for DLL load\n");

    // Wait for DLL to load
    WaitForSingleObject(hRemoteThread, INFINITE);

    DWORD exitCode = 0;
    GetExitCodeThread(hRemoteThread, &exitCode);
    CloseHandle(hRemoteThread);

    // Free the allocated memory
    VirtualFreeEx(hProcess, remotePath, 0, MEM_RELEASE);

    if (exitCode == 0) {
        DebugLog("InjectDLL: LoadLibraryW returned NULL - injection failed\n");
        fwprintf(stderr, L"DLL injection failed - LoadLibraryW returned NULL\n");
        return false;
    }

    DebugLog("InjectDLL: DLL loaded successfully\n");
    return true;
}

bool CreateInjectedProcess(
    const std::wstring& program,
    const std::vector<std::wstring>& args,
    bool hideWindow,
    const std::wstring& dllPath,
    HANDLE& hProcess,
    HANDLE& hThread
) {
    DebugLog("CreateInjectedProcess: Starting\n");
    DebugLog("Program: ");
    DebugLogW(program.c_str());
    DebugLog("\n");

    std::wstring cmdLine = BuildCommandLine(program, args);
    std::vector<wchar_t> cmdLineBuf(cmdLine.begin(), cmdLine.end());
    cmdLineBuf.push_back(L'\0');

    DebugLog("CommandLine: ");
    DebugLogW(cmdLine.c_str());
    DebugLog("\n");

    STARTUPINFOW si = {};
    si.cb = sizeof(si);

    if (hideWindow) {
        si.dwFlags |= STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
    }

    PROCESS_INFORMATION pi = {};

    // Create process suspended so we can inject before it runs
    DebugLog("CreateInjectedProcess: Creating suspended process\n");
    BOOL success = CreateProcessW(
        nullptr,
        cmdLineBuf.data(),
        nullptr,
        nullptr,
        FALSE,
        CREATE_NEW_CONSOLE | CREATE_SUSPENDED,
        nullptr,
        nullptr,
        &si,
        &pi
    );

    if (!success) {
        DebugLog("CreateInjectedProcess: CreateProcessW failed\n");
        fwprintf(stderr, L"Failed to create process: %lu\n", GetLastError());
        return false;
    }

    char pidBuf[64];
    sprintf_s(pidBuf, "CreateInjectedProcess: Process created, PID=%lu\n", pi.dwProcessId);
    DebugLog(pidBuf);

    // Inject DLL
    if (!InjectDLL(pi.hProcess, dllPath)) {
        DebugLog("CreateInjectedProcess: Injection failed, terminating process\n");
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return false;
    }

    // Resume the process
    DebugLog("CreateInjectedProcess: Resuming process\n");
    ResumeThread(pi.hThread);

    hProcess = pi.hProcess;
    hThread = pi.hThread;

    DebugLog("CreateInjectedProcess: Success\n");
    return true;
}

static void StdinForwardThread(HANDLE hPipe) {
    DebugLog("StdinForwardThread: Starting\n");
    HANDLE hStdin = GetStdHandle(STD_INPUT_HANDLE);
    char buffer[4096];
    DWORD bytesRead;

    while (g_InjectedRunning) {
        if (!ReadFile(hStdin, buffer, sizeof(buffer), &bytesRead, nullptr) || bytesRead == 0) {
            char debugBuf[64];
            sprintf_s(debugBuf, "StdinForwardThread: ReadFile ended, error=%lu\n", GetLastError());
            DebugLog(debugBuf);
            break;
        }
        char debugBuf[128];
        sprintf_s(debugBuf, "StdinForwardThread: Read %lu bytes from stdin, forwarding to pipe\n", bytesRead);
        DebugLog(debugBuf);

        DWORD bytesWritten;
        if (!WriteFile(hPipe, buffer, bytesRead, &bytesWritten, nullptr)) {
            sprintf_s(debugBuf, "StdinForwardThread: WriteFile to pipe failed, error=%lu\n", GetLastError());
            DebugLog(debugBuf);
            break;
        }
        sprintf_s(debugBuf, "StdinForwardThread: Wrote %lu bytes to pipe\n", bytesWritten);
        DebugLog(debugBuf);
    }
    DebugLog("StdinForwardThread: Exiting\n");
}

void RunInjectedLoop(HANDLE hProcess, const std::wstring& pipeName, const std::wstring& program) {
    g_InjectedRunning = true;

    // Store process info for console control handler
    g_hTargetProcess = hProcess;
    g_dwTargetPID = GetProcessId(hProcess);

    // Detect Source Engine (srcds.exe, hl2.exe, etc.)
    std::wstring programLower = program;
    for (auto& c : programLower) c = towlower(c);
    size_t lastSlash = programLower.find_last_of(L"\\/");
    std::wstring exeName = (lastSlash != std::wstring::npos) ? programLower.substr(lastSlash + 1) : programLower;
    g_isSourceEngine = (exeName == L"srcds.exe" || exeName == L"hl2.exe" ||
                        exeName == L"csgo.exe" || exeName == L"left4dead2.exe" ||
                        exeName == L"portal2.exe");
    if (g_isSourceEngine) {
        DebugLog("RunInjectedLoop: Source Engine detected\n");
    }

    // Register console control handler for graceful shutdown
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

    // Connect to the named pipes created by the injected DLL
    // Output pipe: hook writes, launcher reads
    // Input pipe: launcher writes, hook reads
    std::wstring pipeNameOut = L"\\\\.\\pipe\\" + pipeName + L"_out";
    std::wstring pipeNameIn = L"\\\\.\\pipe\\" + pipeName + L"_in";

    DebugLog("RunInjectedLoop: Connecting to pipes\n");

    HANDLE hPipeOut = INVALID_HANDLE_VALUE;
    HANDLE hPipeIn = INVALID_HANDLE_VALUE;

    for (int i = 0; i < 50; i++) { // Try for 5 seconds
        if (hPipeOut == INVALID_HANDLE_VALUE) {
            hPipeOut = CreateFileW(
                pipeNameOut.c_str(),
                GENERIC_READ,
                0,
                nullptr,
                OPEN_EXISTING,
                0,
                nullptr
            );
            if (hPipeOut != INVALID_HANDLE_VALUE) {
                DebugLog("RunInjectedLoop: Output pipe connected\n");
            }
        }

        if (hPipeIn == INVALID_HANDLE_VALUE) {
            hPipeIn = CreateFileW(
                pipeNameIn.c_str(),
                GENERIC_WRITE,
                0,
                nullptr,
                OPEN_EXISTING,
                0,
                nullptr
            );
            if (hPipeIn != INVALID_HANDLE_VALUE) {
                DebugLog("RunInjectedLoop: Input pipe connected\n");
            }
        }

        if (hPipeOut != INVALID_HANDLE_VALUE && hPipeIn != INVALID_HANDLE_VALUE) {
            break;
        }

        Sleep(100);
    }

    if (hPipeOut == INVALID_HANDLE_VALUE || hPipeIn == INVALID_HANDLE_VALUE) {
        DebugLog("RunInjectedLoop: Failed to connect to pipes, waiting for process\n");
        fwprintf(stderr, L"Failed to connect to injected DLL pipes\n");
        if (hPipeOut != INVALID_HANDLE_VALUE) CloseHandle(hPipeOut);
        if (hPipeIn != INVALID_HANDLE_VALUE) CloseHandle(hPipeIn);
        WaitForSingleObject(hProcess, INFINITE);
        return;
    }

    // Store input pipe handle for console control handler (to send quit command)
    g_hPipeIn = hPipeIn;

    DebugLog("RunInjectedLoop: Starting read loop\n");

    HANDLE hStdout = GetStdHandle(STD_OUTPUT_HANDLE);
    HANDLE hStderr = GetStdHandle(STD_ERROR_HANDLE);

    // Start stdin forwarding thread (writes to input pipe)
    std::thread stdinThread(StdinForwardThread, hPipeIn);

    // Read from output pipe and write to stdout/stderr based on message type
    // Protocol: [type:1byte][length:4bytes][data:length bytes]
    char buffer[4096];

    while (true) {
        // Read message type
        char msgType;
        DWORD bytesRead;
        if (!ReadFile(hPipeOut, &msgType, 1, &bytesRead, nullptr) || bytesRead == 0) {
            char errBuf[64];
            sprintf_s(errBuf, "RunInjectedLoop: ReadFile (type) ended, error=%lu\n", GetLastError());
            DebugLog(errBuf);
            break;
        }

        // Read message length
        DWORD msgLen;
        if (!ReadFile(hPipeOut, &msgLen, sizeof(DWORD), &bytesRead, nullptr) || bytesRead != sizeof(DWORD)) {
            DebugLog("RunInjectedLoop: ReadFile (length) failed\n");
            break;
        }

        // Read message data
        DWORD totalRead = 0;
        while (totalRead < msgLen) {
            DWORD toRead = min(msgLen - totalRead, (DWORD)sizeof(buffer));
            if (!ReadFile(hPipeOut, buffer, toRead, &bytesRead, nullptr) || bytesRead == 0) {
                DebugLog("RunInjectedLoop: ReadFile (data) failed\n");
                break;
            }

            // Write to appropriate handle
            HANDLE hOutput = (msgType == 0x02) ? hStderr : hStdout;
            DWORD bytesWritten;
            WriteFile(hOutput, buffer, bytesRead, &bytesWritten, nullptr);
            totalRead += bytesRead;
        }

        if (totalRead < msgLen) break;
    }

    DebugLog("RunInjectedLoop: Loop ended, cleaning up\n");

    g_InjectedRunning = false;
    CloseHandle(hPipeOut);
    CloseHandle(hPipeIn);

    CancelIoEx(GetStdHandle(STD_INPUT_HANDLE), nullptr);
    if (stdinThread.joinable()) {
        stdinThread.join();
    }

    DebugLog("RunInjectedLoop: Waiting for process to exit\n");
    WaitForSingleObject(hProcess, INFINITE);
    DebugLog("RunInjectedLoop: Process exited\n");

    // Cleanup
    SetConsoleCtrlHandler(ConsoleCtrlHandler, FALSE);
    g_hTargetProcess = nullptr;
    g_dwTargetPID = 0;
    g_hPipeIn = INVALID_HANDLE_VALUE;
    g_isSourceEngine = false;
}
