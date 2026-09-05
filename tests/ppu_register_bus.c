#include "test_system.h"

static TestSystem s;

static void cpu_write(uint16_t addr, uint8_t value) {
    s.prg[0] = 0x8D; s.prg[1] = (uint8_t)addr; s.prg[2] = addr >> 8;
    s.nes.cpu.program_counter = 0x8000;
    s.nes.cpu.accumulator = value;
    test_step(&s, 4);
}

static uint8_t cpu_read(uint16_t addr) {
    s.prg[0] = 0xAD; s.prg[1] = (uint8_t)addr; s.prg[2] = addr >> 8;
    s.nes.cpu.program_counter = 0x8000;
    test_step(&s, 4);
    return s.nes.cpu.accumulator;
}

static void ppu_address(uint16_t addr) {
    cpu_write(0x2006, (uint8_t)(addr >> 8));
    cpu_write(0x2006, (uint8_t)addr);
}

static void cpu_ram_and_ppu_register_mirrors(void) {
    test_system_init(&s);
    cpu_write(0x0012, 0xA5);
    assert(cpu_read(0x0812) == 0xA5);
    assert(cpu_read(0x1012) == 0xA5);
    assert(cpu_read(0x1812) == 0xA5);
    cpu_write(0x3FF8, 0x04); // Last mirror of PPUCTRL.
    assert(s.nes.ppu.ppu_ctrl == 0x04);
    cpu_write(0x2005, 0x13);
    assert(s.nes.ppu.w == 1 && s.nes.ppu.x == 3);
    (void)cpu_read(0x3FFA); // PPUSTATUS clears the shared address/scroll latch.
    assert(s.nes.ppu.w == 0);
    ppu_address(0x2000);
    cpu_write(0x3FFF, 0x42); // PPUDATA mirror, increment by 32.
    assert(nes_ppu_bus_read(&s.nes, 0x2000) == 0x42);
    assert(nes_ppu_bus_read(&s.nes, 0x2800) == 0x42); // Vertical CIRAM mirror.
    assert(s.nes.ppu.v == 0x2020);
}

static void ppudata_buffer_and_palette_bypass(void) {
    test_system_init(&s);
    ppu_address(0x2000);
    cpu_write(0x2007, 0x21);
    cpu_write(0x2007, 0x32);
    assert(s.nes.ppu.v == 0x2002);
    ppu_address(0x2000);
    s.nes.ppu.buffered_data = 0x55;
    assert(cpu_read(0x2007) == 0x55);
    assert(cpu_read(0x2007) == 0x21);
    assert(cpu_read(0x2007) == 0x32);

    nes_ppu_bus_write(&s.nes, 0x2F00, 0x19);
    ppu_address(0x3F10);
    cpu_write(0x2007, 0x2A); // Universal background palette mirror.
    assert(nes_ppu_bus_read(&s.nes, 0x3F00) == 0x2A);
    ppu_address(0x3F00);
    assert((cpu_read(0x2007) & 0x3F) == 0x2A); // No delayed palette read.
    ppu_address(0x2000);
    assert(cpu_read(0x2007) == 0x19); // Palette read refilled buffer from $2F00.
}

static void status_is_sampled_on_the_cpu_read_cycle(void) {
    test_system_init(&s);
    s.nes.ppu.scanline = 240;
    s.nes.ppu.cycle = 329;
    // A four-cycle LDA advances 12 dots before its data read. The first ends
    // just before vblank; the second must see vblank raised during execution.
    assert(!(cpu_read(0x2002) & 0x80));
    assert(s.nes.ppu.scanline == 241 && s.nes.ppu.cycle == 0);
    assert(cpu_read(0x2002) & 0x80);
    assert(s.nes.ppu.scanline == 241 && s.nes.ppu.cycle == 12);
    assert(!(s.nes.ppu.ppu_status & 0x80));
}

int main(void) {
    RUN_TEST(cpu_ram_and_ppu_register_mirrors);
    RUN_TEST(ppudata_buffer_and_palette_bypass);
    RUN_TEST(status_is_sampled_on_the_cpu_read_cycle);
    return 0;
}
