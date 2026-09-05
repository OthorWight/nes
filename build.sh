#!/bin/bash
set -e

cd "$(dirname "$0")"

usage() {
    echo "Usage: ./build.sh [--run | --build-only | --test | --help]"
    echo "  --run         Build and launch (default); install missing dependencies."
    echo "  --build-only  Build without launching or installing dependencies."
    echo "  --test        Build and run core tests; no SDL, ROMs, or GUI required."
}

if [ "$#" -gt 1 ]; then
    usage >&2
    exit 2
fi
build_mode=${1:---run}
case "$build_mode" in
    --run|--build-only|--test) ;;
    --help|-h) usage; exit 0 ;;
    *) usage >&2; exit 2 ;;
esac

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

if [ "$build_mode" = "--run" ]; then
    install_dependencies
fi

if ! command -v gcc >/dev/null 2>&1; then
    echo "Error: gcc is required. Install a C toolchain before running this command." >&2
    exit 1
fi

if [ "$build_mode" = "--test" ]; then
    mkdir -p build/tests
    core_sources=()
    for source in src/*.c; do
        case "$source" in
            src/gui_main.c|src/debugger.c) ;;
            *) core_sources+=("$source") ;;
        esac
    done
    test_count=0
    for test_source in tests/*.c; do
        [ -f "$test_source" ] || continue
        test_name=$(basename "$test_source" .c)
        test_executable="build/tests/$test_name"
        echo "Building test: $test_name"
        if ! gcc -Wall -Wextra -Werror -std=c11 -O2 -Isrc \
            "$test_source" "${core_sources[@]}" -o "$test_executable" -lm; then
            echo "FAIL: $test_name (compilation)" >&2
            exit 1
        fi
        echo "Running test: $test_name"
        if ! "$test_executable"; then
            echo "FAIL: $test_name" >&2
            exit 1
        fi
        echo "PASS: $test_name"
        test_count=$((test_count + 1))
    done
    if [ "$test_count" -eq 0 ]; then
        echo "Error: no C tests found in tests/." >&2
        exit 1
    fi
    echo "All $test_count tests passed."
    exit 0
fi

if ! command -v pkg-config >/dev/null 2>&1 || ! pkg-config --exists sdl2; then
    echo "Error: pkg-config and SDL2 development libraries are required." >&2
    echo "Install them first, or use ./build.sh --run for dependency setup." >&2
    exit 1
fi

echo "Compiling NES Emulator..."
mkdir -p build

SDL_CFLAGS=$(pkg-config --cflags sdl2)
SDL_LIBS=$(pkg-config --libs sdl2)

gcc -Wall -Wextra -std=c11 -O2 -Isrc src/*.c -o build/nes_emulator ${SDL_CFLAGS} ${SDL_LIBS} -lm

echo "Compilation successful!"
if [ "$build_mode" = "--build-only" ]; then
    exit 0
fi
echo "Launching NES Emulator..."
echo "----------------------------------------"
(cd build && ./nes_emulator)
echo "----------------------------------------"
