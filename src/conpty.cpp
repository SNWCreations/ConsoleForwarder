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

void RunConPTYLoop(ConPTYHandle& handle) {
    HANDLE hStdout = GetStdHandle(STD_OUTPUT_HANDLE);
    char buffer[4096];
    DWORD bytesRead;

    // Start stdin reader thread
    std::thread stdinThread(StdinReaderThread, handle.hPipeIn);

    // Read from pseudo console and write to stdout
    while (true) {
        if (!ReadFile(handle.hPipeOut, buffer, sizeof(buffer), &bytesRead, nullptr) || bytesRead == 0) {
            break;
        }
        DWORD bytesWritten;
        WriteFile(hStdout, buffer, bytesRead, &bytesWritten, nullptr);
    }

    g_Running = false;

    // Cancel stdin read if blocked
    CancelIoEx(GetStdHandle(STD_INPUT_HANDLE), nullptr);

    if (stdinThread.joinable()) {
        stdinThread.join();
    }

    // Wait for process to exit
    WaitForSingleObject(handle.hProcess, INFINITE);
}
