#include "mappers.h"

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

static uint8_t uxrom_read_chr(void *cart, uint16_t address) {
    Cartridge *c = (Cartridge*)cart;
    if (c->chr_rom_size > 0) {
        return c->chr_rom[address & 0x1FFF];
    }
    return 0;
}

static void uxrom_write_chr(void *cart, uint16_t address, uint8_t data) {
    Cartridge *c = (Cartridge*)cart;
    if (c->chr_rom_size > 0) {
        c->chr_rom[address & 0x1FFF] = data;
    }
}

void mapper_002_init(Cartridge *cart) {
    cart->read_prg = uxrom_read_prg;
    cart->write_prg = uxrom_write_prg;
    cart->read_chr = uxrom_read_chr;
    cart->write_chr = uxrom_write_chr;
    cart->mapper_state[0] = 0;
}