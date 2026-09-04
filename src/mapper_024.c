#include "mappers.h"
#include "cartridge.h"
#include "nes_system.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t  prg_bank_16k;
    uint8_t  prg_bank_8k;
    uint8_t  chr_regs[8];
    uint8_t  chr_mode;
    bool     ram_enable;

    uint8_t  irq_latch;
    uint8_t  irq_ctrl;
    uint8_t  irq_counter;
    int16_t  prescaler;
    bool     irq_pending;

    bool     swap_a0_a1; // false for Mapper 24 (VRC6a), true for Mapper 26 (VRC6b)
} VRC6Data;

static void m024_reset(Cartridge *c) {
    VRC6Data *d = (VRC6Data*)c->mapper_data;
    d->prg_bank_16k = 0;
    d->prg_bank_8k = 0;
    memset(d->chr_regs, 0, sizeof(d->chr_regs));
    d->chr_mode = 0;
    d->ram_enable = true;

    d->irq_latch = 0;
    d->irq_ctrl = 0;
    d->irq_counter = 0;
    d->prescaler = 341;
    d->irq_pending = false;

    c->mirroring = MIRROR_VERTICAL;
    c->nes->lines.irq_line = false;
}

static void m024_destroy(Cartridge *c) {
    free(c->mapper_data);
    c->mapper_data = NULL;
}

static void m024_clock_m2(Cartridge *c) {
    VRC6Data *d = (VRC6Data*)c->mapper_data;

    if (!(d->irq_ctrl & 0x02)) return;

    bool count_tick = false;
    if (d->irq_ctrl & 0x04) {
        // Cycle mode: tick every CPU cycle
        count_tick = true;
    } else {
        // Scanline mode: prescaler divides by 341/3 (~113.66 CPU cycles)
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
            cpu_set_irq_line(&c->nes->cpu, 0, true);
        } else {
            d->irq_counter++;
        }
    }
}

static uint8_t m024_cpu_read(Cartridge *c, uint16_t addr, bool *handled) {
    VRC6Data *d = (VRC6Data*)c->mapper_data;

    if (addr >= 0x6000 && addr <= 0x7FFF) {
        *handled = true;
        if (c->prg_ram && c->prg_ram_size > 0) {
            return c->prg_ram[(addr - 0x6000) % c->prg_ram_size];
        }
        return 0;
    }

    if (addr >= 0x8000) {
        *handled = true;
        uint32_t total_16k = c->prg_rom_size / 16384;
        uint32_t total_8k = c->prg_rom_size / 8192;
        if (total_8k == 0) return 0;

        if (addr < 0xC000) {
            // $8000-$BFFF: 16KB switchable PRG ROM
            uint32_t bank = d->prg_bank_16k % total_16k;
            return c->prg_rom[bank * 16384 + (addr & 0x3FFF)];
        } else if (addr < 0xE000) {
            // $C000-$DFFF: 8KB switchable PRG ROM
            uint32_t bank = d->prg_bank_8k % total_8k;
            return c->prg_rom[bank * 8192 + (addr & 0x1FFF)];
        } else {
            // $E000-$FFFF: Fixed to last 8KB PRG ROM bank
            uint32_t bank = total_8k - 1;
            return c->prg_rom[bank * 8192 + (addr & 0x1FFF)];
        }
    }

    return 0;
}

static void m024_cpu_write(Cartridge *c, uint16_t addr, uint8_t val) {
    VRC6Data *d = (VRC6Data*)c->mapper_data;

    if (addr >= 0x6000 && addr <= 0x7FFF) {
        if (c->prg_ram && c->prg_ram_size > 0) {
            c->prg_ram[(addr - 0x6000) % c->prg_ram_size] = val;
        }
        return;
    }

    if (addr < 0x8000) return;

    // Decode low register bits: VRC6b (Mapper 26) swaps lines A0 and A1
    uint8_t sub = d->swap_a0_a1
        ? (uint8_t)(((addr & 0x01) << 1) | ((addr & 0x02) >> 1))
        : (uint8_t)(addr & 0x03);

    switch (addr & 0xF000) {
        case 0x8000:
            // 16KB PRG Select ($8000-$BFFF)
            d->prg_bank_16k = val & 0x1F;
            break;

        case 0xB000:
            if (sub == 3) {
                // Mirroring & Control
                switch ((val >> 2) & 0x03) {
                    case 0: c->mirroring = MIRROR_VERTICAL; break;
                    case 1: c->mirroring = MIRROR_HORIZONTAL; break;
                    case 2: c->mirroring = MIRROR_ONE_SCREEN_LOW; break;
                    case 3: c->mirroring = MIRROR_ONE_SCREEN_HIGH; break;
                }
                d->ram_enable = (val & 0x80) != 0; // Bit 7 is WRAM Enable
                d->chr_mode = val & 0x03;
            }
            break;

        case 0xC000:
            // 8KB PRG Select ($C000-$DFFF)
            d->prg_bank_8k = val & 0x1F;
            break;

        case 0xD000:
            // 1KB CHR Select 0..3
            d->chr_regs[sub] = val;
            break;

        case 0xE000:
            // 1KB CHR Select 4..7
            d->chr_regs[4 + sub] = val;
            break;

        case 0xF000:
            if (sub == 0) {
                d->irq_latch = val;
            } else if (sub == 1) {
                d->irq_ctrl = val;
                if (val & 0x02) {
                    d->irq_counter = d->irq_latch;
                    d->prescaler = 341;
                }
                d->irq_pending = false;
                c->nes->lines.irq_line = false;
                cpu_set_irq_line(&c->nes->cpu, 0, false);
            } else if (sub == 2) {
                d->irq_pending = false;
                c->nes->lines.irq_line = false;
                cpu_set_irq_line(&c->nes->cpu, 0, false);
                if (d->irq_ctrl & 0x08) {
                    d->irq_ctrl |= 0x02;
                } else {
                    d->irq_ctrl &= ~0x02;
                }
            }
            break;
    }
}

static uint8_t m024_ppu_read(Cartridge *c, uint16_t addr, bool *handled) {
    VRC6Data *d = (VRC6Data*)c->mapper_data;
    if (addr >= 0x2000 || c->chr_rom_size == 0) return 0;

    *handled = true;
    uint32_t total_1k = c->chr_rom_size / 1024;
    if (total_1k == 0) return 0;

    uint8_t slot = (addr / 1024) & 0x07;
    uint32_t bank = 0;

    switch (d->chr_mode & 0x03) {
        case 0:
            // 1KB banking
            bank = d->chr_regs[slot];
            break;
        case 1:
            // 2KB banking
            bank = (d->chr_regs[slot / 2] & ~1) | (slot & 1);
            break;
        case 2:
        case 3:
            // 1KB + 2KB banking
            if (slot < 4) {
                bank = d->chr_regs[slot];
            } else {
                uint8_t r_idx = 4 + ((slot - 4) / 2) * 2;
                bank = (d->chr_regs[r_idx] & ~1) | (slot & 1);
            }
            break;
    }

    uint32_t offset = (bank % total_1k) * 1024 + (addr & 0x03FF);
    return c->chr_rom[offset % c->chr_rom_size];
}

static void m024_ppu_write(Cartridge *c, uint16_t addr, uint8_t val) {
    VRC6Data *d = (VRC6Data*)c->mapper_data;
    if (addr >= 0x2000 || c->chr_rom_size == 0) return;

    uint32_t total_1k = c->chr_rom_size / 1024;
    if (total_1k == 0) return;

    uint8_t slot = (addr / 1024) & 0x07;
    uint32_t bank = 0;

    switch (d->chr_mode & 0x03) {
        case 0:
            bank = d->chr_regs[slot];
            break;
        case 1:
            bank = (d->chr_regs[slot / 2] & ~1) | (slot & 1);
            break;
        case 2:
        case 3:
            if (slot < 4) {
                bank = d->chr_regs[slot];
            } else {
                uint8_t r_idx = 4 + ((slot - 4) / 2) * 2;
                bank = (d->chr_regs[r_idx] & ~1) | (slot & 1);
            }
            break;
    }

    uint32_t offset = (bank % total_1k) * 1024 + (addr & 0x03FF);
    c->chr_rom[offset % c->chr_rom_size] = val;
}

static uint16_t m024_remap_ciram_addr(Cartridge *c, uint16_t addr, bool *ciram_ce) {
    *ciram_ce = true;
    return cartridge_default_remap_ciram(c->mirroring, addr);
}

static const MapperInterface vrc6_interface = {
    .reset = m024_reset,
    .destroy = m024_destroy,
    .cpu_read = m024_cpu_read,
    .cpu_write = m024_cpu_write,
    .ppu_read = m024_ppu_read,
    .ppu_write = m024_ppu_write,
    .ppu_addr_change = NULL,
    .ppu_dot = NULL,
    .clock_m2 = m024_clock_m2,
    .remap_ciram_addr = m024_remap_ciram_addr
};

void mapper_024_init(Cartridge *cart) {
    VRC6Data *data = calloc(1, sizeof(VRC6Data));
    data->swap_a0_a1 = false; // VRC6a
    cart->mapper_data = data;
    cart->vtable = &vrc6_interface;
    m024_reset(cart);
}

void mapper_026_init(Cartridge *cart) {
    VRC6Data *data = calloc(1, sizeof(VRC6Data));
    data->swap_a0_a1 = true;  // VRC6b
    cart->mapper_data = data;
    cart->vtable = &vrc6_interface;
    m024_reset(cart);
}