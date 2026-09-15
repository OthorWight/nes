#include "test_system.h"
#include "state_io.h"
#include <math.h>
#include <stdlib.h>

static TestSystem s, restored;
static void select_chip(unsigned mapper) { test_system_init(&s); s.cart.mapper_id = mapper; }
static void write_reg(uint16_t address, uint8_t value) { nes_cpu_bus_write(&s.nes, address, value); }
static void ay(unsigned reg, unsigned value) { write_reg(0xC000, (uint8_t)reg); write_reg(0xE000, (uint8_t)value); }
static void fm(unsigned reg, unsigned value) { write_reg(0x9010, (uint8_t)reg); write_reg(0x9030, (uint8_t)value); }
static void n163(unsigned address, unsigned value) { write_reg(0xF800, (uint8_t)address); write_reg(0x4800, (uint8_t)value); }

static void replay_audio(void) {
    for (unsigned i = 0; i < 1234; ++i) (void)expansion_audio_clock(&s.nes);
    StateIO count = {NULL, SIZE_MAX, 0, false, true};
    expansion_audio_state(&s.nes.expansion, &count); assert(count.ok);
    uint8_t *bytes = malloc(count.pos); assert(bytes);
    StateIO out = {bytes, count.pos, 0, false, true};
    expansion_audio_state(&s.nes.expansion, &out); assert(out.ok);
    test_system_init(&restored); restored.cart.mapper_id = s.cart.mapper_id;
    StateIO in = {bytes, count.pos, 0, true, true};
    expansion_audio_state(&restored.nes.expansion, &in); assert(in.ok && in.pos == count.pos);
    bool changed = false;
    float previous = expansion_audio_clock(&s.nes);
    assert(previous == expansion_audio_clock(&restored.nes));
    for (unsigned i = 0; i < 30000; ++i) {
        float a = expansion_audio_clock(&s.nes), b = expansion_audio_clock(&restored.nes);
        assert(isfinite(a) && a == b);
        changed |= a != previous; previous = a;
    }
    assert(changed);
    free(bytes);
}
static void vrc6_pulses_saw_and_address_swap(void) {
    for (unsigned mapper = 24; mapper <= 26; mapper += 2) {
        select_chip(mapper);
        unsigned low = mapper == 26 ? 2 : 1, high = mapper == 26 ? 1 : 2;
        write_reg(0x9000, 0x3F); write_reg((uint16_t)(0x9000 + low), 24); write_reg((uint16_t)(0x9000 + high), 0x80);
        write_reg(0xB000, 33); write_reg((uint16_t)(0xB000 + low), 32); write_reg((uint16_t)(0xB000 + high), 0x80);
        replay_audio();
    }
}
static void namco_wave_ram_and_auto_increment(void) {
    select_chip(19);
    write_reg(0xF800, 0xFF); write_reg(0x4800, 0x0F); write_reg(0x4800, 0xF0);
    assert(s.nes.expansion.n163.ram[127] == 15 && s.nes.expansion.n163.ram[0] == 0xF0);
    write_reg(0xF800, 0xFF);
    assert(nes_cpu_bus_read(&s.nes, 0x4800) == 15);
    assert(nes_cpu_bus_read(&s.nes, 0x4800) == 0xF0);
    n163(0x78, 0xFF); n163(0x7A, 0xFF); n163(0x7C, 0xFC);
    replay_audio();
}
static void sunsoft_envelope_noise_and_tone(void) {
    select_chip(69);
    ay(0, 24); ay(1, 0); ay(6, 5); ay(7, 0x36);
    ay(8, 0x10); ay(11, 8); ay(12, 0); ay(13, 0x0A);
    replay_audio();
}
static void mmc5_pcm_irq_and_pulses(void) {
    select_chip(5);
    write_reg(0x5010, 0x81);
    s.prg[0] = 0; (void)nes_cpu_bus_read(&s.nes, 0x8000);
    assert(s.nes.cpu.irq_lines & 8);
    assert(nes_cpu_bus_read(&s.nes, 0x5010) & 0x80);
    assert(!(s.nes.cpu.irq_lines & 8));
    s.prg[0] = 123; (void)nes_cpu_bus_read(&s.nes, 0x8000);
    assert(s.nes.expansion.mmc5.pcm == 123);
    write_reg(0x5015, 1); write_reg(0x5000, 0xBF);
    write_reg(0x5002, 31); write_reg(0x5003, 0x18);
    assert(nes_cpu_bus_read(&s.nes, 0x5015) == 1);
    replay_audio();
}
static void fds_wave_engine(void) {
    // Exercise the sound unit independently; this is not a disk-loader test.
    select_chip(20);
    write_reg(0x4023, 2); write_reg(0x4089, 0x80);
    for (unsigned i = 0; i < 64; ++i) write_reg((uint16_t)(0x4040 + i), (uint8_t)i);
    assert((nes_cpu_bus_read(&s.nes, 0x407F) & 63) == 63);
    write_reg(0x4080, 0xA0); write_reg(0x4082, 0xFF); write_reg(0x4083, 3);
    write_reg(0x4089, 0);
    replay_audio();
}
static void vrc7_builtin_and_custom_patch(void) {
    select_chip(85);
    fm(0x30, 0x10); fm(0x10, 0x90); fm(0x20, 0x17);
    replay_audio();
    select_chip(85);
    const uint8_t patch[] = {0x03,0x21,0x05,0x06,0xE8,0x81,0x42,0x27};
    for (unsigned i = 0; i < 8; ++i) fm(i, patch[i]);
    fm(0x30, 0); fm(0x10, 0x90); fm(0x20, 0x17);
    replay_audio();
}
int main(void) {
    RUN_TEST(vrc6_pulses_saw_and_address_swap);
    RUN_TEST(namco_wave_ram_and_auto_increment);
    RUN_TEST(sunsoft_envelope_noise_and_tone);
    RUN_TEST(mmc5_pcm_irq_and_pulses);
    RUN_TEST(fds_wave_engine);
    RUN_TEST(vrc7_builtin_and_custom_patch);
    return 0;
}
