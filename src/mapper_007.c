#include "mappers.h"
#include "cartridge.h"
#include <stdlib.h>

typedef struct {
    uint8_t prg_bank;
} AxROMData;

static void axrom_reset(Cartridge *c) {
    AxROMData *d = (AxROMData*)c->mapper_data;
    d->prg_bank = 0;
    c->mirroring = MIRROR_ONE_SCREEN_LOW;
}

static void axrom_destroy(Cartridge *c) {
    free(c->mapper_data);
    c->mapper_data = NULL;
}

static uint8_t axrom_cpu_read(Cartridge *c, uint16_t addr, bool *handled) {
    AxROMData *d = (AxROMData*)c->mapper_data;

    if (addr >= 0x6000 && addr <= 0x7FFF) {
        *handled = true;
        return (c->prg_ram && c->prg_ram_size > 0) ? c->prg_ram[addr - 0x6000] : 0;
    }

    if (addr >= 0x8000) {
        *handled = true;
        uint32_t total_32k = c->prg_rom_size / 32768;
        if (total_32k == 0) return 0;

        uint32_t bank = d->prg_bank % total_32k;
        uint32_t offset = (bank * 32768) + (addr - 0x8000);
        return c->prg_rom[offset % c->prg_rom_size];
    }

    return 0;
}

static void axrom_cpu_write(Cartridge *c, uint16_t addr, uint8_t val) {
    AxROMData *d = (AxROMData*)c->mapper_data;

    if (addr >= 0x6000 && addr <= 0x7FFF) {
        if (c->prg_ram && c->prg_ram_size > 0) {
            c->prg_ram[addr - 0x6000] = val;
        }
        return;
    }

    if (addr >= 0x8000) {
        d->prg_bank = val & 0x07;
        c->mirroring = (val & 0x10) ? MIRROR_ONE_SCREEN_HIGH : MIRROR_ONE_SCREEN_LOW;
    }
}

static uint8_t axrom_ppu_read(Cartridge *c, uint16_t addr, bool *handled) {
    if (addr < 0x2000 && c->chr_rom_size > 0) {
        *handled = true;
        return c->chr_rom[addr & 0x1FFF];
    }
    return 0;
}

static void axrom_ppu_write(Cartridge *c, uint16_t addr, uint8_t val) {
    if (addr < 0x2000 && c->chr_rom_size > 0) {
        c->chr_rom[addr & 0x1FFF] = val;
    }
}

static uint16_t axrom_remap_ciram_addr(Cartridge *c, uint16_t addr, bool *ciram_ce) {
    *ciram_ce = true;
    return cartridge_default_remap_ciram(c->mirroring, addr);
}

static const MapperInterface axrom_interface = {
    .reset = axrom_reset,
    .destroy = axrom_destroy,
    .cpu_read = axrom_cpu_read,
    .cpu_write = axrom_cpu_write,
    .ppu_read = axrom_ppu_read,
    .ppu_write = axrom_ppu_write,
    .ppu_addr_change = NULL,
    .clock_m2 = NULL,
    .remap_ciram_addr = axrom_remap_ciram_addr
};

void mapper_007_init(Cartridge *cart) {
    AxROMData *data = calloc(1, sizeof(AxROMData));
    cart->mapper_data = data;
    cart->vtable = &axrom_interface;
    axrom_reset(cart);
}