#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/../.."
mkdir -p build/tests
sources=()
for source in src/*.c; do
    case "$source" in src/gui_main.c|src/debugger.c|src/host_sokol.c) ;; *) sources+=("$source");; esac
done
gcc -Wall -Wextra -Werror -std=c11 -O3 -flto -Isrc -Itests \
    tests/performance/frame_benchmark.c "${sources[@]}" -o build/tests/frame_benchmark -pthread -lm
build/tests/frame_benchmark "$@"
