#include "ppu2c02.h"
#include <stdio.h>
#include <string.h>

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

static void ppu_increment_scroll_x(PPU2C02 *ppu) {
    if ((ppu->v & 0x001F) == 31) {
        ppu->v &= ~0x001F;
        ppu->v ^= 0x0400;
    } else {
        ppu->v++;
    }
}

static void ppu_increment_scroll_y(PPU2C02 *ppu) {
    if ((ppu->v & 0x7000) != 0x7000) {
        ppu->v += 0x1000;
    } else {
        ppu->v &= ~0x7000;
        int y = (ppu->v & 0x03E0) >> 5;
        if (y == 29) {
            y = 0;
            ppu->v ^= 0x0800;
        } else if (y == 31) {
            y = 0;
        } else {
            y++;
        }
        ppu->v = (uint16_t)((ppu->v & ~0x03E0) | (y << 5));
    }
}

static void ppu_evaluate_sprites(PPU2C02 *ppu, Cartridge *cart, int target_scanline) {
    ppu->scanline_sprite_count = 0;
    if (target_scanline < 0 || target_scanline >= 240) return;
    int sprite_height = (ppu->ppu_ctrl & 0x20) ? 16 : 8;

    if (cart != NULL) {
        cart->ppu_sprite_fetch = true;
    }

    for (int i = 0; i < 64 && ppu->scanline_sprite_count < 8; i++) {
        int sprite_y = (int)ppu->oam_ram[i * 4] + 1;
        if (target_scanline >= sprite_y && target_scanline < sprite_y + sprite_height) {
            uint8_t tile_id  = ppu->oam_ram[i * 4 + 1];
            uint8_t attr     = ppu->oam_ram[i * 4 + 2];
            uint8_t sprite_x = ppu->oam_ram[i * 4 + 3];

            int row = target_scanline - sprite_y;
            if (attr & 0x80) {
                row = (sprite_height - 1) - row;
            }

            uint16_t pattern_addr;
            if (sprite_height == 8) {
                uint8_t sprite_table = (ppu->ppu_ctrl & 0x08) ? 1 : 0;
                pattern_addr = (uint16_t)((sprite_table << 12) | (tile_id << 4) | row);
            } else {
                uint8_t sprite_table = tile_id & 0x01;
                uint8_t actual_tile  = tile_id & 0xFE;
                if (row >= 8) {
                    actual_tile++;
                    row -= 8;
                }
                pattern_addr = (uint16_t)((sprite_table << 12) | (actual_tile << 4) | row);
            }

            ScanlineSprite *spr = &ppu->scanline_sprites[ppu->scanline_sprite_count++];
            spr->x = sprite_x;
            spr->attributes = attr;
            spr->sprite_index = (uint8_t)i;
            spr->low_byte = cart->read_chr(cart, pattern_addr);
            spr->high_byte = cart->read_chr(cart, (uint16_t)(pattern_addr + 8));
        }
    }

    if (cart != NULL) {
        cart->ppu_sprite_fetch = false;
    }
}

void ppu_init(PPU2C02 *ppu) {
    memset(ppu->vram, 0, sizeof(ppu->vram));
    memset(ppu->palette_ram, 0, sizeof(ppu->palette_ram));
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
    ppu->scanline = 0;
    ppu->cycle = 0;
    ppu->nmi_occurred = false;
    ppu->frame_complete = false;
    ppu->scanline_sprite_count = 0;
    ppu->bg_tile_low = 0;
    ppu->bg_tile_high = 0;
    ppu->bg_palette_index = 0;
}

uint8_t ppu_read_reg(PPU2C02 *ppu, Cartridge *cart, uint16_t address) {
    uint8_t data = 0;
    switch (address & 0x2007) {
        case 0x2002:
            data = (uint8_t)((ppu->ppu_status & 0xE0) | (ppu->buffered_data & 0x1F));
            ppu->ppu_status &= 0x7F;
            ppu->w = 0;
            break;
        case 0x2004:
            data = ppu->oam_ram[ppu->oam_addr];
            break;
        case 0x2007:
            data = ppu->buffered_data;
            uint16_t vram_addr = (uint16_t)(ppu->v & 0x3FFF);
            if (vram_addr <= 0x1FFF && cart != NULL) {
                ppu->buffered_data = cart->read_chr(cart, vram_addr);
            } else if (vram_addr >= 0x2000 && vram_addr <= 0x3EFF) {
                uint16_t mirrored_addr = vram_addr & 0x0FFF;
                if (cart->mirroring == MIRROR_VERTICAL) {
                    mirrored_addr &= 0x07FF;
                } else if (cart->mirroring == MIRROR_HORIZONTAL) {
                    mirrored_addr = (uint16_t)((mirrored_addr & 0x03FF) | ((mirrored_addr & 0x0800) >> 1));
                } else if (cart->mirroring == MIRROR_ONE_SCREEN_LOW) {
                    mirrored_addr &= 0x03FF;
                } else if (cart->mirroring == MIRROR_ONE_SCREEN_HIGH) {
                    mirrored_addr = 0x0400 | (mirrored_addr & 0x03FF);
                }
                ppu->buffered_data = ppu->vram[mirrored_addr];
            }
            
            if (vram_addr >= 0x3F00) {
                uint16_t palette_addr = vram_addr & 0x001F;
                if ((palette_addr & 0x0013) == 0x0010) {
                    palette_addr &= 0x000F;
                }
                data = ppu->palette_ram[palette_addr];
            }

            ppu->v += (ppu->ppu_ctrl & 0x04) ? 32 : 1;
            break;
    }
    return data;
}

void ppu_write_reg(PPU2C02 *ppu, Cartridge *cart, uint16_t address, uint8_t data) {
    switch (address & 0x2007) {
        case 0x2000:
            ppu->ppu_ctrl = data;
            if (cart != NULL) {
                cart->ppu_sprite_size_8x16 = (data & 0x20) != 0;
            }
            ppu->t = (uint16_t)((ppu->t & 0xF3FF) | (((uint16_t)data & 0x03) << 10));
            break;
        case 0x2001:
            ppu->ppu_mask = data;
            break;
        case 0x2003:
            ppu->oam_addr = data;
            break;
        case 0x2004:
            ppu->oam_ram[ppu->oam_addr] = data;
            ppu->oam_addr++;
            break;
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
            }
            break;
        case 0x2007: {
            uint16_t vram_write_addr = (uint16_t)(ppu->v & 0x3FFF);
            if (vram_write_addr <= 0x1FFF) {
                if (cart != NULL && cart->write_chr != NULL) {
                    cart->write_chr(cart, vram_write_addr, data);
                }
            } else if (vram_write_addr >= 0x2000 && vram_write_addr <= 0x3EFF) {
                uint16_t mirrored_addr = vram_write_addr & 0x0FFF;
                if (cart->mirroring == MIRROR_VERTICAL) {
                    mirrored_addr &= 0x07FF;
                } else if (cart->mirroring == MIRROR_HORIZONTAL) {
                    mirrored_addr = (uint16_t)((mirrored_addr & 0x03FF) | ((mirrored_addr & 0x0800) >> 1));
                } else if (cart->mirroring == MIRROR_ONE_SCREEN_LOW) {
                    mirrored_addr &= 0x03FF;
                } else if (cart->mirroring == MIRROR_ONE_SCREEN_HIGH) {
                    mirrored_addr = 0x0400 | (mirrored_addr & 0x03FF);
                }
                ppu->vram[mirrored_addr] = data;
            } else if (vram_write_addr >= 0x3F00 && vram_write_addr <= 0x3FFF) {
                uint16_t palette_addr = vram_write_addr & 0x001F;
                if ((palette_addr & 0x0013) == 0x0010) {
                    palette_addr &= 0x000F;
                }
                ppu->palette_ram[palette_addr] = data;
            }
            ppu->v += (ppu->ppu_ctrl & 0x04) ? 32 : 1;
            break;
        }
    }
}

void ppu_step(PPU2C02 *ppu, CPU6502 *cpu, Cartridge *cart) {
    (void)cpu;
    bool rendering_enabled = (ppu->ppu_mask & 0x18) != 0;

    if ((ppu->scanline < 240 || ppu->scanline == 261) && ppu->cycle < 256) {
        uint8_t bg_color_idx = 0;
        uint16_t bg_palette_idx = 0;
        uint8_t fine_x = (uint8_t)((ppu->x + (ppu->cycle & 0x07)) & 0x07);

        // Fetch background tile exactly once per 8-pixel span
        if (rendering_enabled && (ppu->ppu_mask & 0x08)) {
            if (ppu->cycle == 0 || ((ppu->cycle + ppu->x) & 0x07) == 0) {
                uint16_t v_fetch = ppu->v;
                if ((ppu->x + (ppu->cycle & 0x07)) >= 8) {
                    if ((v_fetch & 0x001F) == 31) {
                        v_fetch &= ~0x001F;
                        v_fetch ^= 0x0400;
                    } else {
                        v_fetch++;
                    }
                }

                uint16_t nt_addr = 0x2000 | (v_fetch & 0x0FFF);
                uint16_t mirrored_nt = nt_addr & 0x0FFF;
                if (cart->mirroring == MIRROR_VERTICAL) {
                    mirrored_nt &= 0x07FF;
                } else if (cart->mirroring == MIRROR_HORIZONTAL) {
                    mirrored_nt = (uint16_t)((mirrored_nt & 0x03FF) | ((mirrored_nt & 0x0800) >> 1));
                } else if (cart->mirroring == MIRROR_ONE_SCREEN_LOW) {
                    mirrored_nt &= 0x03FF;
                } else if (cart->mirroring == MIRROR_ONE_SCREEN_HIGH) {
                    mirrored_nt = 0x0400 | (mirrored_nt & 0x03FF);
                }
                uint8_t tile_id = ppu->vram[mirrored_nt];

                uint16_t attr_addr = (uint16_t)(0x23C0 | (v_fetch & 0x0C00) | ((v_fetch >> 4) & 0x38) | ((v_fetch >> 2) & 0x07));
                uint16_t mirrored_attr = attr_addr & 0x0FFF;
                if (cart->mirroring == MIRROR_VERTICAL) {
                    mirrored_attr &= 0x07FF;
                } else if (cart->mirroring == MIRROR_HORIZONTAL) {
                    mirrored_attr = (uint16_t)((mirrored_attr & 0x03FF) | ((mirrored_attr & 0x0800) >> 1));
                } else if (cart->mirroring == MIRROR_ONE_SCREEN_LOW) {
                    mirrored_attr &= 0x03FF;
                } else if (cart->mirroring == MIRROR_ONE_SCREEN_HIGH) {
                    mirrored_attr = 0x0400 | (mirrored_attr & 0x03FF);
                }
                uint8_t attr_byte = ppu->vram[mirrored_attr];

                uint8_t coarse_x = (uint8_t)(v_fetch & 0x001F);
                uint8_t coarse_y = (uint8_t)((v_fetch & 0x03E0) >> 5);
                uint8_t palette_shift = (uint8_t)(((coarse_y & 0x02) ? 4 : 0) | ((coarse_x & 0x02) ? 2 : 0));
                ppu->bg_palette_index = (uint8_t)((attr_byte >> palette_shift) & 0x03);

                uint8_t fine_y = (uint8_t)((v_fetch & 0x7000) >> 12);
                uint8_t bg_table = (ppu->ppu_ctrl & 0x10) ? 1 : 0;
                uint16_t pattern_addr = (uint16_t)((bg_table << 12) | (tile_id << 4) | fine_y);
                ppu->bg_tile_low = cart->read_chr(cart, pattern_addr);
                ppu->bg_tile_high = cart->read_chr(cart, (uint16_t)(pattern_addr + 8));

                            // MMC2 Lookahead trigger: Latch state must be that of the NEXT tile (1-tile pipeline lookahead)
                            if (cart && cart->read_chr && (cart->mapper_id == 9 || cart->mapper_id == 10)) {
                                uint16_t v_next = v_fetch;
                                if ((v_next & 0x001F) == 31) {
                                    v_next &= ~0x001F;
                                    v_next ^= 0x0400;
                                } else {
                                    v_next++;
                                }
                                uint16_t nt_next = 0x2000 | (v_next & 0x0FFF);
                                uint16_t mirrored_nt_next = nt_next & 0x0FFF;
                                if (cart->mirroring == MIRROR_VERTICAL) {
                                    mirrored_nt_next &= 0x07FF;
                                } else if (cart->mirroring == MIRROR_HORIZONTAL) {
                                    mirrored_nt_next = (uint16_t)((mirrored_nt_next & 0x03FF) | ((mirrored_nt_next & 0x0800) >> 1));
                                } else if (cart->mirroring == MIRROR_ONE_SCREEN_LOW) {
                                    mirrored_nt_next &= 0x03FF;
                                } else if (cart->mirroring == MIRROR_ONE_SCREEN_HIGH) {
                                    mirrored_nt_next = 0x0400 | (mirrored_nt_next & 0x03FF);
                                }
                                uint8_t tile_id_next = ppu->vram[mirrored_nt_next];
                                uint8_t fine_y_next = (uint8_t)((v_next & 0x7000) >> 12);
                                uint16_t pattern_addr_next = (uint16_t)((bg_table << 12) | (tile_id_next << 4) | fine_y_next);
                                cart->read_chr(cart, (uint16_t)(pattern_addr_next + 8));
                            }
            }

            uint8_t shift = (uint8_t)(7 - fine_x);
            bg_color_idx = (uint8_t)(((ppu->bg_tile_low >> shift) & 0x01) | (((ppu->bg_tile_high >> shift) & 0x01) << 1));

            uint16_t final_palette_addr = (uint16_t)(0x3F00 | (ppu->bg_palette_index << 2) | bg_color_idx);
            if (bg_color_idx == 0) {
                final_palette_addr = 0x3F00;
            }
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
                if (ppu->cycle >= spr->x && ppu->cycle < spr->x + 8) {
                    int col = ppu->cycle - spr->x;
                    if (spr->attributes & 0x40) {
                        col = 7 - col;
                    }

                    uint8_t shift = (uint8_t)(7 - col);
                    uint8_t pixel_color = (uint8_t)(((spr->low_byte >> shift) & 0x01) |
                                                   (((spr->high_byte >> shift) & 0x01) << 1));

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

        if (ppu->cycle < 8) {
            if (!(ppu->ppu_mask & 0x02)) bg_color_idx = 0;
            if (!(ppu->ppu_mask & 0x04)) sprite_color_idx = 0;
        }

        uint16_t final_palette_idx = bg_palette_idx;
        bool show_sprite = false;

        if (bg_color_idx == 0 && sprite_color_idx != 0) {
            show_sprite = true;
        } else if (bg_color_idx != 0 && sprite_color_idx != 0) {
            bool left_clipped = (ppu->cycle < 8) && (!(ppu->ppu_mask & 0x02) || !(ppu->ppu_mask & 0x04));
            if (sprite_0_active && (ppu->ppu_mask & 0x08) && (ppu->ppu_mask & 0x10) && ppu->cycle < 255 && !left_clipped) {
                if (!(ppu->ppu_status & 0x40)) {
                    ppu->ppu_status |= 0x40; // Sprite 0 Hit
                }
            }
            if (sprite_priority == 0) {
                show_sprite = true;
            }
        }

        if (show_sprite) {
            uint16_t final_sprite_palette_addr = (uint16_t)(0x3F10 | (sprite_palette_idx << 2) | sprite_color_idx);
            final_palette_idx = final_sprite_palette_addr & 0x001F;
            if ((final_palette_idx & 0x0013) == 0x0010) {
                final_palette_idx &= 0x000F;
            }
        }

        if (ppu->scanline < 240) {
            if (rendering_enabled) {
                ppu->screen_buffer[ppu->scanline * 256 + ppu->cycle] = NES_PALETTE[ppu->palette_ram[final_palette_idx] & 0x3F];
            } else {
                ppu->screen_buffer[ppu->scanline * 256 + ppu->cycle] = NES_PALETTE[0];
            }
        }
    }

    // Prefetch sprites for next scanline at cycle 257 (after scanline background fetch completes)
    if (ppu->cycle == 257) {
        if (ppu->scanline == 261) {
            ppu_evaluate_sprites(ppu, cart, 0);
        } else if (ppu->scanline < 239) {
            ppu_evaluate_sprites(ppu, cart, ppu->scanline + 1);
        } else {
            ppu->scanline_sprite_count = 0;
        }
    }

    ppu->cycle++;
    if (ppu->cycle >= 341) {
        ppu->cycle = 0;
        ppu->scanline++;

        if (ppu->scanline == 241) {
            ppu->ppu_status |= 0x80;
            ppu->frame_complete = true;
            if (ppu->ppu_ctrl & 0x80) {
                cpu_trigger_nmi(cpu);
            }
        } else if (ppu->scanline == 261) {
            ppu->ppu_status &= ~0x80;
            ppu->ppu_status &= ~0x40;
            if (cart && cart->reset_irq != NULL) {
                cart->reset_irq(cart);
            }
        } else if (ppu->scanline >= 262) {
            ppu->scanline = 0;
        }
    }

    if (rendering_enabled) {
        if (ppu->cycle == 260 && ppu->scanline < 240) {
            if (cart && cart->clock_irq != NULL) {
                cart->clock_irq(cart, cpu);
            }
        }

        if (ppu->scanline < 240 || ppu->scanline == 261) {
            if (ppu->cycle > 0 && ppu->cycle <= 256 && (ppu->cycle & 0x07) == 0) {
                ppu_increment_scroll_x(ppu);
            }
            if (ppu->cycle == 256) {
                ppu_increment_scroll_y(ppu);
            }
            if (ppu->cycle == 257) {
                ppu->v = (ppu->v & 0xFBE0) | (ppu->t & 0x041F);
            }
            if (ppu->scanline == 261 && ppu->cycle >= 280 && ppu->cycle <= 304) {
                ppu->v = (ppu->v & 0x841F) | (ppu->t & 0x7BE0);
            }

                // MMC2 / Mapper 9 prefetch simulation at cycles 321 and 329
                if (cart && cart->read_chr && (cart->mapper_id == 9 || cart->mapper_id == 10) && (ppu->cycle == 321 || ppu->cycle == 329)) {
                    int offset = (ppu->cycle == 321) ? 0 : 1;
                    uint16_t v_fetch = ppu->v;
                    for (int i = 0; i < offset; i++) {
                        if ((v_fetch & 0x001F) == 31) {
                            v_fetch &= ~0x001F;
                            v_fetch ^= 0x0400;
                        } else {
                            v_fetch++;
                        }
                    }
                    uint16_t nt_addr = 0x2000 | (v_fetch & 0x0FFF);
                    uint16_t mirrored_nt = nt_addr & 0x0FFF;
                    if (cart->mirroring == MIRROR_VERTICAL) {
                        mirrored_nt &= 0x07FF;
                    } else if (cart->mirroring == MIRROR_HORIZONTAL) {
                        mirrored_nt = (uint16_t)((mirrored_nt & 0x03FF) | ((mirrored_nt & 0x0800) >> 1));
                    } else if (cart->mirroring == MIRROR_ONE_SCREEN_LOW) {
                        mirrored_nt &= 0x03FF;
                    } else if (cart->mirroring == MIRROR_ONE_SCREEN_HIGH) {
                        mirrored_nt = 0x0400 | (mirrored_nt & 0x03FF);
                    }
                    uint8_t tile_id = ppu->vram[mirrored_nt];
                    uint8_t fine_y = (uint8_t)((v_fetch & 0x7000) >> 12);
                    uint8_t bg_table = (ppu->ppu_ctrl & 0x10) ? 1 : 0;
                    uint16_t pattern_addr = (uint16_t)((bg_table << 12) | (tile_id << 4) | fine_y);
                    cart->read_chr(cart, pattern_addr);
                    cart->read_chr(cart, (uint16_t)(pattern_addr + 8));
                }
        }
    }
}