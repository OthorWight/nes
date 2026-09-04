#include "mappers.h"
#include "cartridge.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t bank0;
    uint8_t bank1;
} M227Data;

static void m227_update_banks(Cartridge *c, uint16_t addr) {
    M227Data *d = (M227Data*)c->mapper_data;
    uint32_t prg = ((addr >> 2) & 0x1F) | ((addr & 0x0100) >> 3);

    uint32_t bank0 = 0;
    uint32_t bank1 = 0;

    if (addr & 0x0080) { // Mode 1 (A7 = 1)
        if (addr & 0x0001) { // S = 1 (A0 = 1)
            bank0 = prg;
            bank1 = (prg & 0x38) | ((addr & 0x0200) ? 7 : 0);
        } else { // S = 0 (A0 = 0)
            bank0 = prg;
            bank1 = prg;
        }
    } else { // Mode 0 (A7 = 0)
        if (addr & 0x0001) { // S = 1 (A0 = 1) -> 32 KB mode
            bank0 = prg & ~1;
            bank1 = (prg & ~1) | 1;
        } else { // S = 0 (A0 = 0) -> 16 KB mode with fixed bank
            bank0 = prg;
            bank1 = (prg & 0x38) | ((addr & 0x0200) ? 7 : 0);
        }
    }

    uint32_t total_banks = c->prg_rom_size / 16384;
    if (total_banks > 0) {
        bank0 %= total_banks;
        bank1 %= total_banks;
    }

    d->bank0 = (uint8_t)bank0;
    d->bank1 = (uint8_t)bank1;
    c->mirroring = (addr & 0x0002) ? MIRROR_HORIZONTAL : MIRROR_VERTICAL;
}

static void m227_reset(Cartridge *c) {
    M227Data *d = (M227Data*)c->mapper_data;
    memset(d, 0, sizeof(M227Data));
    m227_update_banks(c, 0x0000);
}

static void m227_destroy(Cartridge *c) {
    free(c->mapper_data);
    c->mapper_data = NULL;
}

static uint8_t m227_cpu_read(Cartridge *c, uint16_t addr, bool *handled) {
    M227Data *d = (M227Data*)c->mapper_data;

    if (addr >= 0x6000 && addr <= 0x7FFF) {
        *handled = true;
        return (c->prg_ram && c->prg_ram_size > 0) ? c->prg_ram[addr - 0x6000] : 0;
    }

    if (addr >= 0x8000) {
        *handled = true;
        uint32_t bank = (addr < 0xC000) ? d->bank0 : d->bank1;
        return c->prg_rom[(bank * 16384 + (addr & 0x3FFF)) % c->prg_rom_size];
    }

    return 0;
}

static void m227_cpu_write(Cartridge *c, uint16_t addr, uint8_t val) {
    (void)val;
    if (addr >= 0x6000 && addr <= 0x7FFF) {
        if (c->prg_ram && c->prg_ram_size > 0) {
            c->prg_ram[addr - 0x6000] = val;
        }
        return;
    }

    if (addr >= 0x8000) {
        m227_update_banks(c, addr);
    }
}

static uint8_t m227_ppu_read(Cartridge *c, uint16_t addr, bool *handled) {
    if (addr < 0x2000 && c->chr_rom_size > 0) {
        *handled = true;
        return c->chr_rom[addr & 0x1FFF];
    }
    return 0;
}

static void m227_ppu_write(Cartridge *c, uint16_t addr, uint8_t val) {
    if (addr < 0x2000 && c->chr_rom_size > 0) {
        c->chr_rom[addr & 0x1FFF] = val;
    }
}

static uint16_t m227_remap_ciram_addr(Cartridge *c, uint16_t addr, bool *ciram_ce) {
    *ciram_ce = true;
    return cartridge_default_remap_ciram(c->mirroring, addr);
}

static const MapperInterface m227_interface = {
    .reset = m227_reset,
    .destroy = m227_destroy,
    .cpu_read = m227_cpu_read,
    .cpu_write = m227_cpu_write,
    .ppu_read = m227_ppu_read,
    .ppu_write = m227_ppu_write,
    .ppu_addr_change = NULL,
    .clock_m2 = NULL,
    .remap_ciram_addr = m227_remap_ciram_addr
};

void mapper_227_init(Cartridge *cart) {
    M227Data *data = calloc(1, sizeof(M227Data));
    cart->mapper_data = data;
    cart->vtable = &m227_interface;
    m227_reset(cart);
}