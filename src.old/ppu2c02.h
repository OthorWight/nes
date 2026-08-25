#ifndef PPU2C02_H
#define PPU2C02_H

#include <stdint.h>
#include <stdbool.h>
#include "cartridge.h"
#include "cpu6502.h"

typedef struct {
    uint8_t x;
    uint8_t low_byte;
    uint8_t high_byte;
    uint8_t attributes;
    uint8_t sprite_index;
} ScanlineSprite;

typedef struct {
    uint8_t vram[2048];
    uint8_t palette_ram[32];
    uint8_t oam_ram[256];

    uint16_t v;
    uint16_t t;
    uint8_t  x;
    uint8_t  w;

    uint8_t ppu_ctrl;
    uint8_t ppu_mask;
    uint8_t ppu_status;
    uint8_t oam_addr;
    uint8_t buffered_data;

    int scanline;
    int cycle;
    bool nmi_occurred;
    bool frame_complete;

    // Background tile cache for active 8-pixel span
    uint8_t bg_tile_low;
    uint8_t bg_tile_high;
    uint8_t bg_palette_index;

    ScanlineSprite scanline_sprites[8];
    int scanline_sprite_count;

    uint32_t screen_buffer[256 * 240];
} PPU2C02;

void ppu_init(PPU2C02 *ppu);
uint8_t ppu_read_reg(PPU2C02 *ppu, Cartridge *cart, uint16_t address);
void ppu_write_reg(PPU2C02 *ppu, Cartridge *cart, uint16_t address, uint8_t data);
void ppu_step(PPU2C02 *ppu, CPU6502 *cpu, Cartridge *cart);

#endif