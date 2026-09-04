#include "mappers.h"
#include "cartridge.h"
#include <stdlib.h>

typedef struct {
    uint8_t prg_bank;
    uint8_t chr_bank;
} GxROMData;

static void m066_reset(Cartridge *c) {
    GxROMData *d = (GxROMData*)c->mapper_data;
    d->prg_bank = 0;
    d->chr_bank = 0;
}

static void m066_destroy(Cartridge *c) {
    free(c->mapper_data);
    c->mapper_data = NULL;
}

static uint8_t m066_cpu_read(Cartridge *c, uint16_t addr, bool *handled) {
    GxROMData *d = (GxROMData*)c->mapper_data;

    if (addr >= 0x8000) {
        *handled = true;
        uint32_t total_32k = c->prg_rom_size / 32768;
        if (total_32k == 0) return 0;

        uint32_t bank = d->prg_bank % total_32k;
        uint32_t offset = bank * 32768 + (addr - 0x8000);
        return c->prg_rom[offset % c->prg_rom_size];
    }

    return 0;
}

static void m066_cpu_write(Cartridge *c, uint16_t addr, uint8_t val) {
    GxROMData *d = (GxROMData*)c->mapper_data;

    if (addr >= 0x8000) {
        d->prg_bank = (val >> 4) & 0x03;
        d->chr_bank = val & 0x03;
    }
}

static uint8_t m066_ppu_read(Cartridge *c, uint16_t addr, bool *handled) {
    GxROMData *d = (GxROMData*)c->mapper_data;

    if (addr < 0x2000 && c->chr_rom_size > 0) {
        *handled = true;
        uint32_t total_8k = c->chr_rom_size / 8192;
        if (total_8k == 0) return 0;

        uint32_t bank = d->chr_bank % total_8k;
        uint32_t offset = bank * 8192 + (addr & 0x1FFF);
        return c->chr_rom[offset % c->chr_rom_size];
    }

    return 0;
}

static void m066_ppu_write(Cartridge *c, uint16_t addr, uint8_t val) {
    GxROMData *d = (GxROMData*)c->mapper_data;

    if (addr < 0x2000 && c->chr_rom_size > 0) {
        uint32_t total_8k = c->chr_rom_size / 8192;
        if (total_8k == 0) return;

        uint32_t bank = d->chr_bank % total_8k;
        uint32_t offset = bank * 8192 + (addr & 0x1FFF);
        c->chr_rom[offset % c->chr_rom_size] = val;
    }
}

static uint16_t m066_remap_ciram_addr(Cartridge *c, uint16_t addr, bool *ciram_ce) {
    *ciram_ce = true;
    return cartridge_default_remap_ciram(c->mirroring, addr);
}

static const MapperInterface m066_interface = {
    .reset = m066_reset,
    .destroy = m066_destroy,
    .cpu_read = m066_cpu_read,
    .cpu_write = m066_cpu_write,
    .ppu_read = m066_ppu_read,
    .ppu_write = m066_ppu_write,
    .ppu_addr_change = NULL,
    .clock_m2 = NULL,
    .remap_ciram_addr = m066_remap_ciram_addr
};

void mapper_066_init(Cartridge *cart) {
    GxROMData *data = calloc(1, sizeof(GxROMData));
    cart->mapper_data = data;
    cart->vtable = &m066_interface;
    m066_reset(cart);
}