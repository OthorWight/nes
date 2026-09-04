#include "mappers.h"
#include "cartridge.h"
#include "nes_system.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t  chr_banks[8];
    uint8_t  nt_banks[4];
    uint8_t  prg_banks[3];
    uint16_t irq_counter;
    bool     irq_enabled;
} Namco163Data;

static void m019_reset(Cartridge *c) {
    Namco163Data *d = (Namco163Data*)c->mapper_data;
    memset(d, 0, sizeof(Namco163Data));

    d->nt_banks[0] = 0xE0;
    d->nt_banks[1] = 0xE1;
    d->nt_banks[2] = 0xE0;
    d->nt_banks[3] = 0xE1;
    d->prg_banks[0] = 0;
    d->prg_banks[1] = 1;
    d->prg_banks[2] = 2;

    c->nes->lines.irq_line = false;
}

static void m019_destroy(Cartridge *c) {
    free(c->mapper_data);
    c->mapper_data = NULL;
}

static void m019_clock_m2(Cartridge *c) {
    Namco163Data *d = (Namco163Data*)c->mapper_data;

    if (!d->irq_enabled) return;

    if (d->irq_counter < 0x7FFF) {
        d->irq_counter++;
        if (d->irq_counter == 0x7FFF) {
            c->nes->lines.irq_line = true;
        }
    }
}

static uint8_t m019_cpu_read(Cartridge *c, uint16_t addr, bool *handled) {
    Namco163Data *d = (Namco163Data*)c->mapper_data;

    if (addr >= 0x4800 && addr <= 0x4FFF) {
        *handled = true;
        return (uint8_t)(d->irq_counter & 0xFF);
    }

    if (addr >= 0x5000 && addr <= 0x57FF) {
        *handled = true;
        return (uint8_t)((d->irq_counter >> 8) & 0x7F);
    }

    if (addr >= 0x6000 && addr <= 0x7FFF) {
        *handled = true;
        return (c->prg_ram && c->prg_ram_size > 0) ? c->prg_ram[addr - 0x6000] : 0;
    }

    if (addr >= 0x8000) {
        *handled = true;
        uint32_t total_8k = c->prg_rom_size / 8192;
        if (total_8k == 0) return 0;

        uint32_t bank = 0;
        if (addr <= 0x9FFF) {
            bank = d->prg_banks[0];
        } else if (addr <= 0xBFFF) {
            bank = d->prg_banks[1];
        } else if (addr <= 0xDFFF) {
            bank = d->prg_banks[2];
        } else {
            bank = total_8k - 1;
        }

        uint32_t offset = (bank % total_8k) * 8192 + (addr & 0x1FFF);
        return c->prg_rom[offset % c->prg_rom_size];
    }

    return 0;
}

static void m019_cpu_write(Cartridge *c, uint16_t addr, uint8_t val) {
    Namco163Data *d = (Namco163Data*)c->mapper_data;

    if (addr >= 0x5000 && addr <= 0x57FF) {
        d->irq_counter = (d->irq_counter & 0x7F00) | val;
        c->nes->lines.irq_line = false;
        return;
    }

    if (addr >= 0x5800 && addr <= 0x5FFF) {
        d->irq_counter = (d->irq_counter & 0x00FF) | ((uint16_t)(val & 0x7F) << 8);
        d->irq_enabled = (val & 0x80) != 0;
        c->nes->lines.irq_line = false;
        return;
    }

    if (addr >= 0x6000 && addr <= 0x7FFF) {
        if (c->prg_ram && c->prg_ram_size > 0) {
            c->prg_ram[addr - 0x6000] = val;
        }
        return;
    }

    if (addr >= 0x8000 && addr <= 0xBFFF) {
        uint8_t slot = (addr - 0x8000) / 0x0800;
        d->chr_banks[slot] = val;
        return;
    }

    if (addr >= 0xC000 && addr <= 0xDFFF) {
        uint8_t slot = (addr - 0xC000) / 0x0800;
        d->nt_banks[slot] = val;
        return;
    }

    if (addr >= 0xE000 && addr <= 0xE7FF) {
        d->prg_banks[0] = val & 0x3F;
        return;
    }

    if (addr >= 0xE800 && addr <= 0xEFFF) {
        d->prg_banks[1] = val & 0x3F;
        return;
    }

    if (addr >= 0xF000 && addr <= 0xF7FF) {
        d->prg_banks[2] = val & 0x3F;
        return;
    }
}

static uint8_t m019_ppu_read(Cartridge *c, uint16_t addr, bool *handled) {
    Namco163Data *d = (Namco163Data*)c->mapper_data;
    if (addr >= 0x2000 || c->chr_rom_size == 0) return 0;

    uint8_t slot = (addr / 1024) & 0x07;
    uint8_t bank = d->chr_banks[slot];

    // Bank values >= $E0 map pattern tables to internal CIRAM
    if (bank >= 0xE0) {
        *handled = false;
        return 0;
    }

    *handled = true;
    uint32_t total_1k = c->chr_rom_size / 1024;
    if (total_1k == 0) return 0;

    uint32_t offset = ((uint32_t)bank % total_1k) * 1024 + (addr & 0x03FF);
    return c->chr_rom[offset % c->chr_rom_size];
}

static void m019_ppu_write(Cartridge *c, uint16_t addr, uint8_t val) {
    Namco163Data *d = (Namco163Data*)c->mapper_data;
    if (addr >= 0x2000 || c->chr_rom_size == 0) return;

    uint8_t slot = (addr / 1024) & 0x07;
    uint8_t bank = d->chr_banks[slot];
    if (bank >= 0xE0) return;

    uint32_t total_1k = c->chr_rom_size / 1024;
    if (total_1k == 0) return;

    uint32_t offset = ((uint32_t)bank % total_1k) * 1024 + (addr & 0x03FF);
    c->chr_rom[offset % c->chr_rom_size] = val;
}

static uint16_t m019_remap_ciram_addr(Cartridge *c, uint16_t addr, bool *ciram_ce) {
    Namco163Data *d = (Namco163Data*)c->mapper_data;
    uint8_t slot = (addr >> 10) & 0x03;
    uint8_t bank = d->nt_banks[slot];
    uint16_t offset = addr & 0x03FF;

    if (bank >= 0xE0) {
        *ciram_ce = true;
        return ((bank & 0x01) ? 0x0400 : 0x0000) | offset;
    }

    *ciram_ce = false;
    if (c->chr_rom_size > 0) {
        uint32_t total_1k = c->chr_rom_size / 1024;
        if (total_1k > 0) {
            return c->chr_rom[((uint32_t)bank % total_1k) * 1024 + offset];
        }
    }

    return 0;
}

static const MapperInterface m019_interface = {
    .reset = m019_reset,
    .destroy = m019_destroy,
    .cpu_read = m019_cpu_read,
    .cpu_write = m019_cpu_write,
    .ppu_read = m019_ppu_read,
    .ppu_write = m019_ppu_write,
    .ppu_addr_change = NULL,
    .clock_m2 = m019_clock_m2,
    .remap_ciram_addr = m019_remap_ciram_addr
};

void mapper_019_init(Cartridge *cart) {
    Namco163Data *data = calloc(1, sizeof(Namco163Data));
    cart->mapper_data = data;
    cart->vtable = &m019_interface;
    m019_reset(cart);
}