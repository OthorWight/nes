#include "test_system.h"

static TestSystem s;

static void ntsc_frame_lengths_and_odd_frame_skip(void) {
    const uint8_t masks[] = {0, 0x08, 0x10, 0x18};
    for (unsigned mode = 0; mode < sizeof(masks); mode++) {
        test_system_init(&s);
        nes_cpu_bus_write(&s.nes, 0x2001, masks[mode]);
        for (unsigned frame = 0; frame < 4; frame++) {
            assert(s.nes.ppu.odd_frame == ((frame & 1) != 0));
            unsigned ticks = 0;
            do {
                ppu_step(&s.nes);
                ticks++;
                assert(ticks <= 89342);
            } while (s.nes.ppu.scanline != 0 || s.nes.ppu.cycle != 0);
            assert(ticks == 89342u - ((masks[mode] && (frame & 1)) ? 1u : 0u));
        }
        assert(s.nes.cpu.cycle_count == 0); // PPU stepping must not execute CPU code.
    }
}

static void vblank_and_prerender_flag_edges(void) {
    test_system_init(&s);
    nes_cpu_bus_write(&s.nes, 0x2000, 0x80);
    s.nes.ppu.scanline = 241;
    s.nes.ppu.cycle = 0;
    ppu_step(&s.nes); // Dot 0 has not asserted vblank.
    assert(!(s.nes.ppu.ppu_status & 0x80));
    assert(!s.nes.cpu.nmi_line);
    ppu_step(&s.nes); // Dot 1 asserts vblank and the NMI input.
    assert(s.nes.ppu.ppu_status & 0x80);
    assert(s.nes.cpu.nmi_line && s.nes.cpu.nmi_edge);
    s.nes.ppu.ppu_status |= 0x60;
    s.nes.ppu.scanline = 261;
    s.nes.ppu.cycle = 0;
    ppu_step(&s.nes);
    assert((s.nes.ppu.ppu_status & 0xE0) == 0xE0);
    ppu_step(&s.nes);
    assert(!(s.nes.ppu.ppu_status & 0xE0));
    assert(!s.nes.cpu.nmi_line);
}

// ppu_vbl_nmi/10-even_odd_timing checks late PPUMASK changes. A write
// before dot 338 is processed affects the skip; one after it is too late.
static void rendering_toggle_at_odd_frame_skip_boundary(void) {
    const uint8_t masks[] = {0x08, 0x10, 0x18};
    for (unsigned odd = 0; odd < 2; ++odd) {
        for (unsigned mode = 0; mode < sizeof(masks); ++mode) {
            for (unsigned enable = 0; enable < 2; ++enable) {
                for (int write_at = 336; write_at <= 340; ++write_at) {
                    test_system_init(&s);
                    nes_cpu_bus_write(&s.nes, 0x2001, enable ? 0 : masks[mode]);
                    s.nes.ppu.scanline = 261;
                    s.nes.ppu.odd_frame = odd != 0;
                    s.prg[0] = 0x8D; s.prg[1] = 1; s.prg[2] = 0x20;
                    s.nes.cpu.accumulator = enable ? masks[mode] : 0;
                    for (int dot = 0; dot < write_at - 12; ++dot) ppu_step(&s.nes);
                    test_step(&s, 4); // STA $2001 samples after 12 PPU dots.
                    for (int dot = 0; dot < 5 && s.nes.ppu.scanline == 261; ++dot)
                        ppu_step(&s.nes);
                    bool skip = odd && (write_at <= 338 ? enable : !enable);
                    assert(s.ppu_ticks == (skip ? 340u : 341u));
                    assert(s.nes.ppu.scanline == 0 && s.nes.ppu.cycle == 0);
                }
            }
        }
    }
}

static void status_read_acknowledges_vblank_and_resets_write_latch(void) {
    test_system_init(&s);
    nes_cpu_bus_write(&s.nes, 0x2000, 0x80);
    s.nes.ppu.scanline = 241;
    s.nes.ppu.cycle = 0;
    for (int i = 0; i < 10; i++) ppu_step(&s.nes); // Away from the NMI race window.
    nes_cpu_bus_write(&s.nes, 0x2005, 0x17);
    assert(s.nes.ppu.w == 1);
    assert(nes_cpu_bus_read(&s.nes, 0x3FFA) & 0x80); // Mirror of $2002.
    assert(s.nes.ppu.w == 0);
    assert(!s.nes.cpu.nmi_line);
    assert(!(nes_cpu_bus_read(&s.nes, 0x2002) & 0x80));
}

int main(void) {
    RUN_TEST(ntsc_frame_lengths_and_odd_frame_skip);
    RUN_TEST(vblank_and_prerender_flag_edges);
    RUN_TEST(rendering_toggle_at_odd_frame_skip_boundary);
    RUN_TEST(status_read_acknowledges_vblank_and_resets_write_latch);
    return 0;
}
