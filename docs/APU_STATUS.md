# APU completion audit

Status: **all 26 core test suites passed; full feature completion and hardware
accuracy validation remain pending**. Updated on 2026-09-15 from user-provided
test output. The assistant did not run builds, tests, emulators, or downloads;
the repository's `AGENTS.md` preserves that code-only preference.

The user's completed `build.sh --test` run reports all 26 suites passed,
including standard APU timing, expansion synthesis/replay, PAL timing,
expansion-viewer snapshot isolation, mapper 85 save-state replay and the revised
CPU/APU reset test. GUI and test-ROM validation remain pending. These synthetic
checks do not establish complete hardware accuracy or FDS game support.

## Implemented in source

| Area | Implementation |
| --- | --- |
| Standard APU | Two pulses, envelopes, length counters, duty, sweeps, triangle linear counter/held DAC, noise LFSR, DMC buffering/DMA/loop/IRQ, four/five-step frame sequencers and reset delays |
| Timing fixes | Correct length table; free-running oscillators; noise CPU periods; frame IRQ reassertion; pending length/halt collision handling |
| Output | Nonlinear pulse/TND mixer, 32-tap fractional-phase resampling, 90/440 Hz high-pass and 14 kHz low-pass filters |
| Regions | Header/GUI selection; PAL 16:5 CPU/PPU ratio, 312 scanlines, PAL noise/DMC/frame periods, DMA controller gating; Dendy timing; region-aware pacing and pitch display |
| VRC6 | Two pulses, saw, halt/frequency scaling, mapper 24/26 register wiring |
| Namco 163 | Shared wave/register RAM, auto-increment port, phase updates and multiplexed 1–8 channels |
| Sunsoft 5B | Three tones, shared noise, volume/envelope controls |
| MMC5 | Two pulses with envelopes/length counters and PCM direct/read modes with IRQ acknowledgement |
| VRC7 | Mapper 85 banking/RAM/mirroring/IRQ; six FM voices, built-in/custom patches using emu2413 |
| FDS sound unit | Wave RAM, envelopes, modulation table/counter and wave output; engine-level integration only |
| Save states | Version 5 stores region/divider phase, pending APU writes, output filter history, expansion RAM/registers/timers and FM operator state; versions 1–4 import |
| Viewer | Standard five-channel piano roll/activity/volume display plus up to eight expansion voices; region-aware nominal pitch and elapsed time; noise/PCM levels remain unpitched |

## Remaining work before calling the requested scope complete

- FDS disk-image loading, BIOS selection and disk-controller/mapper integration.
  Mapper 20 is currently rejected by the cartridge loader. Having its sound
  engine does not make FDS games playable.
- PAL-specific PPU details beyond the implemented clock/frame changes, including
  OAM refresh behavior, need an accuracy audit.
- Validate standard reset, length/halt collisions and DMC/OAM DMA against test
  ROMs, including the legacy timing cases that previously failed.
- Validate expansion timing, waveforms and volume balance against reference
  recordings/test ROMs. Current relative chip gains are estimates, not hardware
  calibration. No listening or runtime performance check has been performed.

## User-run validation

The self-contained core suite has passed in the user's run:

```sh
bash build.sh --test
```

New/updated cases cover PAL frame/divider/IRQ timing, PAL DMC periods and
controller DMA, output sample count and DC rejection, length collisions,
triangle DAC hold, expansion registers and deterministic audio replay, VRC7
patches, mapper 85, version 1–4 migration, and PAL mid-frame save/load.

Then build the GUI and run the standard APU/reset ROMs:

```sh
bash build.sh --build-only
bash tests/roms/run.sh /home/ben/code/nes-newppu/nes-test-roms/apu_test/rom_singles/*.nes /home/ben/code/nes-newppu/nes-test-roms/apu_reset/*.nes
```

The current ROM runner uses the `$6000` result protocol. The legacy/PAL suites
with `$F0` results need a compatible runner before they can count as validation.
After the core checks pass, frontend verification is `bash tests/sokol/run.sh`.
