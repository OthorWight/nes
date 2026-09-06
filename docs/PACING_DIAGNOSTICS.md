# Pacing, input and diagnostic captures

Gameplay uses high-resolution fractional deadlines based on the CPU cycles
actually emulated, at the core's NTSC clock of 1,789,773 Hz. Audio enabled, muted,
and unavailable share this scheduler. A host stall more than 50 ms beyond the
deadline discards the backlog. Menus, debugger transitions, focus loss, ROM loads
and state loads rebase time.

## Controls and measurements

| Control | Action |
| --- | --- |
| F2 | Toggle the compact performance display; saved globally |
| Ctrl+F4 | Toggle bounded event tracing |
| F4 | Write the recent capture, including from menus/debugger |
| F3 | Existing console debug display |
| F6 in debugger | Existing instruction log, independent of event tracing |

Settings also contains Performance and Event trace toggles. The overlay summarizes
up to 180 completed gameplay frames: FPS, emulated CPU time as a percentage of
host time, maximum frame interval, intervals above 25 ms, audio queue depth,
observed empty queues, controller reads and image changes. Image checksums use
the NES framebuffer before OSD drawing. Menu/debugger redraws do not count.

At roughly 100% speed, repeated images or fewer controller reads can help
investigate slowdown inside a game. These signals do not prove that game logic is
stalled: static scenes can repeat, polling frequency varies, and animations may
continue while other logic waits.

## Audio

Playback starts paused and primes with at least 1,470 mono samples (33.3 ms).
The queue is limited to 2,940 samples (66.7 ms); excess backlog is cleared and
primed again. These replace the old 4,096-sample (92.9 ms) threshold and blocking
wait. Core samples are drained even when muted or the audio device cannot open.
Samples arrive in frame-sized batches, so startup priming can reach about 50 ms.
The requested device buffer is 1,024 samples (23.2 ms). In the two-second dummy
driver comparison, the earlier 512-sample buffer recorded two empty-queue
indicators; 1,024 samples recorded none, with no queue trims. All three audio
modes measured 60.10 FPS and 100.00% speed. This is a local buffering baseline,
not a physical audio-device certification.

Intentional pauses clear and pause playback. Resume starts with fresh samples.
An empty queue during active playback increments a counter and re-primes audio.
Queue trims and SDL queue errors are counted separately. Intentional pauses do
not count as underruns. Volume previews may play in Settings and are discarded
when gameplay resumes.

Queue depth measures samples waiting in SDL. Captures also report device buffer
duration. Neither measures speaker latency: SDL cannot report exactly how much
audio the device has already played. An observed empty queue is an underrun
indicator, not proof of audible loss.

## Preferences, input and aim

Window scale/maximization, fullscreen and Port 2 are saved per ROM identity.
Settings labels the scope and provides **Use global display/Port 2** and **Make
display/Port 2 global**. ROMs without overrides use the global defaults. Audio,
control bindings, console debug and the performance display remain global.

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

Aim uses SDL's window-to-logical conversion and the same source crop as rendering.
Position is refreshed each loop, including after a resize with a stationary mouse.
Letterbox/outside-window aim is offscreen, rather than clamped to a bright edge.
SDL 2.0.18 or newer is required for this coordinate conversion.

## Captures and debugger reads

F4 replaces `saves/<ROM name>/diagnostics.log`. It includes build date/time, ROM
identity, mapper/submapper, region, Port 2, performance/audio summary, up to 180
frame records and 4,096 events. Frame records include CPU cycles, host interval,
core execution time, queue depth, per-port read counts, controller latches, held
buttons, framebuffer checksum, aim and trigger.

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

`bash tests/sdl/run.sh` builds the real frontend with scripted events, virtual
gamepad and SDL dummy video/audio drivers. It uses an isolated `build/tests/`
directory and preserves captures. It checks focus, disconnect, menu/debugger
releases, ROM overrides, disassembly, resized/fullscreen coordinates and measured
pacing in all audio modes. Dummy drivers do not validate a physical window
manager, high-DPI monitor or speaker latency. Native Windows and gameplay remain
manual checks.

The optional suite also runs a white-raster Zapper probe using a short 6502
polling program. The current sensor reports light for the entire white frame;
it does not reproduce the roughly 19–26 scanline decay measured by Zap Ruder on
hardware. Beam-aware sensing needs a separate compatibility change and sensor
state serialization. The probe reports this discrepancy explicitly; it does not
pass the current behavior as hardware-accurate.

References: [SDL logical rendering](https://wiki.libsdl.org/SDL2/SDL_RenderSetLogicalSize),
[window-to-logical coordinates](https://wiki.libsdl.org/SDL2/SDL_RenderWindowToLogical),
[SDL audio queue measurement](https://wiki.libsdl.org/SDL2/SDL_GetQueuedAudioSize),
[Zapper measurements](https://www.nesdev.org/wiki/Zapper).
