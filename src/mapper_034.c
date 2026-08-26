#include "mappers.h"
#include <string.h>

static uint8_t m034_read_prg(void *cart, uint16_t address) {
    Cartridge *c = (Cartridge*)cart;
    if (address >= 0x8000) {
        uint32_t bank = c->mapper_state[0];
        return c->prg_rom[(bank * 32768 + (address - 0x8000)) % c->prg_rom_size];
    }
    return 0;
}

static void m034_write_prg(void *cart, uint16_t address, uint8_t data) {
    Cartridge *c = (Cartridge*)cart;
    if (address == 0x7FFD) {
        c->mapper_state[0] = data & 0x03; // NINA PRG
    } else if (address == 0x7FFE) {
        c->mapper_state[1] = data & 0x0F; // NINA CHR 0 ($0000-$0FFF)
    } else if (address == 0x7FFF) {
        c->mapper_state[2] = data & 0x0F; // NINA CHR 1 ($1000-$1FFF)
    } else if (address >= 0x8000) {
        c->mapper_state[0] = data & 0x03; // BNROM PRG
    }
}

static uint8_t m034_read_chr(void *cart, uint16_t address) {
    Cartridge *c = (Cartridge*)cart;
    if (c->chr_rom_size == 0) return 0;
    if (address < 0x1000) {
        uint32_t bank = c->mapper_state[1];
        return c->chr_rom[(bank * 4096 + address) % c->chr_rom_size];
    } else {
        uint32_t bank = c->mapper_state[2];
        return c->chr_rom[(bank * 4096 + (address - 0x1000)) % c->chr_rom_size];
    }
}

static void m034_write_chr(void *cart, uint16_t address, uint8_t data) {
    Cartridge *c = (Cartridge*)cart;
    if (c->chr_rom_size == 0) return;
    if (address < 0x1000) {
        uint32_t bank = c->mapper_state[1];
        c->chr_rom[(bank * 4096 + address) % c->chr_rom_size] = data;
    } else {
        uint32_t bank = c->mapper_state[2];
        c->chr_rom[(bank * 4096 + (address - 0x1000)) % c->chr_rom_size] = data;
    }
}

void mapper_034_init(Cartridge *cart) {
    cart->read_prg = m034_read_prg;
    cart->write_prg = m034_write_prg;
    cart->read_chr = m034_read_chr;
    cart->write_chr = m034_write_chr;
    memset(cart->mapper_state, 0, sizeof(cart->mapper_state));
    cart->mapper_state[2] = 1;
}