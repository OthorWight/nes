# Changelog

## 2026-10-10 — Namco 163 graphics routing

- Honor `$E800` bits 6 and 7 when choosing CHR storage versus internal nametable
  RAM for pattern banks `$E0–$FF`. Retain the low six bits for PRG banking and
  keep nametable routing independent of the two pattern controls.
- Preserve the complete register across save/load using its existing state
  byte. Add synthetic coverage of bank boundaries, both pattern-table halves,
  ROM/RAM writes, nametables and reset.
- Validation: all 31 core suites and the Linux build passed. Captured the local
  Namco Classic II title, menu and course tutorial screens after normal
  controller input; title/menu output matches with extra sprites on and off.

## 2026-10-09 — Optional extra sprites

- Add View > Remove Sprite Limit, off by default and saved globally. Draw extra
  OAM sprites in a separate display buffer while preserving hardware OAM
  evaluation, overflow/sprite-zero flags, mapper bus activity and Zapper sensing.
- Fetch extra pattern rows through private mapper snapshots, with priority,
  clipping and both sprite sizes/flips. Save states retain their existing format;
  resets and loads clear the display cache. Existing settings remain readable.
- Cover 9–64 sprites and full-state equivalence across all supported mappers.

## 2026-10-09 — Playback performance follow-up

- Investigated slowdown and audio starvation with the local Super Mario Bros. 3
  ROM. The initial benchmark attributed approximately 2.8 ms/frame of additional
  CPU time to attached debugger execution/history.
- Skip CPU/PPU access callbacks unless matching watchpoints are active; batch
  normal-playback clock accounting once per CPU clock. Preserve exact per-dot
  stepping, historical-input replay, and PPU watchpoint suspension/resumption.
- Added full-state playback equivalence checks in NTSC/PAL/Dendy and transitions
  from access stops back into precise stepping.
- Added an optional sustained real-ROM, large-window CRT/audio probe to the
  Sokol runner, with isolated save files and diagnostic output.
- Recover late playback with at most three complete emulated frames per UI tick
  before presenting once. Keep all frame audio/history and stop immediately for
  breakpoints; retain regular event handling between bounded batches.
- Retain up to 250 ms of scheduler debt so bursts of late frames can recover
  their consumed audio instead of losing emulated time at the old 50 ms limit.
- Enable `-O3` and link-time optimization in GCC/Clang application, core-test and
  performance builds, retaining strict floating-point behavior. A paired local
  rewind-enabled benchmark measured another 12.6% median CPU-time reduction.
- Reserve three audio device blocks before refill and prime with a fourth,
  adding about 23 ms of steady reserve for bursts of late frames. Extend the
  device-clock simulation with 35 ms stalls and eight-frame slowdown bursts.
- Prefer the faster available Linux CPU clock class for the execution worker on
  hybrid machines; respect existing affinity and fall back to OS placement when
  frequency data is unavailable. `NES_EXECUTION_AFFINITY=system` opts out.
- Validation: all 29 optimized core suites passed; the final audio/timing and
  execution changes were checked again, including execution with both automatic
  and OS CPU placement. Desktop UI and audio/muted/unavailable checks passed.
  Final execution checks also passed ASan/UBSan (LeakSanitizer disabled in this
  environment).
- Local Super Mario Bros. 3 checks at 2862×1722 with CRT/metrics enabled: one
  minute on Balanced measured 60.10 FPS / 100.00% speed; a second minute with
  repeated 35 ms host stalls measured 60.06 FPS / 99.94%. Both had zero audio
  underruns, trims and errors. Captures: `build/tests/sokol-balanced-IrUB6v/`.
- The same machine's Power Saver mode still produced occasional underruns despite
  averaging 100% speed. The Balanced comparison was temporary and restored the
  original profile. Software improvements cannot guarantee pacing under host
  power limits or long stalls.

## 2026-10-09 — Debugger and execution control

- Added a parkable execution worker with bounded UI handoffs. It preserves native
  instruction/DMA stacks for exact CPU clock and PPU dot debugging.
- Added instruction, over/out, CPU-cycle, dot, nominal scanline/frame, next
  scanline/frame, address, raster-location, and NMI/IRQ handler stepping.
- Added conditional execution and CPU/PPU access breakpoints, address ranges,
  hit/ignore counts, one-shot behavior, BRK/JAM traps, and per-ROM persistence.
- Added bounded instruction/frame reverse history with checkpoint replay and
  historical controller/Zapper inputs, reusable by future TAS support.
- Expanded the debugger UI with clickable controls, a command bar, memory/call/
  breakpoint/trace/PPU/help tabs, symbols, memory watches/search, safe register/RAM
  edits, explicit held inputs, and CSV trace export.
- Added keyboard tab navigation, scrollable call/breakpoint/trace/OAM lists,
  visible breakpoint conditions, and decoded CHR pattern-table previews.
- Stop NMI/IRQ seeks at safe handler-entry boundaries with tracked returns;
  Step Out is immediately available. Identify JSR, BRK, NMI, and IRQ call frames.
- Keep UI code breakpoints in the conditional table so hit counts and one-shot
  settings are honored; route Alt+F10 past the Alt-activated menu.
- Report CPU status-read watchpoint values after internal open-bus composition,
  and include DMA alias/open-bus reads in access observations.
- Added safe instruction-boundary synchronization for saves, loads, resets, ROM
  switching, and shutdown; unfinished states are rejected by the save API.
- Added execution/symbol core suites and frontend UI regression coverage.
- Reduced inactive-breakpoint overhead and bulk-serialized little-endian
  framebuffers without changing the save format to make rewind checkpoints cheap.
- Documented debugger commands, execution ownership, TAS extension points, and
  bounded-history/inspection limitations in `docs/DEBUGGER.md`.
- Validation: `./build.sh --test` passed all 29 core suites, including the final
  interrupt-return, status-read watchpoint, and historical-input replay regressions.
  `./build.sh --build-only` succeeded without warnings after bounding the
  breakpoint-condition formatter to a byte.
- `bash tests/sokol/run.sh` passed desktop input, debugger controls, breakpoint
  paging, Alt+F10 routing, memory/CHR/OAM inspection, reverse stepping, and all
  audio/muted/unavailable-device pacing modes. Captures are under
  `build/tests/sokol-2Pey9A/`.
- The execution suite passed AddressSanitizer and UndefinedBehaviorSanitizer.
  LeakSanitizer was disabled because it cannot run under this environment's
  ptrace-based tracing; leak detection is not verified.
- Earlier synthetic benchmark modes produced matching cycle counts and framebuffer CRCs;
  frame-history overhead after optimization was approximately 0.25 ms/frame
  beyond execution control in that measurement.
- Windows/macOS builds have not been verified here.
