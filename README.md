## Controls

### Keyboard Layout (Mappable)
*   **D-Pad**: `Arrow Keys`
*   **Button A**: `Z`
*   **Button B**: `X`
*   **Select**: `Space`
*   **Start**: `Enter`
*   **Menu/Pause**: `F1` or `Escape`

### Mouse Menus
*   **Hover** to select an item; **left-click** to activate it.
*   **Right-click** to open/pause or resume from the main menu, go back from a submenu, or cancel control rebinding. Submenus also have a clickable **Back** button.
*   **Mouse wheel** scrolls ROM/save lists or moves through other menus. The list arrows are clickable too.
*   In Settings, click the volume **< / >** arrows or use the wheel over the volume row to adjust it.

### Gamepad Layout (Windows: XInput-compatible controllers)
*   **D-Pad / Left Stick**: NES Directional Pad
*   **A Button**: NES Button A
*   **B Button**: NES Button B
*   **Back/Share**: NES Select
*   **Start/Options**: NES Start

### Zapper

In **Settings**, set **Port 2** to **Zapper** for light-gun games. Aim with the mouse and fire with the left mouse button. Port 2 and display preferences are saved per ROM; other games use their own override or the global defaults. Settings includes actions to use or update those defaults. Controller is the initial default. Letterbox clicks count as offscreen aim.

If a game repeatedly polls the light gun on a black screen for about three seconds, an OSD warning suggests switching Port 2 back to Controller. This detects suspected polling hangs such as Armadillo's boot loop; it does not automatically change the selected device.

### Quick Save & Quick Load
*   **Quick Save**: Press `F5` on keyboard or `X` on gamepad. (Overwrites the oldest of 10 rolling save files).
*   **Quick Load**: Press `F8` on keyboard or `Y` on gamepad. (Loads the newest available rolling save file).

### Performance and captures

Press `F2` for a metrics panel beside the game: FPS, emulation speed, frame spikes, audio queue and input diagnostics. Press `F3` for the full debug panel with CPU/PPU/APU, mapper and controller details; these replace the terminal dashboard.
`Ctrl+F4` toggles recent event tracing; `F4` saves the bounded history to
`saves/<ROM name>/diagnostics.log`. Audio on, muted, and unavailable share the
same NTSC frame scheduler. Focus loss pauses the game and clears held inputs.
See [pacing, input and diagnostics](docs/PACING_DIAGNOSTICS.md) for capture details,
per-ROM preferences, tests and sensor limitations.

---

# High-Fidelity 8-Bit NES Emulator

A lightweight, robust, and cycle-accurate Nintendo Entertainment System (NES) emulator written in C. It features a modular architecture, custom low-level APU/PPU pipelines, real-time in-game step debugging, rolling quick-saves, and a Sokol frontend with native gamepad support. See [Sokol branch details and limits](docs/SOKOL.md).

---

## Design Philosophy

1. **High Fidelity & Timing Accuracy**: Emulates the custom Ricoh 2A03 CPU alongside the PPU 2C02 rendering pipeline down to the scanline cycle, successfully passing many of Blargg's timing and instruction tests.
2. **Modularity & Readability**: Clean separation between CPU, APU, PPU, and memory mapping subsystems. No obfuscated macros, allowing developer-friendly exploration of early console hardware.
3. **Retro Aesthetics & Feel**: Feature integrations—such as adjusting system volume—use the emulator's *actual* emulated APU pulse channels to synthesize retro 8-bit square-wave "chimes" rather than using modern host-space sound APIs.
4. **Developer-First Tools**: Built-in disassembled memory inspector, cycle counter tracking, live stack inspector, and CPU instruction logger for testing ROM behavior.

---

## Features & Supported Mappers

### System Features
*   **Audio/Video Output**: Sokol audio streaming (44.1 kHz downsampled) and scaling logic (1x-5x, Fullscreen support).
*   **Save States**: 10 rolling slots (`quick_0` through `quick_9`) plus timestamped manual saves. Versioned, ROM-checked states preserve CPU/PPU/APU and mapper execution state; invalid loads leave the running game intact.
*   **On-Screen Display (OSD)**: Outlined, double-pass drop-shadow OSD notifications (e.g., "STATE SAVED") rendered on top of active gameplay.
*   **Input**: Native gamepad hot-plugging (Windows XInput, Linux joystick, macOS GameController) with analog deadzones and a fully-mappable keyboard interface.
*   **Zapper Light Gun Support**: Fully emulated light gun logic using host mouse clicks, validating screen pixel luminance values at the cursor target.

### Mapper implementations (iNES and supported NES 2.0 boards)

NES 2.0 parsing includes extended mapper IDs and declared ROM/RAM sizes. Unknown
submappers, unsupported memory layouts and non-NTSC hardware are reported clearly.
See [cartridge support and limits](docs/CARTRIDGES.md) for the supported combinations
and focused regression coverage.
*   **Mapper 0 (NROM)**: Simple early titles (e.g., *Super Mario Bros.*, *Donkey Kong*).
*   **Mapper 1 (MMC1)**: Advanced switching supporting horizontal/vertical split screens (*The Legend of Zelda*, *Metroid*).
*   **Mapper 2 (UxROM)**: Bank-switching PRG-ROM ROMs (*Mega Man*, *Castlevania*).
*   **Mapper 3 (CNROM)**: Bank-switching CHR-ROM selections (*Contra*, *Adventure Island*).
*   **Mapper 4 (MMC3)**: Fine-grained scanline IRQs, supporting split-screen scrolling (*Super Mario Bros. 3*, *Kirby's Adventure*).
*   **Mapper 5 (MMC5)**: Highly complex EXRAM modes, multi-tile rendering modifiers, and hardware arithmetic multi-step multiplication registers (*Castlevania III*).
*   **Mapper 7 (AxROM)**: One-screen mirroring selector titles (*Battletoads*).
*   **Mapper 9 / 10 (MMC2 / MMC4)**: Automatic latch-based tile switching for dense background palettes (*Punch-Out!!*, *Fire Emblem*).
*   **Mapper 11 (Color Dreams)**: Direct nibble switching.
*   **Mapper 19 (Namco 163)**: Custom Namco bank selector registers.
*   **Mapper 23 (VRC2 / VRC4)**: Pin-swapped address modes (*Akumajou Special*).
*   **Mapper 34 (BNROM / NINA-06)**: Dual-mode bank configurations.
*   **Mapper 24 (VRC6a)**: Konami VRC6 variant with PRG/CHR banking, mirroring, and IRQ counter.
*   **Mapper 26 (VRC6b)**: Konami VRC6 variant, similar to VRC6a but with A0/A1 address line swap.
*   **Mapper 64 (RAMBO-1)**: PRG/CHR banking, mirroring, and IRQ counter.
*   **Mapper 66 (GxROM)**: Early multi-bank arcade selections.
*   **Mapper 69 (FME-7)**: Precision IRQ interval timing counters (*Batman Return of the Joker*).
*   **Mapper 71 (Camerica)**: PRG banking and mirroring control, including Bee 52 compatibility.
*   **Mapper 78 (Holy Diver)**: PRG/CHR banking and mirroring control.
*   **Mapper 118 (TxSROM)**: PRG/CHR banking, mirroring, and IRQ counter.
*   **Mapper 206 (DxROM)**: Nintendo-style early MMC3 variants.
*   **Mapper 227 (Karateka)**: Obscure multi-cart configurations.

## Real-Time Step Debugger

Press `F10` during gameplay to pause and open the **Step Debugger** beside the game. The game image stays visible while stepping. `F9` resumes; the panel stays open if F2 or F3 is enabled. These toggles also appear in Settings and preserve existing saved preferences.

```
NES DEBUGGER - PAUSED
=========================
PC:8012  A:00  X:00  Y:00  SP:FD
P:34  [..-..IZ.]  CYC:347101
--------------------------------
   8012: 4C 12 80  JMP $8012 = #$4C
   8015: AD 02 20  LDA $2002 = #$80
   8018: 10 FB     BPL $8015
...
--------------------------------
Stack: [ 00 00 00 00 ]
F10:Step|F9:Run|F6:Log:OFF
F7:BRK | UP/DN:Nav | ESC:Menu
```

### Debugger Commands
*   **F10**: Step one single CPU instruction.
*   **F9**: Exit step-mode and run emulator at full speed.
*   **F7**: Toggle Breakpoint on the currently highlighted address.
*   **F6**: Toggle writing continuous execution logs to `step_trace.log` (logs include full register maps, cycles, scanlines, mapped PRG-banks, and active IRQ lines).
*   **Up / Down**: Navigate instruction view.
*   **F12**: Trigger a cold system reset.
*   **Escape**: Exit debugger and return to the System Menu.

---

## Missing Features & Roadmap

*   **Expansion Audio**: Emulation for cartridge-based expansion audio synthesis (such as Namco 163, Sunsoft 5B, or Konami VRC6/VRC7 sound chips) is not yet supported.
*   **NTSC/PAL Select**: Emulation runs at NTSC clock/divider speeds by default; dynamic PAL system toggle options are not yet implemented.
*   **Save State Compression**: States use an explicit, versioned binary format; optional compression remains planned.

---

## Build Instructions

### Windows (MSVC or GCC / MinGW-w64)
Run `build.bat` to build and launch, or `build.bat --build-only` to compile only.
The executable is `build/nes_emulator.exe`. Sokol headers are checked in; no SDL
SDK or DLL is needed. MSVC requires a Visual Studio developer command prompt.

### macOS & Linux
Run `./build.sh` to build and launch, or `./build.sh --build-only` to compile only.
The executable is `build/nes_emulator`.

- macOS: install Xcode command line tools. The build links Metal, Cocoa,
  CoreAudio and GameController (macOS 11 or newer).
- Debian/Ubuntu: `sudo apt install build-essential pkg-config libx11-dev libxi-dev libxcursor-dev libgl-dev libasound2-dev`.
- Other Linux distributions: install a C compiler, pkg-config and the X11, Xi,
  Xcursor, OpenGL and ALSA development packages. An OpenGL 4.3-capable driver is required.

Unix builds report missing prerequisites without installing packages.
See [Sokol migration notes](docs/SOKOL.md) for backend details and controller limits.

### Build-only and test commands

| Action | Linux / macOS | Windows |
| --- | --- | --- |
| Build and launch (default) | `./build.sh` | `build.bat` |
| Build without launching | `./build.sh --build-only` | `build.bat --build-only` |
| Build and run core tests | `./build.sh --test` | `build.bat --test` |
| Show command help | `./build.sh --help` | `build.bat --help` |

`--run` explicitly selects the default build-and-launch behavior. Commands can
also be invoked by their full path from another working directory.

Build-only and test modes do not install dependencies, open the GUI, or pause
for input. Install prerequisites beforehand when using them in automation.
Build-only needs a compiler and the platform libraries listed above. Test mode
only needs the C compiler; graphics libraries and game ROMs are not required.
Windows selects MSVC when available, otherwise GCC.

Test mode discovers `tests/*.c`, builds each as a separate executable against the
emulation core, and runs it immediately. It prints each test's result and a final
pass count. Compilation errors, failed tests, and missing prerequisites return a
nonzero exit code; an invalid command returns exit code 2. Test executables are
written to the Git-ignored `build/tests/` directory. Test mode does not rebuild
the GUI executable; run build-only as well to validate the full application.

See [test coverage](tests/README.md) for CPU/APU/PPU timing, DMA, interrupt, input,
and save reliability checks, their reference material, and remaining gaps.

---

## Save Directories

*   **Saves (`.sav`)**: Battery-backed memory (PRG NVRAM followed by CHR NVRAM for NES 2.0; existing iNES PRG RAM format preserved) is saved on exit and ROM changes to `saves/<RomName_Without_Extension>/<RomName_Without_Extension>.sav` beside the executable. Existing ROM-adjacent saves are imported when no canonical save exists; legacy files are preserved.
*   **Save States (`.state`)**: Quicksaves and timestamped manual saves are stored in a dedicated subfolder structure separated by game name under:
    `saves/<RomName_Without_Extension>/`

State and battery writes replace the destination only after the temporary file is
written successfully. Failed operations show an OSD error. **Older unversioned
`.state` files are unsupported; create new states with this build.** Valid battery
`.sav` files keep their raw format. See [save reliability and format details](docs/SAVE_STATES.md).
