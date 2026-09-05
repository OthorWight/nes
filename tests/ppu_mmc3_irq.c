#include "nes_system.h"
#include "mappers.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static NES nes;
static Cartridge cart;
static uint8_t chr[8192];

static void check_split(void (*mapper_init)(Cartridge *), bool background_high,
                        bool odd_frame) {
    nes_init(&nes);
    memset(&cart, 0, sizeof(cart));
    cart.nes = &nes;
    cart.chr_rom = chr;
    cart.chr_rom_size = sizeof(chr);
    nes.cart = &cart;
    mapper_init(&cart);
    memset(nes.ppu.oam_ram, 0xFF, sizeof(nes.ppu.oam_ram));
    nes.ppu.ppu_ctrl = background_high ? 0x10 : 0x08;
    nes.ppu.ppu_mask = 0x18;
    nes.ppu.odd_frame = odd_frame;
    nes.ppu.scanline = 261;

    // Warm up through the pre-render boundary, then request four scanline clocks.
    while (nes.ppu.scanline != 20 || nes.ppu.cycle != 0) ppu_step(&nes);
    cart.vtable->cpu_write(&cart, 0xC000, 3);
    cart.vtable->cpu_write(&cart, 0xC001, 0);
    cart.vtable->cpu_write(&cart, 0xE001, 0);

    int irq_scanline = -1;
    int irq_dot = -1;
    while (nes.ppu.scanline < 25) {
        int scanline = nes.ppu.scanline;
        int dot = nes.ppu.cycle;
        ppu_step(&nes);
        if (nes.lines.irq_line) {
            irq_scanline = scanline;
            irq_dot = dot;
            break;
        }
    }
    // A duplicate count at the start of each line would fire around line 21.
    assert(irq_scanline == 23);
    assert(irq_dot >= (background_high ? 320 : 256));
    assert(irq_dot < (background_high ? 332 : 268));

    cart.vtable->cpu_write(&cart, 0xE000, 0);
    assert(!nes.lines.irq_line);
    assert(!(nes.cpu.irq_lines & 1));
    while (nes.ppu.scanline < 26) {
        ppu_step(&nes);
        assert(!nes.lines.irq_line);
    }
    cart.vtable->destroy(&cart);
}

int main(void) {
    for (int odd = 0; odd < 2; odd++) {
        for (int background_high = 0; background_high < 2; background_high++) {
            check_split(mapper_004_init, background_high, odd);
            check_split(mapper_118_init, background_high, odd);
        }
    }
    puts("MMC3/TxSROM split timing passed with both pattern-table layouts and frame parities.");
    return 0;
}
