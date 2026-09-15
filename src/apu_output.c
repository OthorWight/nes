#include "apu2a03.h"
#include "state_io.h"
#include <math.h>

/* 32-tap windowed-sinc impulses, 64 sub-sample phases. Oscillator steps
   deposit their amplitude changes into this ring before audio is decimated.
   The ~16-sample causal delay is independent of the CPU clock/region. */
static float kernel[64][32], pulse_mix[31], tnd_mix[16 * 16 * 128];
static bool initialized;
static void initialize(void) {
    if (initialized) return;
    const double pi = 3.14159265358979323846;
    for (unsigned phase = 0; phase < 64; ++phase) {
        double sum = 0;
        for (unsigned i = 0; i < 32; ++i) {
            double x = i - 15.0 - phase / 64.0;
            double window = 0.42 - 0.5 * cos(2 * pi * i / 31) + 0.08 * cos(4 * pi * i / 31);
            double sinc = fabs(x) < 1e-12 ? 0.9 : sin(0.9 * pi * x) / (pi * x);
            kernel[phase][i] = (float)(sinc * window);
            sum += kernel[phase][i];
        }
        for (unsigned i = 0; i < 32; ++i) kernel[phase][i] /= (float)sum;
    }
    for (unsigned i = 1; i < 31; ++i) pulse_mix[i] = 95.88f / (8128.0f / i + 100.0f);
    for (unsigned t = 0; t < 16; ++t)
        for (unsigned n = 0; n < 16; ++n)
            for (unsigned d = 0; d < 128; ++d) {
                float input = t / 8227.0f + n / 12241.0f + d / 22638.0f;
                tnd_mix[(t * 16 + n) * 128 + d] = input ? 159.79f / (1.0f / input + 100.0f) : 0;
            }
    initialized = true;
}

float apu_mix_dac(unsigned p1, unsigned p2, unsigned t, unsigned n, unsigned d) {
    initialize();
    return pulse_mix[(p1 & 15) + (p2 & 15)] + tnd_mix[((t & 15) * 16 + (n & 15)) * 128 + (d & 127)];
}

void apu_audio_clock(APU2A03 *a, float level) {
    initialize();
    float delta = level - a->mixed_previous;
    a->mixed_previous = level;
    if (delta != 0) {
        unsigned phase = (unsigned)(a->audio_accumulator * 64) & 63;
        for (unsigned i = 0; i < 32; ++i)
            a->sample_impulses[(a->sample_cursor + i) & 31] += delta * kernel[phase][i];
    }
    a->audio_accumulator += 44100.0 / nes_region_cpu_hz(a->region);
    if (a->audio_accumulator < 1) return;
    a->audio_accumulator -= 1;
    a->sample_integrator += a->sample_impulses[a->sample_cursor];
    a->sample_impulses[a->sample_cursor] = 0;
    a->sample_cursor = (a->sample_cursor + 1) & 31;

    // Matched RC poles at 90 Hz, 440 Hz and 14 kHz, at 44.1 kHz output.
    float x = a->sample_integrator;
    float hp = 0.987259f * (a->hp90_output + x - a->hp90_input);
    a->hp90_input = x; a->hp90_output = hp;
    x = hp;
    hp = 0.939235f * (a->hp440_output + x - a->hp440_input);
    a->hp440_input = x; a->hp440_output = hp;
    a->lp14000 += 0.863947f * (hp - a->lp14000);
    if (a->audio_buffer_idx < 4096) a->audio_buffer[a->audio_buffer_idx++] = a->lp14000;
}

/* Appended in save-state version 5; the original APU field order stays intact. */
void apu_state_extension(APU2A03 *a, StateIO *io) {
    a->length_pending = state_u8(io, a->length_pending);
    a->halt_pending = state_u8(io, a->halt_pending);
    state_bytes(io, a->length_previous, sizeof(a->length_previous));
    for (unsigned i = 0; i < 4; ++i) a->halt_previous[i] = state_bool(io, a->halt_previous[i]);
    a->region = state_u8(io, a->region);
    a->noise_rate = state_u8(io, a->noise_rate);
    a->frame_next_mode = state_bool(io, a->frame_next_mode);
    a->frame_clock_block = state_u8(io, a->frame_clock_block);
    for (unsigned i = 0; i < 32; ++i) a->sample_impulses[i] = state_f32(io, a->sample_impulses[i]);
    a->sample_cursor = state_u8(io, a->sample_cursor);
    a->mixed_previous = state_f32(io, a->mixed_previous);
    a->sample_integrator = state_f32(io, a->sample_integrator);
    a->hp90_input = state_f32(io, a->hp90_input);
    a->hp90_output = state_f32(io, a->hp90_output);
    a->hp440_input = state_f32(io, a->hp440_input);
    a->hp440_output = state_f32(io, a->hp440_output);
    a->lp14000 = state_f32(io, a->lp14000);
    if (a->length_pending > 15 || a->halt_pending > 15 || a->region > NES_DENDY ||
        a->noise_rate > 15 || a->frame_clock_block > 2 || a->sample_cursor >= 32) io->ok = false;
}
