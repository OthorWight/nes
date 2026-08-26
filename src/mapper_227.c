#include "mappers.h"
#include <string.h>

static void m227_update_banks(Cartridge *c, uint16_t addr) {
    uint32_t bank = ((addr >> 2) & 0x1F) | ((addr & 0x0100) >> 3);

    bool s_flag   = (addr & 0x0001) != 0;
    bool prg_mode = (addr & 0x0080) != 0;
    bool l_flag   = (addr & 0x0200) != 0;

    uint32_t bank0 = 0;
    uint32_t bank1 = 0;

    if (prg_mode) {
        if (s_flag) {
            bank0 = bank;
            bank1 = l_flag ? ((bank & 0x38) | 7) : (bank & 0x38);
        } else {
            bank0 = bank;
            bank1 = bank;
        }
    } else {
        if (s_flag) {
            bank0 = bank & ~1;
            bank1 = (bank & ~1) | 1;
        } else {
            bank0 = bank;
            bank1 = l_flag ? ((bank & 0x38) | 7) : (bank & 0x38);
        }
    }

    uint32_t total_banks = c->prg_rom_size / 16384;
    if (total_banks > 0) {
        bank0 %= total_banks;
        bank1 %= total_banks;
    }

    c->mapper_state[2] = (uint8_t)bank0;
    c->mapper_state[3] = (uint8_t)bank1;
    c->mirroring = (addr & 0x0002) ? MIRROR_HORIZONTAL : MIRROR_VERTICAL;
}

static uint8_t m227_read_prg(void *cart, uint16_t address) {
    Cartridge *c = (Cartridge*)cart;
    if (address >= 0x6000 && address <= 0x7FFF) {
        return c->prg_ram ? c->prg_ram[address - 0x6000] : 0;
    }
    if (address >= 0x8000 && address <= 0xBFFF) {
        uint32_t bank = c->mapper_state[2];
        return c->prg_rom[(bank * 16384 + (address & 0x3FFF)) % c->prg_rom_size];
    }
    if (address >= 0xC000) {
        uint32_t bank = c->mapper_state[3];
        return c->prg_rom[(bank * 16384 + (address & 0x3FFF)) % c->prg_rom_size];
    }
    return 0;
}

static void m227_write_prg(void *cart, uint16_t address, uint8_t data) {
    (void)data;
    Cartridge *c = (Cartridge*)cart;
    if (address >= 0x6000 && address <= 0x7FFF) {
        if (c->prg_ram) c->prg_ram[address - 0x6000] = data;
        return;
    }
    if (address >= 0x8000) {
        m227_update_banks(c, address);
    }
}

static uint8_t m227_read_chr(void *cart, uint16_t address) {
    Cartridge *c = (Cartridge*)cart;
    if (c->chr_rom_size > 0) {
        return c->chr_rom[address & 0x1FFF];
    }
    return 0;
}

static void m227_write_chr(void *cart, uint16_t address, uint8_t data) {
    Cartridge *c = (Cartridge*)cart;
    if (c->chr_rom_size > 0) {
        c->chr_rom[address & 0x1FFF] = data;
    }
}

void mapper_227_init(Cartridge *cart) {
    cart->read_prg = m227_read_prg;
    cart->write_prg = m227_write_prg;
    cart->read_chr = m227_read_chr;
    cart->write_chr = m227_write_chr;
    memset(cart->mapper_state, 0, sizeof(cart->mapper_state));
    m227_update_banks(cart, 0x0000);
}