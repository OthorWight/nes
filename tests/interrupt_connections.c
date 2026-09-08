#include "test_system.h"
#include "mappers.h"

static TestSystem s;

static void ppu_nmi_reaches_cpu_and_returns_to_interrupted_code(void) {
    test_system_init(&s);
    nes_cpu_bus_write(&s.nes, 0x2000, 0x80);
    s.nes.ppu.scanline = 241;
    s.nes.ppu.cycle = 0;
    test_step(&s, 2); // Vblank begins during this NOP.
    assert(s.nes.cpu.nmi_edge);
    test_step(&s, 7); // NMI ignores the CPU interrupt-disable flag.
    assert(s.nes.cpu.program_counter == 0xA000);
    assert(s.nes.cpu.stack_pointer == 0xFA);
    assert(s.nes.wram[0x1FD] == 0x80 && s.nes.wram[0x1FC] == 1);
    assert(!(s.nes.wram[0x1FB] & FLAG_BREAK_COMMAND));
    test_step(&s, 6); // RTI.
    assert(s.nes.cpu.program_counter == 0x8001);
    assert(s.nes.cpu.stack_pointer == 0xFD);
    test_step(&s, 2); // NMI line remains asserted, but does not retrigger.
    assert(s.nes.cpu.program_counter == 0x8002);

    nes_cpu_bus_write(&s.nes, 0x2000, 0);
    nes_cpu_bus_write(&s.nes, 0x2000, 0x80); // New edge while vblank is still set.
    test_step(&s, 7);
    assert(s.nes.cpu.program_counter == 0xA000);
}

static void pending_nmi_takes_priority_over_irq(void) {
    test_system_init(&s);
    s.nes.cpu.status_flags = FLAG_UNUSED;
    s.nes.lines.irq_line = true;
    nes_cpu_bus_write(&s.nes, 0x2000, 0x80);
    s.nes.ppu.scanline = 241;
    s.nes.ppu.cycle = 1;
    ppu_step(&s.nes);
    test_step(&s, 7);
    assert(s.nes.cpu.program_counter == 0xA000);
    test_step(&s, 6); // RTI restores I=0; IRQ is still asserted.
    test_step(&s, 7);
    assert(s.nes.cpu.program_counter == 0x9000);
}

static void apu_and_mapper_irq_acknowledgements_are_independent(void) {
    test_system_init(&s);
    mapper_118_init(&s.cart); // Use an actual mapper IRQ acknowledgement path.
    // Seed simultaneous pending sources, as if their devices fired together.
    s.nes.lines.irq_line = true;
    cpu_set_irq_line(&s.nes.cpu, 0, true);
    s.nes.apu.frame_irq_active = true;
    cpu_set_irq_line(&s.nes.cpu, APU_IRQ_SOURCE_FRAME, true);
    s.nes.apu.dmc_irq_active = true;
    cpu_set_irq_line(&s.nes.cpu, APU_IRQ_SOURCE_DMC, true);
    nes_cpu_bus_write(&s.nes, 0xE000, 0);
    assert(s.nes.cpu.irq_lines == 6);
    assert((nes_cpu_bus_read(&s.nes, 0x4015) & 0xC0) == 0xC0);
    assert(s.nes.cpu.irq_lines == 4);
    s.nes.cpu.status_flags = FLAG_UNUSED;
    uint64_t cycles = s.nes.cpu.cycle_count;
    nes_clock_tick(&s.nes); // System's mapper-line update must preserve APU IRQ.
    assert(s.nes.cpu.cycle_count - cycles == 7);
    assert(s.nes.cpu.program_counter == 0x9000);
    nes_cpu_bus_write(&s.nes, 0x4010, 0); // Disable/acknowledge DMC IRQ.
    assert(!s.nes.cpu.irq_lines);
    s.cart.vtable->destroy(&s.cart);
}

static void running_apu_delivers_frame_irq_to_cpu(void) {
    test_system_init(&s);
    test_ram_idle(&s);
    // Leave I set while waiting for the device to assert its output. This tests
    // routing and acknowledgement, not the precise frame-sequencer event cycle.
    while (!s.nes.apu.frame_irq_active && s.nes.cpu.cycle_count < 35000) {
        test_step(&s, 3);
    }
    assert(s.nes.apu.frame_irq_active);
    assert(s.nes.cpu.program_counter == 0x0200);
    s.nes.cpu.status_flags = FLAG_UNUSED;
    test_step(&s, 3); // Poll the line with the newly installed I flag.
    test_step(&s, 7);
    assert(s.nes.cpu.program_counter == 0x9000);
    assert(nes_cpu_bus_read(&s.nes, 0x4015) & 0x40);
    assert(!s.nes.cpu.irq_lines);
    test_step(&s, 6);
    assert(s.nes.cpu.program_counter == 0x0200);
    test_step(&s, 3);
}

static void rendering_ppu_clocks_mapper_irq_into_cpu(void) {
    test_system_init(&s);
    mapper_118_init(&s.cart);
    test_ram_idle(&s);
    nes_cpu_bus_write(&s.nes, 0x2000, 0x08);
    nes_cpu_bus_write(&s.nes, 0x2001, 0x18);
    nes_cpu_bus_write(&s.nes, 0xC000, 1);
    nes_cpu_bus_write(&s.nes, 0xC001, 0);
    nes_cpu_bus_write(&s.nes, 0xE001, 0);
    s.nes.cpu.status_flags = FLAG_UNUSED;
    while (s.nes.cpu.program_counter != 0x9000 && s.nes.cpu.cycle_count < 1000) {
        nes_clock_tick(&s.nes);
    }
    assert(s.nes.cpu.program_counter == 0x9000);
    assert(s.nes.wram[0x1FD] == 2 && s.nes.wram[0x1FC] == 0);
    assert(s.nes.lines.irq_line && (s.nes.cpu.irq_lines & 1));
    nes_cpu_bus_write(&s.nes, 0xE000, 0);
    assert(!s.nes.lines.irq_line && !(s.nes.cpu.irq_lines & 1));
    nes_clock_tick(&s.nes); // RTI.
    assert(s.nes.cpu.program_counter == 0x0200);
    s.cart.vtable->destroy(&s.cart);
}

int main(void) {
    RUN_TEST(ppu_nmi_reaches_cpu_and_returns_to_interrupted_code);
    RUN_TEST(pending_nmi_takes_priority_over_irq);
    RUN_TEST(apu_and_mapper_irq_acknowledgements_are_independent);
    RUN_TEST(running_apu_delivers_frame_irq_to_cpu);
    RUN_TEST(rendering_ppu_clocks_mapper_irq_into_cpu);
    return 0;
}
