#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include <string>
#include <mutex>

// Pipe for communication with parent process
static HANDLE g_hPipeOut = INVALID_HANDLE_VALUE;  // Hook writes, launcher reads
static HANDLE g_hPipeIn = INVALID_HANDLE_VALUE;   // Launcher writes, hook reads
static std::mutex g_pipeMutex;
static bool g_initialized = false;
static bool g_pipeConnected = false;
static HANDLE g_hConnectThread = nullptr;
static HANDLE g_hStdinThread = nullptr;
static bool g_stdinRunning = false;

// Original function pointers
static decltype(&WriteConsoleA) g_OriginalWriteConsoleA = nullptr;
static decltype(&WriteConsoleW) g_OriginalWriteConsoleW = nullptr;
static decltype(&WriteFile) g_OriginalWriteFile = nullptr;
static decltype(&WriteConsoleOutputCharacterA) g_OriginalWriteConsoleOutputCharacterA = nullptr;
static decltype(&WriteConsoleOutputCharacterW) g_OriginalWriteConsoleOutputCharacterW = nullptr;
static decltype(&WriteConsoleOutputAttribute) g_OriginalWriteConsoleOutputAttribute = nullptr;
static decltype(&LoadLibraryA) g_OriginalLoadLibraryA = nullptr;
static decltype(&LoadLibraryW) g_OriginalLoadLibraryW = nullptr;
static decltype(&LoadLibraryExA) g_OriginalLoadLibraryExA = nullptr;
static decltype(&LoadLibraryExW) g_OriginalLoadLibraryExW = nullptr;
static decltype(&SetConsoleTitleA) g_OriginalSetConsoleTitleA = nullptr;
static decltype(&SetConsoleTitleW) g_OriginalSetConsoleTitleW = nullptr;

// Status line attribute (captured from WriteConsoleOutputAttribute)
static WORD g_statusLineAttrib = FOREGROUND_GREEN | FOREGROUND_INTENSITY | BACKGROUND_INTENSITY;

// Console handles to intercept
static HANDLE g_hStdout = INVALID_HANDLE_VALUE;
static HANDLE g_hStderr = INVALID_HANDLE_VALUE;

// Message types for pipe protocol
static const char MSG_STDOUT = 0x01;
static const char MSG_STDERR = 0x02;
static const char MSG_STATUSLINE = 0x03;
static const char MSG_STATUSLINE_ATTR = 0x04;
static const char MSG_TITLE = 0x05;
static const char MSG_ICON = 0x06;

// Console window handle for icon interception
static HWND g_hTargetConsoleWnd = nullptr;

// Original SendMessageW function pointer
static decltype(&SendMessageW) g_OriginalSendMessageW = nullptr;

static void SendToPipeWithType(const char* data, DWORD length, char msgType) {
    if (g_hPipeOut == INVALID_HANDLE_VALUE || !g_pipeConnected || length == 0) return;

    std::lock_guard<std::mutex> lock(g_pipeMutex);
    DWORD written;
    // Write type byte first, then length (4 bytes), then data
    WriteFile(g_hPipeOut, &msgType, 1, &written, nullptr);
    WriteFile(g_hPipeOut, &length, sizeof(DWORD), &written, nullptr);
    WriteFile(g_hPipeOut, data, length, &written, nullptr);
}

static void SendToPipeWithTypeW(const wchar_t* data, DWORD length, char msgType) {
    if (g_hPipeOut == INVALID_HANDLE_VALUE || !g_pipeConnected || length == 0) return;

    // Convert to UTF-8
    int utf8Len = WideCharToMultiByte(CP_UTF8, 0, data, length, nullptr, 0, nullptr, nullptr);
    if (utf8Len > 0) {
        std::string utf8Buf(utf8Len, '\0');
        WideCharToMultiByte(CP_UTF8, 0, data, length, &utf8Buf[0], utf8Len, nullptr, nullptr);
        SendToPipeWithType(utf8Buf.data(), utf8Len, msgType);
    }
}

// Serialize HICON to byte buffer and send to pipe for cross-process icon transfer.
//
// Icon Serialization Format (all integers are 4 bytes, little-endian):
// +--------+--------+-----+----------+----------+-----------+----------+------------+-----------+
// | width  | height | bpp | xHotspot | yHotspot | colorSize | maskSize | colorBits  | maskBits  |
// | (4B)   | (4B)   | (4B)| (4B)     | (4B)     | (4B)      | (4B)     | (variable) | (variable)|
// +--------+--------+-----+----------+----------+-----------+----------+------------+-----------+
//
// - width/height: Icon dimensions in pixels
// - bpp: Bits per pixel of the color bitmap (e.g., 32 for ARGB)
// - xHotspot/yHotspot: Cursor hotspot (unused for icons, but preserved)
// - colorSize: Size of color bitmap data in bytes (0 if no color bitmap)
// - maskSize: Size of mask bitmap data in bytes (0 if no mask bitmap)
// - colorBits: Raw pixel data of the color bitmap (device-dependent format)
// - maskBits: Raw pixel data of the AND mask bitmap (1bpp monochrome)
//
// The receiver uses CreateDIBSection + CreateBitmap + CreateIconIndirect to reconstruct the icon.
static void SendIconToPipe(HICON hIcon) {
    if (!hIcon || hIcon == (HICON)1) return;
    if (g_hPipeOut == INVALID_HANDLE_VALUE || !g_pipeConnected) return;

    ICONINFO iconInfo;
    if (!GetIconInfo(hIcon, &iconInfo)) return;

    BITMAP bmpColor = {};
    BITMAP bmpMask = {};
    bool hasColor = iconInfo.hbmColor && GetObject(iconInfo.hbmColor, sizeof(BITMAP), &bmpColor);
    bool hasMask = iconInfo.hbmMask && GetObject(iconInfo.hbmMask, sizeof(BITMAP), &bmpMask);

    if (!hasColor && !hasMask) {
        if (iconInfo.hbmColor) DeleteObject(iconInfo.hbmColor);
        if (iconInfo.hbmMask) DeleteObject(iconInfo.hbmMask);
        return;
    }

    // Use color bitmap dimensions, or mask if no color
    int width = hasColor ? bmpColor.bmWidth : bmpMask.bmWidth;
    int height = hasColor ? bmpColor.bmHeight : bmpMask.bmHeight;
    int bpp = hasColor ? bmpColor.bmBitsPixel : 1;

    // Calculate bitmap data sizes
    int colorStride = hasColor ? ((bmpColor.bmWidth * bmpColor.bmBitsPixel + 31) / 32) * 4 : 0;
    int colorSize = hasColor ? colorStride * bmpColor.bmHeight : 0;
    int maskStride = hasMask ? ((bmpMask.bmWidth + 31) / 32) * 4 : 0;
    int maskSize = hasMask ? maskStride * bmpMask.bmHeight : 0;

    // Header: 7 ints (28 bytes) + colorBits + maskBits
    int headerSize = 7 * sizeof(int);
    int totalSize = headerSize + colorSize + maskSize;

    char* buffer = new char[totalSize];
    int* header = reinterpret_cast<int*>(buffer);
    header[0] = width;
    header[1] = height;
    header[2] = bpp;
    header[3] = (int)iconInfo.xHotspot;
    header[4] = (int)iconInfo.yHotspot;
    header[5] = colorSize;
    header[6] = maskSize;

    // Get bitmap bits
    if (hasColor && colorSize > 0) {
        GetBitmapBits(iconInfo.hbmColor, colorSize, buffer + headerSize);
    }
    if (hasMask && maskSize > 0) {
        GetBitmapBits(iconInfo.hbmMask, maskSize, buffer + headerSize + colorSize);
    }

    SendToPipeWithType(buffer, totalSize, MSG_ICON);

    delete[] buffer;
    if (iconInfo.hbmColor) DeleteObject(iconInfo.hbmColor);
    if (iconInfo.hbmMask) DeleteObject(iconInfo.hbmMask);
}

// Hooked SendMessageW - intercept WM_SETICON sent to console window
static LRESULT WINAPI HookedSendMessageW(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam) {
    // Capture WM_SETICON sent to the console window
    if (Msg == WM_SETICON && hWnd == g_hTargetConsoleWnd && wParam == ICON_BIG) {
        HICON hIcon = (HICON)lParam;
        SendIconToPipe(hIcon);
    }

    // Call original
    if (g_OriginalSendMessageW) {
        return g_OriginalSendMessageW(hWnd, Msg, wParam, lParam);
    }
    static auto pFunc = (decltype(&SendMessageW))GetProcAddress(GetModuleHandleW(L"user32.dll"), "SendMessageW");
    if (pFunc) {
        return pFunc(hWnd, Msg, wParam, lParam);
    }
    return 0;
}

// Hooked WriteConsoleA
static BOOL WINAPI HookedWriteConsoleA(
    HANDLE hConsoleOutput,
    const VOID* lpBuffer,
    DWORD nNumberOfCharsToWrite,
    LPDWORD lpNumberOfCharsWritten,
    LPVOID lpReserved
) {
    // Forward to pipe with type
    char msgType = (hConsoleOutput == g_hStderr) ? MSG_STDERR : MSG_STDOUT;
    SendToPipeWithType((const char*)lpBuffer, nNumberOfCharsToWrite, msgType);

    // Call original
    if (g_OriginalWriteConsoleA) {
        return g_OriginalWriteConsoleA(hConsoleOutput, lpBuffer, nNumberOfCharsToWrite, lpNumberOfCharsWritten, lpReserved);
    }
    if (lpNumberOfCharsWritten) *lpNumberOfCharsWritten = nNumberOfCharsToWrite;
    return TRUE;
}

// Hooked WriteConsoleW
static BOOL WINAPI HookedWriteConsoleW(
    HANDLE hConsoleOutput,
    const VOID* lpBuffer,
    DWORD nNumberOfCharsToWrite,
    LPDWORD lpNumberOfCharsWritten,
    LPVOID lpReserved
) {
    // Forward to pipe with type
    char msgType = (hConsoleOutput == g_hStderr) ? MSG_STDERR : MSG_STDOUT;
    SendToPipeWithTypeW((const wchar_t*)lpBuffer, nNumberOfCharsToWrite, msgType);

    // Call original
    if (g_OriginalWriteConsoleW) {
        return g_OriginalWriteConsoleW(hConsoleOutput, lpBuffer, nNumberOfCharsToWrite, lpNumberOfCharsWritten, lpReserved);
    }
    if (lpNumberOfCharsWritten) *lpNumberOfCharsWritten = nNumberOfCharsToWrite;
    return TRUE;
}

// Hooked WriteFile - intercept console writes
static BOOL WINAPI HookedWriteFile(
    HANDLE hFile,
    LPCVOID lpBuffer,
    DWORD nNumberOfBytesToWrite,
    LPDWORD lpNumberOfBytesWritten,
    LPOVERLAPPED lpOverlapped
) {
    // Check if writing to a console handle by checking the handle type
    DWORD handleType = GetFileType(hFile);

    if (handleType == FILE_TYPE_CHAR) {
        // It's a character device (console), check if it's a console output
        DWORD mode;
        if (GetConsoleMode(hFile, &mode)) {
            // It's a console handle - forward to pipe
            char debugBuf[256];
            sprintf_s(debugBuf, "[ConsoleHook] WriteFile: console handle=%p, bytes=%lu, pipeConnected=%d\n",
                hFile, nNumberOfBytesToWrite, g_pipeConnected ? 1 : 0);
            OutputDebugStringA(debugBuf);

            // Determine if it's stdout or stderr by comparing with current handles
            HANDLE hCurrentStderr = GetStdHandle(STD_ERROR_HANDLE);
            char msgType = (hFile == hCurrentStderr) ? MSG_STDERR : MSG_STDOUT;
            SendToPipeWithType((const char*)lpBuffer, nNumberOfBytesToWrite, msgType);
        }
    }

    // Call original
    if (g_OriginalWriteFile) {
        return g_OriginalWriteFile(hFile, lpBuffer, nNumberOfBytesToWrite, lpNumberOfBytesWritten, lpOverlapped);
    }
    return FALSE;
}

// Hooked WriteConsoleOutputCharacterA - intercept status line writes
static BOOL WINAPI HookedWriteConsoleOutputCharacterA(
    HANDLE hConsoleOutput,
    LPCSTR lpCharacter,
    DWORD nLength,
    COORD dwWriteCoord,
    LPDWORD lpNumberOfCharsWritten
) {
    // Check if writing to the first line (status line)
    if (dwWriteCoord.Y == 0) {
        SendToPipeWithType(lpCharacter, nLength, MSG_STATUSLINE);
    }

    // Call original - use saved pointer or get from kernel32 directly
    if (g_OriginalWriteConsoleOutputCharacterA) {
        return g_OriginalWriteConsoleOutputCharacterA(hConsoleOutput, lpCharacter, nLength, dwWriteCoord, lpNumberOfCharsWritten);
    }
    // Fallback: get the function directly from kernel32
    static auto pFunc = (decltype(&WriteConsoleOutputCharacterA))GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "WriteConsoleOutputCharacterA");
    if (pFunc) {
        return pFunc(hConsoleOutput, lpCharacter, nLength, dwWriteCoord, lpNumberOfCharsWritten);
    }
    if (lpNumberOfCharsWritten) *lpNumberOfCharsWritten = nLength;
    return TRUE;
}

// Hooked WriteConsoleOutputCharacterW - intercept status line writes
static BOOL WINAPI HookedWriteConsoleOutputCharacterW(
    HANDLE hConsoleOutput,
    LPCWSTR lpCharacter,
    DWORD nLength,
    COORD dwWriteCoord,
    LPDWORD lpNumberOfCharsWritten
) {
    // Check if writing to the first line (status line)
    if (dwWriteCoord.Y == 0) {
        SendToPipeWithTypeW(lpCharacter, nLength, MSG_STATUSLINE);
    }

    // Call original - use saved pointer or get from kernel32 directly
    if (g_OriginalWriteConsoleOutputCharacterW) {
        return g_OriginalWriteConsoleOutputCharacterW(hConsoleOutput, lpCharacter, nLength, dwWriteCoord, lpNumberOfCharsWritten);
    }
    // Fallback: get the function directly from kernel32
    static auto pFunc = (decltype(&WriteConsoleOutputCharacterW))GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "WriteConsoleOutputCharacterW");
    if (pFunc) {
        return pFunc(hConsoleOutput, lpCharacter, nLength, dwWriteCoord, lpNumberOfCharsWritten);
    }
    if (lpNumberOfCharsWritten) *lpNumberOfCharsWritten = nLength;
    return TRUE;
}

// Hooked WriteConsoleOutputAttribute - intercept status line color
static BOOL WINAPI HookedWriteConsoleOutputAttribute(
    HANDLE hConsoleOutput,
    const WORD* lpAttribute,
    DWORD nLength,
    COORD dwWriteCoord,
    LPDWORD lpNumberOfAttrsWritten
) {
    // Check if writing to the first line (status line)
    if (dwWriteCoord.Y == 0 && nLength > 0 && lpAttribute) {
        // Capture the attribute and send to pipe
        g_statusLineAttrib = lpAttribute[0];
        SendToPipeWithType((const char*)&g_statusLineAttrib, sizeof(WORD), MSG_STATUSLINE_ATTR);
    }

    // Call original - use saved pointer or get from kernel32 directly
    if (g_OriginalWriteConsoleOutputAttribute) {
        return g_OriginalWriteConsoleOutputAttribute(hConsoleOutput, lpAttribute, nLength, dwWriteCoord, lpNumberOfAttrsWritten);
    }
    // Fallback: get the function directly from kernel32
    static auto pFunc = (decltype(&WriteConsoleOutputAttribute))GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "WriteConsoleOutputAttribute");
    if (pFunc) {
        return pFunc(hConsoleOutput, lpAttribute, nLength, dwWriteCoord, lpNumberOfAttrsWritten);
    }
    if (lpNumberOfAttrsWritten) *lpNumberOfAttrsWritten = nLength;
    return TRUE;
}

// Hooked SetConsoleTitleA - intercept title changes
static BOOL WINAPI HookedSetConsoleTitleA(LPCSTR lpConsoleTitle) {
    if (lpConsoleTitle) {
        SendToPipeWithType(lpConsoleTitle, (DWORD)strlen(lpConsoleTitle), MSG_TITLE);
    }

    // Call original - use saved pointer or get from kernel32 directly
    if (g_OriginalSetConsoleTitleA) {
        return g_OriginalSetConsoleTitleA(lpConsoleTitle);
    }
    static auto pFunc = (decltype(&SetConsoleTitleA))GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetConsoleTitleA");
    if (pFunc) {
        return pFunc(lpConsoleTitle);
    }
    return TRUE;
}

// Hooked SetConsoleTitleW - intercept title changes
static BOOL WINAPI HookedSetConsoleTitleW(LPCWSTR lpConsoleTitle) {
    if (lpConsoleTitle) {
        SendToPipeWithTypeW(lpConsoleTitle, (DWORD)wcslen(lpConsoleTitle), MSG_TITLE);
    }

    // Call original - use saved pointer or get from kernel32 directly
    if (g_OriginalSetConsoleTitleW) {
        return g_OriginalSetConsoleTitleW(lpConsoleTitle);
    }
    static auto pFunc = (decltype(&SetConsoleTitleW))GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetConsoleTitleW");
    if (pFunc) {
        return pFunc(lpConsoleTitle);
    }
    return TRUE;
}

// Simple IAT hooking
static bool HookIAT(HMODULE hModule, const char* dllName, const char* funcName, void* hookFunc, void** origFunc) {
    if (!hModule) hModule = GetModuleHandleW(nullptr);

    PIMAGE_DOS_HEADER dosHeader = (PIMAGE_DOS_HEADER)hModule;
    if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE) return false;

    PIMAGE_NT_HEADERS ntHeaders = (PIMAGE_NT_HEADERS)((BYTE*)hModule + dosHeader->e_lfanew);
    if (ntHeaders->Signature != IMAGE_NT_SIGNATURE) return false;

    PIMAGE_IMPORT_DESCRIPTOR importDesc = (PIMAGE_IMPORT_DESCRIPTOR)((BYTE*)hModule +
        ntHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);

    if (!importDesc) return false;

    while (importDesc->Name) {
        const char* moduleName = (const char*)((BYTE*)hModule + importDesc->Name);

        if (_stricmp(moduleName, dllName) == 0) {
            PIMAGE_THUNK_DATA origThunk = (PIMAGE_THUNK_DATA)((BYTE*)hModule + importDesc->OriginalFirstThunk);
            PIMAGE_THUNK_DATA thunk = (PIMAGE_THUNK_DATA)((BYTE*)hModule + importDesc->FirstThunk);

            while (origThunk->u1.AddressOfData) {
                if (!(origThunk->u1.Ordinal & IMAGE_ORDINAL_FLAG)) {
                    PIMAGE_IMPORT_BY_NAME importByName = (PIMAGE_IMPORT_BY_NAME)((BYTE*)hModule + origThunk->u1.AddressOfData);

                    if (strcmp(importByName->Name, funcName) == 0) {
                        DWORD oldProtect;
                        if (VirtualProtect(&thunk->u1.Function, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
                            if (origFunc) *origFunc = (void*)thunk->u1.Function;
                            thunk->u1.Function = (ULONG_PTR)hookFunc;
                            VirtualProtect(&thunk->u1.Function, sizeof(void*), oldProtect, &oldProtect);
                            return true;
                        }
                    }
                }
                origThunk++;
                thunk++;
            }
        }
        importDesc++;
    }

    return false;
}

// Hook WriteFile/WriteConsole in a single module
static void HookModule(HMODULE hModule) {
    if (!hModule) return;

    char moduleName[MAX_PATH];
    GetModuleFileNameA(hModule, moduleName, MAX_PATH);

    bool hooked = false;
    hooked |= HookIAT(hModule, "kernel32.dll", "WriteConsoleA", (void*)HookedWriteConsoleA, nullptr);
    hooked |= HookIAT(hModule, "kernel32.dll", "WriteConsoleW", (void*)HookedWriteConsoleW, nullptr);
    hooked |= HookIAT(hModule, "kernel32.dll", "WriteFile", (void*)HookedWriteFile, nullptr);
    hooked |= HookIAT(hModule, "kernel32.dll", "WriteConsoleOutputCharacterA", (void*)HookedWriteConsoleOutputCharacterA, nullptr);
    hooked |= HookIAT(hModule, "kernel32.dll", "WriteConsoleOutputCharacterW", (void*)HookedWriteConsoleOutputCharacterW, nullptr);
    hooked |= HookIAT(hModule, "kernel32.dll", "WriteConsoleOutputAttribute", (void*)HookedWriteConsoleOutputAttribute, nullptr);
    hooked |= HookIAT(hModule, "kernel32.dll", "SetConsoleTitleA", (void*)HookedSetConsoleTitleA, nullptr);
    hooked |= HookIAT(hModule, "kernel32.dll", "SetConsoleTitleW", (void*)HookedSetConsoleTitleW, nullptr);
    hooked |= HookIAT(hModule, "user32.dll", "SendMessageW", (void*)HookedSendMessageW, nullptr);

    if (hooked) {
        char debugBuf[512];
        sprintf_s(debugBuf, "[ConsoleHook] HookModule: Hooked newly loaded module: %s\n", moduleName);
        OutputDebugStringA(debugBuf);
    }
}

// Hooked LoadLibraryA
static HMODULE WINAPI HookedLoadLibraryA(LPCSTR lpLibFileName) {
    HMODULE hModule = g_OriginalLoadLibraryA ? g_OriginalLoadLibraryA(lpLibFileName) : LoadLibraryA(lpLibFileName);
    if (hModule) {
        HookModule(hModule);
    }
    return hModule;
}

// Hooked LoadLibraryW
static HMODULE WINAPI HookedLoadLibraryW(LPCWSTR lpLibFileName) {
    HMODULE hModule = g_OriginalLoadLibraryW ? g_OriginalLoadLibraryW(lpLibFileName) : LoadLibraryW(lpLibFileName);
    if (hModule) {
        HookModule(hModule);
    }
    return hModule;
}

// Hooked LoadLibraryExA
static HMODULE WINAPI HookedLoadLibraryExA(LPCSTR lpLibFileName, HANDLE hFile, DWORD dwFlags) {
    HMODULE hModule = g_OriginalLoadLibraryExA ? g_OriginalLoadLibraryExA(lpLibFileName, hFile, dwFlags) : LoadLibraryExA(lpLibFileName, hFile, dwFlags);
    if (hModule && !(dwFlags & (LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE))) {
        HookModule(hModule);
    }
    return hModule;
}

// Hooked LoadLibraryExW
static HMODULE WINAPI HookedLoadLibraryExW(LPCWSTR lpLibFileName, HANDLE hFile, DWORD dwFlags) {
    HMODULE hModule = g_OriginalLoadLibraryExW ? g_OriginalLoadLibraryExW(lpLibFileName, hFile, dwFlags) : LoadLibraryExW(lpLibFileName, hFile, dwFlags);
    if (hModule && !(dwFlags & (LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE))) {
        HookModule(hModule);
    }
    return hModule;
}

static void InstallHooks() {
    HMODULE hModule = GetModuleHandleW(nullptr);

    OutputDebugStringA("[ConsoleHook] InstallHooks: Starting hook installation\n");

    // Hook WriteConsoleA
    if (HookIAT(hModule, "kernel32.dll", "WriteConsoleA", (void*)HookedWriteConsoleA, (void**)&g_OriginalWriteConsoleA)) {
        OutputDebugStringA("[ConsoleHook] Hooked WriteConsoleA in main module\n");
    }

    // Hook WriteConsoleW
    if (HookIAT(hModule, "kernel32.dll", "WriteConsoleW", (void*)HookedWriteConsoleW, (void**)&g_OriginalWriteConsoleW)) {
        OutputDebugStringA("[ConsoleHook] Hooked WriteConsoleW in main module\n");
    }

    // Hook WriteFile
    if (HookIAT(hModule, "kernel32.dll", "WriteFile", (void*)HookedWriteFile, (void**)&g_OriginalWriteFile)) {
        OutputDebugStringA("[ConsoleHook] Hooked WriteFile in main module\n");
    }

    // Hook WriteConsoleOutputCharacterA/W for status line
    if (HookIAT(hModule, "kernel32.dll", "WriteConsoleOutputCharacterA", (void*)HookedWriteConsoleOutputCharacterA, (void**)&g_OriginalWriteConsoleOutputCharacterA)) {
        OutputDebugStringA("[ConsoleHook] Hooked WriteConsoleOutputCharacterA in main module\n");
    }
    if (HookIAT(hModule, "kernel32.dll", "WriteConsoleOutputCharacterW", (void*)HookedWriteConsoleOutputCharacterW, (void**)&g_OriginalWriteConsoleOutputCharacterW)) {
        OutputDebugStringA("[ConsoleHook] Hooked WriteConsoleOutputCharacterW in main module\n");
    }

    // Hook WriteConsoleOutputAttribute for status line color
    if (HookIAT(hModule, "kernel32.dll", "WriteConsoleOutputAttribute", (void*)HookedWriteConsoleOutputAttribute, (void**)&g_OriginalWriteConsoleOutputAttribute)) {
        OutputDebugStringA("[ConsoleHook] Hooked WriteConsoleOutputAttribute in main module\n");
    }

    // Hook SetConsoleTitleA/W for title changes
    if (HookIAT(hModule, "kernel32.dll", "SetConsoleTitleA", (void*)HookedSetConsoleTitleA, (void**)&g_OriginalSetConsoleTitleA)) {
        OutputDebugStringA("[ConsoleHook] Hooked SetConsoleTitleA in main module\n");
    }
    if (HookIAT(hModule, "kernel32.dll", "SetConsoleTitleW", (void*)HookedSetConsoleTitleW, (void**)&g_OriginalSetConsoleTitleW)) {
        OutputDebugStringA("[ConsoleHook] Hooked SetConsoleTitleW in main module\n");
    }

    // Hook SendMessageW for icon changes (user32.dll)
    if (HookIAT(hModule, "user32.dll", "SendMessageW", (void*)HookedSendMessageW, (void**)&g_OriginalSendMessageW)) {
        OutputDebugStringA("[ConsoleHook] Hooked SendMessageW in main module\n");
    }

    // Hook LoadLibrary functions to catch dynamically loaded modules
    if (HookIAT(hModule, "kernel32.dll", "LoadLibraryA", (void*)HookedLoadLibraryA, (void**)&g_OriginalLoadLibraryA)) {
        OutputDebugStringA("[ConsoleHook] Hooked LoadLibraryA in main module\n");
    }
    if (HookIAT(hModule, "kernel32.dll", "LoadLibraryW", (void*)HookedLoadLibraryW, (void**)&g_OriginalLoadLibraryW)) {
        OutputDebugStringA("[ConsoleHook] Hooked LoadLibraryW in main module\n");
    }
    if (HookIAT(hModule, "kernel32.dll", "LoadLibraryExA", (void*)HookedLoadLibraryExA, (void**)&g_OriginalLoadLibraryExA)) {
        OutputDebugStringA("[ConsoleHook] Hooked LoadLibraryExA in main module\n");
    }
    if (HookIAT(hModule, "kernel32.dll", "LoadLibraryExW", (void*)HookedLoadLibraryExW, (void**)&g_OriginalLoadLibraryExW)) {
        OutputDebugStringA("[ConsoleHook] Hooked LoadLibraryExW in main module\n");
    }

    // Also try to hook in other loaded modules
    HMODULE hModules[256];
    DWORD cbNeeded;
    if (EnumProcessModules(GetCurrentProcess(), hModules, sizeof(hModules), &cbNeeded)) {
        DWORD moduleCount = cbNeeded / sizeof(HMODULE);
        char debugBuf[256];
        sprintf_s(debugBuf, "[ConsoleHook] Found %lu loaded modules\n", moduleCount);
        OutputDebugStringA(debugBuf);

        for (DWORD i = 0; i < moduleCount; i++) {
            if (hModules[i] != hModule) {
                char moduleName[MAX_PATH];
                if (GetModuleFileNameA(hModules[i], moduleName, MAX_PATH)) {
                    bool hooked = false;
                    hooked |= HookIAT(hModules[i], "kernel32.dll", "WriteConsoleA", (void*)HookedWriteConsoleA, nullptr);
                    hooked |= HookIAT(hModules[i], "kernel32.dll", "WriteConsoleW", (void*)HookedWriteConsoleW, nullptr);
                    hooked |= HookIAT(hModules[i], "kernel32.dll", "WriteFile", (void*)HookedWriteFile, nullptr);
                    hooked |= HookIAT(hModules[i], "kernel32.dll", "WriteConsoleOutputCharacterA", (void*)HookedWriteConsoleOutputCharacterA, nullptr);
                    hooked |= HookIAT(hModules[i], "kernel32.dll", "WriteConsoleOutputCharacterW", (void*)HookedWriteConsoleOutputCharacterW, nullptr);
                    hooked |= HookIAT(hModules[i], "kernel32.dll", "WriteConsoleOutputAttribute", (void*)HookedWriteConsoleOutputAttribute, nullptr);
                    hooked |= HookIAT(hModules[i], "kernel32.dll", "SetConsoleTitleA", (void*)HookedSetConsoleTitleA, nullptr);
                    hooked |= HookIAT(hModules[i], "kernel32.dll", "SetConsoleTitleW", (void*)HookedSetConsoleTitleW, nullptr);
                    hooked |= HookIAT(hModules[i], "user32.dll", "SendMessageW", (void*)HookedSendMessageW, nullptr);
                    if (hooked) {
                        sprintf_s(debugBuf, "[ConsoleHook] Hooked module: %s\n", moduleName);
                        OutputDebugStringA(debugBuf);
                    }
                }
            }
        }
    }

    OutputDebugStringA("[ConsoleHook] InstallHooks: Hook installation complete\n");
}

// Thread to read stdin from pipe and write to console input
static DWORD WINAPI StdinReaderThread(LPVOID) {
    OutputDebugStringA("[ConsoleHook] StdinReaderThread: Starting\n");

    char buffer[4096];
    DWORD bytesRead;

    while (g_stdinRunning && g_pipeConnected) {
        if (!ReadFile(g_hPipeIn, buffer, sizeof(buffer), &bytesRead, nullptr)) {
            DWORD err = GetLastError();
            if (err == ERROR_NO_DATA) {
                // Non-blocking pipe has no data, sleep and retry
                Sleep(50);
                continue;
            }
            if (err != ERROR_BROKEN_PIPE && err != ERROR_PIPE_NOT_CONNECTED) {
                char debugBuf[64];
                sprintf_s(debugBuf, "[ConsoleHook] StdinReaderThread: ReadFile error %lu\n", err);
                OutputDebugStringA(debugBuf);
            }
            break;
        }

        if (bytesRead == 0) {
            Sleep(50);
            continue;
        }

        char debugBuf[128];
        sprintf_s(debugBuf, "[ConsoleHook] StdinReaderThread: Read %lu bytes from pipe\n", bytesRead);
        OutputDebugStringA(debugBuf);

        // Write to console input
        HANDLE hStdin = GetStdHandle(STD_INPUT_HANDLE);
        if (hStdin != INVALID_HANDLE_VALUE) {
            // Convert to input records for WriteConsoleInput
            INPUT_RECORD* inputRecords = new INPUT_RECORD[bytesRead];
            for (DWORD i = 0; i < bytesRead; i++) {
                inputRecords[i].EventType = KEY_EVENT;
                inputRecords[i].Event.KeyEvent.bKeyDown = TRUE;
                inputRecords[i].Event.KeyEvent.wRepeatCount = 1;
                inputRecords[i].Event.KeyEvent.dwControlKeyState = 0;

                char c = buffer[i];
                if (c == '\n' || c == '\r') {
                    // Enter key
                    inputRecords[i].Event.KeyEvent.wVirtualKeyCode = VK_RETURN;
                    inputRecords[i].Event.KeyEvent.wVirtualScanCode = MapVirtualKeyA(VK_RETURN, MAPVK_VK_TO_VSC);
                    inputRecords[i].Event.KeyEvent.uChar.AsciiChar = '\r';
                } else {
                    inputRecords[i].Event.KeyEvent.wVirtualKeyCode = VkKeyScanA(c) & 0xFF;
                    inputRecords[i].Event.KeyEvent.wVirtualScanCode = MapVirtualKeyA(inputRecords[i].Event.KeyEvent.wVirtualKeyCode, MAPVK_VK_TO_VSC);
                    inputRecords[i].Event.KeyEvent.uChar.AsciiChar = c;
                }
            }

            DWORD written;
            WriteConsoleInputA(hStdin, inputRecords, bytesRead, &written);
            delete[] inputRecords;

            sprintf_s(debugBuf, "[ConsoleHook] StdinReaderThread: Wrote %lu chars to console input\n", written);
            OutputDebugStringA(debugBuf);
        }
    }

    OutputDebugStringA("[ConsoleHook] StdinReaderThread: Exiting\n");
    return 0;
}

// Background thread to wait for pipe connection
static DWORD WINAPI PipeConnectThread(LPVOID) {
    OutputDebugStringA("[ConsoleHook] PipeConnectThread: Waiting for client connection\n");

    bool outConnected = false;
    bool inConnected = false;

    // Wait for clients to connect (with timeout)
    for (int i = 0; i < 100; i++) {  // 10 seconds max
        // Try to connect output pipe
        if (!outConnected) {
            BOOL connected = ConnectNamedPipe(g_hPipeOut, nullptr);
            DWORD err = GetLastError();
            if (connected || err == ERROR_PIPE_CONNECTED) {
                OutputDebugStringA("[ConsoleHook] PipeConnectThread: Output pipe connected!\n");
                DWORD mode = PIPE_READMODE_BYTE | PIPE_WAIT;
                SetNamedPipeHandleState(g_hPipeOut, &mode, nullptr, nullptr);
                outConnected = true;
            }
        }

        // Try to connect input pipe
        if (!inConnected) {
            BOOL connected = ConnectNamedPipe(g_hPipeIn, nullptr);
            DWORD err = GetLastError();
            if (connected || err == ERROR_PIPE_CONNECTED) {
                OutputDebugStringA("[ConsoleHook] PipeConnectThread: Input pipe connected!\n");
                DWORD mode = PIPE_READMODE_BYTE | PIPE_WAIT;
                SetNamedPipeHandleState(g_hPipeIn, &mode, nullptr, nullptr);
                inConnected = true;
            }
        }

        if (outConnected && inConnected) {
            g_pipeConnected = true;

            // Start stdin reader thread
            g_stdinRunning = true;
            g_hStdinThread = CreateThread(nullptr, 0, StdinReaderThread, nullptr, 0, nullptr);

            return 0;
        }

        Sleep(100);
    }

    OutputDebugStringA("[ConsoleHook] PipeConnectThread: Timeout waiting for client\n");
    return 1;
}

static bool Initialize() {
    if (g_initialized) return true;

    OutputDebugStringA("[ConsoleHook] Initialize starting\n");

    // Get console handles
    g_hStdout = GetStdHandle(STD_OUTPUT_HANDLE);
    g_hStderr = GetStdHandle(STD_ERROR_HANDLE);

    // Store console window handle for icon interception
    g_hTargetConsoleWnd = GetConsoleWindow();

    // Create named pipes for communication
    // Pipe names include PID for uniqueness
    wchar_t pipeNameOut[256];
    wchar_t pipeNameIn[256];
    swprintf_s(pipeNameOut, L"\\\\.\\pipe\\ConsoleForwarder_%lu_out", GetCurrentProcessId());
    swprintf_s(pipeNameIn, L"\\\\.\\pipe\\ConsoleForwarder_%lu_in", GetCurrentProcessId());

    char debugBuf[300];
    sprintf_s(debugBuf, "[ConsoleHook] Creating pipes for PID %lu\n", GetCurrentProcessId());
    OutputDebugStringA(debugBuf);

    // Output pipe: hook writes, launcher reads
    g_hPipeOut = CreateNamedPipeW(
        pipeNameOut,
        PIPE_ACCESS_OUTBOUND,  // Write only
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_NOWAIT,
        1,
        4096,
        4096,
        0,
        nullptr
    );

    if (g_hPipeOut == INVALID_HANDLE_VALUE) {
        OutputDebugStringA("[ConsoleHook] Failed to create output pipe\n");
        return false;
    }

    // Input pipe: launcher writes, hook reads
    // Use PIPE_NOWAIT for non-blocking ConnectNamedPipe, will poll in reader thread
    g_hPipeIn = CreateNamedPipeW(
        pipeNameIn,
        PIPE_ACCESS_INBOUND,  // Read only
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_NOWAIT,
        1,
        4096,
        4096,
        0,
        nullptr
    );

    if (g_hPipeIn == INVALID_HANDLE_VALUE) {
        OutputDebugStringA("[ConsoleHook] Failed to create input pipe\n");
        CloseHandle(g_hPipeOut);
        g_hPipeOut = INVALID_HANDLE_VALUE;
        return false;
    }

    OutputDebugStringA("[ConsoleHook] Pipes created, starting connect thread and installing hooks\n");

    // Start background thread to wait for client connection
    // This avoids blocking DllMain
    g_hConnectThread = CreateThread(nullptr, 0, PipeConnectThread, nullptr, 0, nullptr);

    // Install hooks BEFORE waiting for connection
    // This allows DllMain to return quickly
    InstallHooks();

    g_initialized = true;
    OutputDebugStringA("[ConsoleHook] Initialize complete\n");
    return true;
}

static void Cleanup() {
    g_pipeConnected = false;
    g_stdinRunning = false;

    if (g_hStdinThread) {
        WaitForSingleObject(g_hStdinThread, 1000);
        CloseHandle(g_hStdinThread);
        g_hStdinThread = nullptr;
    }

    if (g_hConnectThread) {
        WaitForSingleObject(g_hConnectThread, 1000);
        CloseHandle(g_hConnectThread);
        g_hConnectThread = nullptr;
    }

    if (g_hPipeOut != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(g_hPipeOut);
        DisconnectNamedPipe(g_hPipeOut);
        CloseHandle(g_hPipeOut);
        g_hPipeOut = INVALID_HANDLE_VALUE;
    }

    if (g_hPipeIn != INVALID_HANDLE_VALUE) {
        DisconnectNamedPipe(g_hPipeIn);
        CloseHandle(g_hPipeIn);
        g_hPipeIn = INVALID_HANDLE_VALUE;
    }
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    switch (ul_reason_for_call) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);
        Initialize();
        break;
    case DLL_PROCESS_DETACH:
        Cleanup();
        break;
    }
    return TRUE;
}

// Export for manual initialization if needed
extern "C" __declspec(dllexport) BOOL InitializeHook() {
    return Initialize() ? TRUE : FALSE;
}

// Need psapi for EnumProcessModules
#pragma comment(lib, "psapi.lib")
