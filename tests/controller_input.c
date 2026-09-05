#include "nes_system.h"
#include <assert.h>
#include <stdio.h>

static NES nes;

int main(void) {
    nes_init(&nes);
    assert(!nes.zapper_enabled);

    // Mouse state must not leak onto the bus with standard controllers selected.
    nes.zapper_trigger = true;
    nes.controller_state[0] = 0xA5;
    nes.controller_state[1] = 0x5A;
    nes_cpu_bus_write(&nes, 0x4016, 1);
    nes_cpu_bus_write(&nes, 0x4016, 0);
    for (int bit = 0; bit < 8; bit++) {
        assert(nes_cpu_bus_read(&nes, 0x4016) == (0x40 | ((0xA5 >> bit) & 1)));
        assert(nes_cpu_bus_read(&nes, 0x4017) == (0x40 | ((0x5A >> bit) & 1)));
    }
    assert(nes_cpu_bus_read(&nes, 0x4017) == 0x41);

    nes.controller_state[1] = 1;
    nes_cpu_bus_write(&nes, 0x4016, 1);
    assert(nes_cpu_bus_read(&nes, 0x4017) == 0x41);
    assert(nes_cpu_bus_read(&nes, 0x4017) == 0x41);

    nes.zapper_enabled = true;
    nes.zapper_trigger = false;
    nes.controller_state[1] = 0;
    assert(nes_cpu_bus_read(&nes, 0x4017) == 0x48); // Dark, trigger released.
    nes.zapper_trigger = true;
    assert(nes_cpu_bus_read(&nes, 0x4017) == 0x58);
    nes.ppu.screen_buffer[0] = 0xFFFFFFFF;
    assert(nes_cpu_bus_read(&nes, 0x4017) == 0x50); // Light detected.
    nes.zapper_trigger = false;
    assert(nes_cpu_bus_read(&nes, 0x4017) == 0x40);
    nes.zapper_x = -1;
    assert(nes_cpu_bus_read(&nes, 0x4017) == 0x48); // Aim offscreen.

    nes.zapper_enabled = false;
    assert(nes_cpu_bus_read(&nes, 0x4017) == 0x40);
    nes_reset(&nes);
    assert(!nes.zapper_enabled);
    nes.zapper_enabled = true;
    nes_reset(&nes);
    assert(nes.zapper_enabled);
    nes_init(&nes);
    assert(!nes.zapper_enabled);

    puts("Controller serial reads, Zapper selection, light and trigger checks passed.");
    return 0;
}
