// Exercise menu mouse commands through the actual frontend event loop.
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
static bool delivered;

static void click(SDL_Event *e, int x, int y, Uint8 button) {
    e->type = SDL_MOUSEBUTTONDOWN;
    e->button.button = button;
    e->button.x = x;
    e->button.y = y;
}

static void geometry_and_scrolling(void) {
    SDL_Window *w = SDL_CreateWindow("mouse test", 0, 0, 256, 240, SDL_WINDOW_HIDDEN);
    assert(w);
    SDL_Renderer *r = SDL_CreateRenderer(w, -1, SDL_RENDERER_SOFTWARE);
    assert(r && SDL_RenderSetLogicalSize(r, 256, 240) == 0);
    const int sizes[][2] = {{256, 240}, {768, 720}, {1920, 1080}, {1200, 1600}};
    current_state = GUI_STATE_MENU_MAIN;
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
        SDL_SetWindowSize(w, sizes[i][0], sizes[i][1]);
        SDL_PumpEvents();
        int wx, wy;
        float x, y;
        SDL_RenderLogicalToWindow(r, 100, 138, &wx, &wy);
        SDL_RenderWindowToLogical(r, wx, wy, &x, &y);
        assert(menu_hit_test((int)x, (int)y) == 5);
        SDL_RenderWindowToLogical(r, 0, 0, &x, &y);
        assert(menu_hit_test((int)x, (int)y) == -1);
    }

    current_state = GUI_STATE_MENU_LOAD_ROM;
    rom_file_count = 30;
    menu_selection = rom_scroll_offset = 0;
    SDL_SetWindowSize(w, 768, 720);
    SDL_WarpMouseInWindow(w, 300, 300);
    SDL_Event e;
    SDL_zero(e);
    e.type = SDL_MOUSEWHEEL;
    e.wheel.y = -1;
    assert(menu_mouse_command(&e, r) == SDLK_UNKNOWN);
    assert(rom_scroll_offset == 3 && menu_selection == 3);
    assert(menu_hit_test(40, 70) == 3 && menu_hit_test(40, 202) == 14);
    e.wheel.direction = SDL_MOUSEWHEEL_FLIPPED;
    menu_mouse_command(&e, r);
    assert(rom_scroll_offset == 0);
    for (int i = 0; i < 20; ++i) menu_scroll(1);
    assert(rom_scroll_offset == 18 && menu_selection >= 18 && menu_selection < 30);
    assert(menu_hit_test(40, 202) == 29 && menu_hit_test(40, 214) == -1);
    rom_file_count = 0;
    rom_scroll_offset = menu_selection = 0;
    assert(menu_hit_test(40, 70) == -1);
    current_state = GUI_STATE_MENU_LOAD_STATE;
    state_file_count = 0;
    assert(menu_hit_test(40, 70) == -1);
    current_state = GUI_STATE_MENU_SAVE_STATE;
    assert(menu_hit_test(40, 70) == 0 && menu_hit_test(40, 82) == -1);
    state_file_count = 20;
    menu_scroll(1);
    assert(rom_scroll_offset == 3 && menu_hit_test(40, 70) == 3);
    state_file_count = rom_scroll_offset = 0;

    current_state = GUI_STATE_MENU_SETTINGS;
    SDL_WarpMouseInWindow(w, 300, 264);
    e.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
    e.wheel.y = 1;
    assert(menu_mouse_command(&e, r) == SDLK_RIGHT && menu_selection == 2);
    e.wheel.y = -1;
    assert(menu_mouse_command(&e, r) == SDLK_LEFT);
    current_state = GUI_STATE_GAMEPLAY;
    click(&e, 100, 100, SDL_BUTTON_LEFT);
    assert(menu_mouse_command(&e, r) == SDLK_UNKNOWN);
    click(&e, 100, 100, SDL_BUTTON_RIGHT);
    assert(menu_mouse_command(&e, r) == SDLK_ESCAPE);
    focused = false;
    assert(menu_mouse_command(&e, r) == SDLK_UNKNOWN);
    focused = true;
    current_state = GUI_STATE_MENU_MAIN;
    menu_selection = 1;
    SDL_DestroyRenderer(r);
    SDL_DestroyWindow(w);
}

static int scripted_poll(SDL_Event *e) {
    if (delivered) { delivered = false; ++iteration; return 0; }
    delivered = true;
    SDL_zero(*e);
    switch (iteration) {
        case 0:
            geometry_and_scrolling();
            click(e, 100, 62, SDL_BUTTON_LEFT); break; // Disabled Resume.
        case 1:
            assert(current_state == GUI_STATE_MENU_MAIN && menu_selection == 1);
            e->type = SDL_MOUSEMOTION; e->motion.x = 100; e->motion.y = 138; break;
        case 2:
            assert(menu_selection == 5);
            click(e, -10, 138, SDL_BUTTON_LEFT); break;
        case 3:
            assert(current_state == GUI_STATE_MENU_MAIN);
            click(e, 100, 138, SDL_BUTTON_LEFT); break;
        case 4:
            assert(current_state == GUI_STATE_MENU_SETTINGS);
            master_volume = 50;
            click(e, 210, 88, SDL_BUTTON_LEFT); break;
        case 5:
            assert(master_volume == 40 && menu_selection == 2);
            click(e, 234, 88, SDL_BUTTON_LEFT); break;
        case 6:
            assert(master_volume == 50);
            audio_muted = false;
            click(e, 100, 75, SDL_BUTTON_LEFT); break;
        case 7:
            assert(audio_muted);
            click(e, 30, 232, SDL_BUTTON_LEFT); break;
        case 8:
            assert(current_state == GUI_STATE_MENU_MAIN);
            click(e, 100, 123, SDL_BUTTON_LEFT); break;
        case 9:
            assert(current_state == GUI_STATE_MENU_CONTROLS);
            control_mode_keyboard = true;
            click(e, 100, 64, SDL_BUTTON_LEFT); break;
        case 10:
            assert(rebinding && menu_selection == 1);
            e->type = SDL_MOUSEMOTION; e->motion.x = 100; e->motion.y = 90; break;
        case 11:
            assert(rebinding && menu_selection == 1);
            click(e, 100, 90, SDL_BUTTON_LEFT); break;
        case 12:
            assert(rebinding && menu_selection == 1);
            click(e, 100, 90, SDL_BUTTON_RIGHT); break;
        case 13:
            assert(!rebinding && current_state == GUI_STATE_MENU_CONTROLS);
            click(e, 100, 47, SDL_BUTTON_LEFT); break;
        case 14:
            assert(!control_mode_keyboard);
            click(e, 100, 90, SDL_BUTTON_RIGHT); break;
        case 15:
            assert(current_state == GUI_STATE_MENU_MAIN);
            click(e, 100, 78, SDL_BUTTON_LEFT); break;
        case 16:
            assert(current_state == GUI_STATE_MENU_LOAD_ROM && rom_file_count == 0);
            click(e, 40, 70, SDL_BUTTON_LEFT); break;
        case 17:
            assert(current_state == GUI_STATE_MENU_LOAD_ROM && !nes_sys.cart);
            click(e, 30, 232, SDL_BUTTON_LEFT); break;
        case 18:
            assert(current_state == GUI_STATE_MENU_MAIN && !nes_sys.zapper_trigger);
            click(e, 100, 152, SDL_BUTTON_LEFT); break;
        default: assert(!"Mouse test did not exit"); break;
    }
    return 1;
}

int main(int argc, char **argv) {
    int result = emulator_main(argc, argv);
    assert(result == 0 && iteration == 18);
    puts("SDL menu mouse checks passed");
    return result;
}
