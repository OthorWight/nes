@echo off
setlocal enabledelayedexpansion

:: Navigate to the script's directory to ensure relative paths work
cd /d "%~dp0"

:: Check for GCC
where gcc >nul 2>nul
if %ERRORLEVEL% neq 0 (
    echo GCC is not installed or not in your PATH.
    echo Attempting to install MinGW-w64 via Winget...
    winget install -e --id msys2.msys2 --silent
    echo MSYS2 has been successfully installed!
    echo Please restart your terminal/IDE to refresh PATH, or manually add "C:\msys64\mingw64\bin" to your environment variables.
    pause
    exit /b 1
)

:: Check or download SDL2
set "SDL_DIR=SDL2"
if not exist "%SDL_DIR%" (
    echo SDL2 development libraries not found locally. Downloading SDL2 MinGW development library...
    powershell -Command "[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12; Invoke-WebRequest -Uri 'https://github.com/libsdl-org/SDL/releases/download/release-2.30.8/SDL2-devel-2.30.8-mingw.zip' -OutFile 'sdl2.zip'"
    echo Extracting SDL2...
    powershell -Command "Expand-Archive -Path 'sdl2.zip' -DestinationPath 'temp_sdl'"
    for /d %%i in (temp_sdl\SDL2-*) do (
        xcopy /E /I "%%i" "%SDL_DIR%" >nul
    )
    del /q sdl2.zip
    rmdir /s /q temp_sdl
    echo SDL2 library has been set up successfully.
)

:: Determine architecture directory
set "SDL_ARCH_DIR=%SDL_DIR%\x86_64-w64-mingw32"
if not exist "%SDL_ARCH_DIR%" (
    set "SDL_ARCH_DIR=%SDL_DIR%\i686-w64-mingw32"
)

if not exist "build" mkdir "build"

echo Compiling 6502 CPU Emulator Core and GUI for Windows...
gcc -Wall -Wextra -std=c11 -O2 -Isrc -I"%SDL_ARCH_DIR%\include" src/cpu6502.c src/cartridge.c src/ppu2c02.c src/apu2a03.c src/gui_main.c -o build/nes_emulator.exe -L"%SDL_ARCH_DIR%\lib" -static -lmingw32 -lSDL2main -lSDL2 -luser32 -lgdi32 -lwinmm -limm32 -lole32 -loleaut32 -lshell32 -lsetupapi -lversion -luuid

if %ERRORLEVEL% equ 0 (
    echo Compilation successful!
    echo Launching NES Emulator...
    cd build
    nes_emulator.exe
) else (
    echo Compilation failed!
)
pause