# Sokol testing branch

The frontend uses vendored `sokol_app`, `sokol_gfx`, `sokol_gl`, `sokol_audio`,
and `sokol_time` headers. The emulator core and save-state format are unchanged.
Windows builds use D3D11/WASAPI, Linux uses OpenGL/X11/ALSA, and macOS uses
Metal/CoreAudio. No SDL library or DLL is needed.

`src/host_sokol.c` owns the window, normalized input events, GPU presentation,
clock and audio stream. Game notifications use a transparent CPU overlay.
The debugger and former terminal dashboard use a separate right-hand panel.
F3 toggles the full panel, F2 toggles metrics, and stepping automatically opens
the debugger beside the retained game image. Windowed scale modes expand the
window horizontally for the panel; fullscreen and maximized modes divide the
available area. Mouse aim is confined to the game viewport.
The GPU scales the original NES framebuffer and its mapper-specific
crop directly with nearest filtering. Mouse aim uses the same letterbox and crop.
The existing NTSC scheduler remains responsible for frame timing; vsync is
disabled where Sokol supports it. Metal presentation remains display-throttled.

Audio uses a mutex-protected bounded mono float ring consumed by Sokol's audio
callback. The existing priming, drift correction, volume, mute and diagnostics
remain in the frontend. `NES_DISABLE_AUDIO=1` skips device initialization for
testing unavailable audio. Save states do not include host audio buffers.

Sokol has no gamepad API. This branch polls native backends:

- Windows: XInput-compatible controllers, including hot-plugging. DirectInput-only
  devices and PlayStation controllers without an XInput driver are not supported.
- Linux: `/dev/input/js*`, using the kernel button/axis maps. Device permissions
  must allow reads; controllers without the joystick interface are unavailable.
- macOS: extended gamepads exposed by Apple's GameController framework.

Saved keyboard and controller IDs retain their previous numerical values so
existing bindings remain readable. Menus, debugger, saves, per-ROM preferences,
mouse Zapper and shortcuts retain their existing behavior.

## Build and checks

On Windows, run `build.bat --build-only`, then `build/nes_emulator.exe` to try it.
MSVC and MinGW-w64 builds link Windows system libraries. Sokol headers are
included in the repository. Linux/macOS builds use `./build.sh --build-only`;
see the main README for system development packages.

`build.bat --test` / `./build.sh --test` run the 18 core suites without graphics.
`powershell -File tests/sokol/run.ps1` runs the Windows frontend tests with GCC.
`bash tests/sokol/run.sh` runs the portable frontend tests using a real display
and audio device (Linux CI can supply Xvfb and an ALSA null device).
Fixtures, settings, saves and captures are isolated under `build/tests/`.

The frontend tests exercise menus, focus/disconnect transitions, independent
keyboard/gamepad releases, debugger stepping, preference fallback, save/load,
and measured pacing with audio, muted audio and unavailable audio. Windows also
reads the actual D3D11 render target to check colors, orientation, overlays and
letterboxing and saves `rendering.bmp`. Input is scripted; these tests do not
validate a physical gamepad, speaker output or every DPI/window-manager setup.

Extend the audio run with `powershell -File tests/sokol/run.ps1 -AudioFrames 10800`
or `NES_SOKOL_AUDIO_FRAMES=10800 bash tests/sokol/run.sh`. Avoid concurrent heavy
work when measuring real-time audio. Linux/macOS runtime validation and manual
gameplay remain platform testing tasks.
