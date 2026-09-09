#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"
usage() {
    echo "Usage: ./build.sh [--run | --build-only | --test | --help]"
    echo "  --run         Build and launch (default)."
    echo "  --build-only  Build without launching."
    echo "  --test        Build and run core tests; no display, audio, or ROMs required."
}
if [ "$#" -gt 1 ]; then usage >&2; exit 2; fi
build_mode=${1:---run}
case "$build_mode" in
    --run|--build-only|--test) ;;
    --help|-h) usage; exit 0 ;;
    *) usage >&2; exit 2 ;;
esac
compiler=${CC:-cc}
if ! command -v "$compiler" >/dev/null 2>&1; then
    echo "Error: install a C compiler (Xcode command line tools on macOS)." >&2
    exit 1
fi
if [ "$build_mode" = --test ]; then
    mkdir -p build/tests
    core_sources=()
    for source in src/*.c; do
        case "$source" in src/gui_main.c|src/debugger.c|src/host_sokol.c) ;; *) core_sources+=("$source");; esac
    done
    test_count=0
    for test_source in tests/*.c; do
        [ -f "$test_source" ] || continue
        test_name=$(basename "$test_source" .c)
        echo "Building test: $test_name"
        "$compiler" -Wall -Wextra -Werror -std=c11 -O2 -Isrc "$test_source" "${core_sources[@]}" -o "build/tests/$test_name" -lm
        echo "Running test: $test_name"
        "build/tests/$test_name"
        echo "PASS: $test_name"
        test_count=$((test_count + 1))
    done
    if [ "$test_count" -eq 0 ]; then echo "Error: no C tests found." >&2; exit 1; fi
    echo "All $test_count tests passed."
    exit 0
fi
source scripts/sokol-build.sh
mkdir -p build
echo "Compiling NES Emulator (Sokol)..."
sokol_build build/nes_emulator src/*.c
echo "Compilation successful!"
if [ "$build_mode" = --run ]; then (cd build && ./nes_emulator); fi
