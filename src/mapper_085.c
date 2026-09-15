#include "mappers.h"
#include "nes_system.h"
#include "state_io.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t prg[3], chr[8], control, latch, counter, irq_control;
    int16_t prescaler;
} Vrc7Mapper;

static void irq(Cartridge *c, bool level) {
    c->nes->lines.irq_line = level;
    cpu_set_irq_line(&c->nes->cpu, 0, level);
}
static void reset(Cartridge *c) {
    Vrc7Mapper *m = c->mapper_data;
    memset(m, 0, sizeof(*m));
    m->prescaler = 341;
    c->mirroring = MIRROR_VERTICAL;
    irq(c, false);
}
static void destroy(Cartridge *c) { free(c->mapper_data); c->mapper_data = NULL; }
static uint8_t cpu_read(Cartridge *c, uint16_t address, bool *handled) {
    Vrc7Mapper *m = c->mapper_data;
    if (address >= 0x6000 && address < 0x8000) {
        *handled = true;
        return (m->control & 0x80) ? cartridge_ram_read(c, address - 0x6000) : cartridge_open_bus(c);
    }
    if (address < 0x8000) return 0;
    *handled = true;
    unsigned slot = (address - 0x8000) >> 13;
    unsigned banks = c->prg_rom_size / 8192;
    unsigned bank = slot == 3 ? banks - 1 : m->prg[slot] % banks;
    return c->prg_rom[bank * 8192 + (address & 8191)];
}
static void cpu_write(Cartridge *c, uint16_t address, uint8_t value) {
    Vrc7Mapper *m = c->mapper_data;
    if (address >= 0x6000 && address < 0x8000) {
        if (m->control & 0x80) cartridge_ram_write(c, address - 0x6000, value);
        return;
    }
    if (address < 0x8000) return;
    // Legacy mapper 85 images use either A3 or A4 for register selection.
    unsigned sub = (address & 0x18) != 0;
    switch (address & 0xF000) {
        case 0x8000: m->prg[sub] = value & 63; break;
        case 0x9000: if (!sub) m->prg[2] = value & 63; break;
        case 0xA000: case 0xB000: case 0xC000: case 0xD000:
            m->chr[((address >> 12) - 10) * 2 + sub] = value;
            break;
        case 0xE000:
            if (sub) m->latch = value;
            else {
                static const MirroringMode mirrors[] = {MIRROR_VERTICAL, MIRROR_HORIZONTAL,
                    MIRROR_ONE_SCREEN_LOW, MIRROR_ONE_SCREEN_HIGH};
                m->control = value;
                c->mirroring = mirrors[value & 3];
            }
            break;
        case 0xF000:
            irq(c, false);
            if (sub) m->irq_control = (m->irq_control & ~2u) | ((m->irq_control & 1) << 1);
            else {
                m->irq_control = value & 7;
                if (value & 2) { m->counter = m->latch; m->prescaler = 341; }
            }
            break;
    }
}
static void clock_m2(Cartridge *c) {
    Vrc7Mapper *m = c->mapper_data;
    if (!(m->irq_control & 2)) return;
    if (!(m->irq_control & 4)) {
        m->prescaler -= 3;
        if (m->prescaler > 0) return;
        m->prescaler += 341;
    }
    if (m->counter == 255) { m->counter = m->latch; irq(c, true); }
    else ++m->counter;
}
static unsigned chr_offset(Cartridge *c, uint16_t address) {
    Vrc7Mapper *m = c->mapper_data;
    return ((unsigned)m->chr[address >> 10] * 1024 + (address & 1023)) % c->chr_rom_size;
}
static uint8_t ppu_read(Cartridge *c, uint16_t address, bool *handled) {
    if (address >= 0x2000 || !c->chr_rom_size) return 0;
    *handled = true;
    return c->chr_rom[chr_offset(c, address)];
}
static void ppu_write(Cartridge *c, uint16_t address, uint8_t value) {
    if (address < 0x2000 && c->chr_rom_size) cartridge_chr_write(c, chr_offset(c, address), value);
}
static uint16_t ciram(Cartridge *c, uint16_t address, bool *enabled) {
    *enabled = true;
    return cartridge_default_remap_ciram(c->mirroring, address);
}
static void state(Cartridge *c, StateIO *io) {
    Vrc7Mapper *m = c->mapper_data;
    state_bytes(io, m->prg, sizeof(m->prg));
    state_bytes(io, m->chr, sizeof(m->chr));
    m->control = state_u8(io, m->control);
    m->latch = state_u8(io, m->latch);
    m->counter = state_u8(io, m->counter);
    m->irq_control = state_u8(io, m->irq_control);
    m->prescaler = state_i16(io, m->prescaler);
    if (m->prescaler < 1 || m->prescaler > 341 || m->irq_control > 7 ||
        m->prg[0] > 63 || m->prg[1] > 63 || m->prg[2] > 63) io->ok = false;
}
static const MapperInterface interface = {
    .reset = reset, .destroy = destroy, .cpu_read = cpu_read, .cpu_write = cpu_write,
    .ppu_read = ppu_read, .ppu_write = ppu_write, .clock_m2 = clock_m2,
    .remap_ciram_addr = ciram, .state = state, .state_size = sizeof(Vrc7Mapper)
};
void mapper_085_init(Cartridge *c) {
    c->mapper_data = calloc(1, sizeof(Vrc7Mapper));
    if (!c->mapper_data) return;
    c->vtable = &interface;
    reset(c);
}
