#ifndef FRONTEND_RUNTIME_H
#define FRONTEND_RUNTIME_H
#include <stdbool.h>
#include <stdint.h>

#define NES_HOST_CPU_HZ 1789773.0
#define AUDIO_RATE 44100u
#define AUDIO_PRIME_SAMPLES 1470u
#define AUDIO_MAX_SAMPLES 2940u

typedef struct { double deadline; } FrameScheduler;
void frame_scheduler_reset(FrameScheduler *s, double now);
double frame_scheduler_advance(FrameScheduler *s, double now, uint64_t cycles);

typedef struct {
    bool playing;
    unsigned underruns, trims, errors;
    uint32_t queue_samples, peak_samples;
} AudioQueueMonitor;
// Returns true when the queue must be cleared and primed before playback.
bool audio_queue_observe(AudioQueueMonitor *a, uint32_t queued, uint32_t incoming);
void audio_queue_pause(AudioQueueMonitor *a);

typedef struct { uint8_t keyboard, buttons, axis; } HostInput;
uint8_t host_input_value(const HostInput *input);
void host_input_button(uint8_t *source, unsigned button, bool down);
void host_input_axis(HostInput *input, bool vertical, int value);

typedef struct { int x, y, w, h; } GameCrop;
GameCrop game_crop(unsigned mapper);
bool game_aim(GameCrop crop, double logical_x, double logical_y, int *x, int *y);
#endif
