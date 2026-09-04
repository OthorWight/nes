#include "mappers.h"
#include "cartridge.h"
#include "nes_system.h"
#include <stdlib.h>
#include <string.h>

#define MMC5_EXRAM_SIZE 1024

typedef struct {
    uint8_t  prg_mode;
    uint8_t  chr_mode;
    uint8_t  ram_protect[2];
    uint8_t  exram_mode;
    uint8_t  nametable_ctrl;
    uint8_t  fill_tile;
    uint8_t  fill_attr;

    uint8_t  prg_regs[5];
    uint16_t chr_regs_a[8];
    uint16_t chr_regs_b[4];
    uint8_t  chr_high;

    uint8_t  mult_a;
    uint8_t  mult_b;

    uint8_t  irq_target;
    bool     irq_enabled;
    bool     irq_pending;
    bool     in_frame;
    int      scanline;

    uint8_t  exram[MMC5_EXRAM_SIZE];
    uint8_t  exram_latch;

} MMC5Data;

static inline bool mmc5_ram_write_enabled(MMC5Data *d) {
    return (d->ram_protect[0] == 0x02 && d->ram_protect[1] == 0x01);
}

static void mmc5_reset(Cartridge *c) {
    MMC5Data *d = (MMC5Data*)c->mapper_data;
    memset(d, 0, sizeof(MMC5Data));

    d->prg_mode = 3;
    d->ram_protect[0] = 0x02;
    d->ram_protect[1] = 0x01;

    d->prg_regs[0] = 0x00;
    d->prg_regs[1] = 0xFF;
    d->prg_regs[2] = 0xFF;
    d->prg_regs[3] = 0xFF;
    d->prg_regs[4] = 0xFF;

    c->nes->lines.irq_line = false;
}

static void mmc5_destroy(Cartridge *c) {
    free(c->mapper_data);
    c->mapper_data = NULL;
}

static void mmc5_ppu_dot(Cartridge *c, uint16_t addr) {
    (void)addr;
    MMC5Data *d = (MMC5Data*)c->mapper_data;
    int cycle = c->nes->ppu.cycle;
    int scanline = c->nes->ppu.scanline;
    bool rendering = (c->nes->ppu.ppu_mask & 0x18) != 0;

    if (scanline == 261 && cycle == 1) {
        d->in_frame = false;
        d->scanline = 0;
        d->irq_pending = false;
    }

    if (rendering && (scanline <= 239 || scanline == 261)) {
        if (cycle == 0) {
            d->in_frame = true;
        }
        if (cycle == 256) {
            if (scanline == 261) {
                d->scanline = 0;
            } else {
                d->scanline++;
            }
            if (d->in_frame && d->scanline == d->irq_target && d->irq_target != 0) {
                d->irq_pending = true;
            }
        }
    } else if (scanline == 240 && cycle == 0) {
        d->in_frame = false;
    }

    if (d->irq_pending && d->irq_enabled) {
        c->nes->lines.irq_line = true;
        cpu_set_irq_line(&c->nes->cpu, 0, true);
    } else {
        c->nes->lines.irq_line = false;
        cpu_set_irq_line(&c->nes->cpu, 0, false);
    }
}

static uint8_t mmc5_cpu_read(Cartridge *c, uint16_t addr, bool *handled) {
    MMC5Data *d = (MMC5Data*)c->mapper_data;

    if (addr >= 0x5000 && addr <= 0x5206) {
        *handled = true;
        if (addr == 0x5204) {
            uint8_t status = 0;
            if (d->irq_pending) status |= 0x80;
            if (d->in_frame)    status |= 0x40;
            
            d->irq_pending = false;
            c->nes->lines.irq_line = false;
            cpu_set_irq_line(&c->nes->cpu, 0, false);
            return status;
        }
        if (addr == 0x5205) {
            return (uint8_t)((d->mult_a * d->mult_b) & 0xFF);
        }
        if (addr == 0x5206) {
            return (uint8_t)(((d->mult_a * d->mult_b) >> 8) & 0xFF);
        }
        return 0;
    }

    if (addr >= 0x5C00 && addr <= 0x5FFF) {
        *handled = true;
        return d->exram[addr - 0x5C00];
    }

    if (addr >= 0x6000 && addr <= 0x7FFF) {
        *handled = true;
        if (c->prg_ram && c->prg_ram_size > 0) {
            uint32_t bank_8k = d->prg_regs[0] & 0x07;
            uint32_t offset = (bank_8k * 8192) + (addr - 0x6000);
            return c->prg_ram[offset % c->prg_ram_size];
        }
        return 0;
    }

    if (addr >= 0x8000) {
        *handled = true;
        uint32_t total_8k_ram = c->prg_ram_size / 8192;

        switch (d->prg_mode) {
            case 0: {
                uint8_t reg = d->prg_regs[4];
                uint32_t bank_8k = reg & 0x7C;
                if (!(reg & 0x80) && total_8k_ram > 0) {
                    return c->prg_ram[(bank_8k * 8192 + (addr - 0x8000)) % c->prg_ram_size];
                }
                return c->prg_rom[(bank_8k * 8192 + (addr - 0x8000)) % c->prg_rom_size];
            }
            case 1: {
                if (addr < 0xC000) {
                    uint8_t reg = d->prg_regs[2];
                    uint32_t bank_8k = reg & 0x7E;
                    if (!(reg & 0x80) && total_8k_ram > 0) {
                        return c->prg_ram[(bank_8k * 8192 + (addr - 0x8000)) % c->prg_ram_size];
                    }
                    return c->prg_rom[(bank_8k * 8192 + (addr - 0x8000)) % c->prg_rom_size];
                } else {
                    uint8_t reg = d->prg_regs[4];
                    uint32_t bank_8k = reg & 0x7E;
                    return c->prg_rom[(bank_8k * 8192 + (addr - 0xC000)) % c->prg_rom_size];
                }
            }
            case 2: {
                if (addr < 0xC000) {
                    uint8_t reg = d->prg_regs[2];
                    uint32_t bank_8k = reg & 0x7E;
                    if (!(reg & 0x80) && total_8k_ram > 0) {
                        return c->prg_ram[(bank_8k * 8192 + (addr - 0x8000)) % c->prg_ram_size];
                    }
                    return c->prg_rom[(bank_8k * 8192 + (addr - 0x8000)) % c->prg_rom_size];
                } else if (addr < 0xE000) {
                    uint8_t reg = d->prg_regs[3];
                    uint32_t bank_8k = reg & 0x7F;
                    if (!(reg & 0x80) && total_8k_ram > 0) {
                        return c->prg_ram[(bank_8k * 8192 + (addr - 0xC000)) % c->prg_ram_size];
                    }
                    return c->prg_rom[(bank_8k * 8192 + (addr - 0xC000)) % c->prg_rom_size];
                } else {
                    uint8_t reg = d->prg_regs[4];
                    uint32_t bank_8k = reg & 0x7F;
                    return c->prg_rom[(bank_8k * 8192 + (addr - 0xE000)) % c->prg_rom_size];
                }
            }
            case 3:
            default: {
                int slot = (addr - 0x8000) / 8192;
                uint8_t reg = d->prg_regs[slot + 1];
                uint32_t bank_8k = reg & 0x7F;
                if (slot < 3 && !(reg & 0x80) && total_8k_ram > 0) {
                    return c->prg_ram[(bank_8k * 8192 + (addr & 0x1FFF)) % c->prg_ram_size];
                }
                return c->prg_rom[(bank_8k * 8192 + (addr & 0x1FFF)) % c->prg_rom_size];
            }
        }
    }

    return 0;
}

static void mmc5_cpu_write(Cartridge *c, uint16_t addr, uint8_t val) {
    MMC5Data *d = (MMC5Data*)c->mapper_data;

    if (addr >= 0x5100 && addr <= 0x5206) {
        switch (addr) {
            case 0x5100: d->prg_mode = val & 0x03; break;
            case 0x5101: d->chr_mode = val & 0x03; break;
            case 0x5102: d->ram_protect[0] = val & 0x03; break;
            case 0x5103: d->ram_protect[1] = val & 0x03; break;
            case 0x5104: d->exram_mode = val & 0x03; break;
            case 0x5105: d->nametable_ctrl = val; break;
            case 0x5106: d->fill_tile = val; break;
            case 0x5107: d->fill_attr = (val & 0x03) | ((val & 0x03) << 2) | ((val & 0x03) << 4) | ((val & 0x03) << 6); break;

            case 0x5113: d->prg_regs[0] = val; break;
            case 0x5114: d->prg_regs[1] = val; break;
            case 0x5115: d->prg_regs[2] = val; break;
            case 0x5116: d->prg_regs[3] = val; break;
            case 0x5117: d->prg_regs[4] = val | 0x80; break;

            case 0x5120: case 0x5121: case 0x5122: case 0x5123:
            case 0x5124: case 0x5125: case 0x5126: case 0x5127:
                d->chr_regs_a[addr - 0x5120] = (uint16_t)val | ((uint16_t)(d->chr_high & 0x03) << 8);
                break;

            case 0x5128: case 0x5129: case 0x512A: case 0x512B:
                d->chr_regs_b[addr - 0x5128] = (uint16_t)val | ((uint16_t)(d->chr_high & 0x03) << 8);
                break;

            case 0x5130: d->chr_high = val & 0x03; break;

            case 0x5203: d->irq_target = val; break;
            case 0x5204:
                d->irq_enabled = (val & 0x80) != 0;
                if (!d->irq_enabled) {
                    c->nes->lines.irq_line = false;
                    cpu_set_irq_line(&c->nes->cpu, 0, false);
                } else if (d->irq_pending) {
                    c->nes->lines.irq_line = true;
                    cpu_set_irq_line(&c->nes->cpu, 0, true);
                }
                break;
            case 0x5205: d->mult_a = val; break;
            case 0x5206: d->mult_b = val; break;
        }
        return;
    }

    if (addr >= 0x5C00 && addr <= 0x5FFF) {
        d->exram[addr - 0x5C00] = val;
        return;
    }

    if (addr >= 0x6000 && addr <= 0x7FFF) {
        if (c->prg_ram && mmc5_ram_write_enabled(d) && c->prg_ram_size > 0) {
            uint32_t bank_8k = d->prg_regs[0] & 0x07;
            uint32_t offset = (bank_8k * 8192) + (addr - 0x6000);
            c->prg_ram[offset % c->prg_ram_size] = val;
        }
        return;
    }

    if (addr >= 0x8000 && addr <= 0xDFFF && mmc5_ram_write_enabled(d)) {
        int slot = (addr - 0x8000) / 8192;
        if (slot < 3 && !(d->prg_regs[slot + 1] & 0x80) && c->prg_ram_size > 0) {
            uint32_t bank_8k = d->prg_regs[slot + 1] & 0x07;
            uint32_t offset = (bank_8k * 8192) + (addr & 0x1FFF);
            c->prg_ram[offset % c->prg_ram_size] = val;
        }
    }
}

static uint8_t mmc5_ppu_read(Cartridge *c, uint16_t addr, bool *handled) {
    MMC5Data *d = (MMC5Data*)c->mapper_data;
    if (addr >= 0x2000 || c->chr_rom_size == 0) return 0;

    *handled = true;

    bool is_8x16 = (c->nes->ppu.ppu_ctrl & 0x20) != 0;
    bool is_sprite_fetch = (c->nes->ppu.cycle >= 257 && c->nes->ppu.cycle <= 320);

    if (d->exram_mode == 1 && !is_sprite_fetch) {
        uint32_t bank_4k = ((d->chr_high & 0x03) << 6) | (d->exram_latch & 0x3F);
        uint32_t total_4k_banks = c->chr_rom_size / 4096;
        if (total_4k_banks > 0) bank_4k %= total_4k_banks;
        
        uint32_t offset = (bank_4k * 4096) + (addr & 0x0FFF);
        return c->chr_rom[offset % c->chr_rom_size];
    }

    bool use_set_a = true;
    if (is_8x16) {
        use_set_a = is_sprite_fetch;
    }

    uint32_t bank = 0;
    uint32_t base_size = 1024;

    if (use_set_a) {
        switch (d->chr_mode) {
            case 0: bank = d->chr_regs_a[7]; base_size = 8192; break;
            case 1: bank = d->chr_regs_a[(addr / 4096) * 4 + 3]; base_size = 4096; break;
            case 2: bank = d->chr_regs_a[(addr / 2048) * 2 + 1]; base_size = 2048; break;
            case 3: default: bank = d->chr_regs_a[(addr / 1024) & 7]; base_size = 1024; break;
        }
    } else {
        switch (d->chr_mode) {
            case 0: bank = d->chr_regs_b[3]; base_size = 8192; break;
            case 1: bank = d->chr_regs_b[3]; base_size = 4096; break;
            case 2: bank = d->chr_regs_b[((addr / 2048) & 1) * 2 + 1]; base_size = 2048; break;
            case 3: default: bank = d->chr_regs_b[(addr / 1024) & 3]; base_size = 1024; break;
        }
    }

    uint32_t bank_1k = bank;
    if (base_size == 2048) bank_1k &= ~1;
    else if (base_size == 4096) bank_1k &= ~3;
    else if (base_size == 8192) bank_1k &= ~7;

    uint32_t total_1k_banks = c->chr_rom_size / 1024;
    if (total_1k_banks > 0) {
        bank_1k %= total_1k_banks;
    }

    uint32_t offset = (bank_1k * 1024) + (addr & (base_size - 1));
    return c->chr_rom[offset % c->chr_rom_size];
}

static void mmc5_ppu_write(Cartridge *c, uint16_t addr, uint8_t val) {
    if (addr < 0x2000 && c->chr_rom_size > 0) {
        c->chr_rom[addr % c->chr_rom_size] = val;
    }
}

static uint16_t mmc5_remap_ciram_addr(Cartridge *c, uint16_t addr, bool *ciram_ce) {
    MMC5Data *d = (MMC5Data*)c->mapper_data;
    uint16_t nt_offset = addr & 0x03FF;
    uint8_t select = (addr >> 10) & 0x03;
    uint8_t mode = (d->nametable_ctrl >> (select * 2)) & 0x03;

    bool is_attribute = (nt_offset >= 0x03C0);

    if (d->exram_mode == 1) {
        if (is_attribute) {
            *ciram_ce = false;
            uint8_t pal = (d->exram_latch >> 6) & 0x03;
            return (uint16_t)((pal << 6) | (pal << 4) | (pal << 2) | pal);
        } else {
            d->exram_latch = d->exram[nt_offset];
        }
    }

    switch (mode) {
        case 0:
            *ciram_ce = true;
            return nt_offset;
        case 1:
            *ciram_ce = true;
            return 0x0400 | nt_offset;
        case 2:
            *ciram_ce = false;
            return d->exram[nt_offset];
        case 3:
        default:
            *ciram_ce = false;
            return is_attribute ? d->fill_attr : d->fill_tile;
    }
}

static const MapperInterface mmc5_interface = {
    .reset = mmc5_reset,
    .destroy = mmc5_destroy,
    .cpu_read = mmc5_cpu_read,
    .cpu_write = mmc5_cpu_write,
    .ppu_read = mmc5_ppu_read,
    .ppu_write = mmc5_ppu_write,
    .ppu_addr_change = NULL,
    .ppu_dot = mmc5_ppu_dot,
    .clock_m2 = NULL,
    .remap_ciram_addr = mmc5_remap_ciram_addr
};

void mapper_005_init(Cartridge *cart) {
    MMC5Data *data = calloc(1, sizeof(MMC5Data));
    cart->mapper_data = data;
    cart->vtable = &mmc5_interface;
    mmc5_reset(cart);
}