#ifndef EXPANSION_AUDIO_H
#define EXPANSION_AUDIO_H
#include <stdbool.h>
#include <stdint.h>
#include "vrc7_audio.h"
typedef struct NES NES;
typedef struct StateIO StateIO;

typedef struct {
    uint8_t regs[3][3], control, phase[3], accumulator;
    uint16_t timer[3];
} Vrc6Audio;
typedef struct {
    uint8_t ram[128], address, divider, channel;
    bool disabled;
    int16_t output;
} N163Audio;
typedef struct {
    uint8_t regs[16], address, divider, tone[3], noise_divider;
    uint16_t timer[3], noise_timer, envelope_timer;
    uint32_t noise;
    int8_t envelope, direction;
    bool holding;
} S5bAudio;
typedef struct {
    uint8_t regs[2][4], enabled, length[2], phase[2], envelope[2], divider[2];
    bool start[2], half, toggle, read_mode, irq_enabled, irq;
    uint16_t timer[2], frame;
    uint8_t pcm;
} Mmc5Audio;
typedef struct {
    uint8_t wave[64], modulation[64], regs[11], gain[2], position, mod_position;
    uint16_t phase, mod_phase;
    uint32_t envelope_timer[2];
    int16_t bias;
    bool enabled;
} FdsAudio;
typedef struct {
    Vrc6Audio vrc6;
    N163Audio n163;
    S5bAudio s5b;
    Mmc5Audio mmc5;
    FdsAudio fds;
    Vrc7Audio vrc7;
} ExpansionAudio;

void expansion_audio_reset(NES *nes);
float expansion_audio_clock(NES *nes);
bool expansion_audio_write(NES *nes, uint16_t address, uint8_t value);
bool expansion_audio_read(NES *nes, uint16_t address, uint8_t *value);
void expansion_audio_observe_read(NES *nes, uint16_t address, uint8_t value);
void expansion_audio_state(ExpansionAudio *audio, StateIO *io);
#endif
