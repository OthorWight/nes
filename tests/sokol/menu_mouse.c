/* Exercise desktop controls through the real Sokol event loop. */
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
static bool delivered;
static void click(HostEvent *e, int x, int y) {
    int w,h; float scale = host_chrome_layout(sapp_width(),sapp_height(),&w,&h);
    e->type = HOST_MOUSEBUTTONDOWN; e->button.button = HOST_BUTTON_LEFT;
    e->window_mouse.valid = true;
    e->window_mouse.x = (int)(x*scale); e->window_mouse.y = (int)(y*scale);
}
static void key(HostEvent *e, HostKey code) { e->type = HOST_KEYDOWN; e->key.keysym.sym = code; }
static void geometry(void) {
    int w,h; float dpi = host_chrome_layout(256,276,&w,&h);
    assert(dpi >= 2.0f && dpi == floorf(dpi)); /* Enlarged, uniform bitmap pixels. */
    const int sizes[][2] = {{256,276},{768,756},{1024,996},{2557,1401},{3840,2160}};
    for (unsigned i=0;i<sizeof(sizes)/sizeof(sizes[0]);++i) {
        assert(host_chrome_layout(sizes[i][0],sizes[i][1],&w,&h) == dpi);
        assert(abs((int)(w*dpi)-sizes[i][0]) <= (int)ceilf(dpi));
        HostRect game,panel; host_layout(sizes[i][0],sizes[i][1],&game,&panel);
        assert(game.y >= MENU_BAR_HEIGHT*dpi);
        assert(game.y+game.h <= sizes[i][1]-STATUS_BAR_HEIGHT*dpi);
    }
}
static bool scripted_poll(HostEvent *e) {
    if (delivered) { delivered=false; ++iteration; return false; }
    delivered=true; host_zero(*e);
    switch(iteration) {
        case 0: geometry(); assert(!nes_sys.cart && !file_browser.active); click(e,12,8); break;
        case 1:
            assert(desktop_menu.active && desktop_menu.depth == 1);
            capture_window("desktop-menu.bmp"); click(e,40,30); break;
        case 2:
            assert(file_browser.active && !desktop_menu.active);
            capture_window("file-browser.bmp"); key(e,HOST_KEY_ESCAPE); break;
        case 3:
            assert(!file_browser.active); key(e,'o'); e->key.keysym.mod=HOST_MOD_CTRL; break;
        case 4:
            assert(file_browser.active); key(e,'l'); e->key.keysym.mod=HOST_MOD_CTRL; break;
        case 5: assert(file_browser.editing); key(e,HOST_KEY_ESCAPE); break;
        case 6: assert(file_browser.active && !file_browser.editing); key(e,HOST_KEY_ESCAPE); break;
        case 7:
            assert(!file_browser.active); desktop_command(MENU_BINDINGS); key(e,HOST_KEY_DOWN); break;
        case 8: assert(help_page==3 && control_selection==1); key(e,HOST_KEY_RETURN); break;
        case 9: assert(rebinding); key(e,'k'); break;
        case 10:
            assert(!rebinding && control_mappings[0]=='k'); capture_window("controls.bmp"); key(e,HOST_KEY_ESCAPE); break;
        case 11:
            assert(!help_page); desktop_command(MENU_MUTE); assert(audio_muted);
            desktop_command(MENU_VOLUME_DOWN); assert(master_volume==90);
            desktop_command(MENU_VOLUME_UP); assert(master_volume==100);
            desktop_command(MENU_CONTROLLER); assert(!zapper_enabled);
            desktop_command(MENU_ZAPPER); assert(zapper_enabled); key(e,HOST_KEY_F10); break;
        case 12: assert(desktop_menu.active); key(e,HOST_KEY_ESCAPE); break;
        case 13: assert(!desktop_menu.active); e->type=HOST_QUIT; break;
        default: assert(!"Desktop test did not exit"); break;
    }
    return true;
}
static void cleanup(void) { assert(iteration==13); app_cleanup(); puts("DPI-independent chrome, file browser, controls and desktop input passed"); }
sapp_desc sokol_main(int argc, char **argv) {
    sapp_desc desc=emulator_desc(argc,argv);
    desc.cleanup_cb=cleanup; desc.event_cb=NULL;
    desc.width=1024; desc.height=996; window_scale=4;
    return desc;
}
