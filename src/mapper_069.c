#include "mappers.h"
#include "cartridge.h"
#include "nes_system.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t  cmd;
    uint8_t  chr_banks[8];
    uint8_t  prg_banks[4]; // 0: $6000, 1: $8000, 2: $A000, 3: $C000
    uint8_t  irq_ctrl;
    uint16_t irq_counter;
} FME7Data;

static void fme7_reset(Cartridge *c) {
    FME7Data *d = (FME7Data*)c->mapper_data;
    memset(d, 0, sizeof(FME7Data));

    uint32_t total_8k = c->prg_rom_size / 8192;
    d->prg_banks[0] = 0x00;
    d->prg_banks[1] = 0;
    d->prg_banks[2] = 1;
    d->prg_banks[3] = (total_8k >= 2) ? (total_8k - 2) : 0;

    c->mirroring = MIRROR_VERTICAL;
    c->nes->lines.irq_line = false;
}

static void fme7_destroy(Cartridge *c) {
    free(c->mapper_data);
    c->mapper_data = NULL;
}

static void fme7_clock_m2(Cartridge *c) {
    FME7Data *d = (FME7Data*)c->mapper_data;

    if (d->irq_ctrl & 0x80) {
        uint16_t prev = d->irq_counter;
        d->irq_counter--;

        if (prev == 0 && d->irq_counter == 0xFFFF) {
            if (d->irq_ctrl & 0x01) {
                c->nes->lines.irq_line = true;
            }
        }
    }
}

static uint8_t fme7_cpu_read(Cartridge *c, uint16_t addr, bool *handled) {
    FME7Data *d = (FME7Data*)c->mapper_data;
    uint32_t total_8k = c->prg_rom_size / 8192;
    if (total_8k == 0) return 0;

    if (addr >= 0x6000 && addr <= 0x7FFF) {
        *handled = true;
        uint8_t reg = d->prg_banks[0];
        
        if (!(reg & 0x80)) {
            // Bit 7 is 0: Map ROM into $6000-$7FFF
            uint32_t bank = (reg & 0x3F) % total_8k;
            return c->prg_rom[bank * 8192 + (addr - 0x6000)];
        } else {
            // Bit 7 is 1: Map RAM into $6000-$7FFF (Bit 6 is ignored for reads)
            if (c->prg_ram && c->prg_ram_size > 0) {
                return c->prg_ram[(addr - 0x6000) % c->prg_ram_size];
            }
            return 0;
        }
    }

    if (addr >= 0x8000) {
        *handled = true;
        uint32_t bank = 0;
        if (addr <= 0x9FFF) {
            bank = d->prg_banks[1] & 0x3F;
        } else if (addr <= 0xBFFF) {
            bank = d->prg_banks[2] & 0x3F;
        } else if (addr <= 0xDFFF) {
            bank = d->prg_banks[3] & 0x3F;
        } else {
            bank = total_8k - 1;
        }

        uint32_t offset = (bank % total_8k) * 8192 + (addr & 0x1FFF);
        return c->prg_rom[offset % c->prg_rom_size];
    }

    return 0;
}

static void fme7_cpu_write(Cartridge *c, uint16_t addr, uint8_t val) {
    FME7Data *d = (FME7Data*)c->mapper_data;

    if (addr >= 0x6000 && addr <= 0x7FFF) {
        uint8_t reg = d->prg_banks[0];
        // Bit 7 must be 1 (RAM Enable) AND Bit 6 must be 1 (Write Enable)
        if ((reg & 0x80) && (reg & 0x40) && c->prg_ram && c->prg_ram_size > 0) {
            c->prg_ram[(addr - 0x6000) % c->prg_ram_size] = val;
        }
        return;
    }

    if (addr >= 0x8000 && addr <= 0x9FFF) {
        d->cmd = val & 0x0F;
        return;
    }

    if (addr >= 0xA000 && addr <= 0xBFFF) {
        if (d->cmd <= 7) {
            d->chr_banks[d->cmd] = val;
        } else if (d->cmd >= 8 && d->cmd <= 11) {
            d->prg_banks[d->cmd - 8] = val;
        } else if (d->cmd == 12) {
            switch (val & 0x03) {
                case 0: c->mirroring = MIRROR_VERTICAL; break;
                case 1: c->mirroring = MIRROR_HORIZONTAL; break;
                case 2: c->mirroring = MIRROR_ONE_SCREEN_LOW; break;
                case 3: c->mirroring = MIRROR_ONE_SCREEN_HIGH; break;
            }
        } else if (d->cmd == 13) {
            d->irq_ctrl = val;
            c->nes->lines.irq_line = false;
        } else if (d->cmd == 14) {
            d->irq_counter = (d->irq_counter & 0xFF00) | val;
        } else if (d->cmd == 15) {
            d->irq_counter = (d->irq_counter & 0x00FF) | ((uint16_t)val << 8);
        }
    }
}

static uint8_t fme7_ppu_read(Cartridge *c, uint16_t addr, bool *handled) {
    FME7Data *d = (FME7Data*)c->mapper_data;
    if (addr >= 0x2000 || c->chr_rom_size == 0) return 0;

    *handled = true;
    uint32_t total_1k = c->chr_rom_size / 1024;
    if (total_1k == 0) return 0;

    uint32_t bank = d->chr_banks[(addr / 1024) & 0x07];
    uint32_t offset = (bank % total_1k) * 1024 + (addr & 0x03FF);
    return c->chr_rom[offset % c->chr_rom_size];
}

static void fme7_ppu_write(Cartridge *c, uint16_t addr, uint8_t val) {
    FME7Data *d = (FME7Data*)c->mapper_data;
    if (addr >= 0x2000 || c->chr_rom_size == 0) return;

    uint32_t total_1k = c->chr_rom_size / 1024;
    if (total_1k == 0) return;

    uint32_t bank = d->chr_banks[(addr / 1024) & 0x07];
    uint32_t offset = (bank % total_1k) * 1024 + (addr & 0x03FF);
    c->chr_rom[offset % c->chr_rom_size] = val;
}

static uint16_t fme7_remap_ciram_addr(Cartridge *c, uint16_t addr, bool *ciram_ce) {
    *ciram_ce = true;
    return cartridge_default_remap_ciram(c->mirroring, addr);
}

static const MapperInterface fme7_interface = {
    .reset = fme7_reset,
    .destroy = fme7_destroy,
    .cpu_read = fme7_cpu_read,
    .cpu_write = fme7_cpu_write,
    .ppu_read = fme7_ppu_read,
    .ppu_write = fme7_ppu_write,
    .ppu_addr_change = NULL,
    .clock_m2 = fme7_clock_m2,
    .remap_ciram_addr = fme7_remap_ciram_addr
};

void mapper_069_init(Cartridge *cart) {
    FME7Data *data = calloc(1, sizeof(FME7Data));
    cart->mapper_data = data;
    cart->vtable = &fme7_interface;
    fme7_reset(cart);
}