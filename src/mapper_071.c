#include "mappers.h"
#include <string.h>

static uint8_t m071_read_prg(void *cart, uint16_t address) {
    Cartridge *c = (Cartridge*)cart;
    uint32_t total_banks = c->prg_rom_size / 16384;
    if (total_banks == 0) return 0;

    if (address >= 0x8000 && address <= 0xBFFF) {
        uint32_t bank = c->mapper_state[0] % total_banks;
        return c->prg_rom[bank * 16384 + (address - 0x8000)];
    }
    if (address >= 0xC000) {
        uint32_t bank = total_banks - 1;
        return c->prg_rom[bank * 16384 + (address - 0xC000)];
    }
    return 0;
}

static void m071_write_prg(void *cart, uint16_t address, uint8_t data) {
    Cartridge *c = (Cartridge*)cart;
    if (address >= 0x9000 && address <= 0x9FFF) {
        if (data & 0x10) {
            c->mirroring = (data & 0x08) ? MIRROR_ONE_SCREEN_HIGH : MIRROR_ONE_SCREEN_LOW;
        }
    } else if (address >= 0xC000) {
        c->mapper_state[0] = data & 0x0F;
    }
}

static uint8_t m071_read_chr(void *cart, uint16_t address) {
    Cartridge *c = (Cartridge*)cart;
    if (c->chr_rom_size > 0) {
        return c->chr_rom[address & 0x1FFF];
    }
    return 0;
}

static void m071_write_chr(void *cart, uint16_t address, uint8_t data) {
    Cartridge *c = (Cartridge*)cart;
    if (c->chr_rom_size > 0) {
        c->chr_rom[address & 0x1FFF] = data;
    }
}

void mapper_071_init(Cartridge *cart) {
    cart->read_prg = m071_read_prg;
    cart->write_prg = m071_write_prg;
    cart->read_chr = m071_read_chr;
    cart->write_chr = m071_write_chr;
    memset(cart->mapper_state, 0, sizeof(cart->mapper_state));
}