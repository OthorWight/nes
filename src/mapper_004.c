#include "mappers.h"
#include "cpu6502.h"

static uint8_t mmc3_read_prg(void *cart, uint16_t address) {
    Cartridge *c = (Cartridge*)cart;
    if (address >= 0x6000 && address <= 0x7FFF) {
        return c->prg_ram ? c->prg_ram[address - 0x6000] : 0;
    }

    uint32_t total_banks = c->prg_rom_size / 8192;
    if (total_banks == 0) return 0;

    uint8_t bank_select = c->mapper_state[8];
    uint8_t prg_mode = (bank_select >> 6) & 0x01;
    uint32_t bank = 0;

    if (address >= 0x8000 && address <= 0x9FFF) {
        bank = (prg_mode == 0) ? c->mapper_state[6] : (total_banks - 2);
    } else if (address >= 0xA000 && address <= 0xBFFF) {
        bank = c->mapper_state[7];
    } else if (address >= 0xC000 && address <= 0xDFFF) {
        bank = (prg_mode == 0) ? (total_banks - 2) : c->mapper_state[6];
    } else {
        bank = total_banks - 1;
    }

    bank %= total_banks;
    uint32_t offset = bank * 8192 + (address & 0x1FFF);
    return c->prg_rom[offset];
}

static void mmc3_write_prg(void *cart, uint16_t address, uint8_t data) {
    Cartridge *c = (Cartridge*)cart;
    if (address >= 0x6000 && address <= 0x7FFF) {
        if (c->prg_ram) c->prg_ram[address - 0x6000] = data;
        return;
    }

    CPU6502 *cpu = (CPU6502*)c->cpu_context;

    if (address >= 0x8000 && address <= 0x9FFF) {
        if ((address & 1) == 0) {
            c->mapper_state[8] = data;
        } else {
            uint8_t target_reg = c->mapper_state[8] & 0x07;
            c->mapper_state[target_reg] = data;
        }
    } else if (address >= 0xA000 && address <= 0xBFFF) {
        if ((address & 1) == 0) {
            if (c->mirroring != MIRROR_FOUR_SCREEN) {
                c->mirroring = (data & 1) ? MIRROR_HORIZONTAL : MIRROR_VERTICAL;
            }
        }
    } else if (address >= 0xC000 && address <= 0xDFFF) {
        if ((address & 1) == 0) {
            c->mapper_state[9] = data;
        } else {
            c->mapper_state[12] = 1;
        }
    } else {
        if ((address & 1) == 0) {
            c->mapper_state[11] = 0;
            if (cpu) {
                cpu_set_irq_line(cpu, 0, false);
            }
        } else {
            c->mapper_state[11] = 1;
        }
    }
}

static uint8_t mmc3_read_chr(void *cart, uint16_t address) {
    Cartridge *c = (Cartridge*)cart;
    if (c->chr_rom_size == 0) return 0;

    uint8_t bank_select = c->mapper_state[8];
    uint8_t chr_mode = (bank_select >> 7) & 0x01;
    uint32_t bank = 0;
    uint16_t sub_addr = address & 0x03FF;

    if (chr_mode == 0) {
        if (address < 0x0400) bank = c->mapper_state[0] & 0xFE;
        else if (address < 0x0800) bank = c->mapper_state[0] | 0x01;
        else if (address < 0x0C00) bank = c->mapper_state[1] & 0xFE;
        else if (address < 0x1000) bank = c->mapper_state[1] | 0x01;
        else if (address < 0x1400) bank = c->mapper_state[2];
        else if (address < 0x1800) bank = c->mapper_state[3];
        else if (address < 0x1C00) bank = c->mapper_state[4];
        else bank = c->mapper_state[5];
    } else {
        if (address < 0x0400) bank = c->mapper_state[2];
        else if (address < 0x0800) bank = c->mapper_state[3];
        else if (address < 0x0C00) bank = c->mapper_state[4];
        else if (address < 0x1000) bank = c->mapper_state[5];
        else if (address < 0x1400) bank = c->mapper_state[0] & 0xFE;
        else if (address < 0x1800) bank = c->mapper_state[0] | 0x01;
        else if (address < 0x1C00) bank = c->mapper_state[1] & 0xFE;
        else bank = c->mapper_state[1] | 0x01;
    }

    uint32_t total_banks = c->chr_rom_size / 1024;
    bank %= total_banks;
    return c->chr_rom[bank * 1024 + sub_addr];
}

static void mmc3_write_chr(void *cart, uint16_t address, uint8_t data) {
    Cartridge *c = (Cartridge*)cart;
    if (c->chr_rom_size == 0) return;

    //uint32_t total_banks = c->chr_rom_size / 1024;
    uint32_t offset = (address & 0x03FF) + (address / 1024) * 1024;
    c->chr_rom[offset % c->chr_rom_size] = data;
}

static void mmc3_clock_irq(void *cart, void *cpu) {
    Cartridge *c = (Cartridge*)cart;
    CPU6502 *cpu_ptr = (CPU6502*)cpu;
    if (cpu_ptr) {
        c->cpu_context = cpu_ptr;
    }

    if (c->mapper_state[10] == 0 || c->mapper_state[12] != 0) {
        c->mapper_state[10] = c->mapper_state[9];
        c->mapper_state[12] = 0;

        if (c->mapper_state[10] == 0 && c->mapper_state[11] != 0) {
            if (cpu_ptr) {
                cpu_set_irq_line(cpu_ptr, 0, true);
            }
        }
    } else {
        c->mapper_state[10]--;
        if (c->mapper_state[10] == 0 && c->mapper_state[11] != 0) {
            if (cpu_ptr) {
                cpu_set_irq_line(cpu_ptr, 0, true);
            }
        }
    }
}

static void mmc3_reset_irq(void *cart) {
    Cartridge *c = (Cartridge*)cart;
    CPU6502 *cpu = (CPU6502*)c->cpu_context;
    if (cpu) {
        cpu_set_irq_line(cpu, 0, false);
    }
}

void mapper_004_init(Cartridge *cart) {
    cart->read_prg = mmc3_read_prg;
    cart->write_prg = mmc3_write_prg;
    cart->read_chr = mmc3_read_chr;
    cart->write_chr = mmc3_write_chr;
    cart->clock_irq = mmc3_clock_irq;
    cart->reset_irq = mmc3_reset_irq;
    cart->mapper_state[0] = 0;
    cart->mapper_state[1] = 0;
    cart->mapper_state[6] = 0;
    cart->mapper_state[7] = 0;
    cart->mapper_state[8] = 0;
}