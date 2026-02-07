#pragma once

#include <windows.h>
#include <string>
#include <vector>

bool InjectDLL(HANDLE hProcess, const std::wstring& dllPath);

bool CreateInjectedProcess(
    const std::wstring& program,
    const std::vector<std::wstring>& args,
    bool hideWindow,
    const std::wstring& dllPath,
    HANDLE& hProcess,
    HANDLE& hThread
);

void RunInjectedLoop(HANDLE hProcess, const std::wstring& pipeName, const std::wstring& program, bool hideWindow);
