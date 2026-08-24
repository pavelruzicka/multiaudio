@echo off
rem Build multiaudio.exe with the Visual Studio compiler.
rem Run this from an "x64 Native Tools Command Prompt for VS".
setlocal

where cl.exe >nul 2>nul
if errorlevel 1 (
    echo cl.exe was not found.
    echo.
    echo Open "x64 Native Tools Command Prompt for VS" and run build.bat there,
    echo or build with CMake:
    echo     cmake -B build
    echo     cmake --build build --config Release
    exit /b 1
)

if not exist build mkdir build

cl /nologo /std:c++17 /EHsc /O2 /W3 /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS ^
   /Fe:build\multiaudio.exe /Fo:build\ ^
   src\main.cpp src\tray.cpp src\mirror.cpp src\devices.cpp src\audio.cpp ^
   src\settings.cpp src\install.cpp src\util.cpp ^
   /link /SUBSYSTEM:WINDOWS ole32.lib avrt.lib shell32.lib user32.lib gdi32.lib advapi32.lib
if errorlevel 1 exit /b 1

echo.
echo Built build\multiaudio.exe
