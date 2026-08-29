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

/* PPU Timing and Screen Dimensions constants */
#define SCREEN_WIDTH         256
#define SCREEN_HEIGHT        240

#define SCANLINE_VISIBLE_MAX 240
#define SCANLINE_PRERENDER   261
#define SCANLINE_POSTRENDER  240

#define CYCLE_PREFETCH_START 321
#define CYCLE_PREFETCH_END   336
#define CYCLE_RENDER_END     256
#define CYCLE_SCANLINE_END   341

static uint8_t ppu_read_nametable_byte(PPU2C02 *ppu, Cartridge *cart, uint16_t address);
static void ppu_write_nametable_byte(PPU2C02 *ppu, Cartridge *cart, uint16_t address, uint8_t data);

/* Loopy register helper functions to improve readability of scroll manipulations */
static inline uint8_t ppu_get_coarse_x(uint16_t v) { return v & 0x001F; }
static inline void ppu_set_coarse_x(uint16_t *v, uint8_t x) { *v = (*v & ~0x001F) | (x & 0x1F); }
static inline uint8_t ppu_get_coarse_y(uint16_t v) { return (v >> 5) & 0x001F; }
static inline void ppu_set_coarse_y(uint16_t *v, uint8_t y) { *v = (*v & ~0x03E0) | ((y & 0x1F) << 5); }
static inline uint8_t ppu_get_nametable_select(uint16_t v) { return (v >> 10) & 0x0003; }
static inline uint8_t ppu_get_fine_y(uint16_t v) { return (v >> 12) & 0x0007; }

static void ppu_update_nmi(PPU2C02 *ppu, CPU6502 *cpu) {
    bool nmi_line = (ppu->ppu_ctrl & 0x80) && (ppu->ppu_status & 0x80);
    if (cpu) {
        cpu_set_nmi_line(cpu, nmi_line);
    }
}

static void ppu_set_a12(PPU2C02 *ppu, bool high, Cartridge *cart, CPU6502 *cpu) {
    if (high) {
        if (!ppu->a12_state) {
            if (ppu->a12_low_counter >= 15) {
                if (cart && cart->clock_irq) {
                    cart->clock_irq(cart, cpu);
                }
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

static void ppu_evaluate_sprites(PPU2C02 *ppu, Cartridge *cart, int target_scanline) {
    ppu->scanline_sprite_count = 0;
    if (target_scanline < 0) return; // Only prevent negative scanlines
    bool rendering_enabled = (ppu->ppu_mask & 0x18) != 0;
    if (!rendering_enabled) return;
    int sprite_height = (ppu->ppu_ctrl & 0x20) ? 16 : 8;

    if (cart != NULL) {
        cart->ppu_sprite_fetch = true;
    }

    for (int i = 0; i < 64; i++) {
        int sprite_y = (int)ppu->oam_ram[i * 4] + 1;
        if (target_scanline >= sprite_y && target_scanline < sprite_y + sprite_height) {
            if (ppu->scanline_sprite_count < 8) {
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
            } else {
                ppu->ppu_status |= 0x20; // Set sprite overflow flag
                break;
            }
        }
    }

    if (cart != NULL) {
        cart->ppu_sprite_fetch = false;
    }
}

static int ppu_calculate_sprite_overflow_cycle(PPU2C02 *ppu, int target_scanline) {
    if (target_scanline < 0) return -1;
    bool rendering_enabled = (ppu->ppu_mask & 0x18) != 0;
    if (!rendering_enabled) return -1;
    int sprite_height = (ppu->ppu_ctrl & 0x20) ? 16 : 8;

    int secondary_oam_count = 0;
    int current_cycle = 65;

    int i = 0;
    for (; i < 64; i++) {
        int sprite_y = (int)ppu->oam_ram[i * 4] + 1;
        if (target_scanline >= sprite_y && target_scanline < sprite_y + sprite_height) {
            if (secondary_oam_count < 8) {
                secondary_oam_count++;
                current_cycle += 8;
            } else {
                current_cycle += 2;
                return current_cycle;
            }
        } else {
            if (secondary_oam_count < 8) {
                current_cycle += 2;
            }
        }
        if (secondary_oam_count == 8) {
            i++;
            break;
        }
    }

    if (secondary_oam_count == 8 && i < 64) {
        int n = i;
        int m = 0;
        while (n < 64 && current_cycle < 256) {
            uint8_t y_val = ppu->oam_ram[n * 4 + m];
            int sprite_y = (int)y_val + 1;
            current_cycle += 2;
            if (target_scanline >= sprite_y && target_scanline < sprite_y + sprite_height) {
                return current_cycle;
            }
            n = n + 1;
            m = (m + 1) & 3;
        }
    }
    return -1;
}

void ppu_init(PPU2C02 *ppu) {
    memset(ppu->vram, 0, sizeof(ppu->vram));
    memset(ppu->palette_ram, 0x0F, sizeof(ppu->palette_ram)); // Initialize palette RAM to 0x0F (black) for Blargg's power_up_palette test
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
    ppu->nmi_suppressed = false;
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
    ppu->open_bus_value = 0;
    ppu->overflow_cycle = -1;
    memset(ppu->open_bus_decay_cycles, 0, sizeof(ppu->open_bus_decay_cycles));
}

static void ppu_update_open_bus_decay(PPU2C02 *ppu, CPU6502 *cpu) {
    if (!cpu) return;
    for (int i = 0; i < 8; i++) {
        if (cpu->cycle_count - ppu->open_bus_decay_cycles[i] > 700000) {
            ppu->open_bus_value &= ~(1 << i);
        }
    }
}

static void ppu_refresh_open_bus(PPU2C02 *ppu, CPU6502 *cpu, uint8_t value, uint8_t driven_mask) {
    if (!cpu) return;
    for (int i = 0; i < 8; i++) {
        if (driven_mask & (1 << i)) {
            ppu->open_bus_decay_cycles[i] = cpu->cycle_count;
            if (value & (1 << i)) {
                ppu->open_bus_value |= (1 << i);
            } else {
                ppu->open_bus_value &= ~(1 << i);
            }
        }
    }
}

uint8_t ppu_read_reg(PPU2C02 *ppu, Cartridge *cart, uint16_t address, CPU6502 *cpu) {
    ppu_update_open_bus_decay(ppu, cpu);
    uint8_t data = ppu->open_bus_value;
    switch (address & 0x2007) {
        case 0x2002: {
            uint8_t status = ppu->ppu_status;
            if (ppu->scanline == 241) {
                // NMI suppression window: when reading 1 clock before, at, or 1 clock after setting.
                // Since ppu_read_reg is evaluated after the 3 PPU steps complete,
                // these correspond exactly to ppu->cycle 1, 2, 3.
                if (ppu->cycle >= 1 && ppu->cycle <= 3) {
                    if (cpu) {
                        cpu->nmi_edge = false; // Suppress NMI
                        cpu->nmi_delayed = false;
                    }
                    ppu->nmi_suppressed = true;
                }
                // VBlank reads as 0 when reading 1 clock before, or exactly at VBlank set.
                // These correspond exactly to ppu->cycle 1, 2.
                if (ppu->cycle >= 1 && ppu->cycle <= 2) {
                    status &= ~0x80;
                }
            }
            data = (uint8_t)((status & 0xE0) | (ppu->open_bus_value & 0x1F));
            ppu->ppu_status &= 0x7F;
            ppu->w = 0;
            ppu_refresh_open_bus(ppu, cpu, data, 0xE0); // Only bits 5-7 are driven
            ppu_update_nmi(ppu, cpu);
            break;
        }
        case 0x2004:
            data = ppu->oam_ram[ppu->oam_addr];
            if ((ppu->oam_addr & 0x03) == 0x02) {
                data &= 0xE3;
            }
            ppu_refresh_open_bus(ppu, cpu, data, 0xFF); // All bits are driven
            break;
        case 0x2007:
            data = ppu->buffered_data;
            uint16_t vram_addr = (uint16_t)(ppu->v & 0x3FFF);
            uint8_t data_for_buffer;

            uint8_t returned_data = ppu->buffered_data;

            if (vram_addr >= 0x3F00) {
                uint16_t palette_read_addr = vram_addr & 0x001F;
                if ((palette_read_addr & 0x0013) == 0x0010) {
                    palette_read_addr &= 0x000F;
                }
                    returned_data = (ppu->palette_ram[palette_read_addr] & 0x3F) | (ppu->open_bus_value & 0xC0);
                data_for_buffer = ppu_read_nametable_byte(ppu, cart, (vram_addr & 0x0FFF) | 0x2000);
                ppu_refresh_open_bus(ppu, cpu, returned_data, 0x3F); // Palette reads only drive bits 0-5
            } else if (vram_addr <= 0x1FFF && cart != NULL) {
                data_for_buffer = cart->read_chr(cart, vram_addr);
                ppu_refresh_open_bus(ppu, cpu, returned_data, 0xFF);
            } else { // vram_addr >= 0x2000 && vram_addr <= 0x3EFF
                data_for_buffer = ppu_read_nametable_byte(ppu, cart, vram_addr);
                ppu_refresh_open_bus(ppu, cpu, returned_data, 0xFF);
            }
            ppu->buffered_data = data_for_buffer; // Update the buffer

            ppu->v = (uint16_t)((ppu->v + ((ppu->ppu_ctrl & 0x04) ? 32 : 1)) & 0x7FFF);
            if (cart && cart->mapper_id == 4) {
                ppu_set_a12(ppu, (ppu->v & 0x1000) != 0, cart, cpu);
            }
            data = returned_data; // Set the data to be returned to the CPU
            break;
    }
    return data;
}

void ppu_write_reg(PPU2C02 *ppu, Cartridge *cart, uint16_t address, uint8_t data, CPU6502 *cpu) {
    ppu_update_open_bus_decay(ppu, cpu);
    ppu_refresh_open_bus(ppu, cpu, data, 0xFF); // Writes drive all 8 bits
    switch (address & 0x2007) {
        case 0x2000: {
            ppu->ppu_ctrl = data;
            if (cart != NULL) {
                cart->ppu_sprite_size_8x16 = (data & 0x20) != 0;
            }
            ppu->t = (uint16_t)((ppu->t & 0xF3FF) | (((uint16_t)data & 0x03) << 10));
            ppu_update_nmi(ppu, cpu);
            break;
        }
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
                ppu->oam_ram[ppu->oam_addr] = data;
                ppu->oam_addr++;
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
                ppu->palette_ram[palette_addr] = data & 0x3F; // Mask to 6 bits
            }
            ppu->v = (uint16_t)((ppu->v + ((ppu->ppu_ctrl & 0x04) ? 32 : 1)) & 0x7FFF);
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

static void ppu_fetch_bg_data(PPU2C02 *ppu, Cartridge *cart) {
    uint8_t step = (ppu->cycle - 1) % 8;
    switch (step) {
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
                uint16_t attr_addr = 0x23C0 | (ppu->v & 0x0C00) | ((ppu->v >> 4) & 0x38) | ((ppu->v >> 2) & 0x07);
                uint8_t attr_byte = ppu_read_nametable_byte(ppu, cart, attr_addr);
                uint8_t coarse_x = ppu_get_coarse_x(ppu->v);
                uint8_t coarse_y = ppu_get_coarse_y(ppu->v);
                uint8_t palette_shift = ((coarse_y & 0x02) ? 4 : 0) | ((coarse_x & 0x02) ? 2 : 0);
                ppu->bg_next_tile_attrib = (attr_byte >> palette_shift) & 0x03;
            }
            break;
        }
        case 4: {
            uint8_t fine_y = ppu_get_fine_y(ppu->v);
            if (cart && cart->mapper_id == 5 && cart->mmc5_exram_mode == 1) {
                uint8_t ex_byte = cart->exram[ppu->v & 0x03FF];
                uint32_t bank = (ex_byte & 0x3F) | ((uint32_t)(cart->mmc5_chr_high & 0x03) << 6);
                uint32_t offset = (bank * 4096) + ((uint32_t)ppu->bg_next_tile_id * 16) + fine_y;
                ppu->bg_next_tile_lsb = (cart->chr_rom_size > 0) ? cart->chr_rom[offset % cart->chr_rom_size] : 0;
            } else {
                uint8_t bg_table = (ppu->ppu_ctrl & 0x10) ? 1 : 0;
                uint16_t pattern_addr = (bg_table << 12) | (ppu->bg_next_tile_id << 4) | fine_y;
                ppu->bg_next_tile_lsb = cart->read_chr(cart, pattern_addr);
            }
            break;
        }
        case 6: {
            uint8_t fine_y = ppu_get_fine_y(ppu->v);
            if (cart && cart->mapper_id == 5 && cart->mmc5_exram_mode == 1) {
                uint8_t ex_byte = cart->exram[ppu->v & 0x03FF];
                uint32_t bank = (ex_byte & 0x3F) | ((uint32_t)(cart->mmc5_chr_high & 0x03) << 6);
                uint32_t offset = (bank * 4096) + ((uint32_t)ppu->bg_next_tile_id * 16) + fine_y + 8;
                ppu->bg_next_tile_msb = (cart->chr_rom_size > 0) ? cart->chr_rom[offset % cart->chr_rom_size] : 0;
            } else {
                uint8_t bg_table = (ppu->ppu_ctrl & 0x10) ? 1 : 0;
                uint16_t pattern_addr = (bg_table << 12) | (ppu->bg_next_tile_id << 4) | fine_y;
                ppu->bg_next_tile_msb = cart->read_chr(cart, pattern_addr + 8);
            }
            break;
        }
        case 7: {
            ppu_load_bg_shifters(ppu);
            ppu_increment_scroll_x(ppu);
            break;
        }
    }
}

static void ppu_render_pixel(PPU2C02 *ppu) {
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

        uint16_t final_palette_addr = 0x3F00 | (ppu->bg_palette_index << 2) | bg_color_idx;
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
        uint16_t final_sprite_palette_addr = 0x3F10 | (sprite_palette_idx << 2) | sprite_color_idx;
        final_palette_idx = final_sprite_palette_addr & 0x001F;
        if ((final_palette_idx & 0x0013) == 0x0010) {
            final_palette_idx &= 0x000F;
        }
    }

    if (ppu->scanline < SCANLINE_VISIBLE_MAX) {
        if (rendering_enabled) {
            ppu->screen_buffer[ppu->scanline * SCREEN_WIDTH + ppu->cycle] = NES_PALETTE[ppu->palette_ram[final_palette_idx] & 0x3F];
        } else {
            // If rendering is disabled, display the universal background color (palette_ram[0]).
            // However, if the current VRAM address points to the palette RAM region ($3F00-$3FFF),
            // the PPU outputs the color at that palette address instead.
            uint16_t vram_addr = ppu->v & 0x3FFF;
            uint16_t palette_idx = 0;
            if (vram_addr >= 0x3F00) {
                palette_idx = vram_addr & 0x001F;
                if ((palette_idx & 0x0013) == 0x0010) {
                    palette_idx &= 0x000F;
                }
            }
            ppu->screen_buffer[ppu->scanline * SCREEN_WIDTH + ppu->cycle] = NES_PALETTE[ppu->palette_ram[palette_idx] & 0x3F];
        }
    }
}

void ppu_step(PPU2C02 *ppu, CPU6502 *cpu, Cartridge *cart) {
    bool rendering_enabled = (ppu->ppu_mask & 0x18) != 0;

    if (ppu->cycle == 0) {
        ppu->overflow_cycle = -1;
        if (ppu->scanline == SCANLINE_PRERENDER) {
            ppu->overflow_cycle = ppu_calculate_sprite_overflow_cycle(ppu, 0);
        } else if (ppu->scanline < SCANLINE_VISIBLE_MAX) {
            ppu->overflow_cycle = ppu_calculate_sprite_overflow_cycle(ppu, ppu->scanline + 1);
        }
    }

    if (ppu->cycle == ppu->overflow_cycle) {
        ppu->ppu_status |= 0x20;
    }

    // Clear VBlank and Sprite 0 Hit flags at cycle 2 of pre-render scanline (scanline 261)
    if (ppu->scanline == SCANLINE_PRERENDER && ppu->cycle == 2) {
        ppu->ppu_status &= ~0xE0; // Clear VBlank (0x80), Sprite 0 Hit (0x40), and Sprite Overflow (0x20)
        ppu->nmi_occurred = false; // Clear internal NMI flag
        ppu->nmi_suppressed = false;
        ppu_update_nmi(ppu, cpu);
        if (cart && cart->reset_irq) {
            cart->reset_irq(cart);
        }
    }

    // Set VBlank flag and trigger NMI at cycle 1 of scanline 241
    if (ppu->scanline == 241 && ppu->cycle == 1) {
        ppu->nmi_occurred = true; // Set internal NMI flag
        ppu->ppu_status |= 0x80;   // Explicitly set the VBlank flag in the status register
        if (!ppu->nmi_suppressed) {
            ppu_update_nmi(ppu, cpu);
        }
    }

    if (rendering_enabled) {
        if (ppu->scanline < SCANLINE_VISIBLE_MAX || ppu->scanline == SCANLINE_PRERENDER) {
            if ((ppu->cycle >= 1 && ppu->cycle <= CYCLE_RENDER_END) ||
                (ppu->cycle >= CYCLE_PREFETCH_START && ppu->cycle <= CYCLE_PREFETCH_END)) {
                ppu_step_shifters(ppu);
                ppu_fetch_bg_data(ppu, cart);
            }
        }
    }

    if ((ppu->scanline < SCANLINE_VISIBLE_MAX || ppu->scanline == SCANLINE_PRERENDER) && ppu->cycle < CYCLE_RENDER_END) {
        ppu_render_pixel(ppu);
    }

    if (ppu->cycle == 257) {
        if (ppu->scanline == SCANLINE_PRERENDER) {
            ppu_evaluate_sprites(ppu, cart, 0);
        } else if (ppu->scanline < SCANLINE_VISIBLE_MAX) { // For scanlines 0 to 239
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

    if (ppu->scanline == SCANLINE_PRERENDER && ppu->cycle == 339 && rendering_enabled && ppu->odd_frame) {
        ppu->cycle = 340;
    }

    if (ppu->cycle >= CYCLE_SCANLINE_END) {
        ppu->cycle = 0;
        ppu->scanline++;

        if (ppu->scanline < SCANLINE_VISIBLE_MAX && rendering_enabled) {
            if (cart && cart->mapper_id == 5 && cart->clock_irq) {
                cart->clock_irq(cart, cpu);
            }
        }

        if (ppu->scanline == 241) {
            ppu->frame_complete = true;
        } else if (ppu->scanline >= 262) {
            ppu->scanline = 0;
            ppu->odd_frame = !ppu->odd_frame;
        }
    }

    if (rendering_enabled) {
        if (ppu->scanline < SCANLINE_VISIBLE_MAX || ppu->scanline == SCANLINE_PRERENDER) {
            if (ppu->cycle == 256) {
                ppu_increment_scroll_y(ppu);
            }
            if (ppu->cycle == 257) {
                ppu->v = (ppu->v & 0xFBE0) | (ppu->t & 0x041F);
                ppu->oam_addr = 0;
            }
            if (ppu->scanline == SCANLINE_PRERENDER && ppu->cycle >= 280 && ppu->cycle <= 304) {
                ppu->v = (ppu->v & 0x841F) | (ppu->t & 0x7BE0);
            }
        }
    }
}