#include "cpu6502.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

// IRQ transitions occur on specified bus cycles, independently of CPU polling.
// Expectations follow the 6502 interrupt/branch timing documented by NESdev.
static CPU6502 cpu;
static uint8_t memory[65536];
static unsigned assert_at, clear_at;
static uint8_t read_memory(void *ctx, uint16_t address) { (void)ctx; return memory[address]; }
static void write_memory(void *ctx, uint16_t address, uint8_t value) { (void)ctx; memory[address] = value; }
static void tick(void *ctx) {
    (void)ctx;
    if (cpu.cycle_count == assert_at) cpu_set_irq_line(&cpu, 0, true);
    if (cpu.cycle_count == clear_at) cpu_set_irq_line(&cpu, 0, false);
}
static CPUBus bus = {NULL, read_memory, write_memory, tick};
static void start(uint8_t opcode, unsigned rise, unsigned fall) {
    memset(memory, 0xEA, sizeof(memory));
    cpu_init(&cpu, CPU_MODEL_RICOH_2A03);
    cpu.program_counter = 0x8000; cpu.status_flags = FLAG_UNUSED;
    memory[0x8000] = opcode;
    memory[0xFFFE] = 0; memory[0xFFFF] = 0x90;
    assert_at = rise; clear_at = fall;
}
static void step(unsigned cycles, unsigned pc) {
    assert(cpu_step(&cpu, &bus) == (int)cycles);
    assert(cpu.program_counter == pc);
}
static void sampled_edges(void) {
    start(0xEA, 1, 0); step(2, 0x8001); step(7, 0x9000);
    start(0xEA, 2, 0); step(2, 0x8001); step(2, 0x8002); step(7, 0x9000);
    start(0xEA, 1, 2); step(2, 0x8001); assert(!cpu.irq_lines); step(7, 0x9000);
    start(0xEA, 1, 0); step(2, 0x8001);
    cpu.stall_cycles = 2; cpu_set_irq_line(&cpu, 0, false);
    step(1, 0x8001); step(1, 0x8001); step(7, 0x9000);
}
static void status_changes(void) {
    start(0x58, 1, 0); cpu.status_flags |= FLAG_INTERRUPT_DISABLE; // CLI
    step(2, 0x8001); step(2, 0x8002); step(7, 0x9000);
    start(0x78, 1, 0); // SEI cannot cancel an already sampled IRQ.
    step(2, 0x8001); assert(cpu.status_flags & FLAG_INTERRUPT_DISABLE); step(7, 0x9000);
    start(0x28, 1, 0); cpu.status_flags |= FLAG_INTERRUPT_DISABLE; // PLP clears I late.
    memory[0x1FE] = FLAG_UNUSED;
    step(4, 0x8001); step(2, 0x8002); step(7, 0x9000);
    start(0x28, 1, 0); memory[0x1FE] = FLAG_UNUSED | FLAG_INTERRUPT_DISABLE;
    step(4, 0x8001); step(7, 0x9000);
    start(0x40, 1, 0); cpu.status_flags |= FLAG_INTERRUPT_DISABLE; // RTI restores I before polling.
    memory[0x1FE] = FLAG_UNUSED; memory[0x1FF] = 0x34; memory[0x100] = 0x81;
    step(6, 0x8134); step(7, 0x9000);
}
static void branch_polls(void) {
    start(0xD0, 2, 0); cpu.status_flags |= FLAG_ZERO; memory[0x8001] = 2;
    step(2, 0x8002); step(2, 0x8003); step(7, 0x9000);
    start(0xD0, 2, 0); memory[0x8001] = 2;
    step(3, 0x8004); step(2, 0x8005); step(7, 0x9000);
    start(0xD0, 3, 0); memory[0x8001] = 0xFC;
    step(4, 0x7FFE); step(7, 0x9000);
    start(0xD0, 1, 2); memory[0x8001] = 0xFC;
    step(4, 0x7FFE); assert(!cpu.irq_lines); step(7, 0x9000);
}
int main(void) {
    sampled_edges(); status_changes(); branch_polls();
    puts("CPU IRQ sampling, CLI/SEI/PLP/RTI, branch polls and stalled boundaries passed.");
    return 0;
}
