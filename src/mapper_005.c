#include "mappers.h"
#include "cpu6502.h"

#define MMC5_IRQ_SOURCE 0

static bool mmc5_ram_write_enabled(Cartridge *c) {
    return (c->mmc5_ram_protect[0] == 0x02 && c->mmc5_ram_protect[1] == 0x01);
}

static uint8_t mmc5_read_prg(void *cart, uint16_t address) {
    Cartridge *c = (Cartridge*)cart;

    if (address >= 0x5000 && address <= 0x5206) {
        if (address == 0x5204) {
            uint8_t status = 0;
            if (c->mmc5_irq_pending) status |= 0x80;
            if (c->mmc5_in_frame)    status |= 0x40;
            c->mmc5_irq_pending = false;
            CPU6502 *cpu = (CPU6502*)c->cpu_context;
            if (cpu) {
                cpu_set_irq_line(cpu, MMC5_IRQ_SOURCE, false);
            }
            return status;
        }
        if (address == 0x5205) {
            return (uint8_t)((c->mmc5_mult_a * c->mmc5_mult_b) & 0xFF);
        }
        if (address == 0x5206) {
            return (uint8_t)(((c->mmc5_mult_a * c->mmc5_mult_b) >> 8) & 0xFF);
        }
        return 0;
    }

    if (address >= 0x5C00 && address <= 0x5FFF) {
        return c->exram[address - 0x5C00];
    }

    if (address >= 0x6000 && address <= 0x7FFF) {
        if (c->prg_ram && c->prg_ram_size > 0) {
            uint32_t bank = (c->mmc5_prg_regs[0] & 0x07);
            uint32_t offset = (bank * 8192) + (address - 0x6000);
            return c->prg_ram[offset % c->prg_ram_size];
        }
        return 0;
    }

    if (address >= 0x8000) {
        uint32_t total_8k_ram = c->prg_ram_size / 8192;
        uint8_t mode = c->mmc5_prg_mode;

        switch (mode) {
            case 0: {
                uint8_t reg = c->mmc5_prg_regs[4] & 0x7C;
                uint32_t offset = ((reg >> 2) * 32768) + (address - 0x8000);
                return c->prg_rom[offset % c->prg_rom_size];
            }
            case 1: {
                if (address < 0xC000) {
                    uint8_t reg = c->mmc5_prg_regs[2];
                    if (!(reg & 0x80) && total_8k_ram > 0) {
                        uint32_t offset = ((reg & 0x06) * 8192) + (address - 0x8000);
                        return c->prg_ram[offset % c->prg_ram_size];
                    }
                    uint32_t offset = (((reg & 0x7E) >> 1) * 16384) + (address - 0x8000);
                    return c->prg_rom[offset % c->prg_rom_size];
                } else {
                    uint8_t reg = c->mmc5_prg_regs[4];
                    uint32_t offset = (((reg & 0x7E) >> 1) * 16384) + (address - 0xC000);
                    return c->prg_rom[offset % c->prg_rom_size];
                }
            }
            case 2: {
                if (address < 0xC000) {
                    uint8_t reg = c->mmc5_prg_regs[2];
                    if (!(reg & 0x80) && total_8k_ram > 0) {
                        uint32_t offset = ((reg & 0x06) * 8192) + (address - 0x8000);
                        return c->prg_ram[offset % c->prg_ram_size];
                    }
                    uint32_t offset = (((reg & 0x7E) >> 1) * 16384) + (address - 0x8000);
                    return c->prg_rom[offset % c->prg_rom_size];
                } else if (address < 0xE000) {
                    uint8_t reg = c->mmc5_prg_regs[3];
                    if (!(reg & 0x80) && total_8k_ram > 0) {
                        uint32_t offset = ((reg & 0x07) * 8192) + (address - 0xC000);
                        return c->prg_ram[offset % c->prg_ram_size];
                    }
                    uint32_t offset = ((reg & 0x7F) * 8192) + (address - 0xC000);
                    return c->prg_rom[offset % c->prg_rom_size];
                } else {
                    uint8_t reg = c->mmc5_prg_regs[4];
                    uint32_t offset = ((reg & 0x7F) * 8192) + (address - 0xE000);
                    return c->prg_rom[offset % c->prg_rom_size];
                }
            }
            case 3: {
                int slot = (address - 0x8000) / 8192;
                uint8_t reg = c->mmc5_prg_regs[slot + 1];
                if (slot < 3 && !(reg & 0x80) && total_8k_ram > 0) {
                    uint32_t offset = ((reg & 0x07) * 8192) + (address & 0x1FFF);
                    return c->prg_ram[offset % c->prg_ram_size];
                }
                uint32_t offset = ((reg & 0x7F) * 8192) + (address & 0x1FFF);
                return c->prg_rom[offset % c->prg_rom_size];
            }
        }
    }

    return 0;
}

static void mmc5_write_prg(void *cart, uint16_t address, uint8_t data) {
    Cartridge *c = (Cartridge*)cart;
    CPU6502 *cpu = (CPU6502*)c->cpu_context;

    if (address >= 0x5100 && address <= 0x5206) {
        switch (address) {
            case 0x5100: c->mmc5_prg_mode = data & 0x03; break;
            case 0x5101: c->mmc5_chr_mode = data & 0x03; break;
            case 0x5102: c->mmc5_ram_protect[0] = data & 0x03; break;
            case 0x5103: c->mmc5_ram_protect[1] = data & 0x03; break;
            case 0x5104: c->mmc5_exram_mode = data & 0x03; break;
            case 0x5105: c->mmc5_nametable_ctrl = data; break;
            case 0x5106: c->mmc5_fill_tile = data; break;
            case 0x5107: c->mmc5_fill_attr = (data & 0x03) | ((data & 0x03) << 2) | ((data & 0x03) << 4) | ((data & 0x03) << 6); break;

            case 0x5113: c->mmc5_prg_regs[0] = data; break;
            case 0x5114: c->mmc5_prg_regs[1] = data; break;
            case 0x5115: c->mmc5_prg_regs[2] = data; break;
            case 0x5116: c->mmc5_prg_regs[3] = data; break;
            case 0x5117: c->mmc5_prg_regs[4] = data | 0x80; break;

            case 0x5120: case 0x5121: case 0x5122: case 0x5123:
            case 0x5124: case 0x5125: case 0x5126: case 0x5127:
                c->mmc5_chr_regs_a[address - 0x5120] = (uint16_t)data | ((uint16_t)(c->mmc5_chr_high & 0x03) << 8);
                break;

            case 0x5128: case 0x5129: case 0x512A: case 0x512B:
                c->mmc5_chr_regs_b[address - 0x5128] = (uint16_t)data | ((uint16_t)(c->mmc5_chr_high & 0x03) << 8);
                break;

            case 0x5130: c->mmc5_chr_high = data & 0x03; break;

            case 0x5203: c->mmc5_irq_target = data; break;
            case 0x5204:
                c->mmc5_irq_enabled = (data & 0x80) != 0;
                if (!c->mmc5_irq_enabled && cpu) {
                    cpu_set_irq_line(cpu, MMC5_IRQ_SOURCE, false);
                }
                break;
            case 0x5205: c->mmc5_mult_a = data; break;
            case 0x5206: c->mmc5_mult_b = data; break;
        }
        return;
    }

    if (address >= 0x5C00 && address <= 0x5FFF) {
        if (c->mmc5_exram_mode != 3) {
            c->exram[address - 0x5C00] = data;
        }
        return;
    }

    if (address >= 0x6000 && address <= 0x7FFF) {
        if (c->prg_ram && mmc5_ram_write_enabled(c) && c->prg_ram_size > 0) {
            uint32_t bank = (c->mmc5_prg_regs[0] & 0x07);
            uint32_t offset = (bank * 8192) + (address - 0x6000);
            c->prg_ram[offset % c->prg_ram_size] = data;
        }
        return;
    }

    if (address >= 0x8000 && address <= 0xDFFF && mmc5_ram_write_enabled(c)) {
        int slot = (address - 0x8000) / 8192;
        if (slot < 3 && !(c->mmc5_prg_regs[slot + 1] & 0x80) && c->prg_ram_size > 0) {
            uint32_t bank = c->mmc5_prg_regs[slot + 1] & 0x07;
            uint32_t offset = (bank * 8192) + (address & 0x1FFF);
            c->prg_ram[offset % c->prg_ram_size] = data;
        }
    }
}

static uint8_t mmc5_read_chr(void *cart, uint16_t address) {
    Cartridge *c = (Cartridge*)cart;
    if (c->chr_rom_size == 0) return 0;

    bool use_set_a = !c->ppu_sprite_size_8x16 || c->ppu_sprite_fetch;
    uint32_t bank = 0;
    uint16_t sub_addr = 0;
    uint32_t base_size = 1024;
    uint8_t mode = c->mmc5_chr_mode;

    if (use_set_a) {
        switch (mode) {
            case 0:
                bank = c->mmc5_chr_regs_a[7];
                sub_addr = address & 0x1FFF;
                base_size = 8192;
                break;
            case 1:
                bank = c->mmc5_chr_regs_a[(address / 4096) * 4 + 3];
                sub_addr = address & 0x0FFF;
                base_size = 4096;
                break;
            case 2:
                bank = c->mmc5_chr_regs_a[(address / 2048) * 2 + 1];
                sub_addr = address & 0x07FF;
                base_size = 2048;
                break;
            case 3:
                bank = c->mmc5_chr_regs_a[address / 1024];
                sub_addr = address & 0x03FF;
                base_size = 1024;
                break;
        }
    } else {
        switch (mode) {
            case 0:
                bank = c->mmc5_chr_regs_b[3];
                sub_addr = address & 0x1FFF;
                base_size = 8192;
                break;
            case 1:
                bank = c->mmc5_chr_regs_b[3];
                sub_addr = address & 0x0FFF;
                base_size = 4096;
                break;
            case 2:
                bank = c->mmc5_chr_regs_b[((address / 2048) & 1) ? 3 : 1];
                sub_addr = address & 0x07FF;
                base_size = 2048;
                break;
            case 3:
                bank = c->mmc5_chr_regs_b[(address / 1024) % 4];
                sub_addr = address & 0x03FF;
                base_size = 1024;
                break;
        }
    }

    uint32_t total_banks = c->chr_rom_size / base_size;
    if (total_banks > 0) {
        bank %= total_banks;
    }
    return c->chr_rom[(bank * base_size + sub_addr) % c->chr_rom_size];
}

static void mmc5_write_chr(void *cart, uint16_t address, uint8_t data) {
    Cartridge *c = (Cartridge*)cart;
    if (c->chr_rom_size == 0) return;
    c->chr_rom[address % c->chr_rom_size] = data;
}

static void mmc5_clock_irq(void *cart, void *cpu) {
    Cartridge *c = (Cartridge*)cart;
    CPU6502 *cpu_ptr = (CPU6502*)cpu;
    if (cpu_ptr) {
        c->cpu_context = cpu_ptr;
    }

    c->mmc5_scanline++;
    c->mmc5_in_frame = true;

    if (c->mmc5_scanline == c->mmc5_irq_target) {
        c->mmc5_irq_pending = true;
        if (c->mmc5_irq_enabled && cpu_ptr) {
            cpu_set_irq_line(cpu_ptr, MMC5_IRQ_SOURCE, true);
        }
    }
}

static void mmc5_reset_irq(void *cart) {
    Cartridge *c = (Cartridge*)cart;
    c->mmc5_scanline = 0;
    c->mmc5_in_frame = false;
    c->mmc5_irq_pending = false;
    CPU6502 *cpu = (CPU6502*)c->cpu_context;
    if (cpu) {
        cpu_set_irq_line(cpu, MMC5_IRQ_SOURCE, false);
    }
}

void mapper_005_init(Cartridge *cart) {
    cart->read_prg = mmc5_read_prg;
    cart->write_prg = mmc5_write_prg;
    cart->read_chr = mmc5_read_chr;
    cart->write_chr = mmc5_write_chr;
    cart->clock_irq = mmc5_clock_irq;
    cart->reset_irq = mmc5_reset_irq;

    cart->mmc5_prg_mode = 3;
    cart->mmc5_chr_mode = 0;
    cart->mmc5_ram_protect[0] = 0x02;
    cart->mmc5_ram_protect[1] = 0x01;
    cart->mmc5_nametable_ctrl = 0x00;
    cart->mmc5_prg_regs[0] = 0x00;
    cart->mmc5_prg_regs[1] = 0xFF;
    cart->mmc5_prg_regs[2] = 0xFF;
    cart->mmc5_prg_regs[3] = 0xFF;
    cart->mmc5_prg_regs[4] = 0xFF;
}