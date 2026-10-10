#include "movie.h"
#include "md5.h"
#include "state_io.h"
#include "execution.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <process.h>
#define task_pid _getpid
#else
#include <unistd.h>
#define task_pid getpid
#endif

static char rom_path[256], movie_path[256], state_path[256];
static uint8_t digest[16];
static void write_rom(void) {
    uint8_t header[16]={'N','E','S',0x1A,2,0,2};
    uint8_t prg[32768]; memset(prg,0xEA,sizeof(prg));
    /* Latch/read both pads into RAM continuously, using the real CPU and bus. */
    const uint8_t code[]={0xA9,1,0x8D,0x16,0x40,0xA9,0,0x8D,0x16,0x40,0xA2,0,
        0xAD,0x16,0x40,0x29,1,0x9D,0x00,0x01,0xAD,0x17,0x40,0x29,1,0x9D,0x08,0x01,
        0xE8,0xE0,8,0xD0,0xEB,0x4C,0x00,0x80};
    memcpy(prg,code,sizeof(code)); prg[32764]=0; prg[32765]=0x80;
    FILE*f=fopen(rom_path,"wb"); assert(f);
    assert(fwrite(header,1,16,f)==16 && fwrite(prg,1,sizeof(prg),f)==sizeof(prg)); assert(!fclose(f));
    nes_md5(prg,sizeof(prg),NULL,0,digest);
}
static void header(FILE*f,const char *extra,bool second) {
    fprintf(f,"version 3\r\nemuVersion 20604\r\nromFilename synthetic\r\nromChecksum 0x");
    for(unsigned i=0;i<16;++i)fprintf(f,"%02x",digest[i]);
    fprintf(f,"\r\nfourscore 0\r\nport0 1\r\nport1 %u\r\nport2 0\r\n%s",second?1:0,extra);
}
static void write_movie(const char *extra,const char *records) {
    FILE*f=fopen(movie_path,"wb");assert(f);header(f,extra,false);fputs(records,f);assert(!fclose(f));
}
static FM2Movie *load(void) {
    char error[256]; FM2Movie*m=movie_load(movie_path,error,sizeof(error));
    if(!m) fprintf(stderr,"%s\n",error);
    assert(m);return m;
}
static void invalid(const char*extra,const char*record) {
    write_movie(extra,record);char error[256];assert(!movie_load(movie_path,error,sizeof(error)));assert(*error);
}
static void md5_tests(void) {
    const char *inputs[]={"","a","abc","message digest","abcdefghijklmnopqrstuvwxyz",
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789",
        "12345678901234567890123456789012345678901234567890123456789012345678901234567890"};
    const char *expected[]={"d41d8cd98f00b204e9800998ecf8427e","0cc175b9c0f1b6a831c399e269772661",
        "900150983cd24fb0d6963f7d28e17f72","f96b697d7cb7938d525a2f31aaf161d0",
        "c3fcd3d76192e4007dfb496cca67e13b","d174ab98d277d9f5a5611c2c9f419d9f",
        "57edf4a22be3c955ac49da2e2107b67a"};
    for(unsigned i=0;i<7;++i)for(size_t split=0;split<=strlen(inputs[i]);++split) {
        uint8_t d[16];char hex[33];nes_md5(inputs[i],split,inputs[i]+split,strlen(inputs[i])-split,d);
        for(unsigned j=0;j<16;++j) snprintf(hex+j*2,3,"%02x",d[j]);
        assert(!strcmp(hex,expected[i]));
    }
}
static void parser_tests(void) {
    write_movie("comment author Tester\nsubtitle 1 Hello there\n","|0|RLDUTSBA|||\n|1|.......A|||\n|2|....T...|||\n");
    FM2Movie*m=load();assert(m->count==3 && m->inputs[0].pads[0]==255 && m->inputs[1].pads[0]==1 &&
        m->inputs[2].pads[0]==8 && m->inputs[1].command==1 && !strcmp(m->author,"Tester"));
    MoviePlayer p={0};p.movie=m;p.frame=1;assert(!strcmp(movie_subtitle(&p),"Hello there"));
    p.frame=181;assert(!*movie_subtitle(&p));movie_free(m);
    invalid("fourscore 1\n","|0|........|||\n");
    invalid("port0 2\n","|0|........|||\n");
    invalid("microphone 1\n","|0|........|||\n");
    invalid("FDS 1\n","|0|........|||\n");
    invalid("savestate 0x1234\n","|0|........|||\n");
    invalid("RAMInitOption 3\n","|0|........|||\n");
    invalid("palFlag 2\n","|0|........|||\n");
    invalid("length 2\n","|0|........|||\n");
    invalid("","|4|........|||\n");invalid("","|0|.......|||\n");
    invalid("","|0|........|||junk\n");invalid("","|0|........|........||\n");
    invalid("romChecksum base64:bad\n","|0|........|||\n");
    FILE*f=fopen(movie_path,"wb");assert(f);header(f,"binary 1\n",true);
    const uint8_t binary[]={'|',0,0xA5,0x3C,2,0x80,1};assert(fwrite(binary,1,sizeof(binary),f)==sizeof(binary));assert(!fclose(f));
    m=load();assert(m->count==2 && m->inputs[0].pads[0]==0xA5 && m->inputs[0].pads[1]==0x3C && m->inputs[1].command==2);movie_free(m);
    f=fopen(movie_path,"ab");assert(f);fputc(0,f);assert(!fclose(f));char error[256];assert(!movie_load(movie_path,error,sizeof(error)));
    write_movie("length 1\n","|0|.......A|||\n|0|R.......|||\n");m=load();assert(m->count==1);movie_free(m);
}
static void frame(MoviePlayer*p,NES*n,bool worker) {
    assert(movie_begin_frame(p,n));
    if(worker) {
        assert(execution_request(n->execution,EXEC_PLAY_FRAME,1,0,0));
        assert(execution_pump(n->execution,200000).reason==EXEC_STOP_FRAME);
    } else {
        unsigned guard=0;while(!n->frame_ready && guard++<50000)nes_clock_tick(n);assert(n->frame_ready);
    }
    movie_end_frame(p,n);n->apu.audio_buffer_idx=0;
}
static void same_state(NES*n,const uint8_t*expected,size_t expected_size) {
    uint8_t*actual;size_t size;assert(nes_state_encode(n,&actual,&size)==NES_STATE_OK);
    assert(size==expected_size && !memcmp(actual,expected,size));free(actual);
}
static void playback_tests(bool worker) {
    FILE*f=fopen(movie_path,"wb");assert(f);header(f,"RAMInitOption 0\n",true);
    for(unsigned i=0;i<605;++i)fprintf(f,"|0|%s|%s||\n",i%2?"R......A":".L....B.",i%2?"..D...B.":"...U...A");
    assert(!fclose(f));
    FM2Movie*m=load();NES*n=malloc(sizeof(*n));assert(n);nes_init(n);n->cart=cartridge_load(n,rom_path);assert(n->cart);
    n->wram[42]=99;n->cart->prg_ram[7]=88;n->region_override=NES_DENDY;nes_reset(n);
    uint8_t*original;size_t original_size;assert(nes_state_encode(n,&original,&original_size)==NES_STATE_OK);
    if(worker)assert(execution_create(n));
    MoviePlayer p={0};char error[256];
    m->rom_md5[0]^=1;assert(!movie_start(&p,n,m,error,sizeof(error)));same_state(n,original,original_size);m->rom_md5[0]^=1;
    assert(movie_start(&p,n,m,error,sizeof(error)));assert(n->ppu.region==NES_NTSC && n->wram[0]==0 && n->wram[4]==255 && n->cart->prg_ram[7]==0);
    for(unsigned i=0;i<605;++i) {
        frame(&p,n,worker);assert(p.frame==i+1);
        const MovieInput *input=&m->inputs[i];
        /* $0100 stores A first; finish any partial sampling loop before checking. */
        for(unsigned j=0;j<1000 && n->cpu.program_counter!=0x8000;++j)nes_clock_tick(n);
        for(unsigned port=0;port<2;++port)for(unsigned bit=0;bit<8;++bit)
            assert(n->wram[0x100+port*8+bit]==((input->pads[port]>>bit)&1));
        if(i==300)assert(movie_state_save(&p,n,state_path)==NES_STATE_OK);
    }
    assert(!movie_begin_frame(&p,n) && !n->controller_state[0] && !n->controller_state[1]);
    assert(movie_state_load(&p,n,state_path)==NES_STATE_OK && p.frame==301);
    uint8_t*snapshot;size_t snapshot_size;
    assert(nes_state_encode(n,&snapshot,&snapshot_size)==NES_STATE_OK);
    frame(&p,n,worker);assert(movie_seek_begin(&p,n,301));same_state(n,snapshot,snapshot_size);free(snapshot);
    assert(movie_seek_begin(&p,n,300) && p.frame==300);
    assert(movie_state_load(&p,n,state_path)==NES_STATE_OK);
    /* Wrong movie, damaged header, and native states cannot silently move the cursor. */
    m->identity[0]^=1;assert(movie_state_load(&p,n,state_path)==NES_STATE_WRONG_MOVIE && p.frame==301);m->identity[0]^=1;
    f=fopen(state_path,"r+b");assert(f);assert(!fseek(f,24,SEEK_SET));fputc(0xFF,f);assert(!fclose(f));
    assert(movie_state_load(&p,n,state_path)==NES_STATE_CORRUPT && p.frame==301);
    assert(nes_state_save(n,state_path)==NES_STATE_OK);assert(movie_state_load(&p,n,state_path)==NES_STATE_CORRUPT);
    assert(movie_restart(&p,n) && !p.frame);frame(&p,n,worker);
    assert(movie_stop(&p,n));same_state(n,original,original_size);assert(!p.movie);free(original);
    execution_destroy(n->execution);cartridge_free(n->cart);free(n);
}
static void commands_and_partial_states(void) {
    write_movie("palFlag 1\nRAMInitOption 1\n", "|0|.......A|||\n|1|......B.|||\n|2|R.......|||\n|0|........|||\n");
    FM2Movie*m=load(); NES*n=malloc(sizeof(*n));assert(n);nes_init(n);n->cart=cartridge_load(n,rom_path);assert(n->cart);
    MoviePlayer p={0};char error[256];assert(movie_start(&p,n,m,error,sizeof(error)));
    assert(n->ppu.region==NES_PAL && n->wram[0]==255 && n->wram[4]==255 && n->ppu.palette_ram[0]==63);
    assert(n->movie_frame_timing && n->movie_startup_frames==2);
    assert(n->movie_disconnected==2);
    nes_cpu_bus_write(n,0x4016,1);nes_cpu_bus_write(n,0x4016,0);
    for (unsigned i=0;i<24;++i) assert(!(nes_cpu_bus_read(n,0x4017)&1));
    assert(movie_begin_frame(&p,n));for(unsigned i=0;i<20;++i)nes_clock_tick(n);
    assert(movie_state_save(&p,n,state_path)==NES_STATE_OK);
    uint8_t*partial;size_t partial_size;assert(nes_state_encode(n,&partial,&partial_size)==NES_STATE_OK);
    frame(&p,n,false);assert(p.frame==1 && n->movie_startup_frames==1);
    assert(movie_state_load(&p,n,state_path)==NES_STATE_OK && p.frame==0 && p.in_frame && n->movie_startup_frames==2);
    same_state(n,partial,partial_size);free(partial);frame(&p,n,false);
    n->wram[0x400]=0xA5;n->cart->prg_ram[0]=0x5A;n->ppu.oam_ram[42]=17;n->ppu.palette_ram[2]=3;
    assert(movie_begin_frame(&p,n));assert(n->wram[0x400]==0xA5 && n->cart->prg_ram[0]==0x5A &&
        n->ppu.oam_ram[42]==17 && n->ppu.palette_ram[2]==3 && n->movie_startup_frames==2);
    frame(&p,n,false);assert(p.frame==2);
    assert(movie_begin_frame(&p,n));assert(n->wram[0x400]==255 && n->cart->prg_ram[0]==255 && n->movie_startup_frames==2);
    frame(&p,n,false);assert(p.frame==3);frame(&p,n,false);assert(p.frame==4);
    assert(movie_stop(&p,n));assert(!n->movie_frame_timing && !n->movie_startup_frames);
    cartridge_free(n->cart);free(n);
    write_movie("RAMInitOption 2\n", "|0|........|||\n");m=load();n=malloc(sizeof(*n));assert(n);
    nes_init(n);n->cart=cartridge_load(n,rom_path);assert(n->cart);assert(movie_start(&p,n,m,error,sizeof(error)));
    assert(!n->wram[0] && !n->wram[4] && !n->cart->chr_rom[7]);assert(movie_stop(&p,n));cartridge_free(n->cart);free(n);
}
int main(void) {
    snprintf(rom_path,sizeof(rom_path),"build/tests/movie-%ld.nes",(long)task_pid());
    snprintf(movie_path,sizeof(movie_path),"build/tests/movie-%ld.fm2",(long)task_pid());
    snprintf(state_path,sizeof(state_path),"build/tests/movie-%ld.state",(long)task_pid());
    md5_tests();write_rom();parser_tests();playback_tests(false);playback_tests(true);commands_and_partial_states();
    remove(rom_path);remove(movie_path);remove(state_path);puts("FM2 parsing, real controller replay, seeking and movie saves passed");return 0;
}
