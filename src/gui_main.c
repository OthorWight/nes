#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "nes_dirent.h"
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#endif
#include <inttypes.h>
#include <math.h>
#include "host.h"
#include "nes_system.h"
#include "debugger.h"
#include "save_state.h"
#include "state_io.h"
#include "diagnostics.h"
#include "frontend_runtime.h"
#include "rom_preferences.h"

NES nes_sys;
static CPUBus cpu_bus_bridge;

static char loaded_rom_name[256] = "";
static char save_state_dir[512] = "";
static bool audio_device;
static bool audio_muted = false;

#ifdef _WIN32
#define MKDIR(path) _mkdir(path)
#else
#define MKDIR(path) mkdir(path, 0777)
#endif

static bool console_debug_enabled = false;
static int window_scale = 5;
static bool fullscreen = false;
static int master_volume = 100;
static bool performance_visible = false;
static bool focused = true;
static FrameScheduler frame_scheduler;
static AudioQueueMonitor audio_monitor;
static AudioResampler audio_resampler;
static NESDiagnostics diagnostics;
static HostInput host_input;
static double audio_device_ms;
static double diagnostic_last_time;
static bool runtime_reset_pending = true;
static RomPreferences global_preferences = {5, false, false};
static bool rom_override;
static bool preferences_inherit = true;

static void clear_host_input(void) {
    memset(&host_input, 0, sizeof(host_input));
    memset(nes_sys.controller_state, 0, sizeof(nes_sys.controller_state));
    nes_sys.zapper_trigger = nes_sys.zapper_light = false;
    nes_sys.zapper_x = nes_sys.zapper_y = -1;
}

static double host_time(void) {
    return (double)host_counter() / host_frequency();
}

static void reset_runtime(void) {
    frame_scheduler_reset(&frame_scheduler, host_time());
    diagnostic_last_time = host_time();
    if (audio_device) {
        host_audio_pause(1);
        host_audio_clear();
    }
    audio_queue_pause(&audio_monitor);
    memset(&audio_resampler, 0, sizeof(audio_resampler));
    nes_sys.apu.audio_buffer_idx = 0;
    memset(diagnostics.polls, 0, sizeof(diagnostics.polls));
    diagnostics.latches = 0;
    diagnostics_event(&nes_sys, DIAG_RESUME, 0, 0);
    runtime_reset_pending = false;
}

static void queue_game_audio(void) {
    uint32_t count = nes_sys.apu.audio_buffer_idx;
    if (audio_device && !audio_muted) {
        uint32_t queued = host_audio_queued_bytes() / sizeof(float);
        // Enough room for the APU's entire buffer plus the bounded correction.
        float output[4096 + 64];
        double ratio = audio_queue_ratio(&audio_monitor, queued);
        uint32_t output_count = audio_resample(&audio_resampler,
            nes_sys.apu.audio_buffer, count, ratio, output);
        if (audio_queue_observe(&audio_monitor, queued, output_count)) {
            host_audio_pause(1);
            host_audio_clear();
            queued = 0;
        }
        for (uint32_t i = 0; i < output_count; ++i)
            output[i] *= master_volume / 100.0f;
        if (host_audio_queue(output, output_count * sizeof(float))) {
            ++audio_monitor.errors;
        } else {
            queued += output_count;
            audio_monitor.queue_samples = queued;
            if (!audio_monitor.playing && queued >= AUDIO_PRIME_SAMPLES) {
                audio_monitor.playing = true;
                host_audio_pause(0);
            }
        }
    }
    nes_sys.apu.audio_buffer_idx = 0;
}

#define CONTROL_COUNT 10

static HostKey control_mappings[CONTROL_COUNT] = {
    HOST_KEY_z,
    HOST_KEY_x,
    HOST_KEY_SPACE,
    HOST_KEY_RETURN,
    HOST_KEY_UP,
    HOST_KEY_DOWN,
    HOST_KEY_LEFT,
    HOST_KEY_RIGHT,
    HOST_KEY_F5,
    HOST_KEY_F8
};

static HostButton controller_button_mappings[CONTROL_COUNT] = {
    HOST_CONTROLLER_BUTTON_A,
    HOST_CONTROLLER_BUTTON_B,
    HOST_CONTROLLER_BUTTON_BACK,
    HOST_CONTROLLER_BUTTON_START,
    HOST_CONTROLLER_BUTTON_DPAD_UP,
    HOST_CONTROLLER_BUTTON_DPAD_DOWN,
    HOST_CONTROLLER_BUTTON_DPAD_LEFT,
    HOST_CONTROLLER_BUTTON_DPAD_RIGHT,
    HOST_CONTROLLER_BUTTON_X,
    HOST_CONTROLLER_BUTTON_Y
};

static const HostButton default_controller_mappings[CONTROL_COUNT] = {
    HOST_CONTROLLER_BUTTON_A,
    HOST_CONTROLLER_BUTTON_B,
    HOST_CONTROLLER_BUTTON_BACK,
    HOST_CONTROLLER_BUTTON_START,
    HOST_CONTROLLER_BUTTON_DPAD_UP,
    HOST_CONTROLLER_BUTTON_DPAD_DOWN,
    HOST_CONTROLLER_BUTTON_DPAD_LEFT,
    HOST_CONTROLLER_BUTTON_DPAD_RIGHT,
    HOST_CONTROLLER_BUTTON_X,
    HOST_CONTROLLER_BUTTON_Y
};

static bool control_mode_keyboard = true;

static uint8_t cpu_bridge_read(void *ctx, uint16_t addr) {
    return nes_cpu_bus_read((NES*)ctx, addr);
}

static void cpu_bridge_write(void *ctx, uint16_t addr, uint8_t data) {
    nes_cpu_bus_write((NES*)ctx, addr, data);
}

static void play_volume_ding(void) {
    if (audio_device == 0 || audio_muted) return;
    host_audio_clear();

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
        host_audio_queue(temp_nes.apu.audio_buffer, temp_nes.apu.audio_buffer_idx * sizeof(float));
        host_audio_pause(0);
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
    unsigned char magic[8] = {0};
    size_t count = fread(magic, 1, sizeof(magic), f);
    fclose(f);
    if (count != 8 || memcmp(magic, "NESSTATE", 8) != 0) {
        snprintf(out_buf, max_len, "%.40s: [Old or invalid state]", filename);
        return;
    }
    struct stat st;
    char time_meta[32] = "";
    if (stat(filepath, &st) == 0) {
        struct tm *tm_info = localtime(&st.st_mtime);
        if (tm_info) strftime(time_meta, sizeof(time_meta), "%m/%d %H:%M", tm_info);
    }
    snprintf(out_buf, max_len, "%.40s: %s", filename, time_meta);
}

static void get_clean_rom_name(char *out_buf, size_t max_len) {
    snprintf(out_buf, max_len, "%s", loaded_rom_name);
    char *ext = strrchr(out_buf, '.');
    if (ext) *ext = '\0';
}

static bool save_battery_ram(void) {
    if (cartridge_save_battery(nes_sys.cart)) return true;
    show_notification("BATTERY SAVE FAILED");
    notification_timer = 180;
    fprintf(stderr, "Battery save failed: %s\n", nes_sys.cart->save_filepath);
    return false;
}

static bool load_battery_ram(void) {
    if (!nes_sys.cart) return true;
    char rom_name_clean[256];
    get_clean_rom_name(rom_name_clean, sizeof(rom_name_clean));
    char filepath[1024];
    snprintf(filepath, sizeof(filepath), "%s/%s.sav", save_state_dir, rom_name_clean);
    if (!cartridge_set_save_path(nes_sys.cart, filepath)) {
        nes_sys.cart->battery_save_blocked = true;
        show_notification("BATTERY LOAD FAILED");
        fprintf(stderr, "Battery load failed; existing saves protected: %s\n", filepath);
        notification_timer = 180;
        return false;
    }
    return true;
}

static void get_settings_filepath(char *out_path, size_t max_len) {
    char *base_path = host_base_path();
    if (base_path) {
        snprintf(out_path, max_len, "%ssaves/settings.bin", base_path);
        free(base_path);
    } else {
        snprintf(out_path, max_len, "saves/settings.bin");
    }
}

static bool zapper_enabled = false;

static void rom_preferences_path(char *path, size_t size) {
    char root[1024]; get_settings_filepath(root, sizeof(root));
    char *slash = strrchr(root, '/');
    if (slash) *slash = '\0';
    snprintf(path, size, "%s/%08X-%08X-%08X.prefs", root,
        nes_sys.cart->rom_identity[0], nes_sys.cart->rom_identity[1], nes_sys.cart->rom_identity[2]);
}

static void apply_display(void) { host_display(window_scale, fullscreen); }

static void apply_rom_preferences(void) {
    RomPreferences value = global_preferences;
    rom_override = false;
    if (nes_sys.cart) {
        char path[1200]; rom_preferences_path(path, sizeof(path));
        if (!rom_preferences_load(path, nes_sys.cart->rom_identity, global_preferences, &value, &rom_override))
            show_notification("BAD ROM SETTINGS: DEFAULTS");
    }
    preferences_inherit = !rom_override;
    window_scale = (int)value.scale; fullscreen = value.fullscreen;
    zapper_enabled = value.zapper; nes_sys.zapper_enabled = zapper_enabled;
    apply_display();
    clear_host_input(); runtime_reset_pending = true;
}

static void save_emulator_settings(void) {
    char filepath[1024];
    get_settings_filepath(filepath, sizeof(filepath));

    char saves_dir[1024];
    char *base_path = host_base_path();
    if (base_path) {
        snprintf(saves_dir, sizeof(saves_dir), "%ssaves", base_path);
        free(base_path);
    } else {
        snprintf(saves_dir, sizeof(saves_dir), "saves");
    }
    MKDIR(saves_dir);

    if (!nes_sys.cart)
        global_preferences = (RomPreferences){(unsigned)window_scale, fullscreen, zapper_enabled};
    else {
        char path[1200]; rom_preferences_path(path, sizeof(path));
        RomPreferences value = {(unsigned)window_scale, fullscreen, zapper_enabled};
        if (!rom_preferences_save(path, nes_sys.cart->rom_identity, value, !preferences_inherit))
            show_notification("ROM SETTINGS SAVE FAILED");
        else rom_override = !preferences_inherit;
    }
    uint8_t data[128];
    StateIO io = {data, sizeof(data), 0, false, true};
    state_u32(&io, 5);
    state_i32(&io, master_volume);
    state_i32(&io, audio_muted ? 1 : 0);
    state_i32(&io, (int)global_preferences.scale);
    state_i32(&io, global_preferences.fullscreen ? 1 : 0);
    state_i32(&io, console_debug_enabled ? 1 : 0);
    for (unsigned i = 0; i < CONTROL_COUNT; ++i) state_i32(&io, control_mappings[i]);
    for (unsigned i = 0; i < CONTROL_COUNT; ++i) state_i32(&io, controller_button_mappings[i]);
    state_i32(&io, global_preferences.zapper ? 1 : 0);
    state_i32(&io, performance_visible ? 1 : 0);
    if (!io.ok || !state_atomic_write(filepath, data, io.pos)) show_notification("GLOBAL SETTINGS SAVE FAILED");
}

static void load_emulator_settings(void) {
    zapper_enabled = false;
    char filepath[1024];
    get_settings_filepath(filepath, sizeof(filepath));

    FILE *f = fopen(filepath, "rb");
    if (!f) return;

    uint32_t version = 0;
    if (fread(&version, sizeof(version), 1, f) != 1 || version < 1 || version > 5) {
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
        fread(control_mappings, sizeof(HostKey), 8, f);
    } else if (version == 2) {
        fread(control_mappings, sizeof(HostKey), 8, f);
        fread(controller_button_mappings, sizeof(HostButton), 8, f);
    } else if (version >= 3) {
        fread(control_mappings, sizeof(HostKey), CONTROL_COUNT, f);
        fread(controller_button_mappings, sizeof(HostButton), CONTROL_COUNT, f);
    }
    if (version >= 4) {
        int temp_zapper = 0;
        if (fread(&temp_zapper, sizeof(temp_zapper), 1, f) == 1) {
            zapper_enabled = (temp_zapper == 1);
        }
    }
    if (version >= 5) {
        int temp_perf = 0;
        if (fread(&temp_perf, sizeof(temp_perf), 1, f) == 1) performance_visible = temp_perf == 1;
    }
    if (window_scale < 1 || window_scale > 5) window_scale = 5;
    if (master_volume < 0 || master_volume > 100) master_volume = 100;
    global_preferences = (RomPreferences){(unsigned)window_scale, fullscreen, zapper_enabled};
    fclose(f);
}

static int game_controller = -1;


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
static const HostKey default_control_mappings[CONTROL_COUNT] = {
    HOST_KEY_z, HOST_KEY_x, HOST_KEY_SPACE, HOST_KEY_RETURN, HOST_KEY_UP, HOST_KEY_DOWN, HOST_KEY_LEFT, HOST_KEY_RIGHT, HOST_KEY_F5, HOST_KEY_F8
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

static int menu_item_count(void) {
    switch (current_state) {
        case GUI_STATE_MENU_MAIN: return 7;
        case GUI_STATE_MENU_LOAD_ROM: return rom_file_count;
        case GUI_STATE_MENU_SAVE_STATE: return state_file_count + 1;
        case GUI_STATE_MENU_LOAD_STATE: return state_file_count;
        case GUI_STATE_MENU_SETTINGS: return 10;
        case GUI_STATE_MENU_CONTROLS: return CONTROL_COUNT + 2;
        default: return 0;
    }
}

static bool menu_is_file_list(void) {
    return current_state == GUI_STATE_MENU_LOAD_ROM ||
        current_state == GUI_STATE_MENU_SAVE_STATE || current_state == GUI_STATE_MENU_LOAD_STATE;
}

// These logical rectangles are shared by drawing and mouse hit testing.
static HostRect menu_item_rect(int item) {
    switch (current_state) {
        case GUI_STATE_MENU_MAIN: return (HostRect){32, 58 + item * 15, 180, 11};
        case GUI_STATE_MENU_SETTINGS: return (HostRect){18, 58 + item * 13, 226, 11};
        case GUI_STATE_MENU_CONTROLS:
            if (item == 0) return (HostRect){12, 43, 232, 11};
            return (HostRect){16, 60 + (item - 1) * 13, 224, 11};
        default: return (HostRect){24, 68 + (item - rom_scroll_offset) * 12, 208, 11};
    }
}

static int menu_hit_test(int x, int y) {
    HostPoint point = {x, y};
    int start = menu_is_file_list() ? rom_scroll_offset : 0;
    int end = menu_item_count();
    if (menu_is_file_list() && end > start + 12) end = start + 12;
    for (int i = start; i < end; ++i) {
        if (current_state == GUI_STATE_MENU_MAIN && !nes_sys.cart &&
            (i == 0 || i == 2 || i == 3)) continue;
        HostRect rect = menu_item_rect(i);
        if (host_point_in_rect(&point, &rect)) return i;
    }
    return -1;
}

static void menu_scroll(int direction) {
    int count = menu_item_count();
    if (!count) return;
    int max_offset = count > 12 ? count - 12 : 0;
    rom_scroll_offset += direction * 3;
    if (rom_scroll_offset < 0) rom_scroll_offset = 0;
    if (rom_scroll_offset > max_offset) rom_scroll_offset = max_offset;
    if (menu_selection < rom_scroll_offset) menu_selection = rom_scroll_offset;
    if (menu_selection >= rom_scroll_offset + 12) menu_selection = rom_scroll_offset + 11;
}

// Return a command for immediate dispatch through the keyboard action handler.
// Host events use the same letterboxed logical coordinates as rendering.
static HostKey menu_mouse_command(const HostEvent *event, HostCanvas *renderer) {
    if (!focused) return HOST_KEY_UNKNOWN;
    if (event->type == HOST_MOUSEBUTTONDOWN && event->button.button == HOST_BUTTON_RIGHT)
        return HOST_KEY_ESCAPE;
    if (current_state == GUI_STATE_GAMEPLAY) return HOST_KEY_UNKNOWN;

    if (event->type == HOST_MOUSEWHEEL) {
        if (rebinding || !event->wheel.y) return HOST_KEY_UNKNOWN;
        int direction = event->wheel.y > 0 ? -1 : 1;
        if (event->wheel.direction == HOST_MOUSEWHEEL_FLIPPED) direction = -direction;
        int mx, my;
        float x, y;
        host_mouse_position(&mx, &my);
        host_to_logical(renderer, mx, my, &x, &y);
        if (x < 0 || x >= 256 || y < 0 || y >= 240) return HOST_KEY_UNKNOWN;
        if (current_state == GUI_STATE_MENU_SETTINGS && menu_hit_test((int)x, (int)y) == 2) {
            menu_selection = 2;
            return direction < 0 ? HOST_KEY_RIGHT : HOST_KEY_LEFT;
        }
        if (menu_is_file_list()) menu_scroll(direction);
        else return direction < 0 ? HOST_KEY_UP : HOST_KEY_DOWN;
        return HOST_KEY_UNKNOWN;
    }

    bool click = event->type == HOST_MOUSEBUTTONDOWN && event->button.button == HOST_BUTTON_LEFT;
    if (!click && event->type != HOST_MOUSEMOTION) return HOST_KEY_UNKNOWN;
    int x = click ? event->button.x : event->motion.x;
    int y = click ? event->button.y : event->motion.y;
    if (click && current_state != GUI_STATE_MENU_MAIN && x >= 8 && x < 72 && y >= 226 && y < 239)
        return HOST_KEY_ESCAPE;
    if (rebinding) return HOST_KEY_UNKNOWN;
    if (click && menu_is_file_list() && x >= 232 && x < 248) {
        if (y >= 68 && y < 79) menu_scroll(-1);
        if (y >= 200 && y < 211) menu_scroll(1);
        return HOST_KEY_UNKNOWN;
    }
    int item = menu_hit_test(x, y);
    if (item < 0) return HOST_KEY_UNKNOWN;
    menu_selection = item;
    if (!click) return HOST_KEY_UNKNOWN;
    if (current_state == GUI_STATE_MENU_SETTINGS && item == 2 && x >= 204)
        return x < 224 ? HOST_KEY_LEFT : HOST_KEY_RIGHT;
    return HOST_KEY_RETURN;
}

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

static void draw_character(HostCanvas *renderer, char c, int x, int y, uint32_t color) {
    if (c < 32 || c > 126) return;
    int idx = c - 32;
    uint8_t r = (color >> 16) & 0xFF;
    uint8_t g = (color >> 8) & 0xFF;
    uint8_t b = color & 0xFF;

    host_color(renderer, r, g, b, 255);
    HostPoint points[64];
    int count = 0;
    for (int row = 0; row < 8; row++) {
        uint8_t row_byte = font8x8[idx][row];
        for (int col = 0; col < 8; col++) {
            if (row_byte & (0x80 >> col)) {
                points[count++] = (HostPoint){x + col, y + row};
            }
        }
    }
    if (count) host_draw_points(renderer, points, count);
}

void draw_string(HostCanvas *renderer, const char *str, int x, int y, uint32_t color) {
    int cur_x = x;
    while (*str) {
        draw_character(renderer, *str, cur_x, y, color);
        cur_x += 8;
        str++;
    }
}

static void draw_notification(HostCanvas *renderer) {
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
}

static void draw_performance(HostCanvas *renderer) {
    DiagnosticSummary s = diagnostics_summary(&diagnostics);
    HostRect box = {2, 2, 252, 48};
    host_color(renderer, 0, 0, 0, 255);
    host_fill_rect(renderer, &box);
    char line[64];
    snprintf(line, sizeof(line), "FPS %.1f SPEED %.1f%%", s.fps, s.speed);
    draw_string(renderer, line, 6, 5, 0xFFFFFF);
    snprintf(line, sizeof(line), "MAX %.1fMS SPIKES %u", s.max_ms, s.spikes);
    draw_string(renderer, line, 6, 16, 0xFFFFFF);
    snprintf(line, sizeof(line), "AUDIO %.0fMS EMPTY %u %s", s.queue_ms, audio_monitor.underruns,
             !audio_device ? "N/A" : (audio_muted ? "MUTED" : ""));
    draw_string(renderer, line, 6, 27, 0xFFFFFF);
    snprintf(line, sizeof(line), "READS %u CHANGES %u TRACE %s", s.polls, s.changed, diagnostics.tracing ? "ON" : "OFF");
    draw_string(renderer, line, 6, 38, 0xFFFF00);
}

static void capture_diagnostics(void) {
    if (!nes_sys.cart) return;
    char path[1200];
    snprintf(path, sizeof(path), "%s/diagnostics.log", save_state_dir);
    bool ok = diagnostics_write(&nes_sys, path, loaded_rom_name, audio_monitor.underruns,
                                audio_monitor.trims, audio_monitor.errors, audio_device_ms);
    show_notification(ok ? "DIAGNOSTICS CAPTURED" : "CAPTURE FAILED");
    if (ok) fprintf(stdout, "Diagnostics: %s\n", path);
}

static void push_synthetic_key(HostKey sym, uint32_t type) {
    HostEvent new_event;
    host_zero(new_event);
    new_event.type = type;
    new_event.key.state = (type == HOST_KEYDOWN) ? HOST_PRESSED : HOST_RELEASED;
    new_event.key.keysym.sym = sym;
    host_push_event(&new_event);
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

    char *base_path = host_base_path();
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
        free(base_path);
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
    double current_time = (double)host_ticks() / 1000.0;
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
    uint32_t current_tick = host_ticks();
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
    printf("\033[1mAudio Queue:\033[0m  %u bytes\033[K\n", host_audio_queued_bytes());
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
    char filepath[1024];
    snprintf(filepath, sizeof(filepath), "%s/%s", dir, filename);
    NES_StateResult result = nes_state_save(&nes_sys, filepath);
    show_notification(result == NES_STATE_OK ? "STATE SAVED" : nes_state_message(result));
    if (result != NES_STATE_OK) {
        notification_timer = 180;
        fprintf(stderr, "%s: %s\n", filepath, nes_state_message(result));
    }
}

static void load_emulator_state(const char *dir, const char *filename) {
    char filepath[1024];
    snprintf(filepath, sizeof(filepath), "%s/%s", dir, filename);
    NES_StateResult result = nes_state_load(&nes_sys, filepath);
    show_notification(result == NES_STATE_OK ? "STATE LOADED" : nes_state_message(result));
    if (result == NES_STATE_OK) {
        nes_sys.zapper_enabled = zapper_enabled;
        clear_host_input();
        runtime_reset_pending = true;
        debugger_view_pc = nes_sys.cpu.program_counter;
        debugger_selected_line = 0;
        clear_view_history(debugger_view_pc);
    } else {
        notification_timer = 180;
        fprintf(stderr, "%s: %s\n", filepath, nes_state_message(result));
    }
}

static HostCanvas canvas;
static HostCanvas *renderer = &canvas;
static bool running = true, cursor_visible = true, was_playing;
static uint32_t last_mouse_activity;
static const uint32_t cursor_idle_ms = 2000;

static void app_init(void) {
    host_setup();
    audio_device = host_audio_valid();
    audio_device_ms = host_audio_latency_ms();
    apply_display();
    nes_init(&nes_sys);
    nes_sys.diagnostics = &diagnostics;
    nes_sys.zapper_enabled = zapper_enabled;

    cpu_bus_bridge.bus_context = &nes_sys;
    cpu_bus_bridge.read = cpu_bridge_read;
    cpu_bus_bridge.write = cpu_bridge_write;
    cpu_bus_bridge.cycle_tick = NULL;

    last_mouse_activity = host_ticks();
    scan_rom_directory();
}

static void app_frame(void) {
    HostEvent event;
    host_poll_gamepads();
    bool playing = current_state == GUI_STATE_GAMEPLAY && nes_sys.cart && !debugger_active && focused;
    if (runtime_reset_pending || playing != was_playing) {
        clear_host_input();
        reset_runtime();
    }
    was_playing = playing;
    if (current_state == GUI_STATE_GAMEPLAY && nes_sys.cart != NULL) {
        if (debugger_active) {
            debugger_render(renderer, &nes_sys.cpu);
            draw_notification(renderer);
            host_present(renderer);
            host_delay(16);
            update_console_debug(&nes_sys.cpu);
        } else {
            uint64_t frame_start_tick = host_counter();
            uint64_t frame_start_cycles = nes_sys.cpu.cycle_count;

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

            if (nes_sys.frame_ready) nes_check_zapper_stall(&nes_sys);

            uint64_t emu_end_tick = host_counter();
            debug_emu_ticks += (emu_end_tick - frame_start_tick);

            // Feed the device before texture upload/presentation can block.
            if (nes_sys.frame_ready) queue_game_audio();

            host_color(renderer, 0, 0, 0, 255);
            host_clear(renderer);

            GameCrop crop = game_crop(nes_sys.cart->mapper_id);
            HostRect src_rect = {crop.x, crop.y, crop.w, crop.h};
            host_draw_frame(renderer, nes_sys.ppu.screen_buffer, &src_rect);



            if (nes_sys.zapper_watchdog.stalled) {
                HostRect warning_box = { 8, 156, 240, 60 };
                host_color(renderer, 0, 0, 0, 255);
                host_fill_rect(renderer, &warning_box);
                draw_string(renderer, "Possible light-gun hang", 16, 164, 0xFFFF00);
                draw_string(renderer, "ESC > Settings > Port 2", 16, 176, 0xFFFFFF);
                draw_string(renderer, "Select Controller", 16, 188, 0xFFFFFF);
                draw_string(renderer, "then resume the game.", 16, 200, 0xFFFFFF);
            }

            if (performance_visible) draw_performance(renderer);
            draw_notification(renderer);
            host_present(renderer);

            if (nes_sys.frame_ready) {
                uint64_t cycles = nes_sys.cpu.cycle_count - frame_start_cycles;
                double wait = frame_scheduler_advance(&frame_scheduler, host_time(), cycles);
                while (wait > 0) {
                    host_delay(wait >= 0.001 ? (uint32_t)(wait * 1000) : 0);
                    wait = frame_scheduler.deadline - host_time();
                }
                double now = host_time();
                diagnostics_frame(&nes_sys, cycles, now - diagnostic_last_time,
                    (double)(emu_end_tick - frame_start_tick) * 1000 / host_frequency(),
                    audio_monitor.queue_samples * 1000.0 / AUDIO_RATE);
                diagnostic_last_time = now;
            } else runtime_reset_pending = true;

            uint64_t frame_end_tick = host_counter();
            debug_total_ticks += (frame_end_tick - frame_start_tick);

            update_console_debug(&nes_sys.cpu);
        }
    } else {
        host_color(renderer, 20, 20, 30, 255);
        host_clear(renderer);

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
                    host_color(renderer, 0, 255, 0, 255);
                    HostRect box = menu_item_rect(i);
                    host_draw_rect(renderer, &box);
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
                host_color(renderer, 0, 255, 0, 255);
                HostRect box = menu_item_rect(0);
                host_draw_rect(renderer, &box);
            }

            draw_string(renderer, "-----------------------------", 16, 52, 0x00FFFF);
            for (int i = 0; i < CONTROL_COUNT; i++) {
                char buf[64];
                if (control_mode_keyboard) {
                    const char *key_name = host_key_name(control_mappings[i]);
                    if (rebinding && (i + 1) == menu_selection) {
                        snprintf(buf, sizeof(buf), "%-11s -> [PRESS KEY...]", button_names[i]);
                    } else {
                        snprintf(buf, sizeof(buf), "%-11s -> %s", button_names[i], key_name);
                    }
                } else {
                    const char *btn_name = "Unknown";
                    switch (controller_button_mappings[i]) {
                        case HOST_CONTROLLER_BUTTON_A: btn_name = "Button A"; break;
                        case HOST_CONTROLLER_BUTTON_B: btn_name = "Button B"; break;
                        case HOST_CONTROLLER_BUTTON_X: btn_name = "Button X"; break;
                        case HOST_CONTROLLER_BUTTON_Y: btn_name = "Button Y"; break;
                        case HOST_CONTROLLER_BUTTON_BACK: btn_name = "Back"; break;
                        case HOST_CONTROLLER_BUTTON_GUIDE: btn_name = "Guide"; break;
                        case HOST_CONTROLLER_BUTTON_START: btn_name = "Start"; break;
                        case HOST_CONTROLLER_BUTTON_LEFTSTICK: btn_name = "Left Stick"; break;
                        case HOST_CONTROLLER_BUTTON_RIGHTSTICK: btn_name = "Right Stick"; break;
                        case HOST_CONTROLLER_BUTTON_LEFTSHOULDER: btn_name = "Left Shoulder"; break;
                        case HOST_CONTROLLER_BUTTON_RIGHTSHOULDER: btn_name = "Right Shoulder"; break;
                        case HOST_CONTROLLER_BUTTON_DPAD_UP: btn_name = "D-Pad Up"; break;
                        case HOST_CONTROLLER_BUTTON_DPAD_DOWN: btn_name = "D-Pad Down"; break;
                        case HOST_CONTROLLER_BUTTON_DPAD_LEFT: btn_name = "D-Pad Left"; break;
                        case HOST_CONTROLLER_BUTTON_DPAD_RIGHT: btn_name = "D-Pad Right"; break;
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
                    host_color(renderer, 0, 255, 0, 255);
                    HostRect box = menu_item_rect(i + 1);
                    host_draw_rect(renderer, &box);
                }
            }
            uint32_t def_col = (menu_selection == (CONTROL_COUNT + 1)) ? 0xFFFFFF : 0x888888;
            draw_string(renderer, "Restore Defaults", 24, 62 + CONTROL_COUNT * 13, def_col);
            if (menu_selection == (CONTROL_COUNT + 1)) {
                host_color(renderer, 0, 255, 0, 255);
                HostRect box = menu_item_rect(CONTROL_COUNT + 1);
                host_draw_rect(renderer, &box);
            }
            draw_string(renderer, "Click/ENTER: Adjust", 16, 210, 0x888888);
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
                        host_color(renderer, 0, 255, 0, 255);
                        HostRect box = menu_item_rect(i);
                        host_draw_rect(renderer, &box);
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
                    host_color(renderer, 0, 255, 0, 255);
                    HostRect box = menu_item_rect(i);
                    host_draw_rect(renderer, &box);
                }
            }
            if (rom_scroll_offset > 0) draw_string(renderer, "^", 236, 68, 0x00FF00);
            if (end_idx < total_options) draw_string(renderer, "v", 236, 68 + 11 * 12, 0x00FF00);
            draw_string(renderer, "Click/ENTER: Save", 24, 214, 0x00FFFF);
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
                        host_color(renderer, 0, 255, 0, 255);
                        HostRect box = menu_item_rect(i);
                        host_draw_rect(renderer, &box);
                    }
                }
                if (rom_scroll_offset > 0) draw_string(renderer, "^", 236, 68, 0x00FF00);
                if (end_idx < state_file_count) draw_string(renderer, "v", 236, 68 + 11 * 12, 0x00FF00);
            }
            draw_string(renderer, "Click/ENTER: Load", 24, 214, 0x00FFFF);
        } else if (current_state == GUI_STATE_MENU_SETTINGS) {
            char rows[10][64];
            snprintf(rows[0], sizeof(rows[0]), "Window: %s", window_scale == 5 ? "Maximized" : "");
            if (window_scale != 5) snprintf(rows[0], sizeof(rows[0]), "Window: %dx", window_scale);
            snprintf(rows[1], sizeof(rows[1]), "Muted: %s", audio_muted ? "ON" : "OFF");
            snprintf(rows[2], sizeof(rows[2]), "Volume: %d%%", master_volume);
            snprintf(rows[3], sizeof(rows[3]), "Fullscreen: %s", fullscreen ? "ON" : "OFF");
            snprintf(rows[4], sizeof(rows[4]), "Console debug: %s", console_debug_enabled ? "ON" : "OFF");
            snprintf(rows[5], sizeof(rows[5]), "Port 2: %s", zapper_enabled ? "Zapper" : "Controller");
            snprintf(rows[6], sizeof(rows[6]), "Use global display/Port 2");
            snprintf(rows[7], sizeof(rows[7]), "Make display/Port 2 global");
            snprintf(rows[8], sizeof(rows[8]), "Performance (F2): %s", performance_visible ? "ON" : "OFF");
            snprintf(rows[9], sizeof(rows[9]), "Event trace: %s", diagnostics.tracing ? "ON" : "OFF");
            draw_string(renderer, nes_sys.cart ? (rom_override ? "THIS ROM: CUSTOM" : "THIS ROM: GLOBAL DEFAULTS") :
                        "GLOBAL DEFAULTS", 24, 44, 0x00FFFF);
            for (int i = 0; i < 10; ++i) {
                draw_string(renderer, rows[i], 24, 60 + i * 13,
                            menu_selection == i ? 0xFFFFFF : 0x888888);
            }
            host_color(renderer, 0, 255, 0, 255);
            HostRect box = menu_item_rect(menu_selection);
            host_draw_rect(renderer, &box);
            draw_string(renderer, "<", 208, 86, 0x00FFFF);
            draw_string(renderer, ">", 232, 86, 0x00FFFF);
            draw_string(renderer, "Click/ENTER: Change", 24, 201, 0xFFFF00);
            draw_string(renderer, "Volume: </> or wheel", 24, 215, 0xFFFF00);
        }

        if (current_state != GUI_STATE_MENU_MAIN)
            draw_string(renderer, "< Back", 8, 229, 0x00FFFF);
        else
            draw_string(renderer, "Click: Select  RMB: Resume", 24, 229, 0x888888);
        draw_notification(renderer);
        host_present(renderer);
        host_delay(16);
        update_console_debug(&nes_sys.cpu);
    }

    while (running && host_poll_event(&event)) {
        if (event.type == HOST_MOUSEMOTION ||
            event.type == HOST_MOUSEBUTTONDOWN || event.type == HOST_MOUSEBUTTONUP ||
            event.type == HOST_MOUSEWHEEL) {
            last_mouse_activity = host_ticks();
            HostKey command = menu_mouse_command(&event, renderer);
            if (command != HOST_KEY_UNKNOWN) {
                host_zero(event);
                event.type = HOST_KEYDOWN;
                event.key.keysym.sym = command;
            }
        }
        if (event.type == HOST_QUIT) {
            if (save_battery_ram()) running = false;
        } else if (event.type == HOST_WINDOWEVENT) {
            if (event.window.event == HOST_WINDOWEVENT_FOCUS_LOST) {
                focused = false;
                clear_host_input();
                if (current_state == GUI_STATE_GAMEPLAY) {
                    current_state = GUI_STATE_MENU_MAIN;
                    menu_selection = 0;
                }
                runtime_reset_pending = true;
            } else if (event.window.event == HOST_WINDOWEVENT_FOCUS_GAINED) {
                focused = true;
                clear_host_input();
            } else if (event.window.event == HOST_WINDOWEVENT_LEAVE) {
                nes_sys.zapper_x = nes_sys.zapper_y = -1;
                nes_sys.zapper_light = false;
            }
        } else if (event.type == HOST_MOUSEBUTTONDOWN) {
            if (focused && current_state == GUI_STATE_GAMEPLAY && !debugger_active &&
                nes_sys.zapper_enabled && event.button.button == HOST_BUTTON_LEFT)
                nes_sys.zapper_trigger = true;
        } else if (event.type == HOST_MOUSEBUTTONUP) {
            if (event.button.button == HOST_BUTTON_LEFT) nes_sys.zapper_trigger = false;
        } else if (event.type == HOST_CONTROLLERDEVICEADDED) {
            if (game_controller < 0) game_controller = event.cdevice.which;
        } else if (event.type == HOST_CONTROLLERDEVICEREMOVED) {
            if (game_controller == event.cdevice.which) {
                clear_host_input();
                game_controller = -1;
            }
        } else if (event.type == HOST_CONTROLLERBUTTONDOWN) {
            if (!focused || game_controller < 0 || event.cbutton.which !=
                game_controller) continue;
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
                } else if (event.cbutton.button == controller_button_mappings[9]) {
                    char filename[128];
                    get_rolling_quicksave_filename(filename, sizeof(filename), false);
                    load_emulator_state(save_state_dir, filename);
                } else {
                    for (int i = 0; i < 8; i++) {
                        if (event.cbutton.button == controller_button_mappings[i]) {
                            host_input_button(&host_input.buttons, (unsigned)i, true);
                        }
                    }
                }
            } else {
                if (event.cbutton.button == HOST_CONTROLLER_BUTTON_DPAD_UP) {
                    push_synthetic_key(HOST_KEY_UP, HOST_KEYDOWN);
                } else if (event.cbutton.button == HOST_CONTROLLER_BUTTON_DPAD_DOWN) {
                    push_synthetic_key(HOST_KEY_DOWN, HOST_KEYDOWN);
                } else if (event.cbutton.button == HOST_CONTROLLER_BUTTON_DPAD_LEFT) {
                    push_synthetic_key(HOST_KEY_LEFT, HOST_KEYDOWN);
                } else if (event.cbutton.button == HOST_CONTROLLER_BUTTON_DPAD_RIGHT) {
                    push_synthetic_key(HOST_KEY_RIGHT, HOST_KEYDOWN);
                } else if (event.cbutton.button == HOST_CONTROLLER_BUTTON_A || event.cbutton.button == HOST_CONTROLLER_BUTTON_START) {
                    push_synthetic_key(HOST_KEY_RETURN, HOST_KEYDOWN);
                } else if (event.cbutton.button == HOST_CONTROLLER_BUTTON_B || event.cbutton.button == HOST_CONTROLLER_BUTTON_BACK) {
                    push_synthetic_key(HOST_KEY_ESCAPE, HOST_KEYDOWN);
                }
            }
        } else if (event.type == HOST_CONTROLLERBUTTONUP) {
            if (game_controller < 0 || event.cbutton.which !=
                game_controller) continue;
            for (int i = 0; i < 8; ++i)
                if (event.cbutton.button == controller_button_mappings[i])
                    host_input_button(&host_input.buttons, (unsigned)i, false);
        } else if (event.type == HOST_CONTROLLERAXISMOTION) {
            if (!focused || game_controller < 0 || event.caxis.which !=
                game_controller) continue;
            if (current_state == GUI_STATE_GAMEPLAY && !debugger_active) {
                if (event.caxis.axis == HOST_CONTROLLER_AXIS_LEFTX)
                    host_input_axis(&host_input, false, event.caxis.value);
                if (event.caxis.axis == HOST_CONTROLLER_AXIS_LEFTY)
                    host_input_axis(&host_input, true, event.caxis.value);
            }
        } else if (event.type == HOST_KEYDOWN) {
            if (!focused || event.key.repeat) continue;
            if (!rebinding && event.key.keysym.sym == HOST_KEY_F2) {
                performance_visible = !performance_visible;
                save_emulator_settings();
                continue;
            }
            if (!rebinding && event.key.keysym.sym == HOST_KEY_F4) {
                if (event.key.keysym.mod & HOST_MOD_CTRL) {
                    diagnostics.tracing = !diagnostics.tracing;
                    if (diagnostics.tracing) {
                        diagnostics.event_count = diagnostics.event_head = 0;
                        diagnostics.overwritten = 0;
                        diagnostics_event(&nes_sys, DIAG_RESUME, 0, 0);
                    }
                    show_notification(diagnostics.tracing ? "EVENT TRACE ON" : "EVENT TRACE OFF");
                } else capture_diagnostics();
                continue;
            }
            if (rebinding) {
                if (control_mode_keyboard && event.key.keysym.sym != HOST_KEY_ESCAPE) {
                    control_mappings[menu_selection - 1] = event.key.keysym.sym;
                    save_emulator_settings();
                }
                rebinding = false;
                break;
            }

            if (event.key.keysym.sym == HOST_KEY_ESCAPE || event.key.keysym.sym == HOST_KEY_F1) {
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
                    case HOST_KEY_UP:
                        do {
                            menu_selection--;
                            if (menu_selection < 0) {
                                if (current_state == GUI_STATE_MENU_MAIN) menu_selection = 6;
                                else if (current_state == GUI_STATE_MENU_LOAD_ROM) menu_selection = rom_file_count - 1;
                                else if (current_state == GUI_STATE_MENU_SAVE_STATE) menu_selection = state_file_count;
                                else if (current_state == GUI_STATE_MENU_LOAD_STATE) menu_selection = state_file_count - 1;
                                else if (current_state == GUI_STATE_MENU_SETTINGS) menu_selection = 9;
                                else if (current_state == GUI_STATE_MENU_CONTROLS) menu_selection = (CONTROL_COUNT + 1);
                            }
                        } while (current_state == GUI_STATE_MENU_MAIN && nes_sys.cart == NULL && (menu_selection == 0 || menu_selection == 2 || menu_selection == 3));
                        break;
                    case HOST_KEY_DOWN:
                        do {
                            menu_selection++;
                            if (current_state == GUI_STATE_MENU_MAIN && menu_selection > 6) menu_selection = 0;
                            else if (current_state == GUI_STATE_MENU_LOAD_ROM && menu_selection >= rom_file_count) menu_selection = 0;
                            else if (current_state == GUI_STATE_MENU_SAVE_STATE && menu_selection > state_file_count) menu_selection = 0;
                            else if (current_state == GUI_STATE_MENU_LOAD_STATE && menu_selection >= state_file_count) menu_selection = 0;
                            else if (current_state == GUI_STATE_MENU_SETTINGS && menu_selection > 9) menu_selection = 0;
                            else if (current_state == GUI_STATE_MENU_CONTROLS && menu_selection > (CONTROL_COUNT + 1)) menu_selection = 0;
                        } while (current_state == GUI_STATE_MENU_MAIN && nes_sys.cart == NULL && (menu_selection == 0 || menu_selection == 2 || menu_selection == 3));
                        break;
                    case HOST_KEY_LEFT:
                        if (current_state == GUI_STATE_MENU_SETTINGS && menu_selection == 2) {
                            master_volume -= 10;
                            if (master_volume < 0) master_volume = 0;
                            play_volume_ding();
                            save_emulator_settings();
                        } else if (current_state == GUI_STATE_MENU_CONTROLS && menu_selection == 0) {
                            control_mode_keyboard = !control_mode_keyboard;
                        }
                        break;
                    case HOST_KEY_RIGHT:
                        if (current_state == GUI_STATE_MENU_SETTINGS && menu_selection == 2) {
                            master_volume += 10;
                            if (master_volume > 100) master_volume = 100;
                            play_volume_ding();
                            save_emulator_settings();
                        } else if (current_state == GUI_STATE_MENU_CONTROLS && menu_selection == 0) {
                            control_mode_keyboard = !control_mode_keyboard;
                        }
                        break;
                    case HOST_KEY_BACKSPACE:
                        if (current_state != GUI_STATE_MENU_MAIN) {
                            current_state = GUI_STATE_MENU_MAIN;
                            menu_selection = nes_sys.cart ? 0 : 1;
                        }
                        break;
                    case HOST_KEY_RETURN:
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
                                if (save_battery_ram()) running = false;
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
                                    if (!save_battery_ram()) continue;
                                    cartridge_free(nes_sys.cart);
                                    nes_sys.cart = NULL;
                                }

                                nes_init(&nes_sys);
                                bool trace_enabled = diagnostics.tracing;
                                memset(&diagnostics, 0, sizeof(diagnostics));
                                diagnostics.tracing = trace_enabled;
                                memset(&audio_monitor, 0, sizeof(audio_monitor));
                                nes_sys.diagnostics = &diagnostics;
                                apply_rom_preferences();

                                char rom_error[128];
                                Cartridge *cart = cartridge_load_ex(&nes_sys, rom_files[menu_selection], rom_error, sizeof(rom_error));
                                if (!cart && strcmp(rom_error, "Cannot open ROM") == 0) {
                                    char *base_path = host_base_path();
                                    if (base_path) {
                                        char full_path[1024];
                                        snprintf(full_path, sizeof(full_path), "%s%s", base_path, rom_files[menu_selection]);
                                        cart = cartridge_load_ex(&nes_sys, full_path, rom_error, sizeof(rom_error));
                                        free(base_path);
                                    }
                                }

                                if (!cart) {
                                    show_notification(rom_error);
                                    notification_timer = 180;
                                    fprintf(stderr, "%s: %s\n", rom_files[menu_selection], rom_error);
                                }

                                if (cart) {
                                    nes_sys.cart = cart;
                                    apply_rom_preferences();
                                    strncpy(loaded_rom_name, rom_files[menu_selection], sizeof(loaded_rom_name) - 1);
                                    loaded_rom_name[sizeof(loaded_rom_name) - 1] = '\0';

                                    char rom_name_clean[256];
                                    strncpy(rom_name_clean, rom_files[menu_selection], sizeof(rom_name_clean) - 1);
                                    rom_name_clean[sizeof(rom_name_clean) - 1] = '\0';
                                    char *ext = strrchr(rom_name_clean, '.');
                                    if (ext) *ext = '\0';

                                    char *base_path = host_base_path();
                                    if (base_path) {
                                        snprintf(save_state_dir, sizeof(save_state_dir), "%s%s/%s", base_path, "saves", rom_name_clean);
                                        free(base_path);
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
                                    nes_clock_tick(&nes_sys);
                                    debugger_init();
                                    if (!load_battery_ram()) {
                                        cartridge_free(nes_sys.cart);
                                        nes_sys.cart = NULL;
                                        apply_rom_preferences();
                                        continue;
                                    }
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
                                preferences_inherit = false;
                                window_scale++;
                                if (window_scale > 5) window_scale = 1;
                                apply_display();
                            } else if (menu_selection == 1) {
                                audio_muted = !audio_muted;
                                runtime_reset_pending = true;
                            } else if (menu_selection == 2) {
                                play_volume_ding();
                            } else if (menu_selection == 3) {
                                preferences_inherit = false;
                                fullscreen = !fullscreen;
                                apply_display();
                            } else if (menu_selection == 4) {
                                console_debug_enabled = !console_debug_enabled;
                                if (!console_debug_enabled) {
                                    printf("\033[H\033[2J");
                                    fflush(stdout);
                                }
                            } else if (menu_selection == 5) {
                                preferences_inherit = false;
                                zapper_enabled = !zapper_enabled;
                                nes_sys.zapper_enabled = zapper_enabled;
                                nes_sys.zapper_trigger = false;
                                nes_sys.zapper_light = false;
                                nes_reset_zapper_watchdog(&nes_sys);
                            } else if (menu_selection == 6) {
                                preferences_inherit = true;
                                save_emulator_settings();
                                apply_rom_preferences();
                            } else if (menu_selection == 7) {
                                global_preferences = (RomPreferences){(unsigned)window_scale, fullscreen, zapper_enabled};
                                show_notification("GLOBAL DEFAULTS UPDATED");
                            } else if (menu_selection == 8) {
                                performance_visible = !performance_visible;
                            } else if (menu_selection == 9) {
                                diagnostics.tracing = !diagnostics.tracing;
                            }
                            save_emulator_settings();
                        }
                        break;
                    default: break;
                }
            } else {
                HostKey sym = event.key.keysym.sym;
                if (sym == control_mappings[8]) {
                    char filename[128];
                    get_rolling_quicksave_filename(filename, sizeof(filename), true);
                    save_emulator_state(save_state_dir, filename);
                } else if (sym == control_mappings[9]) {
                    char filename[128];
                    get_rolling_quicksave_filename(filename, sizeof(filename), false);
                    load_emulator_state(save_state_dir, filename);
                } else {
                    switch (sym) {
                        case HOST_KEY_f:
                        case HOST_KEY_F11: {
                        preferences_inherit = false;
                        fullscreen = !fullscreen;
                        apply_display();
                        save_emulator_settings();
                        break;
                    }
                    case HOST_KEY_F3: {
                        console_debug_enabled = !console_debug_enabled;
                        if (!console_debug_enabled) {
                            printf("\033[H\033[2J");
                            fflush(stdout);
                        }
                        save_emulator_settings();
                        break;
                    }
                    case HOST_KEY_UP: {
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
                                    host_input_button(&host_input.keyboard, (unsigned)i, true);
                                }
                            }
                        }
                        break;
                    }
                    case HOST_KEY_DOWN: {
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
                                    host_input_button(&host_input.keyboard, (unsigned)i, true);
                                }
                            }
                        }
                        break;
                    }
                    case HOST_KEY_F7: {
                        if (debugger_active) {
                            uint16_t target_pc = debugger_line_pcs[debugger_selected_line];
                            breakpoints[target_pc] = !breakpoints[target_pc];
                        }
                        break;
                    }
                    case HOST_KEY_F6: {
                        if (debugger_active) {
                            debugger_logging_active = !debugger_logging_active;
                        }
                        break;
                    }
                    case HOST_KEY_F10: {
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
                    case HOST_KEY_F9: {
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
                    case HOST_KEY_F12: {
                        if (debugger_active) {
                            nes_reset(&nes_sys);
                            nes_clock_tick(&nes_sys);
                            runtime_reset_pending = true;
                        }
                        break;
                    }
                    default: {
                        if (!debugger_active) {
                            for (int i = 0; i < 8; i++) {
                                if (event.key.keysym.sym == control_mappings[i]) {
                                    host_input_button(&host_input.keyboard, (unsigned)i, true);
                                }
                            }
                        }
                        break;
                    }
                }
            }
    }
        } else if (event.type == HOST_KEYUP) {
            for (int i = 0; i < 8; i++) {
                if (event.key.keysym.sym == control_mappings[i]) {
                    host_input_button(&host_input.keyboard, (unsigned)i, false);
                }
            }
        }
    }

    if (current_state != GUI_STATE_GAMEPLAY || debugger_active || !focused)
        clear_host_input();
    nes_sys.controller_state[0] = host_input_value(&host_input);
    if (focused && current_state == GUI_STATE_GAMEPLAY && !debugger_active && nes_sys.zapper_enabled) {
        int mx, my;
        float lx, ly;
        host_mouse_position(&mx, &my);
        host_to_logical(renderer, mx, my, &lx, &ly);
        if (host_window_flags() & HOST_WINDOW_MOUSE_FOCUS) {
            game_aim(game_crop(nes_sys.cart->mapper_id), lx, ly, &nes_sys.zapper_x, &nes_sys.zapper_y);
        } else nes_sys.zapper_x = nes_sys.zapper_y = -1;
    }

    uint32_t cursor_now = host_ticks();
    uint32_t window_flags = host_window_flags();
    bool can_hide_cursor = current_state == GUI_STATE_GAMEPLAY &&
        nes_sys.cart != NULL && !debugger_active && !nes_sys.zapper_enabled &&
        (window_flags & HOST_WINDOW_INPUT_FOCUS) &&
        (window_flags & HOST_WINDOW_MOUSE_FOCUS);
    if (!can_hide_cursor) {
        last_mouse_activity = cursor_now;
    }
    bool show_cursor = !can_hide_cursor ||
        (uint32_t)(cursor_now - last_mouse_activity) < cursor_idle_ms;
    if (show_cursor != cursor_visible) {
        host_show_cursor(show_cursor ? HOST_ENABLE : HOST_DISABLE);
        cursor_visible = show_cursor;
    }
    if (!running) sapp_quit();
}

static void app_cleanup(void) {
    save_emulator_settings();
    debugger_shutdown();
    host_shutdown();
    if (console_debug_enabled) { printf("\033[H\033[2J"); fflush(stdout); }
    if (nes_sys.cart) { cartridge_free(nes_sys.cart); nes_sys.cart = NULL; }
}

sapp_desc sokol_main(int argc, char *argv[]) {
    (void)argc; (void)argv;
    load_emulator_settings();
    return (sapp_desc){
        .init_cb = app_init, .frame_cb = app_frame,
        .cleanup_cb = app_cleanup, .event_cb = host_event,
        .width = 256 * (window_scale == 5 ? 3 : window_scale),
        .height = 240 * (window_scale == 5 ? 3 : window_scale),
        .window_title = "NES Emulator - Sokol", .fullscreen = fullscreen,
        .disable_vsync = true, .high_dpi = true,
    };
}
