#include "test_system.h"
#include "apu_view.h"
#include <math.h>

static TestSystem s;
static ApuView view;

static void pitches_and_channel_gates(void) {
    test_system_init(&s);
    APU2A03 *a = &s.nes.apu;
    a->pulse_timer_reload[0] = 253; // NTSC A4, approximately 440 Hz.
    a->pulse_sweep_shift[0] = 1;
    a->pulse_length_counter[0] = 10;
    a->pulse_constant_volume[0] = true;
    a->pulse_volume[0] = 9;
    a->triangle_timer_reload = 126; // Same pitch, triangle's 32-step period.
    a->triangle_enabled = true;
    a->triangle_length_counter = a->triangle_linear_counter = 10;
    a->noise_enabled = true; a->noise_length_counter = 10;
    a->noise_envelope_decay = 7;
    a->dmc_value = 64; a->dmc_silent = true;
    ApuViewSample sample;
    APU2A03 before = *a;
    apu_view_snapshot(a, &sample);
    assert(!memcmp(a, &before, sizeof(*a)));
    assert(sample.active[0] && fabsf(sample.note[0] - 69) < 0.02f);
    assert(sample.active[2] && fabsf(sample.note[2] - 69) < 0.02f);
    assert(sample.level[0] == 9 && sample.level[3] == 7 && sample.active[3]);
    assert(!sample.active[4] && sample.level[4] == 64); // Held DAC is not a note.
    a->dmc_silent = false; // Buffered playback survives disabling new DMA.
    a->pulse_timer_reload[0] = 7;
    a->triangle_linear_counter = 0;
    a->noise_length_counter = 0;
    apu_view_snapshot(a, &sample);
    assert(!sample.active[0] && !sample.active[2] && !sample.active[3] && sample.active[4]);
    a->pulse_timer_reload[0] = 0x600; // Sweep target overflow mutes the pulse.
    apu_view_snapshot(a, &sample); assert(!sample.active[0]);
}

static void emulated_time_history_and_isolation(void) {
    test_system_init(&s);
    memset(&view, 0, sizeof(view));
    static NES before;
    before = s.nes;
    apu_view_sample(&view, &s.nes.apu, 0);
    assert(view.count == 1);
    apu_view_sample(&view, &s.nes.apu, 0); // Pause/presentation never scrolls.
    assert(view.count == 1 && !memcmp(&before, &s.nes, sizeof(before)));
    for (unsigned i = 1; i < APU_VIEW_HISTORY + 10; ++i) {
        s.nes.apu.dmc_value = i % 128;
        apu_view_sample(&view, &s.nes.apu, ((uint64_t)i * 1789773 + 239) / 240);
    }
    assert(view.count == APU_VIEW_HISTORY);
    assert(apu_view_age(&view, 0)->level[4] == (APU_VIEW_HISTORY + 9) % 128);
    assert(apu_view_age(&view, APU_VIEW_HISTORY - 1)->level[4] == 10);
    assert(!apu_view_age(&view, APU_VIEW_HISTORY));
    apu_view_sample(&view, &s.nes.apu, 1); // Rewind/reset breaks history.
    assert(view.count == 1);
    apu_view_sample(&view, &s.nes.apu, 30000); // Missing ticks remain blank.
    assert(view.count == 5 && !apu_view_age(&view, 1)->active[0]);
}

static void expansion_snapshot_is_read_only(void) {
    test_system_init(&s);
    s.cart.mapper_id = 19;
    s.nes.expansion.n163.ram[127] = 0x1F; // Two channels.
    s.nes.expansion.n163.ram[0x78] = 0xFF;
    s.nes.expansion.n163.ram[0x7A] = 0xFF;
    s.nes.expansion.n163.ram[0x7C] = 0xFC;
    s.nes.expansion.n163.ram[0x77] = 8;
    static NES before;
    before = s.nes;
    ApuViewSample sample;
    apu_view_snapshot_nes(&s.nes, &sample);
    assert(sample.expansion_count == 2 && sample.expansion_level[0] == 255);
    assert(sample.expansion_active[0] && sample.expansion_hz[0] > 14000);
    assert(!memcmp(&before, &s.nes, sizeof(before)));
    memset(&view, 0, sizeof(view));
    apu_view_sample_nes(&view, &s.nes);
    assert(apu_view_age(&view, 0)->expansion_count == 2);
    assert(!memcmp(&before, &s.nes, sizeof(before)));
    s.cart.mapper_id = 85;
    apu_view_snapshot_nes(&s.nes, &sample);
    assert(sample.expansion_count == 6);
    for (unsigned ch = 0; ch < 6; ++ch) assert(!sample.expansion_active[ch]);
}

int main(void) {
    RUN_TEST(pitches_and_channel_gates);
    RUN_TEST(emulated_time_history_and_isolation);
    RUN_TEST(expansion_snapshot_is_read_only);
    return 0;
}
