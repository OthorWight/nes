#include "movie.h"
#include "md5.h"
#include "state_io.h"
#include "execution.h"
#include "sprite_view.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>

enum { MOVIE_MAX_BYTES=64*1024*1024, MOVIE_STATE_HEADER=48 };
static bool fail(char *error, size_t size, const char *message) {
    if (size) snprintf(error,size,"%s",message);
    return false;
}
static bool number(const char *s, long *value) {
    char *end; errno=0; long n=strtol(s,&end,10);
    if (errno || end==s || *end) return false;
    *value=n; return true;
}
static uint8_t *read_file(const char *path, size_t limit, size_t *size) {
    FILE *f=fopen(path,"rb"); if (!f) return NULL;
    if (fseek(f,0,SEEK_END)!=0) { fclose(f); return NULL; }
    long length=ftell(f);
    if (length<0 || (unsigned long)length>limit || fseek(f,0,SEEK_SET)!=0) { fclose(f); return NULL; }
    uint8_t *data=malloc((size_t)length+1);
    bool ok=data && fread(data,1,(size_t)length,f)==(size_t)length && !ferror(f);
    if (fclose(f)!=0) ok=false;
    if (!ok) { free(data); return NULL; }
    *size=(size_t)length; data[*size]=0; return data;
}
static bool checksum(const char *s, uint8_t result[16]) {
    if (!strncmp(s,"base64:",7)) {
        s+=7; if (strlen(s)!=24 || strcmp(s+22,"==")) return false;
        const char *alphabet="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        uint32_t bits=0; unsigned count=0, out=0;
        for (unsigned i=0;i<22;++i) {
            const char *p=strchr(alphabet,s[i]); if (!p || !s[i]) return false;
            bits=(bits<<6)|(unsigned)(p-alphabet); count+=6;
            if (count>=8) { count-=8; result[out++]=(uint8_t)(bits>>count); }
        }
        return out==16 && !(bits&15);
    }
    if (!strncmp(s,"0x",2)) s+=2;
    if (strlen(s)!=32) return false;
    for (unsigned i=0;i<16;++i) {
        unsigned v=0;
        for (unsigned j=0;j<2;++j) {
            char c=s[i*2+j]; unsigned n;
            if (c>='0' && c<='9') n=(unsigned)(c-'0');
            else if (c>='a' && c<='f') n=(unsigned)(c-'a'+10);
            else if (c>='A' && c<='F') n=(unsigned)(c-'A'+10);
            else return false;
            v=v*16+n;
        }
        result[i]=(uint8_t)v;
    }
    return true;
}
void movie_free(FM2Movie *m) {
    if (!m) return;
    free(m->inputs); free(m->subtitles); free(m);
}
FM2Movie *movie_load(const char *path, char *error, size_t error_size) {
    size_t size=0; uint8_t *data=read_file(path,MOVIE_MAX_BYTES,&size);
    FM2Movie *m=calloc(1,sizeof(*m));
    const char *why="Cannot read FM2 (maximum 64 MiB)";
    if (!data || !m) goto bad;
    nes_md5(data,size,NULL,0,m->identity);
    bool binary=false; long length=-1; unsigned required=0, seen=0;
    size_t pos=0;
    while (pos<size && data[pos]!='|') {
        size_t start=pos; while (pos<size && data[pos]!='\n') ++pos;
        size_t end=pos; if (pos<size) ++pos;
        if (end>start && data[end-1]=='\r') --end;
        why="Invalid FM2 header";
        if (end-start>=4096 || memchr(data+start,0,end-start)) goto bad;
        char line[4096]; memcpy(line,data+start,end-start); line[end-start]=0;
        if (!*line) continue;
        char *value=strchr(line,' '); if (!value) goto bad;
        *value++=0; while (*value==' ') ++value;
        if (!seen && strcmp(line,"version")) goto bad;
        ++seen;
        long n;
        if (!strcmp(line,"romFilename")) { snprintf(m->rom_name,sizeof(m->rom_name),"%.255s",value); required|=2; }
        else if (!strcmp(line,"romChecksum")) { if (!checksum(value,m->rom_md5)) goto bad; required|=4; }
        else if (!strcmp(line,"comment")) {
            if (!strncmp(value,"author ",7)) snprintf(m->author,sizeof(m->author),"%.255s",value+7);
        } else if (!strcmp(line,"subtitle")) {
            char *text=strchr(value,' '); if (!text) goto bad; *text++=0;
            if (!number(value,&n) || n<0) goto bad;
            MovieSubtitle *subs=realloc(m->subtitles,(m->subtitle_count+1)*sizeof(*subs));
            if (!subs) { why="Out of memory loading subtitles"; goto bad; }
            m->subtitles=subs; MovieSubtitle *sub=&subs[m->subtitle_count++];
            sub->frame=(size_t)n; snprintf(sub->text,sizeof(sub->text),"%.255s",text);
        } else if (!strcmp(line,"savestate")) {
            why="FM2 starts from an FCEUX save state; power-on movies only"; goto bad;
        } else if (!strcmp(line,"version") || !strcmp(line,"emuVersion") || !strcmp(line,"palFlag") ||
                   !strcmp(line,"NewPPU") || !strcmp(line,"fourscore") || !strcmp(line,"microphone") ||
                   !strcmp(line,"FDS") || !strcmp(line,"port0") || !strcmp(line,"port1") ||
                   !strcmp(line,"port2") || !strcmp(line,"binary") || !strcmp(line,"length") ||
                   !strcmp(line,"RAMInitOption")) {
            if (!number(value,&n)) goto bad;
            if (!strcmp(line,"version")) { if (n!=3 || (required&1)) goto bad; required|=1; }
            else if (!strcmp(line,"emuVersion")) { if (n<0 || (unsigned long)n>UINT_MAX) goto bad; m->emulator_version=(unsigned)n; }
            else if (!strcmp(line,"palFlag")) { if (n<0 || n>1) goto bad; m->pal=n!=0; }
            else if (!strcmp(line,"NewPPU")) { if (n<0 || n>1) goto bad; m->new_ppu=n!=0; }
            else if (!strcmp(line,"binary")) { if (n<0 || n>1) goto bad; binary=n!=0; }
            else if (!strcmp(line,"length")) { if (n< -1) goto bad; length=n; }
            else if (!strcmp(line,"RAMInitOption")) {
                why="Seeded random RAM initialization is not supported";
                if (n<0 || n>2) goto bad;
                m->ram_init=(uint8_t)n;
            } else if (!strcmp(line,"port0") || !strcmp(line,"port1")) {
                why="FM2 supports standard gamepads or disconnected ports only";
                if (n<0 || n>1) goto bad;
                unsigned p=(unsigned)(line[4]-'0'); m->ports[p]=(uint8_t)n; required|=8u<<p;
            } else {
                why="FM2 Four Score, microphone, expansion controllers and FDS are not supported";
                if (n) goto bad;
            }
        }
        /* Unknown metadata is permitted by the extensible FM2 header format. */
    }
    why="FM2 is missing required headers or input records";
    if ((required&31)!=31 || pos>=size) goto bad;
    size_t capacity=0;
    if (binary) {
        ++pos; unsigned stride=1+m->ports[0]+m->ports[1];
        if ((size-pos)%stride) { why="Truncated binary FM2 input"; goto bad; }
        capacity=(size-pos)/stride;
        m->inputs=calloc(capacity ? capacity : 1,sizeof(*m->inputs));
        if (!m->inputs) goto memory;
        while (pos<size) {
            MovieInput *input=&m->inputs[m->count++]; input->command=data[pos++];
            for (unsigned p=0;p<2;++p) if (m->ports[p]) input->pads[p]=data[pos++];
        }
    } else {
        while (pos<size) {
            size_t start=pos; while (pos<size && data[pos]!='\n') ++pos;
            size_t end=pos; if (pos<size) ++pos;
            if (end>start && data[end-1]=='\r') --end;
            if (start==end) continue;
            why="Malformed FM2 input record";
            if (end-start>=128 || memchr(data+start,0,end-start)) goto bad;
            char line[128]; memcpy(line,data+start,end-start); line[end-start]=0;
            if (line[0]!='|') goto bad;
            char *fields[4], *s=line+1;
            for (unsigned i=0;i<4;++i) {
                fields[i]=s; char *delim=strchr(s,'|');
                if (!delim) goto bad;
                *delim=0; s=delim+1;
            }
            if (*s || *fields[3]) goto bad;
            long command; if (!number(fields[0],&command) || command<0 || command>255) goto bad;
            MovieInput input={(uint8_t)command,{0,0}};
            for (unsigned p=0;p<2;++p) {
                if (strlen(fields[p+1])!=(m->ports[p] ? 8u : 0u)) goto bad;
                for (unsigned i=0;m->ports[p] && i<8;++i)
                    if (fields[p+1][i]!='.' && fields[p+1][i]!=' ') input.pads[p]|=(uint8_t)(0x80u>>i);
            }
            if (m->count==capacity) {
                capacity=capacity ? capacity*2 : 1024;
                MovieInput *inputs=realloc(m->inputs,capacity*sizeof(*inputs));
                if (!inputs) goto memory;
                m->inputs=inputs;
            }
            m->inputs[m->count++]=input;
        }
    }
    why="FM2 length exceeds the available records";
    if (length>=0) { if ((unsigned long)length>m->count) goto bad; m->count=(size_t)length; }
    why="FM2 has no input frames"; if (!m->count) goto bad;
    why="FM2 contains unsupported disk/VS commands";
    for (size_t i=0;i<m->count;++i) if (m->inputs[i].command&~3u) goto bad;
    if (error_size) *error=0;
    free(data); return m;
memory:
    why="Out of memory loading FM2";
bad:
    fail(error,error_size,why); free(data); movie_free(m); return NULL;
}

bool movie_matches_rom(const FM2Movie *m, const Cartridge *cart) {
    if (!m || !cart || cart->info.prg_rom_size>cart->prg_rom_size ||
        cart->info.chr_rom_size>cart->chr_rom_size) return false;
    uint8_t digest[16];
    nes_md5(cart->prg_rom,cart->info.prg_rom_size,cart->chr_rom,cart->info.chr_rom_size,digest);
    return !memcmp(digest,m->rom_md5,16);
}
static void release(MovieCheckpoint *c) { free(c->data); memset(c,0,sizeof(*c)); }
static bool capture(MovieCheckpoint *c, NES *nes, size_t frame) {
    uint8_t *data=NULL; size_t size=0;
    if (nes_state_encode(nes,&data,&size)!=NES_STATE_OK) return false;
    release(c); *c=(MovieCheckpoint){data,size,frame,nes->movie_startup_frames}; return true;
}
static bool restore(MoviePlayer *p, NES *nes, const MovieCheckpoint *c) {
    if (!c->data || nes_state_decode(nes,c->data,c->size)!=NES_STATE_OK) return false;
    p->frame=c->frame; p->in_frame=false;
    nes->movie_frame_timing=true; nes->movie_startup_frames=c->startup_frames;
    nes->movie_disconnected=(p->movie->ports[0] ? 0 : 1) | (p->movie->ports[1] ? 0 : 2);
    execution_invalidate(nes->execution); return true;
}
static void power(MoviePlayer *p, NES *nes) {
    Cartridge *cart=nes->cart;
    NESDiagnostics *diagnostics=nes->diagnostics; NametableView *nt=nes->nametable_view;
    SpriteView *sprites=nes->sprite_view; NESExecution *execution=nes->execution;
    uint16_t muted=nes->audio_muted_channels;
    nes_init(nes); nes->cart=cart; cart->nes=nes;
    nes->diagnostics=diagnostics; nes->nametable_view=nt; nes->sprite_view=sprites;
    nes->execution=execution; nes->audio_muted_channels=muted;
    nes->region_override=p->movie->pal ? NES_PAL : NES_NTSC;
    for (unsigned i=0;i<sizeof(nes->wram);++i)
        nes->wram[i]=p->movie->ram_init==1 ? 0xFF : p->movie->ram_init==2 ? 0 : (i&4) ? 0xFF : 0;
    uint8_t fill=p->movie->ram_init==1 ? 0xFF : 0;
    memset(nes->ciram,fill,sizeof(nes->ciram));
    memset(nes->ppu.palette_ram,fill&0x3F,sizeof(nes->ppu.palette_ram));
    memset(nes->ppu.oam_ram,fill,sizeof(nes->ppu.oam_ram));
    if (cart->prg_ram) memset(cart->prg_ram,fill,cart->prg_ram_size);
    if (cart->chr_is_ram) for (uint32_t i=0;i<cart->chr_rom_size;++i)
        cart->chr_rom[i]=p->movie->ram_init==0 ? (i&4) ? 0xFF : 0 : fill;
    if (cart->mapper_data) memset(cart->mapper_data,0,cart->vtable->state_size);
    cart->mirroring=cart->info.mirroring;
    nes_reset(nes);
    nes->movie_frame_timing=true; nes->movie_startup_frames=2;
    nes->movie_disconnected=(p->movie->ports[0] ? 0 : 1) | (p->movie->ports[1] ? 0 : 2);
    /* FCEUX starts records in the post-render line, with two PPU-dead frames.
       Its startup polling phase avoids the dot-0 $2002/vblank suppression race. */
    nes->ppu.scanline=240; nes->ppu.cycle=1;
    if (cart->info.trainer && cart->prg_ram_size>=0x1200) memcpy(cart->prg_ram+0x1000,cart->trainer_data,512);
    execution_invalidate(execution);
}
bool movie_start(MoviePlayer *p, NES *nes, FM2Movie *m, char *error, size_t size) {
    if (!movie_matches_rom(m,nes->cart)) return fail(error,size,"FM2 ROM checksum does not match the loaded ROM");
    if (p->movie) return fail(error,size,"Stop the current movie before opening another");
    execution_sync(nes->execution);
    MoviePlayer staged={0}; staged.movie=m;
    if (!capture(&staged.before_playback,nes,0)) return fail(error,size,"Cannot capture pre-playback state");
    power(&staged,nes);
    if (!capture(&staged.initial,nes,0)) {
        nes_state_decode(nes,staged.before_playback.data,staged.before_playback.size);
        nes->movie_frame_timing=false; nes->movie_startup_frames=nes->movie_disconnected=0;
        release(&staged.before_playback); return fail(error,size,"Cannot capture movie power-on state");
    }
    *p=staged; if (size) *error=0; return true;
}
bool movie_stop(MoviePlayer *p, NES *nes) {
    if (!p->movie) return true;
    execution_sync(nes->execution);
    if (nes_state_decode(nes,p->before_playback.data,p->before_playback.size)!=NES_STATE_OK) return false;
    nes->movie_frame_timing=false; nes->movie_startup_frames=nes->movie_disconnected=0;
    execution_invalidate(nes->execution);
    movie_free(p->movie); release(&p->initial); release(&p->previous); release(&p->before_playback);
    for (unsigned i=0;i<32;++i) release(&p->checkpoints[i]);
    memset(p,0,sizeof(*p)); return true;
}
bool movie_restart(MoviePlayer *p, NES *nes) {
    if (!p->movie) return false;
    execution_sync(nes->execution);
    bool ok=restore(p,nes,&p->initial); release(&p->previous); return ok;
}
bool movie_begin_frame(MoviePlayer *p, NES *nes) {
    if (!p->movie || p->frame>=p->movie->count) return false;
    if (!p->in_frame) {
        capture(&p->previous,nes,p->frame);
        unsigned command=p->movie->inputs[p->frame].command;
        if (command&2) power(p,nes);
        else if (command&1) {
            /* FCEUX soft reset preserves memory but restarts its PPU registers
               and two-frame startup delay. Ordinary Reset retains core behavior. */
            uint8_t palette[32], oam[256];
            memcpy(palette,nes->ppu.palette_ram,sizeof(palette));
            memcpy(oam,nes->ppu.oam_ram,sizeof(oam));
            ppu_init(&nes->ppu);
            memcpy(nes->ppu.palette_ram,palette,sizeof(palette));
            memcpy(nes->ppu.oam_ram,oam,sizeof(oam));
            nes_reset(nes); nes->movie_startup_frames=2;
            nes->ppu.scanline=240; nes->ppu.cycle=1;
        }
        p->in_frame=true; nes->frame_ready=false;
    }
    memcpy(nes->controller_state,p->movie->inputs[p->frame].pads,2);
    return true;
}
void movie_end_frame(MoviePlayer *p, NES *nes) {
    if (!p->movie || !p->in_frame || !nes->frame_ready) return;
    ++p->frame; p->in_frame=false;
    if (!(p->frame%300)) {
        capture(&p->checkpoints[p->checkpoint_next%32],nes,p->frame); ++p->checkpoint_next;
        size_t bytes=0;
        for (unsigned i=0;i<32;++i) bytes+=p->checkpoints[i].size;
        /* Retain at most 32 MiB of seek checkpoints, even for large cartridges. */
        while (bytes>32u*1024u*1024u) {
            MovieCheckpoint *oldest=NULL;
            for (unsigned i=0;i<32;++i) if (p->checkpoints[i].data &&
                (!oldest || p->checkpoints[i].frame<oldest->frame)) oldest=&p->checkpoints[i];
            if (!oldest) break;
            bytes-=oldest->size; release(oldest);
        }
    }
    if (p->frame==p->movie->count) memset(nes->controller_state,0,2);
}
bool movie_seek_begin(MoviePlayer *p, NES *nes, size_t target) {
    if (!p->movie || target>p->movie->count) return false;
    execution_sync(nes->execution);
    if (target>=p->frame && !p->in_frame) return true;
    const MovieCheckpoint *best=&p->initial;
    if (p->previous.data && p->previous.frame<=target) best=&p->previous;
    for (unsigned i=0;i<32;++i) {
        const MovieCheckpoint *c=&p->checkpoints[i];
        if (c->data && c->frame<=target && c->frame>best->frame) best=c;
    }
    return restore(p,nes,best);
}
const char *movie_subtitle(const MoviePlayer *p) {
    if (!p->movie) return "";
    const MovieSubtitle *best=NULL;
    for (size_t i=0;i<p->movie->subtitle_count;++i) {
        const MovieSubtitle *s=&p->movie->subtitles[i];
        if (s->frame<=p->frame && (!best || s->frame>=best->frame)) best=s;
    }
    return best && p->frame-best->frame<180 ? best->text : "";
}
void movie_input_text(uint8_t pads, char text[9]) {
    const char *buttons="RLDUTSBA";
    for (unsigned i=0;i<8;++i) text[i]=(pads&(0x80u>>i)) ? buttons[i] : '.';
    text[8]=0;
}
NES_StateResult movie_state_save(MoviePlayer *p, NES *nes, const char *path) {
    if (!p->movie) return nes_state_save(nes,path);
    if (p->in_frame && nes->frame_ready) movie_end_frame(p,nes);
    uint8_t *core=NULL; size_t size=0; NES_StateResult result=nes_state_encode(nes,&core,&size);
    if (result!=NES_STATE_OK) return result;
    uint8_t *data=calloc(1,size+MOVIE_STATE_HEADER);
    if (!data) { free(core); return NES_STATE_MEMORY; }
    StateIO io={data,MOVIE_STATE_HEADER,0,false,true};
    state_bytes(&io,(uint8_t *)"NESFM2\0\1",8); state_bytes(&io,p->movie->identity,16);
    state_u64(&io,p->frame); state_u32(&io,(p->in_frame ? 1u : 0u) | (unsigned)nes->movie_startup_frames<<8); state_u32(&io,(uint32_t)size);
    state_u32(&io,state_crc32(core,size)); state_u32(&io,state_crc32(data,44));
    memcpy(data+MOVIE_STATE_HEADER,core,size);
    bool ok=state_atomic_write(path,data,size+MOVIE_STATE_HEADER);
    free(core); free(data); return ok ? NES_STATE_OK : NES_STATE_IO;
}
NES_StateResult movie_state_load(MoviePlayer *p, NES *nes, const char *path) {
    if (!p->movie) return nes_state_load(nes,path);
    size_t size=0; uint8_t *data=read_file(path,NES_STATE_MAX_SIZE+MOVIE_STATE_HEADER,&size);
    if (!data) return NES_STATE_OPEN;
    NES_StateResult result=NES_STATE_CORRUPT;
    if (size<MOVIE_STATE_HEADER || memcmp(data,"NESFM2\0\1",8)) goto done;
    if (memcmp(data+8,p->movie->identity,16)) { result=NES_STATE_WRONG_MOVIE; goto done; }
    StateIO io={data,MOVIE_STATE_HEADER,24,true,true};
    uint64_t frame=state_u64(&io,0); unsigned flags=state_u32(&io,0);
    unsigned in_frame=flags&1, startup=flags>>8;
    size_t core_size=state_u32(&io,0); uint32_t core_crc=state_u32(&io,0), header_crc=state_u32(&io,0);
    if (frame>p->movie->count || (flags&0xFE) || startup>2 || (frame==p->movie->count && in_frame) ||
        core_size!=size-MOVIE_STATE_HEADER || header_crc!=state_crc32(data,44) ||
        core_crc!=state_crc32(data+MOVIE_STATE_HEADER,core_size)) goto done;
    result=nes_state_decode(nes,data+MOVIE_STATE_HEADER,core_size);
    if (result==NES_STATE_OK) {
        p->frame=(size_t)frame; p->in_frame=in_frame!=0; release(&p->previous);
        nes->movie_frame_timing=true; nes->movie_startup_frames=(uint8_t)startup;
        execution_invalidate(nes->execution);
    }
done:
    free(data); return result;
}
