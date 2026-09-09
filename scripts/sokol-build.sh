# Shared by the application and real frontend tests. Run from the repo root.
sokol_build() {
    local output=$1
    shift
    local compiler=${CC:-cc}
    case "$(uname -s)" in
        Darwin)
            "$compiler" -Wall -Wextra -std=c11 -O2 -Isrc -x objective-c -fobjc-arc "$@" -o "$output" \
                -framework Cocoa -framework QuartzCore -framework Metal -framework MetalKit \
                -framework AudioToolbox -framework GameController -lm
            ;;
        MINGW*|MSYS*)
            "$compiler" -Wall -Wextra -std=c11 -O2 -Isrc "$@" -o "$output" -static \
                -luser32 -lgdi32 -lwinmm -lole32 -lshell32 -ld3d11 -ldxgi
            ;;
        Linux)
            if ! command -v pkg-config >/dev/null || ! pkg-config --exists x11 xi xcursor gl alsa; then
                echo "Install X11, Xi, Xcursor, OpenGL and ALSA development packages and pkg-config." >&2
                echo "Debian/Ubuntu: sudo apt install build-essential pkg-config libx11-dev libxi-dev libxcursor-dev libgl-dev libasound2-dev" >&2
                return 1
            fi
            local flags
            flags=$(pkg-config --cflags --libs x11 xi xcursor gl alsa)
            # pkg-config intentionally supplies separate compiler/linker arguments.
            # shellcheck disable=SC2086
            "$compiler" -Wall -Wextra -std=c11 -O2 -Isrc "$@" -o "$output" $flags -pthread -ldl -lm
            ;;
        *) echo "Unsupported host: $(uname -s)" >&2; return 1 ;;
    esac
}
