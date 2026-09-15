# emu2413

The user supplied `emu2413.c`, `emu2413.h`, and `LICENSE` from
https://github.com/digital-sound-antiques/emu2413 on 2026-09-15.
The source identifies itself as version 1.5.9. The exact upstream commit was
not recorded; these checked-in files are the build inputs.

License: MIT, Copyright Mitsutaka Okazaki; see [LICENSE](LICENSE).

`src/vrc7_audio.c` includes the upstream implementation once and adapts it to
the NES CPU clock and save-state codec. Compile that wrapper, not a second copy
of `emu2413.c`. The wrapper uses native FM ticks and the NES output resampler.
