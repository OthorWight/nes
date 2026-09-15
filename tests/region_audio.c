#include "test_system.h"
#include <math.h>

static TestSystem s;
static void tick(void) { s.nes.cpu.stall_cycles = 1; nes_clock_tick(&s.nes); }

static void cpu_ppu_ratio_and_frame_length(void) {
    for (unsigned region = NES_NTSC; region <= NES_DENDY; ++region) {
        test_system_init(&s);
        nes_set_region(&s.nes, region);
        for (unsigned i = 0; i < 10; ++i) tick();
        assert(s.ppu_ticks == (region == NES_PAL ? 32 : 30));
        assert(s.m2_ticks == 10 && s.nes.cpu.cycle_count == 10);
        s.nes.ppu.scanline = s.nes.ppu.cycle = 0;
        s.nes.ppu.ppu_mask = 0x18;
        s.nes.ppu.odd_frame = true;
        unsigned dots = 0;
        do { ppu_step(&s.nes); ++dots; }
        while (s.nes.ppu.scanline || s.nes.ppu.cycle);
        assert(dots == (region == NES_NTSC ? 89341u : 106392u));
        s.nes.ppu.ppu_status = 0;
        s.nes.ppu.scanline = region == NES_DENDY ? 291 : 241;
        s.nes.ppu.cycle = 1;
        ppu_step(&s.nes);
        assert(s.nes.ppu.ppu_status & 0x80);
    }
}
static void pal_dmc_and_noise_periods(void) {
    const unsigned dmc[] = {398,354,316,298,276,236,210,198,176,148,132,118,98,78,66,50};
    const unsigned noise[] = {4,8,14,30,60,88,118,148,188,236,354,472,708,944,1890,3778};
    for (unsigned rate = 0; rate < 16; ++rate) {
        test_system_init(&s); nes_set_region(&s.nes, NES_PAL);
        nes_cpu_bus_write(&s.nes, 0x4010, (uint8_t)rate);
        nes_cpu_bus_write(&s.nes, 0x400E, (uint8_t)rate);
        s.nes.apu.dmc_silent = false; s.nes.apu.dmc_shift_reg = 255;
        s.nes.apu.dmc_value = 64;
        tick();
        assert(s.nes.apu.dmc_value == 66);
        assert(s.nes.apu.noise_timer == noise[rate] - 1);
        for (unsigned i = 1; i < dmc[rate]; ++i) tick();
        assert(s.nes.apu.dmc_value == 66);
        tick(); assert(s.nes.apu.dmc_value == 68);
    }
}
static void pal_frame_irq_edges_and_read_acknowledgement(void) {
    test_system_init(&s); nes_set_region(&s.nes, NES_PAL);
    s.nes.apu.frame_cycles = 33250;
    tick(); assert(!(nes_cpu_bus_read(&s.nes, 0x4015) & 0x40));
    for (unsigned i = 0; i < 3; ++i) {
        tick();
        assert(nes_cpu_bus_read(&s.nes, 0x4015) & 0x40);
        assert(!(s.nes.cpu.irq_lines & (1 << APU_IRQ_SOURCE_FRAME)));
    }
    tick(); assert(!(nes_cpu_bus_read(&s.nes, 0x4015) & 0x40));
}
static void pal_dmc_does_not_shift_extra_controller_bits(void) {
    test_system_init(&s); nes_set_region(&s.nes, NES_PAL);
    s.prg[0] = 0xAD; s.prg[1] = 0x16; s.prg[2] = 0x40; // LDA $4016.
    s.nes.controller_shift[0] = 0x55;
    s.nes.apu.dmc_bytes_remaining = 1; s.nes.apu.dmc_current_addr = 0xC000;
    s.nes.dmc_dma_pending = true; s.nes.dmc_dma_cycle = 4;
    nes_clock_tick(&s.nes);
    assert(s.nes.controller_shift[0] == 0xAA);
    assert((s.nes.cpu.accumulator & 1) == 1);
}
static void output_rate_and_dc_rejection(void) {
    for (unsigned region = NES_NTSC; region <= NES_DENDY; ++region) {
        APU2A03 a; apu_init(&a); apu_set_region(&a, region);
        unsigned samples = 0, hz = nes_region_cpu_hz(region);
        for (unsigned i = 0; i < hz; ++i) {
            apu_audio_clock(&a, 0.5f);
            samples += a.audio_buffer_idx;
            a.audio_buffer_idx = 0;
        }
        assert(samples >= 44099 && samples <= 44100);
        assert(fabsf(a.lp14000) < 0.0001f);
    }
    assert(apu_mix_dac(0,0,0,0,0) == 0);
    // DMC offset changes the triangle's gain through the nonlinear TND DAC.
    float quiet = apu_mix_dac(0,0,15,0,0) - apu_mix_dac(0,0,0,0,0);
    float loud = apu_mix_dac(0,0,15,0,127) - apu_mix_dac(0,0,0,0,127);
    assert(quiet > loud && loud > 0);
}
int main(void) {
    RUN_TEST(cpu_ppu_ratio_and_frame_length);
    RUN_TEST(pal_dmc_and_noise_periods);
    RUN_TEST(pal_frame_irq_edges_and_read_acknowledgement);
    RUN_TEST(pal_dmc_does_not_shift_extra_controller_bits);
    RUN_TEST(output_rate_and_dc_rejection);
    return 0;
}
