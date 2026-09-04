#include "mappers.h"
#include "cartridge.h"
#include <stdlib.h>

typedef struct {
    uint8_t prg_bank;
    uint8_t chr_bank_0;
    uint8_t chr_bank_1;
} BNROMNINAData;

static void m034_reset(Cartridge *c) {
    BNROMNINAData *d = (BNROMNINAData*)c->mapper_data;
    d->prg_bank = 0;
    d->chr_bank_0 = 0;
    d->chr_bank_1 = 1;
}

static void m034_destroy(Cartridge *c) {
    free(c->mapper_data);
    c->mapper_data = NULL;
}

static uint8_t m034_cpu_read(Cartridge *c, uint16_t addr, bool *handled) {
    BNROMNINAData *d = (BNROMNINAData*)c->mapper_data;

    if (addr >= 0x6000 && addr <= 0x7FFC) {
        *handled = true;
        return (c->prg_ram && c->prg_ram_size > 0) ? c->prg_ram[addr - 0x6000] : 0;
    }

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

static void m034_cpu_write(Cartridge *c, uint16_t addr, uint8_t val) {
    BNROMNINAData *d = (BNROMNINAData*)c->mapper_data;

    if (addr >= 0x6000 && addr <= 0x7FFC) {
        if (c->prg_ram && c->prg_ram_size > 0) {
            c->prg_ram[addr - 0x6000] = val;
        }
        return;
    }

    if (addr == 0x7FFD) {
        d->prg_bank = val & 0x03;
    } else if (addr == 0x7FFE) {
        d->chr_bank_0 = val & 0x0F;
    } else if (addr == 0x7FFF) {
        d->chr_bank_1 = val & 0x0F;
    } else if (addr >= 0x8000) {
        d->prg_bank = val & 0x03;
    }
}

static uint8_t m034_ppu_read(Cartridge *c, uint16_t addr, bool *handled) {
    BNROMNINAData *d = (BNROMNINAData*)c->mapper_data;

    if (addr < 0x2000) {
        *handled = true;
        if (c->chr_rom_size == 0) return 0;

        uint32_t total_4k = c->chr_rom_size / 4096;
        if (total_4k == 0) return 0;

        uint32_t bank = (addr < 0x1000) ? d->chr_bank_0 : d->chr_bank_1;
        uint32_t offset = (bank % total_4k) * 4096 + (addr & 0x0FFF);
        return c->chr_rom[offset % c->chr_rom_size];
    }

    return 0;
}

static void m034_ppu_write(Cartridge *c, uint16_t addr, uint8_t val) {
    BNROMNINAData *d = (BNROMNINAData*)c->mapper_data;

    if (addr < 0x2000 && c->chr_rom_size > 0) {
        uint32_t total_4k = c->chr_rom_size / 4096;
        if (total_4k == 0) return;

        uint32_t bank = (addr < 0x1000) ? d->chr_bank_0 : d->chr_bank_1;
        uint32_t offset = (bank % total_4k) * 4096 + (addr & 0x0FFF);
        c->chr_rom[offset % c->chr_rom_size] = val;
    }
}

static uint16_t m034_remap_ciram_addr(Cartridge *c, uint16_t addr, bool *ciram_ce) {
    *ciram_ce = true;
    return cartridge_default_remap_ciram(c->mirroring, addr);
}

static const MapperInterface m034_interface = {
    .reset = m034_reset,
    .destroy = m034_destroy,
    .cpu_read = m034_cpu_read,
    .cpu_write = m034_cpu_write,
    .ppu_read = m034_ppu_read,
    .ppu_write = m034_ppu_write,
    .ppu_addr_change = NULL,
    .clock_m2 = NULL,
    .remap_ciram_addr = m034_remap_ciram_addr
};

void mapper_034_init(Cartridge *cart) {
    BNROMNINAData *data = calloc(1, sizeof(BNROMNINAData));
    cart->mapper_data = data;
    cart->vtable = &m034_interface;
    m034_reset(cart);
}