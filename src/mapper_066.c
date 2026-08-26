#include "mappers.h"
#include <string.h>

static uint8_t m066_read_prg(void *cart, uint16_t address) {
    Cartridge *c = (Cartridge*)cart;
    if (address >= 0x8000) {
        uint32_t bank = c->mapper_state[0];
        return c->prg_rom[(bank * 32768 + (address - 0x8000)) % c->prg_rom_size];
    }
    return 0;
}

static void m066_write_prg(void *cart, uint16_t address, uint8_t data) {
    Cartridge *c = (Cartridge*)cart;
    if (address >= 0x8000) {
        c->mapper_state[0] = (data >> 4) & 0x03; // 32 KB PRG Bank
        c->mapper_state[1] = data & 0x03;        // 8 KB CHR Bank
    }
}

static uint8_t m066_read_chr(void *cart, uint16_t address) {
    Cartridge *c = (Cartridge*)cart;
    if (c->chr_rom_size > 0) {
        uint32_t bank = c->mapper_state[1];
        return c->chr_rom[(bank * 8192 + (address & 0x1FFF)) % c->chr_rom_size];
    }
    return 0;
}

static void m066_write_chr(void *cart, uint16_t address, uint8_t data) {
    Cartridge *c = (Cartridge*)cart;
    if (c->chr_rom_size > 0) {
        uint32_t bank = c->mapper_state[1];
        c->chr_rom[(bank * 8192 + (address & 0x1FFF)) % c->chr_rom_size] = data;
    }
}

void mapper_066_init(Cartridge *cart) {
    cart->read_prg = m066_read_prg;
    cart->write_prg = m066_write_prg;
    cart->read_chr = m066_read_chr;
    cart->write_chr = m066_write_chr;
    memset(cart->mapper_state, 0, sizeof(cart->mapper_state));
}