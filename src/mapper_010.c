#include "mappers.h"
#include "cartridge.h"
#include <stdlib.h>

typedef struct {
    uint8_t prg_bank;
    uint8_t chr_bank_0_fd;
    uint8_t chr_bank_0_fe;
    uint8_t chr_bank_1_fd;
    uint8_t chr_bank_1_fe;
    uint8_t latch_0; // 0 = $FD, 1 = $FE
    uint8_t latch_1; // 0 = $FD, 1 = $FE
} MMC4Data;

static void mmc4_reset(Cartridge *c) {
    MMC4Data *d = (MMC4Data*)c->mapper_data;
    d->prg_bank = 0;
    d->chr_bank_0_fd = 0;
    d->chr_bank_0_fe = 0;
    d->chr_bank_1_fd = 0;
    d->chr_bank_1_fe = 0;
    d->latch_0 = 1;
    d->latch_1 = 1;
    c->mirroring = MIRROR_VERTICAL;
}

static void mmc4_destroy(Cartridge *c) {
    free(c->mapper_data);
    c->mapper_data = NULL;
}

static uint8_t mmc4_cpu_read(Cartridge *c, uint16_t addr, bool *handled) {
    MMC4Data *d = (MMC4Data*)c->mapper_data;

    if (addr >= 0x6000 && addr <= 0x7FFF) {
        *handled = true;
        return (c->prg_ram && c->prg_ram_size > 0) ? c->prg_ram[addr - 0x6000] : 0;
    }

    if (addr >= 0x8000) {
        *handled = true;
        uint32_t total_banks = c->prg_rom_size / 16384;
        if (total_banks == 0) return 0;

        uint32_t bank = (addr < 0xC000) ? d->prg_bank : (total_banks - 1);
        uint32_t offset = (bank % total_banks) * 16384 + (addr & 0x3FFF);
        return c->prg_rom[offset % c->prg_rom_size];
    }

    return 0;
}

static void mmc4_cpu_write(Cartridge *c, uint16_t addr, uint8_t val) {
    MMC4Data *d = (MMC4Data*)c->mapper_data;

    if (addr >= 0x6000 && addr <= 0x7FFF) {
        if (c->prg_ram && c->prg_ram_size > 0) {
            c->prg_ram[addr - 0x6000] = val;
        }
        return;
    }

    if (addr >= 0xA000) {
        uint8_t reg = (addr >> 12) & 0x07;
        switch (reg) {
            case 2: d->prg_bank = val & 0x0F; break;
            case 3: d->chr_bank_0_fd = val & 0x1F; break;
            case 4: d->chr_bank_0_fe = val & 0x1F; break;
            case 5: d->chr_bank_1_fd = val & 0x1F; break;
            case 6: d->chr_bank_1_fe = val & 0x1F; break;
            case 7: c->mirroring = (val & 1) ? MIRROR_HORIZONTAL : MIRROR_VERTICAL; break;
        }
    }
}

static uint8_t mmc4_ppu_read(Cartridge *c, uint16_t addr, bool *handled) {
    MMC4Data *d = (MMC4Data*)c->mapper_data;
    if (addr >= 0x2000 || c->chr_rom_size == 0) return 0;

    *handled = true;

    uint8_t latch = (addr < 0x1000) ? d->latch_0 : d->latch_1;
    uint32_t bank = (addr < 0x1000) ?
        ((latch == 0) ? d->chr_bank_0_fd : d->chr_bank_0_fe) :
        ((latch == 0) ? d->chr_bank_1_fd : d->chr_bank_1_fe);

    uint32_t total_banks = c->chr_rom_size / 4096;
    if (total_banks > 0) {
        bank %= total_banks;
    }

    uint32_t offset = bank * 4096 + (addr & 0x0FFF);
    uint8_t data = c->chr_rom[offset % c->chr_rom_size];

    uint16_t sub = addr & 0x1FFF;
    if (sub >= 0x0FD8 && sub <= 0x0FDF) {
        d->latch_0 = 0;
    } else if (sub >= 0x0FE8 && sub <= 0x0FEF) {
        d->latch_0 = 1;
    } else if (sub >= 0x1FD8 && sub <= 0x1FDF) {
        d->latch_1 = 0;
    } else if (sub >= 0x1FE8 && sub <= 0x1FEF) {
        d->latch_1 = 1;
    }

    return data;
}

static void mmc4_ppu_write(Cartridge *c, uint16_t addr, uint8_t val) {
    MMC4Data *d = (MMC4Data*)c->mapper_data;
    if (addr >= 0x2000 || c->chr_rom_size == 0) return;

    uint32_t bank = 0;
    if (addr < 0x1000) {
        bank = (d->latch_0 == 0) ? d->chr_bank_0_fd : d->chr_bank_0_fe;
    } else {
        bank = (d->latch_1 == 0) ? d->chr_bank_1_fd : d->chr_bank_1_fe;
    }

    uint32_t total_banks = c->chr_rom_size / 4096;
    if (total_banks > 0) {
        bank %= total_banks;
    }

    uint32_t offset = bank * 4096 + (addr & 0x0FFF);
    c->chr_rom[offset % c->chr_rom_size] = val;
}

static uint16_t mmc4_remap_ciram_addr(Cartridge *c, uint16_t addr, bool *ciram_ce) {
    *ciram_ce = true;
    return cartridge_default_remap_ciram(c->mirroring, addr);
}

static const MapperInterface mmc4_interface = {
    .reset = mmc4_reset,
    .destroy = mmc4_destroy,
    .cpu_read = mmc4_cpu_read,
    .cpu_write = mmc4_cpu_write,
    .ppu_read = mmc4_ppu_read,
    .ppu_write = mmc4_ppu_write,
    .ppu_addr_change = NULL,
    .clock_m2 = NULL,
    .remap_ciram_addr = mmc4_remap_ciram_addr
};

void mapper_010_init(Cartridge *cart) {
    MMC4Data *data = calloc(1, sizeof(MMC4Data));
    cart->mapper_data = data;
    cart->vtable = &mmc4_interface;
    mmc4_reset(cart);
}