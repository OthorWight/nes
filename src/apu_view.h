#ifndef APU_VIEW_H
#define APU_VIEW_H
#include "apu2a03.h"

enum { APU_VIEW_CHANNELS = 5, APU_VIEW_HISTORY = 1840 };
typedef struct {
    float note[3]; /* MIDI pitch numbers, including fractional semitones. */
    float hz[3];
    uint8_t level[5]; /* Pulse/noise 0..15, triangle activity, DMC DAC 0..127. */
    bool active[5];
    uint8_t duty[2];
} ApuViewSample;
typedef struct {
    ApuViewSample history[APU_VIEW_HISTORY];
    unsigned next, count;
    uint64_t last_tick, last_cycle;
} ApuView;

/* Read-only observation at 240 Hz of emulated time. No register/bus reads,
   host audio changes, or save-state fields. Paused emulation freezes history. */
void apu_view_snapshot(const APU2A03 *apu, ApuViewSample *sample);
void apu_view_sample(ApuView *view, const APU2A03 *apu, uint64_t cpu_cycle);
const ApuViewSample *apu_view_age(const ApuView *view, unsigned age);
#endif
