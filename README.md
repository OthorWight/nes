# RetroNES: High-Fidelity 8-Bit NES Emulator

A lightweight, robust, and cycle-accurate Nintendo Entertainment System (NES) emulator written in C. It features a modular architecture, custom low-level APU/PPU pipelines, real-time in-game step debugging, rolling quick-saves, and seamless plug-and-play controller support via SDL2.

---

## Design Philosophy

1. **High Fidelity & Timing Accuracy**: Emulates the custom Ricoh 2A03 CPU alongside the PPU 2C02 rendering pipeline down to the scanline cycle, successfully passing many of Blargg's timing and instruction tests.
2. **Modularity & Readability**: Clean separation between CPU, APU, PPU, and memory mapping subsystems. No obfuscated macros, allowing developer-friendly exploration of early console hardware.
3. **Retro Aesthetics & Feel**: Feature integrations—such as adjusting system volume—use the emulator's *actual* emulated APU pulse channels to synthesize retro 8-bit square-wave "chimes" rather than using modern host-space sound APIs.
4. **Developer-First Tools**: Built-in disassembled memory inspector, cycle counter tracking, live stack inspector, and CPU instruction logger for testing ROM behavior.

---

## Features & Supported Mappers

### System Features
*   **Audio/Video Output**: Pure SDL2-driven audio queue (44.1 kHz downsampled) and scaling logic (1x-5x, Fullscreen support).
*   **Save States**: 10 automatic rolling slots (`quick_0` through `quick_9`) to ensure you never accidentally overwrite a good save, alongside limitless timestamped manual saves.
*   **On-Screen Display (OSD)**: Outlined, double-pass drop-shadow OSD notifications (e.g., "STATE SAVED") rendered on top of active gameplay.
*   **Input**: Real-time hot-plugging support for USB/Bluetooth gamepads with analog deadzones and a fully-mappable keyboard interface.
*   **Zapper Light Gun Support**: Fully emulated light gun logic using host mouse clicks, validating screen pixel luminance values at the cursor target.

### Supported Mappers (iNES)
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
*   **Mapper 66 (GxROM)**: Early multi-bank arcade selections.
*   **Mapper 69 (FME-7)**: Precision IRQ interval timing counters (*Batman Return of the Joker*).
*   **Mapper 206 (DxROM)**: Nintendo-style early MMC3 variants.
*   **Mapper 227 (Karateka)**: Obscure multi-cart configurations.

---

## Controls

### Keyboard Layout (Mappable)
*   **D-Pad**: `Arrow Keys`
*   **Button A**: `Z`
*   **Button B**: `X`
*   **Select**: `Space`
*   **Start**: `Enter`
*   **Menu/Pause**: `F1` or `Escape`

### Gamepad Layout (Standard Xbox/PlayStation)
*   **D-Pad / Left Stick**: NES Directional Pad
*   **A Button**: NES Button A
*   **B Button**: NES Button B
*   **Back/Share**: NES Select
*   **Start/Options**: NES Start

### Quick Save & Quick Load
*   **Quick Save**: Press `F5` on keyboard or `X` on gamepad. (Overwrites the oldest of 10 rolling save files).
*   **Quick Load**: Press `F8` on keyboard or `Y` on gamepad. (Loads the newest available rolling save file).

---

## Real-Time Step Debugger

Press `F10` during gameplay to freeze the emulator and launch the interactive **Step Debugger**.

```
NES IN-GAME STEP DEBUGGER
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
*   **Save State Compression**: States are written as uncompressed binary blobs; adding GZIP/Deflate serialization is planned.

---

## Build Instructions

### Prerequisites
Ensure you have **SDL2** development libraries installed.

### Windows (MSVC or GCC / MinGW-w64)
A unified `build.bat` script is provided which detects your environment, automatically fetches the correct SDL2 development packages if missing, and compiles the emulator.

1. Double-click or run `build.bat` in a command prompt:
   ```cmd
   build.bat
   ```
2. The executable `nes_emulator.exe` will be built inside the `build/` folder and automatically launched.

### macOS & Linux
The `build.sh` script checks for packages on Debian/RHEL/Homebrew, installs any missing compilers or dependencies, and builds the application.

1. Run the script:
   ```bash
   chmod +x build.sh
   ./build.sh
   ```
2. The executable `nes_emulator` will compile inside `build/` and run automatically.

---

## Save Directories
*   **Saves (`.sav`)**: Battery-backed progress (WRAM/SRAM) is flushed to the matching game file directory automatically on exit.
*   **Save States (`.state`)**: Quicksaves and timestamped manual saves are stored in a dedicated subfolder structure separated by game name under:
    `saves/<RomName_Without_Extension>/`