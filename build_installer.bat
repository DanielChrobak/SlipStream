@echo off
setlocal
echo.
echo ========== SlipStream Installer Build ==========
echo.

set "VCPKG="
for %%p in ("%VCPKG_ROOT%" "C:\vcpkg" "%USERPROFILE%\vcpkg" "vcpkg" "..\vcpkg") do (
    if exist "%%~p\vcpkg.exe" set "VCPKG=%%~p" & goto :run
)
echo [ERROR] vcpkg not found! & pause & exit /b 1

:run
echo Using vcpkg: %VCPKG%
echo.

set USE_NSIS=0
where makensis >nul 2>&1 && set USE_NSIS=1
if exist "C:\Program Files (x86)\NSIS\makensis.exe" set USE_NSIS=1
if exist "C:\Program Files\NSIS\makensis.exe" set USE_NSIS=1
echo NSIS %USE_NSIS%
echo.

echo [1/4] Installing dependencies...
"%VCPKG%\vcpkg.exe" install --triplet x64-windows || goto :err

echo.
echo [2/4] Configuring CMake...
if exist build rmdir /s /q build
mkdir build && cd build || goto :err
cmake .. -G "Visual Studio 17 2022" -A x64 -DCMAKE_TOOLCHAIN_FILE="%VCPKG%/scripts/buildsystems/vcpkg.cmake" || (cd .. & goto :err)

echo.
echo [3/4] Building Release...
cmake --build . --config Release --verbose || (cd .. & goto :err)

echo.
echo [4/4] Creating package...
if "%USE_NSIS%"=="1" (cpack -G NSIS -C Release --verbose || cpack -G ZIP -C Release --verbose) else (cpack -G ZIP -C Release --verbose)
if errorlevel 1 (cd .. & goto :err)
cd ..

echo.
echo ========== Installer Build Complete ==========
for %%f in ("build\SlipStream-1.0.0-win64.exe" "build\SlipStream-1.0.0-win64.zip") do if exist %%f echo Created: %%f
echo.
pause
exit /b 0

:err
echo [ERROR] Build failed & pause & exit /b 1
