/* cpu6502.h */
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

typedef struct {
    uint8_t  accumulator;
    uint8_t  index_x;
    uint8_t  index_y;
    uint8_t  stack_pointer;
    uint16_t program_counter;
    uint8_t  status_flags;
    uint64_t cycle_count;

    bool nmi_pending;
    bool irq_pending;
    bool reset_pending;
    bool page_crossed;
    bool decimal_mode;
} CPU6502;

typedef uint8_t (*CPUReadCallback)(void *bus_context, uint16_t address);
typedef void    (*CPUWriteCallback)(void *bus_context, uint16_t address, uint8_t data);

typedef struct {
    void *bus_context;
    CPUReadCallback  read;
    CPUWriteCallback write;
} CPUBus;

void cpu_init(CPU6502 *cpu);
void cpu_reset(CPU6502 *cpu, CPUBus *bus);
int  cpu_step(CPU6502 *cpu, CPUBus *bus);
void cpu_trigger_nmi(CPU6502 *cpu);
void cpu_trigger_irq(CPU6502 *cpu);
void cpu_trigger_reset(CPU6502 *cpu);
bool cpu_is_opcode_implemented(uint8_t opcode);

#endif