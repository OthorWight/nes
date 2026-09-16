#ifndef APU_VIEW_H
#define APU_VIEW_H
#include "apu2a03.h"

enum { APU_VIEW_CHANNELS = 5, APU_VIEW_HISTORY = 1840, APU_VIEW_EXPANSION_CHANNELS = 8 };
enum { APU_VIEW_ROWS_Y = 416, APU_VIEW_ROW_HEIGHT = 12,
       APU_VIEW_NAME_X = 12, APU_VIEW_NAME_CELLS = 8,
       APU_VIEW_EXP_NAME_X = 268, APU_VIEW_EXP_NAME_CELLS = 6 };
typedef struct {
    float note[3]; /* MIDI pitch numbers, including fractional semitones. */
    float hz[3];
    uint8_t level[5]; /* Pulse/noise 0..15, triangle activity, DMC DAC 0..127. */
    bool active[5];
    uint8_t duty[2];
    float expansion_note[APU_VIEW_EXPANSION_CHANNELS], expansion_hz[APU_VIEW_EXPANSION_CHANNELS];
    uint8_t expansion_level[APU_VIEW_EXPANSION_CHANNELS], expansion_count;
    bool expansion_active[APU_VIEW_EXPANSION_CHANNELS];
} ApuViewSample;
typedef struct {
    ApuViewSample history[APU_VIEW_HISTORY];
    unsigned next, count;
    uint64_t last_tick, last_cycle;
} ApuView;

/* Read-only observation at 240 Hz of emulated time. No register/bus reads,
   host audio changes, or save-state fields. Paused emulation freezes history. */
void apu_view_snapshot(const APU2A03 *apu, ApuViewSample *sample);
void apu_view_snapshot_nes(const NES *nes, ApuViewSample *sample);
void apu_view_sample(ApuView *view, const APU2A03 *apu, uint64_t cpu_cycle);
void apu_view_sample_nes(ApuView *view, const NES *nes);
const ApuViewSample *apu_view_age(const ApuView *view, unsigned age);
/* Panel-space hit test. Returns the host mixer bit, or -1 outside a name. */
int apu_view_channel_at(unsigned expansion_count, int x, int y);
#endif
