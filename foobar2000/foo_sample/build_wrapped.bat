@echo off
setlocal enabledelayedexpansion

:: 1. Find the latest Visual Studio installation
set "VS_PATH="
if exist "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" set "VS_PATH=C:\Program Files\Microsoft Visual Studio\2022\Community"
if exist "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" set "VS_PATH=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
if exist "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" set "VS_PATH=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools"

if "%VS_PATH%"=="" (
    echo [ERROR] Could not find Visual Studio Build Tools or Community!
    echo Please ensure "Desktop development with C++" is installed.
    exit /b 1
)

echo [INFO] Found VS at: %VS_PATH%

:: 2. Initialize the environment for x64
echo [INFO] Initializing environment...
call "%VS_PATH%\VC\Auxiliary\Build\vcvarsall.bat" x64

:: 3. Run the build
echo [INFO] Starting build...
msbuild foo_sample.sln /p:Configuration=Release /p:Platform=x64 /t:foo_wrapped

if %ERRORLEVEL% EQU 0 (
    echo.
    echo [SUCCESS] Build complete! 
    echo Find your DLL here: c:\Users\cyber\Downloads\PROJECTS\foobar2000\foobar2000\Release\foo_wrapped.dll
) else (
    echo.
    echo [ERROR] Build failed.
)
