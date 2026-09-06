#include "nes_system.h"
#include <assert.h>
#include <stdio.h>

static NES nes;

// A CPU absolute read fetches the $40 operand byte before reading $401x.
static uint8_t read_port(uint16_t address) {
    nes.wram[0] = 0x40;
    (void)nes_cpu_bus_read(&nes, 0);
    return nes_cpu_bus_read(&nes, address);
}

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
        assert(read_port(0x4016) == (0x40 | ((0xA5 >> bit) & 1)));
        assert(read_port(0x4017) == (0x40 | ((0x5A >> bit) & 1)));
    }
    assert(read_port(0x4017) == 0x41);

    nes.controller_state[1] = 1;
    nes_cpu_bus_write(&nes, 0x4016, 1);
    assert(read_port(0x4017) == 0x41);
    assert(read_port(0x4017) == 0x41);

    nes.zapper_enabled = true;
    nes.zapper_trigger = false;
    nes.controller_state[1] = 0;
    assert(read_port(0x4017) == 0x48); // Dark, trigger released.
    nes.zapper_trigger = true;
    assert(read_port(0x4017) == 0x58);
    nes.ppu.screen_buffer[0] = 0xFFFFFFFF;
    assert(read_port(0x4017) == 0x50); // Light detected.
    nes.zapper_trigger = false;
    assert(read_port(0x4017) == 0x40);
    nes.zapper_x = -1;
    assert(read_port(0x4017) == 0x48); // Aim offscreen.

    nes.zapper_enabled = false;
    assert(read_port(0x4017) == 0x40);
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
