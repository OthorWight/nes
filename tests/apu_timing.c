#include "test_system.h"

static TestSystem s;

static void clock_one(void) {
    // Hold instruction execution for one cycle while using the real system
    // clock path. This gives exact sample points between ordinary instructions.
    s.nes.cpu.stall_cycles = 1;
    test_step(&s, 1);
}

static void clock_cycles(unsigned cycles) {
    while (cycles--) clock_one();
}

static void frame_counter_write_is_delayed_in_both_phases(void) {
    for (unsigned phase = 0; phase < 2; phase++) {
        test_system_init(&s);
        if (phase) clock_one();
        nes_cpu_bus_write(&s.nes, 0x4015, 1);
        nes_cpu_bus_write(&s.nes, 0x4000, 0); // Length counter not halted.
        nes_cpu_bus_write(&s.nes, 0x4003, 0x18); // Length table entry 3 = 2.
        assert(s.nes.apu.pulse_length_counter[0] == 2);
        s.prg[0] = 0x8D; s.prg[1] = 0x17; s.prg[2] = 0x40; // STA $4017.
        s.nes.cpu.accumulator = 0x80; // Five-step reset clocks length/envelope.
        test_step(&s, 4);
        assert(s.nes.apu.pulse_length_counter[0] == 2);
        unsigned delay = phase ? 3 : 4;
        clock_cycles(delay - 1);
        assert(s.nes.apu.pulse_length_counter[0] == 2);
        clock_one();
        assert(s.nes.apu.pulse_length_counter[0] == 1);
        assert(s.nes.apu.pulse_envelope_decay[0] == 15);
        assert(!s.nes.apu.frame_counter_reset_pending);
        assert(s.nes.apu.frame_cycles == 0);
    }
}

static void channel_enable_controls_length_status(void) {
    test_system_init(&s);
    const uint16_t length_registers[] = {0x4003, 0x4007, 0x400B, 0x400F};
    for (unsigned i = 0; i < 4; i++) nes_cpu_bus_write(&s.nes, length_registers[i], 0);
    assert((nes_cpu_bus_read(&s.nes, 0x4015) & 15) == 0); // Disabled loads ignored.
    nes_cpu_bus_write(&s.nes, 0x4015, 15);
    for (unsigned i = 0; i < 4; i++) nes_cpu_bus_write(&s.nes, length_registers[i], 0);
    assert((nes_cpu_bus_read(&s.nes, 0x4015) & 15) == 15);
    nes_cpu_bus_write(&s.nes, 0x4015, 0);
    assert((nes_cpu_bus_read(&s.nes, 0x4015) & 15) == 0);
    nes_cpu_bus_write(&s.nes, 0x4015, 15);
    assert((nes_cpu_bus_read(&s.nes, 0x4015) & 15) == 0); // Enabling does not reload.
}

static void pulse_and_triangle_use_different_clock_dividers(void) {
    test_system_init(&s);
    nes_cpu_bus_write(&s.nes, 0x4015, 5);
    nes_cpu_bus_write(&s.nes, 0x4002, 8);
    nes_cpu_bus_write(&s.nes, 0x4003, 0);
    nes_cpu_bus_write(&s.nes, 0x4008, 0x85);
    nes_cpu_bus_write(&s.nes, 0x400A, 2);
    nes_cpu_bus_write(&s.nes, 0x400B, 0);
    // Seed the linear counter to isolate timer division from frame sequencing.
    s.nes.apu.triangle_linear_counter = 5;
    clock_one();
    uint8_t pulse = s.nes.apu.pulse_sequence_idx[0];
    uint8_t triangle = s.nes.apu.triangle_sequence_idx;
    clock_cycles(2);
    assert(s.nes.apu.triangle_sequence_idx == triangle);
    clock_one(); // Triangle timer=2: one step per 3 CPU cycles.
    assert(s.nes.apu.triangle_sequence_idx == ((triangle + 1) & 31));
    clock_cycles(14);
    assert(s.nes.apu.pulse_sequence_idx[0] == pulse);
    clock_one(); // Pulse timer=8: one step per 18 CPU cycles.
    assert(s.nes.apu.pulse_sequence_idx[0] == ((pulse + 1) & 7));
}

static void all_ntsc_dmc_rates_use_cpu_cycles(void) {
    // Hardware periods, independent of the emulator's private rate table.
    const unsigned periods[] = {428,380,340,320,286,254,226,214,
                                190,160,142,128,106,84,72,54};
    for (unsigned rate = 0; rate < 16; rate++) {
        test_system_init(&s);
        nes_cpu_bus_write(&s.nes, 0x4010, (uint8_t)rate);
        nes_cpu_bus_write(&s.nes, 0x4011, 64);
        // A buffered byte continues playing even with sample DMA disabled.
        s.nes.apu.dmc_silent = false;
        s.nes.apu.dmc_shift_reg = 0xFF;
        s.nes.apu.dmc_bits_remaining = 8;
        clock_one();
        assert(s.nes.apu.dmc_value == 66);
        for (unsigned value = 68; value <= 72; value += 2) {
            clock_cycles(periods[rate] - 1);
            assert(s.nes.apu.dmc_value == value - 2);
            clock_one();
            assert(s.nes.apu.dmc_value == value);
        }
    }
}

static void irq_inhibit_clears_frame_irq_but_not_dmc(void) {
    test_system_init(&s);
    s.nes.apu.frame_irq_active = true;
    s.nes.apu.dmc_irq_active = true;
    cpu_set_irq_line(&s.nes.cpu, APU_IRQ_SOURCE_FRAME, true);
    cpu_set_irq_line(&s.nes.cpu, APU_IRQ_SOURCE_DMC, true);
    nes_cpu_bus_write(&s.nes, 0x4017, 0xC0);
    assert(!s.nes.apu.frame_irq_active);
    assert(s.nes.cpu.irq_lines == (1 << APU_IRQ_SOURCE_DMC));
    nes_cpu_bus_write(&s.nes, 0x4017, 0x80); // Five-step mode with inhibit clear.
    clock_cycles(40000); // A complete five-step sequence cannot assert frame IRQ.
    assert(!s.nes.apu.frame_irq_active);
    assert(s.nes.cpu.irq_lines == (1 << APU_IRQ_SOURCE_DMC));
}

static void length_reload_and_halt_collisions(void) {
    for (unsigned empty = 0; empty < 2; ++empty) {
        test_system_init(&s);
        APU2A03 *a = &s.nes.apu;
        a->pulse_enabled[0] = true;
        a->pulse_length_counter[0] = empty ? 0 : 10;
        a->frame_cycles = 14912;
        apu_write_reg(&s.nes, 0x4003, 0x18); // Reload 2 on the half-frame clock.
        clock_one();
        assert(a->pulse_length_counter[0] == (empty ? 2 : 9));
    }
    test_system_init(&s);
    s.nes.apu.pulse_length_counter[0] = 10;
    s.nes.apu.frame_cycles = 14912;
    apu_write_reg(&s.nes, 0x4000, 0x20);
    clock_one();
    assert(s.nes.apu.pulse_length_counter[0] == 9 && s.nes.apu.pulse_halt[0]);
}

static void stopped_triangle_holds_dac(void) {
    test_system_init(&s);
    s.nes.apu.triangle_sequence_idx = 10; // DAC holds 5.
    clock_cycles(10);
    assert(s.nes.apu.triangle_sequence_idx == 10);
    assert(s.nes.apu.mixed_previous == apu_mix_dac(0, 0, 5, 0, 0));
}

static void channel_mutes_only_change_the_mix(void) {
    static TestSystem reference;
    for (unsigned mute = 0; mute < 32; ++mute) {
        test_system_init(&s); test_system_init(&reference);
        APU2A03 *a = &s.nes.apu;
        for (unsigned ch = 0; ch < 2; ++ch) {
            a->pulse_enabled[ch] = a->pulse_constant_volume[ch] = true;
            a->pulse_length_counter[ch] = 10;
            a->pulse_duty[ch] = 3; a->pulse_sequence_idx[ch] = 3;
            a->pulse_timer[ch] = a->pulse_timer_reload[ch] = 100;
            a->pulse_volume[ch] = ch ? 7 : 11;
        }
        a->triangle_sequence_idx = 10; a->triangle_timer = 100; // Held DAC = 5.
        a->noise_enabled = a->noise_constant_volume = true;
        a->noise_length_counter = 10; a->noise_shift_reg = 2;
        a->noise_timer = 100; a->noise_volume = 3;
        a->dmc_value = 64; a->dmc_timer = 100;
        reference.nes.apu = *a;
        s.nes.audio_muted_channels = (uint16_t)mute;
        apu_step(a, &s.nes); apu_step(&reference.nes.apu, &reference.nes);
        float expected = apu_mix_dac(mute & 1 ? 0 : 11, mute & 2 ? 0 : 7,
            mute & 4 ? 0 : 5, mute & 8 ? 0 : 3, mute & 16 ? 0 : 64);
        assert(a->mixed_previous == expected);
        for (unsigned i = 0; i < 1000; ++i) {
            apu_step(a, &s.nes); apu_step(&reference.nes.apu, &reference.nes);
        }
        assert(!memcmp(a, &reference.nes.apu, offsetof(APU2A03, audio_accumulator)));
        assert(a->clock_toggle == reference.nes.apu.clock_toggle);
        assert(!memcmp(&a->length_pending, &reference.nes.apu.length_pending,
            offsetof(APU2A03, sample_impulses) - offsetof(APU2A03, length_pending)));
        s.nes.audio_muted_channels = 0;
        apu_step(a, &s.nes); apu_step(&reference.nes.apu, &reference.nes);
        assert(a->mixed_previous == reference.nes.apu.mixed_previous);
    }
}

int main(void) {
    RUN_TEST(frame_counter_write_is_delayed_in_both_phases);
    RUN_TEST(channel_enable_controls_length_status);
    RUN_TEST(pulse_and_triangle_use_different_clock_dividers);
    RUN_TEST(all_ntsc_dmc_rates_use_cpu_cycles);
    RUN_TEST(irq_inhibit_clears_frame_irq_but_not_dmc);
    RUN_TEST(length_reload_and_halt_collisions);
    RUN_TEST(stopped_triangle_holds_dac);
    RUN_TEST(channel_mutes_only_change_the_mix);
    return 0;
}
