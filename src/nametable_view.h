#ifndef NAMETABLE_VIEW_H
#define NAMETABLE_VIEW_H
#include "nes_system.h"

/* Host observation only: a midpoint snapshot supplies unfetched tiles; actual
   background fetches supply visible rows with their raster-specific graphics.
   Retain the composed view across presentation and pauses. */
struct NametableView {
    uint32_t pixels[PPU_NAMETABLE_WIDTH * PPU_NAMETABLE_HEIGHT];
    uint32_t fetched_pixels[PPU_NAMETABLE_WIDTH * PPU_NAMETABLE_HEIGHT];
    uint32_t fetched_order[PPU_NAMETABLE_WIDTH * PPU_NAMETABLE_HEIGHT / 8];
    uint32_t sequence;
    int mirroring; /* MirroringMode, or -1 for custom routing. */
    bool valid, sampled;
};

/* Begin/finish around each emulated frame; invalidate on ROM/state/reset
   changes. Call sample between CPU steps until it has captured this frame. */
void nametable_view_sample(NametableView *view, const NES *nes);
void nametable_view_begin_frame(NametableView *view);
/* Observe existing PPU fetch results; never performs additional bus reads. */
void nametable_view_record(NametableView *view, uint16_t v, const uint32_t pixels[8]);
void nametable_view_finish_frame(NametableView *view);
#endif
