@echo off
setlocal enabledelayedexpansion

set BUILD_CONFIG=Release
set BUILD_ARCH=both
set DO_CLEAN=0

:: Parse all arguments
:parse_args
if "%1"=="" goto start_build
if "%1"=="build" shift & goto parse_args
if "%1"=="clean" goto do_clean
if "%1"=="rebuild" set DO_CLEAN=1 & shift & goto parse_args
if "%1"=="help" goto help
if "%1"=="-h" goto help
if "%1"=="--help" goto help
if "%1"=="debug" set BUILD_CONFIG=Debug & shift & goto parse_args
if "%1"=="release" set BUILD_CONFIG=Release & shift & goto parse_args
if "%1"=="x86" set BUILD_ARCH=x86 & shift & goto parse_args
if "%1"=="x64" set BUILD_ARCH=x64 & shift & goto parse_args
if "%1"=="32" set BUILD_ARCH=x86 & shift & goto parse_args
if "%1"=="64" set BUILD_ARCH=x64 & shift & goto parse_args
echo Unknown argument: %1
goto help

:start_build
if "%DO_CLEAN%"=="1" call :clean_dirs
goto build

:do_clean
call :clean_dirs
goto end

:clean_dirs
echo Cleaning build directories...
if exist "build" rmdir /s /q "build" && echo Cleaned build x64.
if exist "build32" rmdir /s /q "build32" && echo Cleaned build32 x86.
echo Clean complete.
exit /b 0

:check_requirements
:: First try cmake in PATH
where cmake >nul 2>&1
if not errorlevel 1 (
    set "CMAKE_CMD=cmake"
    goto :check_compiler
)

:: Try Visual Studio's CMake
for %%V in (2022 2019) do (
    for %%E in (Community Professional Enterprise) do (
        set "TEST_PATH=C:\Program Files\Microsoft Visual Studio\%%V\%%E\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
        if exist "!TEST_PATH!" (
            set "CMAKE_CMD=!TEST_PATH!"
            goto :check_compiler
        )
    )
)

echo ERROR: CMake not found.
echo.
echo Please install one of the following:
echo   - CMake (https://cmake.org/download/)
echo   - Visual Studio with C++ workload
exit /b 1

:check_compiler
where cl >nul 2>&1
if not errorlevel 1 exit /b 0

:: Try to set up Visual Studio environment
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "!VSWHERE!" (
    for /f "usebackq tokens=*" %%i in (`"!VSWHERE!" -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS_PATH=%%i"
)
if defined VS_PATH (
    if exist "!VS_PATH!\VC\Auxiliary\Build\vcvarsall.bat" (
        echo Setting up Visual Studio environment...
        call "!VS_PATH!\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul 2>&1
    )
)
where cl >nul 2>&1
if errorlevel 1 (
    echo ERROR: Visual Studio C++ compiler not found.
    exit /b 1
)
exit /b 0

:build
call :check_requirements
if errorlevel 1 goto end

if "%BUILD_ARCH%"=="x64" goto build_x64_only
if "%BUILD_ARCH%"=="x86" goto build_x86_only
goto build_both

:build_both
echo Building ConsoleForwarder [%BUILD_CONFIG%] [x64 and x86]...
echo.

echo === Building 64-bit ===
if not exist "build" (
    "!CMAKE_CMD!" -B build -G "Visual Studio 17 2022" -A x64
    if errorlevel 1 exit /b 1
)
"!CMAKE_CMD!" --build build --config %BUILD_CONFIG%
if errorlevel 1 exit /b 1
echo.

echo === Building 32-bit ===
if not exist "build32" (
    "!CMAKE_CMD!" -B build32 -G "Visual Studio 17 2022" -A Win32
    if errorlevel 1 exit /b 1
)
"!CMAKE_CMD!" --build build32 --config %BUILD_CONFIG%
if errorlevel 1 exit /b 1
echo.

echo Build successful. Output files:
echo   64-bit: build\%BUILD_CONFIG%\
echo   32-bit: build32\%BUILD_CONFIG%\
goto end

:build_x64_only
echo Building ConsoleForwarder [%BUILD_CONFIG%] [x64]...
echo.
if not exist "build" (
    "!CMAKE_CMD!" -B build -G "Visual Studio 17 2022" -A x64
    if errorlevel 1 exit /b 1
)
"!CMAKE_CMD!" --build build --config %BUILD_CONFIG%
if errorlevel 1 exit /b 1
echo.
echo Build successful. Output: build\%BUILD_CONFIG%\
goto end

:build_x86_only
echo Building ConsoleForwarder [%BUILD_CONFIG%] [x86]...
echo.
if not exist "build32" (
    "!CMAKE_CMD!" -B build32 -G "Visual Studio 17 2022" -A Win32
    if errorlevel 1 exit /b 1
)
"!CMAKE_CMD!" --build build32 --config %BUILD_CONFIG%
if errorlevel 1 exit /b 1
echo.
echo Build successful. Output: build32\%BUILD_CONFIG%\
goto end

:help
echo Usage: build.bat [command] [config] [arch]
echo.
echo Commands:
echo   build    Build the project (default)
echo   clean    Remove all build directories
echo   rebuild  Clean and build
echo   help     Show this help message
echo.
echo Config:
echo   release  Build Release configuration (default)
echo   debug    Build Debug configuration
echo.
echo Architecture:
echo   x64, 64  Build 64-bit binaries only
echo   x86, 32  Build 32-bit binaries only
echo   (none)   Build both 64-bit and 32-bit (default)
echo.
echo Examples:
echo   build.bat                    Build Release (both)
echo   build.bat debug              Build Debug (both)
echo   build.bat x64                Build Release 64-bit only
echo   build.bat x86                Build Release 32-bit only
echo   build.bat debug 32           Build Debug 32-bit only
echo   build.bat rebuild            Clean and build Release (both)
echo   build.bat rebuild debug x86  Clean and build Debug 32-bit

:end
endlocal
