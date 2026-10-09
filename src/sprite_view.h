#ifndef SPRITE_VIEW_H
#define SPRITE_VIEW_H
#include "ppu2c02.h"

/* Optional host display enhancement. Hardware OAM, pixels, flags and mapper
   bus activity remain unchanged; this state is excluded from save files. */
typedef struct SpriteView {
    ScanlineSprite extra[56];
    unsigned count;
    int scanline;
    uint32_t pixels[256 * 240];
} SpriteView;

void sprite_view_reset(SpriteView *view, const NES *nes);
void sprite_view_prepare(SpriteView *view, const NES *nes);
uint32_t sprite_view_pixel(const SpriteView *view, const PPU2C02 *ppu,
                          int x, bool background_opaque, bool hardware_sprite,
                          uint32_t hardware_pixel);
#endif
