#include "nametable_view.h"
#include <string.h>

void nametable_view_begin_frame(NametableView *view) {
    view->sampled = false;
    view->sequence = 0;
    memset(view->fetched_order, 0, sizeof(view->fetched_order));
}

void nametable_view_record(NametableView *view, uint16_t v, const uint32_t pixels[8]) {
    unsigned tile_y = (v >> 5) & 31;
    if (tile_y >= 30) return; // Coarse Y 30/31 fetch attributes, not displayed tiles.
    unsigned table = (v >> 10) & 3;
    unsigned y = (table / 2) * 240 + tile_y * 8 + ((v >> 12) & 7);
    unsigned x = (table % 2) * 256 + (v & 31) * 8;
    unsigned offset = y * PPU_NAMETABLE_WIDTH + x;
    memcpy(view->fetched_pixels + offset, pixels, 8 * sizeof(*pixels));
    view->fetched_order[offset / 8] = ++view->sequence;
}

void nametable_view_finish_frame(NametableView *view) {
    if (!view->valid) return;
    unsigned pages[4];
    for (unsigned table = 0; table < 4; ++table)
        pages[table] = view->mirroring < 0 ? table :
            cartridge_default_remap_ciram((MirroringMode)view->mirroring,
                (uint16_t)(0x2000 + table * 0x400));
    /* Unfetched tiles keep the midpoint snapshot. Observed rows use their
       actual banks/attributes/palettes, including HUD splits. Mirror the most
       recent observation into every alias so duplicated tables stay identical. */
    for (unsigned table = 0; table < 4; ++table) {
        for (unsigned y = 0; y < 240; ++y) {
            for (unsigned x = 0; x < 32; ++x) {
                unsigned newest = 0, source = 0;
                for (unsigned alias = 0; alias < 4; ++alias) {
                    if (pages[alias] != pages[table]) continue;
                    unsigned row = ((alias / 2) * 240 + y) * 64 + (alias % 2) * 32 + x;
                    if (view->fetched_order[row] > newest) {
                        newest = view->fetched_order[row];
                        source = row * 8;
                    }
                }
                if (newest) {
                    unsigned dest = ((table / 2) * 240 + y) * 512 + (table % 2) * 256 + x * 8;
                    memcpy(view->pixels + dest, view->fetched_pixels + source, 8 * sizeof(uint32_t));
                }
            }
        }
    }
}

void nametable_view_sample(NametableView *view, const NES *nes) {
    /* CPU instructions/DMA can straddle a scanline boundary. Take the first
       opportunity at or after the midpoint, never vblank or pre-render. */
    if (view->sampled || !nes->cart || nes->ppu.scanline < 120 || nes->ppu.scanline >= 240) return;
    view->sampled = true;
    view->valid = ppu_render_nametables(nes, view->pixels, &view->mirroring);
}
