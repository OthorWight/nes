# Core regression tests

Run `./build.sh --test` on Linux/macOS or `build.bat --test` on Windows.
Each top-level `.c` file is an executable test suite, automatically discovered by
the runner. A failed assertion or compilation stops the command with a nonzero
exit code. No SDL installation, display, audio device, or game ROM is required.

The hardware tests exercise behavior that games depend on, without embedding
game code, filenames, ROM hashes, or game-specific timing workarounds. Save suites
also check application reliability using generated cartridge data in temporary
subdirectories under `build/tests/`, removed on success.

## Coverage

| Suite | Behaviors checked |
| --- | --- |
| `system_clock.c` | Three PPU dots and one mapper M2 tick per CPU cycle; APU advancement; dummy/read-modify-write cycles; indexed-read and branch penalties; seven-cycle reset without stack writes |
| `dma_timing.c` | 513/514-cycle OAM DMA plus the initiating instruction; full copy and OAM address wrap; CPU/APU/PPU advancement during DMA; deferred NMI service; DMC cartridge reads, address wrap, CPU stalls, final-byte IRQ and acknowledgement |
| `interrupt_connections.c` | PPU-generated NMI, seven-cycle entry and six-cycle RTI, saved PC/status, edge behavior and NMI priority; independent APU/mapper IRQ acknowledgements; actual APU and rendering-driven mapper IRQ delivery to CPU |
| `ppu_frame_timing.c` | 89,342-dot NTSC frames; odd-frame shortening only with rendering enabled; vblank/pre-render flag edges; status acknowledgement and shared write-latch reset |
| `ppu_register_bus.c` | CPU instructions accessing RAM and PPU register mirrors; nametable routing; PPUDATA increments, delayed reads, palette bypass and buffer refill; PPUSTATUS sampled on the CPU data-read cycle |
| `apu_timing.c` | Phase-dependent three/four-cycle frame-counter reset; channel-enable/length status; pulse versus triangle timer division; all 16 NTSC DMC output periods; five-step IRQ suppression and independent IRQ inhibition |
| `ppu_mmc3_irq.c` | MMC3/TxSROM filtered A12 split timing with both 8x8 pattern-table layouts and frame parities; IRQ acknowledgement |
| `controller_input.c` | Controller serial reads/strobe behavior; explicit Zapper selection, light, trigger and offscreen behavior |
| `cartridge_loading.c` | iNES/NES 2.0 metadata, extended IDs and sizes, invalid/unsupported/truncated images, ROM padding, absent/small RAM, trainers, CHR NVRAM, mapper 78 header/submapper wiring and reset |
| `cartridge_bus.c` | External CPU open bus and partial reads, all-mapper ROM protection/RAM writes, RAM enable/protection, MMC3/TxSROM mirroring and bank modes, MMC5 read/write agreement |
| `save_states.c` | All 23 mapper IDs: full in-memory restore, fixed-input replay, frame/audio agreement, partial serial writes and IRQ state; wrong-ROM/version/corrupt/legacy rejection, atomic file replacement and failure checks |
| `battery_saves.c` | Canonical/legacy paths, reset/reload and ROM switching, MMC5 full RAM allocation, invalid-file preservation, non-battery behavior |
| `zapper_watchdog.c` | Application-level hang heuristic: sustained polling, recovery, and selected false-positive exclusions |

`test_system.h` supplies a synthetic cartridge and observers for mapper bus
accesses, PPU dots, and M2 clocks. The CPU, PPU, APU, and system bus are the normal
production implementations. Tests seed internal state where necessary to isolate
a boundary or timer phase; those initial conditions do not validate power-on
state or every instruction that could lead to that state.

The APU timer tests use one-cycle CPU stalls to sample exact cycle boundaries
through `nes_clock_tick`. Separate DMA tests initiate transfers through real
register writes. The DMC transfer test verifies stalls and synchronization but
does not assert the current coarse DMA scheduler's fixed stall count as hardware
truth.

## Hardware references

Expected values should come from documented hardware behavior, not from copying
private constants or reading the implementation's counters to choose a passing
expectation. The current checks draw on these references:

- [PPU frame timing](https://www.nesdev.org/wiki/PPU_frame_timing): NTSC clock ratio,
  frame lengths, and odd-frame shortening.
- [PPU rendering](https://www.nesdev.org/wiki/PPU_rendering): scanline/dot schedule.
- [PPU registers](https://www.nesdev.org/wiki/PPU_registers): register mirrors,
  read buffering, address increments, and status side effects.
- [CPU interrupts](https://www.nesdev.org/wiki/CPU_interrupts): entry bus cycles,
  saved status, vectors, and IRQ/NMI distinctions.
- [DMA](https://www.nesdev.org/wiki/DMA): OAM transfer cost, alignment, and DMC
  interaction with the CPU bus.
- [APU frame counter](https://www.nesdev.org/wiki/APU_Frame_Counter): reset delay,
  mode control, and quarter/half-frame behavior.
- [APU DMC](https://www.nesdev.org/wiki/APU_DMC): NTSC rate table, address wrap,
  buffered playback, and end-of-sample IRQ behavior.
- [MMC3](https://www.nesdev.org/wiki/MMC3): filtered A12 clocks and IRQ controls.

## Limits and next additions

Passing these tests is a regression baseline, not full NES hardware certification.
In particular, the following are not yet exhaustively covered:

- Exact four/five-step APU sequencer event cycles and frame-IRQ reassertion windows.
- Cycle-by-cycle DMC halt/alignment/reload scheduling, DMC/OAM overlap, and the
  controller/PPUDATA read corruption caused by DMA.
- Every CPU/PPU phase alignment around vblank, NMI suppression, and interrupt
  polling delays following CLI/SEI/PLP.
- Sprite-zero hits, sprite-overflow edge cases, mixed-table 8x16 sprite fetches,
  and every rendering-enable transition.
- PAL timing, expansion audio, open-bus decay, and electrical power-on behavior.

For new tests, specify the hardware condition, expected observation, and source.
Use bounded loops so regressions fail instead of hanging the runner. Prefer a
small synthetic instruction sequence or register setup over a game dependency.
If a documented expectation fails, investigate the core; do not silently widen
the tolerance or encode the current bug as expected behavior.

See [save-state format and test details](../docs/SAVE_STATES.md) for the resume
boundary, migration policy, and persistence API.

See [cartridge and bus support](../docs/CARTRIDGES.md) for loader limits, mapper
audit details, references and remaining board-specific gaps.
