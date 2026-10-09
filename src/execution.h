#ifndef NES_EXECUTION_H
#define NES_EXECUTION_H
#include "nes_system.h"

/* Execution owns the machine only inside execution_pump(). On return its worker
   is parked, even inside an instruction, and the caller may inspect the machine.
   Mutations/save states require execution_sync() first. No graphics dependencies. */
typedef struct NESExecution NESExecution;
typedef enum {
    EXEC_PAUSE, EXEC_RUN, EXEC_PLAY_FRAME, EXEC_INSTRUCTION, EXEC_CPU_CYCLE,
    EXEC_PPU_DOT, EXEC_SCANLINE, EXEC_FRAME, EXEC_NEXT_SCANLINE, EXEC_NEXT_FRAME,
    EXEC_LOCATION, EXEC_ADDRESS, EXEC_OVER, EXEC_OUT, EXEC_NMI, EXEC_IRQ
} ExecutionMode;
typedef enum {
    EXEC_STOP_INITIAL, EXEC_STOP_STEP, EXEC_STOP_BREAKPOINT, EXEC_STOP_WATCH,
    EXEC_STOP_BRK, EXEC_STOP_JAM, EXEC_STOP_BUDGET, EXEC_STOP_FRAME, EXEC_STOP_LIMIT,
    EXEC_STOP_PAUSED
} ExecutionStop;
typedef enum {
    EXEC_EVENT_DOT, EXEC_EVENT_CYCLE, EXEC_EVENT_READ, EXEC_EVENT_WRITE,
    EXEC_EVENT_PPU_READ, EXEC_EVENT_PPU_WRITE, EXEC_EVENT_NMI, EXEC_EVENT_IRQ
} ExecutionEvent;
enum { EXEC_BREAK_EXEC = 1, EXEC_BREAK_READ = 2, EXEC_BREAK_WRITE = 4,
       EXEC_BREAK_PPU_READ = 8, EXEC_BREAK_PPU_WRITE = 16,
       EXEC_BREAKPOINTS = 32, EXEC_CALLS = 64, EXEC_TRACE = 512 };
typedef struct {
    bool used, enabled, once;
    unsigned access;
    uint16_t first, last;
    int value; /* -1 matches any byte. */
    int a, x, y; /* -1 matches any register value. */
    uint64_t hits, ignore;
} ExecutionBreakpoint;
typedef enum { EXEC_CALL_JSR, EXEC_CALL_BRK, EXEC_CALL_NMI, EXEC_CALL_IRQ } ExecutionCallKind;
typedef struct { uint16_t from, destination, return_pc; uint8_t sp; ExecutionCallKind kind; } ExecutionCall;
typedef struct {
    uint64_t cycle, dots;
    uint16_t pc, address;
    int scanline, dot;
    uint8_t value, a, x, y, sp, flags;
    unsigned access;
} ExecutionTrace;
typedef struct {
    ExecutionMode mode;
    ExecutionStop reason;
    bool pending, boundary;
    uint16_t instruction_pc, address;
    uint8_t value;
    unsigned access;
    int breakpoint;
    uint64_t dots, frames;
} ExecutionStatus;
typedef void (*ExecutionObserver)(NES *, void *);

NESExecution *execution_create(NES *);
void execution_destroy(NESExecution *);
bool execution_request(NESExecution *, ExecutionMode, uint64_t count, int scanline, int dot);
/* A bounded quantum always hands machine ownership back to the caller. */
ExecutionStatus execution_pump(NESExecution *, uint64_t dot_budget);
ExecutionStatus execution_status(const NESExecution *);
bool execution_sync(NESExecution *); /* Finish only the suspended instruction/DMA. */
void execution_invalidate(NESExecution *); /* Call at a synchronized boundary after load/edit. */
void execution_event(NES *, ExecutionEvent, uint16_t address, uint8_t value);
/* Normal playback counts clocks inline; only requested stops/replay need the
   parkable callback. Access hooks are selected separately by the pump. */
static inline void execution_dot(NES *n) {
    NESExecutionClock *clock = &n->execution_clock;
    if (!clock->events) return;
    ++clock->dots;
    if (n->ppu.scanline == 0 && n->ppu.cycle == 0) ++clock->frames;
    if (clock->budget) --clock->budget;
    if (clock->events & (1u << EXEC_EVENT_DOT)) execution_event(n, EXEC_EVENT_DOT, 0, 0);
}
static inline void execution_advance(NES *n, unsigned dots) {
    NESExecutionClock *clock = &n->execution_clock;
    if (!clock->events) return;
    clock->dots += dots;
    if (n->ppu.scanline == 0 && n->ppu.cycle < (int)dots) ++clock->frames;
    clock->budget = clock->budget > dots ? clock->budget - dots : 0;
}
static inline void execution_cycle(NES *n) {
    NESExecutionClock *clock = &n->execution_clock;
    if (clock->events && (!clock->budget || (clock->events & (1u << EXEC_EVENT_CYCLE))))
        execution_event(n, EXEC_EVENT_CYCLE, 0, 0);
}
void execution_set_pc_breakpoints(NESExecution *, bool *);
ExecutionBreakpoint *execution_breakpoints(NESExecution *);
void execution_set_traps(NESExecution *, bool brk, bool jam);
void execution_set_observer(NESExecution *, ExecutionObserver, void *);
void execution_set_instruction_observer(NESExecution *, ExecutionObserver);
const ExecutionCall *execution_calls(const NESExecution *, unsigned *count);
const ExecutionTrace *execution_trace(const NESExecution *, unsigned newest_offset);
unsigned execution_trace_count(const NESExecution *);
bool execution_export_trace(const NESExecution *, const char *path);
bool execution_save_breakpoints(const NESExecution *, const char *path);
bool execution_load_breakpoints(NESExecution *, const char *path);
void execution_enable_history(NESExecution *, bool);
/* Reverse to a previous instruction boundary or frame checkpoint. Checkpoints
   and input stamps are bounded host history, independent of movie-file formats. */
bool execution_reverse(NESExecution *, bool frame);
const char *execution_stop_name(ExecutionStop);
#endif
