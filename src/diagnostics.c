#include "diagnostics.h"
#include "nes_system.h"
#include "frontend_runtime.h"
#include "state_io.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

void diagnostics_event(NES *n, DiagnosticKind kind, uint16_t address, uint8_t value) {
    NESDiagnostics *d = n->diagnostics;
    if (!d || !d->tracing) return;
    DiagnosticEvent *e = &d->events[d->event_head];
    *e = (DiagnosticEvent){n->cpu.cycle_count, d->frame, n->cpu.program_counter,
        address, (uint16_t)n->ppu.scanline, (uint16_t)n->ppu.cycle, (uint8_t)kind, value};
    d->event_head = (d->event_head + 1) % DIAGNOSTIC_EVENTS;
    if (d->event_count < DIAGNOSTIC_EVENTS) ++d->event_count;
    else ++d->overwritten;
}
void diagnostics_lines(NES *n) {
    NESDiagnostics *d = n->diagnostics;
    if (!d || !d->tracing) return;
    uint8_t irq = n->cpu.irq_lines | (n->lines.irq_line ? 1 : 0);
    if (irq != d->irq) { diagnostics_event(n, DIAG_IRQ, 0, irq); d->irq = irq; }
    if (n->cpu.nmi_line != d->nmi) {
        diagnostics_event(n, DIAG_NMI, 0, n->cpu.nmi_line); d->nmi = n->cpu.nmi_line;
    }
}
void diagnostics_frame(NES *n, uint64_t cycles, double elapsed, double work_ms, double queue_ms) {
    NESDiagnostics *d = n->diagnostics;
    if (!d) return;
    DiagnosticFrame *f = &d->frames[d->frame_head];
    memset(f, 0, sizeof(*f));
    f->frame = d->frame++; f->cycles = cycles;
    f->elapsed = elapsed; f->work_ms = work_ms; f->queue_ms = queue_ms;
    memcpy(f->polls, d->polls, sizeof(f->polls)); f->latches = d->latches;
    memcpy(f->input, n->controller_state, sizeof(f->input));
    f->aim_x = n->zapper_x; f->aim_y = n->zapper_y; f->trigger = n->zapper_trigger;
    f->image_crc = state_crc32(n->ppu.screen_buffer, sizeof(n->ppu.screen_buffer));
    memset(d->polls, 0, sizeof(d->polls)); d->latches = 0;
    d->frame_head = (d->frame_head + 1) % DIAGNOSTIC_FRAMES;
    if (d->frame_count < DIAGNOSTIC_FRAMES) ++d->frame_count;
}
DiagnosticSummary diagnostics_summary(const NESDiagnostics *d) {
    DiagnosticSummary s = {0}; double elapsed = 0, cycles = 0;
    uint32_t previous = 0;
    for (unsigned i = 0; i < d->frame_count; ++i) {
        unsigned index = (d->frame_head + DIAGNOSTIC_FRAMES - d->frame_count + i) % DIAGNOSTIC_FRAMES;
        const DiagnosticFrame *f = &d->frames[index];
        elapsed += f->elapsed; cycles += (double)f->cycles;
        double ms = f->elapsed * 1000;
        if (ms > s.max_ms) s.max_ms = ms;
        if (ms > 25) ++s.spikes;
        if (i && previous != f->image_crc) ++s.changed;
        previous = f->image_crc;
        s.polls += f->polls[0] + f->polls[1]; s.queue_ms = f->queue_ms;
    }
    if (elapsed > 0) { s.fps = d->frame_count / elapsed; s.speed = cycles / NES_HOST_CPU_HZ / elapsed * 100; }
    return s;
}
bool diagnostics_write(const NES *n, const char *path, const char *rom, unsigned underruns,
                       unsigned trims, unsigned errors, double device_ms) {
    const NESDiagnostics *d = n->diagnostics;
    if (!d || !n->cart) return false;
    FILE *f = fopen(path, "w");
    if (!f) return false;
    DiagnosticSummary s = diagnostics_summary(d);
    fprintf(f, "NES diagnostics 1; build %s %s; NTSC CPU %.0f Hz\nROM %s\n"
        "identity %08X-%08X-%08X mapper %u submapper %u timing %u zapper %u\n"
        "FPS %.3f speed %.3f%% max_ms %.3f spikes_over_25ms %u queue_ms %.3f device_buffer_ms %.3f\n"
        "observed_queue_empty %u queue_trims %u audio_errors %u trace_enabled %u overwritten_events %" PRIu64 "\n"
        "Queue excludes driver latency. Empty queue is an underrun indicator.\n"
        "Input reads and image changes are diagnostic signals, not proof of game logic progress.\n"
        "Recent frames: frame,cycles,host_ms,work_ms,queue_ms,polls1,polls2,latches,input1,input2,image_crc,aim_x,aim_y,trigger\n",
        __DATE__, __TIME__, NES_HOST_CPU_HZ, rom, n->cart->rom_identity[0], n->cart->rom_identity[1],
        n->cart->rom_identity[2], n->cart->mapper_id, n->cart->info.submapper, n->cart->info.timing,
        n->zapper_enabled, s.fps, s.speed, s.max_ms, s.spikes, s.queue_ms, device_ms,
        underruns, trims, errors, d->tracing, d->overwritten);
    for (unsigned i = 0; i < d->frame_count; ++i) {
        const DiagnosticFrame *v = &d->frames[(d->frame_head + DIAGNOSTIC_FRAMES - d->frame_count + i) % DIAGNOSTIC_FRAMES];
        fprintf(f, "%" PRIu64 ",%" PRIu64 ",%.3f,%.3f,%.3f,%u,%u,%u,%02X,%02X,%08X,%d,%d,%u\n",
            v->frame, v->cycles, v->elapsed * 1000, v->work_ms, v->queue_ms, v->polls[0], v->polls[1],
            v->latches, v->input[0], v->input[1], v->image_crc, v->aim_x, v->aim_y, v->trigger);
    }
    const char *names[] = {"MAP_WRITE", "PPU_WRITE", "IRQ", "NMI", "INPUT_READ", "INPUT_LATCH", "RESUME"};
    fprintf(f, "Recent events: frame,CPU_cycle,PC,scanline,dot,kind,address,value\n");
    for (unsigned i = 0; i < d->event_count; ++i) {
        const DiagnosticEvent *e = &d->events[(d->event_head + DIAGNOSTIC_EVENTS - d->event_count + i) % DIAGNOSTIC_EVENTS];
        fprintf(f, "%" PRIu64 ",%" PRIu64 ",%04X,%u,%u,%s,%04X,%02X\n", e->frame,
                e->cycle, e->pc, e->scanline, e->dot, names[e->kind], e->address, e->value);
    }
    bool ok = !ferror(f);
    if (fclose(f)) ok = false;
    return ok;
}
bool nes_cpu_peek(NES *n, uint16_t address, uint8_t *value) {
    if (address < 0x2000) { *value = n->wram[address & 0x7FF]; return true; }
    // Existing mapper reads at $6000+ only resolve memory; I/O is deliberately unknown.
    if (address >= 0x6000 && n->cart && n->cart->vtable && n->cart->vtable->cpu_read) {
        bool handled = false;
        uint8_t result = n->cart->vtable->cpu_read(n->cart, address, &handled);
        if (handled) { *value = result; return true; }
    }
    return false;
}
