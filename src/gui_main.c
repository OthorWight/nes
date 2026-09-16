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
#include <stdarg.h>
#include "host.h"
#include "menu_bar.h"
#include "file_browser.h"
#include "nes_system.h"
#include "debugger.h"
#include "save_state.h"
#include "state_io.h"
#include "diagnostics.h"
#include "frontend_runtime.h"
#include "nametable_view.h"
#include "apu_view.h"
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

static bool debug_panel_enabled = false;
static int window_scale = 5;
static bool fullscreen = false;
static bool crt_enabled = false;
static bool nametable_viewer_enabled = false;
static NametableView nametable_view;
static bool apu_viewer_enabled;
static ApuView apu_view;
static int region_setting = NES_REGION_AUTO;
static int master_volume = 100;
static bool performance_visible = false;
static bool focused = true;
static MenuBar desktop_menu;
static bool paused;
static int help_page;
static char recent_roms[4][BROWSER_PATH];
static char loaded_rom_path[BROWSER_PATH];
static FileBrowser file_browser;
static int browser_mode; /* 0 ROM, 1 load state, 2 save state */
static int control_selection;
static char recent_labels[4][29] = {"(Empty)", "(Empty)", "(Empty)", "(Empty)"};
static int recent_count;
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
    state_u32(&io, 9);
    state_i32(&io, master_volume);
    state_i32(&io, audio_muted ? 1 : 0);
    state_i32(&io, (int)global_preferences.scale);
    state_i32(&io, global_preferences.fullscreen ? 1 : 0);
    state_i32(&io, debug_panel_enabled ? 1 : 0);
    for (unsigned i = 0; i < CONTROL_COUNT; ++i) state_i32(&io, control_mappings[i]);
    for (unsigned i = 0; i < CONTROL_COUNT; ++i) state_i32(&io, controller_button_mappings[i]);
    state_i32(&io, global_preferences.zapper ? 1 : 0);
    state_i32(&io, performance_visible ? 1 : 0);
    state_i32(&io, crt_enabled ? 1 : 0);
    state_i32(&io, nametable_viewer_enabled ? 1 : 0);
    state_i32(&io, apu_viewer_enabled ? 1 : 0);
    state_i32(&io, region_setting);
    if (!io.ok || !state_atomic_write(filepath, data, io.pos)) show_notification("GLOBAL SETTINGS SAVE FAILED");
}

static void load_emulator_settings(void) {
    zapper_enabled = false;
    crt_enabled = false;
    nametable_viewer_enabled = false;
    apu_viewer_enabled = false;
    region_setting = NES_REGION_AUTO;
    char filepath[1024];
    get_settings_filepath(filepath, sizeof(filepath));

    FILE *f = fopen(filepath, "rb");
    if (!f) return;

    uint32_t version = 0;
    if (fread(&version, sizeof(version), 1, f) != 1 || version < 1 || version > 9) {
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
    debug_panel_enabled = (temp_debug != 0);
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
    if (version >= 6) {
        int temp_crt = 0;
        if (fread(&temp_crt, sizeof(temp_crt), 1, f) == 1) crt_enabled = temp_crt == 1;
    }
    if (version >= 7) {
        int temp_nametables = 0;
        if (fread(&temp_nametables, sizeof(temp_nametables), 1, f) == 1) nametable_viewer_enabled = temp_nametables == 1;
    }
    if (version >= 8) {
        int temp_apu = 0;
        if (fread(&temp_apu, sizeof(temp_apu), 1, f) == 1) apu_viewer_enabled = temp_apu == 1;
    }
    if (version >= 9) {
        int value;
        if (fread(&value, sizeof(value), 1, f) == 1 && value >= NES_NTSC && value <= NES_REGION_AUTO)
            region_setting = value;
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

static void format_text_field(char field[65], const char *text, unsigned cells, bool right) {
    if (cells > 64) cells = 64;
    if (!cells) { field[0] = '\0'; return; }
    size_t length = strlen(text);
    memset(field, ' ', cells);
    if (length > cells) {
        if (right) memset(field, '#', cells);
        else { memcpy(field, text, cells); field[cells - 1] = '>'; }
    } else memcpy(field + (right ? cells - length : 0), text, length);
    field[cells] = '\0';
}

void draw_text_field(HostCanvas *canvas, const char *text, int x, int y,
                     unsigned cells, bool right, uint32_t color) {
    char field[65];
    format_text_field(field, text, cells, right);
    draw_string(canvas, field, x, y, color);
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

static void capture_diagnostics(void) {
    if (!nes_sys.cart) return;
    char path[1200];
    snprintf(path, sizeof(path), "%s/diagnostics.log", save_state_dir);
    bool ok = diagnostics_write(&nes_sys, path, loaded_rom_name, audio_monitor.underruns,
                                audio_monitor.trims, audio_monitor.errors, audio_device_ms);
    show_notification(ok ? "DIAGNOSTICS CAPTURED" : "CAPTURE FAILED");
    if (ok) fprintf(stdout, "Diagnostics: %s\n", path);
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
        region_setting = nes_sys.region_override;
        nametable_view.valid = false;
        memset(&apu_view, 0, sizeof(apu_view));
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
static HostCanvas debug_canvas;
static HostCanvas nametable_canvas;
static HostCanvas apu_canvas;

static void panel_line(int *y, uint32_t color, const char *format, ...) {
    char text[128];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    draw_text_field(&debug_canvas, text, 8, *y, 62, false, color);
    *y += 12;
}

static void panel_metric(int y, unsigned column, unsigned cells, const char *label,
                         const char *format, ...) {
    char value[96];
    va_list args;
    va_start(args, format);
    vsnprintf(value, sizeof(value), format, args);
    va_end(args);
    unsigned label_cells = (unsigned)strlen(label) + 1;
    if (label_cells >= cells) return;
    draw_text_field(&debug_canvas, label, 8 + (int)column * 8, y, label_cells, false, 0xFFFFFF);
    draw_text_field(&debug_canvas, value, 8 + (int)(column + label_cells) * 8, y,
                    cells - label_cells, true, 0xFFFFFF);
}

static void draw_debug_panel(void) {
    bool visible = debug_panel_enabled || performance_visible || debugger_active;
    host_set_debug_panel(visible ? &debug_canvas : NULL);
    if (!visible) return;
    debug_canvas.width = HOST_PANEL_WIDTH;
    debug_canvas.height = HOST_PANEL_HEIGHT;
    bool playing = nes_sys.cart && !debugger_active && !paused && !desktop_menu.active && !help_page && !file_browser.active && focused;
    bool full = debug_panel_enabled || debugger_active;
    host_color(&debug_canvas, 15, 20, 35, 255);
    host_clear(&debug_canvas);
    int y = 12;
    if (full) {
        if (!debugger_active) {
            debugger_view_pc = nes_sys.cpu.program_counter;
            debugger_selected_line = 0;
        }
        debugger_render(&debug_canvas, &nes_sys.cpu);
        y = 258;
    }
    DiagnosticSummary s = diagnostics_summary(&diagnostics);
    panel_line(&y, 0x78C8FF, "PERFORMANCE / AUDIO / INPUT");
    panel_line(&y, 0xFFFFFF, "ROM: %.56s", nes_sys.cart ? loaded_rom_name : "[None]");
    draw_text_field(&debug_canvas, playing ? "RUNNING" : "PAUSED", 8, y, 7, false, 0xFFFFFF);
    draw_text_field(&debug_canvas, nes_region_name(nes_sys.apu.region), 72, y, 5, false, 0xFFFFFF);
    panel_metric(y, 14, 20, "FPS", "%.2f", playing ? s.fps : 0);
    panel_metric(y, 36, 26, "SPEED", "%.2f%%", playing ? s.speed : 0); y += 12;
    panel_metric(y, 0, 26, "CPU", "%.4f MHz", playing ? nes_region_cpu_hz(nes_sys.apu.region) / 1000000.0 * s.speed / 100 : 0);
    panel_metric(y, 28, 34, "HOST LOAD", "%.1f%%", playing && debug_total_ticks ? 100.0 * debug_emu_ticks / debug_total_ticks : 0); y += 12;
    panel_metric(y, 0, 28, "FRAME MAX", "%.2f ms", s.max_ms);
    panel_metric(y, 30, 32, "SPIKES", "%u", s.spikes); y += 12;
    panel_metric(y, 0, 19, "AUDIO", "%s", !audio_device ? "UNAVAILABLE" : audio_muted ? "MUTED" : "ON");
    panel_metric(y, 21, 21, "QUEUE", "%u B", host_audio_queued_bytes());
    panel_metric(y, 44, 18, "DEV", "%.1f ms", audio_device_ms); y += 12;
    panel_metric(y, 0, 20, "EMPTY", "%u", audio_monitor.underruns);
    panel_metric(y, 21, 20, "TRIMS", "%u", audio_monitor.trims);
    panel_metric(y, 42, 20, "ERRORS", "%u", audio_monitor.errors); y += 12;
    panel_metric(y, 0, 22, "READS", "%u", s.polls);
    panel_metric(y, 24, 21, "CHANGES", "%u", s.changed);
    panel_metric(y, 47, 15, "TRACE", "%s", diagnostics.tracing ? "ON" : "OFF"); y += 12;
    if (full) {
        y += 8;
        panel_line(&y, 0x78C8FF, "INTERRUPTS / PPU");
        panel_line(&y, 0xFFFFFF, "IRQ %02X [MAPPER:%d FRAME:%d DMC:%d] NMI %d/%d",
            nes_sys.cpu.irq_lines, !!(nes_sys.cpu.irq_lines & 1), !!(nes_sys.cpu.irq_lines & 2),
            !!(nes_sys.cpu.irq_lines & 4), nes_sys.cpu.nmi_line, nes_sys.cpu.nmi_edge);
        panel_line(&y, 0xFFFFFF, "SCANLINE %3d  DOT %3d  STATUS %02X  %-4s FRAME",
            nes_sys.ppu.scanline, nes_sys.ppu.cycle, nes_sys.ppu.ppu_status, nes_sys.ppu.odd_frame ? "ODD" : "EVEN");
        panel_line(&y, 0xFFFFFF, "CTRL %02X MASK %02X  SCROLL V:%04X T:%04X",
            nes_sys.ppu.ppu_ctrl, nes_sys.ppu.ppu_mask, nes_sys.ppu.v, nes_sys.ppu.t);
        panel_line(&y, 0xFFFFFF, "FINE X %d  LATCH W %d  OAM %02X  SPRITES %d/8",
            nes_sys.ppu.x, nes_sys.ppu.w, nes_sys.ppu.oam_addr, nes_sys.ppu.scanline_sprite_count);
        y += 8;
        panel_line(&y, 0x78C8FF, "APU");
        panel_line(&y, 0xFFFFFF, "ENABLED P1:%d P2:%d TRI:%d NOISE:%d DMC:%d",
            nes_sys.apu.pulse_enabled[0], nes_sys.apu.pulse_enabled[1], nes_sys.apu.triangle_enabled,
            nes_sys.apu.noise_enabled, nes_sys.apu.dmc_enabled);
        panel_line(&y, 0xFFFFFF, "LENGTHS P1:%3u P2:%3u TRI:%3u NOISE:%3u",
            nes_sys.apu.pulse_length_counter[0], nes_sys.apu.pulse_length_counter[1],
            nes_sys.apu.triangle_length_counter, nes_sys.apu.noise_length_counter);
        panel_line(&y, 0xFFFFFF, "SEQUENCER %d-STEP  FRAME IRQ %d", nes_sys.apu.frame_mode ? 5 : 4, nes_sys.apu.frame_irq_active);
        panel_line(&y, 0xFFFFFF, "DMC SAMPLE %04X CURRENT %04X LEFT %4u EMPTY %d",
            nes_sys.apu.dmc_sample_addr, nes_sys.apu.dmc_current_addr,
            nes_sys.apu.dmc_bytes_remaining, nes_sys.apu.dmc_buffer_empty);
        y += 8;
        const char *mirror = "Unknown";
        if (nes_sys.cart) switch (nes_sys.cart->mirroring) {
            case MIRROR_HORIZONTAL: mirror = "Horizontal"; break;
            case MIRROR_VERTICAL: mirror = "Vertical"; break;
            case MIRROR_FOUR_SCREEN: mirror = "4-Screen"; break;
            case MIRROR_ONE_SCREEN_LOW: mirror = "1-Screen Low"; break;
            case MIRROR_ONE_SCREEN_HIGH: mirror = "1-Screen High"; break;
        }
        panel_line(&y, 0x78C8FF, "MAPPER %4d  MIRROR %-13s", nes_sys.cart ? nes_sys.cart->mapper_id : -1, mirror);
        panel_line(&y, 0xFFFFFF, "P1 %02X [A B SELECT START UP DOWN LEFT RIGHT]", nes_sys.controller_state[0]);
        panel_line(&y, 0xFFFFFF, "PORT 2 %-6s  AIM %4d,%4d  TRIGGER %d LIGHT %d", nes_sys.zapper_enabled ? "ZAPPER" : "PAD",
            nes_sys.zapper_x, nes_sys.zapper_y, nes_sys.zapper_trigger, nes_sys.zapper_light);
    }
    draw_string(&debug_canvas, "F3:Debug panel  F2:Metrics  F4:Capture", 8, 612, 0x78C8FF);
    draw_string(&debug_canvas, "Shift+F10:Step  F9:Run  Ctrl+F4:Trace", 8, 626, 0x78C8FF);
}
static bool running = true, cursor_visible = true, was_playing;
static uint32_t last_mouse_activity;
static const uint32_t cursor_idle_ms = 2000;

static void remember_rom(const char *name) {
    int index = 0;
    while (index < recent_count && strcmp(recent_roms[index], name)) ++index;
    if (index == recent_count && recent_count < 4) ++recent_count;
    if (index > 3) index = 3;
    for (int i = index; i > 0; --i) memcpy(recent_roms[i], recent_roms[i - 1], sizeof(recent_roms[i]));
    snprintf(recent_roms[0], sizeof(recent_roms[0]), "%s", name);
    for (int i = 0; i < recent_count; ++i) {
        const char *name = recent_roms[i];
        for (const char *p = name; *p; ++p) if (*p == '/' || *p == '\\') name = p + 1;
        snprintf(recent_labels[i], sizeof(recent_labels[i]), "%.28s", name);
    }
}
static bool frontend_load_rom(const char *name) {
    if (nes_sys.cart) {
        if (!save_battery_ram()) return false;
        cartridge_free(nes_sys.cart);
        nes_sys.cart = NULL;
    }

    debugger_active = false;
    nametable_view.valid = false;
    memset(&apu_view, 0, sizeof(apu_view));
    nes_init(&nes_sys);
    nes_sys.region_override = (uint8_t)region_setting;
    bool trace_enabled = diagnostics.tracing;
    memset(&diagnostics, 0, sizeof(diagnostics));
    diagnostics.tracing = trace_enabled;
    memset(&audio_monitor, 0, sizeof(audio_monitor));
    nes_sys.diagnostics = &diagnostics;
    apply_rom_preferences();

    char rom_error[128];
    Cartridge *cart = cartridge_load_ex(&nes_sys, name, rom_error, sizeof(rom_error));
    if (!cart && strcmp(rom_error, "Cannot open ROM") == 0) {
        char *base_path = host_base_path();
        if (base_path) {
            char full_path[1024];
            snprintf(full_path, sizeof(full_path), "%s%s", base_path, name);
            cart = cartridge_load_ex(&nes_sys, full_path, rom_error, sizeof(rom_error));
            free(base_path);
        }
    }

    if (!cart) {
        show_notification(rom_error);
        notification_timer = 180;
        fprintf(stderr, "%s: %s\n", name, rom_error);
    }

    if (cart) {
        snprintf(loaded_rom_path, sizeof(loaded_rom_path), "%s", name);
        const char *basename = name;
        for (const char *p = name; *p; ++p) if (*p == '/' || *p == '\\') basename = p + 1;
        nes_sys.cart = cart;
        apply_rom_preferences();
        strncpy(loaded_rom_name, basename, sizeof(loaded_rom_name) - 1);
        loaded_rom_name[sizeof(loaded_rom_name) - 1] = '\0';

        char rom_name_clean[256];
        strncpy(rom_name_clean, basename, sizeof(rom_name_clean) - 1);
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
            return false;
        }
        debugger_active = false;
        debugger_logging_active = false;
        if (debugger_active) {
            debugger_view_pc = nes_sys.cpu.program_counter;
            debugger_selected_line = 0;
            clear_view_history(debugger_view_pc);
        }
        paused = false;
        runtime_reset_pending = true;
        remember_rom(name);
    }
    return nes_sys.cart != NULL;
}


#define ITEM(label, shortcut, command) {label, shortcut, command, 0, NULL}
#define SUB(label, menu) {label, NULL, MENU_NONE, 0, &menu}
#define SEPARATOR {NULL, NULL, MENU_NONE, MENU_SEPARATOR, NULL}
#define COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))
static const MenuItem recent_items[] = {
    ITEM(recent_labels[0], NULL, MENU_RECENT_1), ITEM(recent_labels[1], NULL, MENU_RECENT_2),
    ITEM(recent_labels[2], NULL, MENU_RECENT_3), ITEM(recent_labels[3], NULL, MENU_RECENT_4)
};
static const Menu recent_menu = {"Recent ROMs", recent_items, COUNT(recent_items)};
static const MenuItem port_items[] = {
    ITEM("Controller", NULL, MENU_CONTROLLER), ITEM("Zapper", NULL, MENU_ZAPPER)
};
static const Menu port_menu = {"Port 2", port_items, COUNT(port_items)};
static const MenuItem size_items[] = {
    ITEM("1x", NULL, MENU_SIZE_1), ITEM("2x", NULL, MENU_SIZE_2),
    ITEM("3x", NULL, MENU_SIZE_3), ITEM("4x", NULL, MENU_SIZE_4), ITEM("Maximized", NULL, MENU_MAXIMIZED)
};
static const Menu size_menu = {"Window Size", size_items, COUNT(size_items)};
static char save_shortcut[32], load_shortcut[32];
static const MenuItem file_items[] = {
    ITEM("Open ROM...", "Ctrl+O", MENU_OPEN), SUB("Recent ROMs", recent_menu),
    ITEM("Save State", save_shortcut, MENU_SAVE), ITEM("Load State", load_shortcut, MENU_LOAD),
    ITEM("Save State As...", NULL, MENU_SAVE_AS), ITEM("Load State From...", NULL, MENU_LOAD_FROM),
    SEPARATOR, ITEM("Exit", NULL, MENU_EXIT)
};
static const MenuItem audio_items[] = {
    ITEM("Mute", NULL, MENU_MUTE), ITEM("Volume Down", NULL, MENU_VOLUME_DOWN), ITEM("Volume Up", NULL, MENU_VOLUME_UP)
};
static const Menu audio_menu = {"Audio", audio_items, COUNT(audio_items)};
static const MenuItem region_items[] = {
    ITEM("Automatic (ROM header)", NULL, MENU_REGION_AUTO),
    ITEM("NTSC", NULL, MENU_REGION_NTSC), ITEM("PAL", NULL, MENU_REGION_PAL),
    ITEM("Dendy", NULL, MENU_REGION_DENDY)
};
static const Menu region_menu = {"Region", region_items, COUNT(region_items)};
static const MenuItem emulation_items[] = {
    ITEM("Pause", NULL, MENU_PAUSE), ITEM("Reset", NULL, MENU_RESET),
    ITEM("Power Cycle", NULL, MENU_POWER), SEPARATOR, SUB("Region", region_menu), SUB("Port 2", port_menu), SUB("Audio", audio_menu),
    ITEM("Controller Bindings...", NULL, MENU_BINDINGS), SEPARATOR,
    ITEM("Use Global ROM Settings", NULL, MENU_INHERIT), ITEM("Set as Global Defaults", NULL, MENU_GLOBAL_DEFAULTS)
};
static const MenuItem view_items[] = {
    SUB("Window Size", size_menu), ITEM("Fullscreen", "F11", MENU_FULLSCREEN),
    ITEM("CRT Shader", NULL, MENU_CRT),
    ITEM("Nametable Viewer", NULL, MENU_NAMETABLES),
    ITEM("APU Viewer", NULL, MENU_APU_VIEWER),
    ITEM("Debug Panel", "F3", MENU_DEBUG_PANEL), ITEM("Metrics Panel", "F2", MENU_METRICS)
};
static const MenuItem debug_items[] = {
    ITEM("Step Instruction", "Shift+F10", MENU_STEP), ITEM("Run/Pause", "F9", MENU_RUN),
    ITEM("Breakpoint", "F7", MENU_BREAKPOINT), ITEM("Event Trace", "Ctrl+F4", MENU_TRACE)
};
static const MenuItem help_items[] = {
    ITEM("Controls / Shortcuts", NULL, MENU_CONTROLS), ITEM("About", NULL, MENU_ABOUT)
};
static const Menu desktop_menus[] = {
    {"File", file_items, COUNT(file_items)}, {"Emulation", emulation_items, COUNT(emulation_items)},
    {"View", view_items, COUNT(view_items)}, {"Debug", debug_items, COUNT(debug_items)},
    {"Help", help_items, COUNT(help_items)}
};
#undef ITEM
#undef SUB
#undef SEPARATOR

static unsigned desktop_state(void *context, MenuCommand command) {
    (void)context;
    if (command >= MENU_RECENT_1 && command <= MENU_RECENT_4)
        return (int)(command - MENU_RECENT_1) >= recent_count ? MENU_DISABLED : 0;
    if ((command == MENU_SAVE_AS || command == MENU_LOAD_FROM || command == MENU_SAVE || command == MENU_LOAD || command == MENU_PAUSE ||
         command == MENU_RESET || command == MENU_POWER || command == MENU_STEP ||
         command == MENU_RUN || command == MENU_BREAKPOINT) && !nes_sys.cart) return MENU_DISABLED;
    switch (command) {
        case MENU_MUTE: return audio_muted ? MENU_CHECKED : 0;
        case MENU_INHERIT: return !nes_sys.cart ? MENU_DISABLED : preferences_inherit ? MENU_CHECKED : 0;
        case MENU_PAUSE: return paused ? MENU_CHECKED : 0;
        case MENU_RUN: return debugger_active ? MENU_CHECKED : 0;
        case MENU_BREAKPOINT: return !debugger_active ? MENU_DISABLED :
            breakpoints[debugger_line_pcs[debugger_selected_line]] ? MENU_CHECKED : 0;
        case MENU_CONTROLLER: return !zapper_enabled ? MENU_CHECKED : 0;
        case MENU_ZAPPER: return zapper_enabled ? MENU_CHECKED : 0;
        case MENU_FULLSCREEN: return fullscreen ? MENU_CHECKED : 0;
        case MENU_REGION_AUTO: return region_setting == NES_REGION_AUTO ? MENU_CHECKED : 0;
        case MENU_REGION_NTSC: return region_setting == NES_NTSC ? MENU_CHECKED : 0;
        case MENU_REGION_PAL: return region_setting == NES_PAL ? MENU_CHECKED : 0;
        case MENU_REGION_DENDY: return region_setting == NES_DENDY ? MENU_CHECKED : 0;
        case MENU_APU_VIEWER: return apu_viewer_enabled ? MENU_CHECKED : 0;
        case MENU_NAMETABLES: return nametable_viewer_enabled ? MENU_CHECKED : 0;
        case MENU_CRT: return crt_enabled ? MENU_CHECKED : 0;
        case MENU_DEBUG_PANEL: return debug_panel_enabled ? MENU_CHECKED : 0;
        case MENU_METRICS: return performance_visible ? MENU_CHECKED : 0;
        case MENU_TRACE: return diagnostics.tracing ? MENU_CHECKED : 0;
        default: return command >= MENU_SIZE_1 && command <= MENU_MAXIMIZED &&
            window_scale == (int)(command - MENU_SIZE_1) + 1 && !fullscreen ? MENU_CHECKED : 0;
    }
}
static void desktop_command(MenuCommand command) {
    if (desktop_state(NULL, command) & MENU_DISABLED) return;
    clear_host_input(); runtime_reset_pending = true;
    if (command >= MENU_RECENT_1 && command <= MENU_RECENT_4) {
        char name[BROWSER_PATH]; snprintf(name, sizeof(name), "%s", recent_roms[command - MENU_RECENT_1]);
        frontend_load_rom(name); return;
    }
    if (command >= MENU_SIZE_1 && command <= MENU_MAXIMIZED) {
        window_scale = command - MENU_SIZE_1 + 1; fullscreen = false;
        preferences_inherit = false; apply_display(); save_emulator_settings(); return;
    }
    switch (command) {
        case MENU_OPEN:
            browser_mode = 0;
            file_browser_open(&file_browser, *file_browser.path ? file_browser.path : NULL, "Open ROM", ".nes", false); break;
        case MENU_SAVE_AS: case MENU_LOAD_FROM:
            browser_mode = command == MENU_SAVE_AS ? 2 : 1;
            file_browser_open(&file_browser, save_state_dir, browser_mode == 2 ? "Save State As" : "Load State", ".state", browser_mode == 2); break;
        case MENU_SAVE: case MENU_LOAD: {
            char filename[128];
            get_rolling_quicksave_filename(filename, sizeof(filename), command == MENU_SAVE);
            if (command == MENU_SAVE) save_emulator_state(save_state_dir, filename);
            else load_emulator_state(save_state_dir, filename);
            break;
        }
        case MENU_EXIT: if (save_battery_ram()) running = false; break;
        case MENU_PAUSE: paused = !paused; break;
        case MENU_RESET:
            nametable_view.valid = false;
            memset(&apu_view, 0, sizeof(apu_view));
            nes_reset(&nes_sys); nes_clock_tick(&nes_sys);
            debugger_view_pc = nes_sys.cpu.program_counter; debugger_selected_line = 0;
            clear_view_history(debugger_view_pc); break;
        case MENU_POWER: {
            char name[BROWSER_PATH]; snprintf(name, sizeof(name), "%s", loaded_rom_path);
            frontend_load_rom(name); break;
        }
        case MENU_CONTROLLER: case MENU_ZAPPER:
            preferences_inherit = false; zapper_enabled = command == MENU_ZAPPER;
            nes_sys.zapper_enabled = zapper_enabled; nes_reset_zapper_watchdog(&nes_sys);
            save_emulator_settings(); break;
        case MENU_FULLSCREEN:
            preferences_inherit = false; fullscreen = !fullscreen; apply_display(); save_emulator_settings(); break;
        case MENU_DEBUG_PANEL: debug_panel_enabled = !debug_panel_enabled; save_emulator_settings(); break;
        case MENU_APU_VIEWER:
            apu_viewer_enabled = !apu_viewer_enabled;
            memset(&apu_view, 0, sizeof(apu_view));
            save_emulator_settings(); break;
        case MENU_REGION_AUTO: case MENU_REGION_NTSC: case MENU_REGION_PAL: case MENU_REGION_DENDY:
            region_setting = command == MENU_REGION_AUTO ? NES_REGION_AUTO :
                command == MENU_REGION_NTSC ? NES_NTSC : command == MENU_REGION_PAL ? NES_PAL : NES_DENDY;
            nes_sys.region_override = (uint8_t)region_setting;
            nes_reset(&nes_sys);
            nametable_view.valid = false;
            memset(&apu_view, 0, sizeof(apu_view));
            runtime_reset_pending = true;
            save_emulator_settings(); break;
        case MENU_NAMETABLES:
            nametable_viewer_enabled = !nametable_viewer_enabled;
            nametable_view.valid = false; save_emulator_settings(); break;
        case MENU_CRT:
            crt_enabled = !crt_enabled; host_set_crt(crt_enabled); save_emulator_settings(); break;
        case MENU_METRICS: performance_visible = !performance_visible; save_emulator_settings(); break;
        case MENU_STEP: case MENU_RUN:
            paused = false;
            if (debugger_active) {
                debugger_step_instruction(&nes_sys.cpu, &cpu_bus_bridge);
                if (command == MENU_RUN) debugger_active = false;
            } else {
                debugger_active = true; debugger_view_pc = nes_sys.cpu.program_counter;
                debugger_selected_line = 0;
            }
            clear_view_history(debugger_view_pc); break;
        case MENU_BREAKPOINT: {
            uint16_t pc = debugger_line_pcs[debugger_selected_line];
            breakpoints[pc] = !breakpoints[pc]; break;
        }
        case MENU_TRACE:
            diagnostics.tracing = !diagnostics.tracing;
            if (diagnostics.tracing) {
                diagnostics.event_count = diagnostics.event_head = diagnostics.overwritten = 0;
                diagnostics_event(&nes_sys, DIAG_RESUME, 0, 0);
            }
            show_notification(diagnostics.tracing ? "EVENT TRACE ON" : "EVENT TRACE OFF"); break;
        case MENU_MUTE: audio_muted = !audio_muted; save_emulator_settings(); break;
        case MENU_VOLUME_DOWN: case MENU_VOLUME_UP: {
            master_volume += command == MENU_VOLUME_UP ? 10 : -10;
            if (master_volume < 0) master_volume = 0;
            if (master_volume > 100) master_volume = 100;
            char text[32]; snprintf(text, sizeof(text), "VOLUME %d%%", master_volume);
            show_notification(text); play_volume_ding(); save_emulator_settings(); break;
        }
        case MENU_INHERIT: preferences_inherit = true; save_emulator_settings(); apply_rom_preferences(); break;
        case MENU_GLOBAL_DEFAULTS:
            global_preferences = (RomPreferences){(unsigned)window_scale, fullscreen, zapper_enabled};
            save_emulator_settings(); show_notification("GLOBAL DEFAULTS UPDATED"); break;
        case MENU_BINDINGS: help_page = 3; control_selection = 0; rebinding = false; break;
        case MENU_CONTROLS: help_page = 1; break;
        case MENU_ABOUT: help_page = 2; break;
        default: break;
    }
}
static void chrome_fill(void *context, MenuRect r, uint32_t color) {
    (void)context; host_ui_rect((HostRect){r.x, r.y, r.w, r.h}, color);
}
static void chrome_text(void *context, const char *text, int x, int y, uint32_t color) {
    (void)context; host_ui_text(text, x, y, color);
}
static MenuRect controls_bounds(int w, int h) {
    int width = w < 416 ? w - 16 : 400;
    int height = h - MENU_BAR_HEIGHT - STATUS_BAR_HEIGHT - 16;
    if (height > 260) height = 260;
    return (MenuRect){(w-width)/2, MENU_BAR_HEIGHT+8, width, height};
}
static void controls_activate(void) {
    if (!control_selection) control_mode_keyboard = !control_mode_keyboard;
    else if (control_selection == CONTROL_COUNT+1) {
        memcpy(control_mappings, default_control_mappings, sizeof(control_mappings));
        memcpy(controller_button_mappings, default_controller_mappings, sizeof(controller_button_mappings));
        save_emulator_settings();
    } else rebinding = true;
}
static void desktop_draw(void) {
    int w, h; host_chrome_layout(sapp_width(), sapp_height(), &w, &h);
    menu_bar_resize(&desktop_menu, w, h);
    snprintf(save_shortcut, sizeof(save_shortcut), "%s", host_key_name(control_mappings[8]));
    snprintf(load_shortcut, sizeof(load_shortcut), "%s", host_key_name(control_mappings[9]));
    MenuPainter painter = {NULL, chrome_fill, chrome_text};
    DiagnosticSummary summary = diagnostics_summary(&diagnostics);
    bool playing = nes_sys.cart && !paused &&
        !debugger_active && !desktop_menu.active && !help_page && !file_browser.active && focused;
    char text[128], mapper[65], fps[65], speed[65], audio[65], value[64];
    bool compact = w < 360, wide = w >= 576;
    snprintf(value, sizeof(value), compact ? "%.0f" : wide ? "%.2f" : "%.1f", playing ? summary.fps : 0);
    format_text_field(fps, value, compact ? 3 : wide ? 6 : 5, true);
    snprintf(value, sizeof(value), "%.0f", playing ? summary.speed : 0);
    format_text_field(speed, value, compact ? 3 : wide ? 5 : 4, true);
    if (nes_sys.cart) snprintf(value, sizeof(value), wide ? "Mapper %4u" : "M%4u", nes_sys.cart->mapper_id);
    else snprintf(value, sizeof(value), "No ROM");
    format_text_field(mapper, value, wide ? 11 : 5, false);
    const char *audio_status = !audio_device ? "N/A" : audio_muted || !master_volume ? "Muted" :
        !playing ? "Paused" : audio_monitor.errors ? "Error" : "OK";
    if (compact) {
        snprintf(value, sizeof(value), "%.3s", audio_status);
        format_text_field(audio, value, 3, false);
        snprintf(text, sizeof(text), "%.3sFPS|%.3s%%|%.5s|%.3s|P2:%s",
            fps, speed, mapper, audio, zapper_enabled ? "Gun" : "Pad");
    } else if (!wide) {
        format_text_field(audio, audio_status, 6, false);
        snprintf(text, sizeof(text), "%.5sFPS|%.4s%%|%.5s|Audio %.6s|P2:%s",
            fps, speed, mapper, audio, zapper_enabled ? "Gun" : "Pad");
    } else {
        format_text_field(audio, audio_status, 6, false);
        snprintf(text, sizeof(text), "%.6s FPS | %.5s%% | %.11s | Audio %.6s | Port 2: %-10s",
            fps, speed, mapper, audio, zapper_enabled ? "Zapper" : "Controller");
    }
    if (!nes_sys.cart && notification_timer > 0) {
        snprintf(text, sizeof(text), "%s", notification_text); --notification_timer;
    }
    status_bar_draw(&(StatusBar){text}, &painter, w, h);
    if (!nes_sys.cart && !file_browser.active && !help_page) {
        chrome_text(NULL, "File > Open ROM...", 16, MENU_BAR_HEIGHT + 24, 0xAAAAAA);
        chrome_text(NULL, "Ctrl+O", 16, MENU_BAR_HEIGHT + 40, 0xAAAAAA);
    }
    if (file_browser.active) {
        file_browser_resize(&file_browser, w, h); file_browser_draw(&file_browser, &painter);
    }
    if (help_page == 3) {
        MenuRect r = controls_bounds(w, h);
        chrome_fill(NULL, r, 0xD4D0C8);
        chrome_fill(NULL, (MenuRect){r.x,r.y,r.w,20}, 0x0A246A);
        chrome_text(NULL, "Controller Bindings", r.x+8,r.y+6,0xFFFFFF);
        int row_height = (r.h-46)/(CONTROL_COUNT+2);
        for (int i = 0; i < CONTROL_COUNT+2; ++i) {
            char line[96];
            if (!i) snprintf(line,sizeof(line),"Input: %s",control_mode_keyboard ? "Keyboard" : "Controller");
            else if (i == CONTROL_COUNT+1) snprintf(line,sizeof(line),"Restore Defaults");
            else if (rebinding && i == control_selection) snprintf(line,sizeof(line),"%s: Press a %s...",button_names[i-1],control_mode_keyboard ? "key" : "button");
            else if (control_mode_keyboard) snprintf(line,sizeof(line),"%s: %s",button_names[i-1],host_key_name(control_mappings[i-1]));
            else snprintf(line,sizeof(line),"%s: Button %d",button_names[i-1],controller_button_mappings[i-1]);
            int y = r.y+26+i*row_height;
            if (i == control_selection) chrome_fill(NULL,(MenuRect){r.x+4,y-2,r.w-8,row_height},0x0A246A);
            int chars = (r.w-16)/8; if (chars >= 0 && chars < (int)sizeof(line)) line[chars] = 0;
            chrome_text(NULL,line,r.x+8,y,i == control_selection ? 0xFFFFFF : 0x202020);
        }
        chrome_text(NULL,"Enter: Change  Esc: Close",r.x+8,r.y+r.h-16,0x202020);
    } else if (help_page) {
        int x = w > 352 ? (w - 336) / 2 : 8, y = MENU_BAR_HEIGHT + 12;
        chrome_fill(NULL, (MenuRect){x, y, 336, 180}, 0x808080);
        chrome_fill(NULL, (MenuRect){x + 1, y + 1, 334, 178}, 0xF0EEE8);
        static const char *controls[] = {"Controls / Shortcuts", "Alt / F10: Menu bar", "Arrows / Enter: Select", "Escape: Close / Back", "Shift+F10: Step   F9: Run/Pause", "F2: Metrics   F3: Debug panel", "F4: Capture   Ctrl+F4: Trace", "F7: Breakpoint   F11: Fullscreen", "Enter: Controller bindings", "Escape or click: Close"};
        static const char *about[] = {"NES Emulator", "Custom Sokol desktop interface", "8x8 bitmap font", "Windows / Linux / macOS", "", "Escape, Enter or click: Close"};
        const char **lines = help_page == 1 ? controls : about;
        int count = help_page == 1 ? COUNT(controls) : COUNT(about);
        for (int i = 0; i < count; ++i) chrome_text(NULL, lines[i], x + 8, y + 10 + i * 16, 0x202020);
    }
    menu_bar_draw(&desktop_menu, &painter);
}
static void draw_nametable_panel(void) {
    host_set_nametable_panel(nametable_viewer_enabled ? &nametable_canvas : NULL);
    if (!nametable_viewer_enabled) return;
    nametable_canvas.width = HOST_PANEL_WIDTH;
    nametable_canvas.height = HOST_PANEL_HEIGHT;
    host_color(&nametable_canvas, 15, 20, 35, 255);
    host_clear(&nametable_canvas);
    if (!nes_sys.cart) {
        draw_string(&nametable_canvas, "NAMETABLES - Open a ROM to view", 8, 4, 0x78C8FF);
        return;
    }
    if (!nametable_view.valid) {
        draw_string(&nametable_canvas, "NAMETABLES - Resume to capture background", 8, 4, 0x78C8FF);
        return;
    }
    static const char *mirroring[] = {
        "Horizontal (A A / B B)", "Vertical (A B / A B)",
        "Four screen", "One screen low", "One screen high"
    };
    char title[64];
    const char *mode = nametable_view.mirroring < 0 ?
        "Mapper controlled" : mirroring[nametable_view.mirroring];
    snprintf(title, sizeof(title), "NAMETABLES - %s", mode);
    draw_string(&nametable_canvas, title, 8, 4, 0x78C8FF);
    for (int y = 0; y < PPU_NAMETABLE_HEIGHT; ++y) {
        int dest_y = y + (y < 240 ? 32 : 48);
        for (int x = 0; x < PPU_NAMETABLE_WIDTH; ++x) {
            uint32_t p = nametable_view.pixels[y * PPU_NAMETABLE_WIDTH + x];
            nametable_canvas.pixels[dest_y * HOST_PANEL_WIDTH + x] =
                0xFF000000u | ((p & 255) << 16) | (p & 0xFF00) | ((p >> 16) & 255);
        }
    }
    draw_string(&nametable_canvas, "$2000", 8, 20, 0xFFFFFF);
    draw_string(&nametable_canvas, "$2400", 264, 20, 0xFFFFFF);
    draw_string(&nametable_canvas, "$2800", 8, 276, 0xFFFFFF);
    draw_string(&nametable_canvas, "$2C00", 264, 276, 0xFFFFFF);
}
static void apu_rect(int x, int y, int w, int h, uint32_t color) {
    host_color(&apu_canvas, (color >> 16) & 255, (color >> 8) & 255, color & 255, 255);
    host_fill_rect(&apu_canvas, &(HostRect){x, y, w, h});
}
static uint32_t apu_level_color(uint32_t color, unsigned level, unsigned maximum) {
    unsigned brightness = 80 + 175 * level / maximum;
    return (((((color >> 16) & 255) * brightness / 255) << 16) |
            ((((color >> 8) & 255) * brightness / 255) << 8) |
            ((color & 255) * brightness / 255));
}
static void draw_apu_panel(void) {
    static const uint32_t colors[5] = {0x59D8FF, 0xFFAF61, 0x9BE876, 0xD49AFF, 0xFF7799};
    static const uint32_t expansion_colors[8] = {0xFFE36E,0x7DFFCE,0xFF83CB,0xA4AAFF,0xFF896E,0xBBE878,0x72C8FF,0xF3B8FF};
    static const char *names[5] = {"Pulse 1", "Pulse 2", "Triangle", "Noise", "DMC DAC"};
    static const char *notes[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    host_set_apu_panel(apu_viewer_enabled ? &apu_canvas : NULL);
    if (!apu_viewer_enabled) return;
    apu_canvas.width = HOST_PANEL_WIDTH; apu_canvas.height = HOST_APU_HEIGHT;
    host_color(&apu_canvas, 15, 20, 35, 255); host_clear(&apu_canvas);
    draw_string(&apu_canvas, "APU / PIANO ROLL", 12, 10, 0xFFFFFF);
    draw_string(&apu_canvas, !nes_sys.cart ? "Open a ROM to view audio" :
        (paused || debugger_active || desktop_menu.active || !focused || help_page || file_browser.active)
        ? "PAUSED - history frozen" : audio_muted ? "LIVE - audio muted" : "LIVE - notes and sound effects", 12, 28, 0x90A6BD);
    ApuViewSample current;
    apu_view_snapshot_nes(&nes_sys, &current);
    unsigned mapper = nes_sys.cart ? nes_sys.cart->mapper_id : 0;
    const char *chip = mapper == 5 ? "MMC5" : mapper == 19 ? "N163" : mapper == 20 ? "FDS" :
        mapper == 69 ? "5B" : mapper == 85 ? "VRC7" : mapper == 24 || mapper == 26 ? "VRC6" : "None";
    char info[64];
    snprintf(info, sizeof(info), "%-5s / Expansion: %-4s / 44.1 kHz", nes_region_name(nes_sys.apu.region), chip);
    draw_string(&apu_canvas, info, 12, 50, 0x90A6BD);
    draw_string(&apu_canvas, "Click a channel name to mute / unmute", 12, 64, 0x90A6BD);
    // C1..B7; each channel has its own pixel within a semitone row so
    // simultaneous unisons stay visible. One column represents 1/60 second.
    for (int note = 24; note <= 107; ++note) {
        int pitch = note % 12, y = 80 + (107 - note) * 3;
        bool black = pitch == 1 || pitch == 3 || pitch == 6 || pitch == 8 || pitch == 10;
        apu_rect(44, y, 460, 3, black ? 0x141C2C : 0x1C283B);
        apu_rect(12, y, black ? 20 : 28, 3, black ? 0x243145 : 0x708198);
        if (!pitch) apu_rect(44, y + 2, 460, 1, 0x34465C);
    }
    for (int note = 24; note <= 107; note += 12) {
        int y = 80 + (107 - note) * 3;
        char label[8]; snprintf(label, sizeof(label), "C%d", note / 12 - 1);
        apu_rect(12, y - 5, 28, 8, 0x243145);
        draw_string(&apu_canvas, label, 13, y - 5, 0xFFFFFF);
    }
    for (int seconds = 0; seconds <= 7; ++seconds) {
        int x = 503 - seconds * 60;
        apu_rect(x, 80, 1, 252, 0x34465C);
        char label[8]; snprintf(label, sizeof(label), "-%ds", seconds);
        draw_string(&apu_canvas, seconds ? label : "NOW", x - 20, 338, 0x90A6BD);
    }
    draw_string(&apu_canvas, "NOI", 12, 357, colors[3]);
    draw_string(&apu_canvas, "DMC", 12, 381, colors[4]);
    apu_rect(44, 354, 460, 16, 0x1C283B);
    apu_rect(44, 378, 460, 16, 0x1C283B);
    for (unsigned age = 0; age < apu_view.count; ++age) {
        const ApuViewSample *s = apu_view_age(&apu_view, age);
        int x = 503 - (int)(age / 4);
        for (unsigned ch = 0; ch < 3; ++ch) {
            int note = (int)lroundf(s->note[ch]);
            if (s->active[ch] && note >= 24 && note <= 107)
                apu_rect(x, 80 + (107 - note) * 3 + (int)ch, 1, 1,
                    apu_level_color(colors[ch], s->level[ch], 15));
        }
        for (unsigned ch = 0; ch < s->expansion_count; ++ch) {
            int note = (int)lroundf(s->expansion_note[ch]);
            if (s->expansion_active[ch] && note >= 24 && note <= 107)
                apu_rect(x, 80 + (107 - note) * 3 + (int)(ch % 3), 1, 1,
                    apu_level_color(expansion_colors[ch], s->expansion_level[ch], 255));
        }
        if (s->active[3]) apu_rect(x, 369 - s->level[3], 1, s->level[3], colors[3]);
        int height = (s->level[4] * 15 + 126) / 127;
        if (height) apu_rect(x, 393 - height, 1, height,
            s->active[4] ? colors[4] : apu_level_color(colors[4], 0, 127));
    }
    // Two fixed columns share the space under the roll. Eight expansion rows
    // end at y=508, inside the host's 528-pixel texture crop at every scale.
    draw_string(&apu_canvas, "APU / LEVEL / NOTE / Hz", 12, 400, 0x90A6BD);
    apu_rect(258, 400, 1, 108, 0x34465C);
    for (unsigned ch = 0; ch < 5; ++ch) {
        int y = APU_VIEW_ROWS_Y + (int)ch * APU_VIEW_ROW_HEIGHT;
        unsigned level = (ch == 4 || current.active[ch]) ? current.level[ch] : 0;
        unsigned maximum = ch == 4 ? 127 : 15;
        bool muted = (nes_sys.audio_muted_channels & (1u << ch)) != 0;
        draw_text_field(&apu_canvas, names[ch], APU_VIEW_NAME_X, y, APU_VIEW_NAME_CELLS,
            false, muted ? 0x737B89 : colors[ch]);
        apu_rect(84, y, 32, 8, 0x28374D);
        apu_rect(84, y, (int)(32 * level / maximum), 8, colors[ch]);
        char pitch[16] = "--", frequency[32] = "--";
        if (ch < 3 && current.active[ch]) {
            int note = (int)lroundf(current.note[ch]);
            if (note >= 0 && note <= 127) {
                snprintf(pitch, sizeof(pitch), "%s%d", notes[note % 12], note / 12 - 1);
                snprintf(frequency, sizeof(frequency), "%.1f", current.hz[ch]);
            }
        }
        draw_text_field(&apu_canvas, pitch, 124, y, 4, false, colors[ch]);
        draw_text_field(&apu_canvas, frequency, 164, y, 8, true, 0xD9E3F0);
        draw_string(&apu_canvas, "Hz", 236, y, 0x90A6BD);
    }
    static const char *duty[] = {"12.5%", "25%", "50%", "75%"};
    snprintf(info, sizeof(info), "Duty P1:%5s P2:%5s",
        duty[current.duty[0] & 3], duty[current.duty[1] & 3]);
    draw_string(&apu_canvas, info, 12, 484, 0x90A6BD);
    snprintf(info, sizeof(info), "Noise:%-5s DMC:%-4s",
        nes_sys.apu.noise_mode ? "Short" : "Long", nes_sys.apu.dmc_loop ? "Loop" : "Once");
    draw_string(&apu_canvas, info, 12, 500, 0x90A6BD);
    draw_string(&apu_canvas, "EXP / LEVEL / NOTE / Hz", 268, 400, 0x90A6BD);
    if (current.expansion_count) {
        for (unsigned ch = 0; ch < current.expansion_count; ++ch) {
            int y = APU_VIEW_ROWS_Y + (int)ch * APU_VIEW_ROW_HEIGHT;
            char label[48], pitch[16] = "--", frequency[32] = "--";
            snprintf(label, sizeof(label), "%s %u", chip, ch + 1);
            bool muted = (nes_sys.audio_muted_channels & (1u << (NES_AUDIO_EXPANSION_SHIFT + ch))) != 0;
            draw_text_field(&apu_canvas, label, APU_VIEW_EXP_NAME_X, y, APU_VIEW_EXP_NAME_CELLS,
                false, muted ? 0x737B89 : expansion_colors[ch]);
            unsigned level = current.expansion_active[ch] ? current.expansion_level[ch] : 0;
            apu_rect(324, y, 32, 8, 0x28374D);
            apu_rect(324, y, (int)(32 * level / 255), 8, expansion_colors[ch]);
            int note = (int)lroundf(current.expansion_note[ch]);
            if (level && note >= 0 && note <= 127) {
                snprintf(pitch, sizeof(pitch), "%s%d", notes[note % 12], note / 12 - 1);
                snprintf(frequency, sizeof(frequency), "%.1f", current.expansion_hz[ch]);
            }
            draw_text_field(&apu_canvas, pitch, 364, y, 4, false, expansion_colors[ch]);
            draw_text_field(&apu_canvas, frequency, 404, y, 9, true, 0xD9E3F0);
            draw_string(&apu_canvas, "Hz", 484, y, 0x90A6BD);
        }
    } else draw_string(&apu_canvas, "No expansion audio", 268, 416, 0x90A6BD);
    double dmc_bit_rate = nes_sys.apu.dmc_timer_reload
        ? (double)nes_region_cpu_hz(nes_sys.apu.region) / nes_sys.apu.dmc_timer_reload : 0;
    snprintf(info, sizeof(info), "DMC bit rate:%5.0f Hz  DAC:%3u  Bytes:%4u",
        dmc_bit_rate, (unsigned)nes_sys.apu.dmc_value, (unsigned)nes_sys.apu.dmc_bytes_remaining);
    draw_string(&apu_canvas, info, 12, 516, 0x90A6BD);
}
static void desktop_present(HostCanvas *game) {
    draw_nametable_panel();
    draw_apu_panel();
    if (nes_sys.cart && paused) {
        /* Use game pixels so the OSD grows with the picture, unlike desktop text. */
        HostRect badge = {96, 108, 64, 24};
        host_color(game, 0, 0, 0, 220);
        host_fill_rect(game, &badge);
        draw_string(game, "Paused", 104, 116, 0xFFFFFF);
    }
    host_present(game);
}
static bool desktop_event(const HostEvent *event) {
    MenuEvent e = {0};
    MenuCommand command;
    int w, h;
    float scale = host_chrome_layout(sapp_width(), sapp_height(), &w, &h);
    menu_bar_resize(&desktop_menu, w, h);
    if (event->type == HOST_WINDOWEVENT && event->window.event == HOST_WINDOWEVENT_FOCUS_LOST) {
        e.type = MENU_BLUR; menu_bar_event(&desktop_menu, &e, &command); help_page = 0; rebinding = false; return false;
    }
    if (!focused) return false;
    if (file_browser.active) {
        HostEvent input = *event;
        if (event->window_mouse.valid) {
            input.button.x = (int)floorf(event->window_mouse.x / scale);
            input.button.y = (int)floorf(event->window_mouse.y / scale);
        }
        file_browser_resize(&file_browser, w, h);
        char path[BROWSER_PATH];
        if (file_browser_event(&file_browser, &input, host_time(), path)) {
            if (!browser_mode) frontend_load_rom(path);
            else {
                NES_StateResult result = browser_mode == 2 ? nes_state_save(&nes_sys, path) : nes_state_load(&nes_sys, path);
                if (browser_mode != 2 && result == NES_STATE_OK) {
                    region_setting = nes_sys.region_override;
                    nametable_view.valid = false;
                    memset(&apu_view, 0, sizeof(apu_view));
                }
                show_notification(result == NES_STATE_OK ? (browser_mode == 2 ? "STATE SAVED" : "STATE LOADED") : nes_state_message(result));
                clear_host_input(); runtime_reset_pending = true;
                nes_sys.zapper_enabled = zapper_enabled;
                debugger_view_pc = nes_sys.cpu.program_counter; debugger_selected_line = 0; clear_view_history(debugger_view_pc);
            }
        }
        if (!file_browser.active) { clear_host_input(); runtime_reset_pending = true; desktop_menu.swallow_release = true; }
        return event->type == HOST_KEYDOWN || event->type == HOST_KEYUP || event->type == HOST_TEXTINPUT ||
            (event->type >= HOST_MOUSEMOTION && event->type <= HOST_MOUSEWHEEL);
    }
    if (help_page == 3) {
        if (rebinding) {
            if (event->type == HOST_KEYDOWN && !event->key.repeat) {
                if (event->key.keysym.sym == HOST_KEY_ESCAPE) rebinding = false;
                else if (control_mode_keyboard) {
                    control_mappings[control_selection-1] = event->key.keysym.sym; save_emulator_settings(); rebinding = false;
                }
            } else if (!control_mode_keyboard && event->type == HOST_CONTROLLERBUTTONDOWN && event->cbutton.which == game_controller) {
                controller_button_mappings[control_selection-1] = event->cbutton.button; save_emulator_settings(); rebinding = false;
            }
        } else if (event->type == HOST_KEYDOWN) {
            if (event->key.keysym.sym == HOST_KEY_ESCAPE) help_page = 0;
            else if (event->key.keysym.sym == HOST_KEY_UP) control_selection = (control_selection+CONTROL_COUNT+1)%(CONTROL_COUNT+2);
            else if (event->key.keysym.sym == HOST_KEY_DOWN) control_selection = (control_selection+1)%(CONTROL_COUNT+2);
            else if (event->key.keysym.sym == HOST_KEY_RETURN && !event->key.repeat) controls_activate();
        } else if (event->type == HOST_MOUSEBUTTONDOWN && event->button.button == HOST_BUTTON_LEFT && event->window_mouse.valid) {
            MenuRect r = controls_bounds(w,h);
            int x = (int)(event->window_mouse.x / scale), y = (int)(event->window_mouse.y / scale);
            int row_height = (r.h-46)/(CONTROL_COUNT+2);
            if (row_height > 0 && x >= r.x && x < r.x+r.w && y >= r.y+24 && y < r.y+24+(CONTROL_COUNT+2)*row_height) {
                control_selection = (y-r.y-24)/row_height; controls_activate();
            } else if (y >= r.y+r.h-22 && y < r.y+r.h) help_page = 0;
            desktop_menu.swallow_release = true;
        }
        return event->type != HOST_QUIT && event->type != HOST_WINDOWEVENT &&
            event->type != HOST_CONTROLLERDEVICEADDED && event->type != HOST_CONTROLLERDEVICEREMOVED;
    }
    if (event->type == HOST_KEYDOWN || event->type == HOST_KEYUP) {
        e.type = event->type == HOST_KEYDOWN ? MENU_KEY_PRESS : MENU_KEY_RELEASE;
        e.repeat = event->key.repeat;
        switch (event->key.keysym.sym) {
            case HOST_KEY_LALT: case HOST_KEY_RALT: e.key = MENU_KEY_ACTIVATE; break;
            case HOST_KEY_F10: if (!event->key.keysym.mod) e.key = MENU_KEY_ACTIVATE; break;
            case HOST_KEY_LEFT: e.key = MENU_KEY_LEFT; break;
            case HOST_KEY_RIGHT: e.key = MENU_KEY_RIGHT; break;
            case HOST_KEY_UP: e.key = MENU_KEY_UP; break;
            case HOST_KEY_DOWN: e.key = MENU_KEY_DOWN; break;
            case HOST_KEY_RETURN: e.key = MENU_KEY_ENTER; break;
            case HOST_KEY_ESCAPE: e.key = MENU_KEY_ESCAPE; break;
            default: break;
        }
    } else if (event->type >= HOST_MOUSEMOTION && event->type <= HOST_MOUSEWHEEL) {
        if (!event->window_mouse.valid) return false; /* Legacy synthetic game-coordinate events. */
        e.x = (int)floorf(event->window_mouse.x / scale); e.y = (int)floorf(event->window_mouse.y / scale);
        e.type = event->type == HOST_MOUSEMOTION ? MENU_MOVE : event->type == HOST_MOUSEBUTTONDOWN ? MENU_PRESS :
            event->type == HOST_MOUSEBUTTONUP ? MENU_RELEASE : MENU_WHEEL;
        last_mouse_activity = host_ticks();
    } else return false;
    if (help_page && !desktop_menu.active) {
        if ((e.type == MENU_KEY_PRESS && !e.repeat && (e.key == MENU_KEY_ESCAPE || e.key == MENU_KEY_ENTER)) || e.type == MENU_PRESS) {
            if (help_page == 1 && e.key == MENU_KEY_ENTER) { help_page = 3; control_selection = 0; }
            else help_page = 0;
            desktop_menu.swallow_release = e.type == MENU_PRESS;
        }
        return true;
    }
    bool active = desktop_menu.active;
    bool consumed = menu_bar_event(&desktop_menu, &e, &command);
    if (desktop_menu.active != active) { clear_host_input(); runtime_reset_pending = true; rebinding = false; }
    if (command) desktop_command(command);
    /* Panel clicks must not reach the legacy right-click/resume or Zapper paths. */
    if (!consumed && event->window_mouse.valid &&
        (event->type == HOST_MOUSEBUTTONDOWN || event->type == HOST_MOUSEWHEEL)) {
        HostRect game, panel; host_layout(sapp_width(), sapp_height(), &game, &panel);
        HostPoint point = {event->window_mouse.x, event->window_mouse.y};
        if (host_point_in_rect(&point, &panel)) return true;
        host_nametable_layout(sapp_width(), sapp_height(), &panel);
        if (host_point_in_rect(&point, &panel)) return true;
        host_apu_layout(sapp_width(), sapp_height(), &panel);
        if (host_point_in_rect(&point, &panel)) {
            if (nes_sys.cart && apu_viewer_enabled && event->type == HOST_MOUSEBUTTONDOWN &&
                event->button.button == HOST_BUTTON_LEFT && panel.w > 0 && panel.h > 0) {
                int x = (point.x - panel.x) * HOST_PANEL_WIDTH / panel.w;
                int y = (point.y - panel.y) * HOST_APU_HEIGHT / panel.h;
                ApuViewSample current;
                apu_view_snapshot_nes(&nes_sys, &current);
                int channel = apu_view_channel_at(current.expansion_count, x, y);
                if (channel >= 0) nes_sys.audio_muted_channels ^= (uint16_t)(1u << channel);
            }
            return true;
        }
    }
    return consumed;
}

static void app_init(void) {
    host_setup();
    host_set_crt(crt_enabled);
    host_set_chrome(desktop_draw, font8x8);
    menu_bar_init(&desktop_menu, desktop_menus, COUNT(desktop_menus), desktop_state, NULL);
    audio_device = host_audio_valid();
    audio_device_ms = host_audio_latency_ms();
    apply_display();
    nes_init(&nes_sys);
    nes_sys.diagnostics = &diagnostics;
    nes_sys.region_override = (uint8_t)region_setting;
    nes_sys.zapper_enabled = zapper_enabled;

    cpu_bus_bridge.bus_context = &nes_sys;
    cpu_bus_bridge.read = cpu_bridge_read;
    cpu_bus_bridge.write = cpu_bridge_write;
    cpu_bus_bridge.cycle_tick = NULL;

    last_mouse_activity = host_ticks();
}

static void app_frame(void) {
    HostEvent event;
    nes_sys.nametable_view = nametable_viewer_enabled ? &nametable_view : NULL;
    host_poll_gamepads();
    bool playing = nes_sys.cart && !debugger_active && !paused && !desktop_menu.active && !help_page && !file_browser.active && focused;
    if (runtime_reset_pending || playing != was_playing) {
        clear_host_input();
        reset_runtime();
    }
    was_playing = playing;
    if (nes_sys.cart != NULL) {
        if (debugger_active || paused || desktop_menu.active || help_page || file_browser.active || !focused) {
            host_color(renderer, 0, 0, 0, 255);
            host_clear(renderer);
            GameCrop crop = game_crop(nes_sys.cart->mapper_id);
            host_draw_frame(renderer, nes_sys.ppu.screen_buffer, &(HostRect){crop.x, crop.y, crop.w, crop.h});
            draw_notification(renderer);
            draw_debug_panel();
            desktop_present(renderer);
            host_delay(16);
        } else {
            uint64_t frame_start_tick = host_counter();
            uint64_t frame_start_cycles = nes_sys.cpu.cycle_count;

            nes_sys.frame_ready = false;
            if (nametable_viewer_enabled) nametable_view_begin_frame(&nametable_view);
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
                if (apu_viewer_enabled) apu_view_sample_nes(&apu_view, &nes_sys);
                if (nametable_viewer_enabled && !nametable_view.sampled)
                    nametable_view_sample(&nametable_view, &nes_sys);
            }

            if (nametable_viewer_enabled) nametable_view_finish_frame(&nametable_view);
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
                draw_string(renderer, "Emulation > Port 2", 16, 176, 0xFFFFFF);
                draw_string(renderer, "Select Controller", 16, 188, 0xFFFFFF);
                draw_string(renderer, "then resume the game.", 16, 200, 0xFFFFFF);
            }


            draw_notification(renderer);
            draw_debug_panel();
            desktop_present(renderer);

            if (nes_sys.frame_ready) {
                uint64_t cycles = nes_sys.cpu.cycle_count - frame_start_cycles;
                double wait = frame_scheduler_advance_rate(&frame_scheduler, host_time(), cycles,
                    nes_region_cpu_hz(nes_sys.apu.region));
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
        }
    } else {
        host_color(renderer, 18, 18, 22, 255); host_clear(renderer);
        draw_debug_panel(); desktop_present(renderer); host_delay(16);
    }

    while (running && host_poll_event(&event)) {
        if (desktop_event(&event)) continue;
        if (event.type >= HOST_MOUSEMOTION && event.type <= HOST_MOUSEWHEEL) last_mouse_activity = host_ticks();
        if (event.type == HOST_QUIT) {
            if (save_battery_ram()) running = false;
        } else if (event.type == HOST_WINDOWEVENT) {
            if (event.window.event == HOST_WINDOWEVENT_FOCUS_LOST) {
                focused = false;
                clear_host_input();
                if (nes_sys.cart) paused = true;
                runtime_reset_pending = true;
            } else if (event.window.event == HOST_WINDOWEVENT_FOCUS_GAINED) {
                focused = true;
                clear_host_input();
            } else if (event.window.event == HOST_WINDOWEVENT_LEAVE) {
                nes_sys.zapper_x = nes_sys.zapper_y = -1;
                nes_sys.zapper_light = false;
            }
        } else if (event.type == HOST_MOUSEBUTTONDOWN) {
            if (focused && !debugger_active &&
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
                controller_button_mappings[control_selection - 1] = event.cbutton.button;
                save_emulator_settings();
                rebinding = false;
                continue;
            }
            if (nes_sys.cart && !debugger_active && !paused && !desktop_menu.active && !help_page && !file_browser.active) {
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
            if (nes_sys.cart && !debugger_active && !paused && !desktop_menu.active && !help_page && !file_browser.active) {
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
            if (!rebinding && event.key.keysym.sym == HOST_KEY_F3) {
                debug_panel_enabled = !debug_panel_enabled;
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
                    control_mappings[control_selection - 1] = event.key.keysym.sym;
                    save_emulator_settings();
                }
                rebinding = false;
                break;
            }

            if (event.key.keysym.sym == HOST_KEY_ESCAPE || event.key.keysym.sym == HOST_KEY_F1) {
                if (nes_sys.cart) desktop_command(MENU_PAUSE);
                else desktop_command(MENU_OPEN);
                continue;
            }
            if (event.key.keysym.sym == 'o' && (event.key.keysym.mod & HOST_MOD_CTRL)) {
                desktop_command(MENU_OPEN); continue;
            }
            if (nes_sys.cart) {
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
                    case HOST_KEY_F10:
                        if (event.key.keysym.mod & HOST_MOD_SHIFT) desktop_command(MENU_STEP);
                        break;
                    case HOST_KEY_F9: desktop_command(MENU_RUN); break;
                    case HOST_KEY_F12:
                        if (debugger_active) desktop_command(MENU_RESET);
                        break;
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

    if (!nes_sys.cart || debugger_active || paused || desktop_menu.active || help_page || file_browser.active || !focused)
        clear_host_input();
    nes_sys.controller_state[0] = host_input_value(&host_input);
    if (nes_sys.cart && focused && !debugger_active && !paused && !desktop_menu.active && !help_page && !file_browser.active && nes_sys.zapper_enabled) {
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
    int cursor_x, cursor_y;
    float game_x, game_y;
    host_mouse_position(&cursor_x, &cursor_y);
    host_to_logical(renderer, cursor_x, cursor_y, &game_x, &game_y);
    bool can_hide_cursor = !desktop_menu.active && !help_page && !file_browser.active &&
        nes_sys.cart != NULL && !debugger_active && !nes_sys.zapper_enabled &&
        game_x >= 0 && game_x < 256 && game_y >= 0 && game_y < 240 &&
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
