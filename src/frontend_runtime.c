#include "frontend_runtime.h"

void frame_scheduler_reset(FrameScheduler *s, double now) {
    s->deadline = now;
}
double frame_scheduler_advance(FrameScheduler *s, double now, uint64_t cycles) {
    double duration = (double)cycles / NES_HOST_CPU_HZ;
    s->deadline += duration;
    // Keep fractional deadlines; discard accumulated debt after a host stall.
    if (now - s->deadline > 0.050) s->deadline = now;
    return s->deadline > now ? s->deadline - now : 0;
}
bool audio_queue_observe(AudioQueueMonitor *a, uint32_t queued, uint32_t incoming) {
    a->queue_samples = queued;
    if (queued > a->peak_samples) a->peak_samples = queued;
    bool reset = false;
    if (a->playing && !queued) { ++a->underruns; reset = true; }
    if ((uint64_t)queued + incoming > AUDIO_MAX_SAMPLES) { ++a->trims; reset = true; }
    if (reset) { a->playing = false; a->filter_ready = false; }
    return reset;
}
void audio_queue_pause(AudioQueueMonitor *a) {
    a->playing = false;
    a->queue_samples = 0;
    a->filter_ready = false;
}
double audio_queue_ratio(AudioQueueMonitor *a, uint32_t queued) {
    if (!a->playing) { a->filter_ready = false; return 1.0; }
    if (!a->filter_ready) {
        a->filtered_queue = AUDIO_TARGET_SAMPLES;
        a->filter_ready = true;
    }
    // ~0.8-second smoothing at NTSC frame rate avoids following each device
    // callback's 1024-sample sawtooth. Full correction at 500 samples of error
    // keeps enough reserve for late frames even near the correction limit.
    a->filtered_queue += 0.02 * ((double)queued - a->filtered_queue);
    double correction = (AUDIO_TARGET_SAMPLES - a->filtered_queue) * 0.00002;
    if (correction > AUDIO_MAX_RATE_CORRECTION) correction = AUDIO_MAX_RATE_CORRECTION;
    if (correction < -AUDIO_MAX_RATE_CORRECTION) correction = -AUDIO_MAX_RATE_CORRECTION;
    return 1.0 + correction;
}

uint32_t audio_resample(AudioResampler *r, const float *input, uint32_t count,
                        double ratio, float *output) {
    if (!count) return 0;
    uint32_t written = 0;
    double position = r->position;
    double step = 1.0 / ratio;
    // The final input sample is retained for interpolation into the next block.
    // Carry the fractional position too; restarting it per frame loses samples.
    while (position < (double)count - 1) {
        int left = position < 0 ? -1 : (int)position;
        float a = left < 0 ? r->previous : input[left];
        float b = input[left + 1];
        output[written++] = a + (b - a) * (float)(position - left);
        position += step;
    }
    r->position = position - count;
    r->previous = input[count - 1];
    return written;
}
uint8_t host_input_value(const HostInput *input) {
    return input->keyboard | input->buttons | input->axis;
}
void host_input_button(uint8_t *source, unsigned button, bool down) {
    if (button >= 8) return;
    if (down) *source |= (uint8_t)(1u << button);
    else *source &= (uint8_t)~(1u << button);
}
void host_input_axis(HostInput *input, bool vertical, int value) {
    unsigned shift = vertical ? 4 : 6;
    input->axis &= (uint8_t)~(3u << shift);
    if (value < -16000) input->axis |= (uint8_t)(1u << shift);
    if (value > 16000) input->axis |= (uint8_t)(2u << shift);
}
GameCrop game_crop(unsigned mapper) {
    if (mapper == 0 || mapper == 4 || mapper == 206 || mapper == 227)
        return (GameCrop){8, 8, 240, 224};
    if (mapper == 1) return (GameCrop){8, 0, 240, 240};
    return (GameCrop){0, 0, 256, 240};
}
bool game_aim(GameCrop crop, double lx, double ly, int *x, int *y) {
    *x = *y = -1;
    if (!(lx >= 0 && lx < 256 && ly >= 0 && ly < 240)) return false;
    *x = crop.x + (int)(lx * crop.w / 256.0);
    *y = crop.y + (int)(ly * crop.h / 240.0);
    return true;
}
