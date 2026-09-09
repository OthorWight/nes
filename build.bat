@echo off
setlocal enabledelayedexpansion

cd /d "%~dp0"

if not "%~2"=="" goto :usage_error
set "BUILD_MODE=%~1"
if "%BUILD_MODE%"=="" set "BUILD_MODE=--run"
if /i "%BUILD_MODE%"=="--help" goto :usage
if /i "%BUILD_MODE%"=="-h" goto :usage
if /i "%BUILD_MODE%"=="--run" goto :detect_compiler
if /i "%BUILD_MODE%"=="--build-only" goto :detect_compiler
if /i "%BUILD_MODE%"=="--test" goto :detect_compiler
goto :usage_error

:detect_compiler
set "COMPILER_TYPE="

REM Check for MSVC
where cl >nul 2>nul
if %ERRORLEVEL% equ 0 (
    set "COMPILER_TYPE=msvc"
    echo Found MSVC compiler cl.exe.
    goto :detect_compiler_done
)

REM Check for GCC
where gcc >nul 2>nul
if %ERRORLEVEL% equ 0 (
    set "COMPILER_TYPE=gcc"
    echo Found GCC compiler.
    goto :detect_compiler_done
)

REM No compiler found, attempt install
echo No suitable compiler MSVC or GCC found in PATH.
if /i not "%BUILD_MODE%"=="--run" (
    echo Install a C toolchain or open a Visual Studio developer command prompt.
    exit /b 1
)
echo Attempting to install MinGW-w64 via Winget...
winget install -e --id msys2.msys2
if %ERRORLEVEL% neq 0 (
    echo.
    echo Winget failed to install MSYS2 automatically.
    echo Please try installing it manually:
    echo 1. Visit https://www.msys2.org/ to download and install MSYS2.
    echo 2. After installing, open the MSYS2 MinGW x64 terminal from your Start Menu.
    echo 3. Run the command: pacman -Syu
    echo 4. After it finishes, run the command: pacman -S --needed mingw-w64-x86_64-toolchain
    echo 5. This will install GCC. Now, add C:\msys64\mingw64\bin to your PATH.
    echo 6. RESTART this terminal and run the build script again.
    pause
    exit /b 1
)

echo MSYS2 has been successfully installed.
echo.
echo Adding C:\msys64\mingw64\bin to your user PATH...
powershell -Command "$userPath = [System.Environment]::GetEnvironmentVariable('PATH', 'User'); if (-not ($userPath -split ';').Contains('C:\msys64\mingw64\bin')) { [System.Environment]::SetEnvironmentVariable('PATH', ($userPath + ';C:\msys64\mingw64\bin'), 'User') }"
if %ERRORLEVEL% equ 0 (
    echo PATH updated successfully.
) else (
    echo Failed to update PATH automatically. Please add it manually.
)
echo Please RESTART your terminal/IDE for the new PATH to take effect.
pause
exit /b 1

:detect_compiler_done

if /i "%BUILD_MODE%"=="--test" goto :test_step

:build_step
if not exist "build" mkdir "build"
if "%COMPILER_TYPE%"=="msvc" (
    echo Compiling Sokol frontend with MSVC...
    cl /W4 /O2 /std:c11 /Isrc src\*.c /D_CRT_SECURE_NO_WARNINGS /Fo"build\\" /Febuild\nes_emulator.exe /link user32.lib gdi32.lib winmm.lib ole32.lib shell32.lib d3d11.lib dxgi.lib
    if not "!ERRORLEVEL!"=="0" goto :build_failed
) else (
    echo Compiling Sokol frontend with GCC...
    gcc -Wall -Wextra -std=c11 -O2 -Isrc src/*.c -o build/nes_emulator.exe -static -luser32 -lgdi32 -lwinmm -lole32 -lshell32 -ld3d11 -ldxgi
    if not "!ERRORLEVEL!"=="0" goto :build_failed
)
echo Compilation successful!
if /i "%BUILD_MODE%"=="--build-only" exit /b 0
echo Launching NES Emulator...
cd build
nes_emulator.exe
exit /b %ERRORLEVEL%

:build_failed
echo Compilation failed!
exit /b 1

:test_step
if not exist "build\tests" mkdir "build\tests"
set "CORE_SOURCES="
for %%F in (src\*.c) do (
    if /i not "%%~nxF"=="gui_main.c" if /i not "%%~nxF"=="debugger.c" if /i not "%%~nxF"=="host_sokol.c" (
        set "CORE_SOURCES=!CORE_SOURCES! "%%F""
    )
)
set /a TEST_COUNT=0
for %%T in (tests\*.c) do (
    if exist "%%T" (
        call :run_test "%%T"
        if not "!ERRORLEVEL!"=="0" exit /b 1
        set /a TEST_COUNT+=1
    )
)
if %TEST_COUNT% equ 0 (
    echo Error: no C tests found in tests/.
    exit /b 1
)
echo All %TEST_COUNT% tests passed.
exit /b 0

:run_test
set "TEST_NAME=%~n1"
set "TEST_EXE=build\tests\%~n1.exe"
echo Building test: %TEST_NAME%
if "%COMPILER_TYPE%"=="msvc" (
    cl /nologo /W4 /O2 /std:c11 /Isrc /D_CRT_SECURE_NO_WARNINGS "%~1" !CORE_SOURCES! /Fo"build\tests\\" /Fe"%TEST_EXE%"
    if not "!ERRORLEVEL!"=="0" goto :test_compile_failed
) else (
    gcc -Wall -Wextra -Werror -std=c11 -O2 -Isrc "%~1" !CORE_SOURCES! -o "%TEST_EXE%" -lm
    if not "!ERRORLEVEL!"=="0" goto :test_compile_failed
)
echo Running test: %TEST_NAME%
"%TEST_EXE%"
if not "%ERRORLEVEL%"=="0" (
    echo FAIL: %TEST_NAME%
    exit /b 1
)
echo PASS: %TEST_NAME%
exit /b 0

:test_compile_failed
echo FAIL: %TEST_NAME% compilation
exit /b 1

:usage_error
call :usage
exit /b 2

:usage
echo Usage: build.bat [--run ^| --build-only ^| --test ^| --help]
echo   --run         Build and launch ^(default^); install missing dependencies.
echo   --build-only  Build without launching or installing dependencies.
echo   --test        Build and run core tests; no graphics, ROMs, or GUI required.
exit /b 0
