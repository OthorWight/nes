#include "mappers.h"
#include "cartridge.h"

static uint8_t nrom_cpu_read(Cartridge *c, uint16_t addr, bool *handled) {
    if (addr >= 0x6000 && addr <= 0x7FFF) {
        *handled = true;
        return (c->prg_ram && c->prg_ram_size > 0) ? c->prg_ram[addr - 0x6000] : 0;
    }
    if (addr >= 0x8000) {
        *handled = true;
        uint16_t offset = (uint16_t)(addr - 0x8000);
        if (c->prg_rom_size == 16384) {
            offset &= 0x3FFF;
        }
        return c->prg_rom[offset % c->prg_rom_size];
    }
    return 0;
}

static void nrom_cpu_write(Cartridge *c, uint16_t addr, uint8_t val) {
    if (addr >= 0x6000 && addr <= 0x7FFF && c->prg_ram && c->prg_ram_size > 0) {
        c->prg_ram[addr - 0x6000] = val;
    }
}

static uint8_t nrom_ppu_read(Cartridge *c, uint16_t addr, bool *handled) {
    if (addr < 0x2000 && c->chr_rom_size > 0) {
        *handled = true;
        return c->chr_rom[addr & 0x1FFF];
    }
    return 0;
}

static void nrom_ppu_write(Cartridge *c, uint16_t addr, uint8_t val) {
    if (addr < 0x2000 && c->chr_rom_size > 0) {
        c->chr_rom[addr & 0x1FFF] = val;
    }
}

static uint16_t nrom_remap_ciram_addr(Cartridge *c, uint16_t addr, bool *ciram_ce) {
    *ciram_ce = true;
    return cartridge_default_remap_ciram(c->mirroring, addr);
}

static const MapperInterface nrom_interface = {
    .reset = NULL,
    .destroy = NULL,
    .cpu_read = nrom_cpu_read,
    .cpu_write = nrom_cpu_write,
    .ppu_read = nrom_ppu_read,
    .ppu_write = nrom_ppu_write,
    .ppu_addr_change = NULL,
    .clock_m2 = NULL,
    .remap_ciram_addr = nrom_remap_ciram_addr
};

void mapper_000_init(Cartridge *cart) {
    cart->vtable = &nrom_interface;
    cart->mapper_data = NULL;
}