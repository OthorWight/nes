#include "sprite_view.h"
#include "nes_system.h"
#include <stdlib.h>
#include <string.h>

void sprite_view_reset(SpriteView *view, const NES *nes) {
    if (!view) return;
    view->count = 0;
    view->scanline = -1;
    memcpy(view->pixels, nes->ppu.screen_buffer, sizeof(view->pixels));
}

void sprite_view_prepare(SpriteView *view, const NES *nes) {
    const PPU2C02 *p = &nes->ppu;
    view->count = 0;
    view->scanline = p->scanline + 1;
    if (!(p->ppu_mask & 0x18) || p->scanline < 0 || p->scanline >= 239 ||
        !nes->cart || !nes->cart->vtable) return;

    /* Approximate the extra slots from an OAM snapshot at the end of the
       hardware fetch window. The real eight slots still come from the PPU's
       cycle-level evaluation. Unusual mid-evaluation OAM writes can differ. */
    unsigned height = (p->ppu_ctrl & 0x20) ? 16 : 8;
    unsigned found = 0;
    for (unsigned i = 0; i < 64; ++i) {
        int row = p->scanline - p->oam_ram[i * 4];
        if (row < 0 || row >= (int)height) continue;
        if (++found <= 8) continue;
        view->extra[view->count++] = (ScanlineSprite){
            .x = p->oam_ram[i * 4 + 3],
            .attributes = p->oam_ram[i * 4 + 2], .sprite_index = (uint8_t)i
        };
    }
    if (!view->count) return;

    /* Read through a private mapper copy, as the nametable snapshot does.
       CHR reads can change MMC2/MMC4 latches; never issue them on the live
       bus or let them reach debugger/diagnostic callbacks. */
    NES *snapshot = malloc(sizeof(*snapshot));
    size_t mapper_size = nes->cart->vtable->state_size;
    void *mapper = mapper_size ? malloc(mapper_size) : NULL;
    if (!snapshot || (mapper_size && !mapper)) {
        free(snapshot); free(mapper); view->count = 0; return;
    }
    *snapshot = *nes;
    Cartridge cart = *nes->cart;
    if (mapper_size) memcpy(mapper, cart.mapper_data, mapper_size);
    cart.mapper_data = mapper;
    cart.nes = snapshot;
    snapshot->cart = &cart;
    snapshot->diagnostics = NULL;
    snapshot->execution = NULL;
    snapshot->execution_clock.events = 0;
    snapshot->nametable_view = NULL;
    snapshot->sprite_view = NULL;
    snapshot->ppu.cycle = 261; /* Sprite CHR context, including MMC5. */
    for (unsigned s = 0; s < view->count; ++s) {
        ScanlineSprite *spr = &view->extra[s];
        unsigned i = spr->sprite_index * 4;
        unsigned row = (unsigned)(p->scanline - p->oam_ram[i]);
        unsigned tile = p->oam_ram[i + 1];
        if (spr->attributes & 0x80) row ^= height - 1;
        uint16_t address = height == 8 ?
            ((p->ppu_ctrl & 8) ? 0x1000 : 0) | (tile << 4) | row :
            ((tile & 1) << 12) | ((tile & 0xFE) << 4) | ((row & 8) << 1) | (row & 7);
        spr->low_byte = nes_ppu_bus_read(snapshot, address);
        spr->high_byte = nes_ppu_bus_read(snapshot, address + 8);
    }
    free(mapper);
    free(snapshot);
}

uint32_t sprite_view_pixel(const SpriteView *view, const PPU2C02 *p,
                          int x, bool background_opaque, bool hardware_sprite,
                          uint32_t hardware_pixel) {
    if (hardware_sprite || view->scanline != p->scanline ||
        !(p->ppu_mask & 0x10) || (x < 8 && !(p->ppu_mask & 4))) return hardware_pixel;
    for (unsigned i = 0; i < view->count; ++i) {
        const ScanlineSprite *spr = &view->extra[i];
        int col = x - spr->x;
        if (col < 0 || col >= 8) continue;
        unsigned shift = (spr->attributes & 0x40) ? (unsigned)col : 7u - (unsigned)col;
        unsigned color = ((spr->low_byte >> shift) & 1) | (((spr->high_byte >> shift) & 1) << 1);
        if (!color) continue;
        /* First opaque sprite wins even if it is behind the background. */
        if (background_opaque && (spr->attributes & 0x20)) return hardware_pixel;
        return ppu_palette_color(p, 0x10 | ((spr->attributes & 3) << 2) | color);
    }
    return hardware_pixel;
}
