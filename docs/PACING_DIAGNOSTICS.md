# Pacing, input and diagnostic captures

Gameplay uses high-resolution fractional deadlines based on the CPU cycles
actually emulated, at the core's NTSC clock of 1,789,773 Hz. Audio enabled, muted,
and unavailable share this scheduler. A host stall more than 50 ms beyond the
deadline discards the backlog. Menus, debugger transitions, focus loss, ROM loads
and state loads rebase time.

## Controls and measurements

| Control | Action |
| --- | --- |
| F2 | Toggle the metrics side panel; saved globally |
| Ctrl+F4 | Toggle bounded event tracing |
| F4 | Write the recent capture, including from menus/debugger |
| F3 | Toggle the full debug side panel; saved globally |
| F6 in debugger | Existing instruction log, independent of event tracing |

Settings also contains Performance and Event trace toggles. The side panel summarizes
up to 180 completed gameplay frames: FPS, emulated CPU time as a percentage of
host time, maximum frame interval, intervals above 25 ms, audio queue depth,
observed empty queues, controller reads and image changes. Image checksums use
the NES framebuffer before OSD drawing. Menu/debugger redraws do not count.

At roughly 100% speed, repeated images or fewer controller reads can help
investigate slowdown inside a game. These signals do not prove that game logic is
stalled: static scenes can repeat, polling frequency varies, and animations may
continue while other logic waits.

## Audio

Audio is queued immediately after emulation, before texture upload and video
presentation can block. Playback starts paused and primes with at least 3,072
mono samples (69.7 ms), covering three requested 1,024-sample device blocks.
Frame-sized batches bring startup priming to about 83 ms. This adds roughly two
frames of startup reserve compared with the former 1,470-sample threshold.
The 6,144-sample (139.3 ms) queue limit is a recovery guard; normal queue depth is
around 60 ms after a frame's refill, plus the device/driver's own buffering.
Excess backlog is cleared and primed again. Core samples are drained even when
muted or the audio device cannot open.

The host frame clock and the audio device clock can differ slightly. A smoothed
queue-depth controller targets 2,048 queued samples before refill and adjusts
the output/input sample ratio by at most +/-1%. Streaming linear interpolation
preserves fractional phase and the boundary sample between frames. This corrects
gradual queue drain/growth without changing CPU/PPU timing or game speed. The
filter smooths the device callback's individual block-sized changes. Its state
and interpolation history are reset on intentional pauses/loads and are excluded
from emulated save state. Correction is limited: sustained emulation slowdown or
a sufficiently long host stall can still exhaust the reserve.

Intentional pauses clear and pause playback. Resume starts with fresh samples.
An empty queue during active playback increments a counter and re-primes audio.
Queue trims and host queue errors are counted separately. Intentional pauses do
not count as underruns. Volume previews may play in Settings and are discarded
when gameplay resumes.

Queue depth measures samples waiting in the host ring buffer. Captures also report device buffer
duration. Neither measures speaker latency: Sokol cannot report exactly how much
audio the device has already played. An observed empty queue is an underrun
indicator, not proof of audible loss.

## Preferences, input and aim

Window scale/maximization, fullscreen and Port 2 are saved per ROM identity.
Settings labels the scope and provides **Use global display/Port 2** and **Make
display/Port 2 global**. ROMs without overrides use the global defaults. Audio,
control bindings, debug panel and the metrics panel remain global.

Overrides are versioned, checksummed `.prefs` files under the executable's
`saves/` directory, named with all three ROM identity checksums and replaced
atomically. Invalid overrides report an OSD error and fall back to globals.
Global settings versions 1–4 remain readable; version 5 adds the performance
display preference. The previous Port 2 setting becomes the initial global default.

Focus loss pauses to the menu and clears held controls and the Zapper trigger.
Controller removal, menu/debugger transitions and successful state loads also
clear inputs; a fresh press is needed after returning. Keyboard, controller
buttons and stick directions have separate state. Releases are handled in menus
too; repeated keydown events are ignored. The frontend reapplies the selected
Port 2 preference after state loads; the core save API still restores serialized
device state for deterministic replay.

Aim uses the host framebuffer-to-logical conversion and the same source crop as rendering.
Position is refreshed each loop, including after a resize with a stationary mouse.
Letterbox/outside-window aim is offscreen, rather than clamped to a bright edge.
Sokol mouse coordinates and the viewport both use framebuffer pixels.

## Captures and debugger reads

F4 replaces `saves/<ROM name>/diagnostics.log`. It includes build date/time, ROM
identity, mapper/submapper, region, Port 2, performance/audio summary, up to 180
frame records and 4,096 events. Frame records include CPU cycles, host interval,
core execution time, queue depth, per-port read counts, controller latches, held
buttons, framebuffer checksum, aim and trigger.

Frame history is collected even when the panel and event tracing are off.
The checksum processes eight bytes per iteration using read-only lookup tables,
with the same IEEE CRC-32 result as save files. Trace-only IRQ/NMI observation
calls are skipped at the CPU/bus/PPU call sites when tracing is disabled; frame
history and input counters remain active.

Enable tracing before reproducing a problem. Events cover CPU cartridge-space
writes, PPU writes, IRQ-source/NMI-line transitions, controller reads/latches and
resume markers. Each carries CPU PC/cycle and PPU scanline/dot. IRQ values are
source masks. PC is sampled at the access, possibly past the instruction operands.
The capture reports overwritten events. Frame and event history are host
observations excluded from save-state encoding; tracing is off by default.

For a report: enable tracing, reproduce briefly, press F4, and send the capture
with the actions that triggered the problem. This is a bounded observation log,
not a deterministic replay or screenshot file.

Disassembly prints `??` for unknown I/O bytes and `UNKNOWN` for an uninspectable
opcode. Peeking does not read PPU/APU/controllers or acknowledge mapper IRQ status.
Internal RAM and cartridge memory use the current mapping, including banked or
disabled RAM. Reads at $6000+ are side-effect free in current mappers; new mappers
must preserve that assumption or supply a dedicated peek implementation.

## Validation and limits

`./build.sh --test` includes synthetic scheduler, queue, independent-input, aim,
preference corruption/identity, bounded-trace and non-consuming-peek checks.
Scheduler tests simulate ten minutes with sleep jitter and three audio modes,
followed by a stall and resume. Existing core and save/replay suites still run.
`audio_playback.c` separately simulates ten minutes per device-clock offset
(-0.8%, -0.2%, 0%, +0.2%, +0.8%), with 1,024-sample device reads and occasional
20 ms late frames. It checks both empty-queue observations and partial device
reads that would insert silence, plus a 512-sample case. At +/-0.2% drift,
disabling correction reproduces underruns or queue trims despite the larger
reserve; correction must eliminate both. Ramp, silence and constant-signal tests
check interpolation, fractional phase, block boundaries and sample counts.

`powershell -File tests/sokol/run.ps1` (Windows/GCC) and
`bash tests/sokol/run.sh` build the real Sokol frontend with scripted input.
They use isolated `build/tests/` directories and preserve captures. Coverage
includes focus/disconnect/menu/debugger releases, ROM overrides, disassembly,
save/load and measured pacing in all audio modes. Windows also reads back the
D3D11 render target to verify colors, orientation, overlays and letterboxing.
Physical controllers, speakers and DPI/window-manager behavior need manual checks.

To extend the audio-enabled run to about three minutes, use
`powershell -File tests/sokol/run.ps1 -AudioFrames 10800` or
`NES_SOKOL_AUDIO_FRAMES=10800 bash tests/sokol/run.sh`. The audio test requires
zero empty queues, trims and queue errors after startup/transitions. Muted and
unavailable tests remain short. Real-time tests should run without concurrent
heavy work. See [Sokol implementation and platform limits](SOKOL.md).
## Performance regression checks

`bash tests/performance/run.sh` measures unpaced core and diagnostic CPU time
using a synthetic rendering fixture. Optionally pass a ROM path and measured
frame count, for example:

```sh
bash tests/performance/run.sh "build/Super Mario Bros.nes" 240
```

The benchmark warms up for 120 frames, then repeats the same workload with
diagnostics detached, normal frame history, and event tracing. It reports core,
diagnostic and total milliseconds per frame, CPU cycles and the final image CRC.
It does not write game saves. Compare the same ROM/frame count and power profile;
there are no machine-dependent timing assertions. Host rendering, frame waits and
physical audio playback are excluded.

The CRC tables are checked in; normal builds need no generator or CPU-specific
instructions. To regenerate them, run
`python3 scripts/generate-crc32-table.py > src/crc32_table.h`.
Mapper 66 resolves PRG/CHR bank offsets on writes, reset and state loads, removing
division from its CPU/PPU reads. Its serialized state still contains only the
two bank registers.

The September 6 diagnostics change (`215e637`) introduced a bit-at-a-time CRC
over all 245,760 framebuffer bytes on every frame. On a local i7-1355U with the
powersave governor, that cost about 6–8 ms/frame even with tracing off. The byte
table reduced this to about 1.5–1.9 ms. In 240-frame ROM runs pinned to CPU 0,
normal core plus diagnostics changed from 14.39 to 7.84 ms/frame for Super Mario
Bros. and 12.76 to 8.48 ms/frame for Super Mario Bros. 3. Cycle counts and final
image CRCs matched. These are short startup/attract sequences; clock scaling
and host load affect exact numbers.

The September 9 profile still attributed 15–19% of cycles to CRC-32 and 7–8%
to Mapper 66 CPU/PPU reads. Processing eight CRC bytes per iteration and caching
Mapper 66 bank offsets gave the following normal-history results on the same
i7-1355U, using GCC 16.2 `-O2`, the powersave governor and CPU 2 affinity. Values
are medians of three 240-frame runs per build, alternating build order between
rounds after each run's 120-frame warmup:

| ROM | Before (ms/frame) | After (ms/frame) | CPU time reduction |
| --- | ---: | ---: | ---: |
| Super Mario Bros. + Duck Hunt (Mapper 66) | 7.547 | 5.245 | 30.5% |
| Super Mario Bros. (NROM) | 6.916 | 6.010 | 13.1% |
| Super Mario Bros. 3 (MMC3) | 6.678 | 5.801 | 13.1% |

Median checksum/diagnostic time fell from 1.54–1.71 to 0.27–0.29 ms/frame.
Cycle counts and final image CRCs matched across both builds and all three
diagnostic modes. These measurements cover startup/attract sequences and exclude
the frontend; host load and clock scaling still caused variation between runs.

A follow-up PPU pass simplifies `ppu_render_pixel`, which is inlined into
`ppu_step`: forced blanking returns early, clipped sprites skip selection, each
sprite uses one horizontal range check, and the first opaque sprite is composed
directly against the background. No additional PPU state or serialized fields
are needed. Fetches, OAM evaluation, mapper callbacks and dot advancement keep
their existing schedule.

An isolated comparison linked the previous and optimized PPU implementations
into one process and alternated their order every 20 blocks of 89,342 dots, with
400 measured blocks per implementation and rendering mode. The synthetic fixture
used patterned CHR/nametables and groups of eight 8x16 sprites:

| Rendering mode | Before (ms/89,342 dots) | After (ms/89,342 dots) | Reduction |
| --- | ---: | ---: | ---: |
| Forced blank | 1.501 | 1.338 | 10.9% |
| Background | 3.518 | 2.813 | 20.0% |
| Sprites | 3.597 | 3.236 | 10.0% |
| Background and sprites | 3.994 | 3.346 | 16.2% |

The existing ROM benchmark also showed 8.1–9.4% lower detached core time across
the same three ROMs (medians of three 600-frame runs, CPU 2, GCC 16.2 `-O2`).
Separate-process results varied more with tracing and host load, so these
measurements should not be treated as a guaranteed reduction for every mode.
Paired runs within one process alternated the old/new PPU every 20 frames,
reversing order between blocks, for 600 measured frames per implementation and
diagnostic mode. A common dispatch wrapper selected the PPU implementation for
each dot; its overhead is included for both builds. These runs reduced CPU time
by 5.0–9.8% across all three ROMs and modes, including 7.1% for SMB3 with tracing
(the separate-process tracing median had been 5.5% slower). Every measured frame
and audio-buffer CRC and CPU cycle count matched. Normal-history results were:

| ROM | Before (ms/frame) | After (ms/frame) | Reduction |
| --- | ---: | ---: | ---: |
| Super Mario Bros. + Duck Hunt | 5.345 | 4.861 | 9.1% |
| Super Mario Bros. | 5.922 | 5.344 | 9.8% |
| Super Mario Bros. 3 | 6.095 | 5.629 | 7.6% |

A comparison against the previous PPU passed two million sequential dots with
rendering/register changes plus one million seeded visible dots, checking pixels,
internal state, NMI state and mapper bus-address traces. `ppu_pixels.c` adds
permanent coverage for sprite composition, clipping, hit flags and forced blanking.

The original SDL frontend batched glyph pixels into one draw call. Its local
1280x1200 software-renderer comparison measured the overlay at 1.68 versus 1.31
ms/frame and an outlined notification at 3.13 versus 1.62 ms/frame, with identical
output image CRCs. Accelerated drivers may have different costs.

Sokol now composites glyphs in the CPU overlay before uploading it once per
frame. Those historical SDL measurements do not describe the Sokol backend.

References: [Sokol headers](https://github.com/floooh/sokol),
[historical SDL logical rendering](https://wiki.libsdl.org/SDL2/SDL_RenderSetLogicalSize),
[window-to-logical coordinates](https://wiki.libsdl.org/SDL2/SDL_RenderWindowToLogical),
[SDL audio queue measurement](https://wiki.libsdl.org/SDL2/SDL_GetQueuedAudioSize),
[Zapper measurements](https://www.nesdev.org/wiki/Zapper).
