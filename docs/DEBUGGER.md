# NES debugger

Open the debugger with **F10** or **Debug > Open / Step Instruction**. The game
remains visible beside the debugger, including pixels produced so far in a
partial frame. Click stepping buttons, select a Debug menu command, or open the
command bar with **Ctrl+G**. Keyboard navigation and F7 operate on the selected
disassembly row; right-clicking a row toggles its code breakpoint.
Use **Ctrl+1** through **Ctrl+6** to select workspace tabs. The mouse wheel and
Page Up/Down scroll calls, memory, breakpoints, trace records, or OAM entries in
the selected tab. Clicking a breakpoint prefills its enable/disable command;
Enter applies it.

## Execution controls

| Control | Behavior |
| --- | --- |
| F10 / Into | Execute one instruction; from a partial instruction, finish its remainder |
| Shift+F10 / Over | Execute a call and stop at its return address with the original stack pointer |
| Ctrl+F10 / Out | Return from the most recently tracked subroutine or interrupt |
| Cycle | Advance one CPU clock checkpoint, after PPU/APU/mapper clocks and before the associated CPU bus access |
| Dot | Advance one actual PPU dot, including dots during DMA |
| Line | Stop at the next scanline's dot zero |
| Frame | Stop at the next frame's scanline zero, dot zero |
| F8 | Advance a nominal frame duration in PPU dots |
| Ctrl+F8 | Run to the selected instruction |
| Alt+F10 | Reverse to an earlier instruction boundary |
| Shift+F8 | Restore an earlier frame checkpoint |
| F9 | Switch between ordinary playback and debugger inspection |
| Debug > Run To | Seek to a code address or the next NMI/IRQ handler entry |
| Debug > Finish Instruction | Complete suspended execution to a safe instruction boundary |

The menu also offers a nominal scanline duration (341 dots). A nominal frame
uses the selected region's scanline count; an odd rendered NTSC frame has a
shortened pre-render scanline, so nominal-duration stepping and next-frame-start
stepping are deliberately separate commands.

Seeks run in bounded quanta so menus and Stop remain responsive. An unreachable
target stops after 20 million CPU cycles. Breakpoints and watchpoints can stop a
seek before its target. BRK/JAM traps stop before executing the instruction;
JAM trapping is enabled initially. Interrupt targets refer to CPU handler entry,
at an instruction boundary with the return already tracked. A masked IRQ
therefore does not count, and Step Out can follow a handler-entry stop immediately.

## Inspection workspace

The top of the panel shows CPU registers, flags, disassembly, and the current
instruction's starting address even when its operands have already been fetched.
The stop line identifies the reason and whether execution is inside an
instruction. The workspace tabs provide:

- **Calls:** tracked subroutine/interrupt returns and watched CPU memory.
- **Memory:** CPU or PPU hex views, paging, and RAM editing.
- **Breaks:** up to 32 execution/access breakpoints, ranges, conditions, hit counts,
  ignored hits, and one-shot state.
- **Trace:** a bounded history of 512 instructions/watchpoint hits; scroll to
  inspect older entries and export CSV for deeper analysis.
- **PPU:** live scroll/register state, palettes, all 64 OAM entries through
  scrolling, and both decoded CHR pattern tables. The existing
  Nametable and APU viewers remain available alongside the debugger.
- **Help:** command syntax.

CPU I/O memory is displayed as `??` because reading it could change the machine.
PPU memory inspection runs against private machine/mapper copies so banking
and address callbacks cannot change live rendering state. CPU addresses resolve
through the mapper's current banks; imported symbols identify CPU addresses,
not physical PRG banks.

## Command bar

Addresses and bytes are hexadecimal (`$` is optional); counts, breakpoint IDs,
scanlines, dots, and ignored-hit counts are decimal. Quote filenames containing
spaces. Commands validate arguments before applying them.
Instruction, clock, dot, scanline/frame, boundary, and interrupt steps accept a
count. Over, Out, and address/raster seeks take a single target.

```text
goto $8000
view ppu
label $8000 Reset
labels "game labels.lbl"
mem $0000
vram $2000
watch $0010
unwatch
find FF 0000 07FF
step instruction 10
step cycle 1
step dot 1
step scanline 1
step frame 1
step nextline
step nextframe
step over
step out
step nmi
step irq
until Reset
line 120 17
back instruction
back frame
bp Reset
wp rw $0000-$07FF
wp w $2000 value=$80 a=$01 ignore=3 once
wp prw $0000-$1FFF
enable 0
disable 0
delete 0
clear
set a $7F
set pc Reset
poke $0010 $FF
input $81 $00
trap 1 1
trace "debugger trace.csv"
savebp "breakpoints.bin"
loadbp "breakpoints.bin"
help
```

`wp` access kinds are `r`, `w`, `rw` for CPU accesses and `pr`, `pw`, `prw`
for PPU accesses. Conditions include `value`, `a`, `x`, `y`, `ignore`, and
`once`. Watchpoints stop **after** the access, with its side effects already
performed. Resuming does not repeat that access. Dummy and DMA bus operations
are observable too. Code breakpoints stop before an instruction.

`set` accepts `pc`, `a`, `x`, `y`, `sp`, and `p`. `poke` edits CPU internal RAM
including its mirrors, not ROM or hardware registers. Edits finish the current
instruction first and invalidate reverse history. Save/load/reset operations
also synchronize to instruction boundaries; they never serialize an unfinished
native call stack. A direct save-state API call inside an instruction returns
`NES_STATE_BUSY` instead of producing a state that cannot be resumed correctly.

Held `input` masks apply during debugger stepping and seeking. Bits from low to
high are A, B, Select, Start, Up, Down, Left, Right. These explicit inputs are
separate from keyboard events consumed by the debugger. Future TAS playback can
use the same execution interface and provide its own input source.

Symbols support ca65/VICE `.lbl` lines (`al 008000 .Reset`) and simple
`8000 Reset` lines. Failed imports preserve the existing symbol table. Symbol
names can replace numeric CPU addresses in commands.

Breakpoint definitions save automatically per ROM identity under the emulator's
`saves/` directory as `.debug` files. Hit counts are runtime observations and
restart at zero when definitions are loaded. Imports are bounded, checksummed,
and applied atomically. Explicit `savebp`/`loadbp` commands support sharing files.

## Reverse history and TAS foundation

Execution owns the machine only during `execution_pump()`. Its worker parks
before handing ownership back to the UI, including when paused inside CPU
addressing helpers or DMA loops. Synchronization primitives preserve the
unfinished native call stack. Graphics and window events stay on the main
thread. The core can still run directly without an execution controller for
headless tests and benchmarks.
Normal playback batches debugger clock accounting once per CPU clock and skips
inactive access callbacks. Exact dot steps, replay, and active PPU watchpoints
use per-dot accounting so switching modes from a suspended access remains exact.
Normal run quanta yield at a CPU clock checkpoint; exact dot targets still yield
at the requested PPU dot. Host counters and callback masks are excluded from saves.
On Linux, the worker prefers available CPUs within 10% of the highest reported
maximum clock when the allowed set contains different clock classes. It respects
the existing affinity mask and falls back to OS placement when frequency data is
unavailable. Set `NES_EXECUTION_AFFINITY=system` to retain OS placement. This host
choice does not affect emulated timing, save states or replay.

Reverse history uses up to 32 instruction/frame checkpoints with a 64 MiB cap,
plus 256 input-change stamps. Instruction rewind restores a checkpoint and
replays to the requested instruction boundary with historical controller/Zapper
inputs. Breakpoints are suppressed during this internal replay. If the necessary
checkpoint or input history has been evicted, reverse reports unavailable rather
than guessing. Frame reverse restores a prior safe frame checkpoint; its CPU
instruction boundary may be a few dots after the frame starts.

History is deliberately bounded and is not a TAS movie or project format.
The next TAS layer can add a persistent input timeline, longer-lived checkpoints,
branching, seeking, and file formats on top of these execution/input primitives.
Changing state externally must call `execution_invalidate()` after synchronization.
Call stacks start when execution attaches or history resets; older calls are
unknown, and deliberately manipulated stacks can exceed automatic tracking.

## Validation

`./build.sh --test` checks exact stops, state equivalence after partial
instructions, interrupt entry/returns, DMA suspension, controller side effects,
conditional breakpoints, calls, rewind, read-only PPU inspection, and symbol
imports. `bash tests/sokol/run.sh` exercises the real frontend controls,
command parsing, panel mouse routing, trace export, memory views, reverse stepping,
breakpoint paging, and screenshots, alongside the existing
audio/pacing checks. Windows equivalents
are `build.bat --test` and `powershell -File tests/sokol/run.ps1`.
