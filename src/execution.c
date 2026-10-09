#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#include "execution.h"
#include "diagnostics.h"
#include "state_io.h"
#include "save_state.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif
#ifdef __linux__
#include <sched.h>
#endif

enum { EXEC_HISTORY = 32, EXEC_INPUTS = 256 };
typedef struct {
    uint8_t *data;
    size_t size;
    uint64_t cycle, dots, frames;
    ExecutionCall calls[EXEC_CALLS];
    unsigned call_count;
} ExecutionCheckpoint;
typedef struct { uint64_t dots; uint8_t pad[2]; int x, y; bool trigger; } ExecutionInput;

struct NESExecution {
    NES *nes;
#ifdef _WIN32
    HANDLE thread;
    CRITICAL_SECTION mutex;
    CONDITION_VARIABLE condition;
#else
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t condition;
#endif
    bool running, quitting, synchronizing, bypass, trap_brk, trap_jam;
    ExecutionStatus status;
    uint64_t remaining, start_cycle;
    int target_line, target_dot;
    uint16_t return_pc;
    uint8_t return_sp;
    ExecutionCallKind interrupt_kind;
    bool *pc_breakpoints;
    unsigned active_access;
    uint64_t generation;
    ExecutionBreakpoint breaks[EXEC_BREAKPOINTS];
    ExecutionCall calls[EXEC_CALLS];
    unsigned call_count;
    ExecutionTrace trace[EXEC_TRACE];
    unsigned trace_head, trace_count;
    ExecutionObserver observer;
    ExecutionObserver instruction_observer;
    void *observer_context;
    bool history_enabled, replaying;
    uint64_t replay_target, last_history_frame;
    ExecutionCheckpoint history[EXEC_HISTORY];
    unsigned history_count, input_count;
    size_t history_bytes;
    ExecutionInput inputs[EXEC_INPUTS];
};
static void history_clear(NESExecution *e) {
    for (unsigned i=0;i<e->history_count;++i) free(e->history[i].data);
    e->history_count=0; e->history_bytes=0; e->input_count=0;
}
static void checkpoint(NESExecution *e) {
    if (!e->history_enabled || e->replaying || !e->status.boundary) return;
    if (e->history_count && e->history[e->history_count-1].cycle==e->nes->cpu.cycle_count) return;
    uint8_t *data; size_t size;
    if (nes_state_encode(e->nes,&data,&size)!=NES_STATE_OK) return;
    while (e->history_count && (e->history_count==EXEC_HISTORY || e->history_bytes+size>64u*1024u*1024u)) {
        e->history_bytes-=e->history[0].size; free(e->history[0].data);
        memmove(e->history,e->history+1,(--e->history_count)*sizeof(*e->history));
    }
    ExecutionCheckpoint *h=&e->history[e->history_count++];
    *h=(ExecutionCheckpoint){.data=data,.size=size,.cycle=e->nes->cpu.cycle_count,
        .dots=e->nes->execution_clock.dots,.frames=e->nes->execution_clock.frames,.call_count=e->call_count};
    memcpy(h->calls,e->calls,sizeof(e->calls)); e->history_bytes+=size;
    e->last_history_frame=e->nes->execution_clock.frames;
}
static void replay_input(NESExecution *e) {
    if (!e->replaying) return;
    const ExecutionInput *input=NULL;
    for (unsigned i=0;i<e->input_count && e->inputs[i].dots<=e->nes->execution_clock.dots;++i) input=&e->inputs[i];
    if (input) {
        memcpy(e->nes->controller_state,input->pad,2);
        e->nes->zapper_x=input->x; e->nes->zapper_y=input->y; e->nes->zapper_trigger=input->trigger;
    }
}
static void lock(NESExecution *e) {
#ifdef _WIN32
    EnterCriticalSection(&e->mutex);
#else
    pthread_mutex_lock(&e->mutex);
#endif
}
static void unlock(NESExecution *e) {
#ifdef _WIN32
    LeaveCriticalSection(&e->mutex);
#else
    pthread_mutex_unlock(&e->mutex);
#endif
}
static void signal_worker(NESExecution *e) {
#ifdef _WIN32
    WakeAllConditionVariable(&e->condition);
#else
    pthread_cond_broadcast(&e->condition);
#endif
}
static void wait_worker(NESExecution *e) {
#ifdef _WIN32
    SleepConditionVariableCS(&e->condition, &e->mutex, INFINITE);
#else
    pthread_cond_wait(&e->condition, &e->mutex);
#endif
}
/* The stack containing cpu_step, its addressing helpers and DMA locals stays
   intact here. No longjmp, rollback, replayed bus access, or partial return. */
static void park(NESExecution *e, ExecutionStop reason, bool complete) {
    if (complete) e->status.pending = false;
    e->status.reason = reason;
    lock(e);
    e->running = false;
    signal_worker(e);
    while (!e->running && !e->quitting) wait_worker(e);
    unlock(e);
}
static void record(NESExecution *e, unsigned access, uint16_t addr, uint8_t value) {
    ExecutionTrace *t = &e->trace[e->trace_head];
    CPU6502 *c = &e->nes->cpu;
    *t = (ExecutionTrace){c->cycle_count, e->nes->execution_clock.dots, e->status.instruction_pc,
        addr, e->nes->ppu.scanline, e->nes->ppu.cycle, value,
        c->accumulator, c->index_x, c->index_y, c->stack_pointer, c->status_flags, access};
    e->trace_head = (e->trace_head + 1) % EXEC_TRACE;
    if (e->trace_count < EXEC_TRACE) ++e->trace_count;
}
static bool breakpoint(NESExecution *e, unsigned access, uint16_t addr, uint8_t value) {
    if (!(e->active_access & access)) return false;
    CPU6502 *c = &e->nes->cpu;
    for (unsigned i = 0; i < EXEC_BREAKPOINTS; ++i) {
        ExecutionBreakpoint *b = &e->breaks[i];
        if (!b->used || !b->enabled || !(b->access & access) || addr < b->first || addr > b->last ||
            (b->value >= 0 && b->value != value) || (b->a >= 0 && b->a != c->accumulator) ||
            (b->x >= 0 && b->x != c->index_x) || (b->y >= 0 && b->y != c->index_y)) continue;
        if (++b->hits <= b->ignore) continue;
        if (b->once) b->enabled = false;
        e->status.breakpoint = (int)i;
        e->status.address = addr; e->status.value = value; e->status.access = access;
        return true;
    }
    return false;
}
static bool count_down(NESExecution *e) { return e->remaining && --e->remaining == 0; }
static bool jam(uint8_t op) {
    return op == 0x02 || op == 0x12 || op == 0x22 || op == 0x32 || op == 0x42 ||
           op == 0x52 || op == 0x62 || op == 0x72 || op == 0x92 || op == 0xB2 || op == 0xD2 || op == 0xF2;
}
void execution_event(NES *nes, ExecutionEvent event, uint16_t addr, uint8_t value) {
    NESExecution *e = nes->execution;
    /* Ignore private renderer snapshots and direct setup/inspection accesses. */
    if (!e || e->nes != nes || !e->running || e->quitting) return;
    bool stop = false;
    if (event == EXEC_EVENT_DOT) {
        replay_input(e);
        if (!e->synchronizing) {
            switch (e->status.mode) {
                case EXEC_PPU_DOT: case EXEC_SCANLINE: case EXEC_FRAME: stop = count_down(e); break;
                case EXEC_NEXT_SCANLINE: stop = nes->ppu.cycle == 0 && count_down(e); break;
                case EXEC_NEXT_FRAME: stop = nes->ppu.scanline == 0 && nes->ppu.cycle == 0 && count_down(e); break;
                case EXEC_LOCATION: stop = nes->ppu.scanline == e->target_line && nes->ppu.cycle == e->target_dot; break;
                default: break;
            }
        }
    } else if (event == EXEC_EVENT_CYCLE) {
        if (!e->synchronizing && e->status.mode == EXEC_CPU_CYCLE) stop = count_down(e);
    } else if (event == EXEC_EVENT_NMI || event == EXEC_EVENT_IRQ) {
        /* The vector is loaded, but cpu_step still has bookkeeping to finish.
           Stop at its return so the handler PC and call stack are both ready. */
        e->interrupt_kind = event == EXEC_EVENT_NMI ? EXEC_CALL_NMI : EXEC_CALL_IRQ;
    } else {
        unsigned access = event == EXEC_EVENT_READ ? EXEC_BREAK_READ : event == EXEC_EVENT_WRITE ? EXEC_BREAK_WRITE :
                          event == EXEC_EVENT_PPU_READ ? EXEC_BREAK_PPU_READ : EXEC_BREAK_PPU_WRITE;
        /* Keep the instruction trace useful: bus operations are recorded on hits. */
        if (!e->synchronizing && !e->replaying && breakpoint(e, access, addr, value)) {
            record(e, access, addr, value);
            park(e, EXEC_STOP_WATCH, true);
        }
    }
    if (e->quitting) return;
    if (stop) park(e, EXEC_STOP_STEP, true);
    /* A quantum boundary is an ownership handoff, not a debugger stop. */
    if (!e->quitting && !e->synchronizing && !e->nes->execution_clock.budget) park(e, EXEC_STOP_BUDGET, false);
}
static void push_call(NESExecution *e, ExecutionCall call) {
    if (e->call_count == EXEC_CALLS) {
        memmove(e->calls, e->calls + 1, (EXEC_CALLS - 1) * sizeof(*e->calls));
        --e->call_count;
    }
    e->calls[e->call_count++] = call;
}
static void update_calls(NESExecution *e, uint16_t pc, uint8_t op, uint8_t sp, bool interrupt) {
    CPU6502 *c = &e->nes->cpu;
    if (interrupt || op == 0x00) push_call(e, (ExecutionCall){pc, c->program_counter,
        (uint16_t)(pc + (interrupt ? 0 : 2)), sp, interrupt ? e->interrupt_kind : EXEC_CALL_BRK});
    else if (op == 0x20) push_call(e, (ExecutionCall){pc, c->program_counter, (uint16_t)(pc + 3), sp, EXEC_CALL_JSR});
    else if (op == 0x60 || op == 0x40) {
        for (unsigned i = e->call_count; i > 0; --i) {
            ExecutionCall *call = &e->calls[i - 1];
            if (call->return_pc == c->program_counter && call->sp == c->stack_pointer) {
                e->call_count = i - 1; break;
            }
        }
    }
}
/* A sleeping real-time worker can migrate between CPU classes on hybrid
   machines. Prefer the faster clock class, while retaining the caller's allowed
   CPUs. Missing topology/frequency data leaves placement to the OS. This only
   changes our worker's affinity, never CPU clocks or system scheduling policy. */
static void place_worker(void) {
#ifdef __linux__
    const char *policy = getenv("NES_EXECUTION_AFFINITY");
    if (policy && !strcmp(policy, "system")) return;
    cpu_set_t allowed, preferred;
    if (sched_getaffinity(0, sizeof(allowed), &allowed) || CPU_COUNT(&allowed) < 2) return;
    unsigned long peaks[CPU_SETSIZE] = {0}, fastest = 0, slowest = 100000000;
    for (unsigned cpu = 0; cpu < CPU_SETSIZE; ++cpu) if (CPU_ISSET(cpu, &allowed)) {
        char path[128];
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%u/cpufreq/cpuinfo_max_freq", cpu);
        FILE *file = fopen(path, "r");
        if (!file) return;
        int read = fscanf(file, "%lu", &peaks[cpu]); fclose(file);
        if (read != 1 || !peaks[cpu] || peaks[cpu] > 100000000) return;
        if (peaks[cpu] > fastest) fastest = peaks[cpu];
        if (peaks[cpu] < slowest) slowest = peaks[cpu];
    }
    if (slowest * 10 >= fastest * 9) return;
    CPU_ZERO(&preferred);
    for (unsigned cpu = 0; cpu < CPU_SETSIZE; ++cpu)
        if (CPU_ISSET(cpu, &allowed) && peaks[cpu] * 10 >= fastest * 9) CPU_SET(cpu, &preferred);
    (void)pthread_setaffinity_np(pthread_self(), sizeof(preferred), &preferred);
#endif
}
#ifdef _WIN32
static DWORD WINAPI worker(void *context)
#else
static void *worker(void *context)
#endif
{
    NESExecution *e = context;
    NES *n = e->nes;
    place_worker();
    park(e, EXEC_STOP_INITIAL, true);
    while (!e->quitting) {
        e->status.boundary = true;
        e->status.instruction_pc = n->cpu.program_counter;
        replay_input(e);
        if (e->replaying && n->cpu.cycle_count>=e->replay_target) park(e, EXEC_STOP_STEP, true);
        bool pending_interrupt = n->cpu.reset_pending || n->cpu.nmi_edge ||
            (n->cpu.irq_poll_valid ? n->cpu.irq_pending :
             (n->cpu.irq_lines || n->lines.irq_line) && !(n->cpu.status_flags & FLAG_INTERRUPT_DISABLE));
        uint8_t op = 0xEA;
        uint64_t generation=e->generation;
        bool known = nes_cpu_peek(n, n->cpu.program_counter, &op);
        if (!e->synchronizing && !e->replaying && !e->bypass && !pending_interrupt) {
            e->status.address = n->cpu.program_counter;
            e->status.access = EXEC_BREAK_EXEC; e->status.value = op; e->status.breakpoint = -1;
            if ((e->pc_breakpoints && e->pc_breakpoints[n->cpu.program_counter]) ||
                breakpoint(e, EXEC_BREAK_EXEC, n->cpu.program_counter, op)) park(e, EXEC_STOP_BREAKPOINT, true);
            else if (known && ((op == 0x00 && e->trap_brk) || (jam(op) && e->trap_jam)))
                park(e, op == 0x00 ? EXEC_STOP_BRK : EXEC_STOP_JAM, true);
            else if (e->status.mode == EXEC_ADDRESS && n->cpu.program_counter == (uint16_t)e->target_line)
                park(e, EXEC_STOP_STEP, true);
        }
        e->bypass = false;
        if (e->quitting) break;
        /* An inspection command can edit/load state while parked at a code
           breakpoint. Refresh decoding after the ownership handoff. */
        if(generation!=e->generation) {
            known = nes_cpu_peek(n, n->cpu.program_counter, &op);
            pending_interrupt = n->cpu.reset_pending || n->cpu.nmi_edge ||
                (n->cpu.irq_poll_valid ? n->cpu.irq_pending :
                 (n->cpu.irq_lines || n->lines.irq_line) && !(n->cpu.status_flags & FLAG_INTERRUPT_DISABLE));
        }
        uint16_t pc = n->cpu.program_counter;
        uint8_t sp = n->cpu.stack_pointer;
        bool stalled = n->cpu.stall_cycles != 0;
        bool resetting = n->cpu.reset_pending;
        bool interrupt = pending_interrupt && !resetting && !stalled;
        e->interrupt_kind = EXEC_CALL_JSR;
        e->status.boundary = false;
        if (e->instruction_observer && !pending_interrupt && !stalled) e->instruction_observer(n, e->observer_context);
        if (known && !pending_interrupt && !stalled) record(e, EXEC_BREAK_EXEC, pc, op);
        nes_clock_tick(n);
        e->status.boundary = true;
        if (e->quitting) break;
        if (!stalled && !resetting) update_calls(e, pc, op, sp, interrupt);
        if (e->observer) e->observer(n, e->observer_context);
        if (e->history_enabled && !e->replaying && (e->last_history_frame!=e->nes->execution_clock.frames ||
            e->status.mode==EXEC_INSTRUCTION || e->status.mode==EXEC_OVER || e->status.mode==EXEC_OUT)) checkpoint(e);
        e->status.instruction_pc = n->cpu.program_counter;
        if (e->synchronizing) park(e, EXEC_STOP_STEP, true);
        else {
            bool done = e->status.mode == EXEC_INSTRUCTION && count_down(e);
            if (((e->status.mode == EXEC_NMI && e->interrupt_kind == EXEC_CALL_NMI) ||
                 (e->status.mode == EXEC_IRQ && e->interrupt_kind == EXEC_CALL_IRQ)) && count_down(e)) done = true;
            if ((e->status.mode == EXEC_OVER || e->status.mode == EXEC_OUT) &&
                n->cpu.program_counter == e->return_pc && n->cpu.stack_pointer == e->return_sp) done = true;
            if (done) park(e, EXEC_STOP_STEP, true);
            else if (e->status.mode == EXEC_PLAY_FRAME && n->frame_ready) park(e, EXEC_STOP_FRAME, true);
            else if (n->cpu.cycle_count - e->start_cycle >= UINT64_C(20000000)) park(e, EXEC_STOP_LIMIT, true);
        }
    }
    lock(e); e->running = false; signal_worker(e); unlock(e);
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}
NESExecution *execution_create(NES *n) {
    if (!n || n->execution) return NULL;
    NESExecution *e = calloc(1, sizeof(*e));
    if (!e) return NULL;
    e->nes = n; e->status.boundary = true; e->status.instruction_pc = n->cpu.program_counter;
    e->status.breakpoint = -1; e->trap_jam = true;
#ifdef _WIN32
    InitializeCriticalSection(&e->mutex); InitializeConditionVariable(&e->condition);
    lock(e);
    e->thread = CreateThread(NULL, 0, worker, e, 0, NULL);
    if (!e->thread) { unlock(e); DeleteCriticalSection(&e->mutex); free(e); return NULL; }
#else
    if (pthread_mutex_init(&e->mutex, NULL)) { free(e); return NULL; }
    if (pthread_cond_init(&e->condition, NULL)) { pthread_mutex_destroy(&e->mutex); free(e); return NULL; }
    lock(e);
    if (pthread_create(&e->thread, NULL, worker, e)) {
        unlock(e); pthread_cond_destroy(&e->condition); pthread_mutex_destroy(&e->mutex); free(e); return NULL;
    }
#endif
    /* Initial handshake: worker signals the initial park. */
    e->running = true;
    while (e->running) wait_worker(e);
    unlock(e);
    n->execution_clock = (NESExecutionClock){0};
    n->execution = e;
    return e;
}
bool execution_request(NESExecution *e, ExecutionMode mode, uint64_t count, int line, int dot) {
    if (!e || mode < EXEC_PAUSE || mode > EXEC_IRQ || !count) return false;
    if ((mode == EXEC_OVER || mode == EXEC_OUT || mode == EXEC_ADDRESS || mode == EXEC_LOCATION) && count != 1) return false;
    if (mode == EXEC_LOCATION && (line < 0 || line > (e->nes->ppu.region == NES_NTSC ? 261 : 311) || dot < 0 || dot > 340)) return false;
    if (mode == EXEC_ADDRESS && (line < 0 || line > 65535)) return false;
    if (count > UINT64_C(10000000)) return false;
    if (mode == EXEC_OVER) {
        uint8_t op;
        if (!e->status.boundary) return false;
        if (nes_cpu_peek(e->nes, e->nes->cpu.program_counter, &op) && (op == 0x20 || op == 0x00)) {
            e->return_pc = e->nes->cpu.program_counter + (op == 0x20 ? 3 : 2);
            e->return_sp = e->nes->cpu.stack_pointer;
        } else mode = EXEC_INSTRUCTION;
    } else if (mode == EXEC_OUT) {
        if (!e->call_count) return false;
        e->return_pc = e->calls[e->call_count - 1].return_pc;
        e->return_sp = e->calls[e->call_count - 1].sp;
    }
    e->status.mode = mode; e->status.pending = mode != EXEC_PAUSE;
    if (mode == EXEC_PAUSE) e->status.reason = EXEC_STOP_PAUSED;
    e->status.breakpoint = -1; e->remaining = count;
    if (mode == EXEC_SCANLINE) e->remaining *= 341;
    if (mode == EXEC_FRAME) e->remaining *= (e->nes->ppu.region == NES_NTSC ? 262u : 312u) * 341u;
    e->target_line = line; e->target_dot = dot;
    e->start_cycle = e->nes->cpu.cycle_count;
    e->bypass = e->status.boundary && ((mode != EXEC_RUN && mode != EXEC_PLAY_FRAME) ||
        e->status.reason == EXEC_STOP_BREAKPOINT || e->status.reason == EXEC_STOP_BRK || e->status.reason == EXEC_STOP_JAM);
    return true;
}
ExecutionStatus execution_pump(NESExecution *e, uint64_t budget) {
    if (!e) return (ExecutionStatus){0};
    if (!e->status.pending && !e->synchronizing) return execution_status(e);
    e->active_access=0;
    for(unsigned i=0;i<EXEC_BREAKPOINTS;++i) if(e->breaks[i].used && e->breaks[i].enabled) e->active_access|=e->breaks[i].access;
    if (e->history_enabled && !e->replaying) {
        ExecutionInput input={.dots=e->nes->execution_clock.dots,.x=e->nes->zapper_x,.y=e->nes->zapper_y,.trigger=e->nes->zapper_trigger};
        memcpy(input.pad,e->nes->controller_state,2);
        ExecutionInput *previous=e->input_count?&e->inputs[e->input_count-1]:NULL;
        if (!previous || memcmp(previous->pad,input.pad,2) || previous->x!=input.x || previous->y!=input.y || previous->trigger!=input.trigger) {
            if (e->input_count==EXEC_INPUTS) memmove(e->inputs,e->inputs+1,(--e->input_count)*sizeof(*e->inputs));
            e->inputs[e->input_count++]=input;
        }
        if(!e->history_count || (e->status.mode!=EXEC_RUN && e->status.mode!=EXEC_PLAY_FRAME)) checkpoint(e);
    }
    e->nes->execution_clock.budget = budget ? budget : 30000;
    unsigned events = (1u << EXEC_EVENT_NMI) | (1u << EXEC_EVENT_IRQ);
    if (!e->synchronizing) {
        if (e->replaying || e->status.mode == EXEC_PPU_DOT || e->status.mode == EXEC_SCANLINE ||
            e->status.mode == EXEC_FRAME || e->status.mode == EXEC_NEXT_SCANLINE ||
            e->status.mode == EXEC_NEXT_FRAME || e->status.mode == EXEC_LOCATION) events |= 1u << EXEC_EVENT_DOT;
        if (e->status.mode == EXEC_CPU_CYCLE) events |= 1u << EXEC_EVENT_CYCLE;
        if (!e->replaying) {
            if (e->active_access & EXEC_BREAK_READ) events |= 1u << EXEC_EVENT_READ;
            if (e->active_access & EXEC_BREAK_WRITE) events |= 1u << EXEC_EVENT_WRITE;
            if (e->active_access & EXEC_BREAK_PPU_READ) events |= 1u << EXEC_EVENT_PPU_READ;
            if (e->active_access & EXEC_BREAK_PPU_WRITE) events |= 1u << EXEC_EVENT_PPU_WRITE;
        }
    }
    e->nes->execution_clock.events = events;
    lock(e); e->running = true; signal_worker(e);
    while (e->running) wait_worker(e);
    unlock(e);
    e->nes->execution_clock.events = 0;
    return execution_status(e);
}
ExecutionStatus execution_status(const NESExecution *e) {
    if (!e) return (ExecutionStatus){.boundary = true};
    ExecutionStatus status = e->status;
    status.dots = e->nes->execution_clock.dots; status.frames = e->nes->execution_clock.frames;
    return status;
}
bool execution_sync(NESExecution *e) {
    if (!e) return true;
    e->status.pending = false;
    if (e->status.boundary) return true;
    e->synchronizing = true;
    execution_pump(e, UINT64_MAX);
    e->synchronizing = false;
    return e->status.boundary;
}
void execution_invalidate(NESExecution *e) {
    if (!e) return;
    e->status.pending = false; e->call_count = e->trace_count = e->trace_head = 0;
    ++e->generation;
    history_clear(e);
    e->nes->execution_clock.dots = e->nes->execution_clock.frames = e->last_history_frame = 0;
    e->status.instruction_pc = e->nes->cpu.program_counter;
    e->status.reason = EXEC_STOP_INITIAL;
}
void execution_destroy(NESExecution *e) {
    if (!e) return;
    execution_sync(e);
    lock(e); e->quitting = true; signal_worker(e); unlock(e);
#ifdef _WIN32
    WaitForSingleObject(e->thread, INFINITE); CloseHandle(e->thread); DeleteCriticalSection(&e->mutex);
#else
    pthread_join(e->thread, NULL); pthread_cond_destroy(&e->condition); pthread_mutex_destroy(&e->mutex);
#endif
    if (e->nes->execution == e) e->nes->execution = NULL;
    history_clear(e);
    free(e);
}
void execution_set_pc_breakpoints(NESExecution *e, bool *b) { if (e) e->pc_breakpoints = b; }
ExecutionBreakpoint *execution_breakpoints(NESExecution *e) { return e ? e->breaks : NULL; }
void execution_set_traps(NESExecution *e, bool brk, bool stop_jam) { if (e) { e->trap_brk = brk; e->trap_jam = stop_jam; } }
void execution_set_observer(NESExecution *e, ExecutionObserver f, void *ctx) { if (e) { e->observer = f; e->observer_context = ctx; } }
void execution_set_instruction_observer(NESExecution *e, ExecutionObserver f) { if (e) e->instruction_observer = f; }
const ExecutionCall *execution_calls(const NESExecution *e, unsigned *count) { if (count) *count = e ? e->call_count : 0; return e ? e->calls : NULL; }
unsigned execution_trace_count(const NESExecution *e) { return e ? e->trace_count : 0; }
const ExecutionTrace *execution_trace(const NESExecution *e, unsigned offset) {
    return e && offset < e->trace_count ? &e->trace[(e->trace_head + EXEC_TRACE - offset - 1) % EXEC_TRACE] : NULL;
}
bool execution_export_trace(const NESExecution *e, const char *path) {
    if (!e || !path) return false;
    FILE *f = fopen(path, "w"); if (!f) return false;
    bool ok = fprintf(f, "cycle,ppu_dots,pc,address,value,access,scanline,dot,a,x,y,sp,p\n") > 0;
    for (unsigned i = e->trace_count; i > 0 && ok; --i) {
        const ExecutionTrace *t = execution_trace(e, i - 1);
        ok = fprintf(f, "%llu,%llu,%04X,%04X,%02X,%u,%d,%d,%02X,%02X,%02X,%02X,%02X\n",
            (unsigned long long)t->cycle, (unsigned long long)t->dots, t->pc, t->address, t->value,
            t->access, t->scanline, t->dot, t->a, t->x, t->y, t->sp, t->flags) > 0;
    }
    if (fclose(f)) ok = false;
    return ok;
}
bool execution_save_breakpoints(const NESExecution *e, const char *path) {
    if (!e) return false;
    uint8_t data[2048]; StateIO io = {data, sizeof(data), 0, false, true};
    state_u32(&io, 0x31474244); /* DBG1 */
    for (unsigned i = 0; i < EXEC_BREAKPOINTS; ++i) {
        const ExecutionBreakpoint *b = &e->breaks[i];
        state_bool(&io, b->used); state_bool(&io, b->enabled); state_bool(&io, b->once);
        state_u32(&io, b->access); state_u16(&io, b->first); state_u16(&io, b->last);
        state_i32(&io, b->value); state_i32(&io, b->a); state_i32(&io, b->x); state_i32(&io, b->y);
        state_u64(&io, b->ignore);
    }
    state_u32(&io, state_crc32(data, io.pos));
    return io.ok && state_atomic_write(path, data, io.pos);
}
bool execution_load_breakpoints(NESExecution *e, const char *path) {
    if (!e || !path) return false;
    uint8_t data[2048]; FILE *f = fopen(path, "rb"); if (!f) return false;
    size_t size = fread(data, 1, sizeof(data), f);
    bool ok = !ferror(f) && fgetc(f) == EOF;
    if (fclose(f)) ok = false;
    StateIO io = {data, size, 0, true, ok};
    if (state_u32(&io, 0) != 0x31474244) return false;
    ExecutionBreakpoint staging[EXEC_BREAKPOINTS] = {0};
    for (unsigned i = 0; i < EXEC_BREAKPOINTS; ++i) {
        ExecutionBreakpoint *b = &staging[i];
        b->used = state_bool(&io, false); b->enabled = state_bool(&io, false); b->once = state_bool(&io, false);
        b->access = state_u32(&io, 0); b->first = state_u16(&io, 0); b->last = state_u16(&io, 0);
        b->value = state_i32(&io, 0); b->a = state_i32(&io, 0); b->x = state_i32(&io, 0); b->y = state_i32(&io, 0);
        b->ignore = state_u64(&io, 0);
        if (b->used && (!b->access || b->access > 31 || b->first > b->last || b->value < -1 || b->value > 255 ||
            b->a < -1 || b->a > 255 || b->x < -1 || b->x > 255 || b->y < -1 || b->y > 255)) io.ok = false;
    }
    uint32_t crc = state_u32(&io, 0);
    if (!io.ok || io.pos != size || size < 4 || crc != state_crc32(data, size - 4)) return false;
    memcpy(e->breaks, staging, sizeof(staging)); return true;
}
const char *execution_stop_name(ExecutionStop stop) {
    static const char *names[] = {"Ready", "Step complete", "Execution breakpoint", "Memory watchpoint",
        "BRK instruction", "CPU JAM instruction", "Seeking...", "Frame complete", "Run limit reached", "Paused"};
    return (unsigned)stop < sizeof(names) / sizeof(*names) ? names[stop] : "Unknown stop";
}

void execution_enable_history(NESExecution *e, bool enabled) {
    if (!e) return;
    e->history_enabled=enabled;
    if (enabled) checkpoint(e); else history_clear(e);
}
bool execution_reverse(NESExecution *e, bool frame) {
    if (!e || !e->history_enabled || !e->history_count) return false;
    uint64_t target=UINT64_MAX;
    if (frame) {
        uint64_t selected_frame=UINT64_MAX;
        for (unsigned i=0;i<e->history_count;++i)
            if (e->history[i].frames<e->nes->execution_clock.frames && e->history[i].cycle<e->nes->cpu.cycle_count &&
                (selected_frame==UINT64_MAX || e->history[i].frames>selected_frame)) {
                target=e->history[i].cycle; selected_frame=e->history[i].frames;
            }
    } else {
        for (unsigned i=0;i<e->trace_count;++i) {
            const ExecutionTrace *t=execution_trace(e,i);
            if (t->access==EXEC_BREAK_EXEC && t->cycle<e->nes->cpu.cycle_count) { target=t->cycle; break; }
        }
    }
    if (target==UINT64_MAX) return false;
    int source=-1;
    for (unsigned i=0;i<e->history_count;++i) if (e->history[i].cycle<=target) source=(int)i;
    if (source<0) return false;
    /* Do not replay past input history that has already been evicted. */
    ExecutionCheckpoint base=e->history[source];
    if (e->input_count==EXEC_INPUTS && base.dots<e->inputs[0].dots) return false;
    e->history_enabled=false;
    bool safe=execution_sync(e);
    e->history_enabled=true;
    if (!safe || nes_state_decode(e->nes,base.data,base.size)!=NES_STATE_OK) return false;
    ++e->generation;
    e->nes->execution_clock.dots=base.dots; e->nes->execution_clock.frames=base.frames;
    e->call_count=base.call_count; memcpy(e->calls,base.calls,sizeof(e->calls));
    while (e->trace_count && execution_trace(e,0)->cycle>=base.cycle) {
        e->trace_head=(e->trace_head+EXEC_TRACE-1)%EXEC_TRACE; --e->trace_count;
    }
    e->replaying=true; e->replay_target=target; e->start_cycle=base.cycle;
    e->status.mode=EXEC_RUN; e->status.pending=true; e->bypass=true;
    if (base.cycle==target) { e->status.pending=false; e->status.instruction_pc=e->nes->cpu.program_counter; }
    unsigned quanta=0;
    while (e->status.pending && quanta++<128) execution_pump(e,30000);
    e->replaying=false;
    bool ok=e->status.boundary && e->nes->cpu.cycle_count==target;
    while (e->history_count && e->history[e->history_count-1].cycle>target) {
        ExecutionCheckpoint *h=&e->history[--e->history_count]; e->history_bytes-=h->size; free(h->data);
    }
    while (e->input_count && e->inputs[e->input_count-1].dots>e->nes->execution_clock.dots) --e->input_count;
    e->status.reason=EXEC_STOP_STEP; e->status.pending=false; e->last_history_frame=e->nes->execution_clock.frames;
    return ok;
}
