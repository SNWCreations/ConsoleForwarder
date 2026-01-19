#pragma once

#include <windows.h>
#include <string>
#include <vector>

struct LegacyConsoleHandle {
    HANDLE hProcess = INVALID_HANDLE_VALUE;
    HANDLE hThread = INVALID_HANDLE_VALUE;
    HWND hConsoleWnd = nullptr;
    HANDLE hStdinWrite = INVALID_HANDLE_VALUE;
    bool running = false;

    void Close();
};

bool CreateLegacyProcess(
    const std::wstring& program,
    const std::vector<std::wstring>& args,
    bool hideWindow,
    LegacyConsoleHandle& handle
);

void RunLegacyLoop(LegacyConsoleHandle& handle);
