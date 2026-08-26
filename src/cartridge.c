#include "cartridge.h"
#include "cpu6502.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ==========================================
// POWER-OF-TWO PADDING & MIRRORING HELPER
// ==========================================

static void pad_and_mirror_rom(uint8_t **rom_data, uint32_t *rom_size) {
    uint32_t orig_size = *rom_size;
    if (orig_size == 0) return;
    uint32_t padded_size = 1;
    while (padded_size < orig_size) {
        padded_size <<= 1;
    }
    if (padded_size == orig_size) return;

    uint8_t *new_data = realloc(*rom_data, padded_size);
    if (!new_data) return;
    *rom_data = new_data;

    uint32_t current_size = orig_size;
    while (current_size < padded_size) {
        uint32_t remaining = padded_size - current_size;
        uint32_t chunk_size = 1;
        while (chunk_size * 2 <= remaining && chunk_size * 2 <= current_size) {
            chunk_size *= 2;
        }
        if (chunk_size > remaining) chunk_size = remaining;
        
        memcpy(new_data + current_size, new_data + current_size - chunk_size, chunk_size);
        current_size += chunk_size;
    }
    *rom_size = padded_size;
}

// ==========================================
// MAPPER 0: NROM
// ==========================================

static uint8_t nrom_read_prg(void *cart, uint16_t address) {
    Cartridge *c = (Cartridge*)cart;
    if (address >= 0x6000 && address <= 0x7FFF) {
        return c->prg_ram ? c->prg_ram[address - 0x6000] : 0;
    }
    uint16_t mapped_address = (uint16_t)(address - 0x8000);
    if (c->prg_rom_size == 16384) {
        mapped_address &= 0x3FFF;
    }
    return c->prg_rom[mapped_address % c->prg_rom_size];
}

static void nrom_write_prg(void *cart, uint16_t address, uint8_t data) {
    Cartridge *c = (Cartridge*)cart;
    if (address >= 0x6000 && address <= 0x7FFF && c->prg_ram) {
        c->prg_ram[address - 0x6000] = data;
    }
}

static uint8_t nrom_read_chr(void *cart, uint16_t address) {
    Cartridge *c = (Cartridge*)cart;
    if (c->chr_rom_size > 0) {
        return c->chr_rom[address & 0x1FFF];
    }
    return 0;
}

static void nrom_write_chr(void *cart, uint16_t address, uint8_t data) {
    Cartridge *c = (Cartridge*)cart;
    if (c->chr_rom_size > 0) {
        c->chr_rom[address & 0x1FFF] = data;
    }
}

// ==========================================
// MAPPER 1: MMC1
// ==========================================

static uint8_t mmc1_read_prg(void *cart, uint16_t address) {
    Cartridge *c = (Cartridge*)cart;
    if (address >= 0x6000 && address <= 0x7FFF) {
        return c->prg_ram ? c->prg_ram[address - 0x6000] : 0;
    }

    uint8_t control = c->mapper_state[2];
    uint8_t prg_bank = c->mapper_state[5] & 0x0F;
    uint8_t prg_mode = (control >> 2) & 0x03;
    uint32_t total_banks = c->prg_rom_size / 16384;
    if (total_banks == 0) return 0;

    uint32_t offset = 0;
    if (prg_mode == 0 || prg_mode == 1) {
        uint32_t bank = (prg_bank & 0xFE) % total_banks;
        offset = bank * 16384 + (address - 0x8000);
    } else if (prg_mode == 2) {
        if (address < 0xC000) {
            offset = address - 0x8000;
        } else {
            uint32_t bank = prg_bank % total_banks;
            offset = bank * 16384 + (address - 0xC000);
        }
    } else {
        if (address < 0xC000) {
            uint32_t bank = prg_bank % total_banks;
            offset = bank * 16384 + (address - 0x8000);
        } else {
            offset = (c->prg_rom_size - 16384) + (address - 0xC000);
        }
    }

    return c->prg_rom[offset % c->prg_rom_size];
}

static void mmc1_write_prg(void *cart, uint16_t address, uint8_t data) {
    Cartridge *c = (Cartridge*)cart;
    if (address >= 0x6000 && address <= 0x7FFF) {
        if (c->prg_ram) c->prg_ram[address - 0x6000] = data;
        return;
    }

    CPU6502 *cpu = (CPU6502*)c->cpu_context;
    if (cpu) {
        if (cpu->cycle_count - c->last_write_cycle < 2) {
            return;
        }
        c->last_write_cycle = cpu->cycle_count;
    }

    if (data & 0x80) {
        c->mapper_state[0] = 0;
        c->mapper_state[1] = 0;
        c->mapper_state[2] |= 0x0C;
        uint8_t control = c->mapper_state[2] & 0x03;
        if (control == 0) c->mirroring = MIRROR_ONE_SCREEN_LOW;
        else if (control == 1) c->mirroring = MIRROR_ONE_SCREEN_HIGH;
        else if (control == 2) c->mirroring = MIRROR_VERTICAL;
        else c->mirroring = MIRROR_HORIZONTAL;
    } else {
        uint8_t val = data & 0x01;
        c->mapper_state[0] |= (val << c->mapper_state[1]);
        c->mapper_state[1]++;
        if (c->mapper_state[1] == 5) {
            uint8_t reg_val = c->mapper_state[0] & 0x1F;
            uint16_t target_reg = (address >> 13) & 0x03;
            c->mapper_state[2 + target_reg] = reg_val;
            c->mapper_state[0] = 0;
            c->mapper_state[1] = 0;
            if (target_reg == 0) {
                uint8_t control = reg_val & 0x03;
                if (control == 0) c->mirroring = MIRROR_ONE_SCREEN_LOW;
                else if (control == 1) c->mirroring = MIRROR_ONE_SCREEN_HIGH;
                else if (control == 2) c->mirroring = MIRROR_VERTICAL;
                else c->mirroring = MIRROR_HORIZONTAL;
            }
        }
    }
}

static uint8_t mmc1_read_chr(void *cart, uint16_t address) {
    Cartridge *c = (Cartridge*)cart;
    if (c->chr_rom_size == 0) return 0;
    uint8_t control = c->mapper_state[2];
    uint8_t chr_mode = (control >> 4) & 0x01;
    uint8_t bank_0 = c->mapper_state[3];
    uint8_t bank_1 = c->mapper_state[4];

    uint32_t offset = 0;
    if (chr_mode == 0) {
        uint8_t bank = bank_0 & 0xFE;
        offset = (uint32_t)bank * 4096 + address;
    } else {
        if (address < 0x1000) {
            offset = (uint32_t)bank_0 * 4096 + address;
        } else {
            offset = (uint32_t)bank_1 * 4096 + (address - 0x1000);
        }
    }

    return c->chr_rom[offset % c->chr_rom_size];
}

static void mmc1_write_chr(void *cart, uint16_t address, uint8_t data) {
    Cartridge *c = (Cartridge*)cart;
    if (c->chr_rom_size == 0) return;
    uint8_t control = c->mapper_state[2];
    uint8_t chr_mode = (control >> 4) & 0x01;
    uint8_t bank_0 = c->mapper_state[3];
    uint8_t bank_1 = c->mapper_state[4];

    uint32_t offset = 0;
    if (chr_mode == 0) {
        uint8_t bank = bank_0 & 0xFE;
        offset = (uint32_t)bank * 4096 + address;
    } else {
        if (address < 0x1000) {
            offset = (uint32_t)bank_0 * 4096 + address;
        } else {
            offset = (uint32_t)bank_1 * 4096 + (address - 0x1000);
        }
    }

    c->chr_rom[offset % c->chr_rom_size] = data;
}

// ==========================================
// MAPPER 3: CNROM
// ==========================================

static uint8_t cnrom_read_prg(void *cart, uint16_t address) {
    return nrom_read_prg(cart, address);
}

static void cnrom_write_prg(void *cart, uint16_t address, uint8_t data) {
    Cartridge *c = (Cartridge*)cart;
    if (address >= 0x8000) {
        c->mapper_state[0] = data & 0x03;
    } else if (address >= 0x6000 && address <= 0x7FFF && c->prg_ram) {
        c->prg_ram[address - 0x6000] = data;
    }
}

static uint8_t cnrom_read_chr(void *cart, uint16_t address) {
    Cartridge *c = (Cartridge*)cart;
    if (c->chr_rom_size > 0) {
        uint32_t offset = ((uint32_t)c->mapper_state[0] * 8192) + (address & 0x1FFF);
        return c->chr_rom[offset % c->chr_rom_size];
    }
    return 0;
}

// ==========================================
// MAPPER 7: AXROM
// ==========================================

static uint8_t axrom_read_prg(void *cart, uint16_t address) {
    Cartridge *c = (Cartridge*)cart;
    uint32_t bank = c->mapper_state[0] & 0x07;
    uint32_t offset = (bank * 32768) + (address - 0x8000);
    return c->prg_rom[offset % c->prg_rom_size];
}

static void axrom_write_prg(void *cart, uint16_t address, uint8_t data) {
    Cartridge *c = (Cartridge*)cart;
    if (address >= 0x8000) {
        c->mapper_state[0] = data & 0x07;
        c->mirroring = (data & 0x10) ? MIRROR_ONE_SCREEN_HIGH : MIRROR_ONE_SCREEN_LOW;
    } else if (address >= 0x6000 && address <= 0x7FFF && c->prg_ram) {
        c->prg_ram[address - 0x6000] = data;
    }
}

// ==========================================
// MAPPER 2: UXROM
// ==========================================

static uint8_t uxrom_read_prg(void *cart, uint16_t address) {
    Cartridge *c = (Cartridge*)cart;
    if (address >= 0x6000 && address <= 0x7FFF) {
        return c->prg_ram ? c->prg_ram[address - 0x6000] : 0;
    }
    uint32_t last_bank_offset = c->prg_rom_size - 16384;
    if (address < 0xC000) {
        uint32_t bank = c->mapper_state[0];
        return c->prg_rom[(bank * 16384) + (address - 0x8000)];
    } else {
        return c->prg_rom[last_bank_offset + (address - 0xC000)];
    }
}

static void uxrom_write_prg(void *cart, uint16_t address, uint8_t data) {
    Cartridge *c = (Cartridge*)cart;
    if (address >= 0x6000 && address <= 0x7FFF) {
        if (c->prg_ram) c->prg_ram[address - 0x6000] = data;
        return;
    }
    uint32_t total_banks = c->prg_rom_size / 16384;
    if (total_banks > 0) {
        c->mapper_state[0] = data % total_banks;
    }
}

// ==========================================
// MAPPER 4: MMC3
// ==========================================

static uint8_t mmc3_read_prg(void *cart, uint16_t address) {
    Cartridge *c = (Cartridge*)cart;
    if (address >= 0x6000 && address <= 0x7FFF) {
        return c->prg_ram ? c->prg_ram[address - 0x6000] : 0;
    }

    uint32_t total_banks = c->prg_rom_size / 8192;
    if (total_banks == 0) return 0;

    uint8_t bank_select = c->mapper_state[8];
    uint8_t prg_mode = (bank_select >> 6) & 0x01;
    uint32_t bank = 0;

    if (address >= 0x8000 && address <= 0x9FFF) {
        bank = (prg_mode == 0) ? c->mapper_state[6] : (total_banks - 2);
    } else if (address >= 0xA000 && address <= 0xBFFF) {
        bank = c->mapper_state[7];
    } else if (address >= 0xC000 && address <= 0xDFFF) {
        bank = (prg_mode == 0) ? (total_banks - 2) : c->mapper_state[6];
    } else {
        bank = total_banks - 1;
    }

    bank %= total_banks;
    uint32_t offset = bank * 8192 + (address & 0x1FFF);
    return c->prg_rom[offset];
}

static void mmc3_write_prg(void *cart, uint16_t address, uint8_t data) {
    Cartridge *c = (Cartridge*)cart;
    if (address >= 0x6000 && address <= 0x7FFF) {
        if (c->prg_ram) c->prg_ram[address - 0x6000] = data;
        return;
    }

    CPU6502 *cpu = (CPU6502*)c->cpu_context;

    if (address >= 0x8000 && address <= 0x9FFF) {
        if ((address & 1) == 0) {
            c->mapper_state[8] = data;
        } else {
            uint8_t target_reg = c->mapper_state[8] & 0x07;
            c->mapper_state[target_reg] = data;
        }
    } else if (address >= 0xA000 && address <= 0xBFFF) {
        if ((address & 1) == 0) {
            if (c->mirroring != MIRROR_FOUR_SCREEN) {
                c->mirroring = (data & 1) ? MIRROR_HORIZONTAL : MIRROR_VERTICAL;
            }
        }
    } else if (address >= 0xC000 && address <= 0xDFFF) {
        if ((address & 1) == 0) {
            c->mapper_state[9] = data;
        } else {
            c->mapper_state[12] = 1;
        }
    } else {
        if ((address & 1) == 0) {
            c->mapper_state[11] = 0;
            if (cpu) {
                cpu_set_irq_line(cpu, 0, false);
            }
        } else {
            c->mapper_state[11] = 1;
        }
    }
}

static uint8_t mmc3_read_chr(void *cart, uint16_t address) {
    Cartridge *c = (Cartridge*)cart;
    if (c->chr_rom_size == 0) return 0;

    uint8_t bank_select = c->mapper_state[8];
    uint8_t chr_mode = (bank_select >> 7) & 0x01;
    uint32_t bank = 0;
    uint16_t sub_addr = address & 0x03FF;

    if (chr_mode == 0) {
        if (address < 0x0400) bank = c->mapper_state[0] & 0xFE;
        else if (address < 0x0800) bank = c->mapper_state[0] | 0x01;
        else if (address < 0x0C00) bank = c->mapper_state[1] & 0xFE;
        else if (address < 0x1000) bank = c->mapper_state[1] | 0x01;
        else if (address < 0x1400) bank = c->mapper_state[2];
        else if (address < 0x1800) bank = c->mapper_state[3];
        else if (address < 0x1C00) bank = c->mapper_state[4];
        else bank = c->mapper_state[5];
    } else {
        if (address < 0x0400) bank = c->mapper_state[2];
        else if (address < 0x0800) bank = c->mapper_state[3];
        else if (address < 0x0C00) bank = c->mapper_state[4];
        else if (address < 0x1000) bank = c->mapper_state[5];
        else if (address < 0x1400) bank = c->mapper_state[0] & 0xFE;
        else if (address < 0x1800) bank = c->mapper_state[0] | 0x01;
        else if (address < 0x1C00) bank = c->mapper_state[1] & 0xFE;
        else bank = c->mapper_state[1] | 0x01;
    }

    uint32_t total_banks = c->chr_rom_size / 1024;
    bank %= total_banks;
    return c->chr_rom[bank * 1024 + sub_addr];
}

static void mmc3_write_chr(void *cart, uint16_t address, uint8_t data) {
    Cartridge *c = (Cartridge*)cart;
    if (c->chr_rom_size == 0) return;

    uint8_t bank_select = c->mapper_state[8];
    uint8_t chr_mode = (bank_select >> 7) & 0x01;
    uint32_t bank = 0;
    uint16_t sub_addr = address & 0x03FF;

    if (chr_mode == 0) {
        if (address < 0x0400) bank = c->mapper_state[0] & 0xFE;
        else if (address < 0x0800) bank = c->mapper_state[0] | 0x01;
        else if (address < 0x0C00) bank = c->mapper_state[1] & 0xFE;
        else if (address < 0x1000) bank = c->mapper_state[1] | 0x01;
        else if (address < 0x1400) bank = c->mapper_state[2];
        else if (address < 0x1800) bank = c->mapper_state[3];
        else if (address < 0x1C00) bank = c->mapper_state[4];
        else bank = c->mapper_state[5];
    } else {
        if (address < 0x0400) bank = c->mapper_state[2];
        else if (address < 0x0800) bank = c->mapper_state[3];
        else if (address < 0x0C00) bank = c->mapper_state[4];
        else if (address < 0x1000) bank = c->mapper_state[5];
        else if (address < 0x1400) bank = c->mapper_state[0] & 0xFE;
        else if (address < 0x1800) bank = c->mapper_state[0] | 0x01;
        else if (address < 0x1C00) bank = c->mapper_state[1] & 0xFE;
        else bank = c->mapper_state[1] | 0x01;
    }

    uint32_t total_banks = c->chr_rom_size / 1024;
    bank %= total_banks;
    c->chr_rom[bank * 1024 + sub_addr] = data;
}

static void mmc3_clock_irq(void *cart, void *cpu) {
    Cartridge *c = (Cartridge*)cart;
    CPU6502 *cpu_ptr = (CPU6502*)cpu;
    if (cpu_ptr) {
        c->cpu_context = cpu_ptr;
    }

    if (c->mapper_state[10] == 0 || c->mapper_state[12] != 0) {
        c->mapper_state[10] = c->mapper_state[9];
        c->mapper_state[12] = 0;

        if (c->mapper_state[10] == 0 && c->mapper_state[11] != 0) {
            if (cpu_ptr) {
                cpu_set_irq_line(cpu_ptr, 0, true);
            }
        }
    } else {
        c->mapper_state[10]--;
        if (c->mapper_state[10] == 0 && c->mapper_state[11] != 0) {
            if (cpu_ptr) {
                cpu_set_irq_line(cpu_ptr, 0, true);
            }
        }
    }
}

static void mmc3_reset_irq(void *cart) {
    Cartridge *c = (Cartridge*)cart;
    CPU6502 *cpu = (CPU6502*)c->cpu_context;
    if (cpu) {
        cpu_set_irq_line(cpu, 0, false);
    }
}

// ==========================================
// MAPPER 9: MMC2 & MAPPER 10: MMC4
// ==========================================

static uint8_t mmc2_read_prg(void *cart, uint16_t address) {
    Cartridge *c = (Cartridge*)cart;
    if (address >= 0x6000 && address <= 0x7FFF) {
        return c->prg_ram ? c->prg_ram[address - 0x6000] : 0;
    }
    uint32_t total_banks = c->prg_rom_size / 8192;
    if (total_banks == 0) return 0;
    uint32_t bank = 0;

    if (address >= 0x8000 && address <= 0x9FFF) {
        bank = c->mapper_state[0];
    } else if (address >= 0xA000 && address <= 0xBFFF) {
        bank = total_banks - 3;
    } else if (address >= 0xC000 && address <= 0xDFFF) {
        bank = total_banks - 2;
    } else {
        bank = total_banks - 1;
    }

    bank %= total_banks;
    return c->prg_rom[bank * 8192 + (address & 0x1FFF)];
}

static uint8_t mmc4_read_prg(void *cart, uint16_t address) {
    Cartridge *c = (Cartridge*)cart;
    if (address >= 0x6000 && address <= 0x7FFF) {
        return c->prg_ram ? c->prg_ram[address - 0x6000] : 0;
    }
    uint32_t total_banks = c->prg_rom_size / 16384;
    if (total_banks == 0) return 0;
    uint32_t bank = 0;

    if (address >= 0x8000 && address <= 0xBFFF) {
        bank = c->mapper_state[0];
    } else {
        bank = total_banks - 1;
    }

    bank %= total_banks;
    return c->prg_rom[bank * 16384 + (address & 0x3FFF)];
}

static void mmc2_write_prg(void *cart, uint16_t address, uint8_t data) {
    Cartridge *c = (Cartridge*)cart;
    if (address >= 0x6000 && address <= 0x7FFF) {
        if (c->prg_ram) c->prg_ram[address - 0x6000] = data;
        return;
    }
    uint8_t reg = (address >> 12) & 0x07;
    switch (reg) {
        case 2:
            c->mapper_state[0] = data & 0x1F;
            break;
        case 3:
            c->mapper_state[1] = data & 0x1F;
            break;
        case 4:
            c->mapper_state[2] = data & 0x1F;
            break;
        case 5:
            c->mapper_state[3] = data & 0x1F;
            break;
        case 6:
            c->mapper_state[4] = data & 0x1F;
            break;
        case 7:
            c->mirroring = (data & 1) ? MIRROR_HORIZONTAL : MIRROR_VERTICAL;
            break;
    }
}

static uint8_t mmc2_read_chr(void *cart, uint16_t address) {
    Cartridge *c = (Cartridge*)cart;
    if (c->chr_rom_size == 0) return 0;

    uint8_t latch = (address < 0x1000) ? c->mapper_state[5] : c->mapper_state[6];
    uint32_t bank = (address < 0x1000) ? 
        ((latch == 0) ? c->mapper_state[1] : c->mapper_state[2]) :
        ((latch == 0) ? c->mapper_state[3] : c->mapper_state[4]);

    uint32_t total_banks = c->chr_rom_size / 4096;
    if (total_banks > 0) {
        bank %= total_banks;
    }
    uint32_t offset = bank * 4096 + (address & 0x0FFF);
    uint8_t data = c->chr_rom[offset % c->chr_rom_size];

    uint16_t addr = address & 0x1FFF;
    if (addr >= 0x0FD8 && addr <= 0x0FDF) {
        c->mapper_state[5] = 0;
    } else if (addr >= 0x0FE8 && addr <= 0x0FEF) {
        c->mapper_state[5] = 1;
    } else if (addr >= 0x1FD8 && addr <= 0x1FDF) {
        c->mapper_state[6] = 0;
    } else if (addr >= 0x1FE8 && addr <= 0x1FEF) {
        c->mapper_state[6] = 1;
    }

    return data;
}

static void mmc2_write_chr(void *cart, uint16_t address, uint8_t data) {
    Cartridge *c = (Cartridge*)cart;
    if (c->chr_rom_size == 0) return;
    uint32_t bank = 0;
    if (address < 0x1000) {
        uint8_t latch = c->mapper_state[5];
        bank = (latch == 0) ? c->mapper_state[1] : c->mapper_state[2];
    } else {
        uint8_t latch = c->mapper_state[6];
        bank = (latch == 0) ? c->mapper_state[3] : c->mapper_state[4];
    }
    uint32_t offset = bank * 4096 + (address & 0x0FFF);
    c->chr_rom[offset % c->chr_rom_size] = data;
}

// ==========================================
// MAPPER 69: SUNSOFT FME-7
// ==========================================

static uint8_t fme7_read_prg(void *cart, uint16_t address) {
    Cartridge *c = (Cartridge*)cart;
    uint32_t total_banks = c->prg_rom_size / 8192;
    if (total_banks == 0) return 0;

    if (address >= 0x6000 && address <= 0x7FFF) {
        uint8_t reg = c->mapper_state[9];
        if (!(reg & 0x80)) {
            uint32_t bank = (reg & 0x3F) % total_banks;
            return c->prg_rom[bank * 8192 + (address - 0x6000)];
        } else {
            if ((reg & 0x40) && c->prg_ram && c->prg_ram_size > 0) {
                return c->prg_ram[(address - 0x6000) % c->prg_ram_size];
            }
            return 0;
        }
    }

    uint32_t bank = 0;
    if (address >= 0x8000 && address <= 0x9FFF) {
        bank = c->mapper_state[10] & 0x3F;
    } else if (address >= 0xA000 && address <= 0xBFFF) {
        bank = c->mapper_state[11] & 0x3F;
    } else if (address >= 0xC000 && address <= 0xDFFF) {
        bank = c->mapper_state[12] & 0x3F;
    } else if (address >= 0xE000) {
        bank = total_banks - 1;
    }

    bank %= total_banks;
    return c->prg_rom[bank * 8192 + (address & 0x1FFF)];
}

static void fme7_write_prg(void *cart, uint16_t address, uint8_t data) {
    Cartridge *c = (Cartridge*)cart;
    if (address >= 0x6000 && address <= 0x7FFF) {
        uint8_t reg = c->mapper_state[9];
        if ((reg & 0x80) && (reg & 0x40) && c->prg_ram && c->prg_ram_size > 0) {
            c->prg_ram[(address - 0x6000) % c->prg_ram_size] = data;
        }
        return;
    }

    CPU6502 *cpu = (CPU6502*)c->cpu_context;

    if (address >= 0x8000 && address <= 0x9FFF) {
        c->mapper_state[0] = data & 0x0F;
    } else if (address >= 0xA000 && address <= 0xBFFF) {
        uint8_t cmd = c->mapper_state[0];
        if (cmd <= 7) {
            c->mapper_state[1 + cmd] = data;
        } else if (cmd >= 8 && cmd <= 11) {
            c->mapper_state[9 + (cmd - 8)] = data;
        } else if (cmd == 12) {
            c->mapper_state[13] = data & 0x03;
            switch (data & 0x03) {
                case 0: c->mirroring = MIRROR_VERTICAL; break;
                case 1: c->mirroring = MIRROR_HORIZONTAL; break;
                case 2: c->mirroring = MIRROR_ONE_SCREEN_LOW; break;
                case 3: c->mirroring = MIRROR_ONE_SCREEN_HIGH; break;
            }
        } else if (cmd == 13) {
            c->mapper_state[14] = data;
            c->mapper_state[17] = 0;
            if (cpu) {
                cpu_set_irq_line(cpu, 0, false);
            }
        } else if (cmd == 14) {
            c->mapper_state[15] = data;
        } else if (cmd == 15) {
            c->mapper_state[16] = data;
        }
    }
}

static uint8_t fme7_read_chr(void *cart, uint16_t address) {
    Cartridge *c = (Cartridge*)cart;
    if (c->chr_rom_size == 0) return 0;
    uint32_t total_banks = c->chr_rom_size / 1024;
    if (total_banks == 0) return 0;

    uint32_t bank_idx = address / 1024;
    uint32_t bank = c->mapper_state[1 + bank_idx];
    bank %= total_banks;
    return c->chr_rom[bank * 1024 + (address & 0x03FF)];
}

static void fme7_write_chr(void *cart, uint16_t address, uint8_t data) {
    Cartridge *c = (Cartridge*)cart;
    if (c->chr_rom_size == 0) return;
    uint32_t total_banks = c->chr_rom_size / 1024;
    if (total_banks == 0) return;

    uint32_t bank_idx = address / 1024;
    uint32_t bank = c->mapper_state[1 + bank_idx];
    bank %= total_banks;
    c->chr_rom[bank * 1024 + (address & 0x03FF)] = data;
}

static void fme7_clock_irq(void *cart, void *cpu) {
    Cartridge *c = (Cartridge*)cart;
    CPU6502 *cpu_ptr = (CPU6502*)cpu;
    if (cpu_ptr) {
        c->cpu_context = cpu_ptr;
    }

    uint8_t control = c->mapper_state[14];
    if (control & 0x80) {
        uint16_t counter = c->mapper_state[15] | (c->mapper_state[16] << 8);
        uint16_t prev_counter = counter;
        counter--;
        c->mapper_state[15] = counter & 0xFF;
        c->mapper_state[16] = (counter >> 8) & 0xFF;

        if (prev_counter == 0 && counter == 0xFFFF) {
            if (control & 0x01) {
                c->mapper_state[17] = 1;
                if (cpu_ptr) {
                    cpu_set_irq_line(cpu_ptr, 0, true);
                }
            }
        }
    }
}

static void fme7_reset_irq(void *cart) {
    Cartridge *c = (Cartridge*)cart;
    c->mapper_state[17] = 0;
    CPU6502 *cpu = (CPU6502*)c->cpu_context;
    if (cpu) {
        cpu_set_irq_line(cpu, 0, false);
    }
}

// ==========================================
// MAPPER 5: MMC5
// ==========================================

#define MMC5_IRQ_SOURCE 0

static bool mmc5_ram_write_enabled(Cartridge *c) {
    return (c->mmc5_ram_protect[0] == 0x02 && c->mmc5_ram_protect[1] == 0x01);
}

static uint8_t mmc5_read_prg(void *cart, uint16_t address) {
    Cartridge *c = (Cartridge*)cart;

    if (address >= 0x5000 && address <= 0x5206) {
        if (address == 0x5204) {
            uint8_t status = 0;
            if (c->mmc5_irq_pending) status |= 0x80;
            if (c->mmc5_in_frame)    status |= 0x40;
            c->mmc5_irq_pending = false;
            CPU6502 *cpu = (CPU6502*)c->cpu_context;
            if (cpu) {
                cpu_set_irq_line(cpu, MMC5_IRQ_SOURCE, false);
            }
            return status;
        }
        if (address == 0x5205) {
            return (uint8_t)((c->mmc5_mult_a * c->mmc5_mult_b) & 0xFF);
        }
        if (address == 0x5206) {
            return (uint8_t)(((c->mmc5_mult_a * c->mmc5_mult_b) >> 8) & 0xFF);
        }
        return 0;
    }

    if (address >= 0x5C00 && address <= 0x5FFF) {
        if (c->mmc5_exram_mode <= 2) {
            return c->exram[address - 0x5C00];
        }
        return 0;
    }

    if (address >= 0x6000 && address <= 0x7FFF) {
        if (c->prg_ram && c->prg_ram_size > 0) {
            uint32_t bank = (c->mmc5_prg_regs[0] & 0x07);
            uint32_t offset = (bank * 8192) + (address - 0x6000);
            return c->prg_ram[offset % c->prg_ram_size];
        }
        return 0;
    }

    if (address >= 0x8000) {
        uint32_t total_8k_ram = c->prg_ram_size / 8192;
        uint8_t mode = c->mmc5_prg_mode;

        switch (mode) {
            case 0: {
                uint8_t reg = c->mmc5_prg_regs[4] & 0x7C;
                uint32_t offset = ((reg >> 2) * 32768) + (address - 0x8000);
                return c->prg_rom[offset % c->prg_rom_size];
            }
            case 1: {
                if (address < 0xC000) {
                    uint8_t reg = c->mmc5_prg_regs[2];
                    if (!(reg & 0x80) && total_8k_ram > 0) {
                        uint32_t offset = ((reg & 0x06) * 8192) + (address - 0x8000);
                        return c->prg_ram[offset % c->prg_ram_size];
                    }
                    uint32_t offset = (((reg & 0x7E) >> 1) * 16384) + (address - 0x8000);
                    return c->prg_rom[offset % c->prg_rom_size];
                } else {
                    uint8_t reg = c->mmc5_prg_regs[4];
                    uint32_t offset = (((reg & 0x7E) >> 1) * 16384) + (address - 0xC000);
                    return c->prg_rom[offset % c->prg_rom_size];
                }
            }
            case 2: {
                if (address < 0xC000) {
                    uint8_t reg = c->mmc5_prg_regs[2];
                    if (!(reg & 0x80) && total_8k_ram > 0) {
                        uint32_t offset = ((reg & 0x06) * 8192) + (address - 0x8000);
                        return c->prg_ram[offset % c->prg_ram_size];
                    }
                    uint32_t offset = (((reg & 0x7E) >> 1) * 16384) + (address - 0x8000);
                    return c->prg_rom[offset % c->prg_rom_size];
                } else if (address < 0xE000) {
                    uint8_t reg = c->mmc5_prg_regs[3];
                    if (!(reg & 0x80) && total_8k_ram > 0) {
                        uint32_t offset = ((reg & 0x07) * 8192) + (address - 0xC000);
                        return c->prg_ram[offset % c->prg_ram_size];
                    }
                    uint32_t offset = ((reg & 0x7F) * 8192) + (address - 0xC000);
                    return c->prg_rom[offset % c->prg_rom_size];
                } else {
                    uint8_t reg = c->mmc5_prg_regs[4];
                    uint32_t offset = ((reg & 0x7F) * 8192) + (address - 0xE000);
                    return c->prg_rom[offset % c->prg_rom_size];
                }
            }
            case 3: {
                int slot = (address - 0x8000) / 8192;
                uint8_t reg = c->mmc5_prg_regs[slot + 1];
                if (slot < 3 && !(reg & 0x80) && total_8k_ram > 0) {
                    uint32_t offset = ((reg & 0x07) * 8192) + (address & 0x1FFF);
                    return c->prg_ram[offset % c->prg_ram_size];
                }
                uint32_t offset = ((reg & 0x7F) * 8192) + (address & 0x1FFF);
                return c->prg_rom[offset % c->prg_rom_size];
            }
        }
    }

    return 0;
}

static void mmc5_write_prg(void *cart, uint16_t address, uint8_t data) {
    Cartridge *c = (Cartridge*)cart;
    CPU6502 *cpu = (CPU6502*)c->cpu_context;

    if (address >= 0x5100 && address <= 0x5206) {
        switch (address) {
            case 0x5100: c->mmc5_prg_mode = data & 0x03; break;
            case 0x5101: c->mmc5_chr_mode = data & 0x03; break;
            case 0x5102: c->mmc5_ram_protect[0] = data & 0x03; break;
            case 0x5103: c->mmc5_ram_protect[1] = data & 0x03; break;
            case 0x5104: c->mmc5_exram_mode = data & 0x03; break;
            case 0x5105: c->mmc5_nametable_ctrl = data; break;
            case 0x5106: c->mmc5_fill_tile = data; break;
            case 0x5107: c->mmc5_fill_attr = (data & 0x03) | ((data & 0x03) << 2) | ((data & 0x03) << 4) | ((data & 0x03) << 6); break;

            case 0x5113: c->mmc5_prg_regs[0] = data; break;
            case 0x5114: c->mmc5_prg_regs[1] = data; break;
            case 0x5115: c->mmc5_prg_regs[2] = data; break;
            case 0x5116: c->mmc5_prg_regs[3] = data; break;
            case 0x5117: c->mmc5_prg_regs[4] = data | 0x80; break;

            case 0x5120: case 0x5121: case 0x5122: case 0x5123:
            case 0x5124: case 0x5125: case 0x5126: case 0x5127:
                c->mmc5_chr_regs_a[address - 0x5120] = (uint16_t)data | ((uint16_t)(c->mmc5_chr_high & 0x03) << 8);
                c->mmc5_last_chr_a = true;
                break;

            case 0x5128: case 0x5129: case 0x512A: case 0x512B:
                c->mmc5_chr_regs_b[address - 0x5128] = (uint16_t)data | ((uint16_t)(c->mmc5_chr_high & 0x03) << 8);
                c->mmc5_last_chr_a = false;
                break;

            case 0x5130: c->mmc5_chr_high = data & 0x03; break;

            case 0x5203: c->mmc5_irq_target = data; break;
            case 0x5204:
                c->mmc5_irq_enabled = (data & 0x80) != 0;
                if (!c->mmc5_irq_enabled && cpu) {
                    cpu_set_irq_line(cpu, MMC5_IRQ_SOURCE, false);
                }
                break;
            case 0x5205: c->mmc5_mult_a = data; break;
            case 0x5206: c->mmc5_mult_b = data; break;
        }
        return;
    }

    if (address >= 0x5C00 && address <= 0x5FFF) {
        if (c->mmc5_exram_mode <= 2) {
            c->exram[address - 0x5C00] = data;
        }
        return;
    }

    if (address >= 0x6000 && address <= 0x7FFF) {
        if (c->prg_ram && mmc5_ram_write_enabled(c) && c->prg_ram_size > 0) {
            uint32_t bank = (c->mmc5_prg_regs[0] & 0x07);
            uint32_t offset = (bank * 8192) + (address - 0x6000);
            c->prg_ram[offset % c->prg_ram_size] = data;
        }
        return;
    }

    if (address >= 0x8000 && address <= 0xDFFF && mmc5_ram_write_enabled(c)) {
        int slot = (address - 0x8000) / 8192;
        if (slot < 3 && !(c->mmc5_prg_regs[slot + 1] & 0x80) && c->prg_ram_size > 0) {
            uint32_t bank = c->mmc5_prg_regs[slot + 1] & 0x07;
            uint32_t offset = (bank * 8192) + (address & 0x1FFF);
            c->prg_ram[offset % c->prg_ram_size] = data;
        }
    }
}

static uint8_t mmc5_read_chr(void *cart, uint16_t address) {
    Cartridge *c = (Cartridge*)cart;
    if (c->chr_rom_size == 0) return 0;

    bool use_set_a = c->ppu_sprite_size_8x16 ? c->ppu_sprite_fetch : c->mmc5_last_chr_a;
    uint32_t bank = 0;
    uint16_t sub_addr = 0;
    uint32_t base_size = 1024;
    uint8_t mode = c->mmc5_chr_mode;

    if (use_set_a) {
        switch (mode) {
            case 0:
                bank = c->mmc5_chr_regs_a[7];
                sub_addr = address & 0x1FFF;
                base_size = 8192;
                break;
            case 1:
                bank = c->mmc5_chr_regs_a[(address / 4096) * 4 + 3];
                sub_addr = address & 0x0FFF;
                base_size = 4096;
                break;
            case 2:
                bank = c->mmc5_chr_regs_a[(address / 2048) * 2 + 1];
                sub_addr = address & 0x07FF;
                base_size = 2048;
                break;
            case 3:
                bank = c->mmc5_chr_regs_a[address / 1024];
                sub_addr = address & 0x03FF;
                base_size = 1024;
                break;
        }
    } else {
        switch (mode) {
            case 0:
                bank = c->mmc5_chr_regs_b[3];
                sub_addr = address & 0x1FFF;
                base_size = 8192;
                break;
            case 1:
                bank = c->mmc5_chr_regs_b[3];
                sub_addr = address & 0x0FFF;
                base_size = 4096;
                break;
            case 2:
                bank = c->mmc5_chr_regs_b[((address / 2048) & 1) ? 3 : 1];
                sub_addr = address & 0x07FF;
                base_size = 2048;
                break;
            case 3:
                bank = c->mmc5_chr_regs_b[(address / 1024) % 4];
                sub_addr = address & 0x03FF;
                base_size = 1024;
                break;
        }
    }

    uint32_t total_banks = c->chr_rom_size / base_size;
    if (total_banks > 0) {
        bank %= total_banks;
    }

    return c->chr_rom[(bank * base_size + sub_addr) % c->chr_rom_size];
}

static void mmc5_write_chr(void *cart, uint16_t address, uint8_t data) {
    Cartridge *c = (Cartridge*)cart;
    if (c->chr_rom_size == 0) return;
    c->chr_rom[address % c->chr_rom_size] = data;
}

static void mmc5_clock_irq(void *cart, void *cpu) {
    Cartridge *c = (Cartridge*)cart;
    CPU6502 *cpu_ptr = (CPU6502*)cpu;
    if (cpu_ptr) {
        c->cpu_context = cpu_ptr;
    }

    c->mmc5_scanline++;
    c->mmc5_in_frame = true;

    if (c->mmc5_scanline == c->mmc5_irq_target) {
        c->mmc5_irq_pending = true;
        if (c->mmc5_irq_enabled && cpu_ptr) {
            cpu_set_irq_line(cpu_ptr, MMC5_IRQ_SOURCE, true);
        }
    }
}

static void mmc5_reset_irq(void *cart) {
    Cartridge *c = (Cartridge*)cart;
    c->mmc5_scanline = 0;
    c->mmc5_in_frame = false;
    c->mmc5_irq_pending = false;
    CPU6502 *cpu = (CPU6502*)c->cpu_context;
    if (cpu) {
        cpu_set_irq_line(cpu, MMC5_IRQ_SOURCE, false);
    }
}

// ==========================================
// MAPPER 227: Multicart (1200-in-1 / 42-in-1)
// ==========================================

static void m227_update_banks(Cartridge *c, uint16_t addr) {
    uint16_t prg_bank = ((addr >> 2) & 0x1F) | ((addr & 0x0100) >> 3);
    bool s_flag   = (addr & 0x0001) != 0;
    bool l_flag   = (addr & 0x0200) != 0;
    bool prg_mode = (addr & 0x0080) != 0;

    uint32_t bank0 = 0;
    uint32_t bank1 = 0;

    if (prg_mode) {
        if (s_flag) {
            bank0 = prg_bank & 0xFE;
            bank1 = (prg_bank & 0xFE) | 1;
        } else {
            bank0 = prg_bank;
            bank1 = prg_bank;
        }
    } else {
        if (s_flag) {
            bank0 = prg_bank & 0x3E;
            bank1 = l_flag ? (prg_bank | 0x07) : (prg_bank & 0x38);
        } else {
            bank0 = prg_bank;
            bank1 = l_flag ? (prg_bank | 0x07) : (prg_bank & 0x38);
        }
    }

    uint32_t total_banks = c->prg_rom_size / 16384;
    if (total_banks > 0) {
        bank0 %= total_banks;
        bank1 %= total_banks;
    }

    c->mapper_state[2] = (uint8_t)bank0;
    c->mapper_state[3] = (uint8_t)bank1;
    c->mirroring = (addr & 0x0002) ? MIRROR_HORIZONTAL : MIRROR_VERTICAL;
}

static uint8_t m227_read_prg(void *cart, uint16_t address) {
    Cartridge *c = (Cartridge*)cart;
    if (address >= 0x6000 && address <= 0x7FFF) {
        return c->prg_ram ? c->prg_ram[address - 0x6000] : 0;
    }
    if (address >= 0x8000 && address <= 0xBFFF) {
        uint32_t bank = c->mapper_state[2];
        return c->prg_rom[bank * 16384 + (address & 0x3FFF)];
    }
    if (address >= 0xC000) {
        uint32_t bank = c->mapper_state[3];
        return c->prg_rom[bank * 16384 + (address & 0x3FFF)];
    }
    return 0;
}

static void m227_write_prg(void *cart, uint16_t address, uint8_t data) {
    (void)data;
    Cartridge *c = (Cartridge*)cart;
    if (address >= 0x6000 && address <= 0x7FFF) {
        if (c->prg_ram) c->prg_ram[address - 0x6000] = data;
        return;
    }
    if (address >= 0x8000) {
        m227_update_banks(c, address);
    }
}

// ==========================================
// LOADER & LIFECYCLE
// ==========================================

Cartridge* cartridge_load(const char *filepath) {
    FILE *f = fopen(filepath, "rb");
    if (!f) {
        fprintf(stderr, "Error: Could not open iNES file '%s'\n", filepath);
        return NULL;
    }

    uint8_t header[16];
    if (fread(header, 1, 16, f) != 16) {
        fprintf(stderr, "Error: Failed to read iNES header from '%s'\n", filepath);
        fclose(f);
        return NULL;
    }

    if (header[0] != 'N' || header[1] != 'E' || header[2] != 'S' || header[3] != 0x1A) {
        fprintf(stderr, "Error: Invalid iNES header signature in '%s'\n", filepath);
        fclose(f);
        return NULL;
    }

    Cartridge *cart = calloc(1, sizeof(Cartridge));
    if (!cart) {
        fclose(f);
        return NULL;
    }

    uint8_t prg_rom_chunks = header[4];
    uint8_t chr_rom_chunks = header[5];
    uint8_t flags6 = header[6];
    uint8_t flags7 = header[7];

    cart->prg_rom_size = prg_rom_chunks * 16384;
    cart->chr_rom_size = chr_rom_chunks * 8192;
    cart->mapper_id = (uint8_t)((flags7 & 0xF0) | (flags6 >> 4));

    if (flags6 & 0x08) {
        cart->mirroring = MIRROR_FOUR_SCREEN;
    } else if (flags6 & 0x01) {
        cart->mirroring = MIRROR_VERTICAL;
    } else {
        cart->mirroring = MIRROR_HORIZONTAL;
    }

    if (flags6 & 0x04) {
        fseek(f, 512, SEEK_CUR);
    }

    cart->prg_rom = malloc(cart->prg_rom_size);
    if (cart->prg_rom_size > 0 && fread(cart->prg_rom, 1, cart->prg_rom_size, f) != cart->prg_rom_size) {
        fprintf(stderr, "Error: Failed to read PRG-ROM data\n");
        cartridge_free(cart);
        fclose(f);
        return NULL;
    }
    pad_and_mirror_rom(&cart->prg_rom, &cart->prg_rom_size);

    if (cart->chr_rom_size > 0) {
        cart->chr_rom = malloc(cart->chr_rom_size);
        if (fread(cart->chr_rom, 1, cart->chr_rom_size, f) != cart->chr_rom_size) {
            fprintf(stderr, "Error: Failed to read CHR-ROM data\n");
            cartridge_free(cart);
            fclose(f);
            return NULL;
        }
        pad_and_mirror_rom(&cart->chr_rom, &cart->chr_rom_size);
    } else {
        cart->chr_rom_size = 8192;
        cart->chr_rom = calloc(1, 8192);
    }

    fclose(f);

    cart->prg_ram_size = 8192;
    cart->prg_ram = calloc(1, cart->prg_ram_size);

    bool battery = (flags6 & 0x02) != 0;
    cart->has_battery = battery;
    if (battery) {
        strncpy(cart->save_filepath, filepath, sizeof(cart->save_filepath) - 5);
        cart->save_filepath[sizeof(cart->save_filepath) - 5] = '\0';
        char *ext = strrchr(cart->save_filepath, '.');
        if (ext) {
            strcpy(ext, ".sav");
        } else {
            strcat(cart->save_filepath, ".sav");
        }
        FILE *sf = fopen(cart->save_filepath, "rb");
        if (sf) {
            fread(cart->prg_ram, 1, cart->prg_ram_size, sf);
            fclose(sf);
            printf("Loaded battery-backed save file: %s\n", cart->save_filepath);
        }
    }

    if (cart->mapper_id == 0) {
        cart->read_prg = nrom_read_prg;
        cart->write_prg = nrom_write_prg;
        cart->read_chr = nrom_read_chr;
        cart->write_chr = nrom_write_chr;
    } else if (cart->mapper_id == 3) {
        cart->read_prg = cnrom_read_prg;
        cart->write_prg = cnrom_write_prg;
        cart->read_chr = cnrom_read_chr;
        cart->write_chr = nrom_write_chr;
        cart->mapper_state[0] = 0;
    } else if (cart->mapper_id == 7) {
        cart->read_prg = axrom_read_prg;
        cart->write_prg = axrom_write_prg;
        cart->read_chr = nrom_read_chr;
        cart->write_chr = nrom_write_chr;
        cart->mapper_state[0] = 0;
    } else if (cart->mapper_id == 1) {
        cart->read_prg = mmc1_read_prg;
        cart->write_prg = mmc1_write_prg;
        cart->read_chr = mmc1_read_chr;
        cart->write_chr = mmc1_write_chr;
        cart->mapper_state[0] = 0;
        cart->mapper_state[1] = 0;
        cart->mapper_state[2] = 0x0C;
        cart->mapper_state[3] = 0;
        cart->mapper_state[4] = 0;
        cart->mapper_state[5] = 0;
    } else if (cart->mapper_id == 2) {
        cart->read_prg = uxrom_read_prg;
        cart->write_prg = uxrom_write_prg;
        cart->read_chr = nrom_read_chr;
        cart->write_chr = nrom_write_chr;
        cart->mapper_state[0] = 0;
    } else if (cart->mapper_id == 4) {
        cart->read_prg = mmc3_read_prg;
        cart->write_prg = mmc3_write_prg;
        cart->read_chr = mmc3_read_chr;
        cart->write_chr = mmc3_write_chr;
        cart->clock_irq = mmc3_clock_irq;
        cart->reset_irq = mmc3_reset_irq;
        cart->mapper_state[0] = 0;
        cart->mapper_state[1] = 0;
        cart->mapper_state[6] = 0;
        cart->mapper_state[7] = 0;
        cart->mapper_state[8] = 0;
    } else if (cart->mapper_id == 5) {
        cart->read_prg = mmc5_read_prg;
        cart->write_prg = mmc5_write_prg;
        cart->read_chr = mmc5_read_chr;
        cart->write_chr = mmc5_write_chr;
        cart->clock_irq = mmc5_clock_irq;
        cart->reset_irq = mmc5_reset_irq;

        cart->prg_ram_size = 65536;
        free(cart->prg_ram);
        cart->prg_ram = calloc(1, cart->prg_ram_size);

        cart->mmc5_prg_mode = 3;
        cart->mmc5_chr_mode = 0;
        cart->mmc5_ram_protect[0] = 0x02;
        cart->mmc5_ram_protect[1] = 0x01;
        cart->mmc5_last_chr_a = true;
        cart->mmc5_nametable_ctrl = 0x00;
        cart->mmc5_prg_regs[0] = 0x00;
        cart->mmc5_prg_regs[1] = 0xFF;
        cart->mmc5_prg_regs[2] = 0xFF;
        cart->mmc5_prg_regs[3] = 0xFF;
        cart->mmc5_prg_regs[4] = 0xFF;
    } else if (cart->mapper_id == 9) {
        cart->read_prg = mmc2_read_prg;
        cart->write_prg = mmc2_write_prg;
        cart->read_chr = mmc2_read_chr;
        cart->write_chr = mmc2_write_chr;
        cart->mapper_state[0] = 0;
        cart->mapper_state[1] = 0;
        cart->mapper_state[2] = 0;
        cart->mapper_state[3] = 0;
        cart->mapper_state[4] = 0;
        cart->mapper_state[5] = 1;
        cart->mapper_state[6] = 1;
    } else if (cart->mapper_id == 10) {
        cart->read_prg = mmc4_read_prg;
        cart->write_prg = mmc2_write_prg;
        cart->read_chr = mmc2_read_chr;
        cart->write_chr = mmc2_write_chr;
        cart->mapper_state[0] = 0;
        cart->mapper_state[1] = 0;
        cart->mapper_state[2] = 0;
        cart->mapper_state[3] = 0;
        cart->mapper_state[4] = 0;
        cart->mapper_state[5] = 1;
        cart->mapper_state[6] = 1;
    } else if (cart->mapper_id == 69) {
        cart->read_prg = fme7_read_prg;
        cart->write_prg = fme7_write_prg;
        cart->read_chr = fme7_read_chr;
        cart->write_chr = fme7_write_chr;
        cart->clock_irq = fme7_clock_irq;
        cart->reset_irq = fme7_reset_irq;
        memset(cart->mapper_state, 0, sizeof(cart->mapper_state));
        uint32_t total_banks = cart->prg_rom_size / 8192;
        if (total_banks > 0) {
            cart->mapper_state[10] = 0;
            cart->mapper_state[11] = 1;
            cart->mapper_state[12] = (total_banks >= 2) ? (total_banks - 2) : 0;
            cart->mapper_state[9] = 0x00;
            cart->cpu_clocked_irq = true;
        }
    } else if (cart->mapper_id == 227) {
        cart->read_prg = m227_read_prg;
        cart->write_prg = m227_write_prg;
        cart->read_chr = nrom_read_chr;
        cart->write_chr = nrom_write_chr;
        memset(cart->mapper_state, 0, sizeof(cart->mapper_state));
        m227_update_banks(cart, 0x0000);
    } else {
        fprintf(stderr, "Error: Unsupported mapper ID %u\n", cart->mapper_id);
        cartridge_free(cart);
        return NULL;
    }

    return cart;
}

void cartridge_save_battery(Cartridge *cart) {
    if (cart && cart->has_battery && cart->prg_ram) {
        FILE *sf = fopen(cart->save_filepath, "wb");
        if (sf) {
            fwrite(cart->prg_ram, 1, cart->prg_ram_size, sf);
            fclose(sf);
            printf("Saved battery-backed progress to: %s\n", cart->save_filepath);
        }
    }
}

void cartridge_free(Cartridge *cart) {
    if (cart) {
        cartridge_save_battery(cart);
        free(cart->prg_rom);
        free(cart->chr_rom);
        free(cart->prg_ram);
        free(cart);
    }
}