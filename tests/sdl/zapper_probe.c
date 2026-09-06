// Characterization probe, not a hardware-accuracy pass/fail test.
#include "nes_system.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    NES *n = malloc(sizeof(*n)); assert(n); nes_init(n);
    n->zapper_enabled = true; n->zapper_x = 128; n->zapper_y = 120;
    n->ppu.palette_ram[0] = 0x30; // Continuous white backdrop.
    // LDA $4017; STA $10; JMP $0200. Run actual CPU reads across the raster.
    const uint8_t program[] = {0xAD,0x17,0x40,0x85,0x10,0x4C,0x00,0x02};
    memcpy(n->wram + 0x200, program, sizeof(program));
    n->cpu.program_counter = 0x200;
    unsigned frames = 0, light_scanlines = 0, samples = 0;
    int previous_line = -1;
    for (unsigned steps = 0; steps < 100000 && frames < 3; ++steps) {
        nes_clock_tick(n);
        if (n->frame_ready) { n->frame_ready = false; ++frames; }
        if (frames == 2 && n->ppu.scanline != previous_line) {
            previous_line = n->ppu.scanline;
            ++samples;
            if (!(n->wram[0x10] & 8)) ++light_scanlines;
        }
    }
    assert(frames == 3 && samples == 262);
    printf("Zapper white-raster probe: light active on %u/%u sampled scanlines.\n"
           "Hardware reference: roughly 26 scanlines after white illumination (NESdev/Zap Ruder).\n"
           "This reports the existing sensor model; it is not a hardware-accuracy pass.\n",
           light_scanlines, samples);
    free(n);
    return 0;
}
