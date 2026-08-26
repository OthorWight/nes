@echo off
setlocal enabledelayedexpansion

cd /d "%~dp0"

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

set "SDL_DIR=SDL2"
if exist "%SDL_DIR%" goto :build_step

if "%COMPILER_TYPE%"=="msvc" (
    echo SDL2 development libraries not found. Downloading for MSVC...
    powershell -Command "[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12; Invoke-WebRequest -Uri 'https://github.com/libsdl-org/SDL/releases/download/release-2.30.8/SDL2-devel-2.30.8-VC.zip' -OutFile 'sdl2.zip'"
) else (
    echo SDL2 development libraries not found. Downloading for MinGW...
    powershell -Command "[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12; Invoke-WebRequest -Uri 'https://github.com/libsdl-org/SDL/releases/download/release-2.30.8/SDL2-devel-2.30.8-mingw.zip' -OutFile 'sdl2.zip'"
)

echo Extracting SDL2...
powershell -Command "Expand-Archive -Path 'sdl2.zip' -DestinationPath 'temp_sdl'"
for /d %%i in (temp_sdl\SDL2-*) do (
    xcopy /E /I "%%i" "%SDL_DIR%" >nul
)
del /q sdl2.zip
rmdir /s /q temp_sdl
echo SDL2 library has been set up.

:build_step
if not exist "build" mkdir "build"

if "%COMPILER_TYPE%"=="msvc" (
    echo Compiling with MSVC...
    set "SDL_INCLUDE_DIR=%SDL_DIR%\include"
    set "SDL_LIB_DIR=%SDL_DIR%\lib\x64"

    cl /W4 /O2 /Isrc /I"%SDL_INCLUDE_DIR%" src\*.c /D_CRT_SECURE_NO_WARNINGS /Febuild\nes_emulator.exe /link /LIBPATH:"%SDL_LIB_DIR%" SDL2.lib SDL2main.lib user32.lib gdi32.lib winmm.lib imm32.lib ole32.lib oleaut32.lib shell32.lib setupapi.lib version.lib uuid.lib
)

if "%COMPILER_TYPE%"=="gcc" (
    echo Compiling with GCC...
    set "SDL_ARCH_DIR=%SDL_DIR%\x86_64-w64-mingw32"
    if not exist "!SDL_ARCH_DIR!" (
        set "SDL_ARCH_DIR=%SDL_DIR%\i686-w64-mingw32"
    )

    gcc -Wall -Wextra -std=c11 -O2 -Isrc -I"!SDL_ARCH_DIR!\include" src/*.c -o build/nes_emulator.exe -L"!SDL_ARCH_DIR!\lib" -static -lmingw32 -lSDL2main -lSDL2 -luser32 -lgdi32 -lwinmm -limm32 -lole32 -loleaut32 -lshell32 -lsetupapi -lversion -luuid
)

if %ERRORLEVEL% equ 0 (
    echo Compilation successful!
    echo Launching NES Emulator...
    echo ----------------------------------------
    cd build
    nes_emulator.exe
    echo ----------------------------------------
) else (
    echo Compilation failed!
)
pause