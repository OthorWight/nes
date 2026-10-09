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

/* Deterministic music illustration and portable readback of the real panel
   drawing code. Restore all emulation/observer state before continuing. */
static void capture_apu_panel(void) {
    APU2A03 saved_apu = nes_sys.apu;
    ApuView saved_view = apu_view;
    ExpansionAudio saved_expansion = nes_sys.expansion;
    uint16_t saved_mapper = nes_sys.cart->mapper_id;
    uint64_t saved_cycle = nes_sys.cpu.cycle_count;
    // Capture the worst-case layout: all eight N163 voices must fit inside
    // the actual cropped texture, alongside the five standard channels.
    nes_sys.cart->mapper_id = 19;
    memset(&nes_sys.expansion, 0, sizeof(nes_sys.expansion));
    memset(&apu_view, 0, sizeof(apu_view));
    apu_init(&nes_sys.apu);
    APU2A03 *a = &nes_sys.apu;
    static const int melody[] = {60, 64, 67, 72, 71, 67, 64, 62, 65, 69, 72, 77, 76, 72, 67, 64};
    for (unsigned i = 0; i < APU_VIEW_HISTORY; ++i) {
        unsigned beat = i / 120;
        for (unsigned ch = 0; ch < 2; ++ch) {
            int note = ch ? melody[(beat / 2) % 16] - 12 : melody[beat % 16];
            double hz = 440 * pow(2, (note - 69) / 12.0);
            a->pulse_timer_reload[ch] = (uint16_t)lround(1789773 / (16 * hz) - 1);
            a->pulse_sweep_shift[ch] = 1;
            a->pulse_length_counter[ch] = i % 120 < 108 ? 10 : 0;
            a->pulse_constant_volume[ch] = true;
            a->pulse_volume[ch] = (uint8_t)(15 - (i % 120) / 12);
            a->pulse_duty[ch] = ch + 1;
        }
        double bass_hz = 440 * pow(2, ((beat % 4 < 2 ? 48 : 43) - 69) / 12.0);
        a->triangle_enabled = true;
        a->triangle_length_counter = a->triangle_linear_counter = 10;
        a->triangle_timer_reload = (uint16_t)lround(1789773 / (32 * bass_hz) - 1);
        a->noise_enabled = true; a->noise_length_counter = i % 60 < 24 ? 10 : 0;
        a->noise_envelope_decay = (uint8_t)(15 - (i % 60) / 4);
        a->dmc_value = i % 240 < 45 ? (uint8_t)(100 - i % 45) : 20;
        a->dmc_silent = i % 240 >= 45;
        for (unsigned ch = 0; ch < 8; ++ch) {
            unsigned base = 0x78 - ch * 8;
            double hz = 440 * pow(2, (melody[beat % 16] - 12 + (int)ch * 2 - 69) / 12.0);
            uint32_t frequency = (uint32_t)lround(hz * 15 * 8 * 65536 * 32 / 1789773);
            uint8_t *ram = nes_sys.expansion.n163.ram;
            ram[base] = (uint8_t)frequency;
            ram[base + 2] = (uint8_t)(frequency >> 8);
            ram[base + 4] = 0xE0 | ((frequency >> 16) & 3);
            ram[base + 7] = (ch ? 0 : 0x70) | (15 - ch);
        }
        nes_sys.cpu.cycle_count = ((uint64_t)i * 1789773 + 239) / 240;
        apu_view_sample_nes(&apu_view, &nes_sys);
    }
    draw_apu_panel();
    FILE *f = fopen("apu-piano-roll.ppm", "wb"); assert(f);
    fprintf(f, "P6\n%d %d\n255\n", HOST_PANEL_WIDTH, HOST_APU_HEIGHT);
    for (int i = 0; i < HOST_PANEL_WIDTH * HOST_APU_HEIGHT; ++i) {
        uint32_t pixel = apu_canvas.pixels[i];
        unsigned char rgb[] = {pixel & 255, (pixel >> 8) & 255, (pixel >> 16) & 255};
        assert(fwrite(rgb, 1, 3, f) == 3);
    }
    assert(!fclose(f));
    nes_sys.apu = saved_apu;
    nes_sys.expansion = saved_expansion;
    nes_sys.cart->mapper_id = saved_mapper;
    nes_sys.cpu.cycle_count = saved_cycle;
    apu_view = saved_view;
}


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

static void rom_history_persistence(void) {
    char original[BROWSER_PATH], folder[BROWSER_PATH], paths[5][BROWSER_PATH], normalized[BROWSER_PATH];
    snprintf(original, sizeof(original), "%s", recent_roms[0]);
    assert(absolute_rom_path("rom folder", folder));
    assert(!MKDIR(folder));
    FILE *f = fopen(original, "rb"); assert(f);
    uint8_t image[16 + 32768];
    assert(fread(image, 1, sizeof(image), f) == sizeof(image));
    assert(!fclose(f));
    for (unsigned i = 0; i < 5; ++i) {
        int length = snprintf(paths[i], sizeof(paths[i]), "%s/game%u.nes", folder, i);
        assert(length > 0 && length < BROWSER_PATH);
        assert(absolute_rom_path(paths[i], normalized));
        snprintf(paths[i], sizeof(paths[i]), "%s", normalized);
        f = fopen(paths[i], "wb"); assert(f);
        assert(fwrite(image, 1, sizeof(image), f) == sizeof(image));
        assert(!fclose(f));
        assert(frontend_load_rom(paths[i]));
    }
    assert(recent_count == 4 && !strcmp(recent_roms[0], paths[4]));
    assert(!strcmp(recent_roms[3], paths[1]));
    assert(frontend_load_rom(paths[2]));
    assert(recent_count == 4 && !strcmp(recent_roms[0], paths[2]));
    assert(!strcmp(recent_roms[1], paths[4]) && !strcmp(recent_roms[2], paths[3]));

    /* Clear session state, then restore it solely from the saved file. */
    memset(recent_roms, 0, sizeof(recent_roms)); recent_count = 0;
    last_rom_directory[0] = 0;
    memset(&file_browser, 0, sizeof(file_browser));
    load_emulator_settings(); apply_rom_preferences();
    assert(recent_count == 4 && !strcmp(recent_roms[0], paths[2]));
    assert(!strcmp(recent_roms[3], paths[1]) && !strcmp(recent_labels[0], "game2.nes"));
    assert(!(desktop_state(NULL, MENU_RECENT_4) & MENU_DISABLED));
    desktop_command(MENU_RECENT_2);
    assert(nes_sys.cart && !strcmp(loaded_rom_path, recent_roms[0]));
    assert(!strcmp(recent_labels[0], "game4.nes"));

    /* The state browser must not change the saved ROM folder. */
    desktop_command(MENU_SAVE_AS);
    HostEvent escape = {.type = HOST_KEYDOWN, .key.keysym.sym = HOST_KEY_ESCAPE};
    assert(desktop_event(&escape));
    desktop_command(MENU_OPEN);
    assert(absolute_rom_path(file_browser.path, normalized));
    assert(!strcmp(normalized, folder));
    /* Navigating and cancelling also remembers the new ROM folder. */
    assert(file_browser_scan(&file_browser, save_state_dir));
    assert(desktop_event(&escape));
    assert(absolute_rom_path(save_state_dir, normalized));
    load_emulator_settings(); apply_rom_preferences();
    assert(!strcmp(last_rom_directory, normalized));
    desktop_command(MENU_OPEN);
    assert(absolute_rom_path(file_browser.path, normalized));
    assert(!strcmp(last_rom_directory, normalized));
    assert(desktop_event(&escape));

    /* A failed ROM load must not enter history. */
    char missing[BROWSER_PATH];
    assert(absolute_rom_path("missing-history-rom.nes", missing));
    assert(!frontend_load_rom(missing));
    load_emulator_settings();
    assert(recent_count == 4 && !strcmp(recent_roms[0], paths[4]));

    char settings[1024]; get_settings_filepath(settings, sizeof(settings));
    f = fopen(settings, "r+b"); assert(f);
    assert(!fseek(f, -1, SEEK_END));
    int byte = fgetc(f); assert(byte != EOF);
    assert(!fseek(f, -1, SEEK_END));
    assert(fputc(byte ^ 1, f) != EOF); assert(!fclose(f));
    load_emulator_settings();
    assert(!recent_count && !*last_rom_directory);
    assert(!strcmp(recent_labels[0], "(Empty)"));

    uint8_t legacy[128];
    f = fopen(settings, "rb"); assert(f);
    assert(fread(legacy, 1, sizeof(legacy), f) == sizeof(legacy)); assert(!fclose(f));
    /* A truncated version 10 history is also discarded. */
    assert(state_atomic_write(settings, legacy, sizeof(legacy)));
    load_emulator_settings();
    assert(!recent_count && !*last_rom_directory);

    /* Version 9 keeps its preferences and starts with empty history. */
    legacy[0] = 9;
    int volume = master_volume;
    assert(state_atomic_write(settings, legacy, sizeof(legacy)));
    load_emulator_settings();
    assert(master_volume == volume && !recent_count && !*last_rom_directory);

    /* An unavailable saved directory falls back to a usable browser. */
    for (unsigned i = 0; i < 5; ++i) assert(!remove(paths[i]));
#ifdef _WIN32
    assert(!_rmdir(folder));
#else
    assert(!rmdir(folder));
#endif
    snprintf(last_rom_directory, sizeof(last_rom_directory), "%s", folder);
    save_emulator_settings(); load_emulator_settings();
    desktop_command(MENU_OPEN);
    assert(file_browser.active && !*file_browser.error);
    assert(desktop_event(&escape));
    assert(frontend_load_rom(original));
    assert(recent_count == 1 && !strcmp(recent_labels[0], "fixture.nes"));
}

static void capture_debugger_panel(const char *path) {
    FILE *f = fopen(path, "wb"); assert(f);
    fprintf(f, "P6\n%d %d\n255\n", HOST_PANEL_WIDTH, HOST_PANEL_HEIGHT);
    for (int i = 0; i < HOST_PANEL_WIDTH * HOST_PANEL_HEIGHT; ++i) {
        uint32_t pixel = debug_canvas.pixels[i];
        unsigned char rgb[] = {pixel & 255, (pixel >> 8) & 255, (pixel >> 16) & 255};
        assert(fwrite(rgb, 1, 3, f) == 3);
    }
    assert(!fclose(f));
}
static void debugger_workspace_checks(void) {
    execution_sync(nes_sys.execution);
    uint8_t *saved; size_t saved_size;
    assert(nes_state_encode(&nes_sys, &saved, &saved_size) == NES_STATE_OK);
    desktop_command(MENU_STEP); assert(debugger_active);
    uint64_t dots = execution_status(nes_sys.execution).dots;
    desktop_command(MENU_STEP_DOT);
    assert(execution_status(nes_sys.execution).dots == dots + 1);
    assert(!execution_status(nes_sys.execution).boundary);
    desktop_command(MENU_DEBUG_FINISH); assert(execution_status(nes_sys.execution).boundary);
    assert(debugger_command("label $0010 counter"));
    assert(debugger_command("poke counter $A5")); assert(nes_sys.wram[0x10] == 0xA5);
    assert(debugger_command("watch counter")); assert(debugger_command("mem counter"));
    uint8_t shift = nes_sys.controller_shift[0];
    draw_debug_panel(); desktop_present(renderer);
    assert(nes_sys.controller_shift[0] == shift);
    capture_window("debugger-memory.bmp");
    capture_debugger_panel("debugger-memory.ppm");
    assert(debugger_command("vram $3F00"));
    uint16_t bus = nes_sys.ppu.bus_address;
    draw_debug_panel(); assert(nes_sys.ppu.bus_address == bus);
    assert(debugger_command("view ppu")); draw_debug_panel();
    assert(nes_sys.ppu.bus_address == bus);
    HostEvent oam_page={.type=HOST_KEYDOWN,.key.keysym.sym=HOST_KEY_PAGEDOWN};
    for(unsigned i=0;i<6;++i) assert(debugger_event(&oam_page,-1,-1));
    draw_debug_panel(); assert(nes_sys.ppu.bus_address == bus);
    capture_debugger_panel("debugger-ppu.ppm");
    assert(debugger_command("wp rw $0010-$001F value=$A5 ignore=2 once"));
    ExecutionBreakpoint *b = execution_breakpoints(nes_sys.execution);
    assert(b[0].used && b[0].first == 0x10 && b[0].last == 0x1F && b[0].ignore == 2 && b[0].once);
    assert(debugger_command("disable 0") && !b[0].enabled);
    assert(debugger_command("enable 0") && b[0].enabled);
    assert(!debugger_command("wp rw $FF00-$0010"));
    assert(!debugger_command("set imaginary $FF"));
    assert(debugger_command("delete 0") && !b[0].used);
    /* Keyboard code breakpoints must use the structured entry: the legacy
       boolean path would bypass conditions, hit counters, and one-shot state. */
    debugger_toggle_breakpoint(0x0200);
    assert(b[0].used && b[0].enabled && !breakpoints[0x0200]);
    assert(debugger_command("delete 0"));
    for(unsigned i=0;i<EXEC_BREAKPOINTS;++i) {
        char command[48]; snprintf(command,sizeof(command),"bp $%04X",0x0200+i);
        assert(debugger_command(command));
    }
    draw_debug_panel();
    HostEvent page={.type=HOST_KEYDOWN,.key.keysym.sym=HOST_KEY_PAGEDOWN};
    for(unsigned i=0;i<3;++i) assert(debugger_event(&page,-1,-1));
    HostEvent entry={.type=HOST_MOUSEBUTTONDOWN,.button.button=HOST_BUTTON_LEFT};
    assert(debugger_event(&entry,16,334) && debugger_console_active());
    HostEvent enter={.type=HOST_KEYDOWN,.key.keysym.sym=HOST_KEY_RETURN};
    assert(debugger_event(&enter,-1,-1) && !b[24].enabled && b[23].enabled);
    assert(debugger_command("clear"));
    assert(debugger_command("trace \"debugger trace.csv\""));
    assert(!remove("debugger trace.csv"));
    assert(debugger_command("step instruction 3"));
    while (execution_status(nes_sys.execution).pending) debugger_update();
    uint64_t cycle = nes_sys.cpu.cycle_count;
    assert(debugger_command("back instruction")); assert(nes_sys.cpu.cycle_count < cycle);
    assert(debugger_command("step instruction 2"));
    while(execution_status(nes_sys.execution).pending) debugger_update();
    cycle=nes_sys.cpu.cycle_count;
    HostEvent alt={.type=HOST_KEYDOWN,.key.keysym.sym=HOST_KEY_LALT};
    assert(desktop_event(&alt) && desktop_menu.active);
    HostEvent reverse={.type=HOST_KEYDOWN,.key.keysym={HOST_KEY_F10,HOST_MOD_ALT}};
    assert(desktop_event(&reverse) && !desktop_menu.active && nes_sys.cpu.cycle_count<cycle);
    assert(debugger_command("help")); draw_debug_panel(); desktop_present(renderer);
    capture_window("debugger-help.bmp");
    capture_debugger_panel("debugger-help.ppm");
    desktop_command(MENU_DEBUG_COMMAND); assert(debugger_console_active());
    HostEvent escape = {.type = HOST_KEYDOWN, .key.keysym.sym = HOST_KEY_ESCAPE};
    assert(desktop_event(&escape) && !debugger_console_active());
    /* Exercise real panel hit testing at its rendered screen coordinates. */
    draw_debug_panel();
    HostRect game, panel; host_layout(sapp_width(), sapp_height(), &game, &panel);
    HostEvent click = {.type=HOST_MOUSEBUTTONDOWN,.button.button=HOST_BUTTON_LEFT};
    click.window_mouse.valid = true;
    click.window_mouse.x = panel.x + 12 * panel.w / HOST_PANEL_WIDTH;
    click.window_mouse.y = panel.y + 255 * panel.h / HOST_PANEL_HEIGHT;
    cycle = nes_sys.cpu.cycle_count;
    assert(desktop_event(&click)); assert(nes_sys.cpu.cycle_count > cycle);
    assert(execution_sync(nes_sys.execution));
    assert(nes_state_decode(&nes_sys, saved, saved_size) == NES_STATE_OK); free(saved);
    execution_invalidate(nes_sys.execution);
    debugger_active = false; runtime_reset_pending = true;
    assert(debugger_command("unwatch"));
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
            desktop_command(MENU_OPEN);
            for (int i = 0; i < file_browser.count; ++i)
                if (!strcmp(file_browser.entries[i].name, "fixture.nes")) file_browser.selected = i;
            key(e, HOST_KEYDOWN, HOST_KEY_RETURN); break;
        case 1:
            assert(nes_sys.cart && !paused);
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
            assert(paused);
            if (audio_device) assert(host_audio_queued_bytes() == 0);
            key(e, HOST_KEYUP, HOST_KEY_z); break;
        case 8: e->type = HOST_WINDOWEVENT; e->window.event = HOST_WINDOWEVENT_FOCUS_GAINED; break;
        case 9: key(e, HOST_KEYDOWN, HOST_KEY_ESCAPE); break;
        case 10: assert(nes_sys.controller_state[0] == 0); key(e, HOST_KEYDOWN, HOST_KEY_x); break;
        case 11: assert(nes_sys.controller_state[0] & 2); key(e, HOST_KEYDOWN, HOST_KEY_F10); break;
        case 12: {
            assert(debugger_active && !desktop_menu.active && nes_sys.controller_state[0] == 0);
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
            desktop_command(MENU_PAUSE); assert(paused);
            desktop_command(MENU_PAUSE); assert(!paused);
            desktop_command(MENU_STEP); assert(debugger_active);
            desktop_command(MENU_RUN); assert(!debugger_active);
            desktop_command(MENU_SAVE);
            uint8_t saved_byte = nes_sys.wram[20];
            nes_sys.wram[20] ^= 0xFF;
            desktop_command(MENU_LOAD); assert(nes_sys.wram[20] == saved_byte);
            desktop_command(MENU_RESET); assert(nes_sys.cart && !nes_sys.zapper_trigger);
            nes_sys.wram[20] = 0xA5;
            desktop_command(MENU_POWER); assert(nes_sys.cart && nes_sys.wram[20] == 0);
            assert(recent_count == 1 && !strcmp(loaded_rom_name, "fixture.nes"));
            desktop_command(MENU_RECENT_1); assert(nes_sys.cart && recent_count == 1);
            rom_history_persistence();
            debugger_workspace_checks();
            break;
        case 17: key(e, HOST_KEYDOWN, HOST_KEY_LEFT); break;
        case 18:
            assert(nes_sys.controller_state[0] & 0x40);
            key(e, HOST_KEYDOWN, HOST_KEY_ESCAPE); break;
        case 19:
            assert(paused && !nes_sys.controller_state[0]);
            assert(renderer->pixels[108 * 256 + 96] == 0xDC000000u);
            capture_window("paused.bmp");
            key(e, HOST_KEYUP, HOST_KEY_LEFT); break;
        case 20:
            memset(&diagnostics, 0, sizeof(diagnostics)); diagnostics.tracing = true;
            memset(&audio_monitor, 0, sizeof(audio_monitor));
            runtime_reset_pending = true;
            key(e, HOST_KEYDOWN, HOST_KEY_ESCAPE); break;
        case 21:
            assert(!paused && !nes_sys.zapper_trigger);
            assert(renderer->pixels[108 * 256 + 96] == 0); /* OSD clears on resume. */
            key(e, HOST_KEYDOWN, HOST_KEY_F3);
            break;
        case 22:
            assert(debug_panel_enabled && !debugger_active && renderer->has_frame);
            key(e, HOST_KEYDOWN, HOST_KEY_F3);
            break;
        case 23:
            assert(!debug_panel_enabled && performance_visible && renderer->has_frame);
            desktop_command(MENU_NAMETABLES);
            assert(nametable_viewer_enabled && (desktop_state(NULL, MENU_NAMETABLES) & MENU_CHECKED));
            nametable_viewer_enabled = false; load_emulator_settings();
            assert(nametable_viewer_enabled);
            break;
        case 24: {
            assert(!paused && !debugger_active && renderer->has_frame);
            assert(nametable_view.valid && nametable_view.sampled);
            HostRect game, panel, nt;
            host_layout(sapp_width(), sapp_height(), &game, &panel);
            host_nametable_layout(sapp_width(), sapp_height(), &nt);
            assert(game.w > 0 && panel.w > 0 && nt.w > 0);
            assert(game.x + game.w <= panel.x && panel.x + panel.w <= nt.x);
            assert(abs(nt.w * HOST_NAMETABLE_HEIGHT - nt.h * HOST_PANEL_WIDTH) < HOST_PANEL_HEIGHT);
            float x, y;
            host_to_logical(renderer, game.x + game.w / 2, game.y + game.h / 2, &x, &y);
            assert(fabsf(x - 128) < 1 && fabsf(y - 120) < 1);
            host_to_logical(renderer, nt.x + nt.w / 2, nt.y + nt.h / 2, &x, &y);
            assert(x > 256);
            capture_window("nametable-viewer.bmp");
            key(e, HOST_KEYDOWN, HOST_KEY_RIGHT);
            break;
        }
        case 25:
            assert(nes_sys.controller_state[0] & 0x80);
            key(e, HOST_KEYUP, HOST_KEY_RIGHT);
            break;
        case 26:
            assert(!nes_sys.controller_state[0]);
            desktop_command(MENU_APU_VIEWER);
            assert(apu_viewer_enabled && (desktop_state(NULL, MENU_APU_VIEWER) & MENU_CHECKED));
            apu_viewer_enabled = false; load_emulator_settings();
            assert(apu_viewer_enabled);
            break;
        case 27: {
            HostRect game, debug, nt, apu;
            host_layout(sapp_width(), sapp_height(), &game, &debug);
            host_nametable_layout(sapp_width(), sapp_height(), &nt);
            host_apu_layout(sapp_width(), sapp_height(), &apu);
            assert(apu.w > 0 && apu.h > 0 && apu_view.count > 0);
            assert(game.x + game.w <= debug.x && debug.x + debug.w <= nt.x);
            assert(nt.x + nt.w <= apu.x && apu.x + apu.w <= sapp_width());
            HostEvent click = {0}; click.type = HOST_MOUSEBUTTONDOWN;
            click.window_mouse.valid = true;
            click.window_mouse.x = apu.x + apu.w / 2;
            click.window_mouse.y = apu.y + apu.h / 2;
            assert(desktop_event(&click));
            click.button.button = HOST_BUTTON_LEFT;
            click.window_mouse.x = apu.x + (APU_VIEW_NAME_X + 16) * apu.w / HOST_PANEL_WIDTH;
            click.window_mouse.y = apu.y + (APU_VIEW_ROWS_Y + 4) * apu.h / HOST_APU_HEIGHT;
            uint16_t muted_before = nes_sys.audio_muted_channels;
            uint8_t controller_before = nes_sys.controller_state[0];
            bool trigger_before = nes_sys.zapper_trigger;
            assert(desktop_event(&click));
            assert(nes_sys.audio_muted_channels == (muted_before ^ 1));
            assert(nes_sys.controller_state[0] == controller_before && nes_sys.zapper_trigger == trigger_before);
            assert(desktop_event(&click));
            assert(nes_sys.audio_muted_channels == muted_before);
            unsigned count = apu_view.count;
            paused = true; desktop_present(renderer);
            assert(apu_view.count == count);
            paused = false;
            capture_apu_panel();
            capture_window("apu-viewer.bmp");
            /* Both viewers remain open during the audio/pacing checks. */
            break;
        }
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
