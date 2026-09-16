#include "vrc7_audio.h"
#include "state_io.h"
#include <string.h>

/* Keep the upstream MIT implementation in one translation unit. Native-rate
   output bypasses its allocating rate converter; the NES mixer resamples it. */
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"
#endif
#include "../third_party/emu2413/emu2413.c"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

static void rebind(Vrc7Audio *a) {
    for (unsigned i = 0; i < 18; ++i)
        a->core.slot[i].patch = &a->core.patch[a->core.patch_number[i / 2] * 2 + (i & 1)];
}
static bool ready(Vrc7Audio *a, unsigned cpu_hz) {
    if (!a->initialized) {
        OPLL *core = OPLL_new(cpu_hz * 2, cpu_hz * 2 / 72);
        if (!core) return false;
        OPLL_setChipType(core, 1);
        OPLL_resetPatch(core, OPLL_VRC7_TONE);
        OPLL_setMask(core, OPLL_MASK_CH(6) | OPLL_MASK_CH(7) | OPLL_MASK_CH(8) | OPLL_MASK_RHYTHM);
        a->core = *core;
        a->core.conv = NULL;
        OPLL_delete(core);
        a->initialized = true;
    }
    rebind(a); // NES snapshots and staged save loads copy the embedded core.
    return true;
}
void vrc7_audio_write(Vrc7Audio *a, bool data, uint8_t value, unsigned hz) {
    if (!ready(a, hz)) return;
    if (!data) a->address = value & 63;
    else if (a->address < 8 || (a->address >= 0x10 && a->address <= 0x15) ||
             (a->address >= 0x20 && a->address <= 0x25) || (a->address >= 0x30 && a->address <= 0x35))
        OPLL_writeReg(&a->core, a->address, value);
}
float vrc7_audio_clock(Vrc7Audio *a, unsigned hz) {
    if (a->disabled || (!a->initialized && !ready(a, hz))) return 0;
    if (++a->divider == 36) {
        a->divider = 0;
        rebind(a);
        // Exactly one native FM tick per 36 CPU clocks. OPLL_calc's integer
        // sample-rate accumulator otherwise occasionally advances twice.
        update_output(&a->core);
        // Preserve the LFO period without the upstream signed phase counter
        // overflowing during long sessions.
        a->core.am_phase %= (int)(64 * sizeof(am_table));
        mix_output(&a->core);
        a->output = a->core.mix_out[0];
    }
    return a->output * (0.4f / 4096);
}

float vrc7_audio_sample(const Vrc7Audio *a, unsigned muted) {
    if (!a->initialized || a->disabled) return 0;
    // Do not use OPLL_setMask for isolation: upstream skips operator output
    // calculations for masked voices, altering feedback when they return.
    int output = 0;
    for (unsigned ch = 0; ch < 6; ++ch)
        if (!(muted & (1u << ch))) output += a->core.ch_out[ch];
    return output * (0.4f / 4096);
}

void vrc7_audio_state(Vrc7Audio *a, StateIO *io) {
    a->initialized = state_bool(io, a->initialized);
    a->disabled = state_bool(io, a->disabled);
    a->address = state_u8(io, a->address);
    a->divider = state_u8(io, a->divider);
    a->output = state_i16(io, a->output);
    if (a->address > 63 || a->divider >= 36) io->ok = false;
    if (!a->initialized) {
        if (io->reading) memset(&a->core, 0, sizeof(a->core));
        return;
    }
    OPLL *c = &a->core;
    // Initialize static waveform tables, but never serialize native pointers.
    if (io->reading) {
        OPLL *temporary = OPLL_new(3579546, 49715);
        if (!temporary) { io->ok = false; return; }
        OPLL_delete(temporary);
        memset(c, 0, sizeof(*c));
    }
    c->clk = state_u32(io, c->clk); c->rate = state_u32(io, c->rate);
    c->chip_type = state_u8(io, c->chip_type); c->adr = state_u32(io, c->adr);
    c->inp_step = state_f64(io, c->inp_step); c->out_step = state_f64(io, c->out_step); c->out_time = state_f64(io, c->out_time);
    state_bytes(io, c->reg, sizeof(c->reg));
    c->test_flag = state_u8(io, c->test_flag); c->slot_key_status = state_u32(io, c->slot_key_status);
    c->rhythm_mode = state_u8(io, c->rhythm_mode); c->eg_counter = state_u32(io, c->eg_counter);
    c->pm_phase = state_u32(io, c->pm_phase); c->am_phase = state_i32(io, c->am_phase);
    c->lfo_am = state_u8(io, c->lfo_am); c->noise = state_u32(io, c->noise); c->short_noise = state_u8(io, c->short_noise);
    for (unsigned i = 0; i < 9; ++i) {
        c->patch_number[i] = state_i32(io, c->patch_number[i]);
        if (c->patch_number[i] < 0 || c->patch_number[i] >= 19) io->ok = false;
    }
    for (unsigned i = 0; i < 38; ++i) {
        OPLL_PATCH *p = &c->patch[i];
#define PATCH_FIELD(f) p->f = state_u32(io, p->f)
        PATCH_FIELD(TL); PATCH_FIELD(FB); PATCH_FIELD(EG); PATCH_FIELD(ML); PATCH_FIELD(AR);
        PATCH_FIELD(DR); PATCH_FIELD(SL); PATCH_FIELD(RR); PATCH_FIELD(KR); PATCH_FIELD(KL);
        PATCH_FIELD(AM); PATCH_FIELD(PM); PATCH_FIELD(WS);
#undef PATCH_FIELD
        if (p->TL > 63 || p->FB > 7 || p->EG > 1 || p->ML > 15 || p->AR > 15 || p->DR > 15 ||
            p->SL > 15 || p->RR > 15 || p->KR > 1 || p->KL > 3 || p->AM > 1 || p->PM > 1 || p->WS > 1) io->ok = false;
    }
    for (unsigned i = 0; i < 18; ++i) {
        OPLL_SLOT *s = &c->slot[i];
        s->number = state_u8(io, s->number); s->type = state_u8(io, s->type);
        for (unsigned j = 0; j < 2; ++j) s->output[j] = state_i32(io, s->output[j]);
        uint8_t wave = state_u8(io, s->wave_table == wave_table_map[1]);
        if (wave > 1) io->ok = false;
        else if (io->reading) s->wave_table = wave_table_map[wave];
        s->pg_phase = state_u32(io, s->pg_phase); s->pg_out = state_u32(io, s->pg_out);
        s->pg_keep = state_u8(io, s->pg_keep); s->blk_fnum = state_u16(io, s->blk_fnum);
        s->fnum = state_u16(io, s->fnum); s->blk = state_u8(io, s->blk);
        s->eg_state = state_u8(io, s->eg_state); s->volume = state_i32(io, s->volume);
        s->key_flag = state_u8(io, s->key_flag); s->sus_flag = state_u8(io, s->sus_flag);
        s->tll = state_u16(io, s->tll); s->rks = state_u8(io, s->rks);
        s->eg_rate_h = state_u8(io, s->eg_rate_h); s->eg_rate_l = state_u8(io, s->eg_rate_l);
        s->eg_shift = state_u32(io, s->eg_shift); s->eg_out = state_u32(io, s->eg_out);
        s->update_requests = state_u32(io, s->update_requests);
        if (s->number != i || s->type != (i & 1) || s->blk > 7 || s->fnum > 511 || s->eg_state > DAMP ||
            s->eg_rate_h > 15 || s->eg_rate_l > 3 || s->eg_shift > 13 || s->rks > 15 ||
            s->blk_fnum > 4095 || s->volume < 0 || s->volume > 63 || s->pg_keep > 1 ||
            s->key_flag > 1 || s->sus_flag > 1 || s->pg_out >= PG_WIDTH || s->eg_out > EG_MUTE ||
            s->tll > 255 || s->output[0] < -4096 || s->output[0] > 4095 ||
            s->output[1] < -4096 || s->output[1] > 4095) io->ok = false;
    }
    state_bytes(io, c->pan, sizeof(c->pan));
    for (unsigned i = 0; i < 16; ++i) for (unsigned j = 0; j < 2; ++j)
        c->pan_fine[i][j] = state_f32(io, c->pan_fine[i][j]);
    c->mask = state_u32(io, c->mask);
    for (unsigned i = 0; i < 14; ++i) c->ch_out[i] = state_i16(io, c->ch_out[i]);
    for (unsigned i = 0; i < 2; ++i) c->mix_out[i] = state_i16(io, c->mix_out[i]);
    if (c->chip_type != 1 || c->rhythm_mode || c->inp_step <= 0 || c->out_step <= 0 || c->am_phase < 0 ||
        c->clk < 3000000 || c->clk > 4000000 || c->rate < 40000 || c->rate > 60000) io->ok = false;
    if (io->reading && io->ok) rebind(a);
}
