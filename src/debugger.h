#ifndef DEBUGGER_H
#define DEBUGGER_H

#include <stdbool.h>
#include <SDL2/SDL.h>
#include "cpu6502.h"
#include "cartridge.h"
#include "ppu2c02.h"
#include "apu2a03.h"

extern bool debugger_active;
extern bool breakpoints[65536];
extern uint16_t debugger_view_pc;
extern int debugger_selected_line;
extern uint16_t debugger_line_pcs[12];

extern const uint8_t op_bytes[256];
uint8_t test_bus_peek(uint16_t address);
void debugger_init(void);
void debugger_step_instruction(CPU6502 *cpu, CPUBus *bus);
void debugger_log_instruction(CPU6502 *cpu);
void debugger_render(SDL_Renderer *renderer, CPU6502 *cpu);
void disassemble_instruction(uint16_t pc, char *out_buf, size_t max_len, CPU6502 *cpu);

#endif