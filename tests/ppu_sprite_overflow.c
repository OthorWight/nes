#include "test_system.h"

// Hardware cases from sprite_overflow_tests/source/2.Details.a:
// Y=239 can set overflow; Y=240..255 must remain offscreen, with no wrap.
// https://github.com/christopherpow/nes-test-roms/tree/master/sprite_overflow_tests
static TestSystem s;

static void hidden_sprites_do_not_overflow(void) {
    const uint8_t masks[] = {0x08, 0x10, 0x18};
    for (unsigned tall = 0; tall < 2; ++tall) {
        for (unsigned mode = 0; mode < sizeof(masks); ++mode) {
            for (unsigned y = 240; y <= 255; ++y) {
                test_system_init(&s);
                memset(s.nes.wram + 0x200, 0xFF, 256);
                for (unsigned sprite = 0; sprite < 64; ++sprite)
                    s.nes.wram[0x200 + sprite * 4] = (uint8_t)y;
                s.prg[0] = 0x8D; s.prg[1] = 0x14; s.prg[2] = 0x40;
                s.nes.cpu.accumulator = 2; // STA $4014: load OAM via DMA.
                nes_clock_tick(&s.nes);
                nes_cpu_bus_write(&s.nes, 0x2000, tall ? 0x20 : 0);
                nes_cpu_bus_write(&s.nes, 0x2001, masks[mode]);
                // A full frame from any starting dot covers every scanline.
                for (unsigned dot = 0; dot < 89342; ++dot) {
                    ppu_step(&s.nes);
                    assert(!(s.nes.ppu.ppu_status & 0x20));
                }
                assert(!(nes_cpu_bus_read(&s.nes, 0x2002) & 0x20));
            }
        }
    }
}

static void visible_sprite_range_and_overflow_threshold(void) {
    const int positions[] = {0, 128, 239};
    for (unsigned tall = 0; tall < 2; ++tall) {
        int height = tall ? 16 : 8;
        const int rows[] = {-1, 0, height - 1, height};
        for (unsigned pos = 0; pos < sizeof(positions) / sizeof(positions[0]); ++pos) {
            for (unsigned row = 0; row < sizeof(rows) / sizeof(rows[0]); ++row) {
                int scanline = positions[pos] + rows[row];
                if (scanline < 0 || scanline >= 240) continue;
                for (unsigned count = 8; count <= 9; ++count) {
                    test_system_init(&s);
                    memset(s.nes.ppu.oam_ram, 0xFF, 256);
                    for (unsigned sprite = 0; sprite < count; ++sprite)
                        s.nes.ppu.oam_ram[sprite * 4] = (uint8_t)positions[pos];
                    nes_cpu_bus_write(&s.nes, 0x2000, tall ? 0x20 : 0);
                    nes_cpu_bus_write(&s.nes, 0x2001, 0x18);
                    s.nes.ppu.scanline = scanline;
                    for (unsigned dot = 0; dot < 257; ++dot) ppu_step(&s.nes);
                    bool overflow = count == 9 && rows[row] >= 0 && rows[row] < height;
                    assert(!!(nes_cpu_bus_read(&s.nes, 0x2002) & 0x20) == overflow);
                    // PPUSTATUS reads do not acknowledge sprite overflow.
                    assert(!!(nes_cpu_bus_read(&s.nes, 0x2002) & 0x20) == overflow);
                }
            }
        }
    }
}

int main(void) {
    RUN_TEST(hidden_sprites_do_not_overflow);
    RUN_TEST(visible_sprite_range_and_overflow_threshold);
    return 0;
}
