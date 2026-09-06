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
    if (reset) a->playing = false;
    return reset;
}
void audio_queue_pause(AudioQueueMonitor *a) {
    a->playing = false;
    a->queue_samples = 0;
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
