#include "mappers.h"
#include "cpu6502.h"
#include <string.h>

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

void mapper_069_init(Cartridge *cart) {
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
}