#include "debugger.h"
#include "nes_system.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

bool debugger_active = false;
bool debugger_logging_active = false;

bool breakpoints[65536] = {false};
uint16_t debugger_view_pc = 0;
int debugger_selected_line = 0;
uint16_t debugger_line_pcs[12] = {0};

extern NES nes_sys;

#define LOG_BUFFER_MAX 1024
#define MAX_PATTERN_LEN 16

static char log_buffer[LOG_BUFFER_MAX][256];
static char match_buffer[LOG_BUFFER_MAX][256];
static int log_buffer_count = 0;
static bool atexit_registered = false;

static void process_log_buffer(bool flush_all) {
    FILE *f = fopen("step_trace.log", "a");
    if (!f) return;

    int i = 0;
    int limit = flush_all ? log_buffer_count : (log_buffer_count - 2 * MAX_PATTERN_LEN);

    while (i < limit) {
        bool matched = false;
        int remaining = log_buffer_count - i;

        for (int pat_len = 1; pat_len <= MAX_PATTERN_LEN; pat_len++) {
            if (pat_len * 2 > remaining) break;

            int count = 1;
            while (i + (count + 1) * pat_len <= log_buffer_count) {
                bool match = true;
                for (int k = 0; k < pat_len; k++) {
                    if (strcmp(match_buffer[i + k], match_buffer[i + count * pat_len + k]) != 0) {
                        match = false;
                        break;
                    }
                }
                if (match) {
                    count++;
                } else {
                    break;
                }
            }

            if (count > 2) {
                for (int j = 0; j < pat_len; j++) {
                    fputs(log_buffer[i + j], f);
                }
                fprintf(f, "    ^^^ [Repeated %dx across %d steps] ^^^\n", count, count * pat_len);
                i += count * pat_len;
                matched = true;
                break;
            }
        }

        if (!matched) {
            fputs(log_buffer[i], f);
            i++;
        }
    }

    fclose(f);

    if (i > 0 && i < log_buffer_count) {
        memmove(&log_buffer[0], &log_buffer[i], (log_buffer_count - i) * sizeof(log_buffer[0]));
        memmove(&match_buffer[0], &match_buffer[i], (log_buffer_count - i) * sizeof(match_buffer[0]));
        log_buffer_count -= i;
    } else if (i >= log_buffer_count) {
        log_buffer_count = 0;
    }
}

void debugger_shutdown(void) {
    process_log_buffer(true);
}

extern void draw_string(SDL_Renderer *renderer, const char *str, int x, int y, uint32_t color);

static const char* op_names[256] = {
    "BRK", "ORA", "JAM", "SLO", "NOP", "ORA", "ASL", "SLO", "PHP", "ORA", "ASL", "ANC", "NOP", "ORA", "ASL", "SLO",
    "BPL", "ORA", "JAM", "SLO", "NOP", "ORA", "ASL", "SLO", "CLC", "ORA", "NOP", "SLO", "NOP", "ORA", "ASL", "SLO",
    "JSR", "AND", "JAM", "RLA", "BIT", "AND", "ROL", "RLA", "PLP", "AND", "ROL", "ANC", "BIT", "AND", "ROL", "RLA",
    "BMI", "AND", "JAM", "RLA", "NOP", "AND", "ROL", "RLA", "SEC", "AND", "NOP", "RLA", "NOP", "AND", "ROL", "RLA",
    "RTI", "EOR", "JAM", "SRE", "NOP", "EOR", "LSR", "SRE", "PHA", "EOR", "LSR", "ALR", "JMP", "EOR", "LSR", "SRE",
    "BVC", "EOR", "JAM", "SRE", "NOP", "EOR", "LSR", "SRE", "CLI", "EOR", "NOP", "SRE", "NOP", "EOR", "LSR", "SRE",
    "RTS", "ADC", "JAM", "RRA", "NOP", "ADC", "ROR", "RRA", "PLA", "ADC", "ROR", "ARR", "JMP", "ADC", "ROR", "RRA",
    "BVS", "ADC", "JAM", "RRA", "NOP", "ADC", "ROR", "RRA", "SEI", "ADC", "NOP", "RRA", "NOP", "ADC", "ROR", "RRA",
    "NOP", "STA", "NOP", "SAX", "STY", "STA", "STX", "SAX", "DEY", "NOP", "TXA", "XAA", "STY", "STA", "STX", "SAX",
    "BCC", "STA", "JAM", "SHA", "STY", "STA", "STX", "SAX", "TYA", "STA", "TXS", "TAS", "SHY", "STA", "SHX", "SHA",
    "LDY", "LDA", "LDX", "LAX", "LDY", "LDA", "LDX", "LAX", "TAY", "LDA", "TAX", "ATX", "LDY", "LDA", "LDX", "LAX",
    "BCS", "LDA", "JAM", "LAX", "LDY", "LDA", "LDX", "LAX", "CLV", "LDA", "TSX", "LAS", "LDY", "LDA", "LDX", "LAX",
    "CPY", "CMP", "NOP", "DCP", "CPY", "CMP", "DEC", "DCP", "INY", "CMP", "DEX", "AXS", "CPY", "CMP", "DEC", "DCP",
    "BNE", "CMP", "JAM", "DCP", "NOP", "CMP", "DEC", "DCP", "CLD", "CMP", "NOP", "DCP", "NOP", "CMP", "DEC", "DCP",
    "CPX", "SBC", "NOP", "ISC", "CPX", "SBC", "INC", "ISC", "INX", "SBC", "NOP", "SBC", "CPX", "SBC", "INC", "ISC",
    "BEQ", "SBC", "JAM", "ISC", "NOP", "SBC", "INC", "ISC", "SED", "SBC", "NOP", "ISC", "NOP", "SBC", "INC", "ISC"
};

const uint8_t op_bytes[256] = {
    2, 2, 1, 2, 2, 2, 2, 2, 1, 2, 1, 2, 3, 3, 3, 3,
    2, 2, 1, 2, 2, 2, 2, 2, 1, 3, 1, 3, 3, 3, 3, 3,
    3, 2, 1, 2, 2, 2, 2, 2, 1, 2, 1, 2, 3, 3, 3, 3,
    2, 2, 1, 2, 2, 2, 2, 2, 1, 3, 1, 3, 3, 3, 3, 3,
    1, 2, 1, 2, 2, 2, 2, 2, 1, 2, 1, 2, 3, 3, 3, 3,
    2, 2, 1, 2, 2, 2, 2, 2, 1, 3, 1, 3, 3, 3, 3, 3,
    1, 2, 1, 2, 2, 2, 2, 2, 1, 2, 1, 2, 3, 3, 3, 3,
    2, 2, 1, 2, 2, 2, 2, 2, 1, 3, 1, 3, 3, 3, 3, 3,
    2, 2, 2, 2, 2, 2, 2, 2, 1, 2, 1, 2, 3, 3, 3, 3,
    2, 2, 1, 2, 2, 2, 2, 2, 1, 3, 1, 3, 3, 3, 3, 3,
    2, 2, 2, 2, 2, 2, 2, 2, 1, 2, 1, 2, 3, 3, 3, 3,
    2, 2, 1, 2, 2, 2, 2, 2, 1, 3, 1, 3, 3, 3, 3, 3,
    2, 2, 2, 2, 2, 2, 2, 2, 1, 2, 1, 2, 3, 3, 3, 3,
    2, 2, 1, 2, 2, 2, 2, 2, 1, 3, 1, 3, 3, 3, 3, 3,
    2, 2, 2, 2, 2, 2, 2, 2, 1, 2, 1, 2, 3, 3, 3, 3,
    2, 2, 1, 2, 2, 2, 2, 2, 1, 3, 1, 3, 3, 3, 3, 3
};

typedef enum {
    MODE_IMP, MODE_IMM, MODE_ZP, MODE_ZPX, MODE_ZPY,
    MODE_ABS, MODE_ABSX, MODE_ABSY, MODE_IND, MODE_INDX,
    MODE_INDY, MODE_REL
} AddrMode;

static const uint8_t op_modes[256] = {
    MODE_IMP, MODE_INDX, MODE_IMP, MODE_INDX, MODE_ZP, MODE_ZP, MODE_ZP, MODE_ZP, MODE_IMP, MODE_IMM, MODE_IMP, MODE_IMM, MODE_ABS, MODE_ABS, MODE_ABS, MODE_ABS,
    MODE_REL, MODE_INDY, MODE_IMP, MODE_INDY, MODE_ZPX, MODE_ZPX, MODE_ZPX, MODE_ZPX, MODE_IMP, MODE_ABSY, MODE_IMP, MODE_ABSY, MODE_ABSX, MODE_ABSX, MODE_ABSX, MODE_ABSX,
    MODE_ABS, MODE_INDX, MODE_IMP, MODE_INDX, MODE_ZP, MODE_ZP, MODE_ZP, MODE_ZP, MODE_IMP, MODE_IMM, MODE_IMP, MODE_IMM, MODE_ABS, MODE_ABS, MODE_ABS, MODE_ABS,
    MODE_REL, MODE_INDY, MODE_IMP, MODE_INDY, MODE_ZPX, MODE_ZPX, MODE_ZPX, MODE_ZPX, MODE_IMP, MODE_ABSY, MODE_IMP, MODE_ABSY, MODE_ABSX, MODE_ABSX, MODE_ABSX, MODE_ABSX,
    MODE_IMP, MODE_INDX, MODE_IMP, MODE_INDX, MODE_ZP, MODE_ZP, MODE_ZP, MODE_ZP, MODE_IMP, MODE_IMM, MODE_IMP, MODE_IMM, MODE_ABS, MODE_ABS, MODE_ABS, MODE_ABS,
    MODE_REL, MODE_INDY, MODE_IMP, MODE_INDY, MODE_ZPX, MODE_ZPX, MODE_ZPX, MODE_ZPX, MODE_IMP, MODE_ABSY, MODE_IMP, MODE_ABSY, MODE_ABSX, MODE_ABSX, MODE_ABSX, MODE_ABSX,
    MODE_IMP, MODE_INDX, MODE_IMP, MODE_INDX, MODE_ZP, MODE_ZP, MODE_ZP, MODE_ZP, MODE_IMP, MODE_IMM, MODE_IMP, MODE_IMM, MODE_IND, MODE_ABS, MODE_ABS, MODE_ABS,
    MODE_REL, MODE_INDY, MODE_IMP, MODE_INDY, MODE_ZPX, MODE_ZPX, MODE_ZPX, MODE_ZPX, MODE_IMP, MODE_ABSY, MODE_IMP, MODE_ABSY, MODE_ABSX, MODE_ABSX, MODE_ABSX, MODE_ABSX,
    MODE_IMM, MODE_INDX, MODE_IMM, MODE_INDX, MODE_ZP, MODE_ZP, MODE_ZP, MODE_ZP, MODE_IMP, MODE_IMM, MODE_IMP, MODE_IMM, MODE_ABS, MODE_ABS, MODE_ABS, MODE_ABS,
    MODE_REL, MODE_INDY, MODE_IMP, MODE_INDY, MODE_ZPX, MODE_ZPX, MODE_ZPY, MODE_ZPY, MODE_IMP, MODE_ABSY, MODE_IMP, MODE_ABSY, MODE_ABSX, MODE_ABSX, MODE_ABSY, MODE_ABSY,
    MODE_IMM, MODE_INDX, MODE_IMM, MODE_INDX, MODE_ZP, MODE_ZP, MODE_ZP, MODE_ZP, MODE_IMP, MODE_IMM, MODE_IMP, MODE_IMM, MODE_ABS, MODE_ABS, MODE_ABS, MODE_ABS,
    MODE_REL, MODE_INDY, MODE_IMP, MODE_INDY, MODE_ZPX, MODE_ZPX, MODE_ZPY, MODE_ZPY, MODE_IMP, MODE_ABSY, MODE_IMP, MODE_ABSY, MODE_ABSX, MODE_ABSX, MODE_ABSY, MODE_ABSY,
    MODE_IMM, MODE_INDX, MODE_IMM, MODE_INDX, MODE_ZP, MODE_ZP, MODE_ZP, MODE_ZP, MODE_IMP, MODE_IMM, MODE_IMP, MODE_IMM, MODE_ABS, MODE_ABS, MODE_ABS, MODE_ABS,
    MODE_REL, MODE_INDY, MODE_IMP, MODE_INDY, MODE_ZPX, MODE_ZPX, MODE_ZPX, MODE_ZPX, MODE_IMP, MODE_ABSY, MODE_IMP, MODE_ABSY, MODE_ABSX, MODE_ABSX, MODE_ABSX, MODE_ABSX,
    MODE_IMM, MODE_INDX, MODE_IMM, MODE_INDX, MODE_ZP, MODE_ZP, MODE_ZP, MODE_ZP, MODE_IMP, MODE_IMM, MODE_IMP, MODE_IMM, MODE_ABS, MODE_ABS, MODE_ABS, MODE_ABS,
    MODE_REL, MODE_INDY, MODE_IMP, MODE_INDY, MODE_ZPX, MODE_ZPX, MODE_ZPX, MODE_ZPX, MODE_IMP, MODE_ABSY, MODE_IMP, MODE_ABSY, MODE_ABSX, MODE_ABSX, MODE_ABSX, MODE_ABSX
};

uint8_t test_bus_peek(uint16_t address) {
    if (address < 0x2000) {
        return nes_sys.wram[address & 0x07FF];
    }
    if (address >= 0x6000 && address <= 0x7FFF && nes_sys.cart && nes_sys.cart->prg_ram) {
        return nes_sys.cart->prg_ram[address - 0x6000];
    }
    if (address >= 0x8000 && nes_sys.cart && nes_sys.cart->prg_rom) {
        bool handled = false;
        if (nes_sys.cart->vtable && nes_sys.cart->vtable->cpu_read) {
            return nes_sys.cart->vtable->cpu_read(nes_sys.cart, address, &handled);
        }
    }
    return 0;
}

void disassemble_instruction(uint16_t pc, char *out_buf, size_t max_len, CPU6502 *cpu) {
    uint8_t op = test_bus_peek(pc);
    uint8_t b1 = test_bus_peek(pc + 1);
    uint8_t b2 = test_bus_peek(pc + 2);
    uint16_t operand = b1 | (b2 << 8);

    char hex_str[16];
    if (op_bytes[op] == 1) {
        snprintf(hex_str, sizeof(hex_str), "%02X      ", op);
    } else if (op_bytes[op] == 2) {
        snprintf(hex_str, sizeof(hex_str), "%02X %02X   ", op, b1);
    } else {
        snprintf(hex_str, sizeof(hex_str), "%02X %02X %02X", op, b1, b2);
    }

    char asm_str[64];
    switch (op_modes[op]) {
        case MODE_IMP:
            snprintf(asm_str, sizeof(asm_str), "%s", op_names[op]);
            break;
        case MODE_IMM:
            snprintf(asm_str, sizeof(asm_str), "%s #$%02X", op_names[op], b1);
            break;
        case MODE_ZP:
            snprintf(asm_str, sizeof(asm_str), "%s $%02X = #$%02X", op_names[op], b1, test_bus_peek(b1));
            break;
        case MODE_ZPX: {
            uint8_t addr = (b1 + cpu->index_x) & 0xFF;
            snprintf(asm_str, sizeof(asm_str), "%s $%02X,X @ $%02X = #$%02X", op_names[op], b1, addr, test_bus_peek(addr));
            break;
        }
        case MODE_ZPY: {
            uint8_t addr = (b1 + cpu->index_y) & 0xFF;
            snprintf(asm_str, sizeof(asm_str), "%s $%02X,Y @ $%02X = #$%02X", op_names[op], b1, addr, test_bus_peek(addr));
            break;
        }
        case MODE_ABS: {
            snprintf(asm_str, sizeof(asm_str), "%s $%04X = #$%02X", op_names[op], operand, test_bus_peek(operand));
            break;
        }
        case MODE_ABSX: {
            uint16_t addr = operand + cpu->index_x;
            snprintf(asm_str, sizeof(asm_str), "%s $%04X,X @ $%04X = #$%02X", op_names[op], operand, addr, test_bus_peek(addr));
            break;
        }
        case MODE_ABSY: {
            uint16_t addr = operand + cpu->index_y;
            snprintf(asm_str, sizeof(asm_str), "%s $%04X,Y @ $%04X = #$%02X", op_names[op], operand, addr, test_bus_peek(addr));
            break;
        }
        case MODE_IND: {
            uint16_t target;
            if ((operand & 0x00FF) == 0x00FF) {
                uint8_t low = test_bus_peek(operand);
                uint8_t high = test_bus_peek(operand & 0xFF00);
                target = low | (high << 8);
            } else {
                target = test_bus_peek(operand) | (test_bus_peek(operand + 1) << 8);
            }
            snprintf(asm_str, sizeof(asm_str), "%s ($%04X) = $%04X", op_names[op], operand, target);
            break;
        }
        case MODE_INDX: {
            uint8_t ptr = (b1 + cpu->index_x) & 0xFF;
            uint16_t addr = test_bus_peek(ptr) | (test_bus_peek((ptr + 1) & 0xFF) << 8);
            snprintf(asm_str, sizeof(asm_str), "%s ($%02X,X) @ $%04X = #$%02X", op_names[op], b1, addr, test_bus_peek(addr));
            break;
        }
        case MODE_INDY: {
            uint16_t base = test_bus_peek(b1) | (test_bus_peek((b1 + 1) & 0xFF) << 8);
            uint16_t addr = base + cpu->index_y;
            snprintf(asm_str, sizeof(asm_str), "%s ($%02X),Y @ $%04X = #$%02X", op_names[op], b1, addr, test_bus_peek(addr));
            break;
        }
        case MODE_REL: {
            uint16_t target = pc + 2 + (int8_t)b1;
            snprintf(asm_str, sizeof(asm_str), "%s $%04X", op_names[op], target);
            break;
        }
    }
    snprintf(out_buf, max_len, "%s  %s", hex_str, asm_str);
}

void debugger_init(void) {
    debugger_active = false;
    memset(breakpoints, 0, sizeof(breakpoints));
    debugger_view_pc = 0;
    debugger_selected_line = 0;
    FILE *log_file = fopen("step_trace.log", "w");
    if (log_file) {
        fclose(log_file);
    }
    log_buffer_count = 0;
    if (!atexit_registered) {
        atexit(debugger_shutdown);
        atexit_registered = true;
    }
}

void debugger_step_instruction(CPU6502 *cpu, CPUBus *bus) {
    debugger_log_instruction(cpu);
    cpu_step(cpu, bus);
    debugger_view_pc = cpu->program_counter;
    debugger_selected_line = 0;
}

void debugger_log_instruction(CPU6502 *cpu) {
    if (!debugger_logging_active) {
        return;
    }

    if (log_buffer_count >= 800) {
        process_log_buffer(false);
    }

    char disasm[128];
    disassemble_instruction(cpu->program_counter, disasm, sizeof(disasm), cpu);

    char *line = log_buffer[log_buffer_count];
    int len = snprintf(line, sizeof(log_buffer[0]),
            "PC:%04X  %-30s A:%02X X:%02X Y:%02X P:%02X SP:%02X CYC:%lld SL:%d DOT:%d",
            cpu->program_counter, disasm,
            cpu->accumulator, cpu->index_x, cpu->index_y,
            cpu->status_flags, cpu->stack_pointer,
            (long long)cpu->cycle_count,
            nes_sys.ppu.scanline, nes_sys.ppu.cycle);

    if (nes_sys.cart) {
        len += snprintf(line + len, sizeof(log_buffer[0]) - len, " | M%d", nes_sys.cart->mapper_id);
    }
    snprintf(line + len, sizeof(log_buffer[0]) - len, "\n");

    char *cyc_ptr = strstr(line, " CYC:");
    int match_len = cyc_ptr ? (int)(cyc_ptr - line) : (int)strlen(line);
    if (match_len >= (int)sizeof(match_buffer[0])) {
        match_len = sizeof(match_buffer[0]) - 1;
    }
    memcpy(match_buffer[log_buffer_count], line, match_len);
    match_buffer[log_buffer_count][match_len] = '\0';

    log_buffer_count++;
}

void debugger_render(SDL_Renderer *renderer, CPU6502 *cpu) {
    SDL_SetRenderDrawColor(renderer, 15, 20, 35, 255);
    SDL_RenderClear(renderer);

    char buf[128];
    draw_string(renderer, "NES IN-GAME STEP DEBUGGER", 32, 10, 0x00FF00);
    draw_string(renderer, "=========================", 32, 20, 0x00FF00);

    snprintf(buf, sizeof(buf), "PC:%04X  A:%02X  X:%02X  Y:%02X  SP:%02X", 
             cpu->program_counter, cpu->accumulator, cpu->index_x, cpu->index_y, cpu->stack_pointer);
    draw_string(renderer, buf, 8, 35, 0xFFFFFF);

    snprintf(buf, sizeof(buf), "P:%02X  [%c%c-%c%c%c%c%c]  CYC:%lld", 
             cpu->status_flags,
             (cpu->status_flags & FLAG_NEGATIVE) ? 'N' : '.',
             (cpu->status_flags & FLAG_OVERFLOW_V) ? 'V' : '.',
             (cpu->status_flags & FLAG_BREAK_COMMAND) ? 'B' : '.',
             (cpu->status_flags & FLAG_DECIMAL_MODE) ? 'D' : '.',
             (cpu->status_flags & FLAG_INTERRUPT_DISABLE) ? 'I' : '.',
             (cpu->status_flags & FLAG_ZERO) ? 'Z' : '.',
             (cpu->status_flags & FLAG_CARRY) ? 'C' : '.',
             (long long)cpu->cycle_count);
    draw_string(renderer, buf, 8, 48, 0x00FFFF);

    draw_string(renderer, "--------------------------------", 0, 60, 0x444444);

    uint16_t dis_pc = debugger_view_pc;
    for (int i = 0; i < 12; i++) {
        debugger_line_pcs[i] = dis_pc;
        char dis_buf[64];
        disassemble_instruction(dis_pc, dis_buf, sizeof(dis_buf), cpu);
        
        char prefix[8] = "  ";
        if (breakpoints[dis_pc]) {
            prefix[0] = 'B';
        }
        if (dis_pc == cpu->program_counter) {
            prefix[1] = '>';
        }
        
        snprintf(buf, sizeof(buf), "%s %04X: %s", prefix, dis_pc, dis_buf);
        uint32_t color = 0x888888;
        
        if (i == debugger_selected_line) {
            color = 0x00FFFF;
            char select_buf[150];
            snprintf(select_buf, sizeof(select_buf), "%s *", buf);
            draw_string(renderer, select_buf, 8, 70 + i * 11, color);
        } else {
            if (dis_pc == cpu->program_counter) {
                color = 0xFFFF00;
            } else if (breakpoints[dis_pc]) {
                color = 0xFF00FF;
            }
            draw_string(renderer, buf, 8, 70 + i * 11, color);
        }
        
        uint8_t op = test_bus_peek(dis_pc);
        dis_pc += op_bytes[op] ? op_bytes[op] : 1;
    }

    draw_string(renderer, "--------------------------------", 0, 202, 0x444444);

    uint8_t sp = cpu->stack_pointer;
    uint8_t s1 = nes_sys.wram[0x0100 | ((sp + 1) & 0xFF)];
    uint8_t s2 = nes_sys.wram[0x0100 | ((sp + 2) & 0xFF)];
    uint8_t s3 = nes_sys.wram[0x0100 | ((sp + 3) & 0xFF)];
    uint8_t s4 = nes_sys.wram[0x0100 | ((sp + 4) & 0xFF)];
    snprintf(buf, sizeof(buf), "Stack: [ %02X %02X %02X %02X ]", s1, s2, s3, s4);
    draw_string(renderer, buf, 8, 210, 0x00FF00);

    char log_status_str[64];
    snprintf(log_status_str, sizeof(log_status_str), "F10:Step|F9:Run|F6:Log:%s", debugger_logging_active ? "ON" : "OFF");
    draw_string(renderer, log_status_str, 8, 222, 0xFF00FF);

    draw_string(renderer, "F7:BRK | UP/DN:Nav | ESC:Menu", 8, 231, 0xFF00FF);
}