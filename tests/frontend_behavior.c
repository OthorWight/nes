#include "save_fixture.h"
#include "frontend_runtime.h"
#include "rom_preferences.h"
#include <math.h>

static void precise_clock(void) {
    puts("  fractional deadlines keep all audio modes at the same speed");
    for (unsigned mode = 0; mode < 3; ++mode) {
        FrameScheduler s = {0}; frame_scheduler_reset(&s, 10);
        double now = 10; uint64_t cycles = 0;
        for (unsigned frame = 0; frame < 36000; ++frame) {
            unsigned frame_cycles = frame % 3 == 0 ? 29780 : 29781;
            cycles += frame_cycles;
            now += 0.003 + mode * 0.001;
            double wait = frame_scheduler_advance(&s, now, frame_cycles);
            assert(wait > 0 && wait < 0.017);
            now += wait + (frame % 4) * 0.0001; // Host sleep jitter.
        }
        assert(fabs(s.deadline - (10 + cycles / NES_HOST_CPU_HZ)) < 0.00001);
        assert(frame_scheduler_advance(&s, now + 1, 29781) == 0);
        assert(fabs(s.deadline - (now + 1)) < 1e-9);
        frame_scheduler_reset(&s, 1000);
        assert(fabs(frame_scheduler_advance(&s, 1000.002, 29781) -
                    (29781 / NES_HOST_CPU_HZ - 0.002)) < 1e-9);
    }
}
static void queue_lifecycle(void) {
    puts("  audio priming, empty queue and latency cap ignore intentional pauses");
    AudioQueueMonitor a = {0};
    assert(!audio_queue_observe(&a, 0, 734));
    assert(a.underruns == 0);
    a.playing = true;
    assert(!audio_queue_observe(&a, 700, 734));
    assert(audio_queue_observe(&a, 0, 734));
    assert(a.underruns == 1 && !a.playing);
    assert(!audio_queue_observe(&a, 0, 734));
    a.playing = true;
    assert(audio_queue_observe(&a, AUDIO_MAX_SAMPLES, 734));
    assert(a.trims == 1 && !a.playing);
    audio_queue_pause(&a);
    assert(!audio_queue_observe(&a, 0, 734));
    assert(a.underruns == 1 && a.queue_samples == 0);
}
static void input_sources_and_aim(void) {
    puts("  independent keyboard/button/stick releases and cropped letterbox aim");
    HostInput i = {0};
    host_input_button(&i.keyboard, 7, true);
    host_input_button(&i.buttons, 7, true);
    host_input_axis(&i, false, 20000);
    host_input_button(&i.buttons, 7, false);
    host_input_axis(&i, false, 0);
    assert(host_input_value(&i) == 0x80);
    host_input_button(&i.keyboard, 7, false);
    assert(host_input_value(&i) == 0);
    host_input_axis(&i, true, -20000);
    host_input_axis(&i, false, -20000);
    assert(host_input_value(&i) == 0x50);
    memset(&i, 0, sizeof(i)); assert(host_input_value(&i) == 0);
    const unsigned mappers[] = {0, 1, 78};
    for (unsigned m = 0; m < 3; ++m) {
        GameCrop crop = game_crop(mappers[m]); int x, y;
        assert(game_aim(crop, 128, 120, &x, &y));
        assert(x == crop.x + crop.w / 2 && y == crop.y + crop.h / 2);
        assert(!game_aim(crop, -1, 0, &x, &y) && x == -1 && y == -1);
        assert(!game_aim(crop, 256, 240, &x, &y));
        assert(game_aim(crop, 0, 0, &x, &y) && x == crop.x && y == crop.y);
        assert(game_aim(crop, 255.999, 239.999, &x, &y));
        assert(x == crop.x + crop.w - 1 && y == crop.y + crop.h - 1);
    }
}
static void preferences(void) {
    puts("  ROM overrides round trip, fall back to globals and reject corrupt/wrong identities");
    char path[512], rom[512]; fixture_path(path, "rom.prefs"); fixture_path(rom, "fixture.nes");
    fixture_rom(rom, 0, false, true, 0); NES *n = fixture_load(rom);
    RomPreferences defaults = {3, false, false}, custom = {5, true, true}, out;
    bool overridden;
    assert(rom_preferences_load(path, n->cart->rom_identity, defaults, &out, &overridden));
    assert(out.scale == 3 && !out.zapper && !overridden);
    assert(rom_preferences_save(path, n->cart->rom_identity, custom, true));
    assert(rom_preferences_load(path, n->cart->rom_identity, defaults, &out, &overridden));
    assert(out.scale == 5 && out.zapper && out.fullscreen && overridden);
    uint32_t wrong[3] = {1,2,3};
    assert(!rom_preferences_load(path, wrong, defaults, &out, &overridden));
    assert(!out.zapper && !overridden);
    assert(rom_preferences_save(path, n->cart->rom_identity, custom, false));
    defaults.scale = 2;
    assert(rom_preferences_load(path, n->cart->rom_identity, defaults, &out, &overridden));
    assert(out.scale == 2 && !out.zapper && !overridden);
    assert(state_atomic_write(path, "NRP1", 4));
    assert(!rom_preferences_load(path, n->cart->rom_identity, defaults, &out, &overridden));
    assert(!out.zapper && out.scale == 2 && !overridden);
    fixture_free(n); assert(remove(rom) == 0); assert(remove(path) == 0);
}
int main(void) {
    fixture_start("frontend");
    precise_clock(); queue_lifecycle(); input_sources_and_aim(); preferences();
    assert(SAVE_RMDIR(fixture_dir) == 0);
    puts("Frontend scheduling, input, aim and preference checks passed.");
    return 0;
}
