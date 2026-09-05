#include "mappers.h"

// MMC3B-style banking, RAM protection and filtered A12 IRQs shared by
// mapper 4 and TxSROM (118). Only nametable wiring and CHR A17 differ.
#include "cartridge.h"
#include "state_io.h"
#include "nes_system.h"
#include <stdlib.h>
#include <string.h>

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
    memset(d->bank_regs, 0, sizeof(d->bank_regs));
    d->mirroring = c->info.mirroring == MIRROR_HORIZONTAL ? 1 : 0;
    c->mirroring = c->info.mirroring;
    d->prg_ram_protect = 0x80;
    d->irq_latch = 0;
    d->irq_counter = 0;
    d->irq_enabled = false;
    d->irq_reload = false;
    d->last_a12 = false;
    d->a12_low_count = 0;
    c->nes->lines.irq_line = false;
    cpu_set_irq_line(&c->nes->cpu, 0, false);
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
        if (d->a12_low_count < 8) d->a12_low_count++;
    } else {
        if (!d->last_a12 && current_a12) {
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
            return cartridge_ram_read(c, addr - 0x6000);
        }
        return cartridge_open_bus(c);
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
            cartridge_ram_write(c, addr - 0x6000, val);
        }
        return;
    }

    if (addr < 0x8000) return;

    switch (addr & 0xE001) {
        case 0x8000: d->bank_select = val; break;
        case 0x8001:
            d->bank_regs[d->bank_select & 0x07] =
                (d->bank_select & 7) >= 6 ? (val & 0x3F) : val;
            break;
        case 0xA000:
            if (c->mapper_id == 4) d->mirroring = val & 1;
            break; // TxSROM gets CIRAM A10 from CHR bank registers.
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

static uint32_t mmc3_get_chr_bank(MMC3Data *d, uint16_t sub) {
    bool chr_mode = (d->bank_select & 0x80) != 0;
    if (!chr_mode) {
        if (sub < 0x0800) return (d->bank_regs[0] & 0xFE) | ((sub >> 10) & 1);
        if (sub < 0x1000) return (d->bank_regs[1] & 0xFE) | ((sub >> 10) & 1);
        if (sub < 0x1400) return d->bank_regs[2];
        if (sub < 0x1800) return d->bank_regs[3];
        if (sub < 0x1C00) return d->bank_regs[4];
        return d->bank_regs[5];
    } else {
        if (sub < 0x0400) return d->bank_regs[2];
        if (sub < 0x0800) return d->bank_regs[3];
        if (sub < 0x0C00) return d->bank_regs[4];
        if (sub < 0x1000) return d->bank_regs[5];
        if (sub < 0x1800) return (d->bank_regs[0] & 0xFE) | (((sub - 0x1000) >> 10) & 1);
        return (d->bank_regs[1] & 0xFE) | (((sub - 0x1800) >> 10) & 1);
    }
}

static uint8_t mmc3_ppu_read(Cartridge *c, uint16_t addr, bool *handled) {
    MMC3Data *d = (MMC3Data*)c->mapper_data;
    if (addr >= 0x2000 || c->chr_rom_size == 0) return 0;

    *handled = true;
    uint32_t total_1k = c->chr_rom_size / 1024;
    if (total_1k == 0) return 0;

    uint32_t bank = mmc3_get_chr_bank(d, addr);
    if (c->mapper_id == 118) bank &= 0x7F;
    return c->chr_rom[(bank % total_1k) * 1024 + (addr & 0x03FF)];
}

static void mmc3_ppu_write(Cartridge *c, uint16_t addr, uint8_t val) {
    MMC3Data *d = (MMC3Data*)c->mapper_data;
    if (addr >= 0x2000 || c->chr_rom_size == 0) return;

    uint32_t total_1k = c->chr_rom_size / 1024;
    if (total_1k == 0) return;

    uint32_t bank = mmc3_get_chr_bank(d, addr);
    if (c->mapper_id == 118) bank &= 0x7F;
    cartridge_chr_write(c, (bank % total_1k) * 1024 + (addr & 0x03FF), val);
}

static uint16_t mmc3_remap_ciram(Cartridge *c, uint16_t addr, bool *ciram_ce) {
    MMC3Data *d = (MMC3Data*)c->mapper_data;
    *ciram_ce = true;

    if (c->mapper_id == 4) {
        MirroringMode mode = c->info.mirroring == MIRROR_FOUR_SCREEN ? MIRROR_FOUR_SCREEN :
            ((d->mirroring & 1) ? MIRROR_HORIZONTAL : MIRROR_VERTICAL);
        return cartridge_default_remap_ciram(mode, addr);
    }

    uint16_t sub = addr & 0x0FFF;
    uint32_t bank = mmc3_get_chr_bank(d, sub);

    uint8_t ciram_a10 = (bank >> 7) & 1;
    return (uint16_t)((ciram_a10 ? 0x0400 : 0x0000) | (addr & 0x03FF));
}

static void mmc3_state(Cartridge *c, StateIO *io) {
    MMC3Data *d = (MMC3Data *)c->mapper_data;
    d->bank_select = state_u8(io, d->bank_select);
    state_bytes(io, d->bank_regs, sizeof(d->bank_regs));
    d->mirroring = state_u8(io, d->mirroring);
    d->prg_ram_protect = state_u8(io, d->prg_ram_protect);
    d->irq_latch = state_u8(io, d->irq_latch);
    d->irq_counter = state_u8(io, d->irq_counter);
    d->irq_enabled = state_bool(io, d->irq_enabled);
    d->irq_reload = state_bool(io, d->irq_reload);
    d->last_a12 = state_bool(io, d->last_a12);
    d->a12_low_count = state_i32(io, d->a12_low_count);
    if (d->a12_low_count < 0 || d->a12_low_count > 8) io->ok = false;
}

static const MapperInterface mmc3_interface = {
    .state = mmc3_state,
    .state_size = sizeof(MMC3Data),
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

void mapper_mmc3_init(Cartridge *cart) {
    MMC3Data *data = calloc(1, sizeof(MMC3Data));
    if (!data) return;
    cart->mapper_data = data;
    cart->vtable = &mmc3_interface;
    mmc3_reset(cart);
}