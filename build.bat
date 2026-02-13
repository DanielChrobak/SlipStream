@echo off
setlocal
echo.
echo ========== SlipStream Build ==========
echo.

set "VCPKG="
for %%p in ("%VCPKG_ROOT%" "C:\vcpkg" "%USERPROFILE%\vcpkg" "vcpkg" "..\vcpkg") do (
    if exist "%%~p\vcpkg.exe" set "VCPKG=%%~p" & goto :run
)
echo [ERROR] vcpkg not found! Install to C:\vcpkg or set VCPKG_ROOT & pause & exit /b 1

:run
echo Using vcpkg: %VCPKG%
echo.
echo [1/3] Installing dependencies...
"%VCPKG%\vcpkg.exe" install --triplet x64-windows || goto :err

echo.
echo [2/3] Configuring CMake...
if exist build rmdir /s /q build
mkdir build && cd build || goto :err
cmake .. -A x64 -DCMAKE_TOOLCHAIN_FILE="%VCPKG%/scripts/buildsystems/vcpkg.cmake" || (cd .. & goto :err)

echo.
echo [3/3] Building Release...
cmake --build . --config Release --verbose || (cd .. & goto :err)
cd ..

echo.
echo ========== Build Complete ==========
if exist "build\bin\Release\SlipStream.exe" (echo Output: build\bin\Release\SlipStream.exe) else (dir /s /b build\*.exe 2>nul)
echo.
pause
exit /b 0

:err
echo [ERROR] Build failed & pause & exit /b 1
