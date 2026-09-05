#ifndef TEST_SYSTEM_H
#define TEST_SYSTEM_H

#include "nes_system.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

// Synthetic cartridge: real CPU/APU/PPU and system bus, observable mapper pins.
// No game images or instruction/clock mocks are involved.
typedef struct {
    NES nes;
    Cartridge cart;
    uint8_t prg[32768];
    uint8_t chr[8192];
    uint64_t ppu_ticks;
    uint64_t m2_ticks;
    uint16_t reads[128];
    unsigned read_count;
    uint16_t writes[8];
    uint8_t write_values[8];
    uint64_t write_cycles[8];
    unsigned write_count;
} TestSystem;

static uint8_t test_cart_read(Cartridge *cart, uint16_t addr, bool *handled) {
    TestSystem *s = cart->mapper_data;
    if (addr < 0x8000) return 0;
    *handled = true;
    if (s->read_count < 128) s->reads[s->read_count++] = addr;
    return s->prg[addr - 0x8000];
}

static void test_cart_write(Cartridge *cart, uint16_t addr, uint8_t value) {
    TestSystem *s = cart->mapper_data;
    assert(s->write_count < 8);
    unsigned i = s->write_count++;
    s->writes[i] = addr;
    s->write_values[i] = value;
    s->write_cycles[i] = s->nes.cpu.cycle_count;
}

static uint8_t test_chr_read(Cartridge *cart, uint16_t addr, bool *handled) {
    TestSystem *s = cart->mapper_data;
    if (addr >= 0x2000) return 0;
    *handled = true;
    return s->chr[addr];
}

static void test_ppu_dot(Cartridge *cart, uint16_t addr) {
    (void)addr;
    ((TestSystem *)cart->mapper_data)->ppu_ticks++;
}

static void test_m2_tick(Cartridge *cart) {
    ((TestSystem *)cart->mapper_data)->m2_ticks++;
}

static const MapperInterface test_mapper = {
    .cpu_read = test_cart_read,
    .cpu_write = test_cart_write,
    .ppu_read = test_chr_read,
    .ppu_dot = test_ppu_dot,
    .clock_m2 = test_m2_tick
};

static inline void test_system_init(TestSystem *s) {
    memset(s, 0, sizeof(*s));
    nes_init(&s->nes);
    memset(s->prg, 0xEA, sizeof(s->prg)); // NOPs unless a test supplies code.
    s->cart.nes = &s->nes;
    s->cart.mapper_data = s;
    s->cart.vtable = &test_mapper;
    s->cart.mirroring = MIRROR_VERTICAL;
    s->cart.prg_rom = s->prg;
    s->cart.prg_rom_size = sizeof(s->prg);
    s->cart.chr_rom = s->chr;
    s->cart.chr_rom_size = sizeof(s->chr);
    s->nes.cart = &s->cart;
    s->nes.cpu.program_counter = 0x8000;
    s->prg[0x1000] = 0x40; // IRQ handler at $9000: RTI.
    s->prg[0x2000] = 0x40; // NMI handler at $A000: RTI.
    s->prg[0x7FFA] = 0x00; s->prg[0x7FFB] = 0xA0;
    s->prg[0x7FFC] = 0x00; s->prg[0x7FFD] = 0x80;
    s->prg[0x7FFE] = 0x00; s->prg[0x7FFF] = 0x90;
}

static inline void test_step(TestSystem *s, unsigned cycles) {
    uint64_t cpu_before = s->nes.cpu.cycle_count;
    uint64_t ppu_before = s->ppu_ticks;
    uint64_t m2_before = s->m2_ticks;
    nes_clock_tick(&s->nes);
    assert(s->nes.cpu.cycle_count - cpu_before == cycles);
    assert(s->ppu_ticks - ppu_before == cycles * 3u);
    assert(s->m2_ticks - m2_before == cycles);
}

static inline void test_ram_idle(TestSystem *s) {
    // Leave cartridge reads available for observing DMC sample fetches.
    s->nes.cpu.program_counter = 0x0200;
    s->nes.wram[0x200] = 0x4C; // JMP $0200.
    s->nes.wram[0x201] = 0x00;
    s->nes.wram[0x202] = 0x02;
}

#define RUN_TEST(fn) do { puts("  " #fn); fflush(stdout); fn(); } while (0)

#endif
