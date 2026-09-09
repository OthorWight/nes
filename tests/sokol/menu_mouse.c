// Exercise menu mouse commands through the actual frontend event loop.

#include "../../src/host.h"
#include <assert.h>
static bool scripted_poll(HostEvent *event);
#define host_poll_event scripted_poll
#define sokol_main emulator_desc
#include "../../src/gui_main.c"
#undef sokol_main
#undef host_poll_event

static unsigned iteration;
static bool delivered;

static void click(HostEvent *e, int x, int y, uint8_t button) {
    e->type = HOST_MOUSEBUTTONDOWN;
    e->button.button = button;
    e->button.x = x;
    e->button.y = y;
}

static void test_mouse(float x, float y) {
    HostRect rect; host_viewport(sapp_width(), sapp_height(), &rect);
    sapp_event e = {.type = SAPP_EVENTTYPE_MOUSE_MOVE,
        .mouse_x = rect.x + x * rect.w / 256, .mouse_y = rect.y + y * rect.h / 240};
    host_event(&e);
    HostEvent ignored;
    while (host_poll_event(&ignored)) {}
}
static void geometry_and_scrolling(void) {
    HostCanvas *r = renderer;
    current_state = GUI_STATE_MENU_MAIN;
    const int sizes[][2] = {{256,240}, {768,720}, {1920,1080}, {1200,1600}};
    for (unsigned i = 0; i < 4; ++i) {
        HostRect rect; host_viewport(sizes[i][0], sizes[i][1], &rect);
        assert(rect.w > 0 && rect.h > 0);
        assert(rect.x >= 0 && rect.y >= 0);
        assert(abs(rect.w * 240 - rect.h * 256) <= 256);
    }
    assert(menu_hit_test(100, 138) == 5);
    assert(menu_hit_test(-1, 138) == -1);
    current_state = GUI_STATE_MENU_LOAD_ROM;
    rom_file_count = 30;
    menu_selection = rom_scroll_offset = 0;

    test_mouse(100, 100);
    HostEvent e;
    host_zero(e);
    e.type = HOST_MOUSEWHEEL;
    e.wheel.y = -1;
    assert(menu_mouse_command(&e, r) == HOST_KEY_UNKNOWN);
    assert(rom_scroll_offset == 3 && menu_selection == 3);
    assert(menu_hit_test(40, 70) == 3 && menu_hit_test(40, 202) == 14);
    e.wheel.direction = HOST_MOUSEWHEEL_FLIPPED;
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
    test_mouse(100, 88);
    e.wheel.direction = 0;
    e.wheel.y = 1;
    assert(menu_mouse_command(&e, r) == HOST_KEY_RIGHT && menu_selection == 2);
    e.wheel.y = -1;
    assert(menu_mouse_command(&e, r) == HOST_KEY_LEFT);
    current_state = GUI_STATE_GAMEPLAY;
    click(&e, 100, 100, HOST_BUTTON_LEFT);
    assert(menu_mouse_command(&e, r) == HOST_KEY_UNKNOWN);
    click(&e, 100, 100, HOST_BUTTON_RIGHT);
    assert(menu_mouse_command(&e, r) == HOST_KEY_ESCAPE);
    focused = false;
    assert(menu_mouse_command(&e, r) == HOST_KEY_UNKNOWN);
    focused = true;
    current_state = GUI_STATE_MENU_MAIN;
    menu_selection = 1;


}

static bool scripted_poll(HostEvent *e) {
    if (delivered) { delivered = false; ++iteration; return 0; }
    delivered = true;
    host_zero(*e);
    switch (iteration) {
        case 0:
            geometry_and_scrolling();
            click(e, 100, 62, HOST_BUTTON_LEFT); break; // Disabled Resume.
        case 1:
            assert(current_state == GUI_STATE_MENU_MAIN && menu_selection == 1);
            e->type = HOST_MOUSEMOTION; e->motion.x = 100; e->motion.y = 138; break;
        case 2:
            assert(menu_selection == 5);
            click(e, -10, 138, HOST_BUTTON_LEFT); break;
        case 3:
            assert(current_state == GUI_STATE_MENU_MAIN);
            click(e, 100, 138, HOST_BUTTON_LEFT); break;
        case 4:
            assert(current_state == GUI_STATE_MENU_SETTINGS);
            master_volume = 50;
            click(e, 210, 88, HOST_BUTTON_LEFT); break;
        case 5:
            assert(master_volume == 40 && menu_selection == 2);
            click(e, 234, 88, HOST_BUTTON_LEFT); break;
        case 6:
            assert(master_volume == 50);
            audio_muted = false;
            click(e, 100, 75, HOST_BUTTON_LEFT); break;
        case 7:
            assert(audio_muted);
            click(e, 30, 232, HOST_BUTTON_LEFT); break;
        case 8:
            assert(current_state == GUI_STATE_MENU_MAIN);
            click(e, 100, 123, HOST_BUTTON_LEFT); break;
        case 9:
            assert(current_state == GUI_STATE_MENU_CONTROLS);
            control_mode_keyboard = true;
            click(e, 100, 64, HOST_BUTTON_LEFT); break;
        case 10:
            assert(rebinding && menu_selection == 1);
            e->type = HOST_MOUSEMOTION; e->motion.x = 100; e->motion.y = 90; break;
        case 11:
            assert(rebinding && menu_selection == 1);
            click(e, 100, 90, HOST_BUTTON_LEFT); break;
        case 12:
            assert(rebinding && menu_selection == 1);
            click(e, 100, 90, HOST_BUTTON_RIGHT); break;
        case 13:
            assert(!rebinding && current_state == GUI_STATE_MENU_CONTROLS);
            click(e, 100, 47, HOST_BUTTON_LEFT); break;
        case 14:
            assert(!control_mode_keyboard);
            click(e, 100, 90, HOST_BUTTON_RIGHT); break;
        case 15:
            assert(current_state == GUI_STATE_MENU_MAIN);
            click(e, 100, 78, HOST_BUTTON_LEFT); break;
        case 16:
            assert(current_state == GUI_STATE_MENU_LOAD_ROM && rom_file_count == 0);
            click(e, 40, 70, HOST_BUTTON_LEFT); break;
        case 17:
            assert(current_state == GUI_STATE_MENU_LOAD_ROM && !nes_sys.cart);
            click(e, 30, 232, HOST_BUTTON_LEFT); break;
        case 18:
            assert(current_state == GUI_STATE_MENU_MAIN && !nes_sys.zapper_trigger);
            click(e, 100, 152, HOST_BUTTON_LEFT); break;
        default: assert(!"Mouse test did not exit"); break;
    }
    return 1;
}

static void test_cleanup(void) {
    assert(iteration == 18);
    app_cleanup();
    puts("Sokol menu mouse checks passed");
}
sapp_desc sokol_main(int argc, char **argv) {
    sapp_desc desc = emulator_desc(argc, argv);
    desc.cleanup_cb = test_cleanup;
    desc.event_cb = NULL; /* Scripted input; desktop focus does not affect assertions. */
    desc.width = 768; desc.height = 720;
    window_scale = 3;
    return desc;
}
