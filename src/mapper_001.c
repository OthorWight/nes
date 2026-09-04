#include "mappers.h"
#include "cartridge.h"
#include "nes_system.h"
#include <stdlib.h>

typedef struct {
    uint8_t  shift_reg;
    uint8_t  write_count;
    uint8_t  control;
    uint8_t  chr_bank_0;
    uint8_t  chr_bank_1;
    uint8_t  prg_bank;
    uint64_t last_write_cycle;
} MMC1Data;

static void mmc1_reset(Cartridge *c) {
    MMC1Data *d = (MMC1Data*)c->mapper_data;
    d->shift_reg = 0;
    d->write_count = 0;
    d->control = 0x0C;
    d->chr_bank_0 = 0;
    d->chr_bank_1 = 0;
    d->prg_bank = 0;
    d->last_write_cycle = 0;
    c->mirroring = MIRROR_HORIZONTAL;
}

static void mmc1_destroy(Cartridge *c) {
    free(c->mapper_data);
    c->mapper_data = NULL;
}

static uint8_t mmc1_cpu_read(Cartridge *c, uint16_t addr, bool *handled) {
    MMC1Data *d = (MMC1Data*)c->mapper_data;

    if (addr >= 0x6000 && addr <= 0x7FFF) {
        *handled = true;
        return (c->prg_ram && c->prg_ram_size > 0) ? c->prg_ram[addr - 0x6000] : 0;
    }

    if (addr >= 0x8000) {
        *handled = true;
        uint32_t total_16k = c->prg_rom_size / 16384;
        if (total_16k == 0) return 0;

        uint8_t prg_mode = (d->control >> 2) & 0x03;
        uint32_t bank = 0;

        if (prg_mode == 0 || prg_mode == 1) {
            bank = (d->prg_bank & 0x0E) & ~1;
            if (addr >= 0xC000) bank |= 1;
        } else if (prg_mode == 2) {
            bank = (addr < 0xC000) ? 0 : (d->prg_bank & 0x0F);
        } else {
            bank = (addr < 0xC000) ? (d->prg_bank & 0x0F) : (total_16k - 1);
        }

        uint32_t offset = (bank % total_16k) * 16384 + (addr & 0x3FFF);
        return c->prg_rom[offset % c->prg_rom_size];
    }

    return 0;
}

static void mmc1_cpu_write(Cartridge *c, uint16_t addr, uint8_t val) {
    MMC1Data *d = (MMC1Data*)c->mapper_data;

    if (addr >= 0x6000 && addr <= 0x7FFF) {
        if (c->prg_ram && c->prg_ram_size > 0) {
            c->prg_ram[addr - 0x6000] = val;
        }
        return;
    }

    if (addr < 0x8000) return;

    // Ignore consecutive-cycle writes
    uint64_t cur_cycle = c->nes->cpu.cycle_count;
    if (cur_cycle - d->last_write_cycle < 2) {
        return;
    }
    d->last_write_cycle = cur_cycle;

    if (val & 0x80) {
        d->shift_reg = 0;
        d->write_count = 0;
        d->control |= 0x0C;
        return;
    }

    d->shift_reg |= ((val & 0x01) << d->write_count);
    d->write_count++;

    if (d->write_count == 5) {
        uint8_t data = d->shift_reg & 0x1F;
        uint8_t reg = (addr >> 13) & 0x03;

        switch (reg) {
            case 0:
                d->control = data;
                break;
            case 1:
                d->chr_bank_0 = data;
                break;
            case 2:
                d->chr_bank_1 = data;
                break;
            case 3:
                d->prg_bank = data;
                break;
        }

        d->shift_reg = 0;
        d->write_count = 0;
    }
}

static uint8_t mmc1_ppu_read(Cartridge *c, uint16_t addr, bool *handled) {
    MMC1Data *d = (MMC1Data*)c->mapper_data;
    if (addr >= 0x2000 || c->chr_rom_size == 0) return 0;

    *handled = true;
    uint32_t total_4k = c->chr_rom_size / 4096;
    if (total_4k == 0) return 0;

    uint32_t bank = 0;
    if ((d->control & 0x10) == 0) {
        bank = (d->chr_bank_0 & 0x1E) | ((addr >> 12) & 1);
    } else {
        bank = (addr < 0x1000) ? d->chr_bank_0 : d->chr_bank_1;
    }

    uint32_t offset = (bank % total_4k) * 4096 + (addr & 0x0FFF);
    return c->chr_rom[offset % c->chr_rom_size];
}

static void mmc1_ppu_write(Cartridge *c, uint16_t addr, uint8_t val) {
    MMC1Data *d = (MMC1Data*)c->mapper_data;
    if (addr >= 0x2000 || c->chr_rom_size == 0) return;

    uint32_t total_4k = c->chr_rom_size / 4096;
    if (total_4k == 0) return;

    uint32_t bank = 0;
    if ((d->control & 0x10) == 0) {
        bank = (d->chr_bank_0 & 0x1E) | ((addr >> 12) & 1);
    } else {
        bank = (addr < 0x1000) ? d->chr_bank_0 : d->chr_bank_1;
    }

    uint32_t offset = (bank % total_4k) * 4096 + (addr & 0x0FFF);
    c->chr_rom[offset % c->chr_rom_size] = val;
}

static uint16_t mmc1_remap_ciram_addr(Cartridge *c, uint16_t addr, bool *ciram_ce) {
    MMC1Data *d = (MMC1Data*)c->mapper_data;
    *ciram_ce = true;

    MirroringMode mode;
    switch (d->control & 0x03) {
        case 0: mode = MIRROR_ONE_SCREEN_LOW; break;
        case 1: mode = MIRROR_ONE_SCREEN_HIGH; break;
        case 2: mode = MIRROR_VERTICAL; break;
        case 3:
        default: mode = MIRROR_HORIZONTAL; break;
    }

    return cartridge_default_remap_ciram(mode, addr);
}

static const MapperInterface mmc1_interface = {
    .reset = mmc1_reset,
    .destroy = mmc1_destroy,
    .cpu_read = mmc1_cpu_read,
    .cpu_write = mmc1_cpu_write,
    .ppu_read = mmc1_ppu_read,
    .ppu_write = mmc1_ppu_write,
    .ppu_addr_change = NULL,
    .clock_m2 = NULL,
    .remap_ciram_addr = mmc1_remap_ciram_addr
};

void mapper_001_init(Cartridge *cart) {
    MMC1Data *data = calloc(1, sizeof(MMC1Data));
    cart->mapper_data = data;
    cart->vtable = &mmc1_interface;
    mmc1_reset(cart);
}