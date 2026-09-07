#include "frontend_runtime.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>

static void streaming_interpolation(void) {
    puts("  resampling preserves phase, block boundaries, silence and gain");
    const double ratios[] = {0.99, 1.0, 1.01};
    for (unsigned mode = 0; mode < 3; ++mode) {
        AudioResampler r = {0};
        float input[4096], output[4160];
        unsigned consumed = 0, produced = 0;
        const unsigned sizes[] = {1, 2, 735, 734, 3, 4096, 1, 734};
        for (unsigned block = 0; block < sizeof(sizes) / sizeof(sizes[0]); ++block) {
            unsigned count = sizes[block];
            for (unsigned i = 0; i < count; ++i) input[i] = (float)(consumed + i) / 16384;
            unsigned written = audio_resample(&r, input, count, ratios[mode], output);
            assert(written <= sizeof(output) / sizeof(output[0]));
            for (unsigned i = 0; i < written; ++i) {
                double expected = (produced + i) / ratios[mode] / 16384;
                assert(fabs(output[i] - expected) < 0.000001);
            }
            produced += written; consumed += count;
        }
        assert(fabs(produced - (consumed - 1) * ratios[mode]) <= 1.000001);
        double position = r.position;
        assert(audio_resample(&r, NULL, 0, 1, output) == 0 && r.position == position);
    }
    AudioResampler r = {0};
    float input[735] = {0}, output[750];
    unsigned written = audio_resample(&r, input, 735, 1.01, output);
    for (unsigned i = 0; i < written; ++i) assert(output[i] == 0);
    // Constant amplitude must stay constant across a changing rate and blocks.
    r = (AudioResampler){0};
    for (unsigned i = 0; i < 735; ++i) input[i] = 0.25f;
    for (unsigned frame = 0; frame < 100; ++frame) {
        written = audio_resample(&r, input, 735, ratios[frame % 3], output);
        for (unsigned i = 0; i < written; ++i) assert(output[i] == 0.25f);
    }
}

static unsigned simulate(double drift, unsigned device_block, bool correct) {
    AudioQueueMonitor monitor = {0};
    AudioResampler resampler = {0};
    float input[735] = {0}, output[750];
    uint32_t queued = 0;
    unsigned missing = 0;
    double next_device = 0, last_host = 0, sample_fraction = 0;
    const double frame_duration = 29780.5 / NES_HOST_CPU_HZ;
    const double device_duration = device_block / (AUDIO_RATE * (1 + drift));
    for (unsigned frame = 0; frame < 36000; ++frame) {
        // Irregular wakeups and an occasional 20ms late frame; subsequent
        // frames catch up to the same absolute scheduler deadlines.
        double host = frame * frame_duration + (frame % 4) * 0.001;
        if (frame % 113 == 112) host += 0.020;
        if (host < last_host + 0.001) host = last_host + 0.001;
        last_host = host;
        while (monitor.playing && next_device <= host) {
            if (queued < device_block) ++missing;
            queued = queued > device_block ? queued - device_block : 0;
            next_device += device_duration;
        }
        sample_fraction += frame_duration * AUDIO_RATE;
        unsigned incoming = (unsigned)sample_fraction;
        sample_fraction -= incoming;
        double ratio = audio_queue_ratio(&monitor, queued);
        assert(ratio >= 0.99 && ratio <= 1.01);
        incoming = audio_resample(&resampler, input, incoming, correct ? ratio : 1, output);
        if (audio_queue_observe(&monitor, queued, incoming)) queued = 0;
        queued += incoming;
        if (!monitor.playing && queued >= AUDIO_PRIME_SAMPLES) {
            monitor.playing = true;
            next_device = host;
        }
        assert(queued <= AUDIO_MAX_SAMPLES);
    }
    printf("    drift=%+.2f%% block=%u correction=%u empty=%u trims=%u short_reads=%u\n",
           drift * 100, device_block, correct, monitor.underruns, monitor.trims, missing);
    if (correct) assert(monitor.underruns == 0 && monitor.trims == 0 && missing == 0);
    return monitor.underruns + monitor.trims + missing;
}

int main(void) {
    streaming_interpolation();
    puts("  ten minutes of device clock drift, block reads and host jitter");
    const double drift[] = {-0.008, -0.002, 0, 0.002, 0.008};
    for (unsigned i = 0; i < sizeof(drift) / sizeof(drift[0]); ++i)
        (void)simulate(drift[i], 1024, true);
    (void)simulate(0.002, 512, true);
    // Demonstrate that a larger buffer alone only postpones clock drift.
    assert(simulate(0.002, 1024, false) > 0);
    assert(simulate(-0.002, 1024, false) > 0);
    AudioQueueMonitor monitor = {.playing = true};
    (void)audio_queue_ratio(&monitor, 0);
    audio_queue_pause(&monitor);
    assert(!monitor.playing && !monitor.filter_ready);
    assert(audio_queue_ratio(&monitor, 0) == 1);
    puts("Audio streaming and drift checks passed.");
    return 0;
}
