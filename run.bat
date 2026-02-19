@echo off
net session >nul 2>&1 || (powershell -Command "Start-Process -FilePath '%~f0' -Verb RunAs" & exit /b)
cd /d "%~dp0"
if not exist "build\bin\Release\SlipStream.exe" (echo Run build.bat first & pause & exit /b 1)
cd build\bin\Release && SlipStream.exe
echo.
echo [SlipStream exited with code %errorlevel%]
pause
