#include "save_fixture.h"
#include "nametable_view.h"
#include "save_state.h"

static uint32_t pixels[PPU_NAMETABLE_WIDTH * PPU_NAMETABLE_HEIGHT];
static uint32_t at(unsigned table, unsigned x, unsigned y) {
    return pixels[((table / 2) * 240 + y) * 512 + (table % 2) * 256 + x];
}
static void check_mirroring(NES *n, MirroringMode mode, const unsigned pages[4]) {
    n->cart->mirroring = mode;
    int active_mirroring = -1;
    assert(ppu_render_nametables(n, pixels, &active_mirroring));
    assert(active_mirroring == (int)mode);
    const uint32_t colors[] = {0xFF0000FC, 0xFF007800, 0xFFF8F8F8, 0xFF000000};
    for (unsigned table = 0; table < 4; ++table) {
        assert(at(table, 0, 0) == colors[pages[table]]);
        assert(at(table, 255, 239) == colors[pages[table]]);
    }
}
static void layout_and_pixels(const char *path) {
    fixture_rom(path, 0, false, true, 0);
    NES *n = fixture_load(path);
    memset(n->cart->chr_rom, 0, n->cart->chr_rom_size);
    /* Four tiles: color 1, 2, 3, and transparent. */
    memset(n->cart->chr_rom, 0xFF, 8);
    memset(n->cart->chr_rom + 16 + 8, 0xFF, 8);
    memset(n->cart->chr_rom + 32, 0xFF, 16);
    n->ppu.palette_ram[0] = 0x0F;
    n->ppu.palette_ram[1] = 0x01;
    n->ppu.palette_ram[2] = 0x09;
    n->ppu.palette_ram[3] = 0x20;
    for (unsigned page = 0; page < 4; ++page) {
        memset(n->ciram + page * 1024, page, 960);
        memset(n->ciram + page * 1024 + 960, 0, 64);
    }
    check_mirroring(n, MIRROR_VERTICAL, (unsigned[]){0,1,0,1});
    check_mirroring(n, MIRROR_HORIZONTAL, (unsigned[]){0,0,1,1});
    check_mirroring(n, MIRROR_FOUR_SCREEN, (unsigned[]){0,1,2,3});
    check_mirroring(n, MIRROR_ONE_SCREEN_LOW, (unsigned[]){0,0,0,0});
    check_mirroring(n, MIRROR_ONE_SCREEN_HIGH, (unsigned[]){1,1,1,1});
    /* Each 16x16 attribute quadrant selects a different palette. */
    n->cart->mirroring = MIRROR_VERTICAL;
    n->ciram[960] = 0xE4;
    n->ppu.palette_ram[5] = 0x09;
    n->ppu.palette_ram[9] = 0x20;
    n->ppu.palette_ram[13] = 0x00;
    assert(ppu_render_nametables(n, pixels, NULL));
    assert(at(0, 0, 0) == 0xFF0000FC);
    assert(at(0, 16, 0) == 0xFF007800);
    assert(at(0, 0, 16) == 0xFFF8F8F8);
    assert(at(0, 16, 16) == 0xFF7C7C7C);
    /* Pattern table selection, bit order, and universal background color. */
    n->ppu.ppu_ctrl = 0x10;
    n->cart->chr_rom[0x1000] = 0x80;
    n->cart->chr_rom[0x1008] = 0x40;
    assert(ppu_render_nametables(n, pixels, NULL));
    assert(at(0, 0, 0) == 0xFF0000FC);
    assert(at(0, 1, 0) == 0xFF007800);
    assert(at(0, 2, 0) == 0xFF000000);
    assert(at(0, 16, 17) == 0xFF000000);
    fixture_free(n);
    assert(remove(path) == 0);
}
static void mapper_isolation(const char *path, unsigned mapper_id) {
    fixture_rom(path, mapper_id, false, false, 0);
    NES *n = fixture_load(path);
    memset(n->ciram, 0xFD, sizeof(n->ciram));
    /* Force latch-changing reads and enable MMC5 extended attributes. */
    if (mapper_id == 9 || mapper_id == 10) {
        nes_cpu_bus_write(n, 0xB000, 1);
        nes_cpu_bus_write(n, 0xC000, 2);
        nes_ppu_bus_read(n, 0x0FE8);
    }
    if (mapper_id == 5) {
        nes_cpu_bus_write(n, 0x5104, 1);
        nes_cpu_bus_write(n, 0x5C00, 0xC1);
    }
    n->ppu.ppu_mask = 0x18;
    n->ppu.cycle = 280;
    NES *before = malloc(sizeof(*before));
    assert(before); *before = *n;
    Cartridge cart_before = *n->cart;
    size_t size = n->cart->vtable->state_size;
    void *mapper_before = malloc(size ? size : 1);
    assert(mapper_before);
    if (size) memcpy(mapper_before, n->cart->mapper_data, size);
    uint32_t chr_crc = state_crc32(n->cart->chr_rom, n->cart->chr_rom_size);
    int active_mirroring;
    assert(ppu_render_nametables(n, pixels, &active_mirroring));
    assert(!memcmp(before, n, sizeof(*n)));
    assert(!memcmp(&cart_before, n->cart, sizeof(cart_before)));
    if (size) assert(!memcmp(mapper_before, n->cart->mapper_data, size));
    assert(chr_crc == state_crc32(n->cart->chr_rom, n->cart->chr_rom_size));
    free(mapper_before); free(before);
    fixture_free(n);
    assert(remove(path) == 0);
}
static void mmc3_status_bar_split(const char *path) {
    fixture_rom(path, 4, false, false, 0);
    NES *n = fixture_load(path);
    NametableView *view = calloc(1, sizeof(*view));
    assert(view);
    memset(n->cart->chr_rom, 0, n->cart->chr_rom_size);
    /* Gameplay bank 0: blue. HUD bank 2: green, with the same tile ID. */
    memset(n->cart->chr_rom, 0xFF, 8);
    memset(n->cart->chr_rom + 2048 + 8, 0xFF, 8);
    n->ppu.palette_ram[1] = 0x01;
    n->ppu.palette_ram[2] = 0x09;
    n->ppu.ppu_ctrl = 0;
    nes_cpu_bus_write(n, 0x8000, 0);
    nes_cpu_bus_write(n, 0x8001, 0);
    n->ppu.scanline = 119;
    nametable_view_sample(view, n);
    assert(!view->valid && !view->sampled);
    n->ppu.scanline = 120;
    nametable_view_sample(view, n);
    assert(view->valid && view->sampled);
    assert(view->pixels[0] == 0xFF0000FC);
    assert(view->mirroring == MIRROR_VERTICAL);
    uint32_t gameplay_crc = state_crc32(view->pixels, sizeof(view->pixels));

    n->ppu.scanline = 200;
    nes_cpu_bus_write(n, 0x8001, 2);
    nes_cpu_bus_write(n, 0xA000, 1);
    n->ppu.palette_ram[1] = 0x20;
    nametable_view_sample(view, n);
    assert(state_crc32(view->pixels, sizeof(view->pixels)) == gameplay_crc);
    assert(view->mirroring == MIRROR_VERTICAL);
    /* Reproduce the old frame-end decode: it sees the HUD graphics. */
    n->ppu.scanline = 241;
    assert(ppu_render_nametables(n, pixels, NULL));
    assert(pixels[0] == 0xFF007800);
    assert(pixels[0] != view->pixels[0]);

    /* A new frame must wait through vblank and the top HUD region, while
       the previous capture remains available for paused presentation. */
    view->sampled = false;
    nametable_view_sample(view, n);
    n->ppu.scanline = 261; nametable_view_sample(view, n);
    n->ppu.scanline = 0; nametable_view_sample(view, n);
    assert(!view->sampled && view->pixels[0] == 0xFF0000FC);
    nes_cpu_bus_write(n, 0x8001, 0);
    n->ppu.scanline = 121; /* An instruction or DMA crossed the sample line. */
    nametable_view_sample(view, n);
    assert(view->valid && view->sampled && view->pixels[0] == 0xFFF8F8F8);
    assert(view->mirroring == MIRROR_HORIZONTAL);
    free(view);
    fixture_free(n);
    assert(remove(path) == 0);
}
static void fetch_tile_row(NES *n, unsigned scanline, unsigned table, unsigned tile_y) {
    n->ppu.scanline = (int)scanline;
    n->ppu.cycle = 1;
    n->ppu.v = (uint16_t)((table << 10) | (tile_y << 5));
    for (unsigned dot = 0; dot < 7; ++dot) ppu_step(n);
}
static void observed_hud_rows(const char *path) {
    fixture_rom(path, 4, false, false, 0);
    NES *n = fixture_load(path);
    NametableView *view = calloc(1, sizeof(*view));
    assert(view);
    n->nametable_view = view;
    memset(n->cart->chr_rom, 0, n->cart->chr_rom_size);
    memset(n->cart->chr_rom, 0xFF, 8); // Bank 0, tile 0 = color 1.
    memset(n->cart->chr_rom + 2048 + 8, 0xFF, 8); // Bank 2 = color 2.
    n->ppu.palette_ram[1] = 0x01;
    n->ppu.palette_ram[2] = 0x09;
    n->ppu.ppu_mask = 0x08;
    nes_cpu_bus_write(n, 0xA000, 1); // Horizontal aliases 0/1, 2/3.
    nametable_view_begin_frame(view);
    fetch_tile_row(n, 100, 2, 10);
    n->ppu.scanline = 120;
    nametable_view_sample(view, n);
    nes_cpu_bus_write(n, 0x8000, 0);
    nes_cpu_bus_write(n, 0x8001, 2); // Late HUD bank switch.
    fetch_tile_row(n, 216, 2, 27);
    assert(view->pixels[(240 + 216) * 512] == 0xFF0000FC); // Old snapshot is wrong here.
    nametable_view_finish_frame(view);
    assert(view->pixels[(240 + 80) * 512] == 0xFF0000FC); // Gameplay remains blue.
    assert(view->pixels[(240 + 216) * 512] == 0xFF007800); // HUD is green.
    assert(view->pixels[(240 + 216) * 512 + 256] == 0xFF007800); // Mirrored HUD.
    assert(view->pixels[(240 + 216) * 512 + 8] == 0xFF0000FC); // Unfetched tile uses snapshot.
    /* A later fetch through either alias wins, irrespective of table order. */
    n->ppu.palette_ram[2] = 0x20;
    fetch_tile_row(n, 220, 3, 27);
    n->ppu.palette_ram[2] = 0x00;
    fetch_tile_row(n, 221, 2, 27);
    nametable_view_finish_frame(view);
    assert(view->pixels[(240 + 216) * 512] == 0xFF7C7C7C);
    assert(view->pixels[(240 + 216) * 512 + 256] == 0xFF7C7C7C);
    /* Rendering disabled must not fabricate background observations. */
    unsigned sequence = view->sequence;
    n->ppu.ppu_mask = 0x10;
    fetch_tile_row(n, 222, 2, 27);
    assert(view->sequence == sequence);
    nametable_view_begin_frame(view);
    assert(!view->sampled && !view->sequence);
    n->ppu.scanline = 120;
    nametable_view_sample(view, n);
    nametable_view_finish_frame(view);
    assert(view->pixels[(240 + 80) * 512] == 0xFF7C7C7C); // No stale previous-frame fetch.
    n->nametable_view = NULL;
    free(view); fixture_free(n); assert(remove(path) == 0);
}
static void observer_does_not_change_emulation(const char *path) {
    fixture_rom(path, 4, false, false, 0);
    NES *a = fixture_load(path), *b = fixture_load(path);
    NametableView *view = calloc(1, sizeof(*view));
    assert(view);
    NES *machines[] = {a, b};
    for (unsigned i = 0; i < 2; ++i) {
        NES *n = machines[i];
        n->cpu.program_counter = 0x200;
        n->wram[0x200] = 0x4C; n->wram[0x201] = 0; n->wram[0x202] = 2;
        n->ppu.ppu_mask = 0x18;
        n->ppu.ppu_ctrl = 0x10;
        nes_cpu_bus_write(n, 0xC000, 5);
        nes_cpu_bus_write(n, 0xE001, 0);
    }
    a->nametable_view = view;
    nametable_view_begin_frame(view);
    while (!a->frame_ready) {
        nes_clock_tick(a); nes_clock_tick(b);
        nametable_view_sample(view, a);
    }
    nametable_view_finish_frame(view);
    assert(view->valid && view->sequence > 0);
    uint8_t *state_a, *state_b; size_t size_a, size_b;
    assert(nes_state_encode(a, &state_a, &size_a) == NES_STATE_OK);
    assert(nes_state_encode(b, &state_b, &size_b) == NES_STATE_OK);
    assert(size_a == size_b && !memcmp(state_a, state_b, size_a));
    free(state_a); free(state_b); free(view);
    fixture_free(a); fixture_free(b); assert(remove(path) == 0);
}
int main(void) {
    fixture_start("nametables");
    char path[512]; fixture_path(path, "fixture.nes");
    layout_and_pixels(path);
    mmc3_status_bar_split(path);
    observed_hud_rows(path);
    observer_does_not_change_emulation(path);
    const unsigned mappers[] = {0,1,2,3,4,5,7,9,10,11,19,23,24,26,34,64,66,69,71,78,118,206,227};
    for (unsigned i = 0; i < sizeof(mappers) / sizeof(mappers[0]); ++i)
        mapper_isolation(path, mappers[i]);
    assert(!ppu_render_nametables(NULL, pixels, NULL));
    assert(SAVE_RMDIR(fixture_dir) == 0);
    puts("Nametable layout, pixels, MMC3 status-bar capture and mapper isolation passed.");
}
