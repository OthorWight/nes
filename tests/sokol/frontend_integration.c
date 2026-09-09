// This optional suite exercises the actual Sokol frontend with synthetic events.

#include "../../src/host.h"
#include <assert.h>
static bool scripted_poll(HostEvent *event);
#define host_poll_event scripted_poll
#define sokol_main emulator_desc
#include "../../src/gui_main.c"
#undef sokol_main
#undef host_poll_event
#include "capture.h"

static unsigned iteration;
static unsigned capture_iteration = 140;
static unsigned reported_empty, reported_trims;
static bool delivered, muted_test, unavailable_test;
static int pad_id = 42;


static void key(HostEvent *e, uint32_t type, HostKey sym) {
    e->type = type; e->key.keysym.sym = sym;
}
static void display_and_preferences(void) {
    global_preferences = (RomPreferences){2, false, false};
    window_scale = 3; fullscreen = false; zapper_enabled = true;
    preferences_inherit = false;
    save_emulator_settings();
    load_emulator_settings();
    assert(!zapper_enabled && window_scale == 2);
    apply_rom_preferences();
    assert(rom_override && zapper_enabled && window_scale == 3);
    uint32_t identity = nes_sys.cart->rom_identity[0];
    nes_sys.cart->rom_identity[0] ^= 1;
    apply_rom_preferences();
    assert(!rom_override && !zapper_enabled && window_scale == 2);
    nes_sys.cart->rom_identity[0] = identity;
    apply_rom_preferences();
    assert(rom_override && zapper_enabled && window_scale == 3);
    preferences_inherit = true; save_emulator_settings(); apply_rom_preferences();
    assert(!rom_override && !zapper_enabled && window_scale == 2);

}
static bool scripted_poll(HostEvent *e) {
    if (delivered) { delivered = false; ++iteration; return 0; }
    delivered = true; host_zero(*e);
    if (iteration > 21 && (audio_monitor.underruns != reported_empty ||
                           audio_monitor.trims != reported_trims)) {
        DiagnosticSummary summary = diagnostics_summary(&diagnostics);
        fprintf(stderr, "Audio event at frame %u: empty=%u trims=%u max=%.2fms speed=%.2f%% filtered=%.0f\n",
                iteration, audio_monitor.underruns, audio_monitor.trims,
                summary.max_ms, summary.speed, audio_monitor.filtered_queue);
        (void)diagnostics_write(&nes_sys, "audio_event.log", "Sokol fixture",
            audio_monitor.underruns, audio_monitor.trims, audio_monitor.errors, audio_device_ms);
        reported_empty = audio_monitor.underruns; reported_trims = audio_monitor.trims;
    }
    if (iteration == capture_iteration) {
        DiagnosticSummary summary = diagnostics_summary(&diagnostics);
        printf("Sokol mode=%s FPS=%.2f speed=%.2f%% queue=%.2fms empty=%u trims=%u\n",
            unavailable_test ? "unavailable" : (muted_test ? "muted" : "audio"),
            summary.fps, summary.speed, summary.queue_ms, audio_monitor.underruns, audio_monitor.trims);
        fflush(stdout);
        assert(summary.speed > 95 && summary.speed < 105);
        assert(!audio_monitor.errors);
        if (muted_test || unavailable_test) assert(audio_monitor.queue_samples == 0);
        else {
            assert(audio_monitor.queue_samples <= AUDIO_MAX_SAMPLES);
            assert(audio_monitor.underruns == 0 && audio_monitor.trims == 0);
        }
        key(e, HOST_KEYDOWN, HOST_KEY_F4);
        return 1;
    }
    if (iteration == capture_iteration + 1) {
        assert(!strcmp(notification_text, "DIAGNOSTICS CAPTURED"));
        e->type = HOST_QUIT;
        return 1;
    }
    switch (iteration) {
        case 0:
            audio_muted = muted_test;
            if (unavailable_test) assert(!audio_device);
            else assert(audio_device);
            current_state = GUI_STATE_MENU_LOAD_ROM;
            menu_selection = 0;
            e->type = HOST_MOUSEBUTTONDOWN; e->button.button = HOST_BUTTON_LEFT;
            e->button.x = 40; e->button.y = 70; break;
        case 1:
            assert(nes_sys.cart && current_state == GUI_STATE_GAMEPLAY);
            game_controller = pad_id;
            key(e, HOST_KEYDOWN, HOST_KEY_z); break;
        case 2:
            assert(nes_sys.controller_state[0] & 1);
            e->type = HOST_CONTROLLERBUTTONDOWN; e->cbutton.which = pad_id;
            e->cbutton.button = HOST_CONTROLLER_BUTTON_A; break;
        case 3: key(e, HOST_KEYUP, HOST_KEY_z); break;
        case 4:
            assert(nes_sys.controller_state[0] & 1);

            e->type = HOST_CONTROLLERDEVICEREMOVED; e->cdevice.which = pad_id; break;
        case 5:
            assert(nes_sys.controller_state[0] == 0);
            key(e, HOST_KEYDOWN, HOST_KEY_z); break;
        case 6:
            assert(nes_sys.controller_state[0] & 1);
            nes_sys.zapper_trigger = true;
            e->type = HOST_WINDOWEVENT; e->window.event = HOST_WINDOWEVENT_FOCUS_LOST; break;
        case 7:
            assert(!focused && !nes_sys.controller_state[0] && !nes_sys.zapper_trigger);
            assert(current_state == GUI_STATE_MENU_MAIN);
            if (audio_device) assert(host_audio_queued_bytes() == 0);
            key(e, HOST_KEYUP, HOST_KEY_z); break;
        case 8: e->type = HOST_WINDOWEVENT; e->window.event = HOST_WINDOWEVENT_FOCUS_GAINED; break;
        case 9: key(e, HOST_KEYDOWN, HOST_KEY_RETURN); break;
        case 10: assert(nes_sys.controller_state[0] == 0); key(e, HOST_KEYDOWN, HOST_KEY_x); break;
        case 11: assert(nes_sys.controller_state[0] & 2); key(e, HOST_KEYDOWN, HOST_KEY_F10); break;
        case 12: {
            assert(debugger_active && nes_sys.controller_state[0] == 0);
            assert(renderer->has_frame); /* Stepping retains the game beside the debugger. */
            HostRect game, panel; host_layout(sapp_width(), sapp_height(), &game, &panel);
            assert(panel.w > 0 && game.x + game.w <= panel.x);
            capture_window("debug-panel.bmp");
            if (audio_device) assert(host_audio_queued_bytes() == 0);
            uint64_t cycle = nes_sys.cpu.cycle_count;
            int dot = nes_sys.ppu.scanline * 341 + nes_sys.ppu.cycle;
            debugger_step_instruction(&nes_sys.cpu, &cpu_bus_bridge);
            assert(nes_sys.cpu.cycle_count - cycle == 3);
            assert(nes_sys.ppu.scanline * 341 + nes_sys.ppu.cycle - dot == 9);
            key(e, HOST_KEYUP, HOST_KEY_x); break;
        }
        case 13: key(e, HOST_KEYDOWN, HOST_KEY_F9); break;
        case 14:
            assert(!debugger_active && nes_sys.controller_state[0] == 0);
            display_and_preferences();
            key(e, HOST_KEYDOWN, HOST_KEY_F2); break;
        case 15: assert(performance_visible); key(e, HOST_KEYDOWN, HOST_KEY_F4); e->key.keysym.mod = HOST_MOD_CTRL; break;
        case 16:
            assert(diagnostics.tracing);
            // Disassembling an I/O operand must display unknown and leave it alone.
            nes_sys.wram[0x600] = 0xAD; nes_sys.wram[0x601] = 0x16; nes_sys.wram[0x602] = 0x40;
            nes_sys.controller_shift[0] = 0x81;
            char text[128]; disassemble_instruction(0x600, text, sizeof(text), &nes_sys.cpu);
            assert(strstr(text, "#$??") && nes_sys.controller_shift[0] == 0x81);
            disassemble_instruction(0x4016, text, sizeof(text), &nes_sys.cpu);
            assert(strstr(text, "UNKNOWN"));
            nes_sys.zapper_enabled = true;
            save_emulator_state(save_state_dir, "frontend.state");
            nes_sys.controller_state[0] = 0xFF;
            nes_sys.zapper_trigger = true;
            load_emulator_state(save_state_dir, "frontend.state");
            assert(!nes_sys.zapper_enabled && !nes_sys.controller_state[0] && !nes_sys.zapper_trigger);
            break;
        case 17: key(e, HOST_KEYDOWN, HOST_KEY_LEFT); break;
        case 18:
            assert(nes_sys.controller_state[0] & 0x40);
            e->type = HOST_MOUSEBUTTONDOWN; e->button.button = HOST_BUTTON_RIGHT; break;
        case 19:
            assert(current_state == GUI_STATE_MENU_MAIN && !nes_sys.controller_state[0]);
            key(e, HOST_KEYUP, HOST_KEY_LEFT); break;
        case 20:
            memset(&diagnostics, 0, sizeof(diagnostics)); diagnostics.tracing = true;
            memset(&audio_monitor, 0, sizeof(audio_monitor));
            runtime_reset_pending = true;
            e->type = HOST_MOUSEBUTTONDOWN; e->button.button = HOST_BUTTON_LEFT;
            e->button.x = 100; e->button.y = 62; break;
        case 21:
            assert(current_state == GUI_STATE_GAMEPLAY && !nes_sys.zapper_trigger);
            key(e, HOST_KEYDOWN, HOST_KEY_F3);
            break;
        case 22:
            assert(debug_panel_enabled && !debugger_active && renderer->has_frame);
            key(e, HOST_KEYDOWN, HOST_KEY_F3);
            break;
        case 23:
            assert(!debug_panel_enabled && performance_visible && renderer->has_frame);
            break;
        default: assert(iteration < capture_iteration); break;
    }
    return 1;
}
sapp_desc sokol_main(int argc, char **argv) {
    muted_test = argc > 1 && !strcmp(argv[1], "muted");
    unavailable_test = argc > 1 && !strcmp(argv[1], "unavailable");
    const char *audio_frames = getenv("NES_SOKOL_AUDIO_FRAMES");
    if (audio_frames && !muted_test && !unavailable_test) {
        char *end;
        unsigned long frames = strtoul(audio_frames, &end, 10);
        assert(*audio_frames && !*end && frames >= 120 && frames <= 36000);
        capture_iteration = 20 + (unsigned)frames;
    }
    FILE *f = fopen("fixture.nes", "wb"); assert(f);
    uint8_t header[16] = {'N','E','S',0x1A,2,0};
    assert(fwrite(header, 1, 16, f) == 16);
    uint8_t rom[32768]; memset(rom, 0xEA, sizeof(rom));
    rom[0] = 0x4C; rom[1] = 0; rom[2] = 0x80;
    for (unsigned i = 32762; i < 32768; i += 2) { rom[i] = 0; rom[i + 1] = 0x80; }
    assert(fwrite(rom, 1, sizeof(rom), f) == sizeof(rom)); assert(!fclose(f));
    sapp_desc desc = emulator_desc(argc, argv);
    desc.event_cb = NULL;
    desc.width = 768; desc.height = 720;
    window_scale = 3;
    return desc;
}
