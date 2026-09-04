#ifndef CPU6502_H
#define CPU6502_H

#include <stdint.h>
#include <stdbool.h>

enum {
    FLAG_CARRY             = 1 << 0,
    FLAG_ZERO              = 1 << 1,
    FLAG_INTERRUPT_DISABLE = 1 << 2,
    FLAG_DECIMAL_MODE      = 1 << 3,
    FLAG_BREAK_COMMAND     = 1 << 4,
    FLAG_UNUSED            = 1 << 5,
    FLAG_OVERFLOW_V        = 1 << 6,
    FLAG_NEGATIVE          = 1 << 7
};

typedef enum {
    CPU_MODEL_NMOS = 0,
    CPU_MODEL_RICOH_2A03,
    CPU_MODEL_CMOS_65C02
} CPUModel;

typedef struct {
    uint8_t  accumulator;
    uint8_t  index_x;
    uint8_t  index_y;
    uint8_t  stack_pointer;
    uint16_t program_counter;
    uint8_t  status_flags;
    uint64_t cycle_count;
    uint32_t stall_cycles;

    // Hardware lines & latch states
    uint8_t  irq_lines;
    bool     nmi_line;
    bool     nmi_prev_line;
    bool     nmi_edge;
    bool     reset_pending;
    bool     rdy;
    uint8_t  open_bus;
    int      nmi_active_count;

    uint64_t nmi_pulsed_cycle;
    bool     nmi_delayed;

    // Configuration
    CPUModel model;
} CPU6502;

typedef uint8_t (*CPUReadCallback)(void *bus_context, uint16_t address);
typedef void    (*CPUWriteCallback)(void *bus_context, uint16_t address, uint8_t data);
typedef void    (*CPUCycleTickCallback)(void *bus_context);

typedef struct {
    void *bus_context;
    CPUReadCallback      read;
    CPUWriteCallback     write;
    CPUCycleTickCallback cycle_tick;
} CPUBus;

void cpu_init(CPU6502 *cpu, CPUModel model);
void cpu_reset(CPU6502 *cpu, CPUBus *bus);
int  cpu_step(CPU6502 *cpu, CPUBus *bus);

void cpu_set_irq_line(CPU6502 *cpu, uint8_t source_id, bool active);
void cpu_set_nmi_line(CPU6502 *cpu, bool active);
void cpu_pulse_nmi(CPU6502 *cpu);
void cpu_trigger_irq(CPU6502 *cpu);
void cpu_trigger_reset(CPU6502 *cpu);
void cpu_set_rdy(CPU6502 *cpu, bool rdy);
void cpu_pulse_so(CPU6502 *cpu);

#endif