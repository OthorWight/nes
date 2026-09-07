#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/../.."
mkdir -p build/tests
test_dir=$(mktemp -d "$PWD/build/tests/sdl-XXXXXX")
sources=()
for source in src/*.c; do
    case "$source" in src/gui_main.c) ;; *) sources+=("$source");; esac
done
gcc -Wall -Wextra -Werror -std=c11 -O2 -Isrc $(pkg-config --cflags sdl2) \
    tests/sdl/frontend_integration.c "${sources[@]}" -o "$test_dir/frontend_integration" $(pkg-config --libs sdl2) -lm
gcc -Wall -Wextra -Werror -std=c11 -O2 -Isrc $(pkg-config --cflags sdl2) \
    tests/sdl/menu_mouse.c "${sources[@]}" -o "$test_dir/menu_mouse" $(pkg-config --libs sdl2) -lm
probe_sources=()
for source in "${sources[@]}"; do
    case "$source" in src/debugger.c) ;; *) probe_sources+=("$source");; esac
done
gcc -Wall -Wextra -Werror -std=c11 -O2 -Isrc tests/sdl/zapper_probe.c \
    "${probe_sources[@]}" -o "$test_dir/zapper_probe" -lm
"$test_dir/zapper_probe"
cd "$test_dir"
mkdir mouse
cp menu_mouse mouse/
(cd mouse && SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ./menu_mouse)
for mode in audio muted unavailable; do
    mkdir "$mode"
    # Each executable gets an isolated base path for settings and captures.
    cp frontend_integration "$mode/"
    (
        cd "$mode"
        if [ "$mode" = unavailable ]; then driver=missing; else driver=dummy; fi
        SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER="$driver" ./frontend_integration "$mode"
    )
done
echo "SDL frontend checks passed. Captures: $test_dir"
