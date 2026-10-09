#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/../.."
if [ "$#" -gt 2 ]; then echo 'Usage: bash tests/sokol/run.sh [ROM-path [pacing-frames]]' >&2; exit 2; fi
rom_path=''
if [ "$#" -gt 0 ]; then rom_path="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"; fi
mkdir -p build/tests
test_dir=$(mktemp -d "$PWD/build/tests/sokol-XXXXXX")
source scripts/sokol-build.sh
sources=()
core=()
for source in src/*.c; do
    case "$source" in src/gui_main.c) ;; *) sources+=("$source");; esac
    case "$source" in src/gui_main.c|src/debugger.c|src/host_sokol.c) ;; *) core+=("$source");; esac
done
for name in frontend_integration menu_mouse; do
    sokol_build "$test_dir/$name" "tests/sokol/$name.c" "${sources[@]}"
done
if [ -n "$rom_path" ]; then sokol_build "$test_dir/pacing" tests/sokol/pacing.c "${sources[@]}"; fi
"${CC:-cc}" -Wall -Wextra -Werror -std=c11 -O2 -Isrc tests/sokol/zapper_probe.c "${core[@]}" -o "$test_dir/zapper_probe" -pthread -lm
"$test_dir/zapper_probe"
for mode in mouse audio muted unavailable; do
    mkdir "$test_dir/$mode"
    if [ "$mode" = mouse ]; then name=menu_mouse; else name=frontend_integration; fi
    cp "$test_dir/$name" "$test_dir/$mode/"
    (cd "$test_dir/$mode"
        if [ "$mode" = unavailable ]; then export NES_DISABLE_AUDIO=1; else unset NES_DISABLE_AUDIO; fi
        "./$name" "$mode"
    )
done
if [ -n "$rom_path" ]; then
    mkdir "$test_dir/rom-pacing"
    cp "$test_dir/pacing" "$test_dir/rom-pacing/"
    (cd "$test_dir/rom-pacing"; unset NES_DISABLE_AUDIO; ./pacing "$rom_path" "${2:-1800}")
fi
echo "Sokol frontend checks passed. Captures: $test_dir"
