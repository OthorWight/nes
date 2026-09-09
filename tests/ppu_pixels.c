#include "test_system.h"

// Pixel selection and hit expectations:
// https://www.nesdev.org/wiki/PPU_rendering#Drawing_overview
// https://www.nesdev.org/wiki/PPU_sprite_priority
// https://www.nesdev.org/wiki/PPU_programmer_reference#Sprite_0_hit_flag
// Seed the fetched pattern bytes to isolate composition from sprite evaluation.
static TestSystem s;
static const uint32_t backdrop = 0xFF000000;
static const uint32_t background = 0xFF3CBCFC;
static const uint32_t sprite_red = 0xFFE45C10;
static const uint32_t sprite_green = 0xFF58D854;

static PPU2C02 *pixel_setup(int pixel_x, bool opaque_background) {
    test_system_init(&s);
    PPU2C02 *p = &s.nes.ppu;
    p->scanline = 100;
    p->cycle = pixel_x + 1;
    p->ppu_mask = 0x1E;
    memset(p->oam_ram, 0xFF, sizeof(p->oam_ram));
    p->bg_shifter_pattern_low = opaque_background ? 0xFFFF : 0;
    p->palette_ram[0] = 0x0F;
    p->palette_ram[1] = 0x21;
    p->palette_ram[0x11] = 0x17;
    p->palette_ram[0x12] = 0x2A;
    p->palette_ram[0x15] = 0x2A;
    return p;
}

static uint32_t draw_pixel(void) {
    int index = s.nes.ppu.scanline * 256 + s.nes.ppu.cycle - 1;
    ppu_step(&s.nes);
    return s.nes.ppu.screen_buffer[index];
}

static void transparency_and_background_priority(void) {
    for (unsigned bg = 0; bg < 2; ++bg) {
        for (unsigned sp = 0; sp < 2; ++sp) {
            for (unsigned behind = 0; behind < 2; ++behind) {
                PPU2C02 *p = pixel_setup(40, bg != 0);
                p->scanline_sprite_count = 1;
                p->scanline_sprites[0] = (ScanlineSprite){40, sp ? 0xFF : 0, 0, behind ? 0x20 : 0, 0};
                uint32_t expected = bg ? background : backdrop;
                if (sp && (!bg || !behind)) expected = sprite_red;
                assert(draw_pixel() == expected);
                assert(!!(p->ppu_status & 0x40) == !!(bg && sp));
            }
        }
    }
}

static void first_opaque_sprite_wins_even_behind_background(void) {
    for (unsigned bg = 0; bg < 2; ++bg) {
        for (unsigned first_opaque = 0; first_opaque < 2; ++first_opaque) {
            PPU2C02 *p = pixel_setup(40, bg != 0);
            p->scanline_sprite_count = 2;
            p->scanline_sprites[0] = (ScanlineSprite){40, first_opaque ? 0xFF : 0, 0, 0x20, 0};
            p->scanline_sprites[1] = (ScanlineSprite){40, 0xFF, 0, 1, 1};
            uint32_t expected = first_opaque ? (bg ? background : sprite_red) : sprite_green;
            assert(draw_pixel() == expected);
            assert(!!(p->ppu_status & 0x40) == !!(bg && first_opaque));
        }
    }
}

static void sprite_bounds_and_horizontal_flip(void) {
    for (unsigned flip = 0; flip < 2; ++flip) {
        for (int x = 38; x < 50; ++x) {
            PPU2C02 *p = pixel_setup(x, true);
            p->scanline_sprite_count = 1;
            p->scanline_sprites[0] = (ScanlineSprite){40, 0x80, 0x01, flip ? 0x40 : 0, 0};
            uint32_t expected = background;
            if (x == 40) expected = flip ? sprite_green : sprite_red;
            if (x == 47) expected = flip ? sprite_red : sprite_green;
            assert(draw_pixel() == expected);
            assert(!!(p->ppu_status & 0x40) == (x == 40 || x == 47));
        }
    }
    // A sprite reaching the right edge does not wrap back onto the left edge.
    PPU2C02 *p = pixel_setup(0, true);
    p->scanline_sprite_count = 1;
    p->scanline_sprites[0] = (ScanlineSprite){252, 0xFF, 0, 0, 0};
    assert(draw_pixel() == background);
    assert(!(p->ppu_status & 0x40));
}

static void sprite_zero_clipping_and_right_edge(void) {
    static const int positions[] = {0, 7, 8, 254, 255};
    for (unsigned i = 0; i < sizeof(positions) / sizeof(positions[0]); ++i) {
        int x = positions[i];
        for (unsigned enables = 0; enables < 4; ++enables) {
            for (unsigned clip = 0; clip < 4; ++clip) {
                PPU2C02 *p = pixel_setup(x, true);
                p->ppu_mask = (uint8_t)((enables << 3) | (clip << 1));
                p->scanline_sprite_count = 1;
                p->scanline_sprites[0] = (ScanlineSprite){(uint8_t)x, 0xFF, 0, 0x20, 0};
                (void)draw_pixel();
                bool hit = enables == 3 && x != 255 && (x >= 8 || clip == 3);
                assert(!!(p->ppu_status & 0x40) == hit);
            }
        }
    }
    // Left-clipped sprite pixels cannot replace a visible background pixel.
    PPU2C02 *p = pixel_setup(7, true);
    p->ppu_mask = 0x1A;
    p->scanline_sprite_count = 1;
    p->scanline_sprites[0] = (ScanlineSprite){7, 0xFF, 0, 0, 0};
    assert(draw_pixel() == background);
    // A hit remains set until the pre-render clear, even on transparent pixels.
    p = pixel_setup(40, false);
    p->ppu_status = 0x40;
    assert(draw_pixel() == backdrop);
    assert(p->ppu_status & 0x40);
}

static void forced_blank_palette_and_mask_changes(void) {
    PPU2C02 *p = pixel_setup(40, true);
    p->ppu_mask = 0;
    p->v = 0x3F11;
    assert(draw_pixel() == sprite_red);
    p->v = 0x3F10;
    assert(draw_pixel() == backdrop);
    p->ppu_mask = 0x1E;
    assert(draw_pixel() == background);
    p->ppu_mask = 0;
    p->v = 0x2000;
    assert(draw_pixel() == backdrop);
}

int main(void) {
    RUN_TEST(transparency_and_background_priority);
    RUN_TEST(first_opaque_sprite_wins_even_behind_background);
    RUN_TEST(sprite_bounds_and_horizontal_flip);
    RUN_TEST(sprite_zero_clipping_and_right_edge);
    RUN_TEST(forced_blank_palette_and_mask_changes);
    return 0;
}
