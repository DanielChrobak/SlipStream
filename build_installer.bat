@echo off
setlocal

echo.
echo ========== SlipStream Installer Build ==========
echo.

:: Find vcpkg installation
for %%p in ("%VCPKG_ROOT%" "C:\vcpkg" "%USERPROFILE%\vcpkg" "vcpkg" "..\vcpkg") do (
    if exist "%%~p\vcpkg.exe" (
        set VCPKG=%%~p
        goto :found
    )
)

echo [ERROR] vcpkg not found! Install to C:\vcpkg or set VCPKG_ROOT
pause
exit /b 1

:found
echo Using vcpkg: %VCPKG%
echo.

:: Check for NSIS
set USE_NSIS=0
where makensis >nul 2>&1 && set USE_NSIS=1
if exist "C:\Program Files (x86)\NSIS\makensis.exe" set USE_NSIS=1
if exist "C:\Program Files\NSIS\makensis.exe" set USE_NSIS=1

if "%USE_NSIS%"=="1" (
    echo NSIS found - will create installer
) else (
    echo NSIS not found - will create ZIP package
)
echo.

:: Step 1: Install dependencies
echo [1/4] Installing dependencies...
"%VCPKG%\vcpkg.exe" install --triplet x64-windows
if errorlevel 1 (
    echo [ERROR] vcpkg failed
    pause
    exit /b 1
)

:: Step 2: Configure CMake
echo.
echo [2/4] Configuring CMake...
if exist build rmdir /s /q build
mkdir build
if errorlevel 1 (
    echo [ERROR] mkdir failed
    pause
    exit /b 1
)

cd build
cmake .. -G "Visual Studio 17 2022" -A x64 -DCMAKE_TOOLCHAIN_FILE="%VCPKG%/scripts/buildsystems/vcpkg.cmake"
if errorlevel 1 (
    cd ..
    echo [ERROR] CMake failed
    pause
    exit /b 1
)

:: Step 3: Build Release
echo.
echo [3/4] Building Release...
cmake --build . --config Release --verbose
if errorlevel 1 (
    cd ..
    echo [ERROR] Build failed
    pause
    exit /b 1
)

:: Step 4: Create package
echo.
echo [4/4] Creating package...
if "%USE_NSIS%"=="1" (
    cpack -G NSIS -C Release --verbose
    if errorlevel 1 (
        echo NSIS failed, falling back to ZIP...
        cpack -G ZIP -C Release --verbose
    )
) else (
    cpack -G ZIP -C Release --verbose
)

if errorlevel 1 (
    cd ..
    echo [ERROR] Packaging failed
    pause
    exit /b 1
)

cd ..

:: Done
echo.
echo ========== Installer Build Complete ==========
for %%f in ("build\SlipStream-1.0.0-win64.exe" "build\SlipStream-1.0.0-win64.zip") do (
    if exist %%f echo Created: %%f
)
echo.
pause
