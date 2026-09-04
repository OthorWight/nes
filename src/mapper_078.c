#include "mappers.h"
#include "cartridge.h"
#include <stdlib.h>

typedef struct {
    uint8_t prg_bank;
    uint8_t chr_bank;
} M078Data;

static void m078_reset(Cartridge *c) {
    M078Data *d = (M078Data*)c->mapper_data;
    d->prg_bank = 0;
    d->chr_bank = 0;
}

static void m078_destroy(Cartridge *c) {
    free(c->mapper_data);
    c->mapper_data = NULL;
}

static uint8_t m078_cpu_read(Cartridge *c, uint16_t addr, bool *handled) {
    M078Data *d = (M078Data*)c->mapper_data;

    if (addr >= 0x8000) {
        *handled = true;
        uint32_t total_16k = c->prg_rom_size / 16384;
        if (total_16k == 0) return 0;

        uint32_t bank = (addr < 0xC000) ? (d->prg_bank % total_16k) : (total_16k - 1);
        return c->prg_rom[bank * 16384 + (addr & 0x3FFF)];
    }
    return 0;
}

static void m078_cpu_write(Cartridge *c, uint16_t addr, uint8_t val) {
    M078Data *d = (M078Data*)c->mapper_data;

    if (addr >= 0x8000) {
        d->prg_bank = val & 0x07;
        d->chr_bank = (val >> 4) & 0x0F;
        c->mirroring = (val & 0x08) ? MIRROR_VERTICAL : MIRROR_HORIZONTAL;
    }
}

static uint8_t m078_ppu_read(Cartridge *c, uint16_t addr, bool *handled) {
    M078Data *d = (M078Data*)c->mapper_data;

    if (addr < 0x2000 && c->chr_rom_size > 0) {
        *handled = true;
        uint32_t total_8k = c->chr_rom_size / 8192;
        if (total_8k == 0) return 0;

        uint32_t offset = (d->chr_bank % total_8k) * 8192 + (addr & 0x1FFF);
        return c->chr_rom[offset % c->chr_rom_size];
    }
    return 0;
}

static void m078_ppu_write(Cartridge *c, uint16_t addr, uint8_t val) {
    M078Data *d = (M078Data*)c->mapper_data;

    if (addr < 0x2000 && c->chr_rom_size > 0) {
        uint32_t total_8k = c->chr_rom_size / 8192;
        if (total_8k == 0) return;

        uint32_t offset = (d->chr_bank % total_8k) * 8192 + (addr & 0x1FFF);
        c->chr_rom[offset % c->chr_rom_size] = val;
    }
}

static uint16_t m078_remap_ciram_addr(Cartridge *c, uint16_t addr, bool *ciram_ce) {
    *ciram_ce = true;
    return cartridge_default_remap_ciram(c->mirroring, addr);
}

static const MapperInterface m078_interface = {
    .reset = m078_reset,
    .destroy = m078_destroy,
    .cpu_read = m078_cpu_read,
    .cpu_write = m078_cpu_write,
    .ppu_read = m078_ppu_read,
    .ppu_write = m078_ppu_write,
    .ppu_addr_change = NULL,
    .clock_m2 = NULL,
    .remap_ciram_addr = m078_remap_ciram_addr
};

void mapper_078_init(Cartridge *cart) {
    M078Data *data = calloc(1, sizeof(M078Data));
    cart->mapper_data = data;
    cart->vtable = &m078_interface;
    m078_reset(cart);
}