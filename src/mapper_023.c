#include "mappers.h"
#include "cartridge.h"
#include "nes_system.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t prg_bank_0;
    uint8_t prg_bank_1;
    uint8_t prg_mode;

    uint8_t chr_low[8];
    uint8_t chr_high[8];

    uint8_t irq_latch;
    uint8_t irq_ctrl;
    uint8_t irq_counter;
    int16_t prescaler;
    bool    irq_pending;
} VRC24Data;

static void m023_reset(Cartridge *c) {
    VRC24Data *d = (VRC24Data*)c->mapper_data;
    memset(d, 0, sizeof(VRC24Data));

    d->prg_bank_0 = 0;
    d->prg_bank_1 = 1;
    d->prescaler = 341;
    c->mirroring = MIRROR_VERTICAL;
    c->nes->lines.irq_line = false;
}

static void m023_destroy(Cartridge *c) {
    free(c->mapper_data);
    c->mapper_data = NULL;
}

static void m023_clock_m2(Cartridge *c) {
    VRC24Data *d = (VRC24Data*)c->mapper_data;

    if (!(d->irq_ctrl & 0x02)) return;

    bool count_tick = false;
    if (d->irq_ctrl & 0x04) {
        // Cycle mode: counts every CPU M2 cycle
        count_tick = true;
    } else {
        // Scanline mode: prescaler divides by 341/3 (~113.66 cycles)
        d->prescaler -= 3;
        if (d->prescaler <= 0) {
            d->prescaler += 341;
            count_tick = true;
        }
    }

    if (count_tick) {
        if (d->irq_counter == 0xFF) {
            d->irq_counter = d->irq_latch;
            d->irq_pending = true;
            c->nes->lines.irq_line = true;
        } else {
            d->irq_counter++;
        }
    }
}

static uint8_t m023_cpu_read(Cartridge *c, uint16_t addr, bool *handled) {
    VRC24Data *d = (VRC24Data*)c->mapper_data;

    if (addr >= 0x6000 && addr <= 0x7FFF) {
        *handled = true;
        return (c->prg_ram && c->prg_ram_size > 0) ? c->prg_ram[addr - 0x6000] : 0;
    }

    if (addr >= 0x8000) {
        *handled = true;
        uint32_t total_8k = c->prg_rom_size / 8192;
        if (total_8k == 0) return 0;

        uint32_t bank = 0;
        bool mode = (d->prg_mode >> 1) & 0x01;

        if (addr <= 0x9FFF) {
            bank = mode ? (total_8k - 2) : d->prg_bank_0;
        } else if (addr <= 0xBFFF) {
            bank = d->prg_bank_1;
        } else if (addr <= 0xDFFF) {
            bank = mode ? d->prg_bank_0 : (total_8k - 2);
        } else {
            bank = total_8k - 1;
        }

        uint32_t offset = (bank % total_8k) * 8192 + (addr & 0x1FFF);
        return c->prg_rom[offset % c->prg_rom_size];
    }

    return 0;
}

static void m023_cpu_write(Cartridge *c, uint16_t addr, uint8_t val) {
    VRC24Data *d = (VRC24Data*)c->mapper_data;

    if (addr >= 0x6000 && addr <= 0x7FFF) {
        if (c->prg_ram && c->prg_ram_size > 0) {
            c->prg_ram[addr - 0x6000] = val;
        }
        return;
    }

    // Decode low address bits for VRC2/VRC4 pin configurations
    uint8_t reg_select = (addr & 0x03) | ((addr >> 2) & 0x03);

    if (addr >= 0x8000 && addr <= 0x8FFF) {
        d->prg_bank_0 = val & 0x1F;
    } else if (addr >= 0x9000 && addr <= 0x9FFF) {
        if (reg_select == 0) {
            switch (val & 0x03) {
                case 0: c->mirroring = MIRROR_VERTICAL; break;
                case 1: c->mirroring = MIRROR_HORIZONTAL; break;
                case 2: c->mirroring = MIRROR_ONE_SCREEN_LOW; break;
                case 3: c->mirroring = MIRROR_ONE_SCREEN_HIGH; break;
            }
        } else if (reg_select == 2) {
            d->prg_mode = val;
        }
    } else if (addr >= 0xA000 && addr <= 0xAFFF) {
        d->prg_bank_1 = val & 0x1F;
    } else if (addr >= 0xB000 && addr <= 0xEFFF) {
        uint8_t slot_base = ((addr - 0xB000) / 0x1000) * 2;
        if (reg_select == 0) {
            d->chr_low[slot_base] = val & 0x0F;
        } else if (reg_select == 1) {
            d->chr_high[slot_base] = val & 0x1F;
        } else if (reg_select == 2) {
            d->chr_low[slot_base + 1] = val & 0x0F;
        } else if (reg_select == 3) {
            d->chr_high[slot_base + 1] = val & 0x1F;
        }
    } else if (addr >= 0xF000) {
        if (reg_select == 0) {
            d->irq_latch = (d->irq_latch & 0xF0) | (val & 0x0F);
        } else if (reg_select == 1) {
            d->irq_latch = (d->irq_latch & 0x0F) | ((val & 0x0F) << 4);
        } else if (reg_select == 2) {
            d->irq_ctrl = val;
            if (val & 0x02) {
                d->irq_counter = d->irq_latch;
                d->prescaler = 341;
            }
            d->irq_pending = false;
            c->nes->lines.irq_line = false;
        } else if (reg_select == 3) {
            d->irq_pending = false;
            c->nes->lines.irq_line = false;
            if (d->irq_ctrl & 0x08) {
                d->irq_ctrl |= 0x02;
            } else {
                d->irq_ctrl &= ~0x02;
            }
        }
    }
}

static uint8_t m023_ppu_read(Cartridge *c, uint16_t addr, bool *handled) {
    VRC24Data *d = (VRC24Data*)c->mapper_data;
    if (addr >= 0x2000 || c->chr_rom_size == 0) return 0;

    *handled = true;
    uint32_t total_1k = c->chr_rom_size / 1024;
    if (total_1k == 0) return 0;

    uint8_t slot = addr / 1024;
    uint32_t bank = ((uint32_t)d->chr_high[slot] << 4) | (d->chr_low[slot] & 0x0F);
    uint32_t offset = (bank % total_1k) * 1024 + (addr & 0x03FF);

    return c->chr_rom[offset % c->chr_rom_size];
}

static void m023_ppu_write(Cartridge *c, uint16_t addr, uint8_t val) {
    VRC24Data *d = (VRC24Data*)c->mapper_data;
    if (addr >= 0x2000 || c->chr_rom_size == 0) return;

    uint32_t total_1k = c->chr_rom_size / 1024;
    if (total_1k == 0) return;

    uint8_t slot = addr / 1024;
    uint32_t bank = ((uint32_t)d->chr_high[slot] << 4) | (d->chr_low[slot] & 0x0F);
    uint32_t offset = (bank % total_1k) * 1024 + (addr & 0x03FF);

    c->chr_rom[offset % c->chr_rom_size] = val;
}

static uint16_t m023_remap_ciram_addr(Cartridge *c, uint16_t addr, bool *ciram_ce) {
    *ciram_ce = true;
    return cartridge_default_remap_ciram(c->mirroring, addr);
}

static const MapperInterface m023_interface = {
    .reset = m023_reset,
    .destroy = m023_destroy,
    .cpu_read = m023_cpu_read,
    .cpu_write = m023_cpu_write,
    .ppu_read = m023_ppu_read,
    .ppu_write = m023_ppu_write,
    .ppu_addr_change = NULL,
    .clock_m2 = m023_clock_m2,
    .remap_ciram_addr = m023_remap_ciram_addr
};

void mapper_023_init(Cartridge *cart) {
    VRC24Data *data = calloc(1, sizeof(VRC24Data));
    cart->mapper_data = data;
    cart->vtable = &m023_interface;
    m023_reset(cart);
}