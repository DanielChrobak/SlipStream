@echo off
setlocal

REM SlipStream - Installer Build Script (Verbose)
REM Copyright 2025-2026 Daniel Chrobak

echo.
echo ============================================
echo      SlipStream Installer Build Script
echo ============================================
echo.

REM Find vcpkg installation
set VCPKG=
echo [INFO] Searching for vcpkg installation...

for %%p in ("%VCPKG_ROOT%" "C:\vcpkg" "%USERPROFILE%\vcpkg" "vcpkg" "..\vcpkg") do (
    if exist "%%~p\vcpkg.exe" (
        set VCPKG=%%~p
        echo [INFO] Found vcpkg at: %%~p
        goto :found
    )
)

echo.
echo [ERROR] vcpkg not found!
echo.
echo Please do one of the following:
echo   1. Set VCPKG_ROOT environment variable to your vcpkg installation
echo   2. Install vcpkg to C:\vcpkg
echo   3. Clone vcpkg to the parent directory (..\vcpkg)
echo.
echo To install vcpkg:
echo   git clone https://github.com/microsoft/vcpkg.git C:\vcpkg
echo   cd C:\vcpkg
echo   .\bootstrap-vcpkg.bat
echo   .\vcpkg integrate install
echo.
pause
exit /b 1

:found
echo.

REM Check for NSIS
set USE_NSIS=0
echo [INFO] Checking for NSIS installer...

where makensis >nul 2>&1 && set USE_NSIS=1 && echo [INFO] Found NSIS in PATH && goto :nsis_done
if exist "C:\Program Files (x86)\NSIS\makensis.exe" set USE_NSIS=1 && echo [INFO] Found NSIS at: C:\Program Files (x86)\NSIS && goto :nsis_done
if exist "C:\Program Files\NSIS\makensis.exe" set USE_NSIS=1 && echo [INFO] Found NSIS at: C:\Program Files\NSIS && goto :nsis_done

echo [INFO] NSIS not found - will create portable ZIP package
echo [INFO] Install NSIS for a proper installer: https://nsis.sourceforge.io

:nsis_done
echo.

if "%USE_NSIS%"=="1" (
    echo [INFO] Package type: NSIS Installer (.exe)
) else (
    echo [INFO] Package type: Portable ZIP (.zip)
)

echo.
echo ============================================
echo [1/4] Installing Dependencies via vcpkg
echo ============================================
echo.
echo [INFO] Using vcpkg at: %VCPKG%
echo [INFO] Triplet: x64-windows
echo [INFO] This may take several minutes on first run...
echo.

"%VCPKG%\vcpkg.exe" install --triplet x64-windows || goto :vcpkg_failed

echo.
echo [SUCCESS] Dependencies installed successfully
echo.

echo ============================================
echo [2/4] Configuring with CMake
echo ============================================
echo.

if exist build (
    echo [INFO] Removing existing build directory...
    rmdir /s /q build
)

echo [INFO] Creating build directory...
mkdir build || goto :mkdir_failed

cd build

echo [INFO] Running CMake configuration...
echo [INFO] Generator: Visual Studio 17 2022
echo [INFO] Architecture: x64
echo [INFO] Toolchain: %VCPKG%\scripts\buildsystems\vcpkg.cmake
echo.

cmake .. -G "Visual Studio 17 2022" -A x64 -DCMAKE_TOOLCHAIN_FILE="%VCPKG%/scripts/buildsystems/vcpkg.cmake" || goto :cmake_failed

echo.
echo [SUCCESS] CMake configuration completed
echo.

echo ============================================
echo [3/4] Building Release Configuration
echo ============================================
echo.

echo [INFO] Starting build process...
echo [INFO] Configuration: Release
echo.

cmake --build . --config Release --verbose || goto :build_failed

echo.
echo [SUCCESS] Build completed successfully
echo.

echo ============================================
echo [4/4] Creating Installation Package
echo ============================================
echo.

if "%USE_NSIS%"=="1" (
    echo [INFO] Creating NSIS installer package...
    echo.
    cpack -G NSIS -C Release --verbose
    if errorlevel 1 (
        echo.
        echo [WARNING] NSIS packaging failed, falling back to ZIP...
        echo.
        cpack -G ZIP -C Release --verbose || goto :pack_failed
    )
) else (
    echo [INFO] Creating ZIP package...
    echo.
    cpack -G ZIP -C Release --verbose || goto :pack_failed
)

cd ..

echo.
echo ============================================
echo           Installer Build Complete!
echo ============================================
echo.
echo [INFO] Searching for created packages...
echo.

if exist "build\SlipStream-1.0.0-win64.exe" (
    echo [SUCCESS] NSIS Installer created:
    echo          build\SlipStream-1.0.0-win64.exe
    for %%F in ("build\SlipStream-1.0.0-win64.exe") do echo          Size: %%~zF bytes
)

if exist "build\SlipStream-1.0.0-win64.zip" (
    echo [SUCCESS] ZIP Package created:
    echo          build\SlipStream-1.0.0-win64.zip
    for %%F in ("build\SlipStream-1.0.0-win64.zip") do echo          Size: %%~zF bytes
)

if not exist "build\SlipStream-1.0.0-win64.exe" if not exist "build\SlipStream-1.0.0-win64.zip" (
    echo [INFO] Listing all packages in build directory:
    echo.
    dir /b build\SlipStream-* 2>nul
)

echo.
pause
exit /b 0

REM ============================================
REM Error Handlers
REM ============================================

:vcpkg_failed
echo.
echo [ERROR] vcpkg dependency installation failed!
echo.
echo Common issues:
echo   - Network connectivity problems
echo   - Insufficient disk space
echo   - Missing build tools (Visual Studio)
echo.
pause
exit /b 1

:mkdir_failed
echo [ERROR] Failed to create build directory!
pause
exit /b 1

:cmake_failed
echo.
echo [ERROR] CMake configuration failed!
echo.
echo Common issues:
echo   - Visual Studio 2022 not installed
echo   - Missing C++ Desktop Development workload
echo   - CMakeLists.txt syntax errors
echo   - Missing dependencies
echo.
cd ..
pause
exit /b 1

:build_failed
echo.
echo [ERROR] Build failed!
echo.
echo Check the error messages above for details.
echo Common issues:
echo   - Compiler errors in source code
echo   - Missing header files
echo   - Linker errors (missing libraries)
echo.
cd ..
pause
exit /b 1

:pack_failed
echo.
echo [ERROR] Packaging failed!
echo.
echo Check the error messages above for details.
echo Common issues:
echo   - NSIS not properly installed
echo   - Missing files for packaging
echo   - Insufficient permissions
echo.
cd ..
pause
exit /b 1
