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

static uint8_t ppu_read_nametable_byte(PPU2C02 *ppu, Cartridge *cart, uint16_t address);
static void ppu_write_nametable_byte(PPU2C02 *ppu, Cartridge *cart, uint16_t address, uint8_t data);

static void ppu_set_a12(PPU2C02 *ppu, bool high, Cartridge *cart, CPU6502 *cpu) {
    if (high) {
        if (!ppu->a12_state) {
            if (ppu->a12_low_counter >= 15) {
                if (cart && cart->clock_irq) {
                    cart->clock_irq(cart, cpu);
                } // Missing brace added here
            }
        }
        ppu->a12_state = true;
        ppu->a12_low_counter = 0;
    } else {
        ppu->a12_state = false;
    }
}

static bool ppu_get_rendering_a12(PPU2C02 *ppu) {
    bool rendering_enabled = (ppu->ppu_mask & 0x18) != 0;
    if (!rendering_enabled) {
        return (ppu->v & 0x1000) != 0;
    }
    if (ppu->scanline >= 240 && ppu->scanline != 261) {
        return (ppu->v & 0x1000) != 0;
    }

    if ((ppu->cycle >= 1 && ppu->cycle <= 256) || (ppu->cycle >= 321 && ppu->cycle <= 336)) {
        uint16_t step = (ppu->cycle - 1) % 8;
        if (step < 4) {
            return false;
        } else {
            return (ppu->ppu_ctrl & 0x10) != 0;
        }
    }

    if (ppu->cycle >= 257 && ppu->cycle <= 320) {
        int sprite_idx = (ppu->cycle - 257) / 8;
        int step = (ppu->cycle - 257) % 8;
        if (step >= 4) {
            if (sprite_idx < ppu->scanline_sprite_count) {
                ScanlineSprite *spr = &ppu->scanline_sprites[sprite_idx];
                uint8_t sprite_height = (ppu->ppu_ctrl & 0x20) ? 16 : 8;
                if (sprite_height == 8) {
                    return (ppu->ppu_ctrl & 0x08) != 0;
                } else {
                    uint8_t tile_id = ppu->oam_ram[spr->sprite_index * 4 + 1];
                    return (tile_id & 0x01) != 0;
                }
            } else {
                uint8_t sprite_height = (ppu->ppu_ctrl & 0x20) ? 16 : 8;
                if (sprite_height == 8) {
                    return (ppu->ppu_ctrl & 0x08) != 0;
                } else {
                    return true;
                }
            }
        }
        return false;
    }

    return false;
}

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
    if (!(ppu->ppu_mask & 0x10)) return;
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
    ppu->bg_shifter_pattern_low = 0;
    ppu->bg_shifter_pattern_high = 0;
    ppu->bg_shifter_attrib_low = 0;
    ppu->bg_shifter_attrib_high = 0;
    ppu->bg_next_tile_id = 0;
    ppu->bg_next_tile_attrib = 0;
    ppu->bg_next_tile_lsb = 0;
    ppu->bg_next_tile_msb = 0;
    ppu->odd_frame = false;
    ppu->a12_state = false;
    ppu->a12_low_counter = 0;
}

uint8_t ppu_read_reg(PPU2C02 *ppu, Cartridge *cart, uint16_t address, CPU6502 *cpu) {
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
                ppu->buffered_data = ppu_read_nametable_byte(ppu, cart, vram_addr);
            }
            
            if (vram_addr >= 0x3F00) {
                uint16_t palette_addr = vram_addr & 0x001F;
                if ((palette_addr & 0x0013) == 0x0010) {
                    palette_addr &= 0x000F;
                }
                data = ppu->palette_ram[palette_addr];
            }

            ppu->v += (ppu->ppu_ctrl & 0x04) ? 32 : 1;
            if (cart && cart->mapper_id == 4) {
                ppu_set_a12(ppu, (ppu->v & 0x1000) != 0, cart, cpu);
            }
            break;
    }
    return data;
}

void ppu_write_reg(PPU2C02 *ppu, Cartridge *cart, uint16_t address, uint8_t data, CPU6502 *cpu) {
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
                if (cart && cart->mapper_id == 4) {
                    ppu_set_a12(ppu, (ppu->v & 0x1000) != 0, cart, cpu);
                }
            }
            break;
        case 0x2007: {
            uint16_t vram_write_addr = (uint16_t)(ppu->v & 0x3FFF);
            if (vram_write_addr <= 0x1FFF) {
                if (cart != NULL && cart->write_chr != NULL) {
                    cart->write_chr(cart, vram_write_addr, data);
                }
            } else if (vram_write_addr >= 0x2000 && vram_write_addr <= 0x3EFF) {
                ppu_write_nametable_byte(ppu, cart, vram_write_addr, data);
            } else if (vram_write_addr >= 0x3F00 && vram_write_addr <= 0x3FFF) {
                uint16_t palette_addr = vram_write_addr & 0x001F;
                if ((palette_addr & 0x0013) == 0x0010) {
                    palette_addr &= 0x000F;
                }
                ppu->palette_ram[palette_addr] = data;
            }
            ppu->v += (ppu->ppu_ctrl & 0x04) ? 32 : 1;
            if (cart && cart->mapper_id == 4) {
                ppu_set_a12(ppu, (ppu->v & 0x1000) != 0, cart, cpu);
            }
            break;
        }
    }
}

static uint8_t ppu_read_nametable_byte(PPU2C02 *ppu, Cartridge *cart, uint16_t address) {
    uint16_t nt_addr = address & 0x0FFF;
    if (cart != NULL) {
        if (cart->mapper_id == 19) {
            uint8_t slot = (address >> 10) & 0x03;
            uint8_t bank = cart->mapper_state[8 + slot];
            if (bank >= 0xE0) {
                return ppu->vram[((bank & 0x01) << 10) | (nt_addr & 0x03FF)];
            }
            if (cart->chr_rom_size > 0) {
                uint32_t total_banks = cart->chr_rom_size / 1024;
                return cart->chr_rom[(bank % total_banks) * 1024 + (nt_addr & 0x03FF)];
            }
            return 0;
        }

        if (cart->mapper_id == 5) {
            uint8_t select = (address >> 10) & 0x03;
            uint8_t mode = (cart->mmc5_nametable_ctrl >> (select * 2)) & 0x03;
            if (mode == 0) {
                return ppu->vram[nt_addr & 0x03FF];
            } else if (mode == 1) {
                return ppu->vram[0x0400 | (nt_addr & 0x03FF)];
            } else if (mode == 2) {
                if (cart->mmc5_exram_mode <= 1) {
                    return cart->exram[nt_addr & 0x03FF];
                }
                return 0;
            } else {
                if ((address & 0x03FF) >= 0x03C0) {
                    return cart->mmc5_fill_attr;
                }
                return cart->mmc5_fill_tile;
            }
        }

        if (cart->mirroring == MIRROR_FOUR_SCREEN) {
            if (nt_addr < 0x0800) {
                return ppu->vram[nt_addr];
            } else if (cart->prg_ram) {
                return cart->prg_ram[nt_addr - 0x0800];
            }
        } else if (cart->mirroring == MIRROR_VERTICAL) {
            nt_addr &= 0x07FF;
        } else if (cart->mirroring == MIRROR_HORIZONTAL) {
            nt_addr = (uint16_t)((nt_addr & 0x03FF) | ((nt_addr & 0x0800) >> 1));
        } else if (cart->mirroring == MIRROR_ONE_SCREEN_LOW) {
            nt_addr &= 0x03FF;
        } else if (cart->mirroring == MIRROR_ONE_SCREEN_HIGH) {
            nt_addr = 0x0400 | (nt_addr & 0x03FF);
        }
    }
    return ppu->vram[nt_addr & 0x07FF];
}

static void ppu_write_nametable_byte(PPU2C02 *ppu, Cartridge *cart, uint16_t address, uint8_t data) {
    uint16_t nt_addr = address & 0x0FFF;
    if (cart != NULL) {
        if (cart->mapper_id == 19) {
            uint8_t slot = (address >> 10) & 0x03;
            uint8_t bank = cart->mapper_state[8 + slot];
            if (bank >= 0xE0) {
                ppu->vram[((bank & 0x01) << 10) | (nt_addr & 0x03FF)] = data;
            } else if (cart->chr_rom_size > 0) {
                uint32_t total_banks = cart->chr_rom_size / 1024;
                cart->chr_rom[(bank % total_banks) * 1024 + (nt_addr & 0x03FF)] = data;
            }
            return;
        }

        if (cart->mapper_id == 5) {
            uint8_t select = (address >> 10) & 0x03;
            uint8_t mode = (cart->mmc5_nametable_ctrl >> (select * 2)) & 0x03;
            if (mode == 0) {
                ppu->vram[nt_addr & 0x03FF] = data;
            } else if (mode == 1) {
                ppu->vram[0x0400 | (nt_addr & 0x03FF)] = data;
            } else if (mode == 2) {
                if (cart->mmc5_exram_mode <= 1) {
                    cart->exram[nt_addr & 0x03FF] = data;
                }
            }
            return;
        }

        if (cart->mirroring == MIRROR_FOUR_SCREEN) {
            if (nt_addr < 0x0800) {
                ppu->vram[nt_addr] = data;
            } else if (cart->prg_ram) {
                cart->prg_ram[nt_addr - 0x0800] = data;
            }
            return;
        } else if (cart->mirroring == MIRROR_VERTICAL) {
            nt_addr &= 0x07FF;
        } else if (cart->mirroring == MIRROR_HORIZONTAL) {
            nt_addr = (uint16_t)((nt_addr & 0x03FF) | ((nt_addr & 0x0800) >> 1));
        } else if (cart->mirroring == MIRROR_ONE_SCREEN_LOW) {
            nt_addr &= 0x03FF;
        } else if (cart->mirroring == MIRROR_ONE_SCREEN_HIGH) {
            nt_addr = 0x0400 | (nt_addr & 0x03FF);
        }
    }
    ppu->vram[nt_addr & 0x07FF] = data;
}

void ppu_step(PPU2C02 *ppu, CPU6502 *cpu, Cartridge *cart) {
    bool rendering_enabled = (ppu->ppu_mask & 0x18) != 0;

    if (rendering_enabled) {
        if (ppu->scanline < 240 || ppu->scanline == 261) {
            // Shift on both active scanline cycles and next-scanline prefetch cycles
            if ((ppu->cycle >= 1 && ppu->cycle <= 256) || (ppu->cycle >= 321 && ppu->cycle <= 336)) {
                ppu->bg_shifter_pattern_low <<= 1;
                ppu->bg_shifter_pattern_high <<= 1;
                ppu->bg_shifter_attrib_low <<= 1;
                ppu->bg_shifter_attrib_high <<= 1;

                switch ((ppu->cycle - 1) % 8) {
                    case 0: {
                        uint16_t nt_addr = 0x2000 | (ppu->v & 0x0FFF);
                        ppu->bg_next_tile_id = ppu_read_nametable_byte(ppu, cart, nt_addr);
                        break;
                    }
                    case 2: {
                        if (cart && cart->mapper_id == 5 && cart->mmc5_exram_mode == 1) {
                            uint8_t ex_byte = cart->exram[ppu->v & 0x03FF];
                            ppu->bg_next_tile_attrib = (ex_byte >> 6) & 0x03;
                        } else {
                            uint16_t attr_addr = (uint16_t)(0x23C0 | (ppu->v & 0x0C00) | ((ppu->v >> 4) & 0x38) | ((ppu->v >> 2) & 0x07));
                            uint8_t attr_byte = ppu_read_nametable_byte(ppu, cart, attr_addr);
                            uint8_t coarse_x = (uint8_t)(ppu->v & 0x001F);
                            uint8_t coarse_y = (uint8_t)((ppu->v & 0x03E0) >> 5);
                            uint8_t palette_shift = (uint8_t)(((coarse_y & 0x02) ? 4 : 0) | ((coarse_x & 0x02) ? 2 : 0));
                            ppu->bg_next_tile_attrib = (uint8_t)((attr_byte >> palette_shift) & 0x03);
                        }
                        break;
                    }
                    case 4: {
                        uint8_t fine_y = (uint8_t)((ppu->v & 0x7000) >> 12);
                        if (cart && cart->mapper_id == 5 && cart->mmc5_exram_mode == 1) {
                            uint8_t ex_byte = cart->exram[ppu->v & 0x03FF];
                            uint32_t bank = (ex_byte & 0x3F) | ((uint32_t)(cart->mmc5_chr_high & 0x03) << 6);
                            uint32_t offset = (bank * 4096) + ((uint32_t)ppu->bg_next_tile_id * 16) + fine_y;
                            ppu->bg_next_tile_lsb = (cart->chr_rom_size > 0) ? cart->chr_rom[offset % cart->chr_rom_size] : 0;
                        } else {
                            uint8_t bg_table = (ppu->ppu_ctrl & 0x10) ? 1 : 0;
                            uint16_t pattern_addr = (uint16_t)((bg_table << 12) | (ppu->bg_next_tile_id << 4) | fine_y);
                            ppu->bg_next_tile_lsb = cart->read_chr(cart, pattern_addr);
                        }
                        break;
                    }
                    case 6: {
                        uint8_t fine_y = (uint8_t)((ppu->v & 0x7000) >> 12);
                        if (cart && cart->mapper_id == 5 && cart->mmc5_exram_mode == 1) {
                            uint8_t ex_byte = cart->exram[ppu->v & 0x03FF];
                            uint32_t bank = (ex_byte & 0x3F) | ((uint32_t)(cart->mmc5_chr_high & 0x03) << 6);
                            uint32_t offset = (bank * 4096) + ((uint32_t)ppu->bg_next_tile_id * 16) + fine_y + 8;
                            ppu->bg_next_tile_msb = (cart->chr_rom_size > 0) ? cart->chr_rom[offset % cart->chr_rom_size] : 0;
                        } else {
                            uint8_t bg_table = (ppu->ppu_ctrl & 0x10) ? 1 : 0;
                            uint16_t pattern_addr = (uint16_t)((bg_table << 12) | (ppu->bg_next_tile_id << 4) | fine_y);
                            ppu->bg_next_tile_msb = cart->read_chr(cart, (uint16_t)(pattern_addr + 8));
                        }
                        break;
                    }
                    case 7: {
                        ppu->bg_shifter_pattern_low = (uint16_t)((ppu->bg_shifter_pattern_low & 0xFF00) | ppu->bg_next_tile_lsb);
                        ppu->bg_shifter_pattern_high = (uint16_t)((ppu->bg_shifter_pattern_high & 0xFF00) | ppu->bg_next_tile_msb);
                        ppu->bg_shifter_attrib_low = (uint16_t)((ppu->bg_shifter_attrib_low & 0xFF00) | ((ppu->bg_next_tile_attrib & 0x01) ? 0xFF : 0x00));
                        ppu->bg_shifter_attrib_high = (uint16_t)((ppu->bg_shifter_attrib_high & 0xFF00) | ((ppu->bg_next_tile_attrib & 0x02) ? 0xFF : 0x00));
                        ppu_increment_scroll_x(ppu);
                        break;
                    }
                }
            }
        }
    }

    // Remaining rendering, sprite evaluation, and scanline timing unchanged...
    if ((ppu->scanline < 240 || ppu->scanline == 261) && ppu->cycle < 256) {
        uint8_t bg_color_idx = 0;
        uint16_t bg_palette_idx = 0;

        if (rendering_enabled && (ppu->ppu_mask & 0x08)) {
            uint16_t bit_mux = (uint16_t)(0x8000 >> ppu->x);

            uint8_t p0 = (ppu->bg_shifter_pattern_low & bit_mux) ? 1 : 0;
            uint8_t p1 = (ppu->bg_shifter_pattern_high & bit_mux) ? 1 : 0;
            bg_color_idx = (uint8_t)(p0 | (p1 << 1));

            uint8_t a0 = (ppu->bg_shifter_attrib_low & bit_mux) ? 1 : 0;
            uint8_t a1 = (ppu->bg_shifter_attrib_high & bit_mux) ? 1 : 0;
            ppu->bg_palette_index = (uint8_t)(a0 | (a1 << 1));

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
                    ppu->ppu_status |= 0x40;
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

    if (ppu->cycle == 257) {
        if (ppu->scanline == 261) {
            ppu_evaluate_sprites(ppu, cart, 0);
        } else if (ppu->scanline < 239) {
            ppu_evaluate_sprites(ppu, cart, ppu->scanline + 1);
        } else {
            ppu->scanline_sprite_count = 0;
        }
    }

    if (cart && cart->mapper_id == 4) {
        bool current_a12 = ppu_get_rendering_a12(ppu);
        ppu_set_a12(ppu, current_a12, cart, cpu);
        if (!ppu->a12_state) {
            if (ppu->a12_low_counter < 100) {
                ppu->a12_low_counter++;
            }
        }
    }

    ppu->cycle++;

    if (ppu->scanline == 261 && ppu->cycle == 339 && rendering_enabled && ppu->odd_frame) {
        ppu->cycle = 340;
    }

    if (ppu->cycle >= 341) {
        ppu->cycle = 0;
        ppu->scanline++;

        if (ppu->scanline < 240 && rendering_enabled) {
            if (cart && cart->mapper_id == 5 && cart->clock_irq) {
                cart->clock_irq(cart, cpu);
            }
        }

        if (ppu->scanline == 241) {
            ppu->ppu_status |= 0x80;
            ppu->frame_complete = true;
            if (ppu->ppu_ctrl & 0x80) {
                cpu_pulse_nmi(cpu);
            }
        } else if (ppu->scanline == 261) {
            ppu->ppu_status &= ~0x80;
            ppu->ppu_status &= ~0x40;
            if (cart && cart->reset_irq) {
                cart->reset_irq(cart);
            }
        } else if (ppu->scanline >= 262) {
            ppu->scanline = 0;
            ppu->odd_frame = !ppu->odd_frame;
        }
    }

    if (rendering_enabled) {
        if (ppu->scanline < 240 || ppu->scanline == 261) {
            if (ppu->cycle == 256) {
                ppu_increment_scroll_y(ppu);
            }
            if (ppu->cycle == 257) {
                ppu->v = (ppu->v & 0xFBE0) | (ppu->t & 0x041F);
            }
            if (ppu->scanline == 261 && ppu->cycle >= 280 && ppu->cycle <= 304) {
                ppu->v = (ppu->v & 0x841F) | (ppu->t & 0x7BE0);
            }
        }
    }
}