#include "test_system.h"

static TestSystem s;

static void instructions_clock_all_devices(void) {
    test_system_init(&s);
    const uint8_t program[] = {0xEA, 0xA9, 0x42, 0x85, 0x10, 0xAD, 0x10, 0x00};
    memcpy(s.prg, program, sizeof(program));
    test_step(&s, 2); // NOP (includes its dummy read).
    test_step(&s, 2); // LDA #$42.
    test_step(&s, 3); // STA $10.
    test_step(&s, 4); // LDA $0010.
    assert(s.nes.cpu.accumulator == 0x42);
    assert(s.nes.wram[0x10] == 0x42);
    assert(s.nes.apu.frame_cycles == 11);
    assert(s.nes.ppu.scanline == 0 && s.nes.ppu.cycle == 33);
}

static void indexed_reads_and_branches_clock_penalties(void) {
    for (int crossing = 0; crossing < 2; crossing++) {
        test_system_init(&s);
        s.nes.cpu.index_x = 1;
        s.prg[0] = 0xBD; // LDA $90FE,X or $90FF,X.
        s.prg[1] = crossing ? 0xFF : 0xFE;
        s.prg[2] = 0x90;
        s.prg[crossing ? 0x1100 : 0x10FF] = 0x37;
        test_step(&s, crossing ? 5 : 4);
        assert(s.nes.cpu.accumulator == 0x37);
        assert(s.nes.apu.frame_cycles == (unsigned)(crossing ? 5 : 4));
    }
    for (int mode = 0; mode < 3; mode++) {
        test_system_init(&s);
        unsigned offset = mode == 2 ? 0xFD : 0;
        s.nes.cpu.program_counter = 0x8000 + offset;
        s.prg[offset] = 0xD0; s.prg[offset + 1] = 2; // BNE +2.
        if (mode == 0) s.nes.cpu.status_flags |= FLAG_ZERO;
        test_step(&s, 2 + mode); // Not taken, taken, taken across a page.
        assert(s.nes.cpu.program_counter == 0x8000 + offset + (mode ? 4 : 2));
        assert(s.nes.apu.frame_cycles == (unsigned)(2 + mode));
    }
}

static void read_modify_write_emits_both_bus_writes(void) {
    test_system_init(&s);
    s.prg[0] = 0xEE; s.prg[1] = 0x00; s.prg[2] = 0x90; // INC $9000.
    s.prg[0x1000] = 0x7F;
    test_step(&s, 6);
    assert(s.write_count == 2);
    assert(s.writes[0] == 0x9000 && s.writes[1] == 0x9000);
    assert(s.write_values[0] == 0x7F && s.write_values[1] == 0x80);
    assert(s.write_cycles[0] == 5 && s.write_cycles[1] == 6);
    assert(s.nes.apu.frame_cycles == 6);
}

static void reset_clocks_devices_without_writing_stack(void) {
    test_system_init(&s);
    memset(s.nes.wram + 0x100, 0x5A, 256);
    nes_reset(&s.nes);
    test_step(&s, 7);
    assert(s.nes.cpu.program_counter == 0x8000);
    assert(s.nes.cpu.stack_pointer == 0xFA);
    for (int i = 0x100; i < 0x200; i++) assert(s.nes.wram[i] == 0x5A);
    assert(s.nes.apu.frame_cycles == 7);
}

int main(void) {
    RUN_TEST(instructions_clock_all_devices);
    RUN_TEST(indexed_reads_and_branches_clock_penalties);
    RUN_TEST(read_modify_write_emits_both_bus_writes);
    RUN_TEST(reset_clocks_devices_without_writing_stack);
    return 0;
}
