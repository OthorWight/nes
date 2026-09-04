#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#endif
#include <inttypes.h>
#include <math.h>
#include <SDL2/SDL.h>
#include "nes_system.h"
#include "debugger.h"

NES nes_sys;
static CPUBus cpu_bus_bridge;

static char loaded_rom_name[256] = "";
static char save_state_dir[512] = "";
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

#define CONTROL_COUNT 10

static SDL_Keycode control_mappings[CONTROL_COUNT] = {
    SDLK_z,
    SDLK_x,
    SDLK_SPACE,
    SDLK_RETURN,
    SDLK_UP,
    SDLK_DOWN,
    SDLK_LEFT,
    SDLK_RIGHT,
    SDLK_F5,
    SDLK_F8
};

static SDL_GameControllerButton controller_button_mappings[CONTROL_COUNT] = {
    SDL_CONTROLLER_BUTTON_A,
    SDL_CONTROLLER_BUTTON_B,
    SDL_CONTROLLER_BUTTON_BACK,
    SDL_CONTROLLER_BUTTON_START,
    SDL_CONTROLLER_BUTTON_DPAD_UP,
    SDL_CONTROLLER_BUTTON_DPAD_DOWN,
    SDL_CONTROLLER_BUTTON_DPAD_LEFT,
    SDL_CONTROLLER_BUTTON_DPAD_RIGHT,
    SDL_CONTROLLER_BUTTON_X,
    SDL_CONTROLLER_BUTTON_Y
};

static const SDL_GameControllerButton default_controller_mappings[CONTROL_COUNT] = {
    SDL_CONTROLLER_BUTTON_A,
    SDL_CONTROLLER_BUTTON_B,
    SDL_CONTROLLER_BUTTON_BACK,
    SDL_CONTROLLER_BUTTON_START,
    SDL_CONTROLLER_BUTTON_DPAD_UP,
    SDL_CONTROLLER_BUTTON_DPAD_DOWN,
    SDL_CONTROLLER_BUTTON_DPAD_LEFT,
    SDL_CONTROLLER_BUTTON_DPAD_RIGHT,
    SDL_CONTROLLER_BUTTON_X,
    SDL_CONTROLLER_BUTTON_Y
};

static int master_volume = 100;
static bool control_mode_keyboard = true;

static uint8_t cpu_bridge_read(void *ctx, uint16_t addr) {
    return nes_cpu_bus_read((NES*)ctx, addr);
}

static void cpu_bridge_write(void *ctx, uint16_t addr, uint8_t data) {
    nes_cpu_bus_write((NES*)ctx, addr, data);
}

static void play_volume_ding(void) {
    if (audio_device == 0 || audio_muted) return;
    SDL_ClearQueuedAudio(audio_device);

    NES temp_nes;
    nes_init(&temp_nes);

    apu_write_reg(&temp_nes, 0x4015, 0x01);
    apu_write_reg(&temp_nes, 0x4000, 0xBF);
    apu_write_reg(&temp_nes, 0x4002, 0xFD);
    apu_write_reg(&temp_nes, 0x4003, 0x00);

    uint32_t target_samples = 3900;
    int max_steps = 200000;
    for (int i = 0; i < max_steps && temp_nes.apu.audio_buffer_idx < target_samples; i++) {
        apu_step(&temp_nes.apu, &temp_nes);
    }

    float vol = (float)master_volume / 100.0f;
    for (uint32_t i = 0; i < temp_nes.apu.audio_buffer_idx; i++) {
        float envelope = 1.0f - ((float)i / temp_nes.apu.audio_buffer_idx);
        temp_nes.apu.audio_buffer[i] *= envelope * vol;
    }

    if (temp_nes.apu.audio_buffer_idx > 0) {
        SDL_QueueAudio(audio_device, temp_nes.apu.audio_buffer, temp_nes.apu.audio_buffer_idx * sizeof(float));
    }
}

static char notification_text[32] = "";
static int notification_timer = 0;

static void show_notification(const char *text) {
    strncpy(notification_text, text, sizeof(notification_text) - 1);
    notification_text[sizeof(notification_text) - 1] = '\0';
    notification_timer = 60;
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
    if (!nes_sys.cart || !nes_sys.cart->prg_ram || nes_sys.cart->prg_ram_size == 0) return;
    char rom_name_clean[256];
    get_clean_rom_name(rom_name_clean, sizeof(rom_name_clean));
    char filepath[1024];
    snprintf(filepath, sizeof(filepath), "%s/%s.sav", save_state_dir, rom_name_clean);
    FILE *f = fopen(filepath, "wb");
    if (f) {
        fwrite(nes_sys.cart->prg_ram, 1, nes_sys.cart->prg_ram_size, f);
        fclose(f);
    }
}

static void load_battery_ram(void) {
    if (!nes_sys.cart || !nes_sys.cart->prg_ram || nes_sys.cart->prg_ram_size == 0) return;
    char rom_name_clean[256];
    get_clean_rom_name(rom_name_clean, sizeof(rom_name_clean));
    char filepath[1024];
    snprintf(filepath, sizeof(filepath), "%s/%s.sav", save_state_dir, rom_name_clean);
    FILE *f = fopen(filepath, "rb");
    if (f) {
        fread(nes_sys.cart->prg_ram, 1, nes_sys.cart->prg_ram_size, f);
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

    uint32_t version = 3;
    fwrite(&version, sizeof(version), 1, f);
    fwrite(&master_volume, sizeof(master_volume), 1, f);
    int temp_muted = audio_muted ? 1 : 0;
    fwrite(&temp_muted, sizeof(temp_muted), 1, f);
    fwrite(&window_scale, sizeof(window_scale), 1, f);
    int temp_fs = fullscreen ? 1 : 0;
    fwrite(&temp_fs, sizeof(temp_fs), 1, f);
    int temp_debug = console_debug_enabled ? 1 : 0;
    fwrite(&temp_debug, sizeof(temp_debug), 1, f);
    fwrite(control_mappings, sizeof(SDL_Keycode), CONTROL_COUNT, f);
    fwrite(controller_button_mappings, sizeof(SDL_GameControllerButton), CONTROL_COUNT, f);
    fclose(f);
}

static void load_emulator_settings(void) {
    char filepath[1024];
    get_settings_filepath(filepath, sizeof(filepath));

    FILE *f = fopen(filepath, "rb");
    if (!f) return;

    uint32_t version = 0;
    if (fread(&version, sizeof(version), 1, f) != 1 || version < 1 || version > 3) {
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
    if (version == 1) {
        fread(control_mappings, sizeof(SDL_Keycode), 8, f);
    } else if (version == 2) {
        fread(control_mappings, sizeof(SDL_Keycode), 8, f);
        fread(controller_button_mappings, sizeof(SDL_GameControllerButton), 8, f);
    } else if (version == 3) {
        fread(control_mappings, sizeof(SDL_Keycode), CONTROL_COUNT, f);
        fread(controller_button_mappings, sizeof(SDL_GameControllerButton), CONTROL_COUNT, f);
    }
    fclose(f);
}

static SDL_GameController *game_controller = NULL;


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

static uint64_t debug_emu_ticks = 0;
static uint64_t debug_total_ticks = 0;

static bool rebinding = false;
static const SDL_Keycode default_control_mappings[CONTROL_COUNT] = {
    SDLK_z, SDLK_x, SDLK_SPACE, SDLK_RETURN, SDLK_UP, SDLK_DOWN, SDLK_LEFT, SDLK_RIGHT, SDLK_F5, SDLK_F8
};
static const char *button_names[CONTROL_COUNT] = {
    "Button A", "Button B", "Select", "Start",
    "D-Pad Up", "D-Pad Down", "D-Pad Left", "D-Pad Right",
    "Quick Save", "Quick Load"
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

    // 1. Scan current working directory (.)
    DIR *d = opendir(".");
    struct dirent *dir;
    if (d) {
        while ((dir = readdir(d)) != NULL) {
            size_t len = strlen(dir->d_name);
            if (len > 4 && (strcmp(dir->d_name + len - 4, ".nes") == 0 || strcmp(dir->d_name + len - 4, ".NES") == 0)) {
                bool duplicate = false;
                for (int i = 0; i < rom_file_count; i++) {
                    if (strcmp(rom_files[i], dir->d_name) == 0) {
                        duplicate = true;
                        break;
                    }
                }
                if (!duplicate) {
                    strncpy(rom_files[rom_file_count], dir->d_name, 255);
                    rom_files[rom_file_count][255] = '\0';
                    rom_file_count++;
                    if (rom_file_count >= 512) break;
                }
            }
        }
        closedir(d);
    }

    // 2. Scan executable base path directory
    char *base_path = SDL_GetBasePath();
    if (base_path && rom_file_count < 512) {
        DIR *db = opendir(base_path);
        if (db) {
            while ((dir = readdir(db)) != NULL) {
                size_t len = strlen(dir->d_name);
                if (len > 4 && (strcmp(dir->d_name + len - 4, ".nes") == 0 || strcmp(dir->d_name + len - 4, ".NES") == 0)) {
                    bool duplicate = false;
                    for (int i = 0; i < rom_file_count; i++) {
                        if (strcmp(rom_files[i], dir->d_name) == 0) {
                            duplicate = true;
                            break;
                        }
                    }
                    if (!duplicate) {
                        strncpy(rom_files[rom_file_count], dir->d_name, 255);
                        rom_files[rom_file_count][255] = '\0';
                        rom_file_count++;
                        if (rom_file_count >= 512) break;
                    }
                }
            }
            closedir(db);
        }
        SDL_free(base_path);
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
    if (nes_sys.cart) {
        switch (nes_sys.cart->mirroring) {
            case MIRROR_HORIZONTAL: mirror_mode_str = "Horizontal"; break;
            case MIRROR_VERTICAL: mirror_mode_str = "Vertical"; break;
            case MIRROR_FOUR_SCREEN: mirror_mode_str = "4-Screen"; break;
            case MIRROR_ONE_SCREEN_LOW: mirror_mode_str = "1-Screen Low"; break;
            case MIRROR_ONE_SCREEN_HIGH: mirror_mode_str = "1-Screen High"; break;
        }
    }

    uint8_t sp = cpu->stack_pointer;
    uint8_t s1 = nes_sys.wram[0x0100 | ((sp + 1) & 0xFF)];
    uint8_t s2 = nes_sys.wram[0x0100 | ((sp + 2) & 0xFF)];
    uint8_t s3 = nes_sys.wram[0x0100 | ((sp + 3) & 0xFF)];
    uint8_t s4 = nes_sys.wram[0x0100 | ((sp + 4) & 0xFF)];

    char disasm[128];
    disassemble_instruction(cpu->program_counter, disasm, sizeof(disasm), cpu);

    printf("\033[H");
    printf("\033[1;36m==================================================\033[0m\033[K\n");
    printf("\033[1;32m                 NES EMULATOR DEBUGGER            \033[0m\033[K\n");
    printf("\033[1;36m==================================================\033[0m\033[K\n");
    printf("\033[1mPerformance:\033[0m  %.2f FPS (Target: 60.10 FPS)\033[K\n", fps);
    printf("\033[1mCPU Speed:\033[0m    %.4f MHz (NES standard: 1.7898 MHz)\033[K\n", cpu_speed_mhz);
    printf("\033[1mHost Load:\033[0m    %.2f%%\033[K\n", cpu_usage);
    printf("\033[1mLoaded ROM:\033[0m   %-30.30s\033[K\n", nes_sys.cart ? loaded_rom_name : "[None]");
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
    printf("Scanline: %-4d  Cycle: %-4d   Status: 0x%02X\033[K\n", nes_sys.ppu.scanline, nes_sys.ppu.cycle, nes_sys.ppu.ppu_status);
    printf("Ctrl:     0x%02X  Mask:  0x%02X   Scroll V: 0x%04X, T: 0x%04X\033[K\n", nes_sys.ppu.ppu_ctrl, nes_sys.ppu.ppu_mask, nes_sys.ppu.v, nes_sys.ppu.t);
    printf("Fine X:   %-4d  Latch W: %-3d  Frame Parity: %s\033[K\n", nes_sys.ppu.x, nes_sys.ppu.w, nes_sys.ppu.odd_frame ? "Odd" : "Even");
    printf("OAM Addr: 0x%02X  Active Scanline Sprites: %d / 8\033[K\n", nes_sys.ppu.oam_addr, nes_sys.ppu.scanline_sprite_count);
    printf("\033[K\n");

    printf("\033[1;33m--- APU Status ---\033[0m\033[K\n");
    printf("Channels Enabled: Pulse1: %s  Pulse2: %s  Triangle: %s  Noise: %s  DMC: %s\033[K\n",
           nes_sys.apu.pulse_enabled[0] ? "On" : "Off",
           nes_sys.apu.pulse_enabled[1] ? "On" : "Off",
           nes_sys.apu.triangle_enabled ? "On" : "Off",
           nes_sys.apu.noise_enabled ? "On" : "Off",
           nes_sys.apu.dmc_enabled ? "On" : "Off");
    printf("Lengths Remaining: P1:%-3d  P2:%-3d  Tri:%-3d  Noise:%-3d\033[K\n",
           nes_sys.apu.pulse_length_counter[0], nes_sys.apu.pulse_length_counter[1],
           nes_sys.apu.triangle_length_counter, nes_sys.apu.noise_length_counter);
    printf("Frame Sequencer:  Mode: %s  APU IRQ Active: %s\033[K\n",
           nes_sys.apu.frame_mode ? "5-Step" : "4-Step",
           nes_sys.apu.frame_irq_active ? "Yes" : "No");
    printf("DMC State:        Sample: 0x%04X  Current: 0x%04X  Left: %-5d  Empty: %s\033[K\n",
           nes_sys.apu.dmc_sample_addr, nes_sys.apu.dmc_current_addr, nes_sys.apu.dmc_bytes_remaining,
           nes_sys.apu.dmc_buffer_empty ? "Yes" : "No");
    printf("\033[K\n");

    if (nes_sys.cart) {
        printf("\033[1;33m--- Mapper State (%d) ---\033[0m\033[K\n", nes_sys.cart->mapper_id);
        printf("Mirroring Mode: %s\033[K\n", mirror_mode_str);
    }
    printf("\033[K\n");

    printf("\033[1;33m--- Controller ---\033[0m\033[K\n");
    printf("P1 State: 0x%02X [", nes_sys.controller_state[0]);
    const char* names[8] = {"A", "B", "SL", "ST", "U", "D", "L", "R"};
    for (int i = 0; i < 8; i++) {
        printf("%s ", (nes_sys.controller_state[0] & (1 << i)) ? names[i] : ".");
    }
    printf("]\033[K\n");
    printf("\033[1;36m==================================================\033[0m\033[K\n");
    printf("\033[J");
    fflush(stdout);
}

static void save_emulator_state(const char *dir, const char *filename) {
    if (!nes_sys.cart) return;
    char filepath[1024];
    snprintf(filepath, sizeof(filepath), "%s/%s", dir, filename);
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

    fwrite(nes_sys.wram, 1, sizeof(nes_sys.wram), f);
    fwrite(nes_sys.ciram, 1, sizeof(nes_sys.ciram), f);
    fwrite(nes_sys.controller_state, 1, sizeof(nes_sys.controller_state), f);
    fwrite(nes_sys.controller_shift, 1, sizeof(nes_sys.controller_shift), f);
    fwrite(&nes_sys.cpu, sizeof(CPU6502), 1, f);
    fwrite(nes_sys.ppu.palette_ram, 1, sizeof(nes_sys.ppu.palette_ram), f);
    fwrite(nes_sys.ppu.oam_ram, 1, sizeof(nes_sys.ppu.oam_ram), f);
    fwrite(&nes_sys.ppu.v, sizeof(nes_sys.ppu.v), 1, f);
    fwrite(&nes_sys.ppu.t, sizeof(nes_sys.ppu.t), 1, f);
    fwrite(&nes_sys.ppu.x, sizeof(nes_sys.ppu.x), 1, f);
    fwrite(&nes_sys.ppu.w, sizeof(nes_sys.ppu.w), 1, f);
    fwrite(&nes_sys.ppu.ppu_ctrl, sizeof(nes_sys.ppu.ppu_ctrl), 1, f);
    fwrite(&nes_sys.ppu.ppu_mask, sizeof(nes_sys.ppu.ppu_mask), 1, f);
    fwrite(&nes_sys.ppu.ppu_status, sizeof(nes_sys.ppu.ppu_status), 1, f);
    fwrite(&nes_sys.ppu.oam_addr, sizeof(nes_sys.ppu.oam_addr), 1, f);
    fwrite(&nes_sys.ppu.buffered_data, sizeof(nes_sys.ppu.buffered_data), 1, f);
    fwrite(&nes_sys.ppu.scanline, sizeof(nes_sys.ppu.scanline), 1, f);
    fwrite(&nes_sys.ppu.cycle, sizeof(nes_sys.ppu.cycle), 1, f);
    fwrite(&nes_sys.apu, sizeof(APU2A03), 1, f);

    uint32_t mirroring_val = (uint32_t)nes_sys.cart->mirroring;
    fwrite(&mirroring_val, sizeof(mirroring_val), 1, f);

    uint32_t prg_ram_sz = (nes_sys.cart->prg_ram != NULL) ? nes_sys.cart->prg_ram_size : 0;
    fwrite(&prg_ram_sz, sizeof(prg_ram_sz), 1, f);
    if (prg_ram_sz > 0) {
        fwrite(nes_sys.cart->prg_ram, 1, prg_ram_sz, f);
    }
    fclose(f);
}

static void load_emulator_state(const char *dir, const char *filename) {
    if (!nes_sys.cart) return;
    char filepath[1024];
    snprintf(filepath, sizeof(filepath), "%s/%s", dir, filename);
    FILE *f = fopen(filepath, "rb");
    if (!f) return;

    uint32_t magic = 0;
    if (fread(&magic, sizeof(magic), 1, f) != 1 || magic != 0x53544154) {
        fclose(f);
        return;
    }
    char rom_meta[64];
    char time_meta[32];
    if (fread(rom_meta, 1, 64, f) != 64 || fread(time_meta, 1, 32, f) != 32) { fclose(f); return; }
    if (fread(nes_sys.wram, 1, sizeof(nes_sys.wram), f) != sizeof(nes_sys.wram)) { fclose(f); return; }
    if (fread(nes_sys.ciram, 1, sizeof(nes_sys.ciram), f) != sizeof(nes_sys.ciram)) { fclose(f); return; }
    if (fread(nes_sys.controller_state, 1, sizeof(nes_sys.controller_state), f) != sizeof(nes_sys.controller_state)) { fclose(f); return; }
    if (fread(nes_sys.controller_shift, 1, sizeof(nes_sys.controller_shift), f) != sizeof(nes_sys.controller_shift)) { fclose(f); return; }
    if (fread(&nes_sys.cpu, sizeof(CPU6502), 1, f) != 1) { fclose(f); return; }
    if (fread(nes_sys.ppu.palette_ram, 1, sizeof(nes_sys.ppu.palette_ram), f) != sizeof(nes_sys.ppu.palette_ram)) { fclose(f); return; }
    if (fread(nes_sys.ppu.oam_ram, 1, sizeof(nes_sys.ppu.oam_ram), f) != sizeof(nes_sys.ppu.oam_ram)) { fclose(f); return; }
    if (fread(&nes_sys.ppu.v, sizeof(nes_sys.ppu.v), 1, f) != 1) { fclose(f); return; }
    if (fread(&nes_sys.ppu.t, sizeof(nes_sys.ppu.t), 1, f) != 1) { fclose(f); return; }
    if (fread(&nes_sys.ppu.x, sizeof(nes_sys.ppu.x), 1, f) != 1) { fclose(f); return; }
    if (fread(&nes_sys.ppu.w, sizeof(nes_sys.ppu.w), 1, f) != 1) { fclose(f); return; }
    if (fread(&nes_sys.ppu.ppu_ctrl, sizeof(nes_sys.ppu.ppu_ctrl), 1, f) != 1) { fclose(f); return; }
    if (fread(&nes_sys.ppu.ppu_mask, sizeof(nes_sys.ppu.ppu_mask), 1, f) != 1) { fclose(f); return; }
    if (fread(&nes_sys.ppu.ppu_status, sizeof(nes_sys.ppu.ppu_status), 1, f) != 1) { fclose(f); return; }
    if (fread(&nes_sys.ppu.oam_addr, sizeof(nes_sys.ppu.oam_addr), 1, f) != 1) { fclose(f); return; }
    if (fread(&nes_sys.ppu.buffered_data, sizeof(nes_sys.ppu.buffered_data), 1, f) != 1) { fclose(f); return; }
    if (fread(&nes_sys.ppu.scanline, sizeof(nes_sys.ppu.scanline), 1, f) != 1) { fclose(f); return; }
    if (fread(&nes_sys.ppu.cycle, sizeof(nes_sys.ppu.cycle), 1, f) != 1) { fclose(f); return; }
    if (fread(&nes_sys.apu, sizeof(APU2A03), 1, f) != 1) { fclose(f); return; }

    uint32_t mirroring_val = 0;
    if (fread(&mirroring_val, sizeof(mirroring_val), 1, f) != 1) { fclose(f); return; }
    nes_sys.cart->mirroring = (MirroringMode)mirroring_val;

    uint32_t prg_ram_sz = 0;
    if (fread(&prg_ram_sz, sizeof(prg_ram_sz), 1, f) == 1 && prg_ram_sz > 0) {
        if (nes_sys.cart->prg_ram != NULL && nes_sys.cart->prg_ram_size >= prg_ram_sz) {
            if (fread(nes_sys.cart->prg_ram, 1, prg_ram_sz, f) != prg_ram_sz) {}
        } else if (nes_sys.cart->prg_ram != NULL) {
            if (fread(nes_sys.cart->prg_ram, 1, nes_sys.cart->prg_ram_size, f) != nes_sys.cart->prg_ram_size) {}
            if (prg_ram_sz > nes_sys.cart->prg_ram_size) {
                fseek(f, prg_ram_sz - nes_sys.cart->prg_ram_size, SEEK_CUR);
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

    for (int i = 0; i < SDL_NumJoysticks(); ++i) {
        if (SDL_IsGameController(i)) {
            game_controller = SDL_GameControllerOpen(i);
            if (game_controller) break;
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

    nes_init(&nes_sys);

    cpu_bus_bridge.bus_context = &nes_sys;
    cpu_bus_bridge.read = cpu_bridge_read;
    cpu_bus_bridge.write = cpu_bridge_write;
    cpu_bus_bridge.cycle_tick = NULL;

    bool running = true;
    SDL_Event event;
    scan_rom_directory();

    while (running) {
        if (current_state == GUI_STATE_GAMEPLAY && nes_sys.cart != NULL) {
            if (debugger_active) {
                debugger_render(renderer, &nes_sys.cpu);
                SDL_RenderPresent(renderer);
                SDL_Delay(16);
                update_console_debug(&nes_sys.cpu);
            } else {
                uint64_t frame_start_tick = SDL_GetPerformanceCounter();

                nes_sys.frame_ready = false;
                while (!nes_sys.frame_ready) {
                    if (breakpoints[nes_sys.cpu.program_counter]) {
                        debugger_active = true;
                        debugger_view_pc = nes_sys.cpu.program_counter;
                        debugger_selected_line = 0;
                        clear_view_history(debugger_view_pc);
                        break;
                    }
                    if (debugger_logging_active && nes_sys.clock.cpu_divider == 0) {
                        debugger_log_instruction(&nes_sys.cpu);
                    }
                    nes_clock_tick(&nes_sys);
                }

                uint64_t emu_end_tick = SDL_GetPerformanceCounter();
                debug_emu_ticks += (emu_end_tick - frame_start_tick);

                SDL_UpdateTexture(texture, NULL, nes_sys.ppu.screen_buffer, 256 * sizeof(uint32_t));
                SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
                SDL_RenderClear(renderer);

                SDL_Rect src_rect = { 0, 0, 256, 240 };
                if (nes_sys.cart != NULL) {
                    uint8_t mapper = nes_sys.cart->mapper_id;
                    if (mapper == 0 || mapper == 4 || mapper == 206 || mapper == 227) {
                        src_rect.x = 8;
                        src_rect.y = 8;
                        src_rect.w = 240;
                        src_rect.h = 224;
                    } else if (mapper == 1) {
                        src_rect.x = 8;
                        src_rect.y = 0;
                        src_rect.w = 240;
                        src_rect.h = 240;
                    }
                }
                SDL_RenderCopy(renderer, texture, &src_rect, NULL);

                if (notification_timer > 0) {
                    notification_timer--;
                    int text_w = (int)strlen(notification_text) * 8;
                    int x = 256 - text_w - 12;
                    int y = 240 - 8 - 12;

                    for (int dx = -2; dx <= 2; dx++) {
                        for (int dy = -2; dy <= 2; dy++) {
                            if (abs(dx) == 2 || abs(dy) == 2) {
                                draw_string(renderer, notification_text, x + dx, y + dy, 0xFFFFFF);
                            }
                        }
                    }
                    for (int dx = -1; dx <= 1; dx++) {
                        for (int dy = -1; dy <= 1; dy++) {
                            if (dx != 0 || dy != 0) {
                                draw_string(renderer, notification_text, x + dx, y + dy, 0x000000);
                            }
                        }
                    }
                    draw_string(renderer, notification_text, x, y, 0x00FF00);
                }

                SDL_RenderPresent(renderer);

                if (audio_device != 0 && nes_sys.apu.audio_buffer_idx > 0) {
                    if (!audio_muted) {
                        for (uint32_t i = 0; i < nes_sys.apu.audio_buffer_idx; i++) {
                            nes_sys.apu.audio_buffer[i] *= ((float)master_volume / 100.0f);
                        }
                        SDL_QueueAudio(audio_device, nes_sys.apu.audio_buffer, nes_sys.apu.audio_buffer_idx * sizeof(float));
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
                    nes_sys.apu.audio_buffer_idx = 0;
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

                update_console_debug(&nes_sys.cpu);
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
                    if ((i == 0 || i == 2 || i == 3) && nes_sys.cart == NULL) {
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
                char header_buf[64];
                if (control_mode_keyboard) {
                    snprintf(header_buf, sizeof(header_buf), "Mode: < KEYBOARD >");
                } else {
                    snprintf(header_buf, sizeof(header_buf), "Mode: < CONTROLLER >");
                }
                uint32_t header_col = (menu_selection == 0) ? 0xFFFFFF : 0x00FFFF;
                draw_string(renderer, header_buf, 16, 45, header_col);
                if (menu_selection == 0) {
                    SDL_SetRenderDrawColor(renderer, 0, 255, 0, 255);
                    SDL_Rect box = { 12, 43, 232, 11 };
                    SDL_RenderDrawRect(renderer, &box);
                }

                draw_string(renderer, "-----------------------------", 16, 52, 0x00FFFF);
                for (int i = 0; i < CONTROL_COUNT; i++) {
                    char buf[64];
                    if (control_mode_keyboard) {
                        const char *key_name = SDL_GetKeyName(control_mappings[i]);
                        if (rebinding && (i + 1) == menu_selection) {
                            snprintf(buf, sizeof(buf), "%-11s -> [PRESS KEY...]", button_names[i]);
                        } else {
                            snprintf(buf, sizeof(buf), "%-11s -> %s", button_names[i], key_name);
                        }
                    } else {
                        const char *btn_name = "Unknown";
                        switch (controller_button_mappings[i]) {
                            case SDL_CONTROLLER_BUTTON_A: btn_name = "Button A"; break;
                            case SDL_CONTROLLER_BUTTON_B: btn_name = "Button B"; break;
                            case SDL_CONTROLLER_BUTTON_X: btn_name = "Button X"; break;
                            case SDL_CONTROLLER_BUTTON_Y: btn_name = "Button Y"; break;
                            case SDL_CONTROLLER_BUTTON_BACK: btn_name = "Back"; break;
                            case SDL_CONTROLLER_BUTTON_GUIDE: btn_name = "Guide"; break;
                            case SDL_CONTROLLER_BUTTON_START: btn_name = "Start"; break;
                            case SDL_CONTROLLER_BUTTON_LEFTSTICK: btn_name = "Left Stick"; break;
                            case SDL_CONTROLLER_BUTTON_RIGHTSTICK: btn_name = "Right Stick"; break;
                            case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: btn_name = "Left Shoulder"; break;
                            case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: btn_name = "Right Shoulder"; break;
                            case SDL_CONTROLLER_BUTTON_DPAD_UP: btn_name = "D-Pad Up"; break;
                            case SDL_CONTROLLER_BUTTON_DPAD_DOWN: btn_name = "D-Pad Down"; break;
                            case SDL_CONTROLLER_BUTTON_DPAD_LEFT: btn_name = "D-Pad Left"; break;
                            case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: btn_name = "D-Pad Right"; break;
                            default: btn_name = "None"; break;
                        }
                        if (rebinding && (i + 1) == menu_selection) {
                            snprintf(buf, sizeof(buf), "%-11s -> [PRESS BUTTON...]", button_names[i]);
                        } else {
                            snprintf(buf, sizeof(buf), "%-11s -> %s", button_names[i], btn_name);
                        }
                    }
                    uint32_t col = ((i + 1) == menu_selection) ? 0xFFFFFF : 0x888888;
                    draw_string(renderer, buf, 24, 62 + i * 13, col);
                    if ((i + 1) == menu_selection) {
                        SDL_SetRenderDrawColor(renderer, 0, 255, 0, 255);
                        SDL_Rect box = { 16, 60 + i * 13, 224, 11 };
                        SDL_RenderDrawRect(renderer, &box);
                    }
                }
                uint32_t def_col = (menu_selection == (CONTROL_COUNT + 1)) ? 0xFFFFFF : 0x888888;
                draw_string(renderer, "Restore Defaults", 24, 62 + CONTROL_COUNT * 13, def_col);
                if (menu_selection == (CONTROL_COUNT + 1)) {
                    SDL_SetRenderDrawColor(renderer, 0, 255, 0, 255);
                    SDL_Rect box = { 16, 60 + CONTROL_COUNT * 13, 224, 11 };
                    SDL_RenderDrawRect(renderer, &box);
                }
                draw_string(renderer, "ENTER/LEFT/RIGHT: Adjust | ESC: Return", 8, 225, 0x888888);
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
                    if (rom_scroll_offset > 0) draw_string(renderer, "^", 236, 68, 0x00FF00);
                    if (end_idx < rom_file_count) draw_string(renderer, "v", 236, 68 + 11 * 12, 0x00FF00);
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
            update_console_debug(&nes_sys.cpu);
        }

        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                running = false;
            } else if (event.type == SDL_MOUSEBUTTONDOWN) {
                if (current_state == GUI_STATE_GAMEPLAY && !debugger_active) {
                    if (event.button.button == SDL_BUTTON_LEFT) {
                        int mx = event.button.x;
                        int my = event.button.y;
                        if (mx < 0) mx = 0;
                        if (mx > 255) mx = 255;
                        if (my < 0) my = 0;
                        if (my > 239) my = 239;
                        nes_sys.zapper_x = mx;
                        nes_sys.zapper_y = my;
                        nes_sys.zapper_trigger = true;
                    }
                }
            } else if (event.type == SDL_MOUSEBUTTONUP) {
                if (current_state == GUI_STATE_GAMEPLAY) {
                    if (event.button.button == SDL_BUTTON_LEFT) {
                        nes_sys.zapper_trigger = false;
                    }
                }
            } else if (event.type == SDL_MOUSEMOTION) {
                if (current_state == GUI_STATE_GAMEPLAY) {
                    int mx = event.motion.x;
                    int my = event.motion.y;
                    if (mx < 0) mx = 0;
                    if (mx > 255) mx = 255;
                    if (my < 0) my = 0;
                    if (my > 239) my = 239;
                    nes_sys.zapper_x = mx;
                    nes_sys.zapper_y = my;
                }
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
                                if (game_controller) break;
                            }
                        }
                    }
                }
            } else if (event.type == SDL_CONTROLLERBUTTONDOWN) {
                if (rebinding && !control_mode_keyboard) {
                    controller_button_mappings[menu_selection - 1] = event.cbutton.button;
                    save_emulator_settings();
                    rebinding = false;
                    continue;
                }
                if (current_state == GUI_STATE_GAMEPLAY && !debugger_active) {
                    if (event.cbutton.button == controller_button_mappings[8]) {
                        char filename[128];
                        get_rolling_quicksave_filename(filename, sizeof(filename), true);
                        save_emulator_state(save_state_dir, filename);
                        show_notification("STATE SAVED");
                    } else if (event.cbutton.button == controller_button_mappings[9]) {
                        char filename[128];
                        get_rolling_quicksave_filename(filename, sizeof(filename), false);
                        load_emulator_state(save_state_dir, filename);
                        show_notification("STATE LOADED");
                    } else {
                        for (int i = 0; i < 8; i++) {
                            if (event.cbutton.button == controller_button_mappings[i]) {
                                nes_sys.controller_state[0] |= (1 << i);
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
                            nes_sys.controller_state[0] &= ~(1 << i);
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
                            nes_sys.controller_state[0] |= (1 << 6);
                            nes_sys.controller_state[0] &= ~(1 << 7);
                        } else if (event.caxis.value > 16000) {
                            nes_sys.controller_state[0] |= (1 << 7);
                            nes_sys.controller_state[0] &= ~(1 << 6);
                        } else {
                            nes_sys.controller_state[0] &= ~(1 << 6);
                            nes_sys.controller_state[0] &= ~(1 << 7);
                        }
                    }
                    if (event.caxis.axis == SDL_CONTROLLER_AXIS_LEFTY) {
                        if (event.caxis.value < -16000) {
                            nes_sys.controller_state[0] |= (1 << 4);
                            nes_sys.controller_state[0] &= ~(1 << 5);
                        } else if (event.caxis.value > 16000) {
                            nes_sys.controller_state[0] |= (1 << 5);
                            nes_sys.controller_state[0] &= ~(1 << 4);
                        } else {
                            nes_sys.controller_state[0] &= ~(1 << 4);
                            nes_sys.controller_state[0] &= ~(1 << 5);
                        }
                    }
                }
            } else if (event.type == SDL_KEYDOWN) {
                if (rebinding) {
                    if (control_mode_keyboard && event.key.keysym.sym != SDLK_ESCAPE) {
                        control_mappings[menu_selection - 1] = event.key.keysym.sym;
                        save_emulator_settings();
                    }
                    rebinding = false;
                    break;
                }

                if (event.key.keysym.sym == SDLK_ESCAPE || event.key.keysym.sym == SDLK_F1) {
                    if (current_state == GUI_STATE_GAMEPLAY) {
                        current_state = GUI_STATE_MENU_MAIN;
                        menu_selection = nes_sys.cart ? 0 : 1;
                    } else if (current_state == GUI_STATE_MENU_MAIN) {
                        if (nes_sys.cart != NULL) {
                            current_state = GUI_STATE_GAMEPLAY;
                        }
                    } else {
                        current_state = GUI_STATE_MENU_MAIN;
                        menu_selection = nes_sys.cart ? 0 : 1;
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
                                    else if (current_state == GUI_STATE_MENU_CONTROLS) menu_selection = (CONTROL_COUNT + 1);
                                }
                            } while (current_state == GUI_STATE_MENU_MAIN && nes_sys.cart == NULL && (menu_selection == 0 || menu_selection == 2 || menu_selection == 3));
                            break;
                        case SDLK_DOWN:
                            do {
                                menu_selection++;
                                if (current_state == GUI_STATE_MENU_MAIN && menu_selection > 6) menu_selection = 0;
                                else if (current_state == GUI_STATE_MENU_LOAD_ROM && menu_selection >= rom_file_count) menu_selection = 0;
                                else if (current_state == GUI_STATE_MENU_SAVE_STATE && menu_selection > state_file_count) menu_selection = 0;
                                else if (current_state == GUI_STATE_MENU_LOAD_STATE && menu_selection >= state_file_count) menu_selection = 0;
                                else if (current_state == GUI_STATE_MENU_SETTINGS && menu_selection > 4) menu_selection = 0;
                                else if (current_state == GUI_STATE_MENU_CONTROLS && menu_selection > (CONTROL_COUNT + 1)) menu_selection = 0;
                            } while (current_state == GUI_STATE_MENU_MAIN && nes_sys.cart == NULL && (menu_selection == 0 || menu_selection == 2 || menu_selection == 3));
                            break;
                        case SDLK_LEFT:
                            if (current_state == GUI_STATE_MENU_SETTINGS && menu_selection == 2) {
                                master_volume -= 10;
                                if (master_volume < 0) master_volume = 0;
                                play_volume_ding();
                                save_emulator_settings();
                            } else if (current_state == GUI_STATE_MENU_CONTROLS && menu_selection == 0) {
                                control_mode_keyboard = !control_mode_keyboard;
                            }
                            break;
                        case SDLK_RIGHT:
                            if (current_state == GUI_STATE_MENU_SETTINGS && menu_selection == 2) {
                                master_volume += 10;
                                if (master_volume > 100) master_volume = 100;
                                play_volume_ding();
                                save_emulator_settings();
                            } else if (current_state == GUI_STATE_MENU_CONTROLS && menu_selection == 0) {
                                control_mode_keyboard = !control_mode_keyboard;
                            }
                            break;
                        case SDLK_BACKSPACE:
                            if (current_state != GUI_STATE_MENU_MAIN) {
                                current_state = GUI_STATE_MENU_MAIN;
                                menu_selection = nes_sys.cart ? 0 : 1;
                            }
                            break;
                        case SDLK_RETURN:
                            if (current_state == GUI_STATE_MENU_MAIN) {
                                if (menu_selection == 0) {
                                    if (nes_sys.cart != NULL) {
                                        current_state = GUI_STATE_GAMEPLAY;
                                    }
                                } else if (menu_selection == 1) {
                                    current_state = GUI_STATE_MENU_LOAD_ROM;
                                    menu_selection = 0;
                                    rom_scroll_offset = 0;
                                    scan_rom_directory();
                                } else if (menu_selection == 2) {
                                    if (nes_sys.cart != NULL) {
                                        current_state = GUI_STATE_MENU_SAVE_STATE;
                                        menu_selection = 0;
                                        rom_scroll_offset = 0;
                                        scan_save_state_directory();
                                    }
                                } else if (menu_selection == 3) {
                                    if (nes_sys.cart != NULL) {
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
                                if (menu_selection == 0) {
                                    control_mode_keyboard = !control_mode_keyboard;
                                } else if (menu_selection == (CONTROL_COUNT + 1)) {
                                    for (int i = 0; i < CONTROL_COUNT; i++) {
                                        control_mappings[i] = default_control_mappings[i];
                                        controller_button_mappings[i] = default_controller_mappings[i];
                                    }
                                    save_emulator_settings();
                                } else {
                                    rebinding = true;
                                }
                            } else if (current_state == GUI_STATE_MENU_LOAD_ROM) {
                                if (rom_file_count > 0) {
                                    if (nes_sys.cart) {
                                        save_battery_ram();
                                        cartridge_free(nes_sys.cart);
                                        nes_sys.cart = NULL;
                                        cleanup_default_sav();
                                    }

                                    nes_init(&nes_sys);

                                    Cartridge *cart = cartridge_load(&nes_sys, rom_files[menu_selection]);
                                    if (!cart) {
                                        char *base_path = SDL_GetBasePath();
                                        if (base_path) {
                                            char full_path[1024];
                                            snprintf(full_path, sizeof(full_path), "%s%s", base_path, rom_files[menu_selection]);
                                            cart = cartridge_load(&nes_sys, full_path);
                                            SDL_free(base_path);
                                        }
                                    }

                                    if (cart) {
                                        nes_sys.cart = cart;
                                        strncpy(loaded_rom_name, rom_files[menu_selection], sizeof(loaded_rom_name) - 1);
                                        loaded_rom_name[sizeof(loaded_rom_name) - 1] = '\0';

                                        char rom_name_clean[256];
                                        strncpy(rom_name_clean, rom_files[menu_selection], sizeof(rom_name_clean) - 1);
                                        rom_name_clean[sizeof(rom_name_clean) - 1] = '\0';
                                        char *ext = strrchr(rom_name_clean, '.');
                                        if (ext) *ext = '\0';

                                        char *base_path = SDL_GetBasePath();
                                        if (base_path) {
                                            snprintf(save_state_dir, sizeof(save_state_dir), "%s%s/%s", base_path, "saves", rom_name_clean);
                                            SDL_free(base_path);
                                        } else {
                                            snprintf(save_state_dir, sizeof(save_state_dir), "%s/%s", "saves", rom_name_clean);
                                        }

                                        char saves_root_dir[512];
                                        strncpy(saves_root_dir, save_state_dir, sizeof(saves_root_dir) - 1);
                                        saves_root_dir[sizeof(saves_root_dir) - 1] = '\0';
                                        char *last_slash = strrchr(saves_root_dir, '/');
                                        if (last_slash) *last_slash = '\0';
                                        MKDIR(saves_root_dir);
                                        MKDIR(save_state_dir);

                                        nes_reset(&nes_sys);
                                        cpu_reset(&nes_sys.cpu, &cpu_bus_bridge);
                                        debugger_init();
                                        load_battery_ram();
                                        debugger_active = console_debug_enabled;
                                        debugger_logging_active = false;
                                        if (debugger_active) {
                                            debugger_view_pc = nes_sys.cpu.program_counter;
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
                                    save_emulator_state(save_state_dir, name_buf);
                                } else {
                                    save_emulator_state(save_state_dir, state_files[menu_selection - 1]);
                                }
                                current_state = GUI_STATE_GAMEPLAY;
                            } else if (current_state == GUI_STATE_MENU_LOAD_STATE) {
                                if (state_file_count > 0) {
                                    load_emulator_state(save_state_dir, state_files[menu_selection]);
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
                    SDL_Keycode sym = event.key.keysym.sym;
                    if (sym == control_mappings[8]) {
                        char filename[128];
                        get_rolling_quicksave_filename(filename, sizeof(filename), true);
                        save_emulator_state(save_state_dir, filename);
                        show_notification("STATE SAVED");
                    } else if (sym == control_mappings[9]) {
                        char filename[128];
                        get_rolling_quicksave_filename(filename, sizeof(filename), false);
                        load_emulator_state(save_state_dir, filename);
                        show_notification("STATE LOADED");
                    } else {
                        switch (sym) {
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
                                        nes_sys.controller_state[0] |= (1 << i);
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
                                        nes_sys.controller_state[0] |= (1 << i);
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
                                debugger_step_instruction(&nes_sys.cpu, &cpu_bus_bridge);
                                clear_view_history(debugger_view_pc);
                            } else {
                                debugger_active = true;
                                debugger_view_pc = nes_sys.cpu.program_counter;
                                debugger_selected_line = 0;
                                clear_view_history(debugger_view_pc);
                            }
                            break;
                        }
                        case SDLK_F9: {
                            if (debugger_active) {
                                debugger_step_instruction(&nes_sys.cpu, &cpu_bus_bridge);
                                debugger_active = false;
                            } else {
                                debugger_active = true;
                                debugger_view_pc = nes_sys.cpu.program_counter;
                                debugger_selected_line = 0;
                                clear_view_history(debugger_view_pc);
                            }
                            break;
                        }
                        case SDLK_F12: {
                            if (debugger_active) {
                                cpu_trigger_reset(&nes_sys.cpu);
                                cpu_step(&nes_sys.cpu, &cpu_bus_bridge);
                            }
                            break;
                        }
                        default: {
                            if (!debugger_active) {
                                for (int i = 0; i < 8; i++) {
                                    if (event.key.keysym.sym == control_mappings[i]) {
                                        nes_sys.controller_state[0] |= (1 << i);
                                    }
                                }
                            }
                            break;
                        }
                    }
                }
        }
            } else if (event.type == SDL_KEYUP && current_state == GUI_STATE_GAMEPLAY) {
                for (int i = 0; i < 8; i++) {
                    if (event.key.keysym.sym == control_mappings[i]) {
                        nes_sys.controller_state[0] &= ~(1 << i);
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

    if (nes_sys.cart) {
        save_battery_ram();
        cartridge_free(nes_sys.cart);
        cleanup_default_sav();
    }
    save_emulator_settings();
    return 0;
}