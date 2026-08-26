#include "mappers.h"
#include "cpu6502.h"

static uint8_t mmc1_read_prg(void *cart, uint16_t address) {
    Cartridge *c = (Cartridge*)cart;
    if (address >= 0x6000 && address <= 0x7FFF) {
        return c->prg_ram ? c->prg_ram[address - 0x6000] : 0;
    }

    uint8_t control = c->mapper_state[2];
    uint8_t prg_bank = c->mapper_state[5] & 0x0F;
    uint8_t prg_mode = (control >> 2) & 0x03;
    uint32_t total_banks = c->prg_rom_size / 16384;
    if (total_banks == 0) return 0;

    uint32_t offset = 0;
    if (prg_mode == 0 || prg_mode == 1) {
        uint32_t bank = (prg_bank & 0xFE) % total_banks;
        offset = bank * 16384 + (address - 0x8000);
    } else if (prg_mode == 2) {
        if (address < 0xC000) {
            offset = address - 0x8000;
        } else {
            uint32_t bank = prg_bank % total_banks;
            offset = bank * 16384 + (address - 0xC000);
        }
    } else {
        if (address < 0xC000) {
            uint32_t bank = prg_bank % total_banks;
            offset = bank * 16384 + (address - 0x8000);
        } else {
            offset = (c->prg_rom_size - 16384) + (address - 0xC000);
        }
    }

    return c->prg_rom[offset % c->prg_rom_size];
}

static void mmc1_write_prg(void *cart, uint16_t address, uint8_t data) {
    Cartridge *c = (Cartridge*)cart;
    if (address >= 0x6000 && address <= 0x7FFF) {
        if (c->prg_ram) c->prg_ram[address - 0x6000] = data;
        return;
    }

    CPU6502 *cpu = (CPU6502*)c->cpu_context;
    if (cpu) {
        if (cpu->cycle_count - c->last_write_cycle < 2) {
            return;
        }
        c->last_write_cycle = cpu->cycle_count;
    }

    if (data & 0x80) {
        c->mapper_state[0] = 0;
        c->mapper_state[1] = 0;
        c->mapper_state[2] |= 0x0C;
        uint8_t control = c->mapper_state[2] & 0x03;
        if (control == 0) c->mirroring = MIRROR_ONE_SCREEN_LOW;
        else if (control == 1) c->mirroring = MIRROR_ONE_SCREEN_HIGH;
        else if (control == 2) c->mirroring = MIRROR_VERTICAL;
        else c->mirroring = MIRROR_HORIZONTAL;
    } else {
        uint8_t val = data & 0x01;
        c->mapper_state[0] |= (val << c->mapper_state[1]);
        c->mapper_state[1]++;
        if (c->mapper_state[1] == 5) {
            uint8_t reg_val = c->mapper_state[0] & 0x1F;
            uint16_t target_reg = (address >> 13) & 0x03;
            c->mapper_state[2 + target_reg] = reg_val;
            c->mapper_state[0] = 0;
            c->mapper_state[1] = 0;
            if (target_reg == 0) {
                uint8_t control = reg_val & 0x03;
                if (control == 0) c->mirroring = MIRROR_ONE_SCREEN_LOW;
                else if (control == 1) c->mirroring = MIRROR_ONE_SCREEN_HIGH;
                else if (control == 2) c->mirroring = MIRROR_VERTICAL;
                else c->mirroring = MIRROR_HORIZONTAL;
            }
        }
    }
}

static uint8_t mmc1_read_chr(void *cart, uint16_t address) {
    Cartridge *c = (Cartridge*)cart;
    if (c->chr_rom_size == 0) return 0;
    uint8_t control = c->mapper_state[2];
    uint8_t chr_mode = (control >> 4) & 0x01;
    uint8_t bank_0 = c->mapper_state[3];
    uint8_t bank_1 = c->mapper_state[4];

    uint32_t offset = 0;
    if (chr_mode == 0) {
        uint8_t bank = bank_0 & 0xFE;
        offset = (uint32_t)bank * 4096 + address;
    } else {
        if (address < 0x1000) {
            offset = (uint32_t)bank_0 * 4096 + address;
        } else {
            offset = (uint32_t)bank_1 * 4096 + (address - 0x1000);
        }
    }

    return c->chr_rom[offset % c->chr_rom_size];
}

static void mmc1_write_chr(void *cart, uint16_t address, uint8_t data) {
    Cartridge *c = (Cartridge*)cart;
    if (c->chr_rom_size == 0) return;
    uint8_t control = c->mapper_state[2];
    uint8_t chr_mode = (control >> 4) & 0x01;
    uint8_t bank_0 = c->mapper_state[3];
    uint8_t bank_1 = c->mapper_state[4];

    uint32_t offset = 0;
    if (chr_mode == 0) {
        uint8_t bank = bank_0 & 0xFE;
        offset = (uint32_t)bank * 4096 + address;
    } else {
        if (address < 0x1000) {
            offset = (uint32_t)bank_0 * 4096 + address;
        } else {
            offset = (uint32_t)bank_1 * 4096 + (address - 0x1000);
        }
    }

    c->chr_rom[offset % c->chr_rom_size] = data;
}

void mapper_001_init(Cartridge *cart) {
    cart->read_prg = mmc1_read_prg;
    cart->write_prg = mmc1_write_prg;
    cart->read_chr = mmc1_read_chr;
    cart->write_chr = mmc1_write_chr;
    cart->mapper_state[0] = 0;
    cart->mapper_state[1] = 0;
    cart->mapper_state[2] = 0x0C;
    cart->mapper_state[3] = 0;
    cart->mapper_state[4] = 0;
    cart->mapper_state[5] = 0;
}