# Cartridge loading and bus behavior

The loader accepts ordinary iNES files and supported NES 2.0 board layouts.
`cartridge_parse_header` decodes metadata without allocating cartridge memory;
`cartridge_load_ex` checks the supported layout, reads the image, and returns a
complete cartridge or a useful error. The frontend displays load errors in the
OSD, including unsupported mapper/submapper and timing combinations.

## Metadata and limits

`CartridgeInfo` retains the declared PRG ROM, CHR ROM, volatile PRG/CHR RAM and
nonvolatile PRG/CHR RAM sizes. Mapper IDs are 16-bit containers for NES 2.0's
12-bit ID, with a separate submapper number. Both NES 2.0 linear ROM sizes and
exponent/multiplier sizes are decoded with overflow checks before narrowing.

For existing mapper code, `chr_rom`/`chr_rom_size` name the CHR **backing memory**;
`chr_is_ram` determines whether it can be written. Declared sizes remain separate
from power-of-two ROM backing sizes. Non-power-of-two ROMs mirror their final
populated address-line segments into the remaining backing. RAM is allocated at
its declared size, and smaller PRG RAM chips mirror within the mapped window.
Every mapper CHR write uses the guarded cartridge helper, including direct
mapper callbacks; CHR ROM writes cannot change graphics data.

| Case | Behavior |
| --- | --- |
| iNES with no CHR ROM | Allocate 8 KiB CHR RAM |
| iNES PRG RAM size byte zero | Retain the historical 8 KiB default, or 64 KiB for MMC5 |
| iNES battery flag | Persist PRG RAM in the existing raw `.sav` format |
| NES 2.0 RAM shift zero | No such memory; do not invent 8 KiB of RAM |
| NES 2.0 RAM shift nonzero | Allocate `64 << shift` bytes, subject to board limits |
| Trainer | Validate/read all 512 bytes and install at `$7000–$71FF`, including after canonical battery-save selection |
| Dual-region NES 2.0 image | Run with the existing NTSC timing |
| Nonstandard console, PAL-only or Dendy | Reject; those hardware/timing modes are not implemented |
| Unknown mapper or unsupported submapper | Reject explicitly, preserving the full mapper ID in the error; mapper 78 supports submappers 1 and 3 |
| Mixed CHR ROM/RAM, mixed volatile/nonvolatile chips on one bus | Reject until board-specific selection is implemented |
| Miscellaneous ROM areas or invalid NES 2.0 reserved fields | Reject |

The parser caps PRG ROM at 64 MiB and CHR ROM at 8 MiB. Loading additionally
requires PRG ROM of at least 16 KiB, CHR RAM of at least 8 KiB when no CHR ROM is
present, and PRG RAM within the implemented bank capacity (8 KiB, or 64 KiB for
MMC5). Unbanked CHR RAM boards reject larger NES 2.0 RAM layouts. NES 2.0 NROM,
MMC1, MMC2/MMC4, MMC3/TxSROM, MMC5 and mapper 78 images also have ROM limits matching their
implemented address lines; larger outer-bank variants are rejected. Legacy iNES
images retain the existing bank-wrapping behavior for compatibility.

Four-screen layouts are accepted for NROM, MMC3 and DxROM. Mapper 78 uses that
header flag as a legacy board selector: set selects horizontal/vertical mirroring
(Holy Diver), clear selects lower/upper one-screen mirroring (Cosmo Carrier).
NES 2.0 submapper 1 explicitly selects one-screen wiring and submapper 3 selects
horizontal/vertical wiring, overriding the flag; submapper 0 uses the legacy
hint. Register bit 3 switches between the two nametable arrangements. Reset
selects the bit-zero arrangement and bank zero. This board uses the ordinary
2 KiB CIRAM, not four independent nametables. Mapper 78 bus conflicts are not
yet emulated.

Other boards with that flag are rejected until their wiring is implemented.
Byte 15's default expansion
device is advisory: controller/Zapper selection remains in Settings. Battery-backed
flash/EEPROM layouts without supported NVRAM are rejected.

Actual file size is checked against header + trainer + declared ROM sizes before
ROM allocation. Reads and mapper allocations are checked, and failures release
partial allocations. No failure path saves battery RAM. Existing iNES trailing
data is tolerated; declared ROM regions must still be complete.

NES 2.0 battery files contain declared PRG NVRAM followed by CHR NVRAM, omitting
volatile RAM. Existing PRG-only iNES battery files remain unchanged. The shared
save API validates the complete size and writes atomically. Trainer bytes are
included in ROM identity; images with different trainers cannot share states.

## CPU data bus

The external bus latch records the last driven read/write byte, including DMA
reads. Unmapped memory, write-only APU registers, and absent/disabled cartridge
RAM return this latch. Controller reads preserve bits 7–5 and supply the NES port
input bits. Reading `$4015` preserves bit 5, acknowledges the APU frame interrupt,
and leaves the **external** bus latch unchanged because it is an internal CPU
register. The existing CPU `open_bus` field separately follows instruction reads.
Controller shift registers now also capture changes while strobe is held high
when the strobe falls.

This models the NES wiring, not every Famicom expansion port or clone. Analog bus
decay, bus conflicts on ambiguous discrete boards, and precise DMC/controller DMA
collisions remain separate compatibility work.

## Mapper audit and regression coverage

- All 23 mapper IDs have synthetic checks for CHR ROM protection, CHR RAM writes,
  absent PRG RAM, extreme bank-register values, and unchanged PRG ROM contents.
- MMC1 checks cover RAM disable/re-enable, serial reset and lower one-screen
  mirroring after reset.
- MMC3 and TxSROM use `mapper_mmc3.c` for common banks, RAM protection, A12 filtering
  and IRQ state. Their original state field order is retained. Tests cover bank
  modes, read-only/disabled RAM, reset IRQ clearing, four-screen MMC3, and TxSROM
  CIRAM selection through CHR register bit 7. Both existing split-timing layouts
  and frame parities still run in `ppu_mmc3_irq.c`.
- VRC6 RAM enable now controls reads as well as writes. FME-7 correctly separates
  RAM selection (bit 6) from RAM enable (bit 7), returning open bus when disabled.
- MMC5 resolves PRG reads and writes through one bank calculation in every PRG
  mode; selecting absent RAM floats instead of exposing ROM. CHR RAM writes use
  the same bank mapping as reads.
- Namco 163 pattern banks routed to CIRAM now read/write that RAM rather than
  falling through to zero. Its broader register variants remain unvalidated.
- Save version 1 remains readable, but CHR ROM bytes in a state must match the
  loaded ROM. A state that contains modified CHR ROM is rejected without changing
  the running machine; loading never writes over cartridge ROM.

`tests/cartridge_loading.c` checks header parsing, extended IDs, checked sizes,
rejection paths, padding, absent/small RAM, trainers, NVRAM persistence and
mapper 78's legacy/explicit wiring, bank selection and reset behavior.
`tests/cartridge_bus.c` checks bus behavior and the mapper cases above. Existing
save/replay tests continue to cover all mapper IDs. Run `./build.sh --test` or
`build.bat --test`; no game ROM is needed.

These focused checks do not certify every mapper or board variant. MMC6, outer
bank variants, MMC5's complete ExRAM/CHR-mode-transition behavior, Namco/VRC wiring
variants, and expansion audio still need dedicated work. Reset values used for
unspecified hardware registers are deterministic emulator defaults, not claims
about physical power-on contents. Native Windows and commercial-game checks
were not run for this milestone.

## References

- [NES 2.0 format](https://www.nesdev.org/wiki/NES_2.0)
- [iNES format](https://www.nesdev.org/wiki/INES)
- [Open bus behavior](https://www.nesdev.org/wiki/Open_bus_behavior)
- [APU status](https://www.nesdev.org/wiki/APU_Status)
- [Controller reading](https://www.nesdev.org/wiki/Controller_reading)
- [MMC1](https://www.nesdev.org/wiki/MMC1), [MMC3](https://www.nesdev.org/wiki/MMC3),
  [TxSROM](https://www.nesdev.org/wiki/INES_Mapper_118)
- [MMC5](https://www.nesdev.org/wiki/MMC5), [FME-7](https://www.nesdev.org/wiki/Sunsoft_FME-7)
- [Mapper 78](https://www.nesdev.org/wiki/INES_Mapper_078)
