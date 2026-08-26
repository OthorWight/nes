#include "mappers.h"
#include <stddef.h>

static uint8_t cnrom_read_prg(void *cart, uint16_t address) {
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

void mapper_003_init(Cartridge *cart) {
    cart->read_prg = cnrom_read_prg;
    cart->write_prg = cnrom_write_prg;
    cart->read_chr = cnrom_read_chr;
    // Safe fallback to write RAM CHR if CHR RAM is used instead of CHR ROM
    cart->write_chr = cart->write_chr ? cart->write_chr : NULL;
    cart->mapper_state[0] = 0;
}