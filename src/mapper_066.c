#include "mappers.h"
#include "cartridge.h"
#include "state_io.h"
#include <stdlib.h>

typedef struct {
    uint8_t prg_bank;
    uint8_t chr_bank;
    uint32_t prg_offset;
    uint32_t chr_offset;
} GxROMData;

static void m066_update_banks(Cartridge *c) {
    GxROMData *d = (GxROMData*)c->mapper_data;
    uint32_t total_32k = c->prg_rom_size / 32768;
    uint32_t total_8k = c->chr_rom_size / 8192;
    // Resolve wrapping on register changes, not on every CPU/PPU read. Store
    // offsets rather than pointers so staged state loads can copy them safely.
    d->prg_offset = total_32k ? (d->prg_bank % total_32k) * 32768 : 0;
    d->chr_offset = total_8k ? (d->chr_bank % total_8k) * 8192 : 0;
}

static void m066_reset(Cartridge *c) {
    GxROMData *d = (GxROMData*)c->mapper_data;
    d->prg_bank = 0;
    d->chr_bank = 0;
    m066_update_banks(c);
}

static void m066_destroy(Cartridge *c) {
    free(c->mapper_data);
    c->mapper_data = NULL;
}

static uint8_t m066_cpu_read(Cartridge *c, uint16_t addr, bool *handled) {
    GxROMData *d = (GxROMData*)c->mapper_data;

    if (addr >= 0x8000) {
        *handled = true;
        if (c->prg_rom_size < 32768) return 0;
        return c->prg_rom[d->prg_offset + (addr - 0x8000)];
    }

    return 0;
}

static void m066_cpu_write(Cartridge *c, uint16_t addr, uint8_t val) {
    GxROMData *d = (GxROMData*)c->mapper_data;

    if (addr >= 0x8000) {
        d->prg_bank = (val >> 4) & 0x03;
        d->chr_bank = val & 0x03;
        m066_update_banks(c);
    }
}

static uint8_t m066_ppu_read(Cartridge *c, uint16_t addr, bool *handled) {
    GxROMData *d = (GxROMData*)c->mapper_data;

    if (addr < 0x2000 && c->chr_rom_size > 0) {
        *handled = true;
        if (c->chr_rom_size < 8192) return 0;
        return c->chr_rom[d->chr_offset + addr];
    }

    return 0;
}

static void m066_ppu_write(Cartridge *c, uint16_t addr, uint8_t val) {
    GxROMData *d = (GxROMData*)c->mapper_data;

    if (addr < 0x2000 && c->chr_rom_size >= 8192) {
        cartridge_chr_write(c, d->chr_offset + addr, val);
    }
}

static uint16_t m066_remap_ciram_addr(Cartridge *c, uint16_t addr, bool *ciram_ce) {
    *ciram_ce = true;
    return cartridge_default_remap_ciram(c->mirroring, addr);
}

static void mapper_066_state(Cartridge *c, StateIO *io) {
    GxROMData *d = (GxROMData *)c->mapper_data;
    d->prg_bank = state_u8(io, d->prg_bank);
    d->chr_bank = state_u8(io, d->chr_bank);
    // The on-disk layout remains the two bank registers; rebuild derived data.
    if (io->reading) m066_update_banks(c);
}

static const MapperInterface m066_interface = {
    .state = mapper_066_state,
    .state_size = sizeof(GxROMData),
    .reset = m066_reset,
    .destroy = m066_destroy,
    .cpu_read = m066_cpu_read,
    .cpu_write = m066_cpu_write,
    .ppu_read = m066_ppu_read,
    .ppu_write = m066_ppu_write,
    .ppu_addr_change = NULL,
    .clock_m2 = NULL,
    .remap_ciram_addr = m066_remap_ciram_addr
};

void mapper_066_init(Cartridge *cart) {
    GxROMData *data = calloc(1, sizeof(GxROMData));
    if (!data) return;
    cart->mapper_data = data;
    cart->vtable = &m066_interface;
    m066_reset(cart);
}
