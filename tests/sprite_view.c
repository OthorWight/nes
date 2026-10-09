#include "save_fixture.h"
#include "save_state.h"
#include "sprite_view.h"

static SpriteView view;
static const uint32_t backdrop = 0xFF000000;
static const uint32_t background = 0xFF3CBCFC;
static const uint32_t red = 0xFFE45C10;
static const uint32_t green = 0xFF58D854;

static void setup(NES *n, unsigned count, bool tall) {
    PPU2C02 *p = &n->ppu;
    ppu_init(p);
    p->scanline = 99;
    p->cycle = 320;
    p->ppu_mask = 0x1E;
    p->ppu_ctrl = tall ? 0x20 : 0;
    memset(p->oam_ram, 0xFF, sizeof(p->oam_ram));
    for (unsigned i = 0; i < count; ++i) {
        p->oam_ram[i * 4] = 99;
        p->oam_ram[i * 4 + 1] = 0;
        p->oam_ram[i * 4 + 2] = 0;
        p->oam_ram[i * 4 + 3] = i < 8 ? 200 : 40;
    }
    memset(n->cart->chr_rom, 0, n->cart->chr_rom_size);
    n->cart->chr_rom[0] = 0x80;
    n->cart->chr_rom[8] = 0x01;
    p->palette_ram[0] = 0x0F;
    p->palette_ram[1] = 0x21;
    p->palette_ram[0x11] = 0x17;
    p->palette_ram[0x12] = 0x2A;
    p->palette_ram[0x15] = 0x2A;
    n->sprite_view = &view;
    sprite_view_reset(&view, n);
}

/* Isolate pixel selection after the extra-slot fetch. */
static uint32_t pixel(NES *n, int x, bool bg) {
    PPU2C02 *p = &n->ppu;
    p->scanline = 100; p->cycle = x + 1;
    p->bg_shifter_pattern_low = bg ? 0xFFFF : 0;
    p->bg_shifter_pattern_high = 0;
    p->bg_shifter_attrib_low = p->bg_shifter_attrib_high = 0;
    ppu_step(n);
    return view.pixels[100 * 256 + x];
}

static void limit_and_hidden_sprites(const char *path) {
    fixture_rom(path, 0, false, false, 0);
    NES *n = fixture_load(path);
    for (unsigned count = 0; count <= 64; ++count) {
        setup(n, count, false);
        sprite_view_prepare(&view, n);
        assert(view.count == (count > 8 ? count - 8 : 0));
        assert(pixel(n, 40, false) == (count > 8 ? red : backdrop));
        assert(n->ppu.screen_buffer[100 * 256 + 40] == backdrop);
        assert(!(n->ppu.ppu_status & 0x40));
    }
    for (unsigned tall = 0; tall < 2; ++tall) {
        setup(n, 64, tall != 0);
        n->ppu.scanline = 0;
        for (unsigned i = 0; i < 64; ++i) n->ppu.oam_ram[i * 4] = (uint8_t)(240 + i % 16);
        sprite_view_prepare(&view, n);
        assert(!view.count);
    }
    setup(n, 9, false);
    n->ppu.scanline = ppu_prerender_line(&n->ppu);
    sprite_view_prepare(&view, n); assert(!view.count);
    fixture_free(n);
}

static void priority_clipping_and_flips(const char *path) {
    NES *n = fixture_load(path);
    for (unsigned flip = 0; flip < 2; ++flip) {
        setup(n, 9, false);
        n->ppu.oam_ram[8 * 4 + 2] = flip ? 0x40 : 0;
        sprite_view_prepare(&view, n);
        for (int x = 39; x <= 48; ++x) {
            uint32_t expected = background;
            if (x == 40) expected = flip ? green : red;
            if (x == 47) expected = flip ? red : green;
            assert(pixel(n, x, true) == expected);
            assert(!(n->ppu.ppu_status & 0x40));
        }
    }
    setup(n, 10, false);
    n->ppu.oam_ram[8 * 4 + 2] = 0x20;
    n->ppu.oam_ram[9 * 4 + 2] = 1;
    sprite_view_prepare(&view, n);
    assert(pixel(n, 40, true) == background); // First opaque extra wins behind BG.
    assert(pixel(n, 40, false) == red);
    setup(n, 9, false);
    sprite_view_prepare(&view, n);
    n->ppu.scanline_sprite_count = 1;
    n->ppu.scanline_sprites[0] = (ScanlineSprite){0, 0x80, 0, 0x21, 1};
    assert(pixel(n, 40, true) == background); // Hardware sprite blocks extra even behind BG.
    n->ppu.scanline_sprites[0] = (ScanlineSprite){0, 0x80, 0, 1, 1};
    assert(pixel(n, 40, false) == green);
    for (unsigned mask = 0; mask < 32; ++mask) {
        setup(n, 9, false);
        n->ppu.oam_ram[8 * 4 + 3] = 7;
        sprite_view_prepare(&view, n);
        n->ppu.ppu_mask = (uint8_t)mask;
        bool visible = (mask & 0x14) == 0x14;
        assert(pixel(n, 7, false) == (visible ? red : backdrop));
    }
    setup(n, 9, false);
    n->ppu.oam_ram[8 * 4 + 3] = 252;
    n->cart->chr_rom[0] = 0xFF;
    sprite_view_prepare(&view, n);
    assert(pixel(n, 0, false) == backdrop);
    assert(pixel(n, 255, false) == red);
    fixture_free(n);
}

static void pattern_tables_and_vertical_flip(const char *path) {
    NES *n = fixture_load(path);
    for (unsigned tall = 0; tall < 2; ++tall) {
        for (unsigned bank = 0; bank < 2; ++bank) {
            for (unsigned flip = 0; flip < 2; ++flip) {
                setup(n, 9, tall != 0);
                if (!tall && bank) n->ppu.ppu_ctrl |= 8;
                n->ppu.oam_ram[8 * 4 + 1] = (uint8_t)(2 + (tall ? bank : 0));
                n->ppu.oam_ram[8 * 4 + 2] = flip ? 0x80 : 0;
                n->ppu.oam_ram[8 * 4] = 96; // Unflipped row 3.
                unsigned row = flip ? (tall ? 12 : 4) : 3;
                unsigned addr = bank * 4096 + 32 + (row & 7) + ((row & 8) << 1);
                n->cart->chr_rom[addr] = 0x80;
                sprite_view_prepare(&view, n);
                assert(view.count == 1);
                assert(pixel(n, 40, false) == red);
            }
        }
    }
    fixture_free(n);
}

static void ninth_sprite_through_real_evaluation(const char *path) {
    for (unsigned enabled = 0; enabled < 2; ++enabled) {
        NES *n = fixture_load(path);
        setup(n, 9, false);
        n->sprite_view = enabled ? &view : NULL;
        n->ppu.cycle = 1;
        n->ppu.ppu_mask = 0x14;
        unsigned dots = 0;
        while ((n->ppu.scanline != 100 || n->ppu.cycle != 42) && dots++ < 400)
            ppu_step(n);
        assert(dots < 400 && n->ppu.scanline_sprite_count == 8);
        assert(n->ppu.ppu_status & 0x20); // Overflow still reports the hardware limit.
        assert(!(n->ppu.ppu_status & 0x40));
        assert(n->ppu.screen_buffer[100 * 256 + 40] == backdrop);
        if (enabled) assert(view.pixels[100 * 256 + 40] == red);
        fixture_free(n);
    }
}

static void mapper_and_state_isolation(const char *path, unsigned mapper) {
    fixture_rom(path, mapper, false, false, 0);
    NES *a = fixture_load(path), *b = fixture_load(path);
    NES *machines[] = {a, b};
    for (unsigned i = 0; i < 2; ++i) {
        NES *n = machines[i];
        n->cpu.program_counter = 0x200;
        n->wram[0x200] = 0x4C; n->wram[0x201] = 0; n->wram[0x202] = 2;
        n->ppu.ppu_mask = 0x1E;
        for (unsigned j = 0; j < 64; ++j) {
            n->ppu.oam_ram[j * 4] = 99;
            n->ppu.oam_ram[j * 4 + 1] = j >= 62 ? (uint8_t)(0xFD + j - 62) : (uint8_t)(j * 4);
            n->ppu.oam_ram[j * 4 + 2] = 0;
            n->ppu.oam_ram[j * 4 + 3] = (uint8_t)(j * 4);
        }
    }
    a->sprite_view = &view;
    sprite_view_reset(&view, a);
    bool fetched_extra = false;
    unsigned ticks = 0;
    while (!a->frame_ready && ticks++ < 40000) {
        nes_clock_tick(a); nes_clock_tick(b);
        if (view.count) fetched_extra = true;
    }
    assert(a->frame_ready && b->frame_ready && fetched_extra);
    uint8_t *state_a, *state_b; size_t size_a, size_b;
    assert(nes_state_encode(a, &state_a, &size_a) == NES_STATE_OK);
    assert(nes_state_encode(b, &state_b, &size_b) == NES_STATE_OK);
    assert(size_a == size_b && !memcmp(state_a, state_b, size_a));
    // Restoring retains the user's option and removes stale display data.
    memset(view.pixels, 0xFF, sizeof(view.pixels)); view.count = 56;
    assert(nes_state_decode(a, state_a, size_a) == NES_STATE_OK);
    assert(a->sprite_view == &view && !view.count && view.scanline == -1);
    assert(!memcmp(view.pixels, a->ppu.screen_buffer, sizeof(view.pixels)));
    sprite_view_prepare(&view, a);
    nes_reset(a);
    assert(a->sprite_view == &view && !view.count);
    free(state_a); free(state_b);
    fixture_free(a); fixture_free(b);
}

int main(void) {
    fixture_start("sprite-view");
    char path[512]; fixture_path(path, "fixture.nes");
    limit_and_hidden_sprites(path);
    priority_clipping_and_flips(path);
    pattern_tables_and_vertical_flip(path);
    ninth_sprite_through_real_evaluation(path);
    const unsigned mappers[] = {0,1,2,3,4,5,7,9,10,11,19,23,24,26,34,64,66,69,71,78,85,118,206,227};
    for (unsigned i = 0; i < sizeof(mappers) / sizeof(mappers[0]); ++i)
        mapper_and_state_isolation(path, mappers[i]);
    assert(!remove(path)); assert(SAVE_RMDIR(fixture_dir) == 0);
    puts("Extra sprites, priority, clipping, flips and all-mapper hardware-state isolation passed.");
}
