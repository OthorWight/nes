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
static uint64_t saved_cycle;
static void key(HostEvent *e, HostKey sym, int modifiers) {
    e->type=HOST_KEYDOWN; e->key.keysym.sym=sym; e->key.keysym.mod=modifiers;
}
static void fixture(void) {
    uint8_t header[16]={'N','E','S',0x1A,2,1};
    uint8_t prg[32768]; memset(prg,0xEA,sizeof(prg));
    prg[0]=0x4C;prg[1]=0;prg[2]=0x80;
    prg[32764]=0;prg[32765]=0x80;
    uint8_t chr[8192]={0};
    FILE *f=fopen("synthetic.nes","wb");assert(f);
    assert(fwrite(header,1,sizeof(header),f)==sizeof(header));
    assert(fwrite(prg,1,sizeof(prg),f)==sizeof(prg));
    assert(fwrite(chr,1,sizeof(chr),f)==sizeof(chr));assert(!fclose(f));
    assert(frontend_load_rom("synthetic.nes"));
    paused=true; execution_sync(nes_sys.execution);
    nes_sys.cpu.accumulator=0x20;
}
static void binding_checks(void) {
    desktop_command(MENU_BINDINGS);
    HostEvent e={0};key(&e,HOST_KEY_RETURN,0);assert(desktop_event(&e));
    key(&e,HOST_KEY_F8,0);assert(desktop_event(&e));
    assert(binding_editor.listening && control_mappings[0]=='z' && *binding_editor.message);
    key(&e,'k',HOST_MOD_CTRL);assert(desktop_event(&e));assert(binding_editor.listening && control_mappings[0]=='z');
    key(&e,'x',0);assert(desktop_event(&e));
    assert(!binding_editor.listening && control_mappings[0]=='x' && control_mappings[1]=='z');
    key(&e,HOST_KEY_RETURN,0);assert(desktop_event(&e));
    key(&e,'k',0);assert(desktop_event(&e));assert(!binding_editor.listening && control_mappings[0]=='k');
    binding_editor.selected=8;
    key(&e,HOST_KEY_RETURN,0);assert(desktop_event(&e));
    assert(binding_editor.listening);
    key(&e,HOST_KEY_F8,0);assert(desktop_event(&e));
    assert(!binding_editor.listening && control_mappings[8]==HOST_KEY_F8 && control_mappings[9]==HOST_KEY_F5);
    binding_editor.keyboard=false;binding_editor.selected=0;
    key(&e,HOST_KEY_RETURN,0);assert(desktop_event(&e));
    int saved_controller=game_controller;game_controller=7;
    e=(HostEvent){.type=HOST_CONTROLLERBUTTONDOWN,.cbutton={7,HOST_CONTROLLER_BUTTON_Y}};
    assert(desktop_event(&e));
    assert(!binding_editor.listening && controller_button_mappings[0]==HOST_CONTROLLER_BUTTON_Y &&
        controller_button_mappings[9]==HOST_CONTROLLER_BUTTON_A);
    game_controller=saved_controller;
    control_mappings[0]=HOST_KEY_F8;control_mappings[1]='z';control_mappings[2]='z';
    control_mappings[8]='p';control_mappings[9]=HOST_KEY_F10;
    controller_button_mappings[0]=HOST_CONTROLLER_BUTTON_Y;
    normalize_control_mappings();
    assert(control_mappings[8]=='p' && control_mappings[9]==HOST_KEY_F8);
    for(int i=0;i<8;++i) {
        assert(shortcut_binding_key_allowed(control_mappings[i]));
        assert(!mapping_used(control_mappings,CONTROL_COUNT,control_mappings[i],i));
    }
    for(int i=0;i<CONTROL_COUNT;++i) assert(!mapping_used(controller_button_mappings,CONTROL_COUNT,controller_button_mappings[i],i));
    memcpy(control_mappings,binding_default_keys,sizeof(control_mappings));
    memcpy(controller_button_mappings,binding_default_buttons,sizeof(controller_button_mappings));
    save_emulator_settings();
    key(&e,HOST_KEY_ESCAPE,0);assert(desktop_event(&e));assert(!help_page);
    control_mappings[8]='k';control_mappings[9]='l';save_emulator_settings();
    control_mappings[8]=HOST_KEY_F5;control_mappings[9]=HOST_KEY_F8;load_emulator_settings();
    assert(control_mappings[8]=='k' && control_mappings[9]=='l');
    assert(!strcmp(file_items[2].shortcut,"K") && !strcmp(file_items[3].shortcut,"L"));
    assert(!strcmp(file_items[4].shortcut,"Shift+K") && !strcmp(file_items[5].shortcut,"Shift+L"));
    uint8_t saved=nes_sys.cpu.accumulator;
    key(&e,'k',0);assert(desktop_event(&e));nes_sys.cpu.accumulator=0x55;
    key(&e,HOST_KEY_F8,0);assert(!desktop_event(&e));assert(nes_sys.cpu.accumulator==0x55);
    key(&e,'l',0);assert(desktop_event(&e));assert(nes_sys.cpu.accumulator==saved);
    desktop_command(MENU_RUN);assert(debugger_active);
    key(&e,'k',0);assert(desktop_event(&e));nes_sys.cpu.accumulator=0x33;
    key(&e,'l',0);assert(desktop_event(&e));assert(nes_sys.cpu.accumulator==saved);
    desktop_command(MENU_RUN);assert(!debugger_active);
    key(&e,'k',HOST_MOD_SHIFT);assert(desktop_event(&e));assert(file_browser.active && browser_mode==2);
    key(&e,HOST_KEY_ESCAPE,0);assert(desktop_event(&e));assert(!file_browser.active);
    key(&e,'l',HOST_MOD_SHIFT);assert(desktop_event(&e));assert(file_browser.active && browser_mode==1);
    key(&e,HOST_KEY_ESCAPE,0);assert(desktop_event(&e));assert(!file_browser.active);
    memcpy(control_mappings,binding_default_keys,sizeof(control_mappings));save_emulator_settings();
    paused=true;
}
static bool scripted_poll(HostEvent *e) {
    if(delivered){delivered=false;++iteration;return false;}
    delivered=true;host_zero(*e);
    switch(iteration) {
        case 0:fixture();key(e,HOST_KEY_F5,0);break;
        case 1:nes_sys.cpu.accumulator=0x77;key(e,HOST_KEY_F8,0);break;
        case 2:assert(nes_sys.cpu.accumulator==0x20);saved_cycle=nes_sys.cpu.cycle_count;key(e,HOST_KEY_F10,0);break;
        case 3:
            assert(debugger_active && nes_sys.cpu.cycle_count==saved_cycle+3);
            saved_cycle=nes_sys.cpu.cycle_count;key(e,HOST_KEY_F6,0);break;
        case 4:
            if (execution_status(nes_sys.execution).pending) { --iteration; break; }
            assert(debugger_active && nes_sys.cpu.cycle_count>=saved_cycle+29780 &&
                nes_sys.cpu.cycle_count<=saved_cycle+29781);
            nes_sys.cpu.accumulator=0x77;key(e,HOST_KEY_F8,0);break;
        case 5:assert(nes_sys.cpu.accumulator==0x20);key(e,HOST_KEY_F8,HOST_MOD_SHIFT);break;
        case 6:
            assert(file_browser.active && browser_mode==1);key(e,HOST_KEY_ESCAPE,0);break;
        case 7:assert(!file_browser.active);key(e,'g',HOST_MOD_CTRL);break;
        case 8: {
            const char *text="set a $7F";
            for(;*text;++text){HostEvent t={.type=HOST_TEXTINPUT,.character=(unsigned char)*text};assert(desktop_event(&t));}
            key(e,'g',HOST_MOD_CTRL);e->key.repeat=true;break;
        }
        case 9:key(e,HOST_KEY_RETURN,0);break;
        case 10:
            assert(nes_sys.cpu.accumulator==0x7F);
            debugger_selected_line=5;key(e,HOST_KEY_UP,HOST_MOD_CTRL);break;
        case 11:assert(debugger_selected_line==4);key(e,HOST_KEY_F9,0);break;
        case 12:
            assert(!debugger_active);paused=true;binding_checks();key(e,HOST_KEY_F1,0);break;
        case 13:
            assert(!paused);key(e,'f',0);break;
        case 14:
            assert(!fullscreen);key(e,HOST_KEY_ESCAPE,0);break;
        case 15:
            assert(!paused);key(e,'z',HOST_MOD_CTRL);break;
        case 16:
            assert(!(nes_sys.controller_state[0]&1));key(e,HOST_KEY_F1,0);break;
        case 17:
            assert(paused);key(e,HOST_KEY_F8,HOST_MOD_CTRL);break;
        case 18:
            assert(paused && nes_sys.cpu.accumulator==0x7F);e->type=HOST_QUIT;break;
        default:assert(!"Shortcut test failed to exit");
    }
    return true;
}
static void cleanup(void) {
    assert(iteration==18);app_cleanup();
    puts("Frontend state/step/frame shortcuts, exact modifiers, console preservation and binding validation passed");
}
sapp_desc sokol_main(int argc,char **argv) {
    sapp_desc desc=emulator_desc(argc,argv);desc.cleanup_cb=cleanup;desc.event_cb=NULL;return desc;
}
