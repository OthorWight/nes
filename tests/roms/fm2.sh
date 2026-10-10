#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/../.."
if [ "$#" -lt 2 ] || [ "$#" -gt 3 ]; then echo 'Usage: bash tests/roms/fm2.sh ROM.nes movie.fm2 [final.ppm]' >&2; exit 2; fi
mkdir -p build/tests
core=()
for source in src/*.c; do
    case "$source" in src/gui_main.c|src/debugger.c|src/host_sokol.c) ;; *) core+=("$source");; esac
done
"${CC:-cc}" -Wall -Wextra -Werror -std=c11 -O3 -flto -Isrc tests/roms/fm2_probe.c "${core[@]}" -o build/tests/fm2_probe -pthread -lm
build/tests/fm2_probe "$@"
