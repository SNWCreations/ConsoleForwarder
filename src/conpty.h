#pragma once

#include <windows.h>
#include <string>
#include <vector>
#include "args.h"

bool IsConPTYAvailable();

struct ConPTYHandle {
    HPCON hPC = nullptr;
    HANDLE hPipeIn = INVALID_HANDLE_VALUE;
    HANDLE hPipeOut = INVALID_HANDLE_VALUE;
    HANDLE hProcess = INVALID_HANDLE_VALUE;
    HANDLE hThread = INVALID_HANDLE_VALUE;

    void Close();
};

bool CreateConPTYProcess(
    const std::wstring& program,
    const std::vector<std::wstring>& args,
    bool hideWindow,
    ConPTYHandle& handle
);

void RunConPTYLoop(ConPTYHandle& handle, StdinMode stdinMode);
