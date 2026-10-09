#include "save_fixture.h"
#include "execution.h"
#include "save_state.h"
#include "diagnostics.h"

static NES *machine;
static NESExecution *control;
static uint8_t *initial;
static size_t initial_size;
static void restore(void) {
    assert(execution_sync(control));
    assert(nes_state_decode(machine, initial, initial_size) == NES_STATE_OK);
    execution_invalidate(control);
}
static ExecutionStatus advance(ExecutionMode mode, uint64_t count, int line, int dot) {
    assert(execution_request(control, mode, count, line, dot));
    ExecutionStatus s;
    unsigned quanta = 0;
    do { s = execution_pump(control, 10000); assert(++quanta < 100); } while (s.pending);
    return s;
}
static void exact_steps_and_replay(void) {
    puts("  dot/cycle pauses preserve interrupted instructions and serialized machine state");
    restore(); nes_clock_tick(machine);
    uint8_t *expected; size_t expected_size;
    assert(nes_state_encode(machine, &expected, &expected_size) == NES_STATE_OK);
    restore();
    uint64_t dots = execution_status(control).dots;
    ExecutionStatus s = advance(EXEC_PPU_DOT, 1, 0, 0);
    assert(s.dots == dots + 1 && !s.boundary && machine->ppu.cycle == 1);
    uint8_t *bad; size_t bad_size;
    assert(nes_state_encode(machine, &bad, &bad_size) == NES_STATE_BUSY);
    assert(execution_sync(control));
    uint8_t *actual; size_t actual_size;
    assert(nes_state_encode(machine, &actual, &actual_size) == NES_STATE_OK);
    assert(actual_size == expected_size && !memcmp(actual, expected, actual_size)); free(actual);
    restore();
    uint64_t cycles = machine->cpu.cycle_count;
    advance(EXEC_CPU_CYCLE, 2, 0, 0);
    assert(machine->cpu.cycle_count == cycles + 2);
    assert(execution_sync(control));
    assert(nes_state_encode(machine, &actual, &actual_size) == NES_STATE_OK);
    assert(actual_size == expected_size && !memcmp(actual, expected, actual_size)); free(actual); free(expected);
    restore(); advance(EXEC_INSTRUCTION, 1, 0, 0);
    assert(machine->cpu.program_counter == 0x0202 && machine->cpu.accumulator == 1);
}
static void video_and_interrupt_targets(void) {
    puts("  scanline/frame/location and interrupt-entry stops are exact and region aware");
    restore(); advance(EXEC_NEXT_SCANLINE, 1, 0, 0);
    assert(machine->ppu.scanline == 1 && machine->ppu.cycle == 0);
    advance(EXEC_NEXT_SCANLINE, 2, 0, 0);
    assert(machine->ppu.scanline == 3 && machine->ppu.cycle == 0);
    advance(EXEC_LOCATION, 1, 4, 17);
    assert(machine->ppu.scanline == 4 && machine->ppu.cycle == 17);
    uint64_t dots = execution_status(control).dots;
    advance(EXEC_SCANLINE, 1, 0, 0);
    assert(execution_status(control).dots == dots + 341);
    advance(EXEC_NEXT_FRAME, 1, 0, 0);
    assert(machine->ppu.scanline == 0 && machine->ppu.cycle == 0);
    restore(); cpu_pulse_nmi(&machine->cpu);
    advance(EXEC_NMI, 1, 0, 0);
    assert(machine->cpu.program_counter == 0x0100 && execution_status(control).boundary);
    assert(execution_status(control).instruction_pc == 0x0100);
    unsigned calls;
    const ExecutionCall *stack = execution_calls(control, &calls);
    assert(calls == 1 && stack[0].kind == EXEC_CALL_NMI);
    advance(EXEC_OUT, 1, 0, 0);
    assert(machine->cpu.program_counter == 0x0200);
    execution_calls(control, &calls); assert(calls == 0);
    restore(); machine->cpu.status_flags &= ~FLAG_INTERRUPT_DISABLE;
    machine->cpu.irq_poll_valid = false;
    machine->lines.irq_line = true;
    advance(EXEC_IRQ, 1, 0, 0);
    assert(machine->cpu.program_counter == 0x0100 && execution_status(control).boundary);
    stack = execution_calls(control, &calls);
    assert(calls == 1 && stack[0].kind == EXEC_CALL_IRQ);
    machine->lines.irq_line = false;
    cpu_set_irq_line(&machine->cpu, 0, false);
    advance(EXEC_OUT, 1, 0, 0);
    assert(machine->cpu.program_counter == 0x0200);
    restore(); nes_set_region(machine, NES_PAL);
    dots = execution_status(control).dots;
    advance(EXEC_FRAME, 1, 0, 0);
    assert(execution_status(control).dots == dots + 312 * 341);
    assert(!execution_request(control, EXEC_LOCATION, 1, 312, 0));
    assert(!execution_request(control, EXEC_OVER, 2, 0, 0));
}
static void playback_and_hook_transitions(void) {
    puts("  fast playback matches direct execution in all regions; precision resumes after access stops");
    for(unsigned region=NES_NTSC;region<=NES_DENDY;++region) {
        restore(); nes_set_region(machine,region); machine->ppu.ppu_mask=0x1E;
        for(unsigned i=0;i<3;++i) {
            machine->frame_ready=false;
            while(!machine->frame_ready) nes_clock_tick(machine);
        }
        uint8_t *expected; size_t size;
        assert(nes_state_encode(machine,&expected,&size)==NES_STATE_OK);
        restore(); nes_set_region(machine,region); machine->ppu.ppu_mask=0x1E;
        for(unsigned i=0;i<3;++i) {
            machine->frame_ready=false;
            assert(advance(EXEC_PLAY_FRAME,1,0,0).reason==EXEC_STOP_FRAME);
        }
        uint8_t *actual; size_t actual_size;
        assert(nes_state_encode(machine,&actual,&actual_size)==NES_STATE_OK);
        assert(size==actual_size && !memcmp(actual,expected,size));
        assert(execution_status(control).frames==2);
        free(expected); free(actual);
    }
    restore(); machine->ppu.ppu_mask=0x1E;
    machine->frame_ready=false; advance(EXEC_PLAY_FRAME,1,0,0);
    ExecutionBreakpoint *b=execution_breakpoints(control);
    b[0]=(ExecutionBreakpoint){.used=true,.enabled=true,.once=true,.access=EXEC_BREAK_PPU_READ,
        .first=0,.last=0x1FFF,.value=-1,.a=-1,.x=-1,.y=-1};
    assert(advance(EXEC_RUN,1,0,0).reason==EXEC_STOP_WATCH && b[0].hits==1);
    uint64_t dots=execution_status(control).dots;
    advance(EXEC_PPU_DOT,1,0,0);
    assert(execution_status(control).dots==dots+1);
    assert(execution_sync(control));
    memset(b,0,EXEC_BREAKPOINTS*sizeof(*b));
    advance(EXEC_CPU_CYCLE,1,0,0);
    dots=execution_status(control).dots;
    advance(EXEC_PPU_DOT,1,0,0);
    assert(execution_status(control).dots==dots+1);
    assert(execution_sync(control));
}
static void breakpoints_and_accesses(void) {
    puts("  conditional/range/one-shot watchpoints preserve RMW and controller side effects");
    restore();
    ExecutionBreakpoint *b = execution_breakpoints(control);
    b[0] = (ExecutionBreakpoint){.used=true,.enabled=true,.once=true,.access=EXEC_BREAK_WRITE,
        .first=0x10,.last=0x10,.value=-1,.a=1,.x=-1,.y=-1,.ignore=1};
    ExecutionStatus s = advance(EXEC_RUN, 1, 0, 0);
    assert(s.reason == EXEC_STOP_WATCH && s.address == 0x10 && s.value == 1 && b[0].hits == 2);
    assert(!b[0].enabled && execution_sync(control));
    memset(b, 0, EXEC_BREAKPOINTS * sizeof(*b));
    restore(); machine->wram[0x200] = 0xAD; machine->wram[0x201] = 0x16; machine->wram[0x202] = 0x40;
    machine->controller_shift[0] = 0x81;
    b[0] = (ExecutionBreakpoint){.used=true,.enabled=true,.access=EXEC_BREAK_READ,
        .first=0x4016,.last=0x4016,.value=-1,.a=-1,.x=-1,.y=-1};
    s = advance(EXEC_RUN, 1, 0, 0);
    assert(s.reason == EXEC_STOP_WATCH && machine->controller_shift[0] == 0xC0);
    assert(execution_sync(control) && machine->controller_shift[0] == 0xC0);
    memset(b, 0, EXEC_BREAKPOINTS * sizeof(*b));
    restore(); machine->wram[0x200]=0xAD; machine->wram[0x201]=0x15; machine->wram[0x202]=0x40;
    advance(EXEC_CPU_CYCLE,4,0,0);
    /* Model distinct internal/external open buses at the pending status read.
       The CPU's bit 5 must be present in both the stop value and accumulator. */
    machine->cpu.open_bus=0x20; machine->cpu_open_bus=0;
    b[0]=(ExecutionBreakpoint){.used=true,.enabled=true,.access=EXEC_BREAK_READ,
        .first=0x4015,.last=0x4015,.value=-1,.a=-1,.x=-1,.y=-1};
    s=advance(EXEC_RUN,1,0,0);
    assert(s.reason==EXEC_STOP_WATCH && (s.value&0x20));
    uint8_t observed=s.value;
    assert(execution_sync(control) && machine->cpu.accumulator==observed && b[0].hits==1);
    memset(b, 0, EXEC_BREAKPOINTS * sizeof(*b));
    restore(); b[0] = (ExecutionBreakpoint){.used=true,.enabled=true,.access=EXEC_BREAK_EXEC,
        .first=0x0202,.last=0x0202,.value=-1,.a=1,.x=-1,.y=-1};
    s = advance(EXEC_RUN, 1, 0, 0);
    assert(s.reason == EXEC_STOP_BREAKPOINT && s.boundary && machine->cpu.program_counter == 0x0202);
    advance(EXEC_INSTRUCTION, 1, 0, 0);
    assert(machine->cpu.program_counter == 0x0204);
    memset(b, 0, EXEC_BREAKPOINTS * sizeof(*b));
}
static void calls_and_run_limits(void) {
    puts("  nested call tracking, step over/out, BRK/JAM traps and bounded seeking");
    restore();
    uint8_t code[] = {0x20,0x20,0x02,0xEA}; memcpy(machine->wram+0x200,code,sizeof(code));
    uint8_t sub[] = {0x20,0x30,0x02,0x60}; memcpy(machine->wram+0x220,sub,sizeof(sub));
    machine->wram[0x230] = 0x60;
    advance(EXEC_INSTRUCTION, 1, 0, 0);
    unsigned count; assert(execution_calls(control,&count) && count == 1);
    advance(EXEC_OUT, 1, 0, 0); assert(machine->cpu.program_counter == 0x0203);
    assert(execution_calls(control,&count) && count == 0);
    machine->cpu.program_counter = 0x0200; execution_invalidate(control);
    advance(EXEC_OVER, 1, 0, 0); assert(machine->cpu.program_counter == 0x0203);
    restore(); machine->wram[0x202] = 0x00; execution_set_traps(control,true,true);
    ExecutionStatus s = advance(EXEC_RUN, 1, 0, 0); assert(s.reason == EXEC_STOP_BRK);
    restore(); machine->wram[0x202] = 0x02;
    s = advance(EXEC_RUN, 1, 0, 0); assert(s.reason == EXEC_STOP_JAM);
    restore(); assert(execution_request(control,EXEC_NMI,1,0,0));
    s = execution_pump(control,10); assert(s.pending && s.reason == EXEC_STOP_BUDGET);
    assert(execution_sync(control));
    execution_set_traps(control,false,true);
}
static void dma_pause_and_persistence(void) {
    puts("  suspension during OAM DMA and validated breakpoint persistence/trace export");
    restore();
    for (unsigned i=0;i<256;++i) machine->wram[0x300+i]=(uint8_t)i;
    nes_cpu_bus_write(machine,0x4014,3);
    uint64_t cycles=machine->cpu.cycle_count;
    advance(EXEC_CPU_CYCLE,20,0,0);
    assert(machine->cpu.cycle_count == cycles+20 && !execution_status(control).boundary);
    assert(execution_sync(control));
    for (unsigned i=0;i<256;++i) {
        uint8_t expected = (uint8_t)i;
        if ((i & 3) == 2) expected &= 0xE3; /* Unimplemented OAM attribute bits. */
        assert(machine->ppu.oam_ram[i] == expected);
    }
    char path[512]; fixture_path(path,"breakpoints.bin");
    ExecutionBreakpoint *b=execution_breakpoints(control);
    b[3]=(ExecutionBreakpoint){.used=true,.enabled=true,.access=EXEC_BREAK_READ|EXEC_BREAK_WRITE,
        .first=0x20,.last=0x30,.value=7,.a=-1,.x=2,.y=-1,.ignore=5};
    assert(execution_save_breakpoints(control,path)); memset(b,0,EXEC_BREAKPOINTS*sizeof(*b));
    assert(execution_load_breakpoints(control,path) && b[3].used && b[3].ignore==5 && b[3].x==2);
    assert(state_atomic_write(path,"DBG1",4));
    assert(!execution_load_breakpoints(control,path) && b[3].used);
    assert(!remove(path)); fixture_path(path,"trace.csv");
    assert(execution_export_trace(control,path) && execution_trace_count(control)); assert(!remove(path));
}
static void reversible_history_and_ppu_inspection(void) {
    puts("  reverse instruction/frame restores checkpoints and replayed controller inputs");
    restore(); execution_enable_history(control,true);
    advance(EXEC_INSTRUCTION,1,0,0);
    uint8_t *expected; size_t expected_size;
    assert(nes_state_encode(machine,&expected,&expected_size)==NES_STATE_OK);
    advance(EXEC_INSTRUCTION,1,0,0);
    assert(machine->wram[0x10]==1);
    assert(execution_reverse(control,false));
    uint8_t *actual; size_t actual_size;
    assert(nes_state_encode(machine,&actual,&actual_size)==NES_STATE_OK);
    assert(actual_size==expected_size && !memcmp(actual,expected,actual_size));
    free(actual); free(expected);
    assert(execution_reverse(control,false) && machine->cpu.program_counter==0x0200);
    advance(EXEC_NEXT_FRAME,1,0,0); assert(execution_sync(control));
    advance(EXEC_NEXT_FRAME,1,0,0); assert(execution_sync(control));
    assert(execution_reverse(control,true) && execution_status(control).frames==1);
    /* Normal run batches keep frame checkpoints rather than one per opcode.
       Rewind must therefore replay historical inputs from the first checkpoint. */
    restore();
    uint8_t poll[]={0xA9,1,0x8D,0x16,0x40,0xA9,0,0x8D,0x16,0x40,
        0xAD,0x16,0x40,0x85,0x10,0x4C,0,2};
    memcpy(machine->wram+0x200,poll,sizeof(poll));
    machine->controller_state[0]=1;
    assert(execution_request(control,EXEC_RUN,1,0,0));
    assert(execution_pump(control,120).pending);
    machine->controller_state[0]=0;
    assert(execution_pump(control,120).pending);
    machine->controller_state[0]=0x81;
    assert(execution_pump(control,120).pending);
    ExecutionTrace previous={0}; bool found=false;
    for(unsigned i=0;i<execution_trace_count(control);++i) {
        const ExecutionTrace *t=execution_trace(control,i);
        if(t->access==EXEC_BREAK_EXEC && t->cycle<machine->cpu.cycle_count) { previous=*t; found=true; break; }
    }
    assert(found);
    machine->controller_state[0]=0xFF;
    assert(execution_reverse(control,false));
    assert(machine->cpu.cycle_count==previous.cycle && machine->cpu.program_counter==previous.pc);
    assert(machine->cpu.accumulator==previous.a && machine->cpu.status_flags==previous.flags);
    assert(machine->controller_state[0]==(previous.dots>=240?0x81:previous.dots>=120?0:1));
    assert(machine->controller_shift[0]==0x81 && machine->controller_strobe==1);
    uint8_t ppu[32]; machine->ppu.palette_ram[0]=0x21;
    uint16_t before=machine->ppu.bus_address;
    assert(nes_ppu_peek_range(machine,0x3F00,ppu,sizeof(ppu)) && ppu[0]==0x21);
    assert(machine->ppu.bus_address==before);
    execution_enable_history(control,false);
}
int main(void) {
    fixture_start("execution"); char path[512]; fixture_path(path,"fixture.nes");
    fixture_rom(path,0,false,true,0); machine=fixture_load(path);
    machine->cpu.program_counter=0x0200;
    uint8_t code[]={0xA9,1,0xE6,0x10,0x4C,0,2}; memcpy(machine->wram+0x200,code,sizeof(code));
    machine->wram[0x100]=0x40;
    assert(nes_state_encode(machine,&initial,&initial_size)==NES_STATE_OK);
    control=execution_create(machine); assert(control);
    exact_steps_and_replay(); video_and_interrupt_targets(); playback_and_hook_transitions(); breakpoints_and_accesses();
    calls_and_run_limits(); dma_pause_and_persistence();
    reversible_history_and_ppu_inspection();
    execution_destroy(control); free(initial); fixture_free(machine);
    assert(!remove(path)); assert(!SAVE_RMDIR(fixture_dir)); puts("Execution controller checks passed.");
}
