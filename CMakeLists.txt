cmake_minimum_required(VERSION 3.15)
project(ConsoleForwarder LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Main executable
add_executable(ConsoleForwarder
    src/main.cpp
    src/args.cpp
    src/conpty.cpp
    src/legacy_console.cpp
    src/injector.cpp
)

target_include_directories(ConsoleForwarder PRIVATE src)

if(MSVC)
    target_compile_options(ConsoleForwarder PRIVATE /W4 /utf-8)
    target_compile_definitions(ConsoleForwarder PRIVATE _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE)
else()
    target_compile_options(ConsoleForwarder PRIVATE -Wall -Wextra)
    target_compile_definitions(ConsoleForwarder PRIVATE UNICODE _UNICODE)
endif()

target_link_libraries(ConsoleForwarder PRIVATE kernel32)

# Hook DLL for injection approach
add_library(ConsoleHook SHARED
    src/console_hook.cpp
)

target_include_directories(ConsoleHook PRIVATE src)

if(MSVC)
    target_compile_options(ConsoleHook PRIVATE /W4 /utf-8)
    target_compile_definitions(ConsoleHook PRIVATE _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE)
else()
    target_compile_options(ConsoleHook PRIVATE -Wall -Wextra)
    target_compile_definitions(ConsoleHook PRIVATE UNICODE _UNICODE)
endif()

target_link_libraries(ConsoleHook PRIVATE kernel32 psapi)
