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

int main(void) {
    RUN_TEST(frame_counter_write_is_delayed_in_both_phases);
    RUN_TEST(channel_enable_controls_length_status);
    RUN_TEST(pulse_and_triangle_use_different_clock_dividers);
    RUN_TEST(all_ntsc_dmc_rates_use_cpu_cycles);
    RUN_TEST(irq_inhibit_clears_frame_irq_but_not_dmc);
    return 0;
}
