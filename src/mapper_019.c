#include "mappers.h"
#include "cpu6502.h"
#include <string.h>

static uint8_t m019_read_prg(void *cart, uint16_t address) {
    Cartridge *c = (Cartridge*)cart;
    if (address < 0x8000) {
        if (address >= 0x6000 && c->prg_ram) {
            return c->prg_ram[address - 0x6000];
        }
        return 0;
    }

    uint32_t total_banks = c->prg_rom_size / 8192;
    if (total_banks == 0) return 0;

    uint32_t bank = 0;
    if (address >= 0x8000 && address <= 0x9FFF) {
        bank = c->mapper_state[12] & 0x3F;
    } else if (address >= 0xA000 && address <= 0xBFFF) {
        bank = c->mapper_state[13] & 0x3F;
    } else if (address >= 0xC000 && address <= 0xDFFF) {
        bank = c->mapper_state[14] & 0x3F;
    } else {
        bank = total_banks - 1;
    }

    bank %= total_banks;
    return c->prg_rom[bank * 8192 + (address & 0x1FFF)];
}

static void m019_write_prg(void *cart, uint16_t address, uint8_t data) {
    Cartridge *c = (Cartridge*)cart;
    CPU6502 *cpu = (CPU6502*)c->cpu_context;

    if (address >= 0x5000 && address <= 0x57FF) {
        c->mapper_state[15] = data;
        c->mapper_state[17] = data;
        if (cpu) {
            cpu_set_irq_line(cpu, 0, false);
        }
    } else if (address >= 0x5800 && address <= 0x5FFF) {
        c->mapper_state[16] = data;
        c->mapper_state[18] = data & 0x7F;
        if (cpu) {
            cpu_set_irq_line(cpu, 0, false);
        }
    } else if (address >= 0x6000 && address <= 0x7FFF) {
        if (c->prg_ram) {
            c->prg_ram[address - 0x6000] = data;
        }
    } else if (address >= 0x8000 && address <= 0xBFFF) {
        uint8_t slot = (address - 0x8000) / 0x0800;
        c->mapper_state[slot] = data;
    } else if (address >= 0xC000 && address <= 0xDFFF) {
        uint8_t slot = (address - 0xC000) / 0x0800;
        c->mapper_state[8 + slot] = data;
    } else if (address >= 0xE000 && address <= 0xE7FF) {
        c->mapper_state[12] = data & 0x3F;
    } else if (address >= 0xE800 && address <= 0xEFFF) {
        c->mapper_state[13] = data & 0x3F;
    } else if (address >= 0xF000 && address <= 0xF7FF) {
        c->mapper_state[14] = data & 0x3F;
    }
}

static uint8_t m019_read_chr(void *cart, uint16_t address) {
    Cartridge *c = (Cartridge*)cart;
    if (c->chr_rom_size == 0) return 0;
    uint32_t total_banks = c->chr_rom_size / 1024;
    if (total_banks == 0) return 0;

    uint8_t slot = (address / 1024) & 0x07;
    uint32_t bank = c->mapper_state[slot];
    bank %= total_banks;

    return c->chr_rom[bank * 1024 + (address & 0x03FF)];
}

static void m019_write_chr(void *cart, uint16_t address, uint8_t data) {
    Cartridge *c = (Cartridge*)cart;
    if (c->chr_rom_size == 0) return;
    uint32_t total_banks = c->chr_rom_size / 1024;
    if (total_banks == 0) return;

    uint8_t slot = (address / 1024) & 0x07;
    uint32_t bank = c->mapper_state[slot];
    bank %= total_banks;

    c->chr_rom[bank * 1024 + (address & 0x03FF)] = data;
}

static void m019_clock_irq(void *cart, void *cpu) {
    Cartridge *c = (Cartridge*)cart;
    CPU6502 *cpu_ptr = (CPU6502*)cpu;
    if (cpu_ptr) {
        c->cpu_context = cpu_ptr;
    }

    if (!(c->mapper_state[16] & 0x80)) return;

    uint16_t counter = c->mapper_state[17] | (c->mapper_state[18] << 8);
    if (counter < 0x7FFF) {
        counter++;
        c->mapper_state[17] = counter & 0xFF;
        c->mapper_state[18] = (counter >> 8) & 0x7F;

        if (counter == 0x7FFF && cpu_ptr) {
            cpu_set_irq_line(cpu_ptr, 0, true);
        }
    }
}

static void m019_reset_irq(void *cart) {
    Cartridge *c = (Cartridge*)cart;
    CPU6502 *cpu = (CPU6502*)c->cpu_context;
    if (cpu) {
        cpu_set_irq_line(cpu, 0, false);
    }
}

void mapper_019_init(Cartridge *cart) {
    cart->read_prg = m019_read_prg;
    cart->write_prg = m019_write_prg;
    cart->read_chr = m019_read_chr;
    cart->write_chr = m019_write_chr;
    cart->clock_irq = m019_clock_irq;
    cart->reset_irq = m019_reset_irq;
    cart->cpu_clocked_irq = true;

    memset(cart->mapper_state, 0, sizeof(cart->mapper_state));

    cart->mapper_state[8] = 0xE0;
    cart->mapper_state[9] = 0xE1;
    cart->mapper_state[10] = 0xE0;
    cart->mapper_state[11] = 0xE1;
    cart->mapper_state[12] = 0;
    cart->mapper_state[13] = 1;
    cart->mapper_state[14] = 2;
}