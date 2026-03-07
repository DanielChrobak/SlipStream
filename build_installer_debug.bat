@echo off
setlocal
echo.
echo SlipStream Debug Installer Build
echo.
set "VCPKG="
for %%p in ("%VCPKG_ROOT%" "C:\vcpkg" "%USERPROFILE%\vcpkg" "vcpkg" "..\vcpkg") do (
    if exist "%%~p\vcpkg.exe" set "VCPKG=%%~p" & goto :found_vcpkg
)
echo [ERROR] vcpkg not found! Install to C:\vcpkg or set VCPKG_ROOT
pause
exit /b 1
:found_vcpkg
set "ISCC="
for %%p in ("%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe" "%ProgramFiles%\Inno Setup 6\ISCC.exe") do (
    if exist "%%~p" set "ISCC=%%~p" & goto :found_inno
)
echo [ERROR] Inno Setup 6 not found!
pause
exit /b 1
:found_inno
"%VCPKG%\vcpkg.exe" install --triplet x64-windows || goto :err
if exist build rmdir /s /q build
mkdir build && cd build || goto :err
cmake .. -A x64 -DCMAKE_TOOLCHAIN_FILE="%VCPKG%/scripts/buildsystems/vcpkg.cmake" || (cd .. & goto :err)
cmake --build . --config Debug || (cd .. & goto :err)
cd ..
"%ISCC%" /Q /DBuildConfig=Debug /DOutputSuffix=Debug /DRunArgs=--debug SlipStream.iss || goto :err
dir /b build\SlipStream-*-Debug-Setup.exe 2>nul
pause
exit /b 0
:err
echo [ERROR] Build failed!
pause
exit /b 1

