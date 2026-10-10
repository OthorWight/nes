#ifndef NES_MOVIE_H
#define NES_MOVIE_H
#include "nes_system.h"
#include "save_state.h"

typedef struct { uint8_t command, pads[2]; } MovieInput;
typedef struct { size_t frame; char text[256]; } MovieSubtitle;
typedef struct {
    MovieInput *inputs;
    size_t count;
    MovieSubtitle *subtitles;
    size_t subtitle_count;
    char rom_name[256], author[256];
    uint8_t rom_md5[16], identity[16], ports[2], ram_init;
    bool pal, new_ppu;
    unsigned emulator_version;
} FM2Movie;
typedef struct { uint8_t *data; size_t size, frame; uint8_t startup_frames; } MovieCheckpoint;
typedef struct {
    FM2Movie *movie;
    size_t frame; /* Number of fully emulated input records, zero at power-on. */
    bool in_frame;
    MovieCheckpoint initial, previous, checkpoints[32], before_playback;
    size_t checkpoint_next;
} MoviePlayer;

/* Failures leave the current game/player unchanged; caller owns the loaded movie. */
FM2Movie *movie_load(const char *path, char *error, size_t error_size);
void movie_free(FM2Movie *movie);
bool movie_matches_rom(const FM2Movie *movie, const Cartridge *cart);
bool movie_start(MoviePlayer *player, NES *nes, FM2Movie *movie, char *error, size_t error_size);
bool movie_stop(MoviePlayer *player, NES *nes); /* Restores the pre-playback game. */
bool movie_restart(MoviePlayer *player, NES *nes);
bool movie_begin_frame(MoviePlayer *player, NES *nes);
void movie_end_frame(MoviePlayer *player, NES *nes);
/* Restore the closest earlier checkpoint; advance with begin/end until target. */
bool movie_seek_begin(MoviePlayer *player, NES *nes, size_t target);
const char *movie_subtitle(const MoviePlayer *player);
void movie_input_text(uint8_t pads, char text[9]);
/* Movie checkpoints carry file identity AND input position, separate from native states. */
NES_StateResult movie_state_save(MoviePlayer *player, NES *nes, const char *path);
NES_StateResult movie_state_load(MoviePlayer *player, NES *nes, const char *path);
#endif
