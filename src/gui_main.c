#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <dirent.h>
#include "cpu6502.h"
#include "cartridge.h"
#include "ppu2c02.h"
#include "apu2a03.h"
#include <SDL2/SDL.h>

static uint8_t nes_ram[2048];
static uint8_t mock_apu_io[24];
static Cartridge *loaded_cartridge = NULL;
static PPU2C02 nes_ppu;
static APU2A03 nes_apu;

static uint8_t controller_state = 0;
static uint8_t controller_shift = 0;
static char loaded_rom_name[256] = "";

static bool rebinding = false;
static SDL_Keycode control_mappings[8] = {
    SDLK_z,      // Button A (bit 0)
    SDLK_x,      // Button B (bit 1)
    SDLK_SPACE,  // Select   (bit 2)
    SDLK_RETURN, // Start    (bit 3)
    SDLK_UP,     // Up       (bit 4)
    SDLK_DOWN,   // Down     (bit 5)
    SDLK_LEFT,   // Left     (bit 6)
    SDLK_RIGHT   // Right    (bit 7)
};
static const SDL_Keycode default_control_mappings[8] = {
    SDLK_z, SDLK_x, SDLK_SPACE, SDLK_RETURN, SDLK_UP, SDLK_DOWN, SDLK_LEFT, SDLK_RIGHT
};
static const char *button_names[8] = {
    "Button A", "Button B", "Select", "Start",
    "D-Pad Up", "D-Pad Down", "D-Pad Left", "D-Pad Right"
};

typedef enum {
    GUI_STATE_MENU_MAIN,
    GUI_STATE_MENU_LOAD_ROM,
    GUI_STATE_MENU_SAVE_STATE,
    GUI_STATE_MENU_LOAD_STATE,
    GUI_STATE_MENU_SETTINGS,
    GUI_STATE_MENU_CONTROLS,
    GUI_STATE_GAMEPLAY
} GUIState;

static GUIState current_state = GUI_STATE_MENU_MAIN;
static int menu_selection = 1;
static int window_scale = 5; // Default to Maximized (5)
static bool audio_muted = false;
static bool fullscreen = false;

static char rom_files[64][256];
static int rom_file_count = 0;

// Compact embedded 8x8 font representation for ASCII 32-126
static const uint8_t font8x8[95][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // Space
    {0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x00}, // !
    {0x24,0x24,0x24,0x00,0x00,0x00,0x00,0x00}, // "
    {0x24,0x24,0x7E,0x24,0x7E,0x24,0x24,0x00}, // #
    {0x08,0x3E,0x1C,0x08,0x1C,0x3E,0x08,0x00}, // $
    {0x00,0x24,0x54,0x08,0x10,0x2A,0x24,0x00}, // %
    {0x10,0x28,0x18,0x26,0x24,0x24,0x1B,0x00}, // &
    {0x18,0x18,0x08,0x10,0x00,0x00,0x00,0x00}, // '
    {0x0C,0x18,0x30,0x30,0x30,0x18,0x0C,0x00}, // (
    {0x30,0x18,0x0C,0x0C,0x0C,0x18,0x30,0x00}, // )
    {0x00,0x10,0x54,0x38,0x54,0x10,0x00,0x00}, // *
    {0x00,0x08,0x08,0x3E,0x08,0x08,0x00,0x00}, // +
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x08}, // ,
    {0x00,0x00,0x00,0x3E,0x00,0x00,0x00,0x00}, // -
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00}, // .
    {0x00,0x04,0x08,0x10,0x20,0x40,0x00,0x00}, // /
    {0x3C,0x42,0x46,0x4A,0x52,0x62,0x3C,0x00}, // 0
    {0x18,0x28,0x08,0x08,0x08,0x08,0x3E,0x00}, // 1
    {0x3C,0x42,0x02,0x3C,0x40,0x40,0x7E,0x00}, // 2
    {0x3C,0x42,0x02,0x1C,0x02,0x42,0x3C,0x00}, // 3
    {0x08,0x18,0x28,0x48,0x7E,0x08,0x08,0x00}, // 4
    {0x7E,0x40,0x7C,0x02,0x02,0x42,0x3C,0x00}, // 5
    {0x3C,0x40,0x7C,0x42,0x42,0x42,0x3C,0x00}, // 6
    {0x7E,0x42,0x02,0x04,0x08,0x10,0x10,0x00}, // 7
    {0x3C,0x42,0x42,0x3C,0x42,0x42,0x3C,0x00}, // 8
    {0x3C,0x42,0x42,0x3E,0x02,0x02,0x3C,0x00}, // 9
    {0x00,0x18,0x18,0x00,0x18,0x18,0x00,0x00}, // :
    {0x00,0x18,0x18,0x00,0x18,0x18,0x08,0x00}, // ;
    {0x0C,0x18,0x30,0x60,0x30,0x18,0x0C,0x00}, // <
    {0x00,0x00,0x7E,0x00,0x7E,0x00,0x00,0x00}, // =
    {0x30,0x18,0x0C,0x06,0x0C,0x18,0x30,0x00}, // >
    {0x3C,0x42,0x02,0x0C,0x10,0x00,0x10,0x00}, // ?
    {0x3C,0x42,0x5A,0x5A,0x58,0x40,0x3C,0x00}, // @
    {0x18,0x24,0x42,0x42,0x7E,0x42,0x42,0x00}, // A
    {0x7C,0x22,0x22,0x3C,0x22,0x22,0x7C,0x00}, // B
    {0x3C,0x42,0x40,0x40,0x40,0x42,0x3C,0x00}, // C
    {0x78,0x24,0x22,0x22,0x22,0x24,0x78,0x00}, // D
    {0x7E,0x40,0x40,0x78,0x40,0x40,0x7E,0x00}, // E
    {0x7E,0x40,0x40,0x78,0x40,0x40,0x40,0x00}, // F
    {0x3C,0x42,0x40,0x4E,0x42,0x42,0x3C,0x00}, // G
    {0x42,0x42,0x42,0x7E,0x42,0x42,0x42,0x00}, // H
    {0x3E,0x08,0x08,0x08,0x08,0x08,0x3E,0x00}, // I
    {0x02,0x02,0x02,0x02,0x02,0x42,0x3C,0x00}, // J
    {0x44,0x48,0x50,0x60,0x50,0x48,0x44,0x00}, // K
    {0x40,0x40,0x40,0x40,0x40,0x40,0x7E,0x00}, // L
    {0x42,0x66,0x5A,0x42,0x42,0x42,0x42,0x00}, // M
    {0x42,0x62,0x52,0x4A,0x46,0x42,0x42,0x00}, // N
    {0x3C,0x42,0x42,0x42,0x42,0x42,0x3C,0x00}, // O
    {0x7C,0x42,0x42,0x7C,0x40,0x40,0x40,0x00}, // P
    {0x3C,0x42,0x42,0x42,0x4A,0x44,0x3A,0x00}, // Q
    {0x7C,0x42,0x42,0x7C,0x48,0x44,0x42,0x00}, // R
    {0x3C,0x42,0x40,0x3C,0x02,0x42,0x3C,0x00}, // S
    {0x7E,0x08,0x08,0x08,0x08,0x08,0x08,0x00}, // T
    {0x42,0x42,0x42,0x42,0x42,0x42,0x3C,0x00}, // U
    {0x42,0x42,0x42,0x42,0x42,0x24,0x18,0x00}, // V
    {0x42,0x42,0x42,0x42,0x5A,0x66,0x42,0x00}, // W
    {0x42,0x24,0x18,0x18,0x24,0x42,0x42,0x00}, // X
    {0x42,0x42,0x24,0x18,0x08,0x08,0x08,0x00}, // Y
    {0x7E,0x02,0x04,0x08,0x10,0x20,0x7E,0x00}, // Z
    {0x3C,0x20,0x20,0x20,0x20,0x20,0x3C,0x00}, // [
    {0x00,0x40,0x20,0x10,0x08,0x04,0x02,0x00}, // Backslash
    {0x3C,0x02,0x02,0x02,0x02,0x02,0x3C,0x00}, // ]
    {0x08,0x14,0x22,0x00,0x00,0x00,0x00,0x00}, // ^
    {0x00,0x00,0x00,0x00,0x00,0x00,0x7E,0x00}, // _
    {0x18,0x18,0x10,0x08,0x00,0x00,0x00,0x00}, // `
    {0x00,0x00,0x3C,0x02,0x3E,0x42,0x3E,0x00}, // a
    {0x40,0x40,0x7C,0x42,0x42,0x42,0x7C,0x00}, // b
    {0x00,0x00,0x3C,0x40,0x40,0x42,0x3C,0x00}, // c
    {0x02,0x02,0x3E,0x42,0x42,0x42,0x3E,0x00}, // d
    {0x00,0x00,0x3C,0x42,0x7E,0x40,0x3C,0x00}, // e
    {0x1C,0x20,0x78,0x20,0x20,0x20,0x20,0x00}, // f
    {0x00,0x3E,0x42,0x42,0x3E,0x02,0x3C,0x00}, // g
    {0x40,0x40,0x7C,0x42,0x42,0x42,0x42,0x00}, // h
    {0x08,0x00,0x18,0x08,0x08,0x08,0x1C,0x00}, // i
    {0x02,0x00,0x06,0x02,0x02,0x42,0x3C,0x00}, // j
    {0x40,0x44,0x48,0x50,0x60,0x50,0x44,0x00}, // k
    {0x18,0x08,0x08,0x08,0x08,0x08,0x1C,0x00}, // l
    {0x00,0x00,0x7C,0x52,0x52,0x52,0x52,0x00}, // m
    {0x00,0x00,0x7C,0x42,0x42,0x42,0x42,0x00}, // n
    {0x00,0x00,0x3C,0x42,0x42,0x42,0x3C,0x00}, // o
    {0x00,0x00,0x7C,0x42,0x42,0x7C,0x40,0x40}, // p
    {0x00,0x00,0x3E,0x42,0x42,0x3E,0x02,0x02}, // q
    {0x00,0x00,0x5C,0x62,0x40,0x40,0x40,0x00}, // r
    {0x00,0x00,0x3E,0x40,0x3C,0x02,0x7C,0x00}, // s
    {0x20,0x20,0x78,0x20,0x20,0x20,0x1C,0x00}, // t
    {0x00,0x00,0x42,0x42,0x42,0x42,0x3E,0x00}, // u
    {0x00,0x00,0x42,0x42,0x42,0x24,0x18,0x00}, // v
    {0x00,0x00,0x42,0x42,0x5A,0x5A,0x24,0x00}, // w
    {0x00,0x00,0x42,0x24,0x18,0x24,0x42,0x00}, // x
    {0x00,0x00,0x42,0x42,0x3E,0x02,0x3C,0x00}, // y
    {0x00,0x00,0x7E,0x04,0x08,0x10,0x7E,0x00}, // z
    {0x0C,0x10,0x10,0x20,0x10,0x10,0x0C,0x00}, // {
    {0x08,0x08,0x08,0x00,0x08,0x08,0x08,0x00}, // |
    {0x30,0x08,0x08,0x04,0x08,0x08,0x30,0x00}, // }
    {0x3A,0x5C,0x00,0x00,0x00,0x00,0x00,0x00}  // ~
};

static void draw_character(SDL_Renderer *renderer, char c, int x, int y, uint32_t color) {
    if (c < 32 || c > 126) return;
    int idx = c - 32;
    uint8_t r = (color >> 16) & 0xFF;
    uint8_t g = (color >> 8) & 0xFF;
    uint8_t b = color & 0xFF;

    SDL_SetRenderDrawColor(renderer, r, g, b, 255);
    for (int row = 0; row < 8; row++) {
        uint8_t row_byte = font8x8[idx][row];
        for (int col = 0; col < 8; col++) {
            if (row_byte & (0x80 >> col)) {
                SDL_RenderDrawPoint(renderer, x + col, y + row);
            }
        }
    }
}

static void draw_string(SDL_Renderer *renderer, const char *str, int x, int y, uint32_t color) {
    int cur_x = x;
    while (*str) {
        draw_character(renderer, *str, cur_x, y, color);
        cur_x += 8;
        str++;
    }
}

static void scan_rom_directory(void) {
    rom_file_count = 0;
    DIR *d = opendir(".");
    struct dirent *dir;
    if (d) {
        while ((dir = readdir(d)) != NULL) {
            size_t len = strlen(dir->d_name);
            if (len > 4 && strcmp(dir->d_name + len - 4, ".nes") == 0) {
                strncpy(rom_files[rom_file_count], dir->d_name, 255);
                rom_files[rom_file_count][255] = '\0';
                rom_file_count++;
                if (rom_file_count >= 64) break;
            }
        }
        closedir(d);
    }
}

static void get_state_slot_info(int slot, char *out_buf, size_t max_len, bool is_save_menu) {
    char filepath[64];
    snprintf(filepath, sizeof(filepath), "slot%d.state", slot);
    FILE *f = fopen(filepath, "rb");
    if (!f) {
        if (is_save_menu) {
            snprintf(out_buf, max_len, "Slot %d: [Empty - Press Enter to Save]", slot);
        } else {
            snprintf(out_buf, max_len, "Slot %d: [Empty - No Save Data]", slot);
        }
        return;
    }
    uint32_t magic = 0;
    if (fread(&magic, sizeof(magic), 1, f) != 1 || magic != 0x53544154) {
        snprintf(out_buf, max_len, "Slot %d: [Invalid]", slot);
        fclose(f);
        return;
    }
    char rom_meta[64] = {0};
    char time_meta[32] = {0};
    fread(rom_meta, 1, 64, f);
    fread(time_meta, 1, 32, f);
    fclose(f);

    char *ext = strrchr(rom_meta, '.');
    if (ext && strcmp(ext, ".nes") == 0) {
        *ext = '\0';
    }
    snprintf(out_buf, max_len, "Slot %d: %-10.10s %s", slot, rom_meta, time_meta);
}

static uint8_t test_bus_read(void *context, uint16_t address) {
    (void)context;
    if (loaded_cartridge != NULL) {
        if (address <= 0x1FFF) {
            return nes_ram[address & 0x07FF];
        } else if (address >= 0x2000 && address <= 0x3FFF) {
            return ppu_read_reg(&nes_ppu, loaded_cartridge, address);
        } else if (address == 0x4016) {
            uint8_t value = controller_shift & 1;
            controller_shift >>= 1;
            controller_shift |= 0x80;
            return value;
        } else if (address >= 0x4000 && address <= 0x4015) {
            return apu_read_reg(&nes_apu, address);
        } else if (address >= 0x4000 && address <= 0x4017) {
            return mock_apu_io[address - 0x4000];
        } else if (address >= 0x6000 && address <= 0x7FFF) {
            if (loaded_cartridge->prg_ram != NULL) {
                return loaded_cartridge->prg_ram[address - 0x6000];
            }
            return 0;
        } else if (address >= 0x8000) {
            return loaded_cartridge->read_prg(loaded_cartridge, address);
        }
    }
    return 0;
}

static void test_bus_write(void *context, uint16_t address, uint8_t data) {
    (void)context;
    if (loaded_cartridge != NULL) {
        if (address <= 0x1FFF) {
            nes_ram[address & 0x07FF] = data;
        } else if (address >= 0x2000 && address <= 0x3FFF) {
            ppu_write_reg(&nes_ppu, loaded_cartridge, address, data);
        } else if (address == 0x4016) {
            if (data & 1) {
                controller_shift = controller_state;
            }
        } else if (address == 0x4014) {
            uint16_t dma_addr = (uint16_t)(data << 8);
            for (int i = 0; i < 256; i++) {
                nes_ppu.oam_ram[i] = test_bus_read(context, (uint16_t)(dma_addr + i));
            }
        } else if (address >= 0x4000 && address <= 0x4017) {
            apu_write_reg(&nes_apu, address, data);
            mock_apu_io[address - 0x4000] = data;
        } else if (address >= 0x6000 && address <= 0x7FFF) {
            if (loaded_cartridge->mapper_id == 5) {
                loaded_cartridge->write_prg(loaded_cartridge, address, data);
            } else if (loaded_cartridge->prg_ram != NULL) {
                loaded_cartridge->prg_ram[address - 0x6000] = data;
            }
        } else if (address >= 0x8000) {
            loaded_cartridge->write_prg(loaded_cartridge, address, data);
        }
    }
}

static void save_emulator_state(const CPU6502 *cpu, const char *filepath) {
    if (!loaded_cartridge) return;
    FILE *f = fopen(filepath, "wb");
    if (!f) return;
    uint32_t magic = 0x53544154;
    fwrite(&magic, sizeof(magic), 1, f);

    char rom_meta[64] = {0};
    snprintf(rom_meta, sizeof(rom_meta), "%.63s", loaded_rom_name);
    fwrite(rom_meta, 1, 64, f);

    time_t t = time(NULL);
    struct tm *tm_info = localtime(&t);
    char time_meta[32] = {0};
    if (tm_info) {
        strftime(time_meta, sizeof(time_meta), "%m/%d %H:%M", tm_info);
    }
    fwrite(time_meta, 1, 32, f);

    fwrite(nes_ram, 1, sizeof(nes_ram), f);
    fwrite(mock_apu_io, 1, sizeof(mock_apu_io), f);
    fwrite(&controller_state, 1, sizeof(controller_state), f);
    fwrite(&controller_shift, 1, sizeof(controller_shift), f);
    fwrite(cpu, sizeof(CPU6502), 1, f);
    fwrite(nes_ppu.vram, 1, sizeof(nes_ppu.vram), f);
    fwrite(nes_ppu.palette_ram, 1, sizeof(nes_ppu.palette_ram), f);
    fwrite(nes_ppu.oam_ram, 1, sizeof(nes_ppu.oam_ram), f);
    fwrite(&nes_ppu.v, sizeof(nes_ppu.v), 1, f);
    fwrite(&nes_ppu.t, sizeof(nes_ppu.t), 1, f);
    fwrite(&nes_ppu.x, sizeof(nes_ppu.x), 1, f);
    fwrite(&nes_ppu.w, sizeof(nes_ppu.w), 1, f);
    fwrite(&nes_ppu.ppu_ctrl, sizeof(nes_ppu.ppu_ctrl), 1, f);
    fwrite(&nes_ppu.ppu_mask, sizeof(nes_ppu.ppu_mask), 1, f);
    fwrite(&nes_ppu.ppu_status, sizeof(nes_ppu.ppu_status), 1, f);
    fwrite(&nes_ppu.oam_addr, sizeof(nes_ppu.oam_addr), 1, f);
    fwrite(&nes_ppu.buffered_data, sizeof(nes_ppu.buffered_data), 1, f);
    fwrite(&nes_ppu.scanline, sizeof(nes_ppu.scanline), 1, f);
    fwrite(&nes_ppu.cycle, sizeof(nes_ppu.cycle), 1, f);
    fwrite(&nes_ppu.nmi_occurred, sizeof(nes_ppu.nmi_occurred), 1, f);
    fwrite(&nes_ppu.frame_complete, sizeof(nes_ppu.frame_complete), 1, f);
    fwrite(&nes_ppu.scanline_sprite_count, sizeof(nes_ppu.scanline_sprite_count), 1, f);
    fwrite(nes_ppu.scanline_sprites, 1, sizeof(nes_ppu.scanline_sprites), f);
    fwrite(&nes_apu, sizeof(APU2A03), 1, f);
    uint32_t mirroring_val = (uint32_t)loaded_cartridge->mirroring;
    fwrite(&mirroring_val, sizeof(mirroring_val), 1, f);
    fwrite(loaded_cartridge->mapper_state, 1, sizeof(loaded_cartridge->mapper_state), f);
    bool has_prg_ram = (loaded_cartridge->prg_ram != NULL);
    fwrite(&has_prg_ram, sizeof(has_prg_ram), 1, f);
    if (has_prg_ram) {
        fwrite(loaded_cartridge->prg_ram, 1, loaded_cartridge->prg_ram_size, f);
    }
    fclose(f);
}

static void load_emulator_state(CPU6502 *cpu, const char *filepath) {
    if (!loaded_cartridge) return;
    FILE *f = fopen(filepath, "rb");
    if (!f) return;
    uint32_t magic = 0;
    if (fread(&magic, sizeof(magic), 1, f) != 1 || magic != 0x53544154) {
        fclose(f);
        return;
    }
    char rom_meta[64];
    char time_meta[32];
    fread(rom_meta, 1, 64, f);
    fread(time_meta, 1, 32, f);
    fread(nes_ram, 1, sizeof(nes_ram), f);
    fread(mock_apu_io, 1, sizeof(mock_apu_io), f);
    fread(&controller_state, 1, sizeof(controller_state), f);
    fread(&controller_shift, 1, sizeof(controller_shift), f);
    fread(cpu, sizeof(CPU6502), 1, f);
    fread(nes_ppu.vram, 1, sizeof(nes_ppu.vram), f);
    fread(nes_ppu.palette_ram, 1, sizeof(nes_ppu.palette_ram), f);
    fread(nes_ppu.oam_ram, 1, sizeof(nes_ppu.oam_ram), f);
    fread(&nes_ppu.v, sizeof(nes_ppu.v), 1, f);
    fread(&nes_ppu.t, sizeof(nes_ppu.t), 1, f);
    fread(&nes_ppu.x, sizeof(nes_ppu.x), 1, f);
    fread(&nes_ppu.w, sizeof(nes_ppu.w), 1, f);
    fread(&nes_ppu.ppu_ctrl, sizeof(nes_ppu.ppu_ctrl), 1, f);
    fread(&nes_ppu.ppu_mask, sizeof(nes_ppu.ppu_mask), 1, f);
    fread(&nes_ppu.ppu_status, sizeof(nes_ppu.ppu_status), 1, f);
    if (loaded_cartridge != NULL) {
        loaded_cartridge->ppu_sprite_size_8x16 = (nes_ppu.ppu_ctrl & 0x20) != 0;
    }
    fread(&nes_ppu.oam_addr, sizeof(nes_ppu.oam_addr), 1, f);
    fread(&nes_ppu.buffered_data, sizeof(nes_ppu.buffered_data), 1, f);
    fread(&nes_ppu.scanline, sizeof(nes_ppu.scanline), 1, f);
    fread(&nes_ppu.cycle, sizeof(nes_ppu.cycle), 1, f);
    fread(&nes_ppu.nmi_occurred, sizeof(nes_ppu.nmi_occurred), 1, f);
    fread(&nes_ppu.frame_complete, sizeof(nes_ppu.frame_complete), 1, f);
    fread(&nes_ppu.scanline_sprite_count, sizeof(nes_ppu.scanline_sprite_count), 1, f);
    fread(nes_ppu.scanline_sprites, 1, sizeof(nes_ppu.scanline_sprites), f);
    fread(&nes_apu, sizeof(APU2A03), 1, f);
    uint32_t mirroring_val = 0;
    fread(&mirroring_val, sizeof(mirroring_val), 1, f);
    loaded_cartridge->mirroring = (MirroringMode)mirroring_val;
    fread(loaded_cartridge->mapper_state, 1, sizeof(loaded_cartridge->mapper_state), f);
    bool has_prg_ram = false;
    fread(&has_prg_ram, sizeof(has_prg_ram), 1, f);
    if (has_prg_ram) {
        if (loaded_cartridge->prg_ram != NULL) {
            fread(loaded_cartridge->prg_ram, 1, loaded_cartridge->prg_ram_size, f);
        } else {
            fseek(f, 8192, SEEK_CUR);
        }
    }
    fclose(f);
}

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) < 0) {
        return -1;
    }

    SDL_AudioSpec desired, obtained;
    SDL_zero(desired);
    desired.freq = 44100;
    desired.format = AUDIO_F32SYS;
    desired.channels = 1;
    desired.samples = 512;
    SDL_AudioDeviceID audio_device = SDL_OpenAudioDevice(NULL, 0, &desired, &obtained, 0);
    if (audio_device != 0) {
        SDL_PauseAudioDevice(audio_device, 0);
    }

    SDL_Window *window = SDL_CreateWindow(
        "NES Emulator",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        256 * window_scale, 240 * window_scale,
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE
    );
    SDL_Renderer *renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    SDL_RenderSetLogicalSize(renderer, 256, 240);

    if (window_scale == 5) {
        SDL_MaximizeWindow(window);
        SDL_Rect usable_bounds;
        int display_idx = SDL_GetWindowDisplayIndex(window);
        if (display_idx < 0) display_idx = 0;
        if (SDL_GetDisplayUsableBounds(display_idx, &usable_bounds) == 0) {
            SDL_SetWindowSize(window, usable_bounds.w, usable_bounds.h);
        }
    }

    SDL_Texture *texture = SDL_CreateTexture(
        renderer,
        SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STREAMING,
        256, 240
    );

    CPU6502 cpu;
    CPUBus bus;
    bus.bus_context = NULL;
    bus.read = test_bus_read;
    bus.write = test_bus_write;

    cpu_init(&cpu);

    bool running = true;
    SDL_Event event;
    scan_rom_directory();

    while (running) {
        if (current_state == GUI_STATE_GAMEPLAY && loaded_cartridge != NULL) {
            nes_ppu.frame_complete = false;
            while (!nes_ppu.frame_complete) {
                int cycles = cpu_step(&cpu, &bus);
                apu_step(&nes_apu, cycles, &bus, &cpu);
                for (int i = 0; i < cycles * 3; i++) {
                    ppu_step(&nes_ppu, &cpu, loaded_cartridge);
                }
            }

            SDL_UpdateTexture(texture, NULL, nes_ppu.screen_buffer, 256 * sizeof(uint32_t));
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
            SDL_RenderClear(renderer);
            SDL_Rect src_rect = { 8, 0, 240, 240 };
            SDL_RenderCopy(renderer, texture, &src_rect, NULL);
            SDL_RenderPresent(renderer);

            if (audio_device != 0 && nes_apu.audio_buffer_idx > 0) {
                if (!audio_muted) {
                    SDL_QueueAudio(audio_device, nes_apu.audio_buffer, nes_apu.audio_buffer_idx * sizeof(float));
                }
                nes_apu.audio_buffer_idx = 0;
            }

            Uint32 wait_start_tick = SDL_GetTicks();
            while (SDL_GetQueuedAudioSize(audio_device) > 1024 * sizeof(float)) {
                if (SDL_GetTicks() - wait_start_tick > 20) {
                    break;
                }
                SDL_Delay(1);
            }
        } else {
            // Draw dynamic on-screen menu
            SDL_SetRenderDrawColor(renderer, 20, 20, 30, 255);
            SDL_RenderClear(renderer);

            draw_string(renderer, "NES SYSTEM", 64, 20, 0x00FF00);
            draw_string(renderer, "==========", 64, 30, 0x00FF00);

            if (current_state == GUI_STATE_MENU_MAIN) {
                const char *options[] = {
                    "1. Resume Game",
                    "2. Load ROM",
                    "3. Save State Submenu",
                    "4. Load State Submenu",
                    "5. Controls",
                    "6. Settings",
                    "7. Exit Emulator"
                };
                for (int i = 0; i < 7; i++) {
                    uint32_t col;
                    if ((i == 0 || i == 2 || i == 3) && loaded_cartridge == NULL) {
                        col = 0x444444; // Disabled dark gray
                    } else {
                        col = (i == menu_selection) ? 0xFFFFFF : 0x888888;
                    }
                    draw_string(renderer, options[i], 40, 60 + i * 15, col);
                    if (i == menu_selection) {
                        SDL_SetRenderDrawColor(renderer, 0, 255, 0, 255); // Green box
                        SDL_Rect box = { 32, 58 + i * 15, 180, 11 };
                        SDL_RenderDrawRect(renderer, &box);
                    }
                }
            } else if (current_state == GUI_STATE_MENU_CONTROLS) {
                // draw_string(renderer, "REBIND CONTROLS", 68, 30, 0xFFFF00);
                draw_string(renderer, "NES Button  ->  Keyboard Key", 16, 45, 0x00FFFF);
                draw_string(renderer, "-----------------------------", 16, 55, 0x00FFFF);
                for (int i = 0; i < 8; i++) {
                    char buf[64];
                    const char *key_name = SDL_GetKeyName(control_mappings[i]);
                    if (rebinding && i == menu_selection) {
                        snprintf(buf, sizeof(buf), "%-11s -> [PRESS KEY...]", button_names[i]);
                    } else {
                        snprintf(buf, sizeof(buf), "%-11s -> %s", button_names[i], key_name);
                    }
                    uint32_t col = (i == menu_selection) ? 0xFFFFFF : 0x888888;
                    draw_string(renderer, buf, 24, 70 + i * 14, col);
                    if (i == menu_selection) {
                        SDL_SetRenderDrawColor(renderer, 0, 255, 0, 255);
                        SDL_Rect box = { 16, 68 + i * 14, 224, 11 };
                        SDL_RenderDrawRect(renderer, &box);
                    }
                }
                uint32_t def_col = (menu_selection == 8) ? 0xFFFFFF : 0x888888;
                draw_string(renderer, "Restore Defaults", 24, 70 + 8 * 14, def_col);
                if (menu_selection == 8) {
                    SDL_SetRenderDrawColor(renderer, 0, 255, 0, 255);
                    SDL_Rect box = { 16, 68 + 8 * 14, 224, 11 };
                    SDL_RenderDrawRect(renderer, &box);
                }
                draw_string(renderer, "ENTER: Select/Reset | ESC: Return", 16, 215, 0x888888);
            } else if (current_state == GUI_STATE_MENU_LOAD_ROM) {
                draw_string(renderer, "SELECT ROM TO LAUNCH:", 40, 50, 0xFFFF00);
                if (rom_file_count == 0) {
                    draw_string(renderer, "No ROMs found in directory", 40, 70, 0xFF0000);
                } else {
                    for (int i = 0; i < rom_file_count; i++) {
                        uint32_t col = (i == menu_selection) ? 0xFFFFFF : 0x888888;
                        char display_name[32];
                        strncpy(display_name, rom_files[i], 24);
                        display_name[24] = '\0';
                        if (strlen(rom_files[i]) > 24) {
                            strcat(display_name, "...");
                        }
                        draw_string(renderer, display_name, 32, 70 + i * 12, col);
                        if (i == menu_selection) {
                            SDL_SetRenderDrawColor(renderer, 0, 255, 0, 255);
                            SDL_Rect box = { 24, 68 + i * 12, 208, 11 };
                            SDL_RenderDrawRect(renderer, &box);
                        }
                    }
                }
            } else if (current_state == GUI_STATE_MENU_SAVE_STATE) {
                draw_string(renderer, "SELECT SLOT TO SAVE STATE:", 24, 50, 0xFFFF00);
                for (int i = 0; i < 10; i++) {
                    char buf[64];
                    get_state_slot_info(i, buf, sizeof(buf), true);
                    uint32_t col = (i == menu_selection) ? 0xFFFFFF : 0x888888;
                    draw_string(renderer, buf, 8, 70 + i * 14, col);
                    if (i == menu_selection) {
                        SDL_SetRenderDrawColor(renderer, 0, 255, 0, 255);
                        SDL_Rect box = { 6, 68 + i * 14, 244, 11 };
                        SDL_RenderDrawRect(renderer, &box);
                    }
                }
                draw_string(renderer, "UP/DN: Navigate | ENTER: Save | ESC: Back", 8, 220, 0x00FFFF);
            } else if (current_state == GUI_STATE_MENU_LOAD_STATE) {
                draw_string(renderer, "SELECT SLOT TO LOAD STATE:", 24, 50, 0xFFFF00);
                for (int i = 0; i < 10; i++) {
                    char buf[64];
                    get_state_slot_info(i, buf, sizeof(buf), false);
                    uint32_t col = (i == menu_selection) ? 0xFFFFFF : 0x888888;
                    draw_string(renderer, buf, 8, 70 + i * 14, col);
                    if (i == menu_selection) {
                        SDL_SetRenderDrawColor(renderer, 0, 255, 0, 255);
                        SDL_Rect box = { 6, 68 + i * 14, 244, 11 };
                        SDL_RenderDrawRect(renderer, &box);
                    }
                }
                draw_string(renderer, "UP/DN: Navigate | ENTER: Load | ESC: Back", 8, 220, 0x00FFFF);
            } else if (current_state == GUI_STATE_MENU_SETTINGS) {
                char scale_buf[64], mute_buf[64], fs_buf[64];
                if (window_scale == 5) {
                    sprintf(scale_buf, "1. Window Scale: Maximized");
                } else {
                    sprintf(scale_buf, "1. Window Scale: %dx", window_scale);
                }
                sprintf(mute_buf,  "2. Audio Muted:  %s", audio_muted ? "ON" : "OFF");
                sprintf(fs_buf,    "3. Fullscreen:   %s", fullscreen ? "ON" : "OFF");

                uint32_t col0 = (menu_selection == 0) ? 0xFFFFFF : 0x888888;
                uint32_t col1 = (menu_selection == 1) ? 0xFFFFFF : 0x888888;
                uint32_t col2 = (menu_selection == 2) ? 0xFFFFFF : 0x888888;

                draw_string(renderer, scale_buf, 40, 70, col0);
                draw_string(renderer, mute_buf,  40, 90, col1);
                draw_string(renderer, fs_buf,    40, 110, col2);
                
                SDL_SetRenderDrawColor(renderer, 0, 255, 0, 255);
                SDL_Rect box = { 32, 68 + menu_selection * 20, 190, 11 };
                SDL_RenderDrawRect(renderer, &box);

                draw_string(renderer, "Press Enter to Toggle setting", 20, 150, 0xFFFF00);
            }

            SDL_RenderPresent(renderer);
            SDL_Delay(16);
        }

        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                running = false;
            } else if (event.type == SDL_KEYDOWN) {

                if (rebinding) {
                    if (event.key.keysym.sym != SDLK_ESCAPE) {
                        control_mappings[menu_selection] = event.key.keysym.sym;
                    }
                    rebinding = false;
                    break;
                }

                if (event.key.keysym.sym == SDLK_ESCAPE || event.key.keysym.sym == SDLK_F1) {
                    if (current_state == GUI_STATE_GAMEPLAY) {
                        current_state = GUI_STATE_MENU_MAIN;
                        menu_selection = loaded_cartridge ? 0 : 1;
                    } else if (current_state == GUI_STATE_MENU_MAIN) {
                        if (loaded_cartridge != NULL) {
                            current_state = GUI_STATE_GAMEPLAY;
                        }
                    } else {
                        current_state = GUI_STATE_MENU_MAIN;
                        menu_selection = loaded_cartridge ? 0 : 1;
                    }
                    break;
                }

                if (current_state != GUI_STATE_GAMEPLAY) {
                    switch (event.key.keysym.sym) {
                        case SDLK_UP:
                            do {
                                menu_selection--;
                                if (menu_selection < 0) {
                                    if (current_state == GUI_STATE_MENU_MAIN) menu_selection = 6;
                                    else if (current_state == GUI_STATE_MENU_LOAD_ROM) menu_selection = rom_file_count - 1;
                                    else if (current_state == GUI_STATE_MENU_SAVE_STATE || current_state == GUI_STATE_MENU_LOAD_STATE) menu_selection = 9;
                                    else if (current_state == GUI_STATE_MENU_SETTINGS) menu_selection = 2;
                                    else if (current_state == GUI_STATE_MENU_CONTROLS) menu_selection = 8;
                                }
                            } while (current_state == GUI_STATE_MENU_MAIN && loaded_cartridge == NULL && (menu_selection == 0 || menu_selection == 2 || menu_selection == 3));
                            break;
                        case SDLK_DOWN:
                            do {
                                menu_selection++;
                                if (current_state == GUI_STATE_MENU_MAIN && menu_selection > 6) menu_selection = 0;
                                else if (current_state == GUI_STATE_MENU_LOAD_ROM && menu_selection >= rom_file_count) menu_selection = 0;
                                else if ((current_state == GUI_STATE_MENU_SAVE_STATE || current_state == GUI_STATE_MENU_LOAD_STATE) && menu_selection > 9) menu_selection = 0;
                                else if (current_state == GUI_STATE_MENU_SETTINGS && menu_selection > 2) menu_selection = 0;
                                else if (current_state == GUI_STATE_MENU_CONTROLS && menu_selection > 8) menu_selection = 0;
                            } while (current_state == GUI_STATE_MENU_MAIN && loaded_cartridge == NULL && (menu_selection == 0 || menu_selection == 2 || menu_selection == 3));
                            break;
                        case SDLK_BACKSPACE:
                            if (current_state != GUI_STATE_MENU_MAIN) {
                                current_state = GUI_STATE_MENU_MAIN;
                                menu_selection = loaded_cartridge ? 0 : 1;
                            }
                            break;
                        case SDLK_RETURN:
                            if (current_state == GUI_STATE_MENU_MAIN) {
                                if (menu_selection == 0) {
                                    if (loaded_cartridge != NULL) {
                                        current_state = GUI_STATE_GAMEPLAY;
                                    }
                                } else if (menu_selection == 1) {
                                    current_state = GUI_STATE_MENU_LOAD_ROM;
                                    menu_selection = 0;
                                    scan_rom_directory();
                                } else if (menu_selection == 2) {
                                    if (loaded_cartridge != NULL) {
                                        current_state = GUI_STATE_MENU_SAVE_STATE;
                                        menu_selection = 0;
                                    }
                                } else if (menu_selection == 3) {
                                    if (loaded_cartridge != NULL) {
                                        current_state = GUI_STATE_MENU_LOAD_STATE;
                                        menu_selection = 0;
                                    }
                                } else if (menu_selection == 4) {
                                    current_state = GUI_STATE_MENU_CONTROLS;
                                    menu_selection = 0;
                                } else if (menu_selection == 5) {
                                    current_state = GUI_STATE_MENU_SETTINGS;
                                    menu_selection = 0;
                                } else if (menu_selection == 6) {
                                    running = false;
                                }
                            } else if (current_state == GUI_STATE_MENU_CONTROLS) {
                                if (menu_selection == 8) {
                                    for (int i = 0; i < 8; i++) {
                                        control_mappings[i] = default_control_mappings[i];
                                    }
                                } else {
                                    rebinding = true;
                                }
                            } else if (current_state == GUI_STATE_MENU_LOAD_ROM) {
                                if (rom_file_count > 0) {
                                    Cartridge *cart = cartridge_load(rom_files[menu_selection]);
                                    if (cart) {
                                        if (loaded_cartridge) cartridge_free(loaded_cartridge);
                                        loaded_cartridge = cart;
                                        strncpy(loaded_rom_name, rom_files[menu_selection], sizeof(loaded_rom_name) - 1);
                                        loaded_rom_name[sizeof(loaded_rom_name) - 1] = '\0';
                                        memset(nes_ram, 0, sizeof(nes_ram));
                                        ppu_init(&nes_ppu);
                                        apu_init(&nes_apu);
                                        memset(mock_apu_io, 0, sizeof(mock_apu_io));
                                        cpu_init(&cpu);
                                        cpu_trigger_reset(&cpu);
                                        cpu_step(&cpu, &bus);
                                        cpu.decimal_mode = false;
                                        current_state = GUI_STATE_GAMEPLAY;
                                    }
                                }
                            } else if (current_state == GUI_STATE_MENU_SAVE_STATE) {
                                char name_buf[64];
                                sprintf(name_buf, "slot%d.state", menu_selection);
                                save_emulator_state(&cpu, name_buf);
                                current_state = GUI_STATE_GAMEPLAY;
                            } else if (current_state == GUI_STATE_MENU_LOAD_STATE) {
                                char name_buf[64];
                                sprintf(name_buf, "slot%d.state", menu_selection);
                                load_emulator_state(&cpu, name_buf);
                                current_state = GUI_STATE_GAMEPLAY;
                            } else if (current_state == GUI_STATE_MENU_SETTINGS) {
                                if (menu_selection == 0) {
                                    window_scale++;
                                    if (window_scale > 5) window_scale = 1;
                                    if (window_scale == 5) {
                                        SDL_RestoreWindow(window);
                                        SDL_MaximizeWindow(window);
                                        SDL_Rect usable_bounds;
                                        int display_idx = SDL_GetWindowDisplayIndex(window);
                                        if (display_idx < 0) display_idx = 0;
                                        if (SDL_GetDisplayUsableBounds(display_idx, &usable_bounds) == 0) {
                                            SDL_SetWindowSize(window, usable_bounds.w, usable_bounds.h);
                                        }
                                    } else {
                                        SDL_RestoreWindow(window);
                                        SDL_SetWindowSize(window, 256 * window_scale, 240 * window_scale);
                                    }
                                } else if (menu_selection == 1) {
                                    audio_muted = !audio_muted;
                                } else if (menu_selection == 2) {
                                    fullscreen = !fullscreen;
                                    if (fullscreen) {
                                        SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP);
                                    } else {
                                        SDL_SetWindowFullscreen(window, 0);
                                    }
                                }
                            }
                            break;
                        default: break;
                    }
                } else {
                    // Regular gameplay inputs
                    switch (event.key.keysym.sym) {
                        case SDLK_f:
                        case SDLK_F11: {
                            fullscreen = !fullscreen;
                            if (fullscreen) {
                                SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP);
                            } else {
                                SDL_SetWindowFullscreen(window, 0);
                            }
                            break;
                        }
                        case SDLK_F5: {
                            save_emulator_state(&cpu, "quick.state");
                            break;
                        }
                        case SDLK_F8: {
                            load_emulator_state(&cpu, "quick.state");
                            break;
                        }
                        default: {
                            for (int i = 0; i < 8; i++) {
                                if (event.key.keysym.sym == control_mappings[i]) {
                                    controller_state |= (1 << i);
                                }
                            }
                            break;
                        }
                    }
                }
            } else if (event.type == SDL_KEYUP && current_state == GUI_STATE_GAMEPLAY) {
                for (int i = 0; i < 8; i++) {
                    if (event.key.keysym.sym == control_mappings[i]) {
                        controller_state &= ~(1 << i);
                    }
                }
            }
        }
    }

    if (audio_device != 0) {
        SDL_CloseAudioDevice(audio_device);
    }
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    if (loaded_cartridge) {
        cartridge_free(loaded_cartridge);
    }
    return 0;
}