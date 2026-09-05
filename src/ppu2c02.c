#include "ppu2c02.h"
#include "nes_system.h"
#include <string.h>

/*
 * Bee 52 / 2C02 OAM evaluation compatibility
 *
 * Bee 52 reads $2004 while rendering and also consumes PPUSTATUS.5.  During
 * rendering, $2004 exposes the PPU's internal OAM data bus; it is not a
 * direct read of primary OAM[OAMADDR].  A scanline-at-a-time sprite counter
 * cannot reproduce that bus or the 2C02 diagonal sprite-overflow bug.
 *
 * This small cycle state machine runs beside the existing sprite renderer.
 * It supplies the externally visible OAM bus and the sticky overflow flag;
 * the existing renderer continues to fetch and draw sprites.
 */
typedef struct {
    const PPU2C02 *owner;
    uint8_t secondary[32];
    uint8_t bus;
    uint8_t n;
    uint8_t m;
    uint8_t secondary_index;
    bool done;
} Bee52OAMEvalState;

static Bee52OAMEvalState bee52_oam_eval;

static inline bool bee52_ppu_rendering_enabled(const PPU2C02 *p) {
    return (p->ppu_mask & 0x18u) != 0;
}

static inline bool bee52_sprite_y_in_range(const PPU2C02 *p, uint8_t y) {
    /* Evaluation on scanline N prepares sprites for N+1.  OAM Y stores
       top-1, so unsigned (N-Y) is the sprite row for the next scanline. */
    uint8_t row = (uint8_t)((uint8_t)p->scanline - y);
    uint8_t height = (p->ppu_ctrl & 0x20u) ? 16u : 8u;
    return row < height;
}

static void bee52_ppu_oam_eval_tick(PPU2C02 *p) {
    const int sl = p->scanline;
    const int cy = p->cycle;

    if (bee52_oam_eval.owner != p) {
        memset(&bee52_oam_eval, 0, sizeof(bee52_oam_eval));
        bee52_oam_eval.owner = p;
        bee52_oam_eval.bus = 0xFFu;
        memset(bee52_oam_eval.secondary, 0xFF, sizeof(bee52_oam_eval.secondary));
    }

    if (!bee52_ppu_rendering_enabled(p) || sl < 0 || sl >= 240) {
        return;
    }

    if (cy == 1) {
        bee52_oam_eval.n = 0;
        bee52_oam_eval.m = 0;
        bee52_oam_eval.secondary_index = 0;
        bee52_oam_eval.done = false;
        bee52_oam_eval.bus = 0xFFu;
        memset(bee52_oam_eval.secondary, 0xFF, sizeof(bee52_oam_eval.secondary));
    }

    /* Secondary OAM clear: the internal OAM bus reads as $FF. */
    if (cy >= 1 && cy <= 64) {
        bee52_oam_eval.bus = 0xFFu;
        return;
    }

    if (cy >= 65 && cy <= 256) {
        if (cy == 65) {
            bee52_oam_eval.n = 0;
            bee52_oam_eval.m = 0;
            bee52_oam_eval.secondary_index = 0;
            bee52_oam_eval.done = false;
        }

        if (bee52_oam_eval.done || bee52_oam_eval.n >= 64u) {
            bee52_oam_eval.bus = 0xFFu;
            bee52_oam_eval.done = true;
            return;
        }

        if (cy & 1) {
            /* Odd evaluation cycles read primary OAM. */
            unsigned index = ((unsigned)bee52_oam_eval.n << 2) | bee52_oam_eval.m;
            bee52_oam_eval.bus = p->oam_ram[index & 0xFFu];
            return;
        }

        /* Even cycles process/copy the byte read on the preceding odd cycle. */
        if (bee52_oam_eval.secondary_index < 32u) {
            if (bee52_oam_eval.m == 0u) {
                if (bee52_sprite_y_in_range(p, bee52_oam_eval.bus)) {
                    bee52_oam_eval.secondary[bee52_oam_eval.secondary_index++] = bee52_oam_eval.bus;
                    bee52_oam_eval.m = 1u;
                } else {
                    bee52_oam_eval.n++;
                }
            } else {
                bee52_oam_eval.secondary[bee52_oam_eval.secondary_index++] = bee52_oam_eval.bus;
                bee52_oam_eval.m++;
                if (bee52_oam_eval.m >= 4u) {
                    bee52_oam_eval.m = 0u;
                    bee52_oam_eval.n++;
                }
            }
        } else {
            /* Real 2C02 overflow evaluation increments N and, only when the
               currently tested byte is in range, increments M too.  This is
               the diagonal overflow bug and can test tile/attribute/X bytes
               as Y values. */
            if (bee52_sprite_y_in_range(p, bee52_oam_eval.bus)) {
                p->ppu_status |= 0x20u;
                bee52_oam_eval.m = (uint8_t)((bee52_oam_eval.m + 1u) & 3u);
            }
            bee52_oam_eval.n++;
        }

        if (bee52_oam_eval.n >= 64u) {
            bee52_oam_eval.done = true;
        }
        return;
    }

    if (cy >= 257 && cy <= 320) {
        /* Sprite fetch phase repeatedly exposes bytes from secondary OAM. */
        unsigned phase = (unsigned)(cy - 257);
        unsigned sprite = phase >> 3;
        unsigned byte = (phase >> 1) & 3u;
        unsigned index = (sprite << 2) | byte;
        bee52_oam_eval.bus = bee52_oam_eval.secondary[index & 31u];
        p->oam_addr = 0;
        return;
    }

    if (cy >= 321 && cy <= 340) {
        bee52_oam_eval.bus = p->oam_ram[0];
        p->oam_addr = 0;
    }
}

static uint8_t bee52_ppu_oamdata_read(PPU2C02 *p) {
    const int sl = p->scanline;
    const int cy = p->cycle;
    if (bee52_ppu_rendering_enabled(p) && sl >= 0 && sl < 240 && cy >= 1 && cy <= 340) {
        return bee52_oam_eval.bus;
    }
    return p->oam_ram[p->oam_addr];
}

static inline void bee52_ppu_increment_x(PPU2C02 *p) {
    if ((p->v & 0x001Fu) == 31u) {
        p->v &= (uint16_t)~0x001Fu;
        p->v ^= 0x0400u;
    } else {
        p->v++;
    }
}

static inline void bee52_ppu_increment_y(PPU2C02 *p) {
    if ((p->v & 0x7000u) != 0x7000u) {
        p->v += 0x1000u;
    } else {
        unsigned y;
        p->v &= (uint16_t)~0x7000u;
        y = (unsigned)((p->v & 0x03E0u) >> 5);
        if (y == 29u) {
            y = 0;
            p->v ^= 0x0800u;
        } else if (y == 31u) {
            y = 0;
        } else {
            y++;
        }
        p->v = (uint16_t)((p->v & (uint16_t)~0x03E0u) | (uint16_t)(y << 5));
    }
}

static void bee52_ppu_increment_after_2007(PPU2C02 *p) {
    const int sl = p->scanline;
    if (bee52_ppu_rendering_enabled(p) && ((sl >= 0 && sl < 240) || sl == 261 || sl == -1)) {
        bee52_ppu_increment_x(p);
        bee52_ppu_increment_y(p);
    } else {
        p->v = (uint16_t)((p->v + ((p->ppu_ctrl & 0x04u) ? 32u : 1u)) & 0x7FFFu);
    }
}


static const uint32_t NES_PALETTE[64] = {
    0xFF7C7C7C, 0xFF0000FC, 0xFF0000BC, 0xFF4428BC, 0xFF940084, 0xFFA80020, 0xFFA81000, 0xFF881400,
    0xFF503000, 0xFF007800, 0xFF006800, 0xFF005800, 0xFF004058, 0xFF000000, 0xFF000000, 0xFF000000,
    0xFFBCBCBC, 0xFF0078F8, 0xFF0058F8, 0xFF6844FC, 0xFFD800B8, 0xFFE40058, 0xFFF83800, 0xFFE45C10,
    0xFFAC7C00, 0xFF00B800, 0xFF00A800, 0xFF00A844, 0xFF008888, 0xFF000000, 0xFF000000, 0xFF000000,
    0xFFF8F8F8, 0xFF3CBCFC, 0xFF6888FC, 0xFF9878F8, 0xFFF878F8, 0xFFF85898, 0xFFF87858, 0xFFFCA044,
    0xFFF8B800, 0xFFB8F818, 0xFF58D854, 0xFF58F898, 0xFF00E8D8, 0xFF787878, 0xFF000000, 0xFF000000,
    0xFFF8F8F8, 0xFFA4E4FC, 0xFFB8B8F8, 0xFFD8B8F8, 0xFFF8B8F8, 0xFFF8A4C0, 0xFFF0D0B0, 0xFFFCE0A4,
    0xFFF8D878, 0xFFD8F878, 0xFFB8F8B8, 0xFFB8F8D8, 0xFF00FCFC, 0xFFF8D8F8, 0xFF000000, 0xFF000000
};

#define SCREEN_WIDTH         256

#define SCANLINE_VISIBLE_MAX 240
#define SCANLINE_PRERENDER   261

#define CYCLE_SCANLINE_END   341

static inline uint8_t ppu_get_coarse_x(uint16_t v) { return v & 0x001F; }
static inline void ppu_set_coarse_x(uint16_t *v, uint8_t x) { *v = (*v & ~0x001F) | (x & 0x1F); }
static inline uint8_t ppu_get_coarse_y(uint16_t v) { return (v >> 5) & 0x001F; }
static inline void ppu_set_coarse_y(uint16_t *v, uint8_t y) { *v = (*v & ~0x03E0) | ((y & 0x1F) << 5); }
static inline uint8_t ppu_get_fine_y(uint16_t v) { return (v >> 12) & 0x0007; }

static void ppu_update_nmi(PPU2C02 *ppu, NES *nes) {
    bool nmi_line = (ppu->ppu_ctrl & 0x80) && (ppu->ppu_status & 0x80);
    cpu_set_nmi_line(&nes->cpu, nmi_line);
}

static void ppu_increment_scroll_x(PPU2C02 *ppu) {
    if (ppu_get_coarse_x(ppu->v) == 31) {
        ppu_set_coarse_x(&ppu->v, 0);
        ppu->v ^= 0x0400;
    } else {
        ppu->v++;
    }
}

static void ppu_increment_scroll_y(PPU2C02 *ppu) {
    uint8_t fine_y = ppu_get_fine_y(ppu->v);
    if (fine_y < 7) {
        ppu->v += 0x1000;
    } else {
        ppu->v &= ~0x7000;
        uint8_t y = ppu_get_coarse_y(ppu->v);
        if (y == 29) {
            y = 0;
            ppu->v ^= 0x0800;
        } else if (y == 31) {
            y = 0;
        } else {
            y++;
        }
        ppu_set_coarse_y(&ppu->v, y);
    }
}

static void ppu_evaluate_sprites(NES *nes, int target_scanline) {
    PPU2C02 *ppu = &nes->ppu;
    (void)nes;
    ppu->scanline_sprite_count = 0;
    if (target_scanline < 0) return;
    bool rendering_enabled = (ppu->ppu_mask & 0x18) != 0;
    if (!rendering_enabled) return;
    int sprite_height = (ppu->ppu_ctrl & 0x20) ? 16 : 8;

    for (int i = 0; i < 64; i++) {
        int sprite_y = (int)ppu->oam_ram[i * 4] + 1;
        if (target_scanline >= sprite_y && target_scanline < sprite_y + sprite_height) {
            if (ppu->scanline_sprite_count < 8) {
                uint8_t attr     = ppu->oam_ram[i * 4 + 2];
                uint8_t sprite_x = ppu->oam_ram[i * 4 + 3];

                ScanlineSprite *spr = &ppu->scanline_sprites[ppu->scanline_sprite_count++];
                spr->x = sprite_x;
                spr->attributes = attr;
                spr->sprite_index = (uint8_t)i;
                spr->low_byte = 0;
                spr->high_byte = 0;
            } else {
                break;
            }
        }
    }
}

void ppu_init(PPU2C02 *ppu) {
    memset(ppu->palette_ram, 0x0F, sizeof(ppu->palette_ram));
    memset(ppu->oam_ram, 0, sizeof(ppu->oam_ram));
    memset(ppu->screen_buffer, 0, sizeof(ppu->screen_buffer));
    memset(ppu->scanline_sprites, 0, sizeof(ppu->scanline_sprites));
    ppu->v = 0;
    ppu->t = 0;
    ppu->x = 0;
    ppu->w = 0;
    ppu->ppu_ctrl = 0;
    ppu->ppu_mask = 0;
    ppu->ppu_status = 0;
    ppu->oam_addr = 0;
    ppu->buffered_data = 0;
    ppu->bus_address = 0;
    ppu->scanline = 0;
    ppu->cycle = 0;
    ppu->nmi_occurred = false;
    ppu->frame_complete = false;
    ppu->nmi_suppressed = false;
    ppu->scanline_sprite_count = 0;
    ppu->bg_palette_index = 0;
    ppu->bg_shifter_pattern_low = 0;
    ppu->bg_shifter_pattern_high = 0;
    ppu->bg_shifter_attrib_low = 0;
    ppu->bg_shifter_attrib_high = 0;
    ppu->bg_next_tile_id = 0;
    ppu->bg_next_tile_attrib = 0;
    ppu->bg_next_tile_lsb = 0;
    ppu->bg_next_tile_msb = 0;
    ppu->odd_frame = false;
    ppu->open_bus_value = 0;
    ppu->overflow_cycle = -1;
    memset(ppu->open_bus_decay_cycles, 0, sizeof(ppu->open_bus_decay_cycles));
}

static void ppu_update_open_bus_decay(PPU2C02 *ppu, uint64_t cpu_cycle) {
    for (int i = 0; i < 8; i++) {
        if (cpu_cycle - ppu->open_bus_decay_cycles[i] > 700000) {
            ppu->open_bus_value &= ~(1 << i);
        }
    }
}

static void ppu_refresh_open_bus(PPU2C02 *ppu, uint64_t cpu_cycle, uint8_t value, uint8_t driven_mask) {
    for (int i = 0; i < 8; i++) {
        if (driven_mask & (1 << i)) {
            ppu->open_bus_decay_cycles[i] = cpu_cycle;
            if (value & (1 << i)) {
                ppu->open_bus_value |= (1 << i);
            } else {
                ppu->open_bus_value &= ~(1 << i);
            }
        }
    }
}

uint8_t ppu_palette_read(PPU2C02 *ppu, uint16_t addr) {
    addr &= 0x001F;
    if ((addr & 0x0013) == 0x0010) {
        addr &= 0x000F;
    }
    return ppu->palette_ram[addr] & 0x3F;
}

void ppu_palette_write(PPU2C02 *ppu, uint16_t addr, uint8_t data) {
    addr &= 0x001F;
    if ((addr & 0x0013) == 0x0010) {
        addr &= 0x000F;
    }
    ppu->palette_ram[addr] = data & 0x3F;
}

uint8_t ppu_read_reg(NES *nes, uint16_t address) {
    PPU2C02 *ppu = &nes->ppu;
    ppu_update_open_bus_decay(ppu, nes->cpu.cycle_count);
    uint8_t data = ppu->open_bus_value;

    switch (address & 0x2007) {
        case 0x2002: {
            uint8_t status = ppu->ppu_status;
            if (ppu->scanline == 241) {
                if (ppu->cycle >= 1 && ppu->cycle <= 3) {
                    nes->cpu.nmi_edge = false;
                    nes->cpu.nmi_delayed = false;
                    ppu->nmi_suppressed = true;
                }
                if (ppu->cycle >= 1 && ppu->cycle <= 2) {
                    status &= ~0x80;
                }
            }
            data = (uint8_t)((status & 0xE0) | (ppu->open_bus_value & 0x1F));
            ppu->ppu_status &= 0x7F;
            ppu->w = 0;
            ppu_refresh_open_bus(ppu, nes->cpu.cycle_count, data, 0xE0);
            ppu_update_nmi(ppu, nes);
            break;
        }
        case 0x2004:
            data = bee52_ppu_oamdata_read(ppu);
            if ((ppu->oam_addr & 0x03) == 0x02) {
                data &= 0xE3;
            }
            ppu_refresh_open_bus(ppu, nes->cpu.cycle_count, data, 0xFF);
            break;
        case 0x2007: {
            uint16_t vram_addr = (uint16_t)(ppu->v & 0x3FFF);
            uint8_t returned_data = ppu->buffered_data;

            if (vram_addr >= 0x3F00) {
                returned_data = (ppu_palette_read(ppu, vram_addr) & 0x3F) | (ppu->open_bus_value & 0xC0);
                ppu->buffered_data = nes_ppu_bus_read(nes, (vram_addr & 0x0FFF) | 0x2000);
                ppu_refresh_open_bus(ppu, nes->cpu.cycle_count, returned_data, 0x3F);
            } else {
                ppu->buffered_data = nes_ppu_bus_read(nes, vram_addr);
                ppu_refresh_open_bus(ppu, nes->cpu.cycle_count, returned_data, 0xFF);
            }

            bee52_ppu_increment_after_2007(ppu);
            nes_ppu_bus_read(nes, ppu->v & 0x3FFF);
            data = returned_data;
            break;
        }
    }
    return data;
}

void ppu_write_reg(NES *nes, uint16_t address, uint8_t data) {
    PPU2C02 *ppu = &nes->ppu;
    ppu_update_open_bus_decay(ppu, nes->cpu.cycle_count);
    ppu_refresh_open_bus(ppu, nes->cpu.cycle_count, data, 0xFF);

    switch (address & 0x2007) {
        case 0x2000:
            ppu->ppu_ctrl = data;
            ppu->t = (uint16_t)((ppu->t & 0xF3FF) | (((uint16_t)data & 0x03) << 10));
            ppu_update_nmi(ppu, nes);
            break;
        case 0x2001:
            ppu->ppu_mask = data;
            break;
        case 0x2003:
            ppu->oam_addr = data;
            break;
        case 0x2004: {
            bool rendering_enabled = (ppu->ppu_mask & 0x18) != 0;
            bool is_rendering_scanline = (ppu->scanline < 240 || ppu->scanline == 261);
            if (!(rendering_enabled && is_rendering_scanline)) {
                if ((ppu->oam_addr & 0x03) == 0x02) {
                    data &= 0xE3;
                }
                ppu->oam_ram[ppu->oam_addr++] = data;
            }
            break;
        }
        case 0x2005:
            if (ppu->w == 0) {
                ppu->x = data & 0x07;
                ppu->t = (uint16_t)((ppu->t & 0xFFE0) | (data >> 3));
                ppu->w = 1;
            } else {
                ppu->t = (uint16_t)((ppu->t & 0x8C1F) | (((uint16_t)data & 0x07) << 12) | (((uint16_t)data & 0xF8) << 2));
                ppu->w = 0;
            }
            break;
        case 0x2006:
            if (ppu->w == 0) {
                ppu->t = (uint16_t)((ppu->t & 0x00FF) | (((uint16_t)data & 0x3F) << 8));
                ppu->w = 1;
            } else {
                ppu->t = (uint16_t)((ppu->t & 0xFF00) | data);
                ppu->v = ppu->t;
                ppu->w = 0;
                nes_ppu_bus_read(nes, ppu->v & 0x3FFF);
            }
            break;
        case 0x2007:
            nes_ppu_bus_write(nes, ppu->v & 0x3FFF, data);
            bee52_ppu_increment_after_2007(ppu);
            nes_ppu_bus_read(nes, ppu->v & 0x3FFF);
            break;
    }
}

static void ppu_step_shifters(PPU2C02 *ppu) {
    ppu->bg_shifter_pattern_low  <<= 1;
    ppu->bg_shifter_pattern_high <<= 1;
    ppu->bg_shifter_attrib_low   <<= 1;
    ppu->bg_shifter_attrib_high  <<= 1;
}

static void ppu_load_bg_shifters(PPU2C02 *ppu) {
    ppu->bg_shifter_pattern_low  = (ppu->bg_shifter_pattern_low  & 0xFF00) | ppu->bg_next_tile_lsb;
    ppu->bg_shifter_pattern_high = (ppu->bg_shifter_pattern_high & 0xFF00) | ppu->bg_next_tile_msb;
    ppu->bg_shifter_attrib_low   = (ppu->bg_shifter_attrib_low   & 0xFF00) | ((ppu->bg_next_tile_attrib & 0x01) ? 0xFF : 0x00);
    ppu->bg_shifter_attrib_high  = (ppu->bg_shifter_attrib_high  & 0xFF00) | ((ppu->bg_next_tile_attrib & 0x02) ? 0xFF : 0x00);
}

static void ppu_render_pixel(PPU2C02 *ppu, int pixel_x) {
    bool rendering_enabled = (ppu->ppu_mask & 0x18) != 0;
    uint8_t bg_color_idx = 0;
    uint16_t bg_palette_idx = 0;

    if (rendering_enabled && (ppu->ppu_mask & 0x08)) {
        uint16_t bit_mux = 0x8000 >> ppu->x;

        uint8_t p0 = (ppu->bg_shifter_pattern_low  & bit_mux) ? 1 : 0;
        uint8_t p1 = (ppu->bg_shifter_pattern_high & bit_mux) ? 1 : 0;
        bg_color_idx = p0 | (p1 << 1);

        uint8_t a0 = (ppu->bg_shifter_attrib_low   & bit_mux) ? 1 : 0;
        uint8_t a1 = (ppu->bg_shifter_attrib_high  & bit_mux) ? 1 : 0;
        ppu->bg_palette_index = a0 | (a1 << 1);

        uint16_t final_palette_addr = (bg_color_idx == 0) ? 0x0000 : ((ppu->bg_palette_index << 2) | bg_color_idx);
        bg_palette_idx = final_palette_addr & 0x001F;
        if ((bg_palette_idx & 0x0013) == 0x0010) {
            bg_palette_idx &= 0x000F;
        }
    }

    uint8_t sprite_color_idx = 0;
    uint8_t sprite_palette_idx = 0;
    uint8_t sprite_priority = 0;
    bool sprite_0_active = false;

    if (rendering_enabled && (ppu->ppu_mask & 0x10)) {
        for (int s = 0; s < ppu->scanline_sprite_count; s++) {
            ScanlineSprite *spr = &ppu->scanline_sprites[s];
            if (pixel_x >= spr->x && pixel_x < spr->x + 8) {
                int col = pixel_x - spr->x;
                if (spr->attributes & 0x40) {
                    col = 7 - col;
                }

                uint8_t shift = 7 - col;
                uint8_t pixel_color = ((spr->low_byte  >> shift) & 0x01) |
                                     (((spr->high_byte >> shift) & 0x01) << 1);

                if (pixel_color != 0) {
                    sprite_color_idx = pixel_color;
                    sprite_palette_idx = spr->attributes & 0x03;
                    sprite_priority = (spr->attributes >> 5) & 0x01;
                    sprite_0_active = (spr->sprite_index == 0);
                    break;
                }
            }
        }
    }

    if (pixel_x < 8) {
        if (!(ppu->ppu_mask & 0x02)) bg_color_idx = 0;
        if (!(ppu->ppu_mask & 0x04)) sprite_color_idx = 0;
    }

    uint16_t final_palette_idx = bg_palette_idx;
    bool show_sprite = false;

    if (bg_color_idx == 0 && sprite_color_idx != 0) {
        show_sprite = true;
    } else if (bg_color_idx != 0 && sprite_color_idx != 0) {
        bool left_clipped = (pixel_x < 8) && (!(ppu->ppu_mask & 0x02) || !(ppu->ppu_mask & 0x04));
        if (sprite_0_active && (ppu->ppu_mask & 0x08) && (ppu->ppu_mask & 0x10) && pixel_x < 255 && !left_clipped) {
            if (!(ppu->ppu_status & 0x40)) {
                ppu->ppu_status |= 0x40;
            }
        }
        if (sprite_priority == 0) {
            show_sprite = true;
        }
    }

    if (show_sprite) {
        uint16_t final_sprite_palette_addr = 0x0010 | (sprite_palette_idx << 2) | sprite_color_idx;
        final_palette_idx = final_sprite_palette_addr & 0x001F;
        if ((final_palette_idx & 0x0013) == 0x0010) {
            final_palette_idx &= 0x000F;
        }
    }

    if (ppu->scanline < SCANLINE_VISIBLE_MAX) {
        if (rendering_enabled) {
            ppu->screen_buffer[ppu->scanline * SCREEN_WIDTH + pixel_x] = NES_PALETTE[ppu->palette_ram[final_palette_idx] & 0x3F];
        } else {
            uint16_t vram_addr = ppu->v & 0x3FFF;
            uint16_t palette_idx = 0;
            if (vram_addr >= 0x3F00) {
                palette_idx = vram_addr & 0x001F;
                if ((palette_idx & 0x0013) == 0x0010) {
                    palette_idx &= 0x000F;
                }
            }
            ppu->screen_buffer[ppu->scanline * SCREEN_WIDTH + pixel_x] = NES_PALETTE[ppu->palette_ram[palette_idx] & 0x3F];
        }
    }
}

void ppu_step(NES *nes) {
    PPU2C02 *ppu = &nes->ppu;
    const bool rendering_enabled = (ppu->ppu_mask & 0x18) != 0;
    const bool rendering_scanline = (ppu->scanline < SCANLINE_VISIBLE_MAX ||
                                     ppu->scanline == SCANLINE_PRERENDER);

    if (nes->cart && nes->cart->vtable && nes->cart->vtable->ppu_dot) {
        nes->cart->vtable->ppu_dot(nes->cart, ppu->bus_address);
    }

    /* Update the internal OAM evaluation bus and sprite-overflow timing for
       the current PPU dot before CPU-visible register reads can occur. */
    bee52_ppu_oam_eval_tick(ppu);

    /* Status flags are cleared on dot 1 of the pre-render scanline. */
    if (ppu->scanline == SCANLINE_PRERENDER && ppu->cycle == 1) {
        ppu->ppu_status &= (uint8_t)~0xE0;
        ppu->nmi_occurred = false;
        ppu->nmi_suppressed = false;
        ppu_update_nmi(ppu, nes);
    }

    /* VBlank starts on scanline 241, dot 1. */
    if (ppu->scanline == 241 && ppu->cycle == 1) {
        ppu->nmi_occurred = true;
        ppu->ppu_status |= 0x80;
        if (!ppu->nmi_suppressed) {
            ppu_update_nmi(ppu, nes);
        }
    }

    if (rendering_enabled && rendering_scanline) {
        /* The idle dot drives the upcoming background pattern address without
           reading it.  Holding the last nametable address through dot 0 would
           create a second qualified MMC3 A12 rise on every scanline when the
           background uses $1000.  Dot 1 starts the next nametable fetch. */
        if (ppu->cycle == 0) {
            uint16_t table = (ppu->ppu_ctrl & 0x10) ? 0x1000 : 0x0000;
            uint16_t pattern_addr = table |
                ((uint16_t)ppu->bg_next_tile_id << 4) | ppu_get_fine_y(ppu->v);
            nes_ppu_bus_set_address(nes, pattern_addr);
        } else if (ppu->cycle == 1) {
            ppu->bg_next_tile_id = nes_ppu_bus_read(nes, 0x2000 | (ppu->v & 0x0FFF));
        }

        /*
         * Background pipeline.  The shifters advance on dots 2-257 and
         * 322-337.  A new tile is loaded at the start of every 8-dot fetch
         * group, matching the 2C02 fetch schedule.
         */
        if ((ppu->cycle >= 2 && ppu->cycle <= 257) ||
            (ppu->cycle >= 321 && ppu->cycle <= 337)) {
            ppu_step_shifters(ppu);

            switch ((ppu->cycle - 1) & 7) {
                case 0: {
                    ppu_load_bg_shifters(ppu);
                    uint16_t nt_addr = 0x2000 | (ppu->v & 0x0FFF);
                    ppu->bg_next_tile_id = nes_ppu_bus_read(nes, nt_addr);
                    break;
                }
                case 2: {
                    uint16_t attr_addr = 0x23C0 | (ppu->v & 0x0C00) |
                                         ((ppu->v >> 4) & 0x38) |
                                         ((ppu->v >> 2) & 0x07);
                    uint8_t attr_byte = nes_ppu_bus_read(nes, attr_addr);
                    uint8_t shift = (uint8_t)(((ppu->v >> 4) & 4) |
                                              (ppu->v & 2));
                    ppu->bg_next_tile_attrib = (attr_byte >> shift) & 0x03;
                    break;
                }
                case 4: {
                    uint8_t fine_y = ppu_get_fine_y(ppu->v);
                    uint16_t table = (ppu->ppu_ctrl & 0x10) ? 0x1000 : 0x0000;
                    uint16_t pattern_addr = table |
                        ((uint16_t)ppu->bg_next_tile_id << 4) | fine_y;
                    ppu->bg_next_tile_lsb = nes_ppu_bus_read(nes, pattern_addr);
                    break;
                }
                case 6: {
                    uint8_t fine_y = ppu_get_fine_y(ppu->v);
                    uint16_t table = (ppu->ppu_ctrl & 0x10) ? 0x1000 : 0x0000;
                    uint16_t pattern_addr = table |
                        ((uint16_t)ppu->bg_next_tile_id << 4) | fine_y;
                    ppu->bg_next_tile_msb = nes_ppu_bus_read(nes,
                                                             pattern_addr + 8);
                    break;
                }
                case 7:
                    ppu_increment_scroll_x(ppu);
                    break;
                default:
                    break;
            }
        }

        if (ppu->cycle == 256) {
            ppu_increment_scroll_y(ppu);
        }

        if (ppu->cycle == 257) {
            ppu_load_bg_shifters(ppu);
            ppu->v = (ppu->v & 0xFBE0) | (ppu->t & 0x041F);
            ppu->oam_addr = 0;

            if (ppu->scanline == SCANLINE_PRERENDER) {
                ppu_evaluate_sprites(nes, 0);
            } else if (ppu->scanline < SCANLINE_VISIBLE_MAX) {
                ppu_evaluate_sprites(nes, ppu->scanline + 1);
            } else {
                ppu->scanline_sprite_count = 0;
            }
        }

        if (ppu->scanline == SCANLINE_PRERENDER &&
            ppu->cycle >= 280 && ppu->cycle <= 304) {
            ppu->v = (ppu->v & 0x841F) | (ppu->t & 0x7BE0);
        }

        /* The first dummy nametable fetch is already issued at dot 337 above. */
        if (ppu->cycle == 339) {
            uint16_t nt_addr = 0x2000 | (ppu->v & 0x0FFF);
            ppu->bg_next_tile_id = nes_ppu_bus_read(nes, nt_addr);
        }

        /* Sprite pattern fetches for the following scanline. */
        if (ppu->cycle >= 257 && ppu->cycle <= 320) {
            int offset_cycle = ppu->cycle - 257;
            int spr_idx = offset_cycle / 8;
            int step = offset_cycle & 7;

            if (step == 4 || step == 6) {
                int target_scanline = (ppu->scanline == SCANLINE_PRERENDER)
                    ? 0 : (ppu->scanline + 1);
                int sprite_height = (ppu->ppu_ctrl & 0x20) ? 16 : 8;
                uint16_t pattern_addr;

                if (spr_idx < ppu->scanline_sprite_count) {
                    ScanlineSprite *spr = &ppu->scanline_sprites[spr_idx];
                    uint8_t tile_id = ppu->oam_ram[spr->sprite_index * 4 + 1];
                    int sprite_y = (int)ppu->oam_ram[spr->sprite_index * 4] + 1;
                    int row = target_scanline - sprite_y;
                    if (spr->attributes & 0x80) {
                        row = (sprite_height - 1) - row;
                    }

                    if (sprite_height == 8) {
                        uint16_t table = (ppu->ppu_ctrl & 0x08)
                            ? 0x1000 : 0x0000;
                        pattern_addr = table | ((uint16_t)tile_id << 4) |
                                       (uint16_t)(row & 7);
                    } else {
                        uint16_t table = (tile_id & 1) ? 0x1000 : 0x0000;
                        uint8_t actual_tile = tile_id & 0xFE;
                        if (row >= 8) {
                            actual_tile++;
                            row -= 8;
                        }
                        pattern_addr = table |
                            ((uint16_t)actual_tile << 4) |
                            (uint16_t)(row & 7);
                    }

                    if (step == 4) {
                        spr->low_byte = nes_ppu_bus_read(nes, pattern_addr);
                    } else {
                        spr->high_byte = nes_ppu_bus_read(nes,
                                                          pattern_addr + 8);
                    }
                } else {
                    /* Empty secondary-OAM slots contain tile $FF.  In 8x16
                       mode bit 0 of that tile selects pattern table $1000,
                       so the discarded dummy fetch is from $1FE0-$1FFF.
                       MMC3 boards depend on this A12-high fetch to clock the
                       scanline counter when fewer than eight sprites are on
                       the line. */
                    uint16_t table = (sprite_height == 8)
                        ? ((ppu->ppu_ctrl & 0x08) ? 0x1000 : 0x0000)
                        : 0x1000;
                    uint16_t tile = (sprite_height == 8) ? 0xFF : 0xFE;
                    pattern_addr = table | (tile << 4);
                    (void)nes_ppu_bus_read(nes,
                        (step == 4) ? pattern_addr : (pattern_addr + 8));
                }
            }
        }
    }

    /* Visible pixels are produced on dots 1-256. */
    if (ppu->scanline < SCANLINE_VISIBLE_MAX &&
        ppu->cycle >= 1 && ppu->cycle <= 256) {
        ppu_render_pixel(ppu, ppu->cycle - 1);
    }

    /* Advance to the next PPU dot. */
    if (ppu->scanline == SCANLINE_PRERENDER && ppu->cycle == 339 &&
        ppu->odd_frame && rendering_enabled) {
        /* Odd NTSC frames omit the final pre-render dot. */
        ppu->cycle = 0;
        ppu->scanline = 0;
        ppu->odd_frame = false;
    } else {
        ppu->cycle++;
        if (ppu->cycle >= CYCLE_SCANLINE_END) {
            ppu->cycle = 0;
            ppu->scanline++;

            if (ppu->scanline == 241) {
                ppu->frame_complete = true;
                nes->frame_ready = true;
            } else if (ppu->scanline >= 262) {
                ppu->scanline = 0;
                ppu->odd_frame = !ppu->odd_frame;
            }
        }
    }
}
