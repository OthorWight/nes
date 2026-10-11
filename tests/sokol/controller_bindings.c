/* Mouse-only editing through the real frontend, including persisted swaps. */
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
static void click(HostEvent *e, MenuRect r) {
    int w,h; float scale=host_chrome_layout(sapp_width(),sapp_height(),&w,&h);
    assert(r.w>0 && r.h>0);
    *e=(HostEvent){.type=HOST_MOUSEBUTTONDOWN,.button.button=HOST_BUTTON_LEFT,
        .window_mouse={(int)((r.x+r.w/2)*scale),(int)((r.y+r.h/2)*scale),true}};
}
static void choose(HostEvent *e, int value) {
    for (int i=0;i<binding_editor.choice_count;++i) if(binding_editor.choices[i]==value) {
        assert(binding_editor.cells[i].h>0);click(e,binding_editor.cells[i]);return;
    }
    assert(!"Missing choice");
}
static bool scripted_poll(HostEvent *e) {
    if(delivered){delivered=false;++iteration;return false;}
    delivered=true;host_zero(*e);
    switch(iteration) {
        case 0: desktop_command(MENU_BINDINGS); break;
        case 1:
            assert(help_page==3 && !binding_editor.listening);
            capture_window("bindings-keyboard.ppm");click(e,binding_editor.cells[0]);break;
        case 2:
            assert(binding_editor.listening);
            capture_window("bindings-keyboard-picker.ppm");choose(e,'x');break;
        case 3:
            assert(!binding_editor.listening && control_mappings[0]=='x' && control_mappings[1]=='z');
            control_mappings[0]='k';load_emulator_settings();assert(control_mappings[0]=='x');
            click(e,binding_editor.cells[8]);break;
        case 4:
            assert(binding_editor.listening && control_mappings[8]==HOST_KEY_F5);
            choose(e,'s');break;
        case 5:
            assert(!binding_editor.listening && control_mappings[8]=='s');
            assert(!strcmp(state_shortcut_labels[0],"S") && !strcmp(state_shortcut_labels[1],"Shift+S"));
            control_mappings[8]=HOST_KEY_F5;load_emulator_settings();
            assert(control_mappings[8]=='s' && !strcmp(state_shortcut_labels[0],"S"));
            click(e,binding_editor.tabs[1]);break;
        case 6:
            assert(!binding_editor.keyboard);
            capture_window("bindings-gamepad.ppm");click(e,binding_editor.cells[0]);break;
        case 7:
            assert(binding_editor.listening);
            capture_window("bindings-gamepad-picker.ppm");choose(e,HOST_CONTROLLER_BUTTON_Y);break;
        case 8:
            assert(!binding_editor.listening && controller_button_mappings[0]==HOST_CONTROLLER_BUTTON_Y &&
                controller_button_mappings[9]==HOST_CONTROLLER_BUTTON_A);
            controller_button_mappings[0]=HOST_CONTROLLER_BUTTON_B;load_emulator_settings();
            assert(controller_button_mappings[0]==HOST_CONTROLLER_BUTTON_Y);
            click(e,binding_editor.cells[1]);break;
        case 9: assert(binding_editor.listening);click(e,binding_editor.cancel);break;
        case 10:
            assert(!binding_editor.listening && controller_button_mappings[1]==HOST_CONTROLLER_BUTTON_B);
            click(e,binding_editor.reset);break;
        case 11:
            assert(!memcmp(controller_button_mappings,binding_default_buttons,sizeof(controller_button_mappings)));
            assert(control_mappings[0]=='x');click(e,binding_editor.tabs[0]);break;
        case 12: click(e,binding_editor.reset);break;
        case 13:
            assert(!memcmp(control_mappings,binding_default_keys,sizeof(control_mappings)));
            click(e,binding_editor.done);break;
        case 14:
            assert(!help_page && !host_input_value(&host_input));
            desktop_command(MENU_BINDINGS);break;
        case 15: click(e,binding_editor.close);break;
        case 16: assert(!help_page);e->type=HOST_QUIT;break;
        default: assert(!"Binding editor test did not exit");
    }
    return true;
}
static void cleanup(void) {
    assert(iteration==16);app_cleanup();
    puts("Frontend mouse key/button pickers, swaps, configurable save/load, persistence, cancel, reset and close passed.");
}
sapp_desc sokol_main(int argc,char **argv) {
    sapp_desc desc=emulator_desc(argc,argv);desc.cleanup_cb=cleanup;desc.event_cb=NULL;
    desc.width=1024;desc.height=996;window_scale=4;return desc;
}
