#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/../.."
mkdir -p build/tests
sources=()
for source in src/*.c; do
    case "$source" in src/gui_main.c|src/debugger.c|src/host_sokol.c) ;; *) sources+=("$source");; esac
done
gcc -Wall -Wextra -Werror -std=c11 -O2 -Isrc \
    tests/roms/blargg_runner.c "${sources[@]}" -o build/tests/blargg_runner -lm
build/tests/blargg_runner "$@"
