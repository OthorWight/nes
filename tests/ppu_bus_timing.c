#include "test_system.h"

static TestSystem s;

static void address_and_data_phases_can_form_a_hybrid_address(void) {
    test_system_init(&s);
    PPU2C02 *p = &s.nes.ppu;
    p->ppu_mask = 0x08;
    p->cycle = 1;
    p->v = 0x2001;
    s.nes.ciram[1] = 0xAA;
    s.nes.ciram[0x301] = 0xBB;
    ppu_step(&s.nes); // ALE captures the low address byte.
    p->v = 0x2300; // Upper address pins change before /RD.
    ppu_step(&s.nes);
    assert(p->bg_next_tile_id == 0xBB);
    assert(p->bus_address == 0x2301);
}

static void ppudata_refill_is_delayed_and_preserves_the_old_buffer(void) {
    test_system_init(&s);
    PPU2C02 *p = &s.nes.ppu;
    p->scanline = 241;
    p->cycle = 10;
    p->v = 0x2000;
    p->buffered_data = 0xA5;
    s.nes.ciram[0] = 0x3C;
    assert(ppu_read_reg_timed(&s.nes, 0x2007) == 0xA5);
    for (unsigned i = 0; i < 5; ++i) {
        ppu_step(&s.nes);
        assert(p->buffered_data == 0xA5);
    }
    ppu_step(&s.nes); // Four dots after M2 falls.
    assert(p->buffered_data == 0x3C);
    assert(p->v == 0x2001);
    assert(ppu_read_reg_timed(&s.nes, 0x2007) == 0x3C);
}

static void simultaneous_ale_and_cpu_read_corrupt_the_pattern_fetch(void) {
    test_system_init(&s);
    PPU2C02 *p = &s.nes.ppu;
    p->cycle = 224;
    p->scanline = 3;
    p->ppu_mask = 0x08;
    p->v = 0x3C1E;
    memset(s.nes.ciram, 0xF0, sizeof(s.nes.ciram));
    s.nes.ciram[0x7C7] = 0xFF;
    s.chr[0x0FFF] = 0xFF;
    (void)ppu_read_reg_timed(&s.nes, 0x2007);
    for (unsigned i = 0; i < 6; ++i) ppu_step(&s.nes);
    assert(p->buffered_data == 0xFF);
    ppu_step(&s.nes);
    assert(p->bg_next_tile_lsb == 0xFF);
    assert(p->bus_address == 0x0FFF);
}

static void mask_and_address_writes_propagate_after_the_cpu_write(void) {
    test_system_init(&s);
    PPU2C02 *p = &s.nes.ppu;
    p->scanline = 241;
    ppu_write_reg_timed(&s.nes, 0x2001, 0x18);
    ppu_write_reg_timed(&s.nes, 0x2006, 0x23);
    ppu_write_reg_timed(&s.nes, 0x2006, 0x45);
    assert(!p->ppu_mask && !p->v);
    for (unsigned i = 0; i < 2; ++i) {
        ppu_step(&s.nes);
        assert(!p->ppu_mask && !p->v);
    }
    ppu_step(&s.nes);
    assert(p->ppu_mask == 0x18 && p->v == 0x2345);
}

static void sprite_counter_runs_while_pattern_shifters_are_blank(void) {
    test_system_init(&s);
    PPU2C02 *p = &s.nes.ppu;
    p->cycle = 10;
    p->scanline_sprite_count = 1;
    p->scanline_sprites[0] = (ScanlineSprite){2, 0x80, 0, 0, 0};
    p->sprite_counter_active = 1;
    p->palette_ram[0] = 0x0F;
    p->palette_ram[0x11] = 0x17;
    ppu_step(&s.nes);
    ppu_step(&s.nes);
    assert(p->scanline_sprites[0].x == 0);
    assert(p->scanline_sprites[0].low_byte == 0x80);
    p->ppu_mask = 0x14;
    ppu_step(&s.nes);
    assert(p->screen_buffer[11] == 0xFFE45C10);
    assert(p->scanline_sprites[0].low_byte == 0);
}

static void palette_reads_apply_grayscale_without_masking_open_bus(void) {
    test_system_init(&s);
    PPU2C02 *p = &s.nes.ppu;
    p->v = 0x3F01;
    p->palette_ram[1] = 0x2D;
    p->open_bus_value = 0xC0;
    p->ppu_mask = 1;
    assert(ppu_read_reg_timed(&s.nes, 0x2007) == 0xE0);
}

static void secondary_oam_wrap_keeps_the_final_x_byte_on_the_bus(void) {
    static const uint8_t first_eight[] = {
        0x80,0,0,0xFF, 0x7F,1,0x20,0xEE, 0x7E,2,0x40,0xDD, 0x7D,3,0x60,0xCC,
        0x7C,4,0x80,0xBB, 0x7B,5,0xA0,0xAA, 0x7A,6,0xC0,0x99, 0x79,7,0xE0,0x88
    };
    test_system_init(&s);
    PPU2C02 *p = &s.nes.ppu;
    p->scanline = 128;
    p->ppu_mask = 0x18;
    memcpy(p->oam_ram, first_eight, sizeof(first_eight));
    for (unsigned i = 32; i < 256; ++i) p->oam_ram[i] = (uint8_t)(i - 32);
    while (p->cycle <= 128) ppu_step(&s.nes);
    assert(p->oam_eval.secondary_full);
    assert(p->oam_eval.secondary_index == 0);
    assert(ppu_read_reg(&s.nes, 0x2004) == 0x88);
    while (p->cycle < 199) ppu_step(&s.nes);
    assert(p->oam_eval.done);
    assert(p->oam_addr == 0xA0); // Overflow's four-byte fetch realigns OAMADDR.
    ppu_step(&s.nes);
    assert(ppu_read_reg(&s.nes, 0x2004) == 0x80);
    ppu_step(&s.nes);
    ppu_step(&s.nes);
    assert(ppu_read_reg(&s.nes, 0x2004) == 0x84);
}

static void ppudata_increment_drives_a12_during_forced_blank(void) {
    test_system_init(&s);
    PPU2C02 *p = &s.nes.ppu;
    p->scanline = 241;
    p->v = 0x0FFF;
    (void)ppu_read_reg_timed(&s.nes, 0x2007);
    for (unsigned i = 0; i < 6; ++i) ppu_step(&s.nes);
    assert(p->v == 0x1000);
    ppu_step(&s.nes);
    assert(p->bus_address & 0x1000);
}

int main(void) {
    RUN_TEST(address_and_data_phases_can_form_a_hybrid_address);
    RUN_TEST(ppudata_refill_is_delayed_and_preserves_the_old_buffer);
    RUN_TEST(simultaneous_ale_and_cpu_read_corrupt_the_pattern_fetch);
    RUN_TEST(mask_and_address_writes_propagate_after_the_cpu_write);
    RUN_TEST(sprite_counter_runs_while_pattern_shifters_are_blank);
    RUN_TEST(palette_reads_apply_grayscale_without_masking_open_bus);
    RUN_TEST(secondary_oam_wrap_keeps_the_final_x_byte_on_the_bus);
    RUN_TEST(ppudata_increment_drives_a12_during_forced_blank);
    return 0;
}
