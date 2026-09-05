#include "test_system.h"

static TestSystem s;

static void oam_dma_copies_and_clocks_both_alignments(void) {
    for (unsigned parity = 0; parity < 2; parity++) {
        test_system_init(&s);
        s.nes.cpu.cycle_count = parity;
        s.nes.cpu.accumulator = 0x02;
        s.prg[0] = 0x8D; s.prg[1] = 0x14; s.prg[2] = 0x40; // STA $4014.
        nes_cpu_bus_write(&s.nes, 0x2003, 0xE0);
        for (unsigned i = 0; i < 256; i++) s.nes.wram[0x200 + i] = (uint8_t)(i ^ 0xA5);
        unsigned cycles = 4 + 513 + parity; // Instruction plus halt/alignment/transfer.
        test_step(&s, cycles);
        assert(s.nes.apu.frame_cycles == cycles);
        assert(s.nes.ppu.scanline * 341 + s.nes.ppu.cycle == (int)(cycles * 3));
        assert(s.nes.cpu.program_counter == 0x8003);
        assert(s.nes.ppu.oam_addr == 0xE0);
        for (unsigned i = 0; i < 256; i++) {
            assert(s.nes.ppu.oam_ram[(0xE0 + i) & 255] == (uint8_t)(i ^ 0xA5));
        }
        test_step(&s, 2); // CPU resumes at the next instruction.
        assert(s.nes.cpu.program_counter == 0x8004);
    }
}

static void nmi_during_oam_dma_is_serviced_after_transfer(void) {
    test_system_init(&s);
    s.nes.ppu.scanline = 240;
    s.nes.ppu.cycle = 300;
    nes_cpu_bus_write(&s.nes, 0x2000, 0x80);
    s.nes.cpu.accumulator = 2;
    s.prg[0] = 0x8D; s.prg[1] = 0x14; s.prg[2] = 0x40;
    test_step(&s, 517);
    assert(s.nes.cpu.program_counter == 0x8003);
    assert(s.nes.cpu.nmi_edge);
    assert(s.nes.cpu.stack_pointer == 0xFD); // DMA did not run the handler early.
    test_step(&s, 7);
    assert(s.nes.cpu.program_counter == 0xA000);
    assert(s.nes.wram[0x1FC] == 3);
}

static void dmc_fetches_mapped_memory_wraps_and_stalls_cpu(void) {
    test_system_init(&s);
    test_ram_idle(&s);
    nes_cpu_bus_write(&s.nes, 0x4010, 0x8F); // IRQ, no loop, fastest NTSC rate.
    nes_cpu_bus_write(&s.nes, 0x4012, 0xFF); // $FFC0.
    nes_cpu_bus_write(&s.nes, 0x4013, 4); // 65 bytes: includes $FFFF -> $8000.
    nes_cpu_bus_write(&s.nes, 0x4015, 0x10);
    unsigned stalled_steps = 0;
    while (s.nes.apu.dmc_bytes_remaining && s.nes.cpu.cycle_count < 40000) {
        uint16_t pc = s.nes.cpu.program_counter;
        bool stalled = s.nes.cpu.stall_cycles != 0;
        test_step(&s, stalled ? 1 : 3);
        if (stalled) {
            assert(s.nes.cpu.program_counter == pc);
            stalled_steps++;
        }
    }
    assert(s.nes.apu.dmc_bytes_remaining == 0);
    assert(s.read_count == 65);
    for (unsigned i = 0; i < 64; i++) assert(s.reads[i] == 0xFFC0 + i);
    assert(s.reads[64] == 0x8000);
    assert(stalled_steps > 0);
    assert(s.nes.apu.dmc_irq_active);
    assert(s.nes.cpu.irq_lines & (1 << APU_IRQ_SOURCE_DMC));
    // The final sample byte is buffered: IRQ is not delayed until playback ends.
    assert(!s.nes.apu.dmc_buffer_empty);
    assert(s.nes.apu.dmc_buffer == s.prg[0]);
    assert(nes_cpu_bus_read(&s.nes, 0x4015) & 0x80);
    assert(s.nes.apu.dmc_irq_active); // Status reads only acknowledge frame IRQ.
    nes_cpu_bus_write(&s.nes, 0x4015, 0);
    assert(!s.nes.apu.dmc_irq_active);
    assert(!(s.nes.cpu.irq_lines & (1 << APU_IRQ_SOURCE_DMC)));
}

int main(void) {
    RUN_TEST(oam_dma_copies_and_clocks_both_alignments);
    RUN_TEST(nmi_during_oam_dma_is_serviced_after_transfer);
    RUN_TEST(dmc_fetches_mapped_memory_wraps_and_stalls_cpu);
    return 0;
}
