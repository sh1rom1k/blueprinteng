@echo off
setlocal
cd /d "%~dp0"

where cmake >nul 2>&1
if errorlevel 1 (
    echo CMake was not found on PATH.
    echo Install CMake and Visual Studio 2022 with the Desktop development with C++ workload.
    exit /b 1
)

cmake -S . -B build -A x64
if errorlevel 1 exit /b 1

cmake --build build --config Release --parallel
if errorlevel 1 exit /b 1

echo.
echo Built build\Release\blueprinteng.exe
exit /b 0
