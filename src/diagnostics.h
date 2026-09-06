#ifndef DIAGNOSTICS_H
#define DIAGNOSTICS_H
#include <stdbool.h>
#include <stdint.h>
#define DIAGNOSTIC_EVENTS 4096u
#define DIAGNOSTIC_FRAMES 180u
typedef struct NES NES;
typedef enum { DIAG_MAPPER_WRITE, DIAG_PPU_WRITE, DIAG_IRQ, DIAG_NMI,
               DIAG_INPUT_READ, DIAG_INPUT_LATCH, DIAG_RESUME } DiagnosticKind;
typedef struct {
    uint64_t cycle, frame;
    uint16_t pc, address, scanline, dot;
    uint8_t kind, value;
} DiagnosticEvent;
typedef struct {
    uint64_t frame, cycles;
    double elapsed, work_ms, queue_ms;
    uint32_t polls[2], latches, image_crc;
    uint8_t input[2];
    int aim_x, aim_y;
    bool trigger;
} DiagnosticFrame;
typedef struct NESDiagnostics {
    bool tracing;
    uint8_t irq;
    bool nmi;
    uint64_t frame, overwritten;
    unsigned event_head, event_count, frame_head, frame_count;
    uint32_t polls[2], latches;
    DiagnosticEvent events[DIAGNOSTIC_EVENTS];
    DiagnosticFrame frames[DIAGNOSTIC_FRAMES];
} NESDiagnostics;
typedef struct { double fps, speed, max_ms, queue_ms; unsigned spikes, changed, polls; } DiagnosticSummary;
void diagnostics_event(NES *n, DiagnosticKind kind, uint16_t address, uint8_t value);
void diagnostics_lines(NES *n);
void diagnostics_frame(NES *n, uint64_t cycles, double elapsed, double work_ms, double queue_ms);
DiagnosticSummary diagnostics_summary(const NESDiagnostics *d);
bool diagnostics_write(const NES *n, const char *path, const char *rom, unsigned underruns,
                       unsigned trims, unsigned errors, double device_ms);
bool nes_cpu_peek(NES *n, uint16_t address, uint8_t *value);
#endif
