#include "save_fixture.h"
#include "diagnostics.h"
#include "save_state.h"
#include "frontend_runtime.h"
#include <math.h>

static NESDiagnostics history;
static void compare_state(NES *n, const uint8_t *before, size_t size) {
    uint8_t *after; size_t len;
    assert(nes_state_encode(n, &after, &len) == NES_STATE_OK);
    assert(len == size && !memcmp(before, after, size)); free(after);
}
int main(void) {
    fixture_start("diagnostics");
    char rom[512], log[512]; fixture_path(rom, "fixture.nes"); fixture_path(log, "capture.log");
    fixture_rom(rom, 4, false, true, 0);
    NES *n = fixture_load(rom); n->diagnostics = &history;
    uint8_t *before; size_t size;
    n->controller_shift[0] = 0x87; n->ppu.ppu_status = 0x80;
    assert(nes_state_encode(n, &before, &size) == NES_STATE_OK);
    uint8_t value = 0x77;
    assert(!nes_cpu_peek(n, 0x4016, &value) && value == 0x77);
    assert(!nes_cpu_peek(n, 0x2002, &value));
    assert(!nes_cpu_peek(n, 0x5204, &value));
    assert(nes_cpu_peek(n, 0x8000, &value));
    assert(nes_cpu_peek(n, 0x6000, &value));
    compare_state(n, before, size); free(before);
    nes_cpu_bus_write(n, 0xA001, 0); // Disabled MMC3 RAM must not bypass mapping.
    n->cpu_open_bus = 0xA9;
    assert(nes_cpu_peek(n, 0x6000, &value) && value == 0xA9);
    history.tracing = true;
    n->ppu.scanline = 100; n->ppu.cycle = 21;
    nes_cpu_bus_write(n, 0x2005, 0x31);
    nes_cpu_bus_write(n, 0x8000, 6);
    nes_cpu_bus_write(n, 0x4016, 1); nes_cpu_bus_write(n, 0x4016, 0);
    (void)nes_cpu_bus_read(n, 0x4016);
    assert(history.polls[0] == 1 && history.latches == 1);
    assert(history.events[0].kind == DIAG_PPU_WRITE && history.events[0].scanline == 100 && history.events[0].dot == 21);
    assert(history.events[1].kind == DIAG_MAPPER_WRITE);
    n->cpu.irq_lines = 2; diagnostics_lines(n);
    n->cpu.irq_lines = 3; diagnostics_lines(n);
    n->cpu.nmi_line = true; diagnostics_lines(n);
    assert(history.events[history.event_count - 1].kind == DIAG_NMI);
    unsigned count = history.event_count;
    diagnostics_lines(n); assert(history.event_count == count);
    for (unsigned i = 0; i < DIAGNOSTIC_EVENTS + 10; ++i)
        diagnostics_event(n, DIAG_PPU_WRITE, 0x2005, (uint8_t)i);
    assert(history.event_count == DIAGNOSTIC_EVENTS && history.overwritten == count + 10);
    assert(history.events[history.event_head].value == 10);
    for (unsigned i = 0; i < DIAGNOSTIC_FRAMES + 2; ++i)
        diagnostics_frame(n, 29781, 29781 / NES_HOST_CPU_HZ, 3, 33);
    DiagnosticSummary summary = diagnostics_summary(&history);
    assert(fabs(summary.speed - 100) < 0.001 && summary.spikes == 0 && summary.changed == 0);
    n->ppu.screen_buffer[0] ^= 0xFFFFFF;
    diagnostics_frame(n, 29781, 0.050, 3, 0);
    summary = diagnostics_summary(&history);
    assert(summary.spikes == 1 && summary.changed == 1 && summary.speed < 100);
    assert(nes_state_encode(n, &before, &size) == NES_STATE_OK);
    assert(diagnostics_write(n, log, "synthetic fixture", 2, 1, 0, 11.6));
    compare_state(n, before, size);
    assert(nes_state_decode(n, before, size) == NES_STATE_OK && n->diagnostics == &history);
    free(before);
    FILE *f = fopen(log, "rb"); assert(f);
    char header[2048]; size_t len = fread(header, 1, sizeof(header) - 1, f); header[len] = 0;
    assert(strstr(header, "synthetic fixture") && strstr(header, "not proof") && strstr(header, "overwritten_events"));
    assert(fclose(f) == 0);
    assert(!diagnostics_write(n, fixture_dir, "synthetic", 0, 0, 0, 0));
    fixture_free(n); assert(remove(rom) == 0); assert(remove(log) == 0);
    assert(SAVE_RMDIR(fixture_dir) == 0);
    puts("Diagnostic capture, bounded history and non-consuming peek checks passed.");
    return 0;
}
