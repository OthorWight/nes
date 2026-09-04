#include "mappers.h"
#include "cartridge.h"
#include <stdlib.h>

typedef struct {
    uint8_t prg_bank;
} UxROMData;

static void uxrom_reset(Cartridge *c) {
    UxROMData *d = (UxROMData*)c->mapper_data;
    d->prg_bank = 0;
}

static void uxrom_destroy(Cartridge *c) {
    free(c->mapper_data);
    c->mapper_data = NULL;
}

static uint8_t uxrom_cpu_read(Cartridge *c, uint16_t addr, bool *handled) {
    UxROMData *d = (UxROMData*)c->mapper_data;

    if (addr >= 0x6000 && addr <= 0x7FFF) {
        *handled = true;
        return (c->prg_ram && c->prg_ram_size > 0) ? c->prg_ram[addr - 0x6000] : 0;
    }

    if (addr >= 0x8000) {
        *handled = true;
        uint32_t total_16k = c->prg_rom_size / 16384;
        if (total_16k == 0) return 0;

        uint32_t bank = (addr < 0xC000) ? d->prg_bank : (total_16k - 1);
        uint32_t offset = (bank % total_16k) * 16384 + (addr & 0x3FFF);
        return c->prg_rom[offset % c->prg_rom_size];
    }

    return 0;
}

static void uxrom_cpu_write(Cartridge *c, uint16_t addr, uint8_t val) {
    UxROMData *d = (UxROMData*)c->mapper_data;

    if (addr >= 0x6000 && addr <= 0x7FFF) {
        if (c->prg_ram && c->prg_ram_size > 0) {
            c->prg_ram[addr - 0x6000] = val;
        }
        return;
    }

    if (addr >= 0x8000) {
        uint32_t total_16k = c->prg_rom_size / 16384;
        if (total_16k > 0) {
            d->prg_bank = val % total_16k;
        }
    }
}

static uint8_t uxrom_ppu_read(Cartridge *c, uint16_t addr, bool *handled) {
    if (addr < 0x2000 && c->chr_rom_size > 0) {
        *handled = true;
        return c->chr_rom[addr & 0x1FFF];
    }
    return 0;
}

static void uxrom_ppu_write(Cartridge *c, uint16_t addr, uint8_t val) {
    if (addr < 0x2000 && c->chr_rom_size > 0) {
        c->chr_rom[addr & 0x1FFF] = val;
    }
}

static uint16_t uxrom_remap_ciram_addr(Cartridge *c, uint16_t addr, bool *ciram_ce) {
    *ciram_ce = true;
    return cartridge_default_remap_ciram(c->mirroring, addr);
}

static const MapperInterface uxrom_interface = {
    .reset = uxrom_reset,
    .destroy = uxrom_destroy,
    .cpu_read = uxrom_cpu_read,
    .cpu_write = uxrom_cpu_write,
    .ppu_read = uxrom_ppu_read,
    .ppu_write = uxrom_ppu_write,
    .ppu_addr_change = NULL,
    .clock_m2 = NULL,
    .remap_ciram_addr = uxrom_remap_ciram_addr
};

void mapper_002_init(Cartridge *cart) {
    UxROMData *data = calloc(1, sizeof(UxROMData));
    cart->mapper_data = data;
    cart->vtable = &uxrom_interface;
    uxrom_reset(cart);
}