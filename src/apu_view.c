#include "apu_view.h"
#include "nes_system.h"
#include <math.h>
#include <string.h>

int apu_view_channel_at(unsigned expansion_count, int x, int y) {
    if (y < APU_VIEW_ROWS_Y - 2) return -1;
    unsigned row = (unsigned)(y - (APU_VIEW_ROWS_Y - 2)) / APU_VIEW_ROW_HEIGHT;
    if (x >= APU_VIEW_NAME_X && x < APU_VIEW_NAME_X + APU_VIEW_NAME_CELLS * 8 && row < APU_VIEW_CHANNELS)
        return (int)row;
    if (x >= APU_VIEW_EXP_NAME_X && x < APU_VIEW_EXP_NAME_X + APU_VIEW_EXP_NAME_CELLS * 8 &&
        row < expansion_count && row < APU_VIEW_EXPANSION_CHANNELS)
        return NES_AUDIO_EXPANSION_SHIFT + (int)row;
    return -1;
}

void apu_view_snapshot(const APU2A03 *a, ApuViewSample *s) {
    memset(s, 0, sizeof(*s));
    for (unsigned ch = 0; ch < 2; ++ch) {
        int period = a->pulse_timer_reload[ch];
        int change = period >> a->pulse_sweep_shift[ch];
        int target = a->pulse_sweep_negate[ch] ? period - change - (ch == 0) : period + change;
        s->level[ch] = a->pulse_constant_volume[ch] ? a->pulse_volume[ch] : a->pulse_envelope_decay[ch];
        s->active[ch] = a->pulse_length_counter[ch] && period >= 8 && period <= 0x7FF && target <= 0x7FF && s->level[ch];
        s->hz[ch] = (float)(nes_region_cpu_hz(a->region) / (16.0 * (period + 1)));
        s->duty[ch] = a->pulse_duty[ch];
    }
    s->hz[2] = (float)(nes_region_cpu_hz(a->region) / (32.0 * (a->triangle_timer_reload + 1)));
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

static void expansion_voice(ApuViewSample *s, unsigned ch, float hz, unsigned level, bool active) {
    if (ch >= APU_VIEW_EXPANSION_CHANNELS) return;
    s->expansion_count = (uint8_t)(ch + 1);
    s->expansion_hz[ch] = hz;
    s->expansion_note[ch] = hz > 0 ? 69.0f + 12.0f * log2f(hz / 440.0f) : -1000;
    s->expansion_level[ch] = (uint8_t)(level > 255 ? 255 : level);
    s->expansion_active[ch] = active && level;
}

void apu_view_snapshot_nes(const NES *n, ApuViewSample *s) {
    apu_view_snapshot(&n->apu, s);
    if (!n->cart) return;
    const ExpansionAudio *a = &n->expansion;
    float hz = (float)nes_region_cpu_hz(n->apu.region);
    switch (n->cart->mapper_id) {
        case 24: case 26:
            for (unsigned ch = 0; ch < 3; ++ch) {
                unsigned period = a->vrc6.regs[ch][1] | ((a->vrc6.regs[ch][2] & 15) << 8);
                period >>= a->vrc6.control & 4 ? 8 : a->vrc6.control & 2 ? 4 : 0;
                float pitch = hz / ((ch == 2 ? 14 : 16) * (period + 1));
                if (ch < 2 && (a->vrc6.regs[ch][0] & 0x80)) pitch = 0;
                unsigned volume = ch == 2 ? (a->vrc6.regs[ch][0] & 63) * 255 / 63 : (a->vrc6.regs[ch][0] & 15) * 17;
                expansion_voice(s, ch, pitch, volume, (a->vrc6.regs[ch][2] & 128) && !(a->vrc6.control & 1));
            }
            break;
        case 19: {
            unsigned channels = ((a->n163.ram[127] >> 4) & 7) + 1;
            for (unsigned ch = 0; ch < channels; ++ch) {
                unsigned base = 0x78 - ch * 8;
                unsigned frequency = a->n163.ram[base] | (a->n163.ram[base + 2] << 8) | ((a->n163.ram[base + 4] & 3) << 16);
                unsigned length = 256 - (a->n163.ram[base + 4] & 0xFC);
                expansion_voice(s, ch, hz * frequency / (15.0f * channels * 65536 * length),
                    (a->n163.ram[base + 7] & 15) * 17, !a->n163.disabled);
            }
            break;
        }
        case 69:
            for (unsigned ch = 0; ch < 3; ++ch) {
                unsigned period = a->s5b.regs[ch * 2] | ((a->s5b.regs[ch * 2 + 1] & 15) << 8);
                unsigned volume = a->s5b.regs[8 + ch] & 16 ? (unsigned)a->s5b.envelope : a->s5b.regs[8 + ch] & 15;
                float pitch = a->s5b.regs[7] & (1u << ch) ? 0 : hz / (16 * (period ? period : 1));
                expansion_voice(s, ch, pitch, volume * 17, true);
            }
            break;
        case 5:
            for (unsigned ch = 0; ch < 2; ++ch) {
                unsigned period = a->mmc5.regs[ch][2] | ((a->mmc5.regs[ch][3] & 7) << 8);
                unsigned volume = a->mmc5.regs[ch][0] & 16 ? a->mmc5.regs[ch][0] & 15 : a->mmc5.envelope[ch];
                expansion_voice(s, ch, hz / (16 * (period + 1)), volume * 17, a->mmc5.length[ch] != 0);
            }
            expansion_voice(s, 2, 0, a->mmc5.pcm, a->mmc5.pcm != 0);
            break;
        case 20: {
            unsigned frequency = a->fds.regs[2] | ((a->fds.regs[3] & 15) << 8);
            unsigned volume = a->fds.gain[0] > 32 ? 32 : a->fds.gain[0];
            expansion_voice(s, 0, hz * frequency / (65536 * 64), volume * 255 / 32,
                a->fds.enabled && !(a->fds.regs[3] & 128) && !(a->fds.regs[9] & 128));
            break;
        }
        case 85:
            for (unsigned ch = 0; ch < 6; ++ch) {
                const OPLL_SLOT *carrier = &a->vrc7.core.slot[ch * 2 + 1];
                float frequency = hz * 2 / 72 * carrier->fnum * (1u << carrier->blk) / 524288;
                unsigned level = a->vrc7.initialized && carrier->eg_out < 124
                    ? (unsigned)(255 * exp2f(-(float)(carrier->eg_out + carrier->tll) / 16)) : 0;
                expansion_voice(s, ch, frequency, level, !a->vrc7.disabled);
            }
            break;
    }
}

static void sample(ApuView *v, const APU2A03 *a, const NES *n, uint64_t cycle) {
    uint64_t tick = cycle * 240 / nes_region_cpu_hz(a->region);
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
    if (n) apu_view_snapshot_nes(n, &v->history[v->next]);
    else apu_view_snapshot(a, &v->history[v->next]);
    v->next = (v->next + 1) % APU_VIEW_HISTORY;
    if (v->count < APU_VIEW_HISTORY) ++v->count;
    v->last_tick = tick;
    v->last_cycle = cycle;
}

void apu_view_sample(ApuView *v, const APU2A03 *a, uint64_t cycle) { sample(v, a, NULL, cycle); }
void apu_view_sample_nes(ApuView *v, const NES *n) { sample(v, &n->apu, n, n->cpu.cycle_count); }

const ApuViewSample *apu_view_age(const ApuView *v, unsigned age) {
    if (age >= v->count) return NULL;
    return &v->history[(v->next + APU_VIEW_HISTORY - 1 - age) % APU_VIEW_HISTORY];
}
