#include "ppu2c02.h"
#include "nes_system.h"
#include "diagnostics.h"
#include "nametable_view.h"
#include <string.h>
#include <stdlib.h>
#include <stddef.h>

/* Cycle-level OAM evaluation supplies the rendering-time $2004 bus and the
   2C02 diagonal sprite-overflow behavior. */

static inline bool ppu_rendering_enabled(const PPU2C02 *p) {
    return (p->ppu_mask & 0x18u) != 0;
}

static inline bool sprite_y_in_range(const PPU2C02 *p, uint8_t y) {
    /* Evaluation on scanline N prepares sprites for N+1.  OAM Y stores
       top-1, so N-Y is the sprite row for the next scanline. Keep the
       subtraction signed: hidden sprites at Y=$F0-$FF must not wrap
       into range at the top of the frame and falsely set overflow. */
    int row = p->scanline - (int)y;
    int height = (p->ppu_ctrl & 0x20u) ? 16 : 8;
    return row >= 0 && row < height;
}

static void ppu_oam_advance(PPU2C02 *p, unsigned amount, bool align) {
    unsigned next = p->oam_addr + amount;
    if (align) next &= ~3u;
    p->oam_addr = (uint8_t)next;
    p->oam_eval.n = p->oam_addr >> 2;
    p->oam_eval.m = p->oam_addr & 3;
    if (next >= 256) p->oam_eval.done = true;
}

static void ppu_secondary_advance(PPUOAMEvalState *e) {
    if (!e->increment_frozen && ++e->secondary_index == 32) {
        e->secondary_index = 0;
        e->increment_frozen = true;
        e->secondary_full = true;
    }
}

static void ppu_oam_eval_tick(PPU2C02 *p) {
    int cy = p->cycle;
    PPUOAMEvalState *e = &p->oam_eval;
    bool was_full = e->secondary_full;
    if (!ppu_rendering_enabled(p)) return;
    bool visible = p->scanline >= 0 && p->scanline < 240;
    bool prerender = p->scanline == ppu_prerender_line(p);
    if (!visible && !prerender) return;
    if (cy == 63 || cy == 255 || cy == 339) e->increment_frozen = false;
    if (visible && cy >= 1 && cy <= 64) {
        if (cy == 1) {
            e->secondary_index = 0;
            e->secondary_full = false;
            e->sprite_zero = false;
        }
        e->secondary_index = (uint8_t)((cy - 1) / 2);
        e->bus = 0xFF;
        if (!(cy & 1)) {
            e->secondary[e->secondary_index & 31] = 0xFF;
        }
    } else if (visible && cy >= 65 && cy <= 256) {
        if (cy == 65) {
            e->secondary_index = 0;
            e->secondary_full = false;
            e->done = false;
            e->copy_remaining = 0;
            e->sprite_zero = false;
        }
        if (cy & 1) {
            e->bus = p->oam_ram[p->oam_addr];
        } else if (e->done) {
            ppu_oam_advance(p, 4, true);
            e->bus = e->secondary[e->secondary_index & 31];
        } else if (!e->secondary_full) {
            e->secondary[e->secondary_index & 31] = e->bus;
            if (e->copy_remaining) {
                ppu_secondary_advance(e);
                ppu_oam_advance(p, 1, e->copy_remaining == 1 &&
                    !sprite_y_in_range(p, e->bus));
                --e->copy_remaining;
            } else if (sprite_y_in_range(p, e->bus)) {
                if (cy == 66) e->sprite_zero = true;
                ppu_secondary_advance(e);
                ppu_oam_advance(p, 1, false);
                e->copy_remaining = 3;
            } else {
                ppu_oam_advance(p, 4, true);
            }
        } else if (e->copy_remaining) {
            ppu_oam_advance(p, 1, e->copy_remaining == 1);
            if (--e->copy_remaining == 0) e->done = true;
        } else if (sprite_y_in_range(p, e->bus)) {
            p->ppu_status |= 0x20;
            ppu_oam_advance(p, 1, false);
            e->copy_remaining = 3;
        } else {
            /* Failed comparisons increment both counters without carrying
               the low two bits, producing the diagonal overflow scan. */
            unsigned next = (p->oam_addr & 0xFC) + 4;
            p->oam_addr = (uint8_t)(next | ((p->oam_addr + 1) & 3));
            if (next >= 256) e->done = true;
        }
        if (!(cy & 1) && was_full)
            e->bus = e->secondary[e->secondary_index & 31];
    } else if (cy >= 257 && cy <= 320) {
        if (cy == 257) {
            e->secondary_index = 0;
            p->scanline_sprite_count = 8;
        }
        unsigned phase = (unsigned)(cy - 257) & 7;
        unsigned sprite = (unsigned)(cy - 257) >> 3;
        e->bus = e->secondary[e->secondary_index & 31];
        ScanlineSprite *spr = &p->scanline_sprites[sprite];
        if (phase == 0) p->sprite_fetch_y = e->bus;
        if (phase == 1) p->sprite_fetch_tile = e->bus;
        if (phase == 2) spr->attributes = e->bus;
        if (phase == 3) {
            spr->x = e->bus;
            spr->sprite_index = sprite == 0 && e->sprite_zero ? 0 : 1;
        }
        if (phase < 3 || phase == 7) ppu_secondary_advance(e);
        p->oam_addr = 0;
    } else if (cy >= 321 && cy <= 340) {
        e->bus = e->secondary[e->secondary_index & 31];
    }
}

static uint8_t ppu_oamdata_read(PPU2C02 *p) {
    const int sl = p->scanline;
    const int cy = p->cycle;
    if (ppu_rendering_enabled(p) && (sl < 240 || sl == ppu_prerender_line(p)) && cy >= 0 && cy <= 340) {
        return p->oam_eval.bus;
    }
    return p->oam_ram[p->oam_addr];
}

static inline void ppu_increment_x(PPU2C02 *p) {
    if ((p->v & 0x001Fu) == 31u) {
        p->v &= (uint16_t)~0x001Fu;
        p->v ^= 0x0400u;
    } else {
        p->v++;
    }
}

static inline void ppu_increment_y(PPU2C02 *p) {
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

static void ppu_increment_after_2007(PPU2C02 *p) {
    const int sl = p->scanline;
    if (ppu_rendering_enabled(p) && ((sl >= 0 && sl < 240) || sl == ppu_prerender_line(p) || sl == -1)) {
        ppu_increment_x(p);
        ppu_increment_y(p);
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

bool ppu_render_nametables(const NES *nes, uint32_t *pixels, int *mirroring) {
    if (!nes || !nes->cart || !nes->cart->vtable || !pixels) return false;
    NES *snapshot = malloc(sizeof(*snapshot));
    size_t mapper_size = nes->cart->vtable->state_size;
    void *mapper = mapper_size ? malloc(mapper_size) : NULL;
    if (!snapshot || (mapper_size && !mapper)) {
        free(snapshot); free(mapper); return false;
    }
    *snapshot = *nes;
    Cartridge cart = *nes->cart;
    if (mapper_size) memcpy(mapper, cart.mapper_data, mapper_size);
    cart.mapper_data = mapper;
    cart.nes = snapshot;
    snapshot->cart = &cart;
    snapshot->diagnostics = NULL;
    snapshot->nametable_view = NULL;
    /* Background fetch context also selects MMC5's background CHR banks. */
    snapshot->ppu.scanline = 0;
    snapshot->ppu.cycle = 1;
    if (mirroring) {
        /* Mapper registers can override the cartridge header's mirroring.
           Query the private copy, including custom CIRAM/ExRAM routing. */
        int pages[4];
        for (unsigned table = 0; table < 4; ++table) {
            uint16_t address = (uint16_t)(0x2000 + table * 0x400);
            bool ciram = true;
            uint16_t mapped = cart.vtable->remap_ciram_addr ?
                cart.vtable->remap_ciram_addr(&cart, address, &ciram) :
                cartridge_default_remap_ciram(cart.mirroring, address);
            pages[table] = ciram ? (mapped & 0x0FFF) : -1;
        }
        *mirroring = -1;
        for (int mode = MIRROR_HORIZONTAL; mode <= MIRROR_ONE_SCREEN_HIGH; ++mode) {
            bool matches = true;
            for (unsigned table = 0; table < 4; ++table)
                if (pages[table] != cartridge_default_remap_ciram((MirroringMode)mode,
                        (uint16_t)(0x2000 + table * 0x400))) matches = false;
            if (matches) { *mirroring = mode; break; }
        }
    }
    uint16_t pattern = (snapshot->ppu.ppu_ctrl & 0x10) ? 0x1000 : 0;
    for (unsigned table = 0; table < 4; ++table) {
        uint16_t base = (uint16_t)(0x2000 + table * 0x400);
        for (unsigned ty = 0; ty < 30; ++ty) {
            for (unsigned tx = 0; tx < 32; ++tx) {
                uint8_t tile = nes_ppu_bus_read(snapshot, base + ty * 32 + tx);
                uint8_t attr = nes_ppu_bus_read(snapshot, base + 0x3C0 + (ty / 4) * 8 + tx / 4);
                unsigned palette = (attr >> ((ty & 2) * 2 + (tx & 2))) & 3;
                for (unsigned row = 0; row < 8; ++row) {
                    uint16_t address = pattern + tile * 16 + row;
                    uint8_t lo = nes_ppu_bus_read(snapshot, address);
                    uint8_t hi = nes_ppu_bus_read(snapshot, address + 8);
                    unsigned y = (table / 2) * 240 + ty * 8 + row;
                    unsigned x = (table % 2) * 256 + tx * 8;
                    for (unsigned col = 0; col < 8; ++col) {
                        unsigned shift = 7 - col;
                        unsigned color = ((lo >> shift) & 1) | (((hi >> shift) & 1) << 1);
                        unsigned index = color ? palette * 4 + color : 0;
                        pixels[y * PPU_NAMETABLE_WIDTH + x + col] =
                            NES_PALETTE[snapshot->ppu.palette_ram[index] & 0x3F];
                    }
                }
            }
        }
    }
    free(mapper);
    free(snapshot);
    return true;
}

#define SCANLINE_VISIBLE_MAX 240
#define SCANLINE_PRERENDER   ppu_prerender_line(ppu)

#define CYCLE_SCANLINE_END   341

static inline uint8_t ppu_get_coarse_x(uint16_t v) { return v & 0x001F; }
static inline void ppu_set_coarse_x(uint16_t *v, uint8_t x) { *v = (*v & ~0x001F) | (x & 0x1F); }
static inline uint8_t ppu_get_coarse_y(uint16_t v) { return (v >> 5) & 0x001F; }
static inline void ppu_set_coarse_y(uint16_t *v, uint8_t y) { *v = (*v & ~0x03E0) | ((y & 0x1F) << 5); }
static inline uint8_t ppu_get_fine_y(uint16_t v) { return (v >> 12) & 0x0007; }

static void ppu_update_nmi(PPU2C02 *ppu, NES *nes) {
    bool nmi_line = (ppu->ppu_ctrl & 0x80) && (ppu->ppu_status & 0x80);
    // cycle names the next dot. Enabling at pre-render dot 1 leaves only
    // one dot before vblank clears: the output pulse is too short for the
    // CPU to detect. Update the line without latching a new edge, preserving
    // any older pending NMI. Two or more dots before the clear is detectable.
    if (nmi_line && !nes->cpu.nmi_line &&
        ppu->scanline == SCANLINE_PRERENDER && ppu->cycle == 1) {
        nes->cpu.nmi_line = true;
        return;
    }
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

void ppu_init(PPU2C02 *ppu) {
    memset(&ppu->oam_eval, 0, sizeof(ppu->oam_eval));
    ppu->oam_eval.bus = 0xFF;
    memset(ppu->oam_eval.secondary, 0xFF, sizeof(ppu->oam_eval.secondary));
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
    ppu->odd_skip_rendering = false;
    ppu->open_bus_value = 0;
    ppu->overflow_cycle = -1;
    ppu->sprite_counter_active = 0;
    ppu->sprite_fetch_y = ppu->sprite_fetch_tile = 0;
    ppu->mask_delay = ppu->mask_pending = 0;
    ppu->address_delay = ppu->data_read_pipeline = 0;
    ppu->address_pending = ppu->fetch_address = 0;
    ppu->address_latch = ppu->bus_data = ppu->fetch_kind = 0;
    ppu->corruption_pending = false;
    ppu->corruption_seed = 0;
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

/* OAM's data pins are sampled at the trailing portion of M2. Evaluate
   that next dot without advancing the rendering engine or mapper pins. */
static uint8_t ppu_sample_oam(const PPU2C02 *p, bool overflow) {
    PPU2C02 sample;
    memcpy(&sample, p, offsetof(PPU2C02, screen_buffer));
    if (sample.mask_delay && --sample.mask_delay == 0) sample.ppu_mask = sample.mask_pending;
    ppu_oam_eval_tick(&sample);
    return overflow ? sample.ppu_status & 0x20 : ppu_oamdata_read(&sample);
}

static uint8_t ppu_read_register(NES *nes, uint16_t address, bool timed) {
    PPU2C02 *ppu = &nes->ppu;
    ppu_update_open_bus_decay(ppu, nes->cpu.cycle_count);
    uint8_t data = ppu->open_bus_value;

    switch (address & 0x2007) {
        case 0x2002: {
            uint8_t status = ppu->ppu_status;
            if (timed) status = (status & ~0x20) | ppu_sample_oam(ppu, true);
            /* Vblank is latched at M2 rise; the sprite flags remain visible
               until M2 falls almost two PPU dots later. */
            if (timed && ppu->scanline == SCANLINE_PRERENDER && ppu->cycle <= 1)
                status &= (uint8_t)~0x60;
            // cycle is the next dot to execute: 1 is just before the set
            // edge; 2 and 3 are on/just after it. Only the pre-edge read
            // returns clear. All three reads suppress the pending NMI.
            if (ppu->scanline == ppu_vblank_line(ppu)) {
                if (ppu->cycle >= 1 && ppu->cycle <= 3) {
                    nes->cpu.nmi_edge = false;
                    nes->cpu.nmi_delayed = false;
                    ppu->nmi_suppressed = true;
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
            data = timed ? ppu_sample_oam(ppu, false) : ppu_oamdata_read(ppu);
            if (!((ppu->ppu_mask & 0x18) && ppu->scanline < 240) &&
                (ppu->oam_addr & 0x03) == 0x02) {
                data &= 0xE3;
            }
            ppu_refresh_open_bus(ppu, nes->cpu.cycle_count, data, 0xFF);
            break;
        case 0x2007: {
            uint16_t vram_addr = (uint16_t)(ppu->v & 0x3FFF);
            uint8_t returned_data = ppu->buffered_data;

            if (timed) {
                if (vram_addr >= 0x3F00) {
                    returned_data = (ppu_palette_read(ppu, vram_addr) &
                        ((ppu->ppu_mask & 1) ? 0x30 : 0x3F)) | (ppu->open_bus_value & 0xC0);
                }
                ppu_refresh_open_bus(ppu, nes->cpu.cycle_count, returned_data,
                    vram_addr >= 0x3F00 ? 0x3F : 0xFF);
                ppu->data_read_pipeline |= 1;
                data = returned_data;
                break;
            }
            if (vram_addr >= 0x3F00) {
                uint8_t palette_mask = (ppu->ppu_mask & 1) ? 0x30 : 0x3F;
                returned_data = (ppu_palette_read(ppu, vram_addr) & palette_mask) | (ppu->open_bus_value & 0xC0);
                ppu->buffered_data = nes_ppu_bus_read(nes, (vram_addr & 0x0FFF) | 0x2000);
                ppu_refresh_open_bus(ppu, nes->cpu.cycle_count, returned_data, 0x3F);
            } else {
                ppu->buffered_data = nes_ppu_bus_read(nes, vram_addr);
                ppu_refresh_open_bus(ppu, nes->cpu.cycle_count, returned_data, 0xFF);
            }

            ppu_increment_after_2007(ppu);
            nes_ppu_bus_read(nes, ppu->v & 0x3FFF);
            data = returned_data;
            break;
        }
    }
    return data;
}

uint8_t ppu_read_reg(NES *nes, uint16_t address) {
    return ppu_read_register(nes, address, false);
}
uint8_t ppu_read_reg_timed(NES *nes, uint16_t address) {
    return ppu_read_register(nes, address, true);
}

static void ppu_write_register(NES *nes, uint16_t address, uint8_t data, bool timed) {
    PPU2C02 *ppu = &nes->ppu;
    ppu_update_open_bus_decay(ppu, nes->cpu.cycle_count);
    ppu_refresh_open_bus(ppu, nes->cpu.cycle_count, data, 0xFF);

    switch (address & 0x2007) {
        case 0x2000:
            // A short NMI pulse at vblank start can be cancelled by
            // disabling the output before the CPU samples it, just as
            // with a status read in the same two-dot window.
            if ((ppu->ppu_ctrl & 0x80) && !(data & 0x80) && ppu->scanline == ppu_vblank_line(ppu) &&
                ppu->cycle >= 2 && ppu->cycle <= 3) {
                nes->cpu.nmi_edge = false;
                nes->cpu.nmi_delayed = false;
            }
            ppu->ppu_ctrl = data;
            ppu->t = (uint16_t)((ppu->t & 0xF3FF) | (((uint16_t)data & 0x03) << 10));
            ppu_update_nmi(ppu, nes);
            break;
        case 0x2001:
            if (timed) {
                ppu->mask_pending = data;
                ppu->mask_delay = 3;
            } else ppu->ppu_mask = data;
            break;
        case 0x2003:
            ppu->oam_addr = data;
            break;
        case 0x2004: {
            bool rendering_enabled = (ppu->ppu_mask & 0x18) != 0;
            bool is_rendering_scanline = (ppu->scanline < 240 || ppu->scanline == SCANLINE_PRERENDER);
            if (!(rendering_enabled && is_rendering_scanline)) {
                if ((ppu->oam_addr & 0x03) == 0x02) {
                    data &= 0xE3;
                }
                ppu->oam_ram[ppu->oam_addr++] = data;
            } else {
                ppu->oam_addr = (uint8_t)((ppu->oam_addr + 4) & 0xFC);
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
                ppu->w = 0;
                if (timed) {
                    ppu->address_pending = ppu->t;
                    ppu->address_delay = 3;
                } else {
                    ppu->v = ppu->t;
                    nes_ppu_bus_read(nes, ppu->v & 0x3FFF);
                }
            }
            break;
        case 0x2007:
            nes_ppu_bus_write(nes, ppu->v & 0x3FFF, data);
            ppu_increment_after_2007(ppu);
            nes_ppu_bus_read(nes, ppu->v & 0x3FFF);
            break;
    }
}

void ppu_write_reg(NES *nes, uint16_t address, uint8_t data) {
    ppu_write_register(nes, address, data, false);
}
void ppu_write_reg_timed(NES *nes, uint16_t address, uint8_t data) {
    ppu_write_register(nes, address, data, true);
}

static void ppu_step_shifters(PPU2C02 *ppu) {
    ppu->bg_shifter_pattern_low  <<= 1;
    ppu->bg_shifter_pattern_high = (uint16_t)((ppu->bg_shifter_pattern_high << 1) | 1);
    ppu->bg_shifter_attrib_low   <<= 1;
    ppu->bg_shifter_attrib_high  <<= 1;
}

static void ppu_load_bg_shifters(PPU2C02 *ppu) {
    ppu->bg_shifter_pattern_low  = (ppu->bg_shifter_pattern_low  & 0xFF00) | ppu->bg_next_tile_lsb;
    ppu->bg_shifter_pattern_high = (ppu->bg_shifter_pattern_high & 0xFF00) | ppu->bg_next_tile_msb;
    ppu->bg_shifter_attrib_low   = (ppu->bg_shifter_attrib_low   & 0xFF00) | ((ppu->bg_next_tile_attrib & 0x01) ? 0xFF : 0x00);
    ppu->bg_shifter_attrib_high  = (ppu->bg_shifter_attrib_high  & 0xFF00) | ((ppu->bg_next_tile_attrib & 0x02) ? 0xFF : 0x00);
}

// Called only for visible dots. Resolve the first opaque sprite directly into
// the background pixel, keeping sprite priority and sprite-zero timing per dot.
static uint32_t ppu_compose_pixel(PPU2C02 *ppu, int pixel_x) {
    const uint8_t mask = ppu->ppu_mask;
    uint8_t palette_idx = 0;

    if (!(mask & 0x18)) {
        uint16_t vram_addr = ppu->v & 0x3FFF;
        if (vram_addr >= 0x3F00) {
            palette_idx = vram_addr & 0x1F;
            if ((palette_idx & 0x13) == 0x10) palette_idx &= 0x0F;
        }
        return NES_PALETTE[ppu->palette_ram[palette_idx] & 0x3F];
    }

    uint8_t bg_color = 0;
    if (mask & 0x08) {
        uint16_t bit_mux = 0x8000 >> ppu->x;
        bg_color = ((ppu->bg_shifter_pattern_low & bit_mux) ? 1 : 0) |
                   ((ppu->bg_shifter_pattern_high & bit_mux) ? 2 : 0);
        ppu->bg_palette_index = ((ppu->bg_shifter_attrib_low & bit_mux) ? 1 : 0) |
                                ((ppu->bg_shifter_attrib_high & bit_mux) ? 2 : 0);
        if (bg_color) palette_idx = (ppu->bg_palette_index << 2) | bg_color;
        if (pixel_x < 8 && !(mask & 0x02)) bg_color = 0;
    }

    // Clipped sprites cannot contribute a pixel or a sprite-zero hit.
    if ((mask & 0x10) && (pixel_x >= 8 || (mask & 0x04))) {
        for (int s = 0; s < ppu->scanline_sprite_count; ++s) {
            const ScanlineSprite *spr = &ppu->scanline_sprites[s];
            if ((ppu->sprite_counter_active & (1u << s)) && spr->x) continue;
            unsigned shift = (spr->attributes & 0x40) ? 0 : 7;
            uint8_t color = ((spr->low_byte >> shift) & 1) |
                            (((spr->high_byte >> shift) & 1) << 1);
            if (!color) continue;
            if (bg_color) {
                if (spr->sprite_index == 0 && pixel_x < 255) ppu->ppu_status |= 0x40;
                // The first opaque sprite wins sprite selection even when it
                // sits behind the background; later sprites cannot replace it.
                if (spr->attributes & 0x20) break;
            }
            palette_idx = 0x10 | ((spr->attributes & 0x03) << 2) | color;
            break;
        }
    }

    return NES_PALETTE[ppu->palette_ram[palette_idx] & 0x3F];
}

/* The external address latch and multiplexed data pins are shared by
   rendering fetches and the CPU's delayed PPUDATA read sequencer. */
static void ppu_fetch_tick(NES *nes, bool rendering) {
    PPU2C02 *p = &nes->ppu;
    unsigned dot = (unsigned)p->cycle;
    bool cadence = rendering && dot >= 1 && dot <= 340;
    bool ale = cadence && (dot & 1);
    bool read = cadence && !(dot & 1);
    unsigned phase = (dot - 1) & 7;
    uint16_t address = p->fetch_address;
    unsigned kind = p->fetch_kind;
    bool sprite = dot >= 257 && dot <= 320;
    if (cadence) {
        if (dot >= 337 || phase < (sprite ? 4u : 2u)) {
            address = 0x2000 | (p->v & 0x0FFF);
            kind = 1;
        } else if (!sprite && phase < 4) {
            address = 0x23C0 | (p->v & 0x0C00) | ((p->v >> 4) & 0x38) | ((p->v >> 2) & 7);
            kind = 2;
        } else {
            kind = sprite ? (phase < 6 ? 5u : 6u) : (phase < 6 ? 3u : 4u);
            if (ale) {
                if (sprite) {
                    ScanlineSprite *spr = &p->scanline_sprites[(dot - 257) >> 3];
                    int height = (p->ppu_ctrl & 0x20) ? 16 : 8;
                    int row = ((p->scanline & 255) - p->sprite_fetch_y) & (height - 1);
                    if (spr->attributes & 0x80) row ^= height - 1;
                    unsigned tile = p->sprite_fetch_tile;
                    address = height == 8 ? ((p->ppu_ctrl & 8) ? 0x1000 : 0) | (tile << 4) | row :
                        ((tile & 1) << 12) | ((tile & 0xFE) << 4) | ((row & 8) << 1) | (row & 7);
                } else {
                    address = ((p->ppu_ctrl & 0x10) ? 0x1000 : 0) |
                        (p->bg_next_tile_id << 4) | ppu_get_fine_y(p->v);
                }
                if (phase >= 6) address |= 8;
            }
        }
    }
    bool cpu_ale = (p->data_read_pipeline & 8) != 0;
    bool cpu_read = (p->data_read_pipeline & 32) != 0;
    if (!cadence) {
        if (cpu_ale || cpu_read) address = p->v & 0x3FFF;
        else if (!ppu_rendering_enabled(p))
            nes_ppu_bus_set_address(nes, (p->v & 0x3F00) | p->address_latch);
    }
    if (ale || cpu_ale) {
        if (cpu_read) p->address_latch = p->bus_data;
        else p->address_latch = address & 255;
        p->fetch_address = address;
        p->fetch_kind = (uint8_t)kind;
        nes_ppu_bus_set_address(nes, (address & 0x3F00) | p->address_latch);
    }
    if (read || cpu_read) {
        uint16_t actual = (address & 0x3F00) | p->address_latch;
        if (actual >= 0x3F00) actual &= 0x2FFF;
        p->bus_data = nes_ppu_bus_read(nes, actual);
        if (cpu_read) p->buffered_data = p->bus_data;
        if (read) {
            switch (kind) {
                case 1: p->bg_next_tile_id = p->bus_data; break;
                case 2: {
                    unsigned shift = ((p->v >> 4) & 4) | (p->v & 2);
                    p->bg_next_tile_attrib = (p->bus_data >> shift) & 3;
                    break;
                }
                case 3: p->bg_next_tile_lsb = p->bus_data; break;
                case 4:
                    p->bg_next_tile_msb = p->bus_data;
                    if (nes->nametable_view && (p->ppu_mask & 0x08) &&
                        (p->scanline != ppu_prerender_line(p) || p->cycle >= 321) &&
                        (p->scanline != 239 || p->cycle <= 256)) {
                        uint32_t pixels[8];
                        for (unsigned col = 0; col < 8; ++col) {
                            unsigned shift = 7 - col;
                            unsigned color = ((p->bg_next_tile_lsb >> shift) & 1) |
                                (((p->bg_next_tile_msb >> shift) & 1) << 1);
                            unsigned index = color ? p->bg_next_tile_attrib * 4 + color : 0;
                            pixels[col] = NES_PALETTE[p->palette_ram[index] & 0x3F];
                        }
                        nametable_view_record(nes->nametable_view, p->v, pixels);
                    }
                    break;
                case 5: case 6: {
                    if (!sprite) break;
                    ScanlineSprite *spr = &p->scanline_sprites[(dot - 257) >> 3];
                    int row = (p->scanline & 255) - p->sprite_fetch_y;
                    uint8_t value = row >= 0 && row < ((p->ppu_ctrl & 0x20) ? 16 : 8) ? p->bus_data : 0;
                    if (kind == 5) spr->low_byte = value; else spr->high_byte = value;
                    break;
                }
            }
        }
    }
    if (cpu_read) ppu_increment_after_2007(p);
    p->data_read_pipeline = (p->data_read_pipeline << 1) & 63;
}

void ppu_step(NES *nes) {
    PPU2C02 *ppu = &nes->ppu;
    if (ppu->mask_delay && --ppu->mask_delay == 0) {
        bool was_rendering = (ppu->ppu_mask & 0x18) != 0;
        ppu->ppu_mask = ppu->mask_pending;
        if (was_rendering && !(ppu->ppu_mask & 0x18) &&
            (ppu->scanline < 240 || ppu->scanline == SCANLINE_PRERENDER)) {
            unsigned seed = ppu->oam_eval.secondary_index;
            if (ppu->cycle >= 65 && ppu->cycle <= 256) seed = (seed + 3) & ~3u;
            ppu->corruption_seed = seed & 31;
            ppu->corruption_pending = true;
        }
    }
    const bool rendering_enabled = (ppu->ppu_mask & 0x18) != 0;
    const bool rendering_scanline = (ppu->scanline < SCANLINE_VISIBLE_MAX ||
                                     ppu->scanline == SCANLINE_PRERENDER);

    if (rendering_enabled && rendering_scanline && ppu->corruption_pending) {
        unsigned seed = ppu->corruption_seed;
        memmove(ppu->oam_ram + seed * 8, ppu->oam_ram, 8);
        ppu->oam_eval.secondary[seed] = ppu->oam_eval.secondary[0];
        ppu->corruption_pending = false;
    }

    // Rendering enable takes time to reach the odd-frame skip circuit.
    // Sample it one dot before the final pre-render fetch/skip decision.
    if (ppu->scanline == SCANLINE_PRERENDER && ppu->cycle == 338)
        ppu->odd_skip_rendering = rendering_enabled;

    if (ppu->address_delay && --ppu->address_delay == 0) {
        ppu->v = ppu->address_pending;
        if (!rendering_enabled || !rendering_scanline)
            nes_ppu_bus_set_address(nes, ppu->v & 0x3FFF);
    }

    /* Update the internal OAM evaluation bus and sprite-overflow timing for
       the current PPU dot before CPU-visible register reads can occur. */
    ppu_oam_eval_tick(ppu);
    ppu_fetch_tick(nes, rendering_enabled && rendering_scanline);

    if (ppu->scanline == SCANLINE_PRERENDER && ppu->cycle == 1) {
        ppu->ppu_status &= (uint8_t)~0xE0;
        ppu->nmi_occurred = false;
        ppu->nmi_suppressed = false;
        ppu_update_nmi(ppu, nes);
    }

    if (ppu->scanline == ppu_vblank_line(ppu) && ppu->cycle == 1) {
        if (!ppu->nmi_suppressed) {
            ppu->nmi_occurred = true;
            ppu->ppu_status |= 0x80;
            ppu_update_nmi(ppu, nes);
        }
    }

    if (rendering_enabled && rendering_scanline) {
        if (ppu->cycle == 339) {
            ppu->sprite_counter_active = 0;
            for (int i = 0; i < ppu->scanline_sprite_count; ++i)
                if (ppu->scanline_sprites[i].x) ppu->sprite_counter_active |= 1u << i;
        }
        if ((ppu->cycle >= 1 && ppu->cycle <= 256) ||
            (ppu->cycle >= 321 && ppu->cycle <= 336)) {
            if ((ppu->cycle & 7) == 0) ppu_increment_scroll_x(ppu);
        }

        if (ppu->cycle == 256) {
            ppu_increment_scroll_y(ppu);
        }

        if (ppu->cycle == 257) {
            ppu->v = (ppu->v & 0xFBE0) | (ppu->t & 0x041F);
            ppu->oam_addr = 0;


        }

        if (ppu->scanline == SCANLINE_PRERENDER &&
            ppu->cycle >= 280 && ppu->cycle <= 304) {
            ppu->v = (ppu->v & 0x841F) | (ppu->t & 0x7BE0);
        }

    }

    if (ppu->scanline < SCANLINE_VISIBLE_MAX &&
        ppu->cycle >= 1 && ppu->cycle <= 256) {
        ppu->screen_buffer[ppu->scanline * SCREEN_WIDTH + ppu->cycle - 1] =
            ppu_compose_pixel(ppu, ppu->cycle - 1);
        for (int i = 0; i < ppu->scanline_sprite_count; ++i) {
            ScanlineSprite *spr = &ppu->scanline_sprites[i];
            if (ppu->sprite_counter_active & (1u << i)) {
                if (spr->x && --spr->x == 0) ppu->sprite_counter_active &= ~(1u << i);
            } else if (rendering_enabled) {
                if (spr->attributes & 0x40) {
                    spr->low_byte >>= 1; spr->high_byte >>= 1;
                } else {
                    spr->low_byte <<= 1; spr->high_byte <<= 1;
                }
            }
        }
    }

    // Observe this dot's address after the fetch drives the bus, not the
    // preceding dot's address. MMC3's A12 edge must reach the CPU this cycle.
    if (rendering_enabled && rendering_scanline &&
        ((ppu->cycle >= 1 && ppu->cycle <= 256) ||
         (ppu->cycle >= 321 && ppu->cycle <= 336))) {
        ppu_step_shifters(ppu);
        if ((ppu->cycle & 7) == 0) ppu_load_bg_shifters(ppu);
    }

    if (nes->cart && nes->cart->vtable && nes->cart->vtable->ppu_dot) {
        nes->cart->vtable->ppu_dot(nes->cart, ppu->bus_address);
    }
    if (nes->diagnostics && nes->diagnostics->tracing) diagnostics_lines(nes);
    if (ppu->scanline == SCANLINE_PRERENDER && ppu->cycle == 339 &&
        ppu->region == NES_NTSC && ppu->odd_frame && ppu->odd_skip_rendering) {
        /* Odd NTSC frames omit the final pre-render dot. */
        ppu->cycle = 0;
        ppu->scanline = 0;
        ppu->odd_frame = false;
    } else {
        ppu->cycle++;
        if (ppu->cycle >= CYCLE_SCANLINE_END) {
            ppu->cycle = 0;
            ppu->scanline++;

            if (ppu->scanline == ppu_vblank_line(ppu)) {
                ppu->frame_complete = true;
                nes->frame_ready = true;
            } else if (ppu->scanline > SCANLINE_PRERENDER) {
                ppu->scanline = 0;
                ppu->odd_frame = !ppu->odd_frame;
            }
        }
    }
}
