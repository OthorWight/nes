#include "mappers.h"
#include "cartridge.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t bank_select;
    uint8_t regs[8];
} DxROMData;

static void m206_reset(Cartridge *c) {
    DxROMData *d = (DxROMData*)c->mapper_data;
    memset(d, 0, sizeof(DxROMData));
}

static void m206_destroy(Cartridge *c) {
    free(c->mapper_data);
    c->mapper_data = NULL;
}

static uint8_t m206_cpu_read(Cartridge *c, uint16_t addr, bool *handled) {
    DxROMData *d = (DxROMData*)c->mapper_data;

    if (addr >= 0x8000) {
        *handled = true;
        uint32_t total_8k = c->prg_rom_size / 8192;
        if (total_8k == 0) return 0;

        uint32_t bank = 0;
        if (addr <= 0x9FFF) {
            bank = d->regs[6] & 0x3F;
        } else if (addr <= 0xBFFF) {
            bank = d->regs[7] & 0x3F;
        } else if (addr <= 0xDFFF) {
            bank = (total_8k >= 2) ? total_8k - 2 : 0;
        } else {
            bank = total_8k - 1;
        }

        uint32_t offset = (bank % total_8k) * 8192 + (addr & 0x1FFF);
        return c->prg_rom[offset % c->prg_rom_size];
    }

    return 0;
}

static void m206_cpu_write(Cartridge *c, uint16_t addr, uint8_t val) {
    DxROMData *d = (DxROMData*)c->mapper_data;

    if (addr >= 0x8000) {
        if ((addr & 1) == 0) {
            d->bank_select = val & 0x07;
        } else {
            d->regs[d->bank_select & 0x07] = val;
        }
    }
}

static uint8_t m206_ppu_read(Cartridge *c, uint16_t addr, bool *handled) {
    DxROMData *d = (DxROMData*)c->mapper_data;
    if (addr >= 0x2000 || c->chr_rom_size == 0) return 0;

    *handled = true;
    uint32_t total_1k = c->chr_rom_size / 1024;
    if (total_1k == 0) return 0;

    uint32_t bank = 0;
    if (addr < 0x0800) {
        bank = (d->regs[0] & 0x3E) | ((addr >> 10) & 1);
    } else if (addr < 0x1000) {
        bank = (d->regs[1] & 0x3E) | ((addr >> 10) & 1);
    } else if (addr < 0x1400) {
        bank = d->regs[2] & 0x3F;
    } else if (addr < 0x1800) {
        bank = d->regs[3] & 0x3F;
    } else if (addr < 0x1C00) {
        bank = d->regs[4] & 0x3F;
    } else {
        bank = d->regs[5] & 0x3F;
    }

    uint32_t offset = (bank % total_1k) * 1024 + (addr & 0x03FF);
    return c->chr_rom[offset % c->chr_rom_size];
}

static void m206_ppu_write(Cartridge *c, uint16_t addr, uint8_t val) {
    DxROMData *d = (DxROMData*)c->mapper_data;
    if (addr >= 0x2000 || c->chr_rom_size == 0) return;

    uint32_t total_1k = c->chr_rom_size / 1024;
    if (total_1k == 0) return;

    uint32_t bank = 0;
    if (addr < 0x0800) {
        bank = (d->regs[0] & 0x3E) | ((addr >> 10) & 1);
    } else if (addr < 0x1000) {
        bank = (d->regs[1] & 0x3E) | ((addr >> 10) & 1);
    } else if (addr < 0x1400) {
        bank = d->regs[2] & 0x3F;
    } else if (addr < 0x1800) {
        bank = d->regs[3] & 0x3F;
    } else if (addr < 0x1C00) {
        bank = d->regs[4] & 0x3F;
    } else {
        bank = d->regs[5] & 0x3F;
    }

    uint32_t offset = (bank % total_1k) * 1024 + (addr & 0x03FF);
    c->chr_rom[offset % c->chr_rom_size] = val;
}

static uint16_t m206_remap_ciram_addr(Cartridge *c, uint16_t addr, bool *ciram_ce) {
    *ciram_ce = true;
    return cartridge_default_remap_ciram(c->mirroring, addr);
}

static const MapperInterface m206_interface = {
    .reset = m206_reset,
    .destroy = m206_destroy,
    .cpu_read = m206_cpu_read,
    .cpu_write = m206_cpu_write,
    .ppu_read = m206_ppu_read,
    .ppu_write = m206_ppu_write,
    .ppu_addr_change = NULL,
    .clock_m2 = NULL,
    .remap_ciram_addr = m206_remap_ciram_addr
};

void mapper_206_init(Cartridge *cart) {
    DxROMData *data = calloc(1, sizeof(DxROMData));
    cart->mapper_data = data;
    cart->vtable = &m206_interface;
    m206_reset(cart);
}