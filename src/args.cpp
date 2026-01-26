#include "args.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <io.h>
#include <fcntl.h>

static std::vector<std::wstring> ParseArgFile(const std::wstring& filePath) {
    std::vector<std::wstring> result;
    std::wifstream file(filePath);
    if (!file.is_open()) {
        return result;
    }

    std::wstring line;
    while (std::getline(file, line)) {
        // Trim whitespace
        size_t start = line.find_first_not_of(L" \t\r\n");
        if (start == std::wstring::npos) continue;
        size_t end = line.find_last_not_of(L" \t\r\n");
        line = line.substr(start, end - start + 1);

        // Skip comments
        if (line.empty() || line[0] == L'#') continue;

        // Handle quoted strings
        if (line.front() == L'"' && line.back() == L'"' && line.length() > 1) {
            line = line.substr(1, line.length() - 2);
        }

        result.push_back(line);
    }

    return result;
}

static std::vector<std::wstring> ExpandArguments(int argc, wchar_t* argv[]) {
    std::vector<std::wstring> expanded;

    for (int i = 0; i < argc; i++) {
        std::wstring arg = argv[i];

        // Check for @argfile syntax
        if (!arg.empty() && arg[0] == L'@') {
            std::wstring filePath = arg.substr(1);
            auto fileArgs = ParseArgFile(filePath);
            expanded.insert(expanded.end(), fileArgs.begin(), fileArgs.end());
        } else {
            expanded.push_back(arg);
        }
    }

    return expanded;
}

bool ParseArguments(int argc, wchar_t* argv[], LaunchOptions& options) {
    if (argc < 2) {
        options.showHelp = true;
        return true;
    }

    auto args = ExpandArguments(argc, argv);

    bool foundProgram = false;
    for (size_t i = 1; i < args.size(); i++) {
        const std::wstring& arg = args[i];

        if (!foundProgram) {
            if (arg == L"-h" || arg == L"--help" || arg == L"/?") {
                options.showHelp = true;
                return true;
            } else if (arg == L"--mode") {
                if (i + 1 >= args.size()) {
                    fwprintf(stderr, L"Error: --mode requires an argument\n");
                    return false;
                }
                std::wstring mode = args[++i];
                std::transform(mode.begin(), mode.end(), mode.begin(), ::towlower);
                if (mode == L"auto") {
                    options.mode = CaptureMode::Auto;
                } else if (mode == L"conpty") {
                    options.mode = CaptureMode::ConPTY;
                } else if (mode == L"legacy") {
                    options.mode = CaptureMode::Legacy;
                } else if (mode == L"inject") {
                    options.mode = CaptureMode::Inject;
                } else {
                    fwprintf(stderr, L"Error: Unknown mode '%s'\n", mode.c_str());
                    return false;
                }
            } else if (arg == L"--hide") {
                options.hideWindow = true;
            } else if (arg == L"--show") {
                options.hideWindow = false;
            } else if (arg == L"--stdin") {
                options.stdinMode = StdinMode::ForceOn;
            } else if (arg == L"--no-stdin") {
                options.stdinMode = StdinMode::ForceOff;
            } else if (arg[0] == L'-') {
                fwprintf(stderr, L"Error: Unknown option '%s'\n", arg.c_str());
                return false;
            } else {
                options.program = arg;
                foundProgram = true;
            }
        } else {
            options.args.push_back(arg);
        }
    }

    if (!foundProgram) {
        options.showHelp = true;
    }

    return true;
}

void PrintUsage(const wchar_t* programName) {
    wprintf(L"ConsoleForwarder - Capture console output from programs that create their own console\n\n");
    wprintf(L"Usage: %s [options] <program> [program arguments...]\n\n", programName);
    wprintf(L"Options:\n");
    wprintf(L"  -h, --help       Show this help message\n");
    wprintf(L"  --mode <mode>    Capture mode: auto, conpty, legacy, inject (default: auto)\n");
    wprintf(L"  --hide           Hide the child process console window\n");
    wprintf(L"  --show           Show the child process console window (default)\n");
    wprintf(L"  --stdin          Force enable stdin forwarding to child process\n");
    wprintf(L"  --no-stdin       Force disable stdin forwarding to child process\n");
    wprintf(L"\nArgument file:\n");
    wprintf(L"  Use @filename to read arguments from a file (one per line)\n");
    wprintf(L"\nModes:\n");
    wprintf(L"  auto    - Automatically select best method for the system\n");
    wprintf(L"  conpty  - Use Windows Pseudo Console (requires Win10 1809+)\n");
    wprintf(L"  legacy  - Use console buffer reading (works on older Windows)\n");
    wprintf(L"  inject  - Use DLL injection to hook WriteConsole\n");
    wprintf(L"\nStdin forwarding:\n");
    wprintf(L"  By default, stdin is forwarded only when it is a terminal.\n");
    wprintf(L"  Use --stdin or --no-stdin to override this behavior.\n");
    wprintf(L"\nExamples:\n");
    wprintf(L"  %s srcds.exe -game tf +maxplayers 24\n", programName);
    wprintf(L"  %s --mode inject --hide srcds.exe @server_args.txt\n", programName);
    wprintf(L"  %s --mode conpty --no-stdin FactoryServer.exe\n", programName);
}
