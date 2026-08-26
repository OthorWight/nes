#include "mappers.h"
#include "cpu6502.h"
#include <string.h>

static uint8_t m023_read_prg(void *cart, uint16_t address) {
    Cartridge *c = (Cartridge*)cart;
    if (address < 0x8000) {
        if (address >= 0x6000 && c->prg_ram) {
            return c->prg_ram[address - 0x6000];
        }
        return 0;
    }

    uint32_t total_banks = c->prg_rom_size / 8192;
    if (total_banks == 0) return 0;

    uint8_t prg_mode = (c->mapper_state[2] >> 1) & 0x01;
    uint32_t bank = 0;

    if (address >= 0x8000 && address <= 0x9FFF) {
        bank = (prg_mode == 0) ? c->mapper_state[0] : (total_banks - 2);
    } else if (address >= 0xA000 && address <= 0xBFFF) {
        bank = c->mapper_state[1];
    } else if (address >= 0xC000 && address <= 0xDFFF) {
        bank = (prg_mode == 0) ? (total_banks - 2) : c->mapper_state[0];
    } else {
        bank = total_banks - 1;
    }

    bank %= total_banks;
    return c->prg_rom[bank * 8192 + (address & 0x1FFF)];
}

static void m023_write_prg(void *cart, uint16_t address, uint8_t data) {
    Cartridge *c = (Cartridge*)cart;
    if (address < 0x8000) {
        if (address >= 0x6000 && c->prg_ram) {
            c->prg_ram[address - 0x6000] = data;
        }
        return;
    }

    CPU6502 *cpu = (CPU6502*)c->cpu_context;
    uint8_t reg_select = (address & 0x03) | ((address >> 2) & 0x03);

    if (address >= 0x8000 && address <= 0x8FFF) {
        c->mapper_state[0] = data & 0x1F;
    } else if (address >= 0x9000 && address <= 0x9FFF) {
        if (reg_select == 0) {
            uint8_t mirror = data & 0x03;
            if (mirror == 0) c->mirroring = MIRROR_VERTICAL;
            else if (mirror == 1) c->mirroring = MIRROR_HORIZONTAL;
            else if (mirror == 2) c->mirroring = MIRROR_ONE_SCREEN_LOW;
            else c->mirroring = MIRROR_ONE_SCREEN_HIGH;
        } else if (reg_select == 2) {
            c->mapper_state[2] = data;
        }
    } else if (address >= 0xA000 && address <= 0xAFFF) {
        c->mapper_state[1] = data & 0x1F;
    } else if (address >= 0xB000 && address <= 0xEFFF) {
        uint8_t slot_base = ((address - 0xB000) / 0x1000) * 2;
        if (reg_select == 0) {
            c->mapper_state[3 + slot_base * 2] = data & 0x0F;
        } else if (reg_select == 1) {
            c->mapper_state[3 + slot_base * 2 + 1] = data & 0x1F;
        } else if (reg_select == 2) {
            c->mapper_state[3 + (slot_base + 1) * 2] = data & 0x0F;
        } else if (reg_select == 3) {
            c->mapper_state[3 + (slot_base + 1) * 2 + 1] = data & 0x1F;
        }
    } else if (address >= 0xF000) {
        if (reg_select == 0) {
            c->mapper_state[19] = (c->mapper_state[19] & 0xF0) | (data & 0x0F);
        } else if (reg_select == 1) {
            c->mapper_state[19] = (c->mapper_state[19] & 0x0F) | ((data & 0x0F) << 4);
        } else if (reg_select == 2) {
            c->mapper_state[20] = data;
            if (data & 0x02) {
                c->mapper_state[21] = c->mapper_state[19];
                c->mapper_state[22] = 114;
            }
            c->mapper_state[23] = 0;
            if (cpu) {
                cpu_set_irq_line(cpu, 0, false);
            }
        } else if (reg_select == 3) {
            if (cpu) {
                cpu_set_irq_line(cpu, 0, false);
            }
            c->mapper_state[23] = 0;
            if (c->mapper_state[20] & 0x08) {
                c->mapper_state[20] |= 0x02;
            } else {
                c->mapper_state[20] &= ~0x02;
            }
        }
    }
}

static uint8_t m023_read_chr(void *cart, uint16_t address) {
    Cartridge *c = (Cartridge*)cart;
    if (c->chr_rom_size == 0) return 0;
    uint32_t total_banks = c->chr_rom_size / 1024;
    if (total_banks == 0) return 0;

    uint8_t slot = address / 1024;
    uint16_t low = c->mapper_state[3 + slot * 2];
    uint16_t high = c->mapper_state[3 + slot * 2 + 1];
    uint32_t bank = (high << 4) | (low & 0x0F);

    bank %= total_banks;
    return c->chr_rom[bank * 1024 + (address & 0x03FF)];
}

static void m023_write_chr(void *cart, uint16_t address, uint8_t data) {
    Cartridge *c = (Cartridge*)cart;
    if (c->chr_rom_size == 0) return;
    uint32_t total_banks = c->chr_rom_size / 1024;
    if (total_banks == 0) return;

    uint8_t slot = address / 1024;
    uint16_t low = c->mapper_state[3 + slot * 2];
    uint16_t high = c->mapper_state[3 + slot * 2 + 1];
    uint32_t bank = (high << 4) | (low & 0x0F);

    bank %= total_banks;
    c->chr_rom[bank * 1024 + (address & 0x03FF)] = data;
}

static void m023_clock_irq(void *cart, void *cpu) {
    Cartridge *c = (Cartridge*)cart;
    CPU6502 *cpu_ptr = (CPU6502*)cpu;
    if (cpu_ptr) {
        c->cpu_context = cpu_ptr;
    }

    uint8_t ctrl = c->mapper_state[20];
    if (!(ctrl & 0x02)) return;

    bool count_tick = false;
    if (ctrl & 0x04) {
        count_tick = true;
    } else {
        if (c->mapper_state[22] == 0) {
            c->mapper_state[22] = 114;
            count_tick = true;
        } else {
            c->mapper_state[22]--;
        }
    }

    if (count_tick) {
        if (c->mapper_state[21] == 0xFF) {
            c->mapper_state[21] = c->mapper_state[19];
            c->mapper_state[23] = 1;
            if (cpu_ptr) {
                cpu_set_irq_line(cpu_ptr, 0, true);
            }
        } else {
            c->mapper_state[21]++;
        }
    }
}

static void m023_reset_irq(void *cart) {
    Cartridge *c = (Cartridge*)cart;
    CPU6502 *cpu = (CPU6502*)c->cpu_context;
    if (cpu) {
        cpu_set_irq_line(cpu, 0, false);
    }
}

void mapper_023_init(Cartridge *cart) {
    cart->read_prg = m023_read_prg;
    cart->write_prg = m023_write_prg;
    cart->read_chr = m023_read_chr;
    cart->write_chr = m023_write_chr;
    cart->clock_irq = m023_clock_irq;
    cart->reset_irq = m023_reset_irq;
    cart->cpu_clocked_irq = true;

    memset(cart->mapper_state, 0, sizeof(cart->mapper_state));
    cart->mapper_state[0] = 0;
    cart->mapper_state[1] = 1;
    cart->mapper_state[22] = 114;
}