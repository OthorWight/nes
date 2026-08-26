#include "mappers.h"

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

static uint8_t axrom_read_chr(void *cart, uint16_t address) {
    Cartridge *c = (Cartridge*)cart;
    if (c->chr_rom_size > 0) {
        return c->chr_rom[address & 0x1FFF];
    }
    return 0;
}

static void axrom_write_chr(void *cart, uint16_t address, uint8_t data) {
    Cartridge *c = (Cartridge*)cart;
    if (c->chr_rom_size > 0) {
        c->chr_rom[address & 0x1FFF] = data;
    }
}

void mapper_007_init(Cartridge *cart) {
    cart->read_prg = axrom_read_prg;
    cart->write_prg = axrom_write_prg;
    cart->read_chr = axrom_read_chr;
    cart->write_chr = axrom_write_chr;
    cart->mapper_state[0] = 0;
}