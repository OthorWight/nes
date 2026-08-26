#include "mappers.h"
#include <string.h>

static uint8_t m206_read_prg(void *cart, uint16_t address) {
    Cartridge *c = (Cartridge*)cart;
    uint32_t total_banks = c->prg_rom_size / 8192;
    if (total_banks == 0) return 0;

    uint32_t bank = 0;
    if (address >= 0x8000 && address <= 0x9FFF) {
        bank = c->mapper_state[6] & 0x3F;
    } else if (address >= 0xA000 && address <= 0xBFFF) {
        bank = c->mapper_state[7] & 0x3F;
    } else if (address >= 0xC000 && address <= 0xDFFF) {
        bank = (total_banks >= 2) ? total_banks - 2 : 0;
    } else if (address >= 0xE000) {
        bank = total_banks - 1;
    }

    bank %= total_banks;
    return c->prg_rom[bank * 8192 + (address & 0x1FFF)];
}

static void m206_write_prg(void *cart, uint16_t address, uint8_t data) {
    Cartridge *c = (Cartridge*)cart;
    if (address >= 0x8000) {
        if ((address & 1) == 0) {
            c->mapper_state[8] = data & 0x07;
        } else {
            uint8_t target = c->mapper_state[8] & 0x07;
            c->mapper_state[target] = data;
        }
    }
}

static uint8_t m206_read_chr(void *cart, uint16_t address) {
    Cartridge *c = (Cartridge*)cart;
    if (c->chr_rom_size == 0) return 0;
    uint32_t total_banks = c->chr_rom_size / 1024;
    if (total_banks == 0) return 0;

    uint32_t bank = 0;
    if (address < 0x0800) {
        bank = (c->mapper_state[0] & 0x3E) | ((address >> 10) & 1);
    } else if (address < 0x1000) {
        bank = (c->mapper_state[1] & 0x3E) | ((address >> 10) & 1);
    } else if (address < 0x1400) {
        bank = c->mapper_state[2] & 0x3F;
    } else if (address < 0x1800) {
        bank = c->mapper_state[3] & 0x3F;
    } else if (address < 0x1C00) {
        bank = c->mapper_state[4] & 0x3F;
    } else {
        bank = c->mapper_state[5] & 0x3F;
    }

    bank %= total_banks;
    return c->chr_rom[bank * 1024 + (address & 0x03FF)];
}

static void m206_write_chr(void *cart, uint16_t address, uint8_t data) {
    Cartridge *c = (Cartridge*)cart;
    if (c->chr_rom_size == 0) return;
    uint32_t total_banks = c->chr_rom_size / 1024;
    if (total_banks == 0) return;

    uint32_t bank = 0;
    if (address < 0x0800) {
        bank = (c->mapper_state[0] & 0x3E) | ((address >> 10) & 1);
    } else if (address < 0x1000) {
        bank = (c->mapper_state[1] & 0x3E) | ((address >> 10) & 1);
    } else if (address < 0x1400) {
        bank = c->mapper_state[2] & 0x3F;
    } else if (address < 0x1800) {
        bank = c->mapper_state[3] & 0x3F;
    } else if (address < 0x1C00) {
        bank = c->mapper_state[4] & 0x3F;
    } else {
        bank = c->mapper_state[5] & 0x3F;
    }

    bank %= total_banks;
    c->chr_rom[bank * 1024 + (address & 0x03FF)] = data;
}

void mapper_206_init(Cartridge *cart) {
    cart->read_prg = m206_read_prg;
    cart->write_prg = m206_write_prg;
    cart->read_chr = m206_read_chr;
    cart->write_chr = m206_write_chr;
    memset(cart->mapper_state, 0, sizeof(cart->mapper_state));
}