// This optional suite exercises the actual SDL frontend with synthetic events.
#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#include <assert.h>
static int scripted_poll(SDL_Event *event);
#define SDL_PollEvent scripted_poll
#define main emulator_main
#include "../../src/gui_main.c"
#undef main
#undef SDL_PollEvent

static unsigned iteration;
static bool delivered, muted_test, unavailable_test;
static SDL_JoystickID pad_id;
static int virtual_pad;

static void key(SDL_Event *e, Uint32 type, SDL_Keycode sym) {
    e->type = type; e->key.keysym.sym = sym;
}
static void display_and_preferences(void) {
    SDL_Window *w = SDL_CreateWindow("test", 0, 0, 256, 240, SDL_WINDOW_HIDDEN | SDL_WINDOW_RESIZABLE);
    assert(w);
    SDL_Renderer *r = SDL_CreateRenderer(w, -1, SDL_RENDERER_SOFTWARE); assert(r);
    assert(SDL_RenderSetLogicalSize(r, 256, 240) == 0);
    const int sizes[][2] = {{256,240}, {768,720}, {1920,1080}, {1200,1600}, {1537,901}};
    for (unsigned i = 0; i < 5; ++i) {
        SDL_SetWindowSize(w, sizes[i][0], sizes[i][1]);
        SDL_PumpEvents();
        SDL_RenderPresent(r);
        float lx, ly; int x, y;
        SDL_RenderWindowToLogical(r, sizes[i][0] / 2, sizes[i][1] / 2, &lx, &ly);
        assert(game_aim(game_crop(0), lx, ly, &x, &y));
        assert(abs(x - 128) <= 1 && abs(y - 120) <= 1);
        if (i == 2) {
            SDL_RenderWindowToLogical(r, 100, 540, &lx, &ly);
            assert(!game_aim(game_crop(0), lx, ly, &x, &y));
        }
    }
    for (unsigned mode = 0; mode < 2; ++mode) {
        if (mode == 0) SDL_MaximizeWindow(w);
        else assert(SDL_SetWindowFullscreen(w, SDL_WINDOW_FULLSCREEN_DESKTOP) == 0);
        SDL_PumpEvents();
        int width, height, x, y; float lx, ly;
        SDL_GetWindowSize(w, &width, &height);
        SDL_RenderWindowToLogical(r, width / 2, height / 2, &lx, &ly);
        assert(game_aim(game_crop(1), lx, ly, &x, &y));
        assert(abs(x - 128) <= 1 && abs(y - 120) <= 1);
    }

    global_preferences = (RomPreferences){2, false, false};
    window_scale = 3; fullscreen = false; zapper_enabled = true;
    preferences_inherit = false;
    save_emulator_settings();
    load_emulator_settings();
    assert(!zapper_enabled && window_scale == 2);
    apply_rom_preferences(w);
    assert(rom_override && zapper_enabled && window_scale == 3);
    uint32_t identity = nes_sys.cart->rom_identity[0];
    nes_sys.cart->rom_identity[0] ^= 1;
    apply_rom_preferences(w);
    assert(!rom_override && !zapper_enabled && window_scale == 2);
    nes_sys.cart->rom_identity[0] = identity;
    apply_rom_preferences(w);
    assert(rom_override && zapper_enabled && window_scale == 3);
    preferences_inherit = true; save_emulator_settings(); apply_rom_preferences(w);
    assert(!rom_override && !zapper_enabled && window_scale == 2);
    SDL_DestroyRenderer(r); SDL_DestroyWindow(w);
}
static int scripted_poll(SDL_Event *e) {
    if (delivered) { delivered = false; ++iteration; return 0; }
    delivered = true; SDL_zero(*e);
    switch (iteration) {
        case 0:
            audio_muted = muted_test;
            if (unavailable_test) assert(!audio_device);
            else assert(audio_device);
            current_state = GUI_STATE_MENU_LOAD_ROM;
            menu_selection = 0;
            e->type = SDL_MOUSEBUTTONDOWN; e->button.button = SDL_BUTTON_LEFT;
            e->button.x = 40; e->button.y = 70; break;
        case 1:
            assert(nes_sys.cart && current_state == GUI_STATE_GAMEPLAY);
            virtual_pad = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER, 6, 15, 0);
            assert(virtual_pad >= 0);
            game_controller = SDL_GameControllerOpen(virtual_pad); assert(game_controller);
            pad_id = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(game_controller));
            key(e, SDL_KEYDOWN, SDLK_z); break;
        case 2:
            assert(nes_sys.controller_state[0] & 1);
            e->type = SDL_CONTROLLERBUTTONDOWN; e->cbutton.which = pad_id;
            e->cbutton.button = SDL_CONTROLLER_BUTTON_A; break;
        case 3: key(e, SDL_KEYUP, SDLK_z); break;
        case 4:
            assert(nes_sys.controller_state[0] & 1);
            SDL_JoystickDetachVirtual(virtual_pad);
            e->type = SDL_CONTROLLERDEVICEREMOVED; e->cdevice.which = pad_id; break;
        case 5:
            assert(nes_sys.controller_state[0] == 0);
            key(e, SDL_KEYDOWN, SDLK_z); break;
        case 6:
            assert(nes_sys.controller_state[0] & 1);
            nes_sys.zapper_trigger = true;
            e->type = SDL_WINDOWEVENT; e->window.event = SDL_WINDOWEVENT_FOCUS_LOST; break;
        case 7:
            assert(!focused && !nes_sys.controller_state[0] && !nes_sys.zapper_trigger);
            assert(current_state == GUI_STATE_MENU_MAIN);
            if (audio_device) assert(SDL_GetQueuedAudioSize(audio_device) == 0);
            key(e, SDL_KEYUP, SDLK_z); break;
        case 8: e->type = SDL_WINDOWEVENT; e->window.event = SDL_WINDOWEVENT_FOCUS_GAINED; break;
        case 9: key(e, SDL_KEYDOWN, SDLK_RETURN); break;
        case 10: assert(nes_sys.controller_state[0] == 0); key(e, SDL_KEYDOWN, SDLK_x); break;
        case 11: assert(nes_sys.controller_state[0] & 2); key(e, SDL_KEYDOWN, SDLK_F10); break;
        case 12: {
            assert(debugger_active && nes_sys.controller_state[0] == 0);
            if (audio_device) assert(SDL_GetQueuedAudioSize(audio_device) == 0);
            uint64_t cycle = nes_sys.cpu.cycle_count;
            int dot = nes_sys.ppu.scanline * 341 + nes_sys.ppu.cycle;
            debugger_step_instruction(&nes_sys.cpu, &cpu_bus_bridge);
            assert(nes_sys.cpu.cycle_count - cycle == 3);
            assert(nes_sys.ppu.scanline * 341 + nes_sys.ppu.cycle - dot == 9);
            key(e, SDL_KEYUP, SDLK_x); break;
        }
        case 13: key(e, SDL_KEYDOWN, SDLK_F9); break;
        case 14:
            assert(!debugger_active && nes_sys.controller_state[0] == 0);
            display_and_preferences();
            key(e, SDL_KEYDOWN, SDLK_F2); break;
        case 15: assert(performance_visible); key(e, SDL_KEYDOWN, SDLK_F4); e->key.keysym.mod = KMOD_CTRL; break;
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
        case 17: key(e, SDL_KEYDOWN, SDLK_LEFT); break;
        case 18:
            assert(nes_sys.controller_state[0] & 0x40);
            e->type = SDL_MOUSEBUTTONDOWN; e->button.button = SDL_BUTTON_RIGHT; break;
        case 19:
            assert(current_state == GUI_STATE_MENU_MAIN && !nes_sys.controller_state[0]);
            key(e, SDL_KEYUP, SDLK_LEFT); break;
        case 20:
            memset(&diagnostics, 0, sizeof(diagnostics)); diagnostics.tracing = true;
            memset(&audio_monitor, 0, sizeof(audio_monitor));
            runtime_reset_pending = true;
            e->type = SDL_MOUSEBUTTONDOWN; e->button.button = SDL_BUTTON_LEFT;
            e->button.x = 100; e->button.y = 62; break;
        case 21:
            assert(current_state == GUI_STATE_GAMEPLAY && !nes_sys.zapper_trigger);
            break;
        case 140: {
            DiagnosticSummary summary = diagnostics_summary(&diagnostics);
            printf("SDL mode=%s FPS=%.2f speed=%.2f%% queue=%.2fms empty=%u trims=%u\n",
                unavailable_test ? "unavailable" : (muted_test ? "muted" : "audio"),
                summary.fps, summary.speed, summary.queue_ms, audio_monitor.underruns, audio_monitor.trims);
            assert(summary.speed > 95 && summary.speed < 105);
            assert(!audio_monitor.errors);
            if (muted_test || unavailable_test) assert(audio_monitor.queue_samples == 0);
            else assert(audio_monitor.queue_samples <= AUDIO_MAX_SAMPLES);
            key(e, SDL_KEYDOWN, SDLK_F4); break;
        }
        case 141: assert(!strcmp(notification_text, "DIAGNOSTICS CAPTURED")); e->type = SDL_QUIT; break;
        default: assert(iteration < 142); break;
    }
    return 1;
}
int main(int argc, char **argv) {
    muted_test = argc > 1 && !strcmp(argv[1], "muted");
    unavailable_test = argc > 1 && !strcmp(argv[1], "unavailable");
    FILE *f = fopen("fixture.nes", "wb"); assert(f);
    uint8_t header[16] = {'N','E','S',0x1A,2,0};
    assert(fwrite(header, 1, 16, f) == 16);
    uint8_t rom[32768]; memset(rom, 0xEA, sizeof(rom));
    rom[0] = 0x4C; rom[1] = 0; rom[2] = 0x80;
    for (unsigned i = 32762; i < 32768; i += 2) { rom[i] = 0; rom[i + 1] = 0x80; }
    assert(fwrite(rom, 1, sizeof(rom), f) == sizeof(rom)); assert(!fclose(f));
    return emulator_main(argc, argv);
}
