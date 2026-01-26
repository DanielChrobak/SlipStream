@echo off
setlocal

REM SlipStream - Build Script (Verbose)
REM Copyright 2025-2026 Daniel Chrobak

echo.
echo ============================================
echo          SlipStream Build Script
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
echo ============================================
echo [1/3] Installing Dependencies via vcpkg
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
echo [2/3] Configuring with CMake
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
echo [3/3] Building Release Configuration
echo ============================================
echo.

echo [INFO] Starting build process...
echo [INFO] Configuration: Release
echo.

cmake --build . --config Release --verbose || goto :build_failed

cd ..

echo.
echo ============================================
echo              Build Complete!
echo ============================================
echo.
echo Output: build\bin\Release\SlipStream.exe
echo.

if exist "build\bin\Release\SlipStream.exe" (
    echo [SUCCESS] Executable created successfully
    for %%F in ("build\bin\Release\SlipStream.exe") do echo [INFO] Size: %%~zF bytes
) else (
    echo [WARNING] Executable not found at expected location
    echo [INFO] Searching for output...
    dir /s /b build\*.exe 2>nul
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
