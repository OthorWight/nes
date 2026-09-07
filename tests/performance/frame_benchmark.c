#include "test_system.h"
#include "diagnostics.h"
#include "state_io.h"
#include <stdlib.h>
#include <time.h>

// Unpaced CPU time, excluding SDL and sleeps. No timing assertions: host clocks
// and power profiles vary. An optional ROM exercises its actual mapper/core.
static TestSystem system_under_test;
static NESDiagnostics history;

static void frame(NES *n) {
    n->frame_ready = false;
    unsigned instructions = 0;
    while (!n->frame_ready && instructions++ < 100000) nes_clock_tick(n);
    if (!n->frame_ready) {
        fputs("Core did not complete a frame within 100000 instructions.\n", stderr);
        exit(1);
    }
    n->apu.audio_buffer_idx = 0;
}

int main(int argc, char **argv) {
    unsigned frames = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 10) : 600;
    if (!frames) return 2;
    for (unsigned mode = 0; mode < 3; ++mode) {
        test_system_init(&system_under_test);
        NES *n = &system_under_test.nes;
        bool loaded = argc > 1;
        if (loaded) {
            n->cart = cartridge_load(n, argv[1]);
            if (!n->cart) return 1;
            nes_reset(n);
        } else {
            test_ram_idle(&system_under_test);
            n->ppu.ppu_mask = 0x1E;
            memset(n->ppu.oam_ram, 0xFF, sizeof(n->ppu.oam_ram));
        }
        for (unsigned i = 0; i < 120; ++i) frame(n);
        memset(&history, 0, sizeof(history));
        history.tracing = mode == 2;
        n->diagnostics = mode ? &history : NULL;
        clock_t start = clock();
        double core = 0, diagnostic = 0;
        uint64_t cycles_start = n->cpu.cycle_count;
        for (unsigned i = 0; i < frames; ++i) {
            clock_t before = clock();
            uint64_t cycles = n->cpu.cycle_count;
            frame(n);
            clock_t after = clock();
            diagnostics_frame(n, n->cpu.cycle_count - cycles, 1.0 / 60, 0, 0);
            core += (double)(after - before) / CLOCKS_PER_SEC;
            diagnostic += (double)(clock() - after) / CLOCKS_PER_SEC;
        }
        double elapsed = (double)(clock() - start) / CLOCKS_PER_SEC;
        printf("%s frames=%u core=%.3fms diagnostics=%.3fms total=%.3fms cycles=%llu image=%08X\n",
               mode == 0 ? "detached" : mode == 1 ? "normal" : "tracing", frames,
               core * 1000 / frames, diagnostic * 1000 / frames, elapsed * 1000 / frames,
               (unsigned long long)(n->cpu.cycle_count - cycles_start),
               state_crc32(n->ppu.screen_buffer, sizeof(n->ppu.screen_buffer)));
        if (loaded) cartridge_free(n->cart);
    }
    return 0;
}
