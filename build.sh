#!/bin/bash
set -e

cd "$(dirname "$0")"

install_dependencies() {
    if [[ "$OSTYPE" == "linux-gnu"* ]]; then
        if [ -f /etc/arch-release ]; then
            if ! command -v gcc &> /dev/null || ! command -v pkg-config &> /dev/null || ! pkg-config --exists sdl2 &> /dev/null; then
                echo "Dependencies missing. Installing via pacman..."
                sudo pacman -Sy --needed --noconfirm base-devel sdl2 pkgconf
            fi
        elif [ -f /etc/debian_version ]; then
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
            echo "Homebrew is required. Install from https://brew.sh/"
            exit 1
        fi
        if ! command -v pkg-config &> /dev/null || ! pkg-config --exists sdl2 &> /dev/null; then
            echo "Dependencies missing. Installing via Homebrew..."
            brew install pkg-config sdl2
        fi
    fi
}

install_dependencies

echo "Compiling NES Emulator..."
mkdir -p build

SDL_CFLAGS=$(pkg-config --cflags sdl2 2>/dev/null || echo "")
SDL_LIBS=$(pkg-config --libs sdl2 2>/dev/null || echo "-lSDL2")

gcc -Wall -Wextra -std=c11 -O2 -Isrc src/*.c -o build/nes_emulator ${SDL_CFLAGS} ${SDL_LIBS} -lm

echo "Compilation successful!"
echo "Launching NES Emulator..."
echo "----------------------------------------"
(cd build && ./nes_emulator)
echo "----------------------------------------"