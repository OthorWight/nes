#include "mappers.h"
#include "cartridge.h"
#include <stdlib.h>

typedef struct {
    uint8_t chr_bank;
} CNROMData;

static void cnrom_reset(Cartridge *c) {
    CNROMData *d = (CNROMData*)c->mapper_data;
    d->chr_bank = 0;
}

static void cnrom_destroy(Cartridge *c) {
    free(c->mapper_data);
    c->mapper_data = NULL;
}

static uint8_t cnrom_cpu_read(Cartridge *c, uint16_t addr, bool *handled) {
    if (addr >= 0x6000 && addr <= 0x7FFF) {
        *handled = true;
        return (c->prg_ram && c->prg_ram_size > 0) ? c->prg_ram[addr - 0x6000] : 0;
    }

    if (addr >= 0x8000) {
        *handled = true;
        uint16_t offset = (uint16_t)(addr - 0x8000);
        if (c->prg_rom_size == 16384) {
            offset &= 0x3FFF;
        }
        return c->prg_rom[offset % c->prg_rom_size];
    }

    return 0;
}

static void cnrom_cpu_write(Cartridge *c, uint16_t addr, uint8_t val) {
    CNROMData *d = (CNROMData*)c->mapper_data;

    if (addr >= 0x6000 && addr <= 0x7FFF) {
        if (c->prg_ram && c->prg_ram_size > 0) {
            c->prg_ram[addr - 0x6000] = val;
        }
        return;
    }

    if (addr >= 0x8000) {
        d->chr_bank = val & 0x03;
    }
}

static uint8_t cnrom_ppu_read(Cartridge *c, uint16_t addr, bool *handled) {
    CNROMData *d = (CNROMData*)c->mapper_data;

    if (addr < 0x2000 && c->chr_rom_size > 0) {
        *handled = true;
        uint32_t offset = ((uint32_t)d->chr_bank * 8192) + (addr & 0x1FFF);
        return c->chr_rom[offset % c->chr_rom_size];
    }

    return 0;
}

static void cnrom_ppu_write(Cartridge *c, uint16_t addr, uint8_t val) {
    CNROMData *d = (CNROMData*)c->mapper_data;

    if (addr < 0x2000 && c->chr_rom_size > 0) {
        uint32_t offset = ((uint32_t)d->chr_bank * 8192) + (addr & 0x1FFF);
        c->chr_rom[offset % c->chr_rom_size] = val;
    }
}

static uint16_t cnrom_remap_ciram_addr(Cartridge *c, uint16_t addr, bool *ciram_ce) {
    *ciram_ce = true;
    return cartridge_default_remap_ciram(c->mirroring, addr);
}

static const MapperInterface cnrom_interface = {
    .reset = cnrom_reset,
    .destroy = cnrom_destroy,
    .cpu_read = cnrom_cpu_read,
    .cpu_write = cnrom_cpu_write,
    .ppu_read = cnrom_ppu_read,
    .ppu_write = cnrom_ppu_write,
    .ppu_addr_change = NULL,
    .clock_m2 = NULL,
    .remap_ciram_addr = cnrom_remap_ciram_addr
};

void mapper_003_init(Cartridge *cart) {
    CNROMData *data = calloc(1, sizeof(CNROMData));
    cart->mapper_data = data;
    cart->vtable = &cnrom_interface;
    cnrom_reset(cart);
}