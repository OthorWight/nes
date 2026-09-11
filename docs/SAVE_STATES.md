# Save states and battery progress

Quick saves, manual saves, and loads use the same core API in `src/save_state.h`.
The frontend reports success only after the operation succeeds. Errors appear
for three seconds in gameplay, menus, and the debugger, with the path and reason
also written to stderr. Successful loads clear the host audio queue and refresh
the debugger's view of the program counter. The frontend also clears held host
inputs, rebases pacing and reapplies the selected Port 2 preference. Core state
decoding itself preserves serialized device/input state for replay. Diagnostic
history belongs to the host and is excluded from the state format.

## Compatibility

New saves use version 3, which also preserves the PPU rendering-enable latch
used for the odd-frame skip decision. Versions 1 and 2 still load; the missing
PPU latch is initialized from PPUMASK, so a late rendering toggle at the saved
pre-render boundary cannot be reconstructed exactly. Version 2 preserves the
CPU's sampled IRQ and whether an instruction has established a poll result.
Version 1 saves still load; because
they lack that history, their first boundary uses the former live-line behavior.
Precise cross-version interrupt timing cannot be recovered from those files.
Older emulator builds cannot read new version 3 saves.

The former unversioned `STAT` format is rejected with **OLD STATE: CREATE A NEW
SAVE**. It omitted mapper registers, CHR memory, and rendering state, so the
missing information cannot be reconstructed reliably. Existing state files are
not deleted or converted. Create new quick/manual states with this build.

Legacy iNES battery `.sav` files remain raw PRG RAM, separate from state files.
NES 2.0 saves contain declared PRG NVRAM followed by CHR NVRAM, omitting volatile
RAM. The complete size must match the declared nonvolatile storage. Legacy sizes
remain normally 8 KiB, or 64 KiB for MMC5. Partial and oversized files are reported
and protected from automatic overwrite. Old incomplete MMC5 files cannot supply
the missing banks and are rejected rather than treated as complete progress.

The frontend uses `saves/<RomName>/<RomName>.sav` beside the executable. An existing
file there takes precedence over a legacy `.sav` beside the ROM. If the canonical
file is absent, validated legacy RAM is retained and written to the canonical
location on the next successful exit or ROM switch. Legacy files are never
deleted. The headless cartridge API defaults to the ROM-adjacent path; callers
can select a canonical path with `cartridge_set_save_path`.

Battery RAM is read **after** mapper initialization, including MMC5's larger
allocation. Reset preserves it. `cartridge_free` only frees memory; callers must
explicitly call `cartridge_save_battery` and check its result before destroying
progress. Failed saves keep the frontend running with the current cartridge so
the directory or disk problem can be fixed and the operation retried. A failed
battery load returns to ROM selection and preserves the existing file. Restore
that file from a backup, or move it aside to deliberately start fresh, then load
the ROM again. Non-battery cartridges do not create automatic `.sav` files.

Loading a state restores PRG RAM too. Exiting afterward saves that restored
progress to the battery file, as expected when resuming an older point in a game.

## Version 3 format

All integers use explicit little-endian encoding. Booleans are one byte (`0` or
`1`); enums and C `int` fields use 32 bits; signed fields use two's complement.
Floats/doubles use IEEE binary32/binary64 bit patterns. No C structure padding,
allocation sizes, function pointers, or host addresses are stored.

| Header offset | Bytes | Value |
| --- | --- | --- |
| 0 | 8 | ASCII `NESSTATE` |
| 8 | 4 | Format version, currently 3 (versions 1 and 2 remain readable) |
| 12 | 4 | Payload byte count |
| 16 | 4 | CRC32 of payload |
| 20 | 4 | CRC32 of original 16-byte NES header plus trainer bytes, if present |
| 24 | 4 | CRC32 of initial PRG backing after loader padding |
| 28 | 4 | CRC32 of initial CHR backing, or zero for CHR RAM |

The ROM identity is captured at cartridge load time, before emulation can change
CHR contents. Renaming a ROM preserves its identity; changing its header, trainer or ROM
contents does not. States from before trainer identity tracking are rejected for
trainer-bearing images. CRCs detect accidental mismatch/corruption, not intentional
forgery. Total file size is bounded to 16 MiB before allocation.

Payload order is defined explicitly by `machine_fields` and `payload` in
`src/save_state.c`, including the selected mapper's `state` callback:

1. The version 1 CPU fields, including stalls, interrupt/reset lines and cycles.
2. All PPU fields: sprite evaluation, OAM, palette backing, scroll/address
   latches, background fetches/shifters, scanline sprites, open-bus decay, dot and
   frame phase, and the complete framebuffer.
3. All APU fields: channel timers/envelopes/sweeps, DMC fetch/output state, frame
   sequencer and delayed reset, sample accumulator, and buffered audio.
4. NES clocks, lines, WRAM/CIRAM, controller shift/strobe/input state, Zapper
   configuration and watchdog, and frame-ready flag.
5. Mirroring, mapper ID and cartridge memory sizes, all PRG RAM, and all CHR
   backing memory. For compatibility the version 1 payload still contains CHR ROM
   bytes; loads verify them against the cartridge and restore only mutable CHR RAM.
   States containing previously modified CHR ROM are rejected.
6. All mapper-private fields, explicitly encoded by the mapper. This includes
   MMC1 partial serial writes, MMC3/TxSROM A12 filtering and IRQ state, MMC2/MMC4
   latches, MMC5 ExRAM, bank registers, protection, and VRC counters. Every current
   mapper ID has a codec; mapper 26 shares the VRC6 codec with mapper 24. Expansion
   audio is not implemented in the current mappers, so no such state exists yet.
7. Version 2 appends the CPU's `irq_pending` and `irq_poll_valid` booleans. The
   preceding field order is unchanged from version 1.
8. Version 3 appends the PPU's `odd_skip_rendering` boolean. The preceding
   field order is unchanged from version 2.

Adding or reordering a serialized field requires a format version change and
explicit compatibility handling. The mapper `state_size` is used only to allocate
a temporary **in-memory** copy; it does not determine file layout.

## Transaction and resume boundaries

Call the API between `nes_clock_tick` calls: after a complete CPU instruction,
interrupt entry, or one-cycle stall. Mid-scanline and pending-DMA/interrupt states
are supported at these boundaries. Saving from inside a bus callback halfway
through `cpu_step` is not supported, since its C call stack is not serialized.

Load checks magic, version, exact length, checksum, ROM identity, memory sizes,
boolean representations, floating-point values, and index/counter ranges. It
decodes into a temporary NES, mapper allocation, PRG RAM and CHR backing. The
live machine is updated only after the entire payload is accepted. Host pointers
and cartridge paths remain attached to the current machine.

State and battery writes use a uniquely created temporary file in the destination
directory. Writes, flushing, syncing, and closing must succeed before replacement.
POSIX uses `rename` and syncs the parent directory; Windows uses `MoveFileExA` with
replacement enabled after committing the temporary file. The destination is never
removed first. A failure after POSIX rename while syncing the directory is
reported even though the complete replacement is already visible. Filesystem and
hardware guarantees still govern power-loss durability.

API references: [POSIX rename](https://www.man7.org/linux/man-pages/man2/rename.2.html),
[mkstemp](https://www.man7.org/linux/man-pages/man3/mkstemp.3.html),
[Windows MoveFileEx](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-movefileexa).

## Verification

`./build.sh --test` / `build.bat --test` discover both save suites automatically.
`tests/save_states.c` uses synthetic cartridges for all 23 mapper IDs. It saves during
rendering with non-default banks, partial MMC1 writes, IRQ and APU/DMC work,
controller reads, and changed CHR/PRG RAM. It compares the entire in-memory
machine and mapper allocation after load, then repeats 24,000 instruction/stall
steps with fixed inputs and compares execution, audio, and frame results. Both a
fresh instance and a previously advanced instance must match.

The suite also rejects legacy, wrong-ROM, unsupported-version, truncated,
oversized, checksum-damaged, and invalid-field states without changing the live
machine. File checks cover replacement, loading, missing directories and failed
replacement. `battery_saves.c` covers canonical/legacy precedence, resets, reloads,
ROM switching, every MMC5 RAM bank, invalid file preservation, and non-battery
cartridges. These are application reliability tests, not hardware accuracy tests.

The original Linux core suites and SDL build were checked locally. Windows-specific file
replacement is implemented but has not been exercised on a native Windows host.
No commercial ROM or GUI is required by these tests; manual gameplay save/load
checks remain useful alongside the synthetic regression coverage.
