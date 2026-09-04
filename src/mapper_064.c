#include "mappers.h"
#include "cartridge.h"
#include "nes_system.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t bank_select;
    uint8_t regs[16];
    bool    chr_1k_mode;  // K bit (D5 of $8000)
    bool    prg_mode;     // P bit (D6 of $8000)
    bool    chr_mode;     // C bit (D7 of $8000)

    // IRQ system
    uint8_t irq_latch;
    uint8_t irq_counter;
    bool    irq_enabled;
    bool    irq_reload;
    uint8_t irq_mode;     // 0 = Scanline (A12), 1 = Cycle (M2)
    uint8_t cycle_prescaler;

    // A12 edge filter for scanline mode
    bool    last_a12;
    int     a12_low_count;
} M064Data;

static void m064_clock_irq(Cartridge *c) {
    M064Data *d = (M064Data*)c->mapper_data;

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

static void m064_reset(Cartridge *c) {
    M064Data *d = (M064Data*)c->mapper_data;
    memset(d, 0, sizeof(M064Data));

    c->mirroring = MIRROR_VERTICAL;
    c->nes->lines.irq_line = false;
}

static void m064_destroy(Cartridge *c) {
    free(c->mapper_data);
    c->mapper_data = NULL;
}

static void m064_clock_m2(Cartridge *c) {
    M064Data *d = (M064Data*)c->mapper_data;

    if (d->irq_mode == 1) {
        d->cycle_prescaler++;
        if (d->cycle_prescaler >= 4) {
            d->cycle_prescaler = 0;
            m064_clock_irq(c);
        }
    }
}

static void m064_ppu_dot(Cartridge *c, uint16_t addr) {
    M064Data *d = (M064Data*)c->mapper_data;
    bool current_a12 = (addr & 0x1000) != 0;

    if (!current_a12) {
        d->a12_low_count++;
    } else {
        if (!d->last_a12 && current_a12) {
            if (d->a12_low_count >= 8) {
                if (d->irq_mode == 0) {
                    m064_clock_irq(c);
                }
            }
        }
        d->a12_low_count = 0;
    }
    d->last_a12 = current_a12;
}

static uint8_t m064_cpu_read(Cartridge *c, uint16_t addr, bool *handled) {
    M064Data *d = (M064Data*)c->mapper_data;

    if (addr >= 0x6000 && addr <= 0x7FFF) {
        *handled = true;
        return (c->prg_ram && c->prg_ram_size > 0) ? c->prg_ram[addr - 0x6000] : 0;
    }

    if (addr >= 0x8000) {
        *handled = true;
        uint32_t total_8k = c->prg_rom_size / 8192;
        if (total_8k == 0) return 0;

        uint32_t bank = 0;
        if (addr < 0xA000) {
            bank = d->prg_mode ? d->regs[0x0F] : d->regs[6];
        } else if (addr < 0xC000) {
            bank = d->prg_mode ? d->regs[6] : d->regs[7];
        } else if (addr < 0xE000) {
            bank = d->prg_mode ? d->regs[7] : d->regs[0x0F];
        } else {
            bank = total_8k - 1;
        }

        return c->prg_rom[(bank % total_8k) * 8192 + (addr & 0x1FFF)];
    }

    return 0;
}

static void m064_cpu_write(Cartridge *c, uint16_t addr, uint8_t val) {
    M064Data *d = (M064Data*)c->mapper_data;

    if (addr >= 0x6000 && addr <= 0x7FFF) {
        if (c->prg_ram && c->prg_ram_size > 0) {
            c->prg_ram[addr - 0x6000] = val;
        }
        return;
    }

    if (addr < 0x8000) return;

    switch (addr & 0xE001) {
        case 0x8000:
            d->bank_select = val & 0x0F;
            d->chr_1k_mode = (val & 0x20) != 0;
            d->prg_mode    = (val & 0x40) != 0;
            d->chr_mode    = (val & 0x80) != 0;
            break;

        case 0x8001:
            d->regs[d->bank_select] = val;
            break;

        case 0xA000:
            if (c->mirroring != MIRROR_FOUR_SCREEN) {
                c->mirroring = (val & 0x01) ? MIRROR_HORIZONTAL : MIRROR_VERTICAL;
            }
            break;

        case 0xA001:
            break;

        case 0xC000:
            d->irq_latch = val;
            break;

        case 0xC001:
            d->irq_mode = val & 0x01;
            d->irq_reload = true;
            d->cycle_prescaler = 0;
            break;

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

static uint32_t m064_get_chr_bank(M064Data *d, uint8_t slot) {
    if (!d->chr_mode) {
        if (slot == 0) return d->chr_1k_mode ? d->regs[0] : (d->regs[0] & 0xFE);
        if (slot == 1) return d->chr_1k_mode ? d->regs[8] : (d->regs[0] | 0x01);
        if (slot == 2) return d->chr_1k_mode ? d->regs[1] : (d->regs[1] & 0xFE);
        if (slot == 3) return d->chr_1k_mode ? d->regs[9] : (d->regs[1] | 0x01);
        if (slot == 4) return d->regs[2];
        if (slot == 5) return d->regs[3];
        if (slot == 6) return d->regs[4];
        return d->regs[5];
    } else {
        if (slot == 0) return d->regs[2];
        if (slot == 1) return d->regs[3];
        if (slot == 2) return d->regs[4];
        if (slot == 3) return d->regs[5];
        if (slot == 4) return d->chr_1k_mode ? d->regs[0] : (d->regs[0] & 0xFE);
        if (slot == 5) return d->chr_1k_mode ? d->regs[8] : (d->regs[0] | 0x01);
        if (slot == 6) return d->chr_1k_mode ? d->regs[1] : (d->regs[1] & 0xFE);
        return d->chr_1k_mode ? d->regs[9] : (d->regs[1] | 0x01);
    }
}

static uint8_t m064_ppu_read(Cartridge *c, uint16_t addr, bool *handled) {
    M064Data *d = (M064Data*)c->mapper_data;
    if (addr >= 0x2000 || c->chr_rom_size == 0) return 0;

    *handled = true;
    uint32_t total_1k = c->chr_rom_size / 1024;
    if (total_1k == 0) return 0;

    uint8_t slot = (addr / 1024) & 0x07;
    uint32_t bank = m064_get_chr_bank(d, slot);

    return c->chr_rom[(bank % total_1k) * 1024 + (addr & 0x03FF)];
}

static void m064_ppu_write(Cartridge *c, uint16_t addr, uint8_t val) {
    M064Data *d = (M064Data*)c->mapper_data;
    if (addr >= 0x2000 || c->chr_rom_size == 0) return;

    uint32_t total_1k = c->chr_rom_size / 1024;
    if (total_1k == 0) return;

    uint8_t slot = (addr / 1024) & 0x07;
    uint32_t bank = m064_get_chr_bank(d, slot);

    c->chr_rom[(bank % total_1k) * 1024 + (addr & 0x03FF)] = val;
}

static uint16_t m064_remap_ciram_addr(Cartridge *c, uint16_t addr, bool *ciram_ce) {
    *ciram_ce = true;
    return cartridge_default_remap_ciram(c->mirroring, addr);
}

static const MapperInterface m064_interface = {
    .reset = m064_reset,
    .destroy = m064_destroy,
    .cpu_read = m064_cpu_read,
    .cpu_write = m064_cpu_write,
    .ppu_read = m064_ppu_read,
    .ppu_write = m064_ppu_write,
    .ppu_addr_change = NULL,
    .ppu_dot = m064_ppu_dot,
    .clock_m2 = m064_clock_m2,
    .remap_ciram_addr = m064_remap_ciram_addr
};

void mapper_064_init(Cartridge *cart) {
    M064Data *data = calloc(1, sizeof(M064Data));
    cart->mapper_data = data;
    cart->vtable = &m064_interface;
    m064_reset(cart);
}