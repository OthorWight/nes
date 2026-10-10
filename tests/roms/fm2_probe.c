/* Optional real-ROM playback verification; no commercial data is embedded. */
#include "movie.h"
#include "state_io.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    if (argc < 3 || argc > 4) {
        fprintf(stderr, "Usage: fm2_probe ROM.nes movie.fm2 [final.ppm]\n");return 2;
    }
    NES *nes=malloc(sizeof(*nes));if (!nes) return 2;
    nes_init(nes);char error[256];
    nes->cart=cartridge_load_ex(nes,argv[1],error,sizeof(error));
    if (!nes->cart) { fprintf(stderr,"%s\n",error);free(nes);return 1; }
    FM2Movie *movie=movie_load(argv[2],error,sizeof(error));MoviePlayer player={0};
    if (!movie || !movie_start(&player,nes,movie,error,sizeof(error))) {
        fprintf(stderr,"%s\n",error);movie_free(movie);cartridge_free(nes->cart);free(nes);return 1;
    }
    printf("FM2: %zu frames, %s, author %s, FCEUX %u\n",movie->count,movie->pal?"PAL":"NTSC",movie->author,movie->emulator_version);
    bool ok=true;
    while (player.frame<movie->count) {
        if (!movie_begin_frame(&player,nes)) { ok=false;break; }
        unsigned guard=0;
        while (!nes->frame_ready && guard++<50000) nes_clock_tick(nes);
        if (!nes->frame_ready) { ok=false;break; }
        movie_end_frame(&player,nes);nes->apu.audio_buffer_idx=0;
        if (!(player.frame%1000) || player.frame==movie->count) {
            printf("Frame %zu/%zu, PC $%04X, RAM CRC %08X\n",player.frame,movie->count,nes->cpu.program_counter,state_crc32(nes->wram,sizeof(nes->wram)));
            fflush(stdout);
        }
    }
    if (ok && argc==4) {
        FILE*f=fopen(argv[3],"wb");
        if (!f) ok=false;
        else {
            if (fprintf(f,"P6\n256 240\n255\n")<0) ok=false;
            for (unsigned i=0;i<256*240;++i) {
                uint32_t p=nes->ppu.screen_buffer[i];unsigned char rgb[]={(uint8_t)(p>>16),(uint8_t)(p>>8),(uint8_t)p};
                if (fwrite(rgb,1,3,f)!=3) ok=false;
            }
            if (fclose(f)) ok=false;
        }
    }
    printf("%s at frame %zu. Reaching EOF verifies playback, not cross-emulator synchronization.\n",ok?"Completed":"Stopped",player.frame);
    movie_stop(&player,nes);cartridge_free(nes->cart);free(nes);return ok?0:1;
}
