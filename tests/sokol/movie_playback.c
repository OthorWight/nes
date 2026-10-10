#include "../../src/host.h"
#include <assert.h>
static bool scripted_poll(HostEvent *event);
#define host_poll_event scripted_poll
#define sokol_main emulator_desc
#include "../../src/gui_main.c"
#undef sokol_main
#undef host_poll_event
#include "../../src/md5.h"
#include "capture.h"

static unsigned iteration;
static bool delivered;
static uint64_t paused_cycles;
static void movie_fixture(void) {
    uint8_t header[16]={'N','E','S',0x1A,2,1};uint8_t bank[32768];memset(bank,0xEA,sizeof(bank));
    const uint8_t code[]={0xA9,0x80,0x8D,0,0x20,0x4C,5,0x80};memcpy(bank,code,sizeof(code));
    bank[0x100]=0x40;bank[32762]=0;bank[32763]=0x81;bank[32764]=0;bank[32765]=0x80;
    FILE*f=fopen("synthetic.nes","wb");assert(f);assert(fwrite(header,1,16,f)==16);assert(fwrite(bank,1,sizeof(bank),f)==sizeof(bank));
    uint8_t chr[8192]={0};assert(fwrite(chr,1,sizeof(chr),f)==sizeof(chr));assert(!fclose(f));
    uint8_t md5[16];nes_md5(bank,sizeof(bank),chr,sizeof(chr),md5);
    f=fopen("synthetic.fm2","wb");assert(f);fputs("version 3\nemuVersion 20604\nromFilename synthetic\nromChecksum 0x",f);
    for(unsigned i=0;i<16;++i)fprintf(f,"%02x",md5[i]);
    fputs("\nport0 1\nport1 1\nport2 0\nfourscore 0\nsubtitle 3 Movie test\n",f);
    for(unsigned i=0;i<36;++i)fprintf(f,"|0|%s|.......A||\n",i%2 ? "R......A" : ".L....B.");
    assert(!fclose(f));assert(frontend_load_rom("synthetic.nes"));
}
static void key(HostEvent*e,HostKey sym){e->type=HOST_KEYDOWN;e->key.keysym.sym=sym;}
static bool scripted_poll(HostEvent*e) {
    if(delivered){delivered=false;++iteration;return false;}delivered=true;host_zero(*e);
    switch(iteration) {
        case 0:
            movie_fixture();assert(frontend_load_movie("synthetic.fm2"));
            assert(!paused && movie_player.frame==0 && !nes_sys.zapper_enabled);key(e,HOST_KEY_F9);break;
        case 1:
            assert(paused && !movie_player.frame);paused_cycles=nes_sys.cpu.cycle_count;key(e,'z');break;
        case 2:
            assert(paused_cycles==nes_sys.cpu.cycle_count && !movie_player.frame);key(e,HOST_KEY_F8);break;
        case 3:
            assert(paused && movie_player.frame==1 && nes_sys.controller_state[0]==0x42 && nes_sys.controller_state[1]==1);
            save_emulator_state(save_state_dir,"movie-test.state");key(e,HOST_KEY_F8);break;
        case 4:
            assert(movie_player.frame==2);key(e,HOST_KEY_F8);e->key.keysym.mod=HOST_MOD_SHIFT;break;
        case 5:
            assert(paused && movie_player.frame==1);
            load_emulator_state(save_state_dir,"movie-test.state");assert(movie_player.frame==1);
            desktop_command(MENU_MOVIE_SEEK);assert(movie_seek_dialog);key(e,'3');break;
        case 6:assert(!strcmp(movie_seek_text,"3"));key(e,'0');break;
        case 7:assert(!strcmp(movie_seek_text,"30"));key(e,HOST_KEY_RETURN);break;
        case 8:
            if(movie_seeking){--iteration;break;}
            assert(paused && movie_player.frame==30);capture_window("tas-playback.bmp");
            assert(desktop_state(NULL,MENU_RESET)&MENU_DISABLED);
            assert(desktop_state(NULL,MENU_STEP)&MENU_DISABLED);
            desktop_command(MENU_MOVIE_TURBO);assert(!movie_speed);key(e,HOST_KEY_F9);break;
        case 9:
            if(movie_player.frame<36){--iteration;break;}
            assert(paused && movie_player.frame==36);key(e,HOST_KEY_F8);break;
        case 10:
            assert(paused && movie_player.frame==36);desktop_command(MENU_MOVIE_RESTART);assert(!movie_player.frame);
            desktop_command(MENU_MOVIE_STOP);assert(!movie_player.movie && paused);
            assert(!(desktop_state(NULL,MENU_RESET)&MENU_DISABLED));key(e,'m');e->key.keysym.mod=HOST_MOD_CTRL;break;
        case 11:assert(file_browser.active && browser_mode==3);key(e,HOST_KEY_ESCAPE);break;
        case 12:assert(!file_browser.active);e->type=HOST_QUIT;break;
        default:assert(!"Movie frontend test did not exit");
    }
    return true;
}
static void cleanup(void){assert(iteration==12);app_cleanup();puts("Movie shortcuts, input isolation, pause, stepping, seeking, end, restart and saves passed");}
sapp_desc sokol_main(int argc,char**argv){sapp_desc desc=emulator_desc(argc,argv);desc.cleanup_cb=cleanup;desc.event_cb=NULL;desc.width=1024;desc.height=996;window_scale=4;return desc;}
