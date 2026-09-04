#include "mappers.h"
#include "cartridge.h"
#include "nes_system.h"
#include <stdlib.h>

typedef struct {
    uint8_t  bank_select;
    uint8_t  bank_regs[8];
    uint8_t  mirroring;
    uint8_t  prg_ram_protect;
    uint8_t  irq_latch;
    uint8_t  irq_counter;
    bool     irq_enabled;
    bool     irq_reload;
    bool     last_a12;
    int      a12_low_count;
} MMC3Data;

static void mmc3_reset(Cartridge *c) {
    MMC3Data *d = (MMC3Data*)c->mapper_data;
    d->bank_select = 0;
    for (int i = 0; i < 8; i++) {
        d->bank_regs[i] = 0;
    }
    d->mirroring = 0;
    d->prg_ram_protect = 0x80;
    d->irq_latch = 0;
    d->irq_counter = 0;
    d->irq_enabled = false;
    d->irq_reload = false;
    d->last_a12 = false;
    d->a12_low_count = 0;
}

static void mmc3_destroy(Cartridge *c) {
    free(c->mapper_data);
    c->mapper_data = NULL;
}

static void mmc3_clock_scanline(Cartridge *c) {
    MMC3Data *d = (MMC3Data*)c->mapper_data;

    if (d->irq_counter == 0 || d->irq_reload) {
        d->irq_counter = d->irq_latch;
        d->irq_reload = false;
    } else {
        d->irq_counter--;
    }

    if (d->irq_counter == 0 && d->irq_enabled) {
        c->nes->lines.irq_line = true;
        cpu_set_irq_line(&c->nes->cpu, 0, true);
    }
}

static void mmc3_ppu_dot(Cartridge *c, uint16_t addr) {
    MMC3Data *d = (MMC3Data*)c->mapper_data;
    bool current_a12 = (addr & 0x1000) != 0;

    if (!current_a12) {
        d->a12_low_count++;
    } else {
        if (!d->last_a12 && current_a12) {
            // RC filter threshold: A12 must stay low for at least 8 PPU dots
            if (d->a12_low_count >= 8) {
                mmc3_clock_scanline(c);
            }
        }
        d->a12_low_count = 0;
    }
    d->last_a12 = current_a12;
}

static uint8_t mmc3_cpu_read(Cartridge *c, uint16_t addr, bool *handled) {
    MMC3Data *d = (MMC3Data*)c->mapper_data;

    if (addr >= 0x6000 && addr <= 0x7FFF) {
        *handled = true;
        if ((d->prg_ram_protect & 0x80) && c->prg_ram && c->prg_ram_size > 0) {
            return c->prg_ram[addr - 0x6000];
        }
        return 0;
    }

    if (addr >= 0x8000) {
        *handled = true;
        uint32_t total_8k = c->prg_rom_size / 8192;
        if (total_8k == 0) return 0;

        uint32_t bank = 0;
        bool prg_mode = (d->bank_select & 0x40) != 0;

        if (addr < 0xA000) {
            bank = prg_mode ? (total_8k - 2) : d->bank_regs[6];
        } else if (addr < 0xC000) {
            bank = d->bank_regs[7];
        } else if (addr < 0xE000) {
            bank = prg_mode ? d->bank_regs[6] : (total_8k - 2);
        } else {
            bank = total_8k - 1;
        }
        return c->prg_rom[(bank % total_8k) * 8192 + (addr & 0x1FFF)];
    }

    return 0;
}

static void mmc3_cpu_write(Cartridge *c, uint16_t addr, uint8_t val) {
    MMC3Data *d = (MMC3Data*)c->mapper_data;

    if (addr >= 0x6000 && addr <= 0x7FFF) {
        if ((d->prg_ram_protect & 0xC0) == 0x80 && c->prg_ram && c->prg_ram_size > 0) {
            c->prg_ram[addr - 0x6000] = val;
        }
        return;
    }

    if (addr < 0x8000) return;

    switch (addr & 0xE001) {
        case 0x8000: d->bank_select = val; break;
        case 0x8001: d->bank_regs[d->bank_select & 0x07] = val; break;
        case 0xA000: d->mirroring = val & 0x01; break;
        case 0xA001: d->prg_ram_protect = val; break;
        case 0xC000: d->irq_latch = val; break;
        case 0xC001: d->irq_reload = true; break;
        case 0xE000:
            d->irq_enabled = false;
            c->nes->lines.irq_line = false;
            cpu_set_irq_line(&c->nes->cpu, 0, false);
            break;
        case 0xE001: 
            d->irq_enabled = true; 
            break;
    }
}

static uint8_t mmc3_ppu_read(Cartridge *c, uint16_t addr, bool *handled) {
    MMC3Data *d = (MMC3Data*)c->mapper_data;
    if (addr >= 0x2000) return 0;

    *handled = true;
    if (c->chr_rom_size == 0) return 0;

    uint32_t total_1k = c->chr_rom_size / 1024;
    if (total_1k == 0) return 0;

    uint32_t bank = 0;
    bool chr_mode = (d->bank_select & 0x80) != 0;

    if (!chr_mode) {
        if (addr < 0x0800) {
            bank = (d->bank_regs[0] & 0xFE) | ((addr >> 10) & 1);
        } else if (addr < 0x1000) {
            bank = (d->bank_regs[1] & 0xFE) | ((addr >> 10) & 1);
        } else if (addr < 0x1400) {
            bank = d->bank_regs[2];
        } else if (addr < 0x1800) {
            bank = d->bank_regs[3];
        } else if (addr < 0x1C00) {
            bank = d->bank_regs[4];
        } else {
            bank = d->bank_regs[5];
        }
    } else {
        if (addr < 0x0400) {
            bank = d->bank_regs[2];
        } else if (addr < 0x0800) {
            bank = d->bank_regs[3];
        } else if (addr < 0x0C00) {
            bank = d->bank_regs[4];
        } else if (addr < 0x1000) {
            bank = d->bank_regs[5];
        } else if (addr < 0x1800) {
            bank = (d->bank_regs[0] & 0xFE) | (((addr - 0x1000) >> 10) & 1);
        } else {
            bank = (d->bank_regs[1] & 0xFE) | (((addr - 0x1800) >> 10) & 1);
        }
    }

    uint32_t offset = (bank % total_1k) * 1024 + (addr & 0x03FF);
    return c->chr_rom[offset % c->chr_rom_size];
}

static void mmc3_ppu_write(Cartridge *c, uint16_t addr, uint8_t val) {
    MMC3Data *d = (MMC3Data*)c->mapper_data;
    if (addr >= 0x2000 || c->chr_rom_size == 0) return;

    uint32_t total_1k = c->chr_rom_size / 1024;
    if (total_1k == 0) return;

    uint32_t bank = 0;
    bool chr_mode = (d->bank_select & 0x80) != 0;

    if (!chr_mode) {
        if (addr < 0x0800) {
            bank = (d->bank_regs[0] & 0xFE) | ((addr >> 10) & 1);
        } else if (addr < 0x1000) {
            bank = (d->bank_regs[1] & 0xFE) | ((addr >> 10) & 1);
        } else if (addr < 0x1400) {
            bank = d->bank_regs[2];
        } else if (addr < 0x1800) {
            bank = d->bank_regs[3];
        } else if (addr < 0x1C00) {
            bank = d->bank_regs[4];
        } else {
            bank = d->bank_regs[5];
        }
    } else {
        if (addr < 0x0400) {
            bank = d->bank_regs[2];
        } else if (addr < 0x0800) {
            bank = d->bank_regs[3];
        } else if (addr < 0x0C00) {
            bank = d->bank_regs[4];
        } else if (addr < 0x1000) {
            bank = d->bank_regs[5];
        } else if (addr < 0x1800) {
            bank = (d->bank_regs[0] & 0xFE) | (((addr - 0x1000) >> 10) & 1);
        } else {
            bank = (d->bank_regs[1] & 0xFE) | (((addr - 0x1800) >> 10) & 1);
        }
    }

    uint32_t offset = (bank % total_1k) * 1024 + (addr & 0x03FF);
    c->chr_rom[offset % c->chr_rom_size] = val;
}

static uint16_t mmc3_remap_ciram(Cartridge *c, uint16_t addr, bool *ciram_ce) {
    MMC3Data *d = (MMC3Data*)c->mapper_data;
    *ciram_ce = true;
    
    if (c->mirroring == MIRROR_FOUR_SCREEN) {
        return cartridge_default_remap_ciram(MIRROR_FOUR_SCREEN, addr);
    }
    
    MirroringMode mode = (d->mirroring & 1) ? MIRROR_HORIZONTAL : MIRROR_VERTICAL;
    return cartridge_default_remap_ciram(mode, addr);
}

static const MapperInterface mmc3_interface = {
    .reset = mmc3_reset,
    .destroy = mmc3_destroy,
    .cpu_read = mmc3_cpu_read,
    .cpu_write = mmc3_cpu_write,
    .ppu_read = mmc3_ppu_read,
    .ppu_write = mmc3_ppu_write,
    .ppu_addr_change = NULL,
    .ppu_dot = mmc3_ppu_dot,
    .clock_m2 = NULL,
    .remap_ciram_addr = mmc3_remap_ciram
};

void mapper_004_init(Cartridge *cart) {
    MMC3Data *data = calloc(1, sizeof(MMC3Data));
    cart->mapper_data = data;
    cart->vtable = &mmc3_interface;
    mmc3_reset(cart);
}