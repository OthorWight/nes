#include "mappers.h"
#include "cartridge.h"
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
} TxSROMData;

static void m118_reset(Cartridge *c) {
    TxSROMData *d = (TxSROMData*)c->mapper_data;
    d->bank_select = 0;
    memset(d->bank_regs, 0, sizeof(d->bank_regs));
    d->mirroring = 0;
    d->prg_ram_protect = 0x80;
    d->irq_latch = 0;
    d->irq_counter = 0;
    d->irq_enabled = false;
    d->irq_reload = false;
    d->last_a12 = false;
    d->a12_low_count = 0;
}

static void m118_destroy(Cartridge *c) {
    free(c->mapper_data);
    c->mapper_data = NULL;
}

static void m118_clock_scanline(Cartridge *c) {
    TxSROMData *d = (TxSROMData*)c->mapper_data;

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

static void m118_ppu_dot(Cartridge *c, uint16_t addr) {
    TxSROMData *d = (TxSROMData*)c->mapper_data;
    bool current_a12 = (addr & 0x1000) != 0;

    if (!current_a12) {
        d->a12_low_count++;
    } else {
        if (!d->last_a12 && current_a12) {
            if (d->a12_low_count >= 8) {
                m118_clock_scanline(c);
            }
        }
        d->a12_low_count = 0;
    }
    d->last_a12 = current_a12;
}

static uint8_t m118_cpu_read(Cartridge *c, uint16_t addr, bool *handled) {
    TxSROMData *d = (TxSROMData*)c->mapper_data;

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

static void m118_cpu_write(Cartridge *c, uint16_t addr, uint8_t val) {
    TxSROMData *d = (TxSROMData*)c->mapper_data;

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
        case 0xA000: /* TxSROM ignores standard $A000 mirroring */ break;
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

static uint32_t m118_get_chr_bank(TxSROMData *d, uint16_t sub) {
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

static uint8_t m118_ppu_read(Cartridge *c, uint16_t addr, bool *handled) {
    TxSROMData *d = (TxSROMData*)c->mapper_data;
    if (addr >= 0x2000 || c->chr_rom_size == 0) return 0;

    *handled = true;
    uint32_t total_1k = c->chr_rom_size / 1024;
    if (total_1k == 0) return 0;

    uint32_t bank = m118_get_chr_bank(d, addr);
    return c->chr_rom[(bank % total_1k) * 1024 + (addr & 0x03FF)];
}

static void m118_ppu_write(Cartridge *c, uint16_t addr, uint8_t val) {
    TxSROMData *d = (TxSROMData*)c->mapper_data;
    if (addr >= 0x2000 || c->chr_rom_size == 0) return;

    uint32_t total_1k = c->chr_rom_size / 1024;
    if (total_1k == 0) return;

    uint32_t bank = m118_get_chr_bank(d, addr);
    c->chr_rom[(bank % total_1k) * 1024 + (addr & 0x03FF)] = val;
}

static uint16_t m118_remap_ciram(Cartridge *c, uint16_t addr, bool *ciram_ce) {
    TxSROMData *d = (TxSROMData*)c->mapper_data;
    *ciram_ce = true;

    uint16_t sub = addr & 0x0FFF;
    uint32_t bank = m118_get_chr_bank(d, sub);

    uint8_t ciram_a10 = (bank >> 7) & 1;
    return (uint16_t)((ciram_a10 ? 0x0400 : 0x0000) | (addr & 0x03FF));
}

static const MapperInterface m118_interface = {
    .reset = m118_reset,
    .destroy = m118_destroy,
    .cpu_read = m118_cpu_read,
    .cpu_write = m118_cpu_write,
    .ppu_read = m118_ppu_read,
    .ppu_write = m118_ppu_write,
    .ppu_addr_change = NULL,
    .ppu_dot = m118_ppu_dot,
    .clock_m2 = NULL,
    .remap_ciram_addr = m118_remap_ciram
};

void mapper_118_init(Cartridge *cart) {
    TxSROMData *data = calloc(1, sizeof(TxSROMData));
    cart->mapper_data = data;
    cart->vtable = &m118_interface;
    m118_reset(cart);
}