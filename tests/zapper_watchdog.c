#include "nes_system.h"
#include <assert.h>
#include <stdio.h>

static NES nes;

static void polling_frame(unsigned reads, bool mixed) {
    for (unsigned i = 0; i < reads; i++) {
        nes.cpu.program_counter = 0x8003 + (mixed ? (i & 1) : 0);
        (void)nes_cpu_bus_read(&nes, 0x4017);
    }
    nes_check_zapper_stall(&nes);
}

static void no_warning(unsigned reads, bool mixed) {
    nes_reset_zapper_watchdog(&nes);
    for (int frame = 0; frame < 200; frame++) {
        polling_frame(reads, mixed);
        assert(!nes.zapper_watchdog.stalled);
    }
}

int main(void) {
    nes_init(&nes);
    no_warning(800, false); // Standard controller mode.
    nes.zapper_enabled = true;
    no_warning(8, false); // Normal once-per-frame controller read.
    no_warning(800, true); // Program is visiting different polling instructions.
    nes.ppu.ppu_mask = 0x18;
    no_warning(800, false); // Normal visible gameplay.
    nes.ppu.ppu_mask = 0;
    nes.zapper_trigger = true;
    no_warning(800, false); // An active shot.
    nes.zapper_trigger = false;
    nes.ppu.screen_buffer[100] = 0xFFFFFFFF;
    no_warning(800, false); // Display is not entirely black.
    nes.ppu.screen_buffer[100] = 0;

    for (int frame = 0; frame < 179; frame++) {
        polling_frame(800, false);
        assert(!nes.zapper_watchdog.stalled);
    }
    polling_frame(800, false);
    assert(nes.zapper_watchdog.stalled);
    polling_frame(800, false);
    assert(nes.zapper_watchdog.stalled); // Remains visible until the hang clears.
    nes.zapper_enabled = false;
    polling_frame(800, false);
    assert(!nes.zapper_watchdog.stalled);

    nes.zapper_enabled = true;
    for (int frame = 0; frame < 120; frame++) polling_frame(800, false);
    polling_frame(0, false); // A quiet frame breaks the consecutive-hang window.
    for (int frame = 0; frame < 120; frame++) polling_frame(800, false);
    assert(!nes.zapper_watchdog.stalled);
    nes_reset(&nes);
    assert(nes.zapper_watchdog.stalled_frames == 0);
    assert(nes.zapper_watchdog.reads == 0);
    puts("Zapper hang detection, recovery and false-positive checks passed.");
    return 0;
}
