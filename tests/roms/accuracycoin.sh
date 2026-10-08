#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/../.."
mkdir -p build/tests
sources=()
for source in src/*.c; do
    case "$source" in src/gui_main.c|src/debugger.c|src/host_sokol.c) ;; *) sources+=("$source");; esac
done
"${CC:-cc}" -Wall -Wextra -Werror -std=c11 -O2 -Isrc \
    tests/roms/accuracycoin_runner.c "${sources[@]}" -o build/tests/accuracycoin_runner -lm
if (( $# == 0 )); then set -- build/AccuracyCoin.nes; fi
exec build/tests/accuracycoin_runner "$@"
