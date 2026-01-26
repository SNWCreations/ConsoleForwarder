#pragma once

#include <string>
#include <vector>

enum class CaptureMode {
    Auto,       // Auto-detect best method
    ConPTY,     // Use ConPTY (Win10 1809+)
    Legacy,     // Use legacy console buffer reading
    Inject      // Use DLL injection
};

enum class StdinMode {
    Auto,       // Auto-detect: enable if stdin is a terminal
    ForceOn,    // Always enable stdin forwarding
    ForceOff    // Always disable stdin forwarding
};

struct LaunchOptions {
    std::wstring program;
    std::vector<std::wstring> args;
    CaptureMode mode = CaptureMode::Auto;
    StdinMode stdinMode = StdinMode::Auto;
    bool hideWindow = false;
    bool showHelp = false;
};

bool ParseArguments(int argc, wchar_t* argv[], LaunchOptions& options);
void PrintUsage(const wchar_t* programName);
