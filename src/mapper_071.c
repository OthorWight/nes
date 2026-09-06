#include "mappers.h"
#include "cartridge.h"
#include "state_io.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#define M071_PRG_BANK_SIZE       0x4000u
#define BEE52_PRG_CRC32          0x6C93377Cu

typedef struct {
    uint8_t prg_bank;
    MirroringMode hardwired_mirroring;
    bool bee52_compatibility;
} CamericaData;

static uint32_t m071_crc32(const uint8_t *data, size_t size) {
    uint32_t crc = 0xFFFFFFFFu;

    if (!data) {
        return 0;
    }

    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit) {
            uint32_t mask = (uint32_t)-(int32_t)(crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }

    return crc ^ 0xFFFFFFFFu;
}

static bool m071_is_bee52(const Cartridge *c) {
    return c && c->prg_rom && c->prg_rom_size > 0 &&
           m071_crc32(c->prg_rom, c->prg_rom_size) == BEE52_PRG_CRC32;
}

static MirroringMode m071_detect_hardwired_mirroring(const Cartridge *c,
                                                      bool is_bee52) {
    if (!c) {
        return MIRROR_HORIZONTAL;
    }

    /* Bee 52's common dump has the opposite mirroring bit in its header. */
    return is_bee52 ? MIRROR_VERTICAL : c->mirroring;
}

static void m071_reset(Cartridge *c) {
    CamericaData *d = c ? (CamericaData *)c->mapper_data : NULL;
    if (!d) {
        return;
    }

    d->prg_bank = 0;
    c->mirroring = d->hardwired_mirroring;
}

static void m071_destroy(Cartridge *c) {
    if (!c) {
        return;
    }

    free(c->mapper_data);
    c->mapper_data = NULL;
}

static uint8_t m071_cpu_read(Cartridge *c, uint16_t addr, bool *handled) {
    CamericaData *d = c ? (CamericaData *)c->mapper_data : NULL;

    if (addr < 0x8000 || !d || !c->prg_rom || c->prg_rom_size == 0) {
        return 0;
    }

    if (handled) {
        *handled = true;
    }

    uint32_t total_16k_banks = c->prg_rom_size / M071_PRG_BANK_SIZE;
    if (total_16k_banks == 0) {
        return 0;
    }

    uint32_t bank = (addr < 0xC000)
        ? ((uint32_t)d->prg_bank % total_16k_banks)
        : (total_16k_banks - 1u);

    uint32_t offset = bank * M071_PRG_BANK_SIZE + (addr & 0x3FFFu);
    return c->prg_rom[offset];
}

static void m071_cpu_write(Cartridge *c, uint16_t addr, uint8_t val) {
    CamericaData *d = c ? (CamericaData *)c->mapper_data : NULL;
    if (!d) {
        return;
    }

    /* BF9097 changes from hardwired to one-screen mirroring on $9000 writes. */
    if (!d->bee52_compatibility && addr >= 0x9000 && addr <= 0x9FFF) {
        c->mirroring = (val & 0x10)
            ? MIRROR_ONE_SCREEN_HIGH
            : MIRROR_ONE_SCREEN_LOW;
        return;
    }

    if (addr >= 0xC000) {
        d->prg_bank = val & 0x0F;
    }
}

static uint8_t m071_ppu_read(Cartridge *c, uint16_t addr, bool *handled) {
    if (c && addr < 0x2000 && c->chr_rom && c->chr_rom_size > 0) {
        if (handled) {
            *handled = true;
        }
        return c->chr_rom[addr % c->chr_rom_size];
    }

    return 0;
}

static void m071_ppu_write(Cartridge *c, uint16_t addr, uint8_t val) {
    if (c && addr < 0x2000 && c->chr_rom && c->chr_rom_size > 0) {
        cartridge_chr_write(c, addr % c->chr_rom_size, val);
    }
}

static uint16_t m071_remap_ciram_addr(Cartridge *c,
                                      uint16_t addr,
                                      bool *ciram_ce) {
    if (ciram_ce) {
        *ciram_ce = true;
    }

    CamericaData *d = c ? (CamericaData *)c->mapper_data : NULL;
    MirroringMode mode = c ? c->mirroring : MIRROR_HORIZONTAL;

    /* Keep Bee 52 fixed even if an old save state restores the bad header bit. */
    if (d && d->bee52_compatibility) {
        mode = d->hardwired_mirroring;
    }

    return cartridge_default_remap_ciram(mode, addr);
}

static void mapper_071_state(Cartridge *c, StateIO *io) {
    CamericaData *d = (CamericaData *)c->mapper_data;
    d->prg_bank = state_u8(io, d->prg_bank);
    d->hardwired_mirroring = state_enum(io, d->hardwired_mirroring);
    d->bee52_compatibility = state_bool(io, d->bee52_compatibility);
    if ((unsigned)d->hardwired_mirroring > MIRROR_ONE_SCREEN_HIGH) io->ok = false;
}

static const MapperInterface m071_interface = {
    .state = mapper_071_state,
    .state_size = sizeof(CamericaData),
    .reset = m071_reset,
    .destroy = m071_destroy,
    .cpu_read = m071_cpu_read,
    .cpu_write = m071_cpu_write,
    .ppu_read = m071_ppu_read,
    .ppu_write = m071_ppu_write,
    .ppu_addr_change = NULL,
    .ppu_dot = NULL,
    .clock_m2 = NULL,
    .remap_ciram_addr = m071_remap_ciram_addr
};

void mapper_071_init(Cartridge *cart) {
    if (!cart) {
        return;
    }

    CamericaData *data = calloc(1, sizeof(*data));
    if (!data) {
        cart->mapper_data = NULL;
        cart->vtable = NULL;
        return;
    }

    data->bee52_compatibility = m071_is_bee52(cart);
    data->hardwired_mirroring =
        m071_detect_hardwired_mirroring(cart, data->bee52_compatibility);

    cart->mapper_data = data;
    cart->vtable = &m071_interface;
    m071_reset(cart);
}
