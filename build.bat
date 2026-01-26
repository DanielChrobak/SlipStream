@echo off
setlocal

echo.
echo ========== SlipStream Build ==========
echo.

for %%p in ("%VCPKG_ROOT%" "C:\vcpkg" "%USERPROFILE%\vcpkg" "vcpkg" "..\vcpkg") do if exist "%%~p\vcpkg.exe" (set VCPKG=%%~p& goto :found)
echo [ERROR] vcpkg not found! Install to C:\vcpkg or set VCPKG_ROOT
pause & exit /b 1

:found
echo Using vcpkg: %VCPKG%
echo.

echo [1/3] Installing dependencies...
"%VCPKG%\vcpkg.exe" install --triplet x64-windows || (echo [ERROR] vcpkg failed & pause & exit /b 1)

echo.
echo [2/3] Configuring CMake...
if exist build rmdir /s /q build
mkdir build || (echo [ERROR] mkdir failed & pause & exit /b 1)

cd build
cmake .. -G "Visual Studio 17 2022" -A x64 -DCMAKE_TOOLCHAIN_FILE="%VCPKG%/scripts/buildsystems/vcpkg.cmake" || (cd .. & echo [ERROR] CMake failed & pause & exit /b 1)

echo.
echo [3/3] Building Release...
cmake --build . --config Release --verbose || (cd .. & echo [ERROR] Build failed & pause & exit /b 1)

cd ..

echo.
echo ========== Build Complete ==========
if exist "build\bin\Release\SlipStream.exe" (echo Output: build\bin\Release\SlipStream.exe) else (dir /s /b build\*.exe 2>nul)
echo.
pause
