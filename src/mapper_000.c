#include "mappers.h"

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

void mapper_000_init(Cartridge *cart) {
    cart->read_prg = nrom_read_prg;
    cart->write_prg = nrom_write_prg;
    cart->read_chr = nrom_read_chr;
    cart->write_chr = nrom_write_chr;
}