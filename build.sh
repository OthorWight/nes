#!/bin/bash
set -e

# Navigate to the script's directory to ensure relative paths work
cd "$(dirname "$0")"

# Function to check and install missing dependencies
install_dependencies() {
    if [[ "$OSTYPE" == "linux-gnu"* ]]; then
        if [ -f /etc/debian_version ]; then
            if ! command -v gcc &> /dev/null || ! command -v pkg-config &> /dev/null || ! pkg-config --exists sdl2 &> /dev/null; then
                echo "Dependencies missing. Installing via apt-get..."
                sudo apt-get update
                sudo apt-get install -y build-essential pkg-config libsdl2-dev
            fi
        elif [ -f /etc/redhat-release ]; then
            if ! command -v gcc &> /dev/null || ! command -v pkg-config &> /dev/null || ! pkg-config --exists sdl2 &> /dev/null; then
                echo "Dependencies missing. Installing via dnf..."
                sudo dnf groupinstall -y "Development Tools"
                sudo dnf install -y SDL2-devel pkgconf-pkg-config
            fi
        fi
    elif [[ "$OSTYPE" == "darwin"* ]]; then
        if ! command -v brew &> /dev/null; then
            echo "Homebrew is required to install macOS dependencies. Please install it from https://brew.sh/"
            exit 1
        fi
        if ! command -v gcc &> /dev/null && ! command -v clang &> /dev/null || ! command -v pkg-config &> /dev/null || ! pkg-config --exists sdl2 &> /dev/null; then
            echo "Dependencies missing. Installing via Homebrew..."
            brew install pkg-config sdl2
        fi
    fi
}

install_dependencies

echo "Compiling 6502 CPU Emulator Core and Test Runner..."
mkdir -p build
gcc -Wall -Wextra -std=c11 -O2 -Isrc src/cpu6502.c src/cartridge.c src/ppu2c02.c src/apu2a03.c src/gui_main.c -o build/nes_emulator $(pkg-config --cflags --libs sdl2 || echo "-lSDL2")

echo "Compilation successful!"
echo "Launching NES Emulator..."
echo "----------------------------------------"
cd build
./nes_emulator
echo "----------------------------------------"