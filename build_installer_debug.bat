@echo off
setlocal
echo.
echo ========================================
echo    SlipStream Debug Installer Build
echo ========================================
echo.

:: Find vcpkg
set "VCPKG="
for %%p in ("%VCPKG_ROOT%" "C:\vcpkg" "%USERPROFILE%\vcpkg" "vcpkg" "..\vcpkg") do (
    if exist "%%~p\vcpkg.exe" set "VCPKG=%%~p" & goto :found_vcpkg
)
echo [ERROR] vcpkg not found! Install to C:\vcpkg or set VCPKG_ROOT
pause & exit /b 1

:found_vcpkg
echo [OK] vcpkg: %VCPKG%

:: Find Inno Setup
set "ISCC="
for %%p in ("%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe" "%ProgramFiles%\Inno Setup 6\ISCC.exe") do (
    if exist "%%~p" set "ISCC=%%~p" & goto :found_inno
)
echo [ERROR] Inno Setup 6 not found!
echo         Download from: https://jrsoftware.org/isdl.php
pause & exit /b 1

:found_inno
echo [OK] Inno Setup: %ISCC%
echo.

echo [1/4] Installing dependencies...
"%VCPKG%\vcpkg.exe" install --triplet x64-windows || goto :err

echo.
echo [2/4] Configuring CMake...
if exist build rmdir /s /q build
mkdir build && cd build || goto :err
cmake .. -A x64 -DCMAKE_TOOLCHAIN_FILE="%VCPKG%/scripts/buildsystems/vcpkg.cmake" || (cd .. & goto :err)

echo.
echo [3/4] Building Debug...
cmake --build . --config Debug || (cd .. & goto :err)
cd ..

echo.
echo [4/4] Creating debug installer...
"%ISCC%" /Q /DBuildConfig=Debug /DOutputSuffix=Debug /DRunArgs=--debug SlipStream.iss || goto :err

echo.
echo ========================================
echo        Debug Build Complete!
echo ========================================
dir /b build\SlipStream-*-Debug-Setup.exe 2>nul
echo.
pause
exit /b 0

:err
echo.
echo [ERROR] Build failed!
pause
exit /b 1
