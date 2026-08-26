#include "mappers.h"

static uint8_t mmc2_read_prg(void *cart, uint16_t address) {
    Cartridge *c = (Cartridge*)cart;
    if (address >= 0x6000 && address <= 0x7FFF) {
        return c->prg_ram ? c->prg_ram[address - 0x6000] : 0;
    }
    uint32_t total_banks = c->prg_rom_size / 8192;
    if (total_banks == 0) return 0;
    uint32_t bank = 0;

    if (address >= 0x8000 && address <= 0x9FFF) {
        bank = c->mapper_state[0];
    } else if (address >= 0xA000 && address <= 0xBFFF) {
        bank = total_banks - 3;
    } else if (address >= 0xC000 && address <= 0xDFFF) {
        bank = total_banks - 2;
    } else {
        bank = total_banks - 1;
    }

    bank %= total_banks;
    return c->prg_rom[bank * 8192 + (address & 0x1FFF)];
}

static void mmc2_write_prg(void *cart, uint16_t address, uint8_t data) {
    Cartridge *c = (Cartridge*)cart;
    if (address >= 0x6000 && address <= 0x7FFF) {
        if (c->prg_ram) c->prg_ram[address - 0x6000] = data;
        return;
    }
    uint8_t reg = (address >> 12) & 0x07;
    switch (reg) {
        case 2: c->mapper_state[0] = data & 0x1F; break;
        case 3: c->mapper_state[1] = data & 0x1F; break;
        case 4: c->mapper_state[2] = data & 0x1F; break;
        case 5: c->mapper_state[3] = data & 0x1F; break;
        case 6: c->mapper_state[4] = data & 0x1F; break;
        case 7: c->mirroring = (data & 1) ? MIRROR_HORIZONTAL : MIRROR_VERTICAL; break;
    }
}

static uint8_t mmc2_read_chr(void *cart, uint16_t address) {
    Cartridge *c = (Cartridge*)cart;
    if (c->chr_rom_size == 0) return 0;

    uint8_t latch = (address < 0x1000) ? c->mapper_state[5] : c->mapper_state[6];
    uint32_t bank = (address < 0x1000) ? 
        ((latch == 0) ? c->mapper_state[1] : c->mapper_state[2]) :
        ((latch == 0) ? c->mapper_state[3] : c->mapper_state[4]);

    uint32_t total_banks = c->chr_rom_size / 4096;
    if (total_banks > 0) {
        bank %= total_banks;
    }
    uint32_t offset = bank * 4096 + (address & 0x0FFF);
    uint8_t data = c->chr_rom[offset % c->chr_rom_size];

    uint16_t addr = address & 0x1FFF;
    if (addr >= 0x0FD8 && addr <= 0x0FDF) {
        c->mapper_state[5] = 0;
    } else if (addr >= 0x0FE8 && addr <= 0x0FEF) {
        c->mapper_state[5] = 1;
    } else if (addr >= 0x1FD8 && addr <= 0x1FDF) {
        c->mapper_state[6] = 0;
    } else if (addr >= 0x1FE8 && addr <= 0x1FEF) {
        c->mapper_state[6] = 1;
    }

    return data;
}

static void mmc2_write_chr(void *cart, uint16_t address, uint8_t data) {
    Cartridge *c = (Cartridge*)cart;
    if (c->chr_rom_size == 0) return;
    uint32_t bank = 0;
    if (address < 0x1000) {
        uint8_t latch = c->mapper_state[5];
        bank = (latch == 0) ? c->mapper_state[1] : c->mapper_state[2];
    } else {
        uint8_t latch = c->mapper_state[6];
        bank = (latch == 0) ? c->mapper_state[3] : c->mapper_state[4];
    }
    uint32_t offset = bank * 4096 + (address & 0x0FFF);
    c->chr_rom[offset % c->chr_rom_size] = data;
}

void mapper_009_init(Cartridge *cart) {
    cart->read_prg = mmc2_read_prg;
    cart->write_prg = mmc2_write_prg;
    cart->read_chr = mmc2_read_chr;
    cart->write_chr = mmc2_write_chr;
    cart->mapper_state[0] = 0;
    cart->mapper_state[1] = 0;
    cart->mapper_state[2] = 0;
    cart->mapper_state[3] = 0;
    cart->mapper_state[4] = 0;
    cart->mapper_state[5] = 1;
    cart->mapper_state[6] = 1;
}