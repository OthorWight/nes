#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h> // For stat, struct stat, mkdir
#ifdef _WIN32
#include <direct.h> // For _mkdir
#endif
#include <inttypes.h>
#include <math.h>
#include "cpu6502.h"
#include "cartridge.h"
#include "ppu2c02.h"
#include "apu2a03.h"
#include <SDL2/SDL.h>
#include "debugger.h"

uint8_t nes_ram[2048];
uint8_t mock_apu_io[24];
Cartridge *loaded_cartridge = NULL;
static CPU6502 *global_cpu = NULL;
CPUBus nes_bus;
PPU2C02 nes_ppu;
APU2A03 nes_apu;

static uint8_t test_bus_read(void *context, uint16_t address);
static void test_bus_write(void *context, uint16_t address, uint8_t data);
static void test_bus_tick(void *context);
static void test_bus_ppu_tick(void *context);

static uint8_t controller_state = 0;
static uint8_t controller_shift = 0;
static uint8_t controller_strobe = 0;
static char loaded_rom_name[256] = "";
static char save_state_dir[512] = ""; // New global variable for per-ROM save states
static SDL_AudioDeviceID audio_device = 0;
static bool audio_muted = false;

#ifdef _WIN32
#define MKDIR(path) _mkdir(path)
#else
#define MKDIR(path) mkdir(path, 0777)
#endif

static bool console_debug_enabled = false;
static int window_scale = 5;
static bool fullscreen = false;

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

static int master_volume = 100;

static void play_volume_ding(void) {
    if (audio_device == 0 || audio_muted) return;
    SDL_ClearQueuedAudio(audio_device);

    APU2A03 temp_apu;
    apu_init(&temp_apu);

    // Configure Pulse 1 for a classic NES retro chime
    apu_write_reg(&temp_apu, 0x4015, 0x01, NULL); // Enable Pulse 1
    apu_write_reg(&temp_apu, 0x4000, 0xBF, NULL); // 50% duty, constant volume 15, halt length counter
    apu_write_reg(&temp_apu, 0x4002, 0xFD, NULL); // Timer low (A4 @ 440 Hz)
    apu_write_reg(&temp_apu, 0x4003, 0x00, NULL); // Timer high

    // Step the APU until we have generated around 0.09 seconds of samples (3900 samples)
    uint32_t target_samples = 3900;
    int max_steps = 200000;
    for (int i = 0; i < max_steps && temp_apu.audio_buffer_idx < target_samples; i++) {
        apu_step(&temp_apu, NULL, NULL);
    }

    // Apply a custom decay envelope and scale based on master volume
    float vol = (float)master_volume / 100.0f;
    for (uint32_t i = 0; i < temp_apu.audio_buffer_idx; i++) {
        float envelope = 1.0f - ((float)i / temp_apu.audio_buffer_idx);
        temp_apu.audio_buffer[i] *= envelope * vol;
    }

    if (temp_apu.audio_buffer_idx > 0) {
        SDL_QueueAudio(audio_device, temp_apu.audio_buffer, temp_apu.audio_buffer_idx * sizeof(float));
    }
}

static char notification_text[32] = "";
static int notification_timer = 0;

static void show_notification(const char *text) {
    strncpy(notification_text, text, sizeof(notification_text) - 1);
    notification_text[sizeof(notification_text) - 1] = '\0';
    notification_timer = 60; // 60 frames (~1 second)
}

static char state_files[512][256];
static int state_file_count = 0;

static void get_rolling_quicksave_filename(char *out_filename, size_t max_len, bool save) {
    int selected_slot = -1;
    time_t extreme_time = 0;

    for (int i = 0; i < 10; i++) {
        char filepath[1024];
        snprintf(filepath, sizeof(filepath), "%s/quick_%d.state", save_state_dir, i);
        struct stat st;
        if (stat(filepath, &st) == 0) {
            if (selected_slot == -1) {
                extreme_time = st.st_mtime;
                selected_slot = i;
            } else {
                if (save) {
                    if (st.st_mtime < extreme_time) {
                        extreme_time = st.st_mtime;
                        selected_slot = i;
                    }
                } else {
                    if (st.st_mtime > extreme_time) {
                        extreme_time = st.st_mtime;
                        selected_slot = i;
                    }
                }
            }
        } else {
            if (save) {
                selected_slot = i;
                break;
            }
        }
    }

    if (selected_slot == -1) {
        selected_slot = 0;
    }
    snprintf(out_filename, max_len, "quick_%d.state", selected_slot);
}

static int compare_state_files(const void *a, const void *b) {
    char path_a[1024];
    char path_b[1024];
    snprintf(path_a, sizeof(path_a), "%s/%s", save_state_dir, (const char *)a);
    snprintf(path_b, sizeof(path_b), "%s/%s", save_state_dir, (const char *)b);
    struct stat stat_a, stat_b;
    time_t time_a = 0;
    time_t time_b = 0;
    if (stat(path_a, &stat_a) == 0) time_a = stat_a.st_mtime;
    if (stat(path_b, &stat_b) == 0) time_b = stat_b.st_mtime;
    if (time_a < time_b) return 1;
    if (time_a > time_b) return -1;
    return 0;
}

static void scan_save_state_directory(void) {
    state_file_count = 0;
    DIR *d = opendir(save_state_dir);
    struct dirent *dir;
    if (d) {
        while ((dir = readdir(d)) != NULL) {
            size_t len = strlen(dir->d_name);
            if (len > 6 && strcmp(dir->d_name + len - 6, ".state") == 0) {
                strncpy(state_files[state_file_count], dir->d_name, 255);
                state_files[state_file_count][255] = '\0';
                state_file_count++;
                if (state_file_count >= 512) break;
            }
        }
        closedir(d);
    }
    if (state_file_count > 0) {
        qsort(state_files, state_file_count, sizeof(state_files[0]), compare_state_files);
    }
}

static void get_state_file_info(const char *filename, char *out_buf, size_t max_len) {
    char filepath[1024];
    snprintf(filepath, sizeof(filepath), "%s/%s", save_state_dir, filename);
    FILE *f = fopen(filepath, "rb");
    if (!f) {
        snprintf(out_buf, max_len, "%.40s: [Error Opening]", filename);
        return;
    }
    uint32_t magic = 0;
    if (fread(&magic, sizeof(magic), 1, f) != 1 || magic != 0x53544154) {
        snprintf(out_buf, max_len, "%.40s: [Invalid]", filename);
        fclose(f);
        return;
    }
    char rom_meta[64] = {0};
    char time_meta[32] = {0};
    if (fread(rom_meta, 1, 64, f) != 64 || fread(time_meta, 1, 32, f) != 32) {
        snprintf(out_buf, max_len, "%.40s: [Corrupt]", filename);
        fclose(f);
        return;
    }
    fclose(f);

    char *ext = strrchr(rom_meta, '.');
    if (ext && (strcmp(ext, ".nes") == 0 || strcmp(ext, ".NES") == 0)) {
        *ext = '\0';
    }

    char clean_filename[128];
    strncpy(clean_filename, filename, sizeof(clean_filename) - 1);
    clean_filename[sizeof(clean_filename) - 1] = '\0';
    char *state_ext = strrchr(clean_filename, '.');
    if (state_ext) *state_ext = '\0';

    snprintf(out_buf, max_len, "%.40s: %s", clean_filename, time_meta);
}

static void get_clean_rom_name(char *out_buf, size_t max_len) {
    snprintf(out_buf, max_len, "%s", loaded_rom_name);
    char *ext = strrchr(out_buf, '.');
    if (ext) *ext = '\0';
}

static void save_battery_ram(void) {
    if (!loaded_cartridge || !loaded_cartridge->prg_ram || loaded_cartridge->prg_ram_size == 0) return;
    char rom_name_clean[256];
    get_clean_rom_name(rom_name_clean, sizeof(rom_name_clean));
    char filepath[1024];
    snprintf(filepath, sizeof(filepath), "%s/%s.sav", save_state_dir, rom_name_clean);
    FILE *f = fopen(filepath, "wb");
    if (f) {
        fwrite(loaded_cartridge->prg_ram, 1, loaded_cartridge->prg_ram_size, f);
        fclose(f);
    }
}

static void load_battery_ram(void) {
    if (!loaded_cartridge || !loaded_cartridge->prg_ram || loaded_cartridge->prg_ram_size == 0) return;
    char rom_name_clean[256];
    get_clean_rom_name(rom_name_clean, sizeof(rom_name_clean));
    char filepath[1024];
    snprintf(filepath, sizeof(filepath), "%s/%s.sav", save_state_dir, rom_name_clean);
    FILE *f = fopen(filepath, "rb");
    if (f) {
        fread(loaded_cartridge->prg_ram, 1, loaded_cartridge->prg_ram_size, f);
        fclose(f);
    }
}

static void cleanup_default_sav(void) {
    char rom_name_clean[256];
    get_clean_rom_name(rom_name_clean, sizeof(rom_name_clean));
    char default_sav[512];
    snprintf(default_sav, sizeof(default_sav), "%s.sav", rom_name_clean);
    remove(default_sav);
}

static void get_settings_filepath(char *out_path, size_t max_len) {
    char *base_path = SDL_GetBasePath();
    if (base_path) {
        snprintf(out_path, max_len, "%ssaves/settings.bin", base_path);
        SDL_free(base_path);
    } else {
        snprintf(out_path, max_len, "saves/settings.bin");
    }
}

static void save_emulator_settings(void) {
    char filepath[1024];
    get_settings_filepath(filepath, sizeof(filepath));

    // Ensure main saves directory exists
    char saves_dir[1024];
    char *base_path = SDL_GetBasePath();
    if (base_path) {
        snprintf(saves_dir, sizeof(saves_dir), "%ssaves", base_path);
        SDL_free(base_path);
    } else {
        snprintf(saves_dir, sizeof(saves_dir), "saves");
    }
    MKDIR(saves_dir);

    FILE *f = fopen(filepath, "wb");
    if (!f) return;

    uint32_t version = 1;
    fwrite(&version, sizeof(version), 1, f);
    fwrite(&master_volume, sizeof(master_volume), 1, f);
    int temp_muted = audio_muted ? 1 : 0;
    fwrite(&temp_muted, sizeof(temp_muted), 1, f);
    fwrite(&window_scale, sizeof(window_scale), 1, f);
    int temp_fs = fullscreen ? 1 : 0;
    fwrite(&temp_fs, sizeof(temp_fs), 1, f);
    int temp_debug = console_debug_enabled ? 1 : 0;
    fwrite(&temp_debug, sizeof(temp_debug), 1, f);
    fwrite(control_mappings, sizeof(SDL_Keycode), 8, f);
    fclose(f);
}

static void load_emulator_settings(void) {
    char filepath[1024];
    get_settings_filepath(filepath, sizeof(filepath));

    FILE *f = fopen(filepath, "rb");
    if (!f) return;

    uint32_t version = 0;
    if (fread(&version, sizeof(version), 1, f) != 1 || version != 1) {
        fclose(f);
        return;
    }
    fread(&master_volume, sizeof(master_volume), 1, f);
    int temp_muted = 0;
    fread(&temp_muted, sizeof(temp_muted), 1, f);
    audio_muted = (temp_muted != 0);
    fread(&window_scale, sizeof(window_scale), 1, f);
    int temp_fs = 0;
    fread(&temp_fs, sizeof(temp_fs), 1, f);
    fullscreen = (temp_fs != 0);
    int temp_debug = 0;
    fread(&temp_debug, sizeof(temp_debug), 1, f);
    console_debug_enabled = (temp_debug != 0);
    fread(control_mappings, sizeof(SDL_Keycode), 8, f);
    fclose(f);
}

static SDL_GameController *game_controller = NULL;

static const SDL_GameControllerButton controller_button_mappings[8] = {
    SDL_CONTROLLER_BUTTON_A,       // Button A (bit 0)
    SDL_CONTROLLER_BUTTON_B,       // Button B (bit 1)
    SDL_CONTROLLER_BUTTON_BACK,    // Select   (bit 2)
    SDL_CONTROLLER_BUTTON_START,   // Start    (bit 3)
    SDL_CONTROLLER_BUTTON_DPAD_UP,     // Up       (bit 4)
    SDL_CONTROLLER_BUTTON_DPAD_DOWN,   // Down     (bit 5)
    SDL_CONTROLLER_BUTTON_DPAD_LEFT,   // Left     (bit 6)
    SDL_CONTROLLER_BUTTON_DPAD_RIGHT   // Right    (bit 7)
};

#define VIEW_HISTORY_MAX 256
static uint16_t view_history[VIEW_HISTORY_MAX];
static int view_history_count = 0;

static void push_view_history(uint16_t pc) {
    if (view_history_count > 0 && view_history[view_history_count - 1] == pc) {
        return;
    }
    if (view_history_count < VIEW_HISTORY_MAX) {
        view_history[view_history_count++] = pc;
    } else {
        memmove(view_history, view_history + 1, (VIEW_HISTORY_MAX - 1) * sizeof(uint16_t));
        view_history[VIEW_HISTORY_MAX - 1] = pc;
    }
}

static uint16_t pop_view_history(void) {
    if (view_history_count > 1) {
        view_history_count--;
        return view_history[view_history_count - 1];
    }
    if (view_history_count == 1) {
        return view_history[0];
    }
    return 0;
}

static void clear_view_history(uint16_t initial_pc) {
    view_history_count = 0;
    push_view_history(initial_pc);
}

static int mouse_x = 0;
static int mouse_y = 0;
static bool mouse_left_pressed = false;
static uint64_t debug_emu_ticks = 0;
static uint64_t debug_total_ticks = 0;

static bool rebinding = false;
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
static int rom_scroll_offset = 0;

static char rom_files[512][256];
static int rom_file_count = 0;

static const uint8_t font8x8[95][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x00},
    {0x24,0x24,0x24,0x00,0x00,0x00,0x00,0x00},
    {0x24,0x24,0x7E,0x24,0x7E,0x24,0x24,0x00},
    {0x08,0x3E,0x1C,0x08,0x1C,0x3E,0x08,0x00},
    {0x00,0x24,0x54,0x08,0x10,0x2A,0x24,0x00},
    {0x10,0x28,0x18,0x26,0x24,0x24,0x1B,0x00},
    {0x18,0x18,0x08,0x10,0x00,0x00,0x00,0x00},
    {0x0C,0x18,0x30,0x30,0x30,0x18,0x0C,0x00},
    {0x30,0x18,0x0C,0x0C,0x0C,0x18,0x30,0x00},
    {0x00,0x10,0x54,0x38,0x54,0x10,0x00,0x00},
    {0x00,0x08,0x08,0x3E,0x08,0x08,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x08},
    {0x00,0x00,0x00,0x3E,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00},
    {0x00,0x04,0x08,0x10,0x20,0x40,0x00,0x00},
    {0x3C,0x42,0x46,0x4A,0x52,0x62,0x3C,0x00},
    {0x18,0x28,0x08,0x08,0x08,0x08,0x3E,0x00},
    {0x3C,0x42,0x02,0x3C,0x40,0x40,0x7E,0x00},
    {0x3C,0x42,0x02,0x1C,0x02,0x42,0x3C,0x00},
    {0x08,0x18,0x28,0x48,0x7E,0x08,0x08,0x00},
    {0x7E,0x40,0x7C,0x02,0x02,0x42,0x3C,0x00},
    {0x3C,0x40,0x7C,0x42,0x42,0x42,0x3C,0x00},
    {0x7E,0x42,0x02,0x04,0x08,0x10,0x10,0x00},
    {0x3C,0x42,0x42,0x3C,0x42,0x42,0x3C,0x00},
    {0x3C,0x42,0x42,0x3E,0x02,0x02,0x3C,0x00},
    {0x00,0x18,0x18,0x00,0x18,0x18,0x00,0x00},
    {0x00,0x18,0x18,0x00,0x18,0x18,0x08,0x00},
    {0x0C,0x18,0x30,0x60,0x30,0x18,0x0C,0x00},
    {0x00,0x00,0x7E,0x00,0x7E,0x00,0x00,0x00},
    {0x30,0x18,0x0C,0x06,0x0C,0x18,0x30,0x00},
    {0x3C,0x42,0x02,0x0C,0x10,0x00,0x10,0x00},
    {0x3C,0x42,0x5A,0x5A,0x58,0x40,0x3C,0x00},
    {0x18,0x24,0x42,0x42,0x7E,0x42,0x42,0x00},
    {0x7C,0x22,0x22,0x3C,0x22,0x22,0x7C,0x00},
    {0x3C,0x42,0x40,0x40,0x40,0x42,0x3C,0x00},
    {0x78,0x24,0x22,0x22,0x22,0x24,0x78,0x00},
    {0x7E,0x40,0x40,0x78,0x40,0x40,0x7E,0x00},
    {0x7E,0x40,0x40,0x78,0x40,0x40,0x40,0x00},
    {0x3C,0x42,0x40,0x4E,0x42,0x42,0x3C,0x00},
    {0x42,0x42,0x42,0x7E,0x42,0x42,0x42,0x00},
    {0x3E,0x08,0x08,0x08,0x08,0x08,0x3E,0x00},
    {0x02,0x02,0x02,0x02,0x02,0x42,0x3C,0x00},
    {0x44,0x48,0x50,0x60,0x50,0x48,0x44,0x00},
    {0x40,0x40,0x40,0x40,0x40,0x40,0x7E,0x00},
    {0x42,0x66,0x5A,0x42,0x42,0x42,0x42,0x00},
    {0x42,0x62,0x52,0x4A,0x46,0x42,0x42,0x00},
    {0x3C,0x42,0x42,0x42,0x42,0x42,0x3C,0x00},
    {0x7C,0x42,0x42,0x7C,0x40,0x40,0x40,0x00},
    {0x3C,0x42,0x42,0x42,0x4A,0x44,0x3A,0x00},
    {0x7C,0x42,0x42,0x7C,0x48,0x44,0x42,0x00},
    {0x3C,0x42,0x40,0x3C,0x02,0x42,0x3C,0x00},
    {0x7E,0x08,0x08,0x08,0x08,0x08,0x08,0x00},
    {0x42,0x42,0x42,0x42,0x42,0x42,0x3C,0x00},
    {0x42,0x42,0x42,0x42,0x42,0x24,0x18,0x00},
    {0x42,0x42,0x42,0x42,0x5A,0x66,0x42,0x00},
    {0x42,0x24,0x18,0x18,0x24,0x42,0x42,0x00},
    {0x42,0x42,0x24,0x18,0x08,0x08,0x08,0x00},
    {0x7E,0x02,0x04,0x08,0x10,0x20,0x7E,0x00},
    {0x3C,0x20,0x20,0x20,0x20,0x20,0x3C,0x00},
    {0x00,0x40,0x20,0x10,0x08,0x04,0x02,0x00},
    {0x3C,0x02,0x02,0x02,0x02,0x02,0x3C,0x00},
    {0x08,0x14,0x22,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x7E,0x00},
    {0x18,0x18,0x10,0x08,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x3C,0x02,0x3E,0x42,0x3E,0x00},
    {0x40,0x40,0x7C,0x42,0x42,0x42,0x7C,0x00},
    {0x00,0x00,0x3C,0x40,0x40,0x42,0x3C,0x00},
    {0x02,0x02,0x3E,0x42,0x42,0x42,0x3E,0x00},
    {0x00,0x00,0x3C,0x42,0x7E,0x40,0x3C,0x00},
    {0x1C,0x20,0x78,0x20,0x20,0x20,0x20,0x00},
    {0x00,0x3E,0x42,0x42,0x3E,0x02,0x3C,0x00},
    {0x40,0x40,0x7C,0x42,0x42,0x42,0x42,0x00},
    {0x08,0x00,0x18,0x08,0x08,0x08,0x1C,0x00},
    {0x02,0x00,0x06,0x02,0x02,0x42,0x3C,0x00},
    {0x40,0x44,0x48,0x50,0x60,0x50,0x44,0x00},
    {0x18,0x08,0x08,0x08,0x08,0x08,0x1C,0x00},
    {0x00,0x00,0x7C,0x52,0x52,0x52,0x52,0x00},
    {0x00,0x00,0x7C,0x42,0x42,0x42,0x42,0x00},
    {0x00,0x00,0x3C,0x42,0x42,0x42,0x3C,0x00},
    {0x00,0x00,0x7C,0x42,0x42,0x7C,0x40,0x40},
    {0x00,0x00,0x3E,0x42,0x42,0x3E,0x02,0x02},
    {0x00,0x00,0x5C,0x62,0x40,0x40,0x40,0x00},
    {0x00,0x00,0x3E,0x40,0x3C,0x02,0x7C,0x00},
    {0x20,0x20,0x78,0x20,0x20,0x20,0x1C,0x00},
    {0x00,0x00,0x42,0x42,0x42,0x42,0x3E,0x00},
    {0x00,0x00,0x42,0x42,0x42,0x24,0x18,0x00},
    {0x00,0x00,0x42,0x42,0x5A,0x5A,0x24,0x00},
    {0x00,0x00,0x42,0x24,0x18,0x24,0x42,0x00},
    {0x00,0x00,0x42,0x42,0x3E,0x02,0x3C,0x00},
    {0x00,0x00,0x7E,0x04,0x08,0x10,0x7E,0x00},
    {0x0C,0x10,0x10,0x20,0x10,0x10,0x0C,0x00},
    {0x08,0x08,0x08,0x00,0x08,0x08,0x08,0x00},
    {0x30,0x08,0x08,0x04,0x08,0x08,0x30,0x00},
    {0x3A,0x5C,0x00,0x00,0x00,0x00,0x00,0x00}
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

void draw_string(SDL_Renderer *renderer, const char *str, int x, int y, uint32_t color) {
    int cur_x = x;
    while (*str) {
        draw_character(renderer, *str, cur_x, y, color);
        cur_x += 8;
        str++;
    }
}

static void push_synthetic_key(SDL_Keycode sym, Uint32 type) {
    SDL_Event new_event;
    SDL_zero(new_event);
    new_event.type = type;
    new_event.key.state = (type == SDL_KEYDOWN) ? SDL_PRESSED : SDL_RELEASED;
    new_event.key.keysym.sym = sym;
    SDL_PushEvent(&new_event);
}

static int compare_rom_files(const void *a, const void *b) {
    return strcmp((const char *)a, (const char *)b);
}

static void scan_rom_directory(void) {
    rom_file_count = 0;
    DIR *d = opendir(".");
    struct dirent *dir;
    if (d) {
        while ((dir = readdir(d)) != NULL) {
            size_t len = strlen(dir->d_name);
            if (len > 4 && (strcmp(dir->d_name + len - 4, ".nes") == 0 || strcmp(dir->d_name + len - 4, ".NES") == 0)) {
                strncpy(rom_files[rom_file_count], dir->d_name, 255);
                rom_files[rom_file_count][255] = '\0';
                rom_file_count++;
                if (rom_file_count >= 512) break;
            }
        }
        closedir(d);
    }
    if (rom_file_count > 0) {
        qsort(rom_files, rom_file_count, sizeof(rom_files[0]), compare_rom_files);
    }
}

static void update_console_debug(CPU6502 *cpu) {
    static bool last_enabled = false;
    if (!console_debug_enabled) {
        last_enabled = false;
        return;
    }

    if (!last_enabled) {
        printf("\033[2J\033[H");
        fflush(stdout);
        last_enabled = true;
    }

    static double last_fps_calc_time = 0;
    static float fps = 0.0f;
    static uint32_t frame_calc_counter = 0;
    static float cpu_usage = 0.0f;
    static float cpu_speed_mhz = 0.0f;
    static uint64_t last_cycles = 0;

    frame_calc_counter++;
    double current_time = (double)SDL_GetTicks() / 1000.0;
    if (current_time - last_fps_calc_time >= 1.0) {
        double delta_time = current_time - last_fps_calc_time;
        fps = (float)frame_calc_counter / delta_time;
        frame_calc_counter = 0;

        uint64_t current_cycles = cpu->cycle_count;
        cpu_speed_mhz = (float)(current_cycles - last_cycles) / delta_time / 1000000.0f;
        last_cycles = current_cycles;

        if (debug_total_ticks > 0) {
            cpu_usage = ((float)debug_emu_ticks / (float)debug_total_ticks) * 100.0f;
        } else {
            cpu_usage = 0.0f;
        }
        debug_emu_ticks = 0;
        debug_total_ticks = 0;

        last_fps_calc_time = current_time;
    }

    static uint32_t last_update_tick = 0;
    uint32_t current_tick = SDL_GetTicks();
    if (current_tick - last_update_tick < 100) {
        return;
    }
    last_update_tick = current_tick;

    const char *mirror_mode_str = "Unknown";
    if (loaded_cartridge) {
        switch (loaded_cartridge->mirroring) {
            case MIRROR_HORIZONTAL: mirror_mode_str = "Horizontal"; break;
            case MIRROR_VERTICAL: mirror_mode_str = "Vertical"; break;
            case MIRROR_FOUR_SCREEN: mirror_mode_str = "4-Screen"; break;
            case MIRROR_ONE_SCREEN_LOW: mirror_mode_str = "1-Screen Low"; break;
            case MIRROR_ONE_SCREEN_HIGH: mirror_mode_str = "1-Screen High"; break;
        }
    }

    uint8_t sp = cpu->stack_pointer;
    uint8_t s1 = nes_ram[0x0100 | ((sp + 1) & 0xFF)];
    uint8_t s2 = nes_ram[0x0100 | ((sp + 2) & 0xFF)];
    uint8_t s3 = nes_ram[0x0100 | ((sp + 3) & 0xFF)];
    uint8_t s4 = nes_ram[0x0100 | ((sp + 4) & 0xFF)];

    char disasm[128];
    disassemble_instruction(cpu->program_counter, disasm, sizeof(disasm), cpu);

    printf("\033[H");
    printf("\033[1;36m==================================================\033[0m\033[K\n");
    printf("\033[1;32m                 NES EMULATOR DEBUGGER            \033[0m\033[K\n");
    printf("\033[1;36m==================================================\033[0m\033[K\n");
    printf("\033[1mPerformance:\033[0m  %.2f FPS (Target: 60.10 FPS)\033[K\n", fps);
    printf("\033[1mCPU Speed:\033[0m    %.4f MHz (NES standard: 1.7898 MHz)\033[K\n", cpu_speed_mhz);
    printf("\033[1mHost Load:\033[0m    %.2f%%\033[K\n", cpu_usage);
    printf("\033[1mLoaded ROM:\033[0m   %-30.30s\033[K\n", loaded_cartridge ? loaded_rom_name : "[None]");
    printf("\033[1mAudio Queue:\033[0m  %u bytes\033[K\n", SDL_GetQueuedAudioSize(audio_device));
    printf("\033[K\n");

    printf("\033[1;33m--- CPU Registers ---\033[0m\033[K\n");
    printf("PC: 0x%04X   A:  0x%02X   X:  0x%02X   Y:  0x%02X\033[K\n", cpu->program_counter, cpu->accumulator, cpu->index_x, cpu->index_y);
    printf("SP: 0x%02X     P:  0x%02X  [", cpu->stack_pointer, cpu->status_flags);
    printf("%c", (cpu->status_flags & FLAG_NEGATIVE) ? 'N' : '.');
    printf("%c", (cpu->status_flags & FLAG_OVERFLOW_V) ? 'V' : '.');
    printf("-");
    printf("%c", (cpu->status_flags & FLAG_BREAK_COMMAND) ? 'B' : '.');
    printf("%c", (cpu->status_flags & FLAG_DECIMAL_MODE) ? 'D' : '.');
    printf("%c", (cpu->status_flags & FLAG_INTERRUPT_DISABLE) ? 'I' : '.');
    printf("%c", (cpu->status_flags & FLAG_ZERO) ? 'Z' : '.');
    printf("%c", (cpu->status_flags & FLAG_CARRY) ? 'C' : '.');
    printf("]   Cycles: %" PRIu64 "\033[K\n", cpu->cycle_count);
    printf("\033[K\n");

    printf("\033[1;33m--- Execution Context ---\033[0m\033[K\n");
    printf("Instruction: $%04X: %-45.45s\033[K\n", cpu->program_counter, disasm);
    printf("Stack Peek:  SP=0x%02X -> [ %02X %02X %02X %02X ]\033[K\n", sp, s1, s2, s3, s4);
    printf("Pending IRQ: Lines: 0x%02X [ %s%s%s]  NMI Line/Edge: %d/%d\033[K\n",
           cpu->irq_lines,
           (cpu->irq_lines & 1) ? "MAPPER " : "",
           (cpu->irq_lines & 2) ? "FRAME " : "",
           (cpu->irq_lines & 4) ? "DMC " : "",
           cpu->nmi_line, cpu->nmi_edge);
    printf("\033[K\n");

    printf("\033[1;33m--- PPU State ---\033[0m\033[K\n");
    printf("Scanline: %-4d  Cycle: %-4d   Status: 0x%02X\033[K\n", nes_ppu.scanline, nes_ppu.cycle, nes_ppu.ppu_status);
    printf("Ctrl:     0x%02X  Mask:  0x%02X   Scroll V: 0x%04X, T: 0x%04X\033[K\n", nes_ppu.ppu_ctrl, nes_ppu.ppu_mask, nes_ppu.v, nes_ppu.t);
    printf("Fine X:   %-4d  Latch W: %-3d  Frame Parity: %s\033[K\n", nes_ppu.x, nes_ppu.w, nes_ppu.odd_frame ? "Odd" : "Even");
    printf("OAM Addr: 0x%02X  Active Scanline Sprites: %d / 8\033[K\n", nes_ppu.oam_addr, nes_ppu.scanline_sprite_count);
    printf("\033[K\n");

    printf("\033[1;33m--- APU Status ---\033[0m\033[K\n");
    printf("Channels Enabled: Pulse1: %s  Pulse2: %s  Triangle: %s  Noise: %s  DMC: %s\033[K\n",
           nes_apu.pulse_enabled[0] ? "On" : "Off",
           nes_apu.pulse_enabled[1] ? "On" : "Off",
           nes_apu.triangle_enabled ? "On" : "Off",
           nes_apu.noise_enabled ? "On" : "Off",
           nes_apu.dmc_enabled ? "On" : "Off");
    printf("Lengths Remaining: P1:%-3d  P2:%-3d  Tri:%-3d  Noise:%-3d\033[K\n",
           nes_apu.pulse_length_counter[0], nes_apu.pulse_length_counter[1],
           nes_apu.triangle_length_counter, nes_apu.noise_length_counter);
    printf("Frame Sequencer:  Mode: %s  APU IRQ Active: %s\033[K\n",
           nes_apu.frame_mode ? "5-Step" : "4-Step",
           nes_apu.frame_irq_active ? "Yes" : "No");
    printf("DMC State:        Sample: 0x%04X  Current: 0x%04X  Left: %-5d  Empty: %s\033[K\n",
           nes_apu.dmc_sample_addr, nes_apu.dmc_current_addr, nes_apu.dmc_bytes_remaining,
           nes_apu.dmc_buffer_empty ? "Yes" : "No");
    printf("\033[K\n");

    if (loaded_cartridge) {
        printf("\033[1;33m--- Mapper State (%d) ---\033[0m\033[K\n", loaded_cartridge->mapper_id);
        printf("Mirroring Mode: %s\033[K\n", mirror_mode_str);
        switch (loaded_cartridge->mapper_id) {
            case 1:
                printf("PRG Mode: %d  PRG Bank: %d\033[K\n", (loaded_cartridge->mapper_state[2] >> 2) & 0x03, loaded_cartridge->mapper_state[5] & 0x0F);
                printf("CHR Mode: %d  CHR Bank 0: %d  Bank 1: %d\033[K\n", (loaded_cartridge->mapper_state[2] >> 4) & 0x01, loaded_cartridge->mapper_state[3], loaded_cartridge->mapper_state[4]);
                break;
            case 4:
                printf("PRG Banks: R6 (Bank 0) = %d, R7 (Bank 1) = %d\033[K\n", loaded_cartridge->mapper_state[6], loaded_cartridge->mapper_state[7]);
                printf("CHR Banks: R0=%d, R1=%d, R2=%d, R3=%d, R4=%d, R5=%d\033[K\n",
                       loaded_cartridge->mapper_state[0], loaded_cartridge->mapper_state[1],
                       loaded_cartridge->mapper_state[2], loaded_cartridge->mapper_state[3],
                       loaded_cartridge->mapper_state[4], loaded_cartridge->mapper_state[5]);
                printf("IRQ Counter: %d  IRQ Reload: %d  IRQ Enabled: %s\033[K\n",
                       loaded_cartridge->mapper_state[10], loaded_cartridge->mapper_state[9],
                       loaded_cartridge->mapper_state[11] ? "Yes" : "No");
                break;
            case 5:
                printf("ExRAM Mode: %d  PRG RAM Protect Flags: [0x%02X, 0x%02X]\033[K\n",
                       loaded_cartridge->mmc5_exram_mode, loaded_cartridge->mmc5_ram_protect[0], loaded_cartridge->mmc5_ram_protect[1]);
                printf("Multiplier Status: %d * %d = %d\033[K\n",
                       loaded_cartridge->mmc5_mult_a, loaded_cartridge->mmc5_mult_b,
                       loaded_cartridge->mmc5_mult_a * loaded_cartridge->mmc5_mult_b);
                printf("Scanline Counter: %d  IRQ Target: %d  Enabled: %s  Pending: %s  In-Frame: %s\033[K\n",
                       loaded_cartridge->mmc5_scanline, loaded_cartridge->mmc5_irq_target,
                       loaded_cartridge->mmc5_irq_enabled ? "Yes" : "No",
                       loaded_cartridge->mmc5_irq_pending ? "Yes" : "No",
                       loaded_cartridge->mmc5_in_frame ? "Yes" : "No");
                break;
            case 69: {
                uint16_t fme7_counter = loaded_cartridge->mapper_state[15] | (loaded_cartridge->mapper_state[16] << 8);
                uint8_t fme7_ctrl = loaded_cartridge->mapper_state[14];
                printf("IRQ Counter: %d  Counter Enabled: %s  IRQ Enabled: %s  Pending: %s\033[K\n",
                       fme7_counter, (fme7_ctrl & 0x80) ? "Yes" : "No",
                       (fme7_ctrl & 0x01) ? "Yes" : "No",
                       loaded_cartridge->mapper_state[17] ? "Yes" : "No");
                break;
            }
            default:
                printf("Mapper state array bytes (Raw): [ ");
                for (int i = 0; i < 16; i++) printf("%02X ", loaded_cartridge->mapper_state[i]);
                printf("]\033[K\n");
                break;
        }
    }
    printf("\033[K\n");

    printf("\033[1;33m--- Controller / Zapper ---\033[0m\033[K\n");
    printf("P1 State: 0x%02X [", controller_state);
    const char* names[8] = {"A", "B", "SL", "ST", "U", "D", "L", "R"};
    for (int i = 0; i < 8; i++) {
        printf("%s ", (controller_state & (1 << i)) ? names[i] : ".");
    }
    printf("]\033[K\n");
    printf("Mouse Pos: X: %d, Y: %d  (Click: %s)\033[K\n", mouse_x, mouse_y, mouse_left_pressed ? "SHOT!" : "OFF");
    printf("\033[1;36m==================================================\033[0m\033[K\n");
    printf("\033[J");
    fflush(stdout);
}

static bool is_zapper_sensing_light(void) {
    if (mouse_x < 8 || mouse_x >= 248 || mouse_y < 8 || mouse_y >= 232) {
        return false;
    }
    uint32_t pixel = nes_ppu.screen_buffer[mouse_y * 256 + mouse_x];
    uint8_t r = (pixel >> 16) & 0xFF;
    uint8_t g = (pixel >> 8) & 0xFF;
    uint8_t b = pixel & 0xFF;
    return (r > 200 && g > 200 && b > 200);
}

static void test_bus_tick(void *context) {
    (void)context;
    if (loaded_cartridge != NULL) {
        if (loaded_cartridge->cpu_clocked_irq) {
            loaded_cartridge->clock_irq(loaded_cartridge, global_cpu);
        }
        apu_step(&nes_apu, &nes_bus, global_cpu);
    }
}

static void test_bus_ppu_tick(void *context) {
    (void)context;
    if (loaded_cartridge != NULL) {
        ppu_step(&nes_ppu, global_cpu, loaded_cartridge);
    }
}

static uint8_t test_bus_read(void *context, uint16_t address) {
    (void)context;
    if (loaded_cartridge != NULL) {
        if (address <= 0x1FFF) {
            return nes_ram[address & 0x07FF];
        } else if (address >= 0x2000 && address <= 0x3FFF) {
            return ppu_read_reg(&nes_ppu, loaded_cartridge, address, global_cpu);
        } else if (address == 0x4016) {
            uint8_t value = 0;
            if (controller_strobe) {
                value = controller_state & 1;
            } else {
                value = controller_shift & 1;
                controller_shift >>= 1;
                controller_shift |= 0x80;
            }
            return value;
        } else if (address == 0x4017) {
            uint8_t value = 0x00;
            if (!is_zapper_sensing_light()) {
                value |= (1 << 3);
            }
            if (mouse_left_pressed) {
                value |= (1 << 4);
            }
            return value;
        } else if (address >= 0x4000 && address <= 0x4015) {
            return apu_read_reg(&nes_apu, address, global_cpu);
        } else if (address >= 0x4000 && address <= 0x4017) {
            return mock_apu_io[address - 0x4000];
        } else if (address >= 0x4018) {
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
            ppu_write_reg(&nes_ppu, loaded_cartridge, address, data, global_cpu);
        } else if (address == 0x4016) {
            controller_strobe = data & 1;
            if (controller_strobe) {
                controller_shift = controller_state;
            }
        } else if (address == 0x4014) {
            uint16_t dma_addr = (uint16_t)(data << 8);
            global_cpu->cycle_count++;
            test_bus_tick(NULL);
            test_bus_ppu_tick(NULL);
            test_bus_ppu_tick(NULL);
            test_bus_ppu_tick(NULL);
            if (global_cpu->cycle_count % 2 == 1) {
                global_cpu->cycle_count++;
                test_bus_tick(NULL);
                test_bus_ppu_tick(NULL);
                test_bus_ppu_tick(NULL);
                test_bus_ppu_tick(NULL);
            }
            uint8_t oam_start_addr = nes_ppu.oam_addr; // Store OAMADDR before DMA
            for (int i = 0; i < 256; i++) {
                uint8_t val = test_bus_read(context, (uint16_t)(dma_addr + i));
                global_cpu->cycle_count++;
                test_bus_tick(NULL);
                test_bus_ppu_tick(NULL);
                test_bus_ppu_tick(NULL);
                test_bus_ppu_tick(NULL);
                nes_ppu.oam_ram[(oam_start_addr + i) & 0xFF] = val; // DMA writes directly to OAM RAM, OAMADDR is not incremented
                global_cpu->cycle_count++;
                test_bus_tick(NULL);
                test_bus_ppu_tick(NULL);
                test_bus_ppu_tick(NULL);
                test_bus_ppu_tick(NULL);
            }
        } else if (address >= 0x4000 && address <= 0x4017) {
            apu_write_reg(&nes_apu, address, data, global_cpu);
            mock_apu_io[address - 0x4000] = data;
        } else if (address >= 0x4018) {
            loaded_cartridge->write_prg(loaded_cartridge, address, data);
            if (loaded_cartridge->mapper_id == 4 && address >= 0xE000 && global_cpu != NULL) {
                cpu_set_irq_line(global_cpu, 0, false);
            }
        }
    }
}

static void save_emulator_state(const CPU6502 *cpu, const char *dir, const char *filename) {
    if (!loaded_cartridge) return;
    char filepath[1024];
    snprintf(filepath, sizeof(filepath), "%s/%s", dir, filename);
    FILE *f = fopen(filepath, "wb");
    if (!f) { fprintf(stderr, "Error: Could not open save state file '%s'\n", filepath); return; }
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
    fwrite(&nes_ppu.bg_shifter_pattern_low, sizeof(nes_ppu.bg_shifter_pattern_low), 1, f);
    fwrite(&nes_ppu.bg_shifter_pattern_high, sizeof(nes_ppu.bg_shifter_pattern_high), 1, f);
    fwrite(&nes_ppu.bg_shifter_attrib_low, sizeof(nes_ppu.bg_shifter_attrib_low), 1, f);
    fwrite(&nes_ppu.bg_shifter_attrib_high, sizeof(nes_ppu.bg_shifter_attrib_high), 1, f);
    fwrite(&nes_ppu.bg_next_tile_id, sizeof(nes_ppu.bg_next_tile_id), 1, f);
    fwrite(&nes_ppu.bg_next_tile_attrib, sizeof(nes_ppu.bg_next_tile_attrib), 1, f);
    fwrite(&nes_ppu.bg_next_tile_lsb, sizeof(nes_ppu.bg_next_tile_lsb), 1, f);
    fwrite(&nes_ppu.bg_next_tile_msb, sizeof(nes_ppu.bg_next_tile_msb), 1, f);
    fwrite(&nes_ppu.odd_frame, sizeof(nes_ppu.odd_frame), 1, f);
    fwrite(&nes_ppu.a12_state, sizeof(nes_ppu.a12_state), 1, f);
    fwrite(&nes_ppu.a12_low_counter, sizeof(nes_ppu.a12_low_counter), 1, f);
    fwrite(&nes_apu, sizeof(APU2A03), 1, f);
    uint32_t mirroring_val = (uint32_t)loaded_cartridge->mirroring;
    fwrite(&mirroring_val, sizeof(mirroring_val), 1, f);
    fwrite(loaded_cartridge->mapper_state, 1, sizeof(loaded_cartridge->mapper_state), f);

    if (loaded_cartridge->mapper_id == 5) {
        fwrite(loaded_cartridge->exram, 1, sizeof(loaded_cartridge->exram), f);
        fwrite(&loaded_cartridge->mmc5_prg_mode, 1, sizeof(loaded_cartridge->mmc5_prg_mode), f);
        fwrite(&loaded_cartridge->mmc5_chr_mode, 1, sizeof(loaded_cartridge->mmc5_chr_mode), f);
        fwrite(loaded_cartridge->mmc5_ram_protect, 1, sizeof(loaded_cartridge->mmc5_ram_protect), f);
        fwrite(&loaded_cartridge->mmc5_exram_mode, 1, sizeof(loaded_cartridge->mmc5_exram_mode), f);
        fwrite(&loaded_cartridge->mmc5_nametable_ctrl, 1, sizeof(loaded_cartridge->mmc5_nametable_ctrl), f);
        fwrite(&loaded_cartridge->mmc5_fill_tile, 1, sizeof(loaded_cartridge->mmc5_fill_tile), f);
        fwrite(&loaded_cartridge->mmc5_fill_attr, 1, sizeof(loaded_cartridge->mmc5_fill_attr), f);
        fwrite(loaded_cartridge->mmc5_prg_regs, 1, sizeof(loaded_cartridge->mmc5_prg_regs), f);
        fwrite(loaded_cartridge->mmc5_chr_regs_a, 1, sizeof(loaded_cartridge->mmc5_chr_regs_a), f);
        fwrite(loaded_cartridge->mmc5_chr_regs_b, 1, sizeof(loaded_cartridge->mmc5_chr_regs_b), f);
        fwrite(&loaded_cartridge->mmc5_chr_high, 1, sizeof(loaded_cartridge->mmc5_chr_high), f);
        fwrite(&loaded_cartridge->mmc5_mult_a, 1, sizeof(loaded_cartridge->mmc5_mult_a), f);
        fwrite(&loaded_cartridge->mmc5_mult_b, 1, sizeof(loaded_cartridge->mmc5_mult_b), f);
        fwrite(&loaded_cartridge->mmc5_irq_target, 1, sizeof(loaded_cartridge->mmc5_irq_target), f);
        uint8_t temp_enabled = loaded_cartridge->mmc5_irq_enabled ? 1 : 0;
        fwrite(&temp_enabled, 1, 1, f);
        uint8_t temp_pending = loaded_cartridge->mmc5_irq_pending ? 1 : 0;
        fwrite(&temp_pending, 1, 1, f);
        uint8_t temp_in_frame = loaded_cartridge->mmc5_in_frame ? 1 : 0;
        fwrite(&temp_in_frame, 1, 1, f);
        fwrite(&loaded_cartridge->mmc5_scanline, 1, sizeof(loaded_cartridge->mmc5_scanline), f);
        uint8_t temp_last_chr_a = loaded_cartridge->mmc5_last_chr_a ? 1 : 0;
        fwrite(&temp_last_chr_a, 1, 1, f);
    }

    uint32_t prg_ram_sz = (loaded_cartridge->prg_ram != NULL) ? loaded_cartridge->prg_ram_size : 0;
    fwrite(&prg_ram_sz, sizeof(prg_ram_sz), 1, f);
    if (prg_ram_sz > 0) {
        fwrite(loaded_cartridge->prg_ram, 1, prg_ram_sz, f);
    }
    fclose(f);
}

static void load_emulator_state(CPU6502 *cpu, const char *dir, const char *filename) {
    if (!loaded_cartridge) return;
    char filepath[1024];
    snprintf(filepath, sizeof(filepath), "%s/%s", dir, filename);
    FILE *f = fopen(filepath, "rb");
    if (!f) { fprintf(stderr, "Error: Could not open save state file '%s'\n", filepath); return; }
    uint32_t magic = 0;
    if (fread(&magic, sizeof(magic), 1, f) != 1 || magic != 0x53544154) {
        fclose(f);
        return;
    }
    char rom_meta[64];
    char time_meta[32];
    if (fread(rom_meta, 1, 64, f) != 64 || fread(time_meta, 1, 32, f) != 32) { fclose(f); return; }
    if (fread(nes_ram, 1, sizeof(nes_ram), f) != sizeof(nes_ram)) { fclose(f); return; }
    if (fread(mock_apu_io, 1, sizeof(mock_apu_io), f) != sizeof(mock_apu_io)) { fclose(f); return; }
    if (fread(&controller_state, 1, sizeof(controller_state), f) != sizeof(controller_state)) { fclose(f); return; }
    if (fread(&controller_shift, 1, sizeof(controller_shift), f) != sizeof(controller_shift)) { fclose(f); return; }
    if (fread(cpu, sizeof(CPU6502), 1, f) != 1) { fclose(f); return; }
    if (fread(nes_ppu.vram, 1, sizeof(nes_ppu.vram), f) != sizeof(nes_ppu.vram)) { fclose(f); return; }
    if (fread(nes_ppu.palette_ram, 1, sizeof(nes_ppu.palette_ram), f) != sizeof(nes_ppu.palette_ram)) { fclose(f); return; }
    if (fread(nes_ppu.oam_ram, 1, sizeof(nes_ppu.oam_ram), f) != sizeof(nes_ppu.oam_ram)) { fclose(f); return; }
    if (fread(&nes_ppu.v, sizeof(nes_ppu.v), 1, f) != 1) { fclose(f); return; }
    if (fread(&nes_ppu.t, sizeof(nes_ppu.t), 1, f) != 1) { fclose(f); return; }
    if (fread(&nes_ppu.x, sizeof(nes_ppu.x), 1, f) != 1) { fclose(f); return; }
    if (fread(&nes_ppu.w, sizeof(nes_ppu.w), 1, f) != 1) { fclose(f); return; }
    if (fread(&nes_ppu.ppu_ctrl, sizeof(nes_ppu.ppu_ctrl), 1, f) != 1) { fclose(f); return; }
    if (fread(&nes_ppu.ppu_mask, sizeof(nes_ppu.ppu_mask), 1, f) != 1) { fclose(f); return; }
    if (fread(&nes_ppu.ppu_status, sizeof(nes_ppu.ppu_status), 1, f) != 1) { fclose(f); return; }
    if (loaded_cartridge != NULL) {
        loaded_cartridge->ppu_sprite_size_8x16 = (nes_ppu.ppu_ctrl & 0x20) != 0;
    }
    if (fread(&nes_ppu.oam_addr, sizeof(nes_ppu.oam_addr), 1, f) != 1) { fclose(f); return; }
    if (fread(&nes_ppu.buffered_data, sizeof(nes_ppu.buffered_data), 1, f) != 1) { fclose(f); return; }
    if (fread(&nes_ppu.scanline, sizeof(nes_ppu.scanline), 1, f) != 1) { fclose(f); return; }
    if (fread(&nes_ppu.cycle, sizeof(nes_ppu.cycle), 1, f) != 1) { fclose(f); return; }
    if (fread(&nes_ppu.nmi_occurred, sizeof(nes_ppu.nmi_occurred), 1, f) != 1) { fclose(f); return; }
    if (fread(&nes_ppu.frame_complete, sizeof(nes_ppu.frame_complete), 1, f) != 1) { fclose(f); return; }
    if (fread(&nes_ppu.scanline_sprite_count, sizeof(nes_ppu.scanline_sprite_count), 1, f) != 1) { fclose(f); return; }
    if (fread(nes_ppu.scanline_sprites, 1, sizeof(nes_ppu.scanline_sprites), f) != sizeof(nes_ppu.scanline_sprites)) { fclose(f); return; }
    if (fread(&nes_ppu.bg_shifter_pattern_low, sizeof(nes_ppu.bg_shifter_pattern_low), 1, f) != 1) { fclose(f); return; }
    if (fread(&nes_ppu.bg_shifter_pattern_high, sizeof(nes_ppu.bg_shifter_pattern_high), 1, f) != 1) { fclose(f); return; }
    if (fread(&nes_ppu.bg_shifter_attrib_low, sizeof(nes_ppu.bg_shifter_attrib_low), 1, f) != 1) { fclose(f); return; }
    if (fread(&nes_ppu.bg_shifter_attrib_high, sizeof(nes_ppu.bg_shifter_attrib_high), 1, f) != 1) { fclose(f); return; }
    if (fread(&nes_ppu.bg_next_tile_id, sizeof(nes_ppu.bg_next_tile_id), 1, f) != 1) { fclose(f); return; }
    if (fread(&nes_ppu.bg_next_tile_attrib, sizeof(nes_ppu.bg_next_tile_attrib), 1, f) != 1) { fclose(f); return; }
    if (fread(&nes_ppu.bg_next_tile_lsb, sizeof(nes_ppu.bg_next_tile_lsb), 1, f) != 1) { fclose(f); return; }
    if (fread(&nes_ppu.bg_next_tile_msb, sizeof(nes_ppu.bg_next_tile_msb), 1, f) != 1) { fclose(f); return; }
    if (fread(&nes_ppu.odd_frame, sizeof(nes_ppu.odd_frame), 1, f) != 1) { fclose(f); return; }
    if (fread(&nes_ppu.a12_state, sizeof(nes_ppu.a12_state), 1, f) != 1) { fclose(f); return; }
    if (fread(&nes_ppu.a12_low_counter, sizeof(nes_ppu.a12_low_counter), 1, f) != 1) { fclose(f); return; }
    if (fread(&nes_apu, sizeof(APU2A03), 1, f) != 1) { fclose(f); return; }
    uint32_t mirroring_val = 0;
    if (fread(&mirroring_val, sizeof(mirroring_val), 1, f) != 1) { fclose(f); return; }
    loaded_cartridge->mirroring = (MirroringMode)mirroring_val;
    if (fread(loaded_cartridge->mapper_state, 1, sizeof(loaded_cartridge->mapper_state), f) != sizeof(loaded_cartridge->mapper_state)) { fclose(f); return; }

    if (loaded_cartridge->mapper_id == 5) {
        if (fread(loaded_cartridge->exram, 1, sizeof(loaded_cartridge->exram), f) != sizeof(loaded_cartridge->exram)) { fclose(f); return; }
        if (fread(&loaded_cartridge->mmc5_prg_mode, 1, sizeof(loaded_cartridge->mmc5_prg_mode), f) != sizeof(loaded_cartridge->mmc5_prg_mode)) { fclose(f); return; }
        if (fread(&loaded_cartridge->mmc5_chr_mode, 1, sizeof(loaded_cartridge->mmc5_chr_mode), f) != sizeof(loaded_cartridge->mmc5_chr_mode)) { fclose(f); return; }
        if (fread(loaded_cartridge->mmc5_ram_protect, 1, sizeof(loaded_cartridge->mmc5_ram_protect), f) != sizeof(loaded_cartridge->mmc5_ram_protect)) { fclose(f); return; }
        if (fread(&loaded_cartridge->mmc5_exram_mode, 1, sizeof(loaded_cartridge->mmc5_exram_mode), f) != sizeof(loaded_cartridge->mmc5_exram_mode)) { fclose(f); return; }
        if (fread(&loaded_cartridge->mmc5_nametable_ctrl, 1, sizeof(loaded_cartridge->mmc5_nametable_ctrl), f) != sizeof(loaded_cartridge->mmc5_nametable_ctrl)) { fclose(f); return; }
        if (fread(&loaded_cartridge->mmc5_fill_tile, 1, sizeof(loaded_cartridge->mmc5_fill_tile), f) != sizeof(loaded_cartridge->mmc5_fill_tile)) { fclose(f); return; }
        if (fread(&loaded_cartridge->mmc5_fill_attr, 1, sizeof(loaded_cartridge->mmc5_fill_attr), f) != sizeof(loaded_cartridge->mmc5_fill_attr)) { fclose(f); return; }
        if (fread(loaded_cartridge->mmc5_prg_regs, 1, sizeof(loaded_cartridge->mmc5_prg_regs), f) != sizeof(loaded_cartridge->mmc5_prg_regs)) { fclose(f); return; }
        if (fread(loaded_cartridge->mmc5_chr_regs_a, 1, sizeof(loaded_cartridge->mmc5_chr_regs_a), f) != sizeof(loaded_cartridge->mmc5_chr_regs_a)) { fclose(f); return; }
        if (fread(loaded_cartridge->mmc5_chr_regs_b, 1, sizeof(loaded_cartridge->mmc5_chr_regs_b), f) != sizeof(loaded_cartridge->mmc5_chr_regs_b)) { fclose(f); return; }
        if (fread(&loaded_cartridge->mmc5_chr_high, 1, sizeof(loaded_cartridge->mmc5_chr_high), f) != sizeof(loaded_cartridge->mmc5_chr_high)) { fclose(f); return; }
        if (fread(&loaded_cartridge->mmc5_mult_a, 1, sizeof(loaded_cartridge->mmc5_mult_a), f) != sizeof(loaded_cartridge->mmc5_mult_a)) { fclose(f); return; }
        if (fread(&loaded_cartridge->mmc5_mult_b, 1, sizeof(loaded_cartridge->mmc5_mult_b), f) != sizeof(loaded_cartridge->mmc5_mult_b)) { fclose(f); return; }
        if (fread(&loaded_cartridge->mmc5_irq_target, 1, sizeof(loaded_cartridge->mmc5_irq_target), f) != sizeof(loaded_cartridge->mmc5_irq_target)) { fclose(f); return; }
        uint8_t temp_enabled = 0;
        if (fread(&temp_enabled, 1, 1, f) != 1) { fclose(f); return; }
        loaded_cartridge->mmc5_irq_enabled = (temp_enabled != 0);
        uint8_t temp_pending = 0;
        if (fread(&temp_pending, 1, 1, f) != 1) { fclose(f); return; }
        loaded_cartridge->mmc5_irq_pending = (temp_pending != 0);
        uint8_t temp_in_frame = 0;
        if (fread(&temp_in_frame, 1, 1, f) != 1) { fclose(f); return; }
        loaded_cartridge->mmc5_in_frame = (temp_in_frame != 0);
        if (fread(&loaded_cartridge->mmc5_scanline, 1, sizeof(loaded_cartridge->mmc5_scanline), f) != sizeof(loaded_cartridge->mmc5_scanline)) { fclose(f); return; }
        uint8_t temp_last_chr_a = 0;
        if (fread(&temp_last_chr_a, 1, 1, f) != 1) { fclose(f); return; }
        loaded_cartridge->mmc5_last_chr_a = (temp_last_chr_a != 0);
    }

    uint32_t prg_ram_sz = 0;
    if (fread(&prg_ram_sz, sizeof(prg_ram_sz), 1, f) == 1 && prg_ram_sz > 0) {
        if (loaded_cartridge->prg_ram != NULL && loaded_cartridge->prg_ram_size >= prg_ram_sz) {
            if (fread(loaded_cartridge->prg_ram, 1, prg_ram_sz, f) != prg_ram_sz) {}
        } else if (loaded_cartridge->prg_ram != NULL) {
            if (fread(loaded_cartridge->prg_ram, 1, loaded_cartridge->prg_ram_size, f) != loaded_cartridge->prg_ram_size) {}
            if (prg_ram_sz > loaded_cartridge->prg_ram_size) {
                fseek(f, prg_ram_sz - loaded_cartridge->prg_ram_size, SEEK_CUR);
            }
        } else {
            fseek(f, prg_ram_sz, SEEK_CUR);
        }
    }
    fclose(f);
}


int main(int argc, char *argv[]) {
    (void)argc; (void)argv;

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) < 0) {
        return -1;
    }

    load_emulator_settings();

    // Open any currently connected game controller
    for (int i = 0; i < SDL_NumJoysticks(); ++i) {
        if (SDL_IsGameController(i)) {
            game_controller = SDL_GameControllerOpen(i);
            if (game_controller) {
                break;
            }
        }
    }

    SDL_AudioSpec desired, obtained;
    SDL_zero(desired);
    desired.freq = 44100;
    desired.format = AUDIO_F32SYS;
    desired.channels = 1;
    desired.samples = 512;
    audio_device = SDL_OpenAudioDevice(NULL, 0, &desired, &obtained, 0);
    if (audio_device != 0) {
        SDL_PauseAudioDevice(audio_device, 0);
    }

    SDL_Window *window = SDL_CreateWindow(
        "NES Emulator",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        256 * window_scale, 240 * window_scale,
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE
    );
    if (fullscreen) {
        SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP);
    }

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
    nes_bus.bus_context = NULL;
    nes_bus.read = test_bus_read;
    nes_bus.write = test_bus_write;
    nes_bus.tick = test_bus_tick;
    nes_bus.ppu_tick = test_bus_ppu_tick;

    global_cpu = &cpu;

    cpu_init(&cpu, CPU_MODEL_RICOH_2A03);

    bool running = true;
    SDL_Event event;
    scan_rom_directory();

    while (running) {
        if (current_state == GUI_STATE_GAMEPLAY && loaded_cartridge != NULL) {
            if (debugger_active) {
                debugger_render(renderer, &cpu);
                SDL_RenderPresent(renderer);
                SDL_Delay(16);
                update_console_debug(&cpu);
            } else {
                uint64_t frame_start_tick = SDL_GetPerformanceCounter();

                nes_ppu.frame_complete = false;
                while (!nes_ppu.frame_complete) {
                    if (breakpoints[cpu.program_counter]) {
                        debugger_active = true;
                        debugger_view_pc = cpu.program_counter;
                        debugger_selected_line = 0;
                        clear_view_history(debugger_view_pc);
                        break;
                    }
                    if (debugger_logging_active) {
                        debugger_log_instruction(&cpu);
                    }
                    cpu_step(&cpu, &nes_bus);
                }

                uint64_t emu_end_tick = SDL_GetPerformanceCounter();
                debug_emu_ticks += (emu_end_tick - frame_start_tick);

                SDL_UpdateTexture(texture, NULL, nes_ppu.screen_buffer, 256 * sizeof(uint32_t));
                SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
                SDL_RenderClear(renderer);

                SDL_Rect src_rect = { 0, 0, 256, 240 };
                if (loaded_cartridge != NULL) {
                    uint8_t mapper = loaded_cartridge->mapper_id;
                    // NROM, MMC3: Crop horizontal/vertical margins to hide scroll overflow
                    if (mapper == 0 || mapper == 4 || mapper == 206 || mapper == 227) {
                        src_rect.x = 8;
                        src_rect.y = 8;
                        src_rect.w = 240;
                        src_rect.h = 224;
                    } else if (mapper == 1) { // MMC1 (e.g. Zelda): Crop left/right margins, show full height
                        src_rect.x = 8;
                        src_rect.y = 0;
                        src_rect.w = 240;
                        src_rect.h = 240;
                    } else { // UxROM (Castlevania), CNROM, AxROM, etc.: Show full output overflow
                        src_rect.x = 0;
                        src_rect.y = 0;
                        src_rect.w = 256;
                        src_rect.h = 240;
                    }
                }
                SDL_RenderCopy(renderer, texture, &src_rect, NULL);

                if (notification_timer > 0) {
                    notification_timer--;
                    int text_w = (int)strlen(notification_text) * 8;
                    int x = 256 - text_w - 12;
                    int y = 240 - 8 - 12;

                    // Outer thin white border (2px offset)
                    for (int dx = -2; dx <= 2; dx++) {
                        for (int dy = -2; dy <= 2; dy++) {
                            if (abs(dx) == 2 || abs(dy) == 2) {
                                draw_string(renderer, notification_text, x + dx, y + dy, 0xFFFFFF);
                            }
                        }
                    }
                    // Inner black border (1px offset)
                    for (int dx = -1; dx <= 1; dx++) {
                        for (int dy = -1; dy <= 1; dy++) {
                            if (dx != 0 || dy != 0) {
                                draw_string(renderer, notification_text, x + dx, y + dy, 0x000000);
                            }
                        }
                    }
                    // Main green text
                    draw_string(renderer, notification_text, x, y, 0x00FF00);
                }

                SDL_RenderPresent(renderer);

                if (audio_device != 0 && nes_apu.audio_buffer_idx > 0) {
                    if (!audio_muted) {
                        for (uint32_t i = 0; i < nes_apu.audio_buffer_idx; i++) {
                            nes_apu.audio_buffer[i] *= ((float)master_volume / 100.0f);
                        }
                        SDL_QueueAudio(audio_device, nes_apu.audio_buffer, nes_apu.audio_buffer_idx * sizeof(float));
                        while (SDL_GetQueuedAudioSize(audio_device) > 4096 * sizeof(float)) {
                            SDL_Delay(1);
                        }
                    } else {
                        static uint32_t last_frame_time = 0;
                        uint32_t now = SDL_GetTicks();
                        if (now < last_frame_time + 16) {
                            SDL_Delay((last_frame_time + 16) - now);
                        }
                        last_frame_time = SDL_GetTicks();
                    }
                    nes_apu.audio_buffer_idx = 0;
                } else {
                    static uint32_t last_frame_time = 0;
                    uint32_t now = SDL_GetTicks();
                    if (now < last_frame_time + 16) {
                        SDL_Delay((last_frame_time + 16) - now);
                    }
                    last_frame_time = SDL_GetTicks();
                }

                uint64_t frame_end_tick = SDL_GetPerformanceCounter();
                debug_total_ticks += (frame_end_tick - frame_start_tick);

                update_console_debug(&cpu);
            }
        } else {
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
                        col = 0x444444;
                    } else {
                        col = (i == menu_selection) ? 0xFFFFFF : 0x888888;
                    }
                    draw_string(renderer, options[i], 40, 60 + i * 15, col);
                    if (i == menu_selection) {
                        SDL_SetRenderDrawColor(renderer, 0, 255, 0, 255);
                        SDL_Rect box = { 32, 58 + i * 15, 180, 11 };
                        SDL_RenderDrawRect(renderer, &box);
                    }
                }
            } else if (current_state == GUI_STATE_MENU_CONTROLS) {
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
                    if (menu_selection < rom_scroll_offset) {
                        rom_scroll_offset = menu_selection;
                    } else if (menu_selection >= rom_scroll_offset + 12) {
                        rom_scroll_offset = menu_selection - 12 + 1;
                    }
                    int end_idx = rom_scroll_offset + 12;
                    if (end_idx > rom_file_count) end_idx = rom_file_count;
                    for (int i = rom_scroll_offset; i < end_idx; i++) {
                        int display_row = i - rom_scroll_offset;
                        uint32_t col = (i == menu_selection) ? 0xFFFFFF : 0x888888;
                        char display_name[32];
                        strncpy(display_name, rom_files[i], 24);
                        display_name[24] = '\0';
                        if (strlen(rom_files[i]) > 24) {
                            strcat(display_name, "...");
                        }
                        draw_string(renderer, display_name, 32, 70 + display_row * 12, col);
                        if (i == menu_selection) {
                            SDL_SetRenderDrawColor(renderer, 0, 255, 0, 255);
                            SDL_Rect box = { 24, 68 + display_row * 12, 208, 11 };
                            SDL_RenderDrawRect(renderer, &box);
                        }
                    }
                    if (rom_scroll_offset > 0) {
                        draw_string(renderer, "^", 236, 68, 0x00FF00);
                    }
                    if (end_idx < rom_file_count) {
                        draw_string(renderer, "v", 236, 68 + 11 * 12, 0x00FF00);
                    }
                }
            } else if (current_state == GUI_STATE_MENU_SAVE_STATE) {
                draw_string(renderer, "SELECT SAVE TO OVERWRITE:", 24, 50, 0xFFFF00);
                int total_options = state_file_count + 1;
                if (menu_selection < rom_scroll_offset) {
                    rom_scroll_offset = menu_selection;
                } else if (menu_selection >= rom_scroll_offset + 12) {
                    rom_scroll_offset = menu_selection - 12 + 1;
                }
                int end_idx = rom_scroll_offset + 12;
                if (end_idx > total_options) end_idx = total_options;
                for (int i = rom_scroll_offset; i < end_idx; i++) {
                    int display_row = i - rom_scroll_offset;
                    uint32_t col = (i == menu_selection) ? 0xFFFFFF : 0x888888;
                    char buf[128];
                    if (i == 0) {
                        snprintf(buf, sizeof(buf), "<Create New Manual Save>");
                    } else {
                        get_state_file_info(state_files[i - 1], buf, sizeof(buf));
                    }
                    char display_buf[32];
                    strncpy(display_buf, buf, 28);
                    display_buf[28] = '\0';
                    if (strlen(buf) > 28) strcat(display_buf, "...");
                    draw_string(renderer, display_buf, 32, 70 + display_row * 12, col);
                    if (i == menu_selection) {
                        SDL_SetRenderDrawColor(renderer, 0, 255, 0, 255);
                        SDL_Rect box = { 24, 68 + display_row * 12, 208, 11 };
                        SDL_RenderDrawRect(renderer, &box);
                    }
                }
                if (rom_scroll_offset > 0) draw_string(renderer, "^", 236, 68, 0x00FF00);
                if (end_idx < total_options) draw_string(renderer, "v", 236, 68 + 11 * 12, 0x00FF00);
                draw_string(renderer, "UP/DN: Nav | ENTER: Save | ESC: Back", 8, 220, 0x00FFFF);
            } else if (current_state == GUI_STATE_MENU_LOAD_STATE) {
                draw_string(renderer, "SELECT SLOT TO LOAD STATE:", 24, 50, 0xFFFF00);
                if (state_file_count == 0) {
                    draw_string(renderer, "No save states found", 40, 70, 0xFF0000);
                } else {
                    if (menu_selection < rom_scroll_offset) {
                        rom_scroll_offset = menu_selection;
                    } else if (menu_selection >= rom_scroll_offset + 12) {
                        rom_scroll_offset = menu_selection - 12 + 1;
                    }
                    int end_idx = rom_scroll_offset + 12;
                    if (end_idx > state_file_count) end_idx = state_file_count;
                    for (int i = rom_scroll_offset; i < end_idx; i++) {
                        int display_row = i - rom_scroll_offset;
                        uint32_t col = (i == menu_selection) ? 0xFFFFFF : 0x888888;
                        char buf[128];
                        get_state_file_info(state_files[i], buf, sizeof(buf));
                        char display_buf[32];
                        strncpy(display_buf, buf, 28);
                        display_buf[28] = '\0';
                        if (strlen(buf) > 28) strcat(display_buf, "...");
                        draw_string(renderer, display_buf, 32, 70 + display_row * 12, col);
                        if (i == menu_selection) {
                            SDL_SetRenderDrawColor(renderer, 0, 255, 0, 255);
                            SDL_Rect box = { 24, 68 + display_row * 12, 208, 11 };
                            SDL_RenderDrawRect(renderer, &box);
                        }
                    }
                    if (rom_scroll_offset > 0) draw_string(renderer, "^", 236, 68, 0x00FF00);
                    if (end_idx < state_file_count) draw_string(renderer, "v", 236, 68 + 11 * 12, 0x00FF00);
                }
                draw_string(renderer, "UP/DN: Nav | ENTER: Load | ESC: Back", 8, 220, 0x00FFFF);
            } else if (current_state == GUI_STATE_MENU_SETTINGS) {
                char scale_buf[64], mute_buf[64], vol_buf[64], fs_buf[64], debug_buf[64];
                if (window_scale == 5) {
                    sprintf(scale_buf, "1. Window Scale: Maximized");
                } else {
                    sprintf(scale_buf, "1. Window Scale: %dx", window_scale);
                }
                sprintf(mute_buf,  "2. Audio Muted:  %s", audio_muted ? "ON" : "OFF");

                int num_bars = master_volume / 10;
                char slider[12];
                for (int i = 0; i < 10; i++) {
                    slider[i] = (i < num_bars) ? '|' : '.';
                }
                slider[10] = '\0';
                sprintf(vol_buf,   "3. Volume: [%s] %d%%", slider, master_volume);

                sprintf(fs_buf,    "4. Fullscreen:   %s", fullscreen ? "ON" : "OFF");
                sprintf(debug_buf, "5. Console Debug:%s", console_debug_enabled ? "ON" : "OFF");

                uint32_t col0 = (menu_selection == 0) ? 0xFFFFFF : 0x888888;
                uint32_t col1 = (menu_selection == 1) ? 0xFFFFFF : 0x888888;
                uint32_t col2 = (menu_selection == 2) ? 0xFFFFFF : 0x888888;
                uint32_t col3 = (menu_selection == 3) ? 0xFFFFFF : 0x888888;
                uint32_t col4 = (menu_selection == 4) ? 0xFFFFFF : 0x888888;

                draw_string(renderer, scale_buf, 40, 70, col0);
                draw_string(renderer, mute_buf,  40, 90, col1);
                draw_string(renderer, vol_buf,   40, 110, col2);
                draw_string(renderer, fs_buf,    40, 130, col3);
                draw_string(renderer, debug_buf, 40, 150, col4);
                
                SDL_SetRenderDrawColor(renderer, 0, 255, 0, 255);
                SDL_Rect box = { 32, 68 + menu_selection * 20, 190, 11 };
                SDL_RenderDrawRect(renderer, &box);

                if (menu_selection == 2) {
                    draw_string(renderer, "Use Left/Right to adjust", 24, 175, 0xFFFF00);
                } else {
                    draw_string(renderer, "Press Enter to Toggle setting", 20, 175, 0xFFFF00);
                }
            }

            SDL_RenderPresent(renderer);
            SDL_Delay(16);

            update_console_debug(&cpu);
        }

        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                running = false;
            } else if (event.type == SDL_CONTROLLERDEVICEADDED) {
                if (!game_controller) {
                    game_controller = SDL_GameControllerOpen(event.cdevice.which);
                }
            } else if (event.type == SDL_CONTROLLERDEVICEREMOVED) {
                if (game_controller) {
                    SDL_Joystick *joystick = SDL_GameControllerGetJoystick(game_controller);
                    if (joystick && SDL_JoystickInstanceID(joystick) == event.cdevice.which) {
                        SDL_GameControllerClose(game_controller);
                        game_controller = NULL;
                        for (int i = 0; i < SDL_NumJoysticks(); ++i) {
                            if (SDL_IsGameController(i)) {
                                game_controller = SDL_GameControllerOpen(i);
                                if (game_controller) {
                                    break;
                                }
                            }
                        }
                    }
                }
            } else if (event.type == SDL_CONTROLLERBUTTONDOWN) {
                if (current_state == GUI_STATE_GAMEPLAY && !debugger_active) {
                    if (event.cbutton.button == SDL_CONTROLLER_BUTTON_X) {
                        char filename[128];
                        get_rolling_quicksave_filename(filename, sizeof(filename), true);
                        save_emulator_state(&cpu, save_state_dir, filename);
                        show_notification("STATE SAVED");
                    } else if (event.cbutton.button == SDL_CONTROLLER_BUTTON_Y) {
                        char filename[128];
                        get_rolling_quicksave_filename(filename, sizeof(filename), false);
                        load_emulator_state(&cpu, save_state_dir, filename);
                        show_notification("STATE LOADED");
                    } else {
                        for (int i = 0; i < 8; i++) {
                            if (event.cbutton.button == controller_button_mappings[i]) {
                                controller_state |= (1 << i);
                            }
                        }
                    }
                } else {
                    if (event.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_UP) {
                        push_synthetic_key(SDLK_UP, SDL_KEYDOWN);
                    } else if (event.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_DOWN) {
                        push_synthetic_key(SDLK_DOWN, SDL_KEYDOWN);
                    } else if (event.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_LEFT) {
                        push_synthetic_key(SDLK_LEFT, SDL_KEYDOWN);
                    } else if (event.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_RIGHT) {
                        push_synthetic_key(SDLK_RIGHT, SDL_KEYDOWN);
                    } else if (event.cbutton.button == SDL_CONTROLLER_BUTTON_A || event.cbutton.button == SDL_CONTROLLER_BUTTON_START) {
                        push_synthetic_key(SDLK_RETURN, SDL_KEYDOWN);
                    } else if (event.cbutton.button == SDL_CONTROLLER_BUTTON_B || event.cbutton.button == SDL_CONTROLLER_BUTTON_BACK) {
                        push_synthetic_key(SDLK_ESCAPE, SDL_KEYDOWN);
                    }
                }
            } else if (event.type == SDL_CONTROLLERBUTTONUP) {
                if (current_state == GUI_STATE_GAMEPLAY) {
                    for (int i = 0; i < 8; i++) {
                        if (event.cbutton.button == controller_button_mappings[i]) {
                            controller_state &= ~(1 << i);
                        }
                    }
                } else {
                    if (event.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_UP) {
                        push_synthetic_key(SDLK_UP, SDL_KEYUP);
                    } else if (event.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_DOWN) {
                        push_synthetic_key(SDLK_DOWN, SDL_KEYUP);
                    } else if (event.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_LEFT) {
                        push_synthetic_key(SDLK_LEFT, SDL_KEYUP);
                    } else if (event.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_RIGHT) {
                        push_synthetic_key(SDLK_RIGHT, SDL_KEYUP);
                    } else if (event.cbutton.button == SDL_CONTROLLER_BUTTON_A || event.cbutton.button == SDL_CONTROLLER_BUTTON_START) {
                        push_synthetic_key(SDLK_RETURN, SDL_KEYUP);
                    } else if (event.cbutton.button == SDL_CONTROLLER_BUTTON_B || event.cbutton.button == SDL_CONTROLLER_BUTTON_BACK) {
                        push_synthetic_key(SDLK_ESCAPE, SDL_KEYUP);
                    }
                }
            } else if (event.type == SDL_CONTROLLERAXISMOTION) {
                if (current_state == GUI_STATE_GAMEPLAY && !debugger_active) {
                    if (event.caxis.axis == SDL_CONTROLLER_AXIS_LEFTX) {
                        if (event.caxis.value < -16000) {
                            controller_state |= (1 << 6);
                            controller_state &= ~(1 << 7);
                        } else if (event.caxis.value > 16000) {
                            controller_state |= (1 << 7);
                            controller_state &= ~(1 << 6);
                        } else {
                            controller_state &= ~(1 << 6);
                            controller_state &= ~(1 << 7);
                        }
                    }
                    if (event.caxis.axis == SDL_CONTROLLER_AXIS_LEFTY) {
                        if (event.caxis.value < -16000) {
                            controller_state |= (1 << 4);
                            controller_state &= ~(1 << 5);
                        } else if (event.caxis.value > 16000) {
                            controller_state |= (1 << 5);
                            controller_state &= ~(1 << 4);
                        } else {
                            controller_state &= ~(1 << 4);
                            controller_state &= ~(1 << 5);
                        }
                    }
                }
            } else if (event.type == SDL_MOUSEMOTION) {
                mouse_x = event.motion.x;
                mouse_y = event.motion.y;
            } else if (event.type == SDL_MOUSEBUTTONDOWN) {
                if (event.button.button == SDL_BUTTON_LEFT) {
                    mouse_left_pressed = true;
                }
            } else if (event.type == SDL_MOUSEBUTTONUP) {
                if (event.button.button == SDL_BUTTON_LEFT) {
                    mouse_left_pressed = false;
                }
            } else if (event.type == SDL_KEYDOWN) {
                if (rebinding) {
                    if (event.key.keysym.sym != SDLK_ESCAPE) {
                        control_mappings[menu_selection] = event.key.keysym.sym;
                        save_emulator_settings();
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
                                    else if (current_state == GUI_STATE_MENU_SAVE_STATE) menu_selection = state_file_count;
                                    else if (current_state == GUI_STATE_MENU_LOAD_STATE) menu_selection = state_file_count - 1;
                                    else if (current_state == GUI_STATE_MENU_SETTINGS) menu_selection = 4;
                                    else if (current_state == GUI_STATE_MENU_CONTROLS) menu_selection = 8;
                                }
                            } while (current_state == GUI_STATE_MENU_MAIN && loaded_cartridge == NULL && (menu_selection == 0 || menu_selection == 2 || menu_selection == 3));
                            break;
                        case SDLK_DOWN:
                            do {
                                menu_selection++;
                                if (current_state == GUI_STATE_MENU_MAIN && menu_selection > 6) menu_selection = 0;
                                else if (current_state == GUI_STATE_MENU_LOAD_ROM && menu_selection >= rom_file_count) menu_selection = 0;
                                else if (current_state == GUI_STATE_MENU_SAVE_STATE && menu_selection > state_file_count) menu_selection = 0;
                                else if (current_state == GUI_STATE_MENU_LOAD_STATE && menu_selection >= state_file_count) menu_selection = 0;
                                else if (current_state == GUI_STATE_MENU_SETTINGS && menu_selection > 4) menu_selection = 0;
                                else if (current_state == GUI_STATE_MENU_CONTROLS && menu_selection > 8) menu_selection = 0;
                            } while (current_state == GUI_STATE_MENU_MAIN && loaded_cartridge == NULL && (menu_selection == 0 || menu_selection == 2 || menu_selection == 3));
                            break;
                        case SDLK_LEFT:
                            if (current_state == GUI_STATE_MENU_SETTINGS && menu_selection == 2) {
                                master_volume -= 10;
                                if (master_volume < 0) master_volume = 0;
                                play_volume_ding();
                                save_emulator_settings();
                            }
                            break;
                        case SDLK_RIGHT:
                            if (current_state == GUI_STATE_MENU_SETTINGS && menu_selection == 2) {
                                master_volume += 10;
                                if (master_volume > 100) master_volume = 100;
                                play_volume_ding();
                                save_emulator_settings();
                            }
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
                                    rom_scroll_offset = 0;
                                    scan_rom_directory();
                                } else if (menu_selection == 2) {
                                    if (loaded_cartridge != NULL) {
                                        current_state = GUI_STATE_MENU_SAVE_STATE;
                                        menu_selection = 0;
                                        rom_scroll_offset = 0;
                                        scan_save_state_directory();
                                    }
                                } else if (menu_selection == 3) {
                                    if (loaded_cartridge != NULL) {
                                        current_state = GUI_STATE_MENU_LOAD_STATE;
                                        menu_selection = 0;
                                        rom_scroll_offset = 0;
                                        scan_save_state_directory();
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
                                    save_emulator_settings();
                                } else {
                                    rebinding = true;
                                }
                            } else if (current_state == GUI_STATE_MENU_LOAD_ROM) {
                                if (rom_file_count > 0) {
                                    Cartridge *cart = cartridge_load(rom_files[menu_selection]);
                                    if (cart) {
                                        if (loaded_cartridge) {
                                            save_battery_ram();
                                            cartridge_free(loaded_cartridge);
                                            cleanup_default_sav();
                                        }
                                        loaded_cartridge = cart;
                                        strncpy(loaded_rom_name, rom_files[menu_selection], sizeof(loaded_rom_name) - 1);

                                        // Create save state directory for the ROM
                                        char rom_name_clean[256];
                                        strncpy(rom_name_clean, rom_files[menu_selection], sizeof(rom_name_clean) - 1);
                                        rom_name_clean[sizeof(rom_name_clean) - 1] = '\0';
                                        char *ext = strrchr(rom_name_clean, '.');
                                        if (ext) *ext = '\0'; // Remove extension

                                        char *base_path = SDL_GetBasePath();
                                        if (base_path) {
                                            snprintf(save_state_dir, sizeof(save_state_dir), "%s%s/%s", base_path, "saves", rom_name_clean);
                                            SDL_free(base_path);
                                        } else {
                                            // Fallback if SDL_GetBasePath fails, use current directory
                                            snprintf(save_state_dir, sizeof(save_state_dir), "%s/%s", "saves", rom_name_clean);
                                        }

                                        // Create the 'saves' root directory if it doesn't exist
                                        char saves_root_dir[512];
                                        strncpy(saves_root_dir, save_state_dir, sizeof(saves_root_dir) - 1);
                                        saves_root_dir[sizeof(saves_root_dir) - 1] = '\0';
                                        char *last_slash = strrchr(saves_root_dir, '/');
                                        if (last_slash) *last_slash = '\0';
                                        MKDIR(saves_root_dir);
                                        MKDIR(save_state_dir); // Create the ROM-specific save state directory

                                        loaded_rom_name[sizeof(loaded_rom_name) - 1] = '\0';
                                        memset(nes_ram, 0, sizeof(nes_ram));
                                        ppu_init(&nes_ppu);
                                        apu_init(&nes_apu);
                                        memset(mock_apu_io, 0, sizeof(mock_apu_io));
                                        debugger_init();
                                        cpu_init(&cpu, CPU_MODEL_RICOH_2A03);
                                        cpu_trigger_reset(&cpu);
                                        cpu_step(&cpu, &nes_bus);
                                        load_battery_ram();
                                        debugger_active = console_debug_enabled;
                                        debugger_logging_active = false;
                                        if (debugger_active) {
                                            debugger_view_pc = cpu.program_counter;
                                            debugger_selected_line = 0;
                                            clear_view_history(debugger_view_pc);
                                        }
                                        current_state = GUI_STATE_GAMEPLAY;
                                    }
                                }
                            } else if (current_state == GUI_STATE_MENU_SAVE_STATE) {
                                if (menu_selection == 0) {
                                    time_t t = time(NULL);
                                    struct tm *tm_info = localtime(&t);
                                    char name_buf[128];
                                    if (tm_info) {
                                        strftime(name_buf, sizeof(name_buf), "manual_%Y%m%d_%H%M%S.state", tm_info);
                                    } else {
                                        snprintf(name_buf, sizeof(name_buf), "manual_%ld.state", (long)t);
                                    }
                                    save_emulator_state(&cpu, save_state_dir, name_buf);
                                } else {
                                    save_emulator_state(&cpu, save_state_dir, state_files[menu_selection - 1]);
                                }
                                current_state = GUI_STATE_GAMEPLAY;
                            } else if (current_state == GUI_STATE_MENU_LOAD_STATE) {
                                if (state_file_count > 0) {
                                    load_emulator_state(&cpu, save_state_dir, state_files[menu_selection]);
                                    current_state = GUI_STATE_GAMEPLAY;
                                }
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
                                    play_volume_ding();
                                } else if (menu_selection == 3) {
                                    fullscreen = !fullscreen;
                                    if (fullscreen) {
                                        SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP);
                                    } else {
                                        SDL_SetWindowFullscreen(window, 0);
                                    }
                                } else if (menu_selection == 4) {
                                    console_debug_enabled = !console_debug_enabled;
                                    if (!console_debug_enabled) {
                                        printf("\033[H\033[2J");
                                        fflush(stdout);
                                    }
                                }
                                save_emulator_settings();
                            }
                            break;
                        default: break;
                    }
                } else {
                    switch (event.key.keysym.sym) {
                        case SDLK_f:
                        case SDLK_F11: {
                            fullscreen = !fullscreen;
                            if (fullscreen) {
                                SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP);
                            } else {
                                SDL_SetWindowFullscreen(window, 0);
                            }
                            save_emulator_settings();
                            break;
                        }
                        case SDLK_F3: {
                            console_debug_enabled = !console_debug_enabled;
                            if (!console_debug_enabled) {
                                printf("\033[H\033[2J");
                                fflush(stdout);
                            }
                            save_emulator_settings();
                            break;
                        }
                        case SDLK_F5: {
                            char filename[128];
                            get_rolling_quicksave_filename(filename, sizeof(filename), true);
                            save_emulator_state(&cpu, save_state_dir, filename);
                            show_notification("STATE SAVED");
                            break;
                        }
                        case SDLK_F8: {
                            char filename[128];
                            get_rolling_quicksave_filename(filename, sizeof(filename), false);
                            load_emulator_state(&cpu, save_state_dir, filename);
                            show_notification("STATE LOADED");
                            break;
                        }
                        case SDLK_UP: {
                            if (debugger_active) {
                                debugger_selected_line--;
                                if (debugger_selected_line < 0) {
                                    debugger_selected_line = 0;
                                    if (view_history_count > 1) {
                                        debugger_view_pc = pop_view_history();
                                    } else {
                                        uint16_t target = debugger_view_pc;
                                        if (target >= 1 && op_bytes[test_bus_peek(target - 1)] == 1) {
                                            debugger_view_pc = target - 1;
                                        } else if (target >= 2 && op_bytes[test_bus_peek(target - 2)] == 2) {
                                            debugger_view_pc = target - 2;
                                        } else if (target >= 3 && op_bytes[test_bus_peek(target - 3)] == 3) {
                                            debugger_view_pc = target - 3;
                                        } else {
                                            debugger_view_pc--;
                                        }
                                        clear_view_history(debugger_view_pc);
                                    }
                                }
                            } else {
                                for (int i = 0; i < 8; i++) {
                                    if (event.key.keysym.sym == control_mappings[i]) {
                                        controller_state |= (1 << i);
                                    }
                                }
                            }
                            break;
                        }
                        case SDLK_DOWN: {
                            if (debugger_active) {
                                debugger_selected_line++;
                                if (debugger_selected_line >= 12) {
                                    debugger_selected_line = 11;
                                    uint8_t op = test_bus_peek(debugger_view_pc);
                                    debugger_view_pc += op_bytes[op] ? op_bytes[op] : 1;
                                    push_view_history(debugger_view_pc);
                                }
                            } else {
                                for (int i = 0; i < 8; i++) {
                                    if (event.key.keysym.sym == control_mappings[i]) {
                                        controller_state |= (1 << i);
                                    }
                                }
                            }
                            break;
                        }
                        case SDLK_F7: {
                            if (debugger_active) {
                                uint16_t target_pc = debugger_line_pcs[debugger_selected_line];
                                breakpoints[target_pc] = !breakpoints[target_pc];
                            }
                            break;
                        }
                        case SDLK_F6: {
                            if (debugger_active) {
                                debugger_logging_active = !debugger_logging_active;
                            }
                            break;
                        }
                        case SDLK_F10: {
                            if (debugger_active) {
                                debugger_step_instruction(&cpu, &nes_bus);
                                clear_view_history(debugger_view_pc);
                            } else {
                                debugger_active = true;
                                debugger_view_pc = cpu.program_counter;
                                debugger_selected_line = 0;
                                clear_view_history(debugger_view_pc);
                            }
                            break;
                        }
                        case SDLK_F9: {
                            if (debugger_active) {
                                debugger_step_instruction(&cpu, &nes_bus);
                                debugger_active = false;
                            } else {
                                debugger_active = true;
                                debugger_view_pc = cpu.program_counter;
                                debugger_selected_line = 0;
                                clear_view_history(debugger_view_pc);
                            }
                            break;
                        }
                        case SDLK_F12: {
                            if (debugger_active) {
                                cpu_trigger_reset(&cpu);
                                cpu_step(&cpu, &nes_bus);
                            }
                            break;
                        }
                        default: {
                            if (!debugger_active) {
                                for (int i = 0; i < 8; i++) {
                                    if (event.key.keysym.sym == control_mappings[i]) {
                                        controller_state |= (1 << i);
                                    }
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

    if (game_controller) {
        SDL_GameControllerClose(game_controller);
    }
    if (audio_device != 0) {
        SDL_CloseAudioDevice(audio_device);
    }
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    if (console_debug_enabled) {
        printf("\033[H\033[2J");
        fflush(stdout);
    }

    if (loaded_cartridge) {
        save_battery_ram();
        cartridge_free(loaded_cartridge);
        cleanup_default_sav();
    }
    save_emulator_settings();
    return 0;
}