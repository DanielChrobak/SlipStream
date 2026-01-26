@echo off

:: Request administrator privileges if not already elevated
net session >nul 2>&1
if errorlevel 1 (
    powershell -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b
)

:: Change to script directory
cd /d "%~dp0"

:: Check if executable exists
if not exist "build\bin\Release\SlipStream.exe" (
    echo Run build.bat first
    pause
    exit /b 1
)

:: Run the application
cd build\bin\Release
SlipStream.exe
cd ..\..\..
pause
