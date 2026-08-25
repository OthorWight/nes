#include "cartridge.h"
#include "cpu6502.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t nrom_read_prg(void *cart, uint16_t address) {
    Cartridge *c = (Cartridge*)cart;
    uint16_t mapped_address = (uint16_t)(address - 0x8000);
    if (c->prg_rom_size == 16384) {
        mapped_address &= 0x3FFF;
    }
    return c->prg_rom[mapped_address];
}

static void nrom_write_prg(void *cart, uint16_t address, uint8_t data) {
    (void)cart; (void)address; (void)data;
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

static uint8_t uxrom_read_prg(void *cart, uint16_t address) {
    Cartridge *c = (Cartridge*)cart;
    uint32_t last_bank_offset = c->prg_rom_size - 16384;
    if (address < 0xC000) {
        uint32_t bank = c->mapper_state[0];
        return c->prg_rom[(bank * 16384) + (address - 0x8000)];
    } else {
        return c->prg_rom[last_bank_offset + (address - 0xC000)];
    }
}

static void uxrom_write_prg(void *cart, uint16_t address, uint8_t data) {
    (void)address;
    Cartridge *c = (Cartridge*)cart;
    uint32_t total_banks = c->prg_rom_size / 16384;
    if (total_banks > 0) {
        c->mapper_state[0] = data % total_banks;
    }
}

static uint8_t mmc1_read_prg(void *cart, uint16_t address) {
    Cartridge *c = (Cartridge*)cart;
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
            uint32_t last_bank_offset = c->prg_rom_size - 16384;
            offset = last_bank_offset + (address - 0xC000);
        }
    }

    if (offset < c->prg_rom_size) {
        return c->prg_rom[offset];
    }
    return 0;
}

static void mmc1_write_prg(void *cart, uint16_t address, uint8_t data) {
    Cartridge *c = (Cartridge*)cart;
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

static uint8_t mmc3_read_prg(void *cart, uint16_t address) {
    Cartridge *c = (Cartridge*)cart;
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

static uint8_t mmc2_read_prg(void *cart, uint16_t address) {
    Cartridge *c = (Cartridge*)cart;
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
    uint8_t reg = (address >> 12) & 0x07;
    switch (reg) {
        case 2: // $A000: PRG ROM Select
            c->mapper_state[0] = data & 0x1F;
            break;
        case 3: // $B000: CHR ROM 0 ($FD Bank)
            c->mapper_state[1] = data & 0x1F;
            break;
        case 4: // $C000: CHR ROM 0 ($FE Bank)
            c->mapper_state[2] = data & 0x1F;
            break;
        case 5: // $D000: CHR ROM 1 ($FD Bank)
            c->mapper_state[3] = data & 0x1F;
            break;
        case 6: // $E000: CHR ROM 1 ($FE Bank)
            c->mapper_state[4] = data & 0x1F;
            break;
        case 7: // $F000: Mirroring (0: Vertical, 1: Horizontal)
            c->mirroring = (data & 1) ? MIRROR_HORIZONTAL : MIRROR_VERTICAL;
            break;
    }
}

static uint8_t mmc2_read_chr(void *cart, uint16_t address) {
    Cartridge *c = (Cartridge*)cart;
    if (c->chr_rom_size == 0) return 0;

    // 1. Read the byte using the CURRENT latch state BEFORE updating it
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

    // 2. Update the MMC2 latch state for all subsequent reads
    uint16_t addr = address & 0x1FFF;
    if (addr >= 0x0FD8 && addr <= 0x0FDF) {
        c->mapper_state[5] = 0; // Latch 0 -> FD
    } else if (addr >= 0x0FE8 && addr <= 0x0FEF) {
        c->mapper_state[5] = 1; // Latch 0 -> FE
    } else if (addr >= 0x1FD8 && addr <= 0x1FDF) {
        c->mapper_state[6] = 0; // Latch 1 -> FD
    } else if (addr >= 0x1FE8 && addr <= 0x1FEF) {
        c->mapper_state[6] = 1; // Latch 1 -> FE
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
static void mmc3_clock_irq(void *cart, void *cpu) {
    Cartridge *c = (Cartridge*)cart;
    CPU6502 *cpu_ptr = (CPU6502*)cpu;
    if (c->mapper_state[12] != 0 || c->mapper_state[10] == 0) {
        c->mapper_state[10] = c->mapper_state[9];
        c->mapper_state[12] = 0;
    } else {
        c->mapper_state[10]--;
    }
    if (c->mapper_state[10] == 0) {
        if (c->mapper_state[11] != 0) {
            cpu_trigger_irq(cpu_ptr);
        }
    }
}

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

    if (cart->chr_rom_size > 0) {
        cart->chr_rom = malloc(cart->chr_rom_size);
        if (fread(cart->chr_rom, 1, cart->chr_rom_size, f) != cart->chr_rom_size) {
            fprintf(stderr, "Error: Failed to read CHR-ROM data\n");
            cartridge_free(cart);
            fclose(f);
            return NULL;
        }
    } else {
        cart->chr_rom_size = 8192;
        cart->chr_rom = calloc(1, 8192);
    }

    fclose(f);

    bool battery = (flags6 & 0x02) != 0;
    cart->has_battery = battery;
    if (battery || cart->mapper_id == 1 || cart->mapper_id == 4 || cart->mapper_id == 9 || cart->mapper_id == 10) {
        cart->prg_ram_size = 8192;
        cart->prg_ram = calloc(1, cart->prg_ram_size);
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
    }

    if (cart->mapper_id == 0) {
        cart->read_prg = nrom_read_prg;
        cart->write_prg = nrom_write_prg;
        cart->read_chr = nrom_read_chr;
        cart->write_chr = nrom_write_chr;
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
        cart->mapper_state[0] = 0;
        cart->mapper_state[1] = 0;
        cart->mapper_state[6] = 0;
        cart->mapper_state[7] = 0;
        cart->mapper_state[8] = 0;
    } else if (cart->mapper_id == 9) {
        cart->read_prg = mmc2_read_prg;
        cart->write_prg = mmc2_write_prg;
        cart->read_chr = mmc2_read_chr;
        cart->write_chr = mmc2_write_chr;
        cart->mapper_state[0] = 0; // PRG bank
        cart->mapper_state[1] = 0; // CHR bank 0 FD
        cart->mapper_state[2] = 0; // CHR bank 0 FE
        cart->mapper_state[3] = 0; // CHR bank 1 FD
        cart->mapper_state[4] = 0; // CHR bank 1 FE
        cart->mapper_state[5] = 1; // Latch 0 power-up default ($FE)
        cart->mapper_state[6] = 1; // Latch 1 power-up default ($FE)
    } else if (cart->mapper_id == 10) {
        cart->read_prg = mmc4_read_prg;
        cart->write_prg = mmc2_write_prg;
        cart->read_chr = mmc2_read_chr;
        cart->write_chr = mmc2_write_chr;
        cart->mapper_state[0] = 0; // PRG bank
        cart->mapper_state[1] = 0; // CHR bank 0 FD
        cart->mapper_state[2] = 0; // CHR bank 0 FE
        cart->mapper_state[3] = 0; // CHR bank 1 FD
        cart->mapper_state[4] = 0; // CHR bank 1 FE
        cart->mapper_state[5] = 1; // Latch 0 power-up default ($FE)
        cart->mapper_state[6] = 1; // Latch 1 power-up default ($FE)
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