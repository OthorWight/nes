#include "apu_view.h"
#include <math.h>
#include <string.h>

void apu_view_snapshot(const APU2A03 *a, ApuViewSample *s) {
    memset(s, 0, sizeof(*s));
    for (unsigned ch = 0; ch < 2; ++ch) {
        int period = a->pulse_timer_reload[ch];
        int change = period >> a->pulse_sweep_shift[ch];
        int target = a->pulse_sweep_negate[ch] ? period - change - (ch == 0) : period + change;
        s->level[ch] = a->pulse_constant_volume[ch] ? a->pulse_volume[ch] : a->pulse_envelope_decay[ch];
        s->active[ch] = a->pulse_length_counter[ch] && period >= 8 && period <= 0x7FF && target <= 0x7FF && s->level[ch];
        s->hz[ch] = (float)(1789773.0 / (16.0 * (period + 1)));
        s->duty[ch] = a->pulse_duty[ch];
    }
    s->hz[2] = (float)(1789773.0 / (32.0 * (a->triangle_timer_reload + 1)));
    s->active[2] = a->triangle_enabled && a->triangle_length_counter &&
        a->triangle_linear_counter && a->triangle_timer_reload >= 2;
    s->level[2] = s->active[2] ? 15 : 0;
    for (unsigned ch = 0; ch < 3; ++ch)
        s->note[ch] = 69.0f + 12.0f * log2f(s->hz[ch] / 440.0f);
    s->level[3] = a->noise_constant_volume ? a->noise_volume : a->noise_envelope_decay;
    s->active[3] = a->noise_enabled && a->noise_length_counter && s->level[3];
    s->level[4] = a->dmc_value;
    s->active[4] = !a->dmc_silent || !a->dmc_buffer_empty || a->dmc_bytes_remaining;
}

void apu_view_sample(ApuView *v, const APU2A03 *a, uint64_t cycle) {
    uint64_t tick = cycle * 240 / 1789773;
    if (v->count && cycle < v->last_cycle) memset(v, 0, sizeof(*v));
    v->last_cycle = cycle;
    if (v->count && tick == v->last_tick) return;
    /* Discontinuous emulation must not join unrelated notes. */
    if (v->count && tick > v->last_tick + 1) {
        uint64_t gap = tick - v->last_tick - 1;
        if (gap >= APU_VIEW_HISTORY) memset(v, 0, sizeof(*v));
        else while (gap--) {
            memset(&v->history[v->next], 0, sizeof(v->history[v->next]));
            v->next = (v->next + 1) % APU_VIEW_HISTORY;
            if (v->count < APU_VIEW_HISTORY) ++v->count;
        }
    }
    apu_view_snapshot(a, &v->history[v->next]);
    v->next = (v->next + 1) % APU_VIEW_HISTORY;
    if (v->count < APU_VIEW_HISTORY) ++v->count;
    v->last_tick = tick;
    v->last_cycle = cycle;
}

const ApuViewSample *apu_view_age(const ApuView *v, unsigned age) {
    if (age >= v->count) return NULL;
    return &v->history[(v->next + APU_VIEW_HISTORY - 1 - age) % APU_VIEW_HISTORY];
}
