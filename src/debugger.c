#include "debugger.h"
#include "nes_system.h"
#include "diagnostics.h"
#include "debug_symbols.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

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
static bool console_open;
static char command_text[192], command_result[128] = "Ctrl+G: command bar   Click a control or select a Debug menu item";
static unsigned workspace_tab;
static uint16_t memory_address;
static bool memory_ppu;
static unsigned trace_offset, break_offset, call_offset, oam_offset;
static uint16_t watches[12];
static unsigned watch_count;
static uint8_t debug_input[2];
static DebugSymbols symbols;

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
    uint8_t value = 0;
    (void)nes_cpu_peek(&nes_sys, address, &value);
    return value;
}

static void peek_text(uint16_t address, char text[3]) {
    uint8_t value;
    if (nes_cpu_peek(&nes_sys, address, &value)) snprintf(text, 3, "%02X", value);
    else memcpy(text, "??", 3);
}

void disassemble_instruction(uint16_t pc, char *out_buf, size_t max_len, CPU6502 *cpu) {
    uint8_t op, b1 = 0, b2 = 0;
    if (!nes_cpu_peek(&nes_sys, pc, &op)) {
        snprintf(out_buf, max_len, "??        UNKNOWN");
        return;
    }
    unsigned size = op_bytes[op];
    bool known = size < 2 || nes_cpu_peek(&nes_sys, (uint16_t)(pc + 1), &b1);
    known = (size < 3 || nes_cpu_peek(&nes_sys, (uint16_t)(pc + 2), &b2)) && known;
    char hex[16], first[3], second[3];
    peek_text((uint16_t)(pc + 1), first);
    peek_text((uint16_t)(pc + 2), second);
    if (size == 1) snprintf(hex, sizeof(hex), "%02X      ", op);
    else if (size == 2) snprintf(hex, sizeof(hex), "%02X %s   ", op, first);
    else snprintf(hex, sizeof(hex), "%02X %s %s", op, first, second);
    if (!known) {
        snprintf(out_buf, max_len, "%s  %s ??", hex, op_names[op]);
        return;
    }
    uint16_t operand = b1 | ((uint16_t)b2 << 8), addr = operand;
    char assembly[64], value[3];
    const char *name = op_names[op];
    switch (op_modes[op]) {
        case MODE_IMP: snprintf(assembly, sizeof(assembly), "%s", name); break;
        case MODE_IMM: snprintf(assembly, sizeof(assembly), "%s #$%02X", name, b1); break;
        case MODE_ZP:
        case MODE_ZPX:
        case MODE_ZPY: {
            char suffix = op_modes[op] == MODE_ZPX ? 'X' : 'Y';
            addr = (uint8_t)(b1 + (op_modes[op] == MODE_ZP ? 0 :
                (suffix == 'X' ? cpu->index_x : cpu->index_y)));
            peek_text(addr, value);
            if (op_modes[op] == MODE_ZP)
                snprintf(assembly, sizeof(assembly), "%s $%02X = #$%s", name, b1, value);
            else snprintf(assembly, sizeof(assembly), "%s $%02X,%c @ $%02X = #$%s", name, b1, suffix, addr, value);
            break;
        }
        case MODE_ABS:
        case MODE_ABSX:
        case MODE_ABSY: {
            char suffix = op_modes[op] == MODE_ABSX ? 'X' : 'Y';
            addr += op_modes[op] == MODE_ABS ? 0 : (suffix == 'X' ? cpu->index_x : cpu->index_y);
            peek_text(addr, value);
            if (op_modes[op] == MODE_ABS)
                snprintf(assembly, sizeof(assembly), "%s $%04X = #$%s", name, operand, value);
            else snprintf(assembly, sizeof(assembly), "%s $%04X,%c @ $%04X = #$%s", name, operand, suffix, addr, value);
            break;
        }
        case MODE_IND: {
            uint8_t low, high;
            uint16_t next = (operand & 0xFF00) | ((operand + 1) & 0xFF);
            if (nes_cpu_peek(&nes_sys, operand, &low) && nes_cpu_peek(&nes_sys, next, &high))
                snprintf(assembly, sizeof(assembly), "%s ($%04X) = $%04X", name, operand, low | (high << 8));
            else snprintf(assembly, sizeof(assembly), "%s ($%04X) = $????", name, operand);
            break;
        }
        case MODE_INDX:
        case MODE_INDY: {
            bool indexed_x = op_modes[op] == MODE_INDX;
            uint8_t ptr = (uint8_t)(b1 + (indexed_x ? cpu->index_x : 0));
            addr = test_bus_peek(ptr) | ((uint16_t)test_bus_peek((uint8_t)(ptr + 1)) << 8);
            if (!indexed_x) addr += cpu->index_y;
            peek_text(addr, value);
            snprintf(assembly, sizeof(assembly), indexed_x ? "%s ($%02X,X) @ $%04X = #$%s" :
                     "%s ($%02X),Y @ $%04X = #$%s", name, b1, addr, value);
            break;
        }
        case MODE_REL:
            snprintf(assembly, sizeof(assembly), "%s $%04X", name, (uint16_t)(pc + 2 + (int8_t)b1));
            break;
        default: snprintf(assembly, sizeof(assembly), "UNKNOWN"); break;
    }
    snprintf(out_buf, max_len, "%s  %s", hex, assembly);
}


void debugger_init(void) {
    debugger_active = false;
    memset(breakpoints, 0, sizeof(breakpoints));
    console_open = false; command_text[0] = 0;
    watch_count = trace_offset = break_offset = workspace_tab = 0;
    call_offset = oam_offset = 0;
    memory_address = 0; memory_ppu = false;
    debugger_set_message("Ctrl+G: commands   Ctrl+1..6: workspace tabs");
    memset(&symbols, 0, sizeof(symbols));
    memset(debug_input, 0, sizeof(debug_input));
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
    (void)bus;
    if (nes_sys.execution) {
        execution_request(nes_sys.execution, EXEC_INSTRUCTION, 1, 0, 0);
        execution_pump(nes_sys.execution, 30000);
    } else {
        debugger_log_instruction(cpu);
        nes_clock_tick(&nes_sys);
    }
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

void debugger_render(HostCanvas *renderer, CPU6502 *cpu) {
    host_color(renderer, 15, 20, 35, 255);
    host_clear(renderer);

    char buf[128];
    draw_string(renderer, debugger_active ? "NES DEBUGGER / INSPECTION" : "NES DEBUGGER / LIVE", 8, 10, 0x8CD6FF);

    snprintf(buf, sizeof(buf), "PC:%04X  A:%02X  X:%02X  Y:%02X  SP:%02X", 
             cpu->program_counter, cpu->accumulator, cpu->index_x, cpu->index_y, cpu->stack_pointer);
    draw_string(renderer, buf, 8, 35, 0xFFFFFF);

    snprintf(buf, sizeof(buf), "P:%02X  [%c%c-%c%c%c%c%c]  CYC:%llu",
             cpu->status_flags,
             (cpu->status_flags & FLAG_NEGATIVE) ? 'N' : '.',
             (cpu->status_flags & FLAG_OVERFLOW_V) ? 'V' : '.',
             (cpu->status_flags & FLAG_BREAK_COMMAND) ? 'B' : '.',
             (cpu->status_flags & FLAG_DECIMAL_MODE) ? 'D' : '.',
             (cpu->status_flags & FLAG_INTERRUPT_DISABLE) ? 'I' : '.',
             (cpu->status_flags & FLAG_ZERO) ? 'Z' : '.',
             (cpu->status_flags & FLAG_CARRY) ? 'C' : '.',
             (unsigned long long)cpu->cycle_count);
    draw_string(renderer, buf, 8, 48, 0x00FFFF);

    draw_string(renderer, "--------------------------------", 0, 60, 0x444444);

    uint16_t dis_pc = debugger_view_pc;
    uint16_t instruction_pc = nes_sys.execution ? execution_status(nes_sys.execution).instruction_pc : cpu->program_counter;
    for (int i = 0; i < 12; i++) {
        debugger_line_pcs[i] = dis_pc;
        char dis_buf[64];
        disassemble_instruction(dis_pc, dis_buf, sizeof(dis_buf), cpu);
        
        char prefix[8] = "  ";
        if (debugger_breakpoint_enabled(dis_pc)) {
            prefix[0] = 'B';
        }
        if (dis_pc == instruction_pc) {
            prefix[1] = '>';
        }
        
        const char *symbol = debug_symbols_name(&symbols, dis_pc);
        snprintf(buf, sizeof(buf), "%s %04X: %.64s%s%.40s", prefix, dis_pc, dis_buf, symbol ? " ; " : "", symbol ? symbol : "");
        uint32_t color = 0x888888;
        
        if (i == debugger_selected_line) {
            color = 0x00FFFF;
            draw_text_field(renderer, buf, 8, 70 + i * 11, 60, false, color);
            draw_string(renderer, "*", 496, 70 + i * 11, color);
        } else {
            if (dis_pc == instruction_pc) {
                color = 0xFFFF00;
            } else if (debugger_breakpoint_enabled(dis_pc)) {
                color = 0xFF00FF;
            }
            draw_text_field(renderer, buf, 8, 70 + i * 11, 60, false, color);
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

    if (debugger_active) draw_string(renderer, "F7:Breakpoint  Ctrl+G:Commands  Shift+F10:Over", 8, 222, 0x91A6BD);
    else {
        char log_status_str[64];
        snprintf(log_status_str, sizeof(log_status_str), "F10:Step|F9:Debugger|Shift+F4:Log:%s", debugger_logging_active ? "ON" : "OFF");
        draw_string(renderer, log_status_str, 8, 222, 0xFF00FF);
        draw_string(renderer, "F7:BRK | Ctrl+UP/DN:Nav | Alt:Menu", 8, 231, 0xFF00FF);
    }
}

void debugger_set_message(const char *text) {
    snprintf(command_result, sizeof(command_result), "%s", text);
}
bool debugger_console_active(void) { return console_open; }
static void follow_execution(void) {
    ExecutionStatus s = execution_status(nes_sys.execution);
    debugger_view_pc = s.instruction_pc;
    debugger_selected_line = 0;
}
bool debugger_request(ExecutionMode mode, uint64_t count, int line, int dot) {
    if (!nes_sys.execution || !execution_request(nes_sys.execution, mode, count, line, dot)) {
        debugger_set_message("Cannot perform this step here. Step to an instruction boundary first."); return false;
    }
    debugger_active = true;
    nes_sys.controller_state[0] = debug_input[0]; nes_sys.controller_state[1] = debug_input[1];
    execution_pump(nes_sys.execution, 30000);
    follow_execution();
    return true;
}
void debugger_update(void) {
    if (!nes_sys.execution || !execution_status(nes_sys.execution).pending) return;
    nes_sys.controller_state[0] = debug_input[0]; nes_sys.controller_state[1] = debug_input[1];
    execution_pump(nes_sys.execution, 30000);
    follow_execution();
}
void debugger_toggle_breakpoint(uint16_t address) {
    ExecutionBreakpoint *items = execution_breakpoints(nes_sys.execution);
    if (!items) { breakpoints[address] = !breakpoints[address]; return; }
    for (unsigned i = 0; i < EXEC_BREAKPOINTS; ++i) {
        if (items[i].used && items[i].access == EXEC_BREAK_EXEC && items[i].first == address && items[i].last == address) {
            items[i].enabled = !items[i].enabled; breakpoints[address] = false; return;
        }
    }
    for (unsigned i = 0; i < EXEC_BREAKPOINTS; ++i) if (!items[i].used) {
        items[i] = (ExecutionBreakpoint){.used=true,.enabled=true,.access=EXEC_BREAK_EXEC,
            .first=address,.last=address,.value=-1,.a=-1,.x=-1,.y=-1};
        breakpoints[address] = false; return;
    }
    debugger_set_message("Breakpoint table is full; delete a breakpoint first.");
}
bool debugger_breakpoint_enabled(uint16_t address) {
    if (breakpoints[address]) return true;
    ExecutionBreakpoint *items=execution_breakpoints(nes_sys.execution);
    for(unsigned i=0;items && i<EXEC_BREAKPOINTS;++i)
        if(items[i].used && items[i].enabled && (items[i].access&EXEC_BREAK_EXEC) && address>=items[i].first && address<=items[i].last) return true;
    return false;
}
static bool number(const char *text, unsigned base, uint64_t max, uint64_t *out) {
    if (!text || !*text || *text == '-') return false;
    if (*text == '$') { ++text; base = 16; }
    char *end; unsigned long long value = strtoull(text, &end, (int)base);
    if (!*text || *end || value > max) {
        uint16_t address;
        if (base==16 && debug_symbols_address(&symbols,text,&address) && address<=max) { *out=address; return true; }
        return false;
    }
    *out = value; return true;
}
static bool command_step(const char *kind, uint64_t count) {
    static const struct { const char *name; ExecutionMode mode; } modes[] = {
        {"instruction",EXEC_INSTRUCTION},{"over",EXEC_OVER},{"out",EXEC_OUT},{"cycle",EXEC_CPU_CYCLE},
        {"dot",EXEC_PPU_DOT},{"scanline",EXEC_SCANLINE},{"frame",EXEC_FRAME},
        {"nextline",EXEC_NEXT_SCANLINE},{"nextframe",EXEC_NEXT_FRAME},{"nmi",EXEC_NMI},{"irq",EXEC_IRQ}};
    for (unsigned i=0;i<sizeof(modes)/sizeof(*modes);++i)
        if (!strcmp(kind,modes[i].name)) return debugger_request(modes[i].mode,count,0,0);
    return false;
}
static bool tokenize(char *buffer, char **tokens, unsigned *count) {
    char *read=buffer,*write=buffer; *count=0;
    while(*read) {
        while(isspace((unsigned char)*read)) ++read;
        if(!*read) break;
        if(*count==20) return false;
        tokens[(*count)++]=write; bool quoted=false;
        while(*read) {
            if(*read=='"') { quoted=!quoted; ++read; continue; }
            if(!quoted && isspace((unsigned char)*read)) { ++read; break; }
            *write++=*read++;
        }
        if(quoted) return false;
        *write++=0;
    }
    return true;
}
bool debugger_command(const char *text) {
    char buffer[192];
    if (!text || strlen(text) >= sizeof(buffer)) return false;
    snprintf(buffer,sizeof(buffer),"%s",text);
    char *tokens[20]; unsigned count=0;
    if(!tokenize(buffer,tokens,&count)) { debugger_set_message("Unclosed quote or too many arguments."); return false; }
    if (!count) return true;
    uint64_t a=0,b=0;
    bool ok=false;
    if (!strcmp(tokens[0],"help")) { workspace_tab=5; ok=true; }
    else if (!strcmp(tokens[0],"view") && count==2) {
        static const char *names[]={"calls","memory","breaks","trace","ppu","help"};
        for(unsigned i=0;i<6;++i) if(!strcmp(tokens[1],names[i])) { workspace_tab=i; ok=true; break; }
    }
    else if (!strcmp(tokens[0],"labels") && count==2) ok=debug_symbols_load(&symbols,tokens[1]);
    else if (!strcmp(tokens[0],"label") && count==3 && number(tokens[1],16,65535,&a)) ok=debug_symbols_add(&symbols,(uint16_t)a,tokens[2]);
    else if (!strcmp(tokens[0],"back") && count==2 && (!strcmp(tokens[1],"instruction") || !strcmp(tokens[1],"frame"))) {
        ok=execution_reverse(nes_sys.execution,!strcmp(tokens[1],"frame"));
        if(ok) follow_execution();
    }
    else if (!strcmp(tokens[0],"goto") && count==2 && number(tokens[1],16,65535,&a)) {
        debugger_view_pc=(uint16_t)a; debugger_selected_line=0; ok=true;
    } else if ((!strcmp(tokens[0],"mem") || !strcmp(tokens[0],"vram")) && count==2 && number(tokens[1],16,65535,&a)) {
        memory_ppu=!strcmp(tokens[0],"vram"); memory_address=(uint16_t)(a & (memory_ppu?0x3FFF:0xFFFF)); workspace_tab=1; ok=true;
    } else if (!strcmp(tokens[0],"watch") && count==2 && number(tokens[1],16,65535,&a) && watch_count<12) {
        watches[watch_count++]=(uint16_t)a; workspace_tab=0; ok=true;
    } else if (!strcmp(tokens[0],"unwatch") && count==1) { watch_count=0; ok=true; }
    else if (!strcmp(tokens[0],"find") && count==4 && number(tokens[1],16,255,&a)) {
        uint64_t first,last;
        if(number(tokens[2],16,65535,&first) && number(tokens[3],16,65535,&last) && first<=last) {
            for(uint32_t addr=(uint32_t)first;addr<=last;++addr) {
                uint8_t value;
                if(nes_cpu_peek(&nes_sys,(uint16_t)addr,&value) && value==a) {
                    memory_address=(uint16_t)addr; memory_ppu=false; workspace_tab=1; ok=true; break;
                }
            }
        }
    }
    else if (!strcmp(tokens[0],"step") && count>=2 && count<=3 &&
             (count==2 || number(tokens[2],10,10000000,&a))) ok=command_step(tokens[1],count==3?a:1);
    else if (!strcmp(tokens[0],"until") && count==2 && number(tokens[1],16,65535,&a)) ok=debugger_request(EXEC_ADDRESS,1,(int)a,0);
    else if (!strcmp(tokens[0],"line") && count>=2 && count<=3 && number(tokens[1],10,311,&a) &&
             (count==2 || number(tokens[2],10,340,&b))) ok=debugger_request(EXEC_LOCATION,1,(int)a,(int)b);
    else if (!strcmp(tokens[0],"input") && count>=2 && count<=3 && number(tokens[1],16,255,&a) &&
             (count==2 || number(tokens[2],16,255,&b))) {
        debug_input[0]=(uint8_t)a; debug_input[1]=(uint8_t)b; ok=true;
    } else if (!strcmp(tokens[0],"set") && count==3 && number(tokens[2],16,65535,&a)) {
        CPU6502 *c=&nes_sys.cpu;
        bool pc=!strcmp(tokens[1],"pc");
        bool reg=pc || !strcmp(tokens[1],"a") || !strcmp(tokens[1],"x") || !strcmp(tokens[1],"y") || !strcmp(tokens[1],"sp") || !strcmp(tokens[1],"p");
        if (reg && (pc || a<=255) && execution_sync(nes_sys.execution)) {
            if (pc) { c->program_counter=(uint16_t)a; ok=true; }
            else if (!strcmp(tokens[1],"a")) { c->accumulator=(uint8_t)a; ok=true; }
            else if (!strcmp(tokens[1],"x")) { c->index_x=(uint8_t)a; ok=true; }
            else if (!strcmp(tokens[1],"y")) { c->index_y=(uint8_t)a; ok=true; }
            else if (!strcmp(tokens[1],"sp")) { c->stack_pointer=(uint8_t)a; ok=true; }
            else if (!strcmp(tokens[1],"p")) { c->status_flags=(uint8_t)a; ok=true; }
            if (ok) { execution_invalidate(nes_sys.execution); follow_execution(); }
        }
    } else if (!strcmp(tokens[0],"poke") && count==3 && number(tokens[1],16,0x1FFF,&a) && number(tokens[2],16,255,&b)) {
        if (execution_sync(nes_sys.execution)) { nes_sys.wram[a&0x7FF]=(uint8_t)b; execution_invalidate(nes_sys.execution); ok=true; }
    } else if ((!strcmp(tokens[0],"bp") || !strcmp(tokens[0],"wp")) && nes_sys.execution) {
        bool exec=!strcmp(tokens[0],"bp"); unsigned address_token=exec?1:2;
        ExecutionBreakpoint item={.used=true,.enabled=true,.value=-1,.a=-1,.x=-1,.y=-1};
        if (exec) item.access=EXEC_BREAK_EXEC;
        else if (count>1) {
            if (!strcmp(tokens[1],"r")) item.access=EXEC_BREAK_READ;
            if (!strcmp(tokens[1],"w")) item.access=EXEC_BREAK_WRITE;
            if (!strcmp(tokens[1],"rw")) item.access=EXEC_BREAK_READ|EXEC_BREAK_WRITE;
            if (!strcmp(tokens[1],"pr")) item.access=EXEC_BREAK_PPU_READ;
            if (!strcmp(tokens[1],"pw")) item.access=EXEC_BREAK_PPU_WRITE;
            if (!strcmp(tokens[1],"prw")) item.access=EXEC_BREAK_PPU_READ|EXEC_BREAK_PPU_WRITE;
        }
        if (count>address_token && item.access) {
            char *range=strchr(tokens[address_token],'-'); if (range) *range++=0;
            if (number(tokens[address_token],16,65535,&a) && (!range || number(range,16,65535,&b))) {
                item.first=(uint16_t)a; item.last=(uint16_t)(range?b:a); ok=item.first<=item.last;
                for (unsigned i=address_token+1;i<count && ok;++i) {
                    char *equal=strchr(tokens[i],'=');
                    if (!strcmp(tokens[i],"once")) item.once=true;
                    else if (equal) {
                        *equal++=0; bool ignore=!strcmp(tokens[i],"ignore");
                        ok=number(equal,ignore?10:16,ignore?UINT32_MAX:255,&a);
                        if (!ok) break;
                        if (ignore) item.ignore=a;
                        else if (!strcmp(tokens[i],"value")) item.value=(int)a;
                        else if (!strcmp(tokens[i],"a")) item.a=(int)a;
                        else if (!strcmp(tokens[i],"x")) item.x=(int)a;
                        else if (!strcmp(tokens[i],"y")) item.y=(int)a;
                        else ok=false;
                    } else ok=false;
                }
                if (ok) {
                    ExecutionBreakpoint *items=execution_breakpoints(nes_sys.execution); ok=false;
                    for (unsigned i=0;i<EXEC_BREAKPOINTS;++i) if (!items[i].used) { items[i]=item; ok=true; break; }
                    workspace_tab=2;
                }
            }
        }
    } else if ((!strcmp(tokens[0],"enable") || !strcmp(tokens[0],"disable") || !strcmp(tokens[0],"delete")) &&
               count==2 && number(tokens[1],10,EXEC_BREAKPOINTS-1,&a) && nes_sys.execution) {
        ExecutionBreakpoint *item=&execution_breakpoints(nes_sys.execution)[a];
        if (item->used) {
            if (item->access==EXEC_BREAK_EXEC && item->first==item->last) breakpoints[item->first]=false;
            if (!strcmp(tokens[0],"delete")) memset(item,0,sizeof(*item));
            else item->enabled=!strcmp(tokens[0],"enable");
            ok=true;
        }
    } else if (!strcmp(tokens[0],"clear") && count==1 && nes_sys.execution) {
        memset(execution_breakpoints(nes_sys.execution),0,EXEC_BREAKPOINTS*sizeof(ExecutionBreakpoint));
        memset(breakpoints,0,sizeof(breakpoints)); ok=true;
    } else if (!strcmp(tokens[0],"trace") && count==2) ok=execution_export_trace(nes_sys.execution,tokens[1]);
    else if (!strcmp(tokens[0],"savebp") && count==2) ok=execution_save_breakpoints(nes_sys.execution,tokens[1]);
    else if (!strcmp(tokens[0],"loadbp") && count==2) { ok=execution_load_breakpoints(nes_sys.execution,tokens[1]); if(ok) memset(breakpoints,0,sizeof(breakpoints)); }
    else if (!strcmp(tokens[0],"trap") && count==3 && number(tokens[1],10,1,&a) && number(tokens[2],10,1,&b)) {
        execution_set_traps(nes_sys.execution,a!=0,b!=0); ok=true;
    }
    debugger_set_message(ok?"Command applied.":"Invalid command. Type help for syntax; addresses and bytes use hex.");
    return ok;
}
static const ExecutionMode toolbar_modes[]={EXEC_INSTRUCTION,EXEC_OVER,EXEC_OUT,EXEC_CPU_CYCLE,EXEC_PPU_DOT,EXEC_NEXT_SCANLINE,EXEC_NEXT_FRAME,EXEC_RUN};
static unsigned breakpoint_count(void) {
    ExecutionBreakpoint *items=execution_breakpoints(nes_sys.execution);
    unsigned count=0;
    for(unsigned i=0;items && i<EXEC_BREAKPOINTS;++i) if(items[i].used) ++count;
    return count;
}
static void scroll_workspace(int rows) {
    if(workspace_tab==1) {
        memory_address=(uint16_t)((memory_address+rows*16)&(memory_ppu?0x3FFF:0xFFFF));
    } else if(workspace_tab<=4) {
        unsigned count=0, visible=0, *offset=NULL;
        if(workspace_tab==0) { execution_calls(nes_sys.execution,&count); visible=7; offset=&call_offset; }
        else if(workspace_tab==2) { count=breakpoint_count(); visible=8; offset=&break_offset; }
        else if(workspace_tab==3) { count=execution_trace_count(nes_sys.execution); visible=16; offset=&trace_offset; }
        else { count=64; visible=10; offset=&oam_offset; }
        unsigned max=count>visible?count-visible:0;
        int next=(int)*offset+rows;
        *offset=next<0?0:(unsigned)next>max?max:(unsigned)next;
    }
}
bool debugger_event(const HostEvent *e, int x, int y) {
    if (!debugger_active) return false;
    if (!console_open && e->type==HOST_KEYDOWN && e->key.keysym.mod==HOST_MOD_CTRL && e->key.keysym.sym>='1' && e->key.keysym.sym<='6') {
        workspace_tab=(unsigned)(e->key.keysym.sym-'1'); return true;
    }
    if (e->type==HOST_KEYDOWN && e->key.keysym.sym=='g' && e->key.keysym.mod==HOST_MOD_CTRL) {
        if (!console_open && !e->key.repeat) { console_open=true; command_text[0]=0; }
        return true;
    }
    if (console_open) {
        if (e->type==HOST_TEXTINPUT && e->character>=32 && e->character<127) {
            size_t len=strlen(command_text); if (len+1<sizeof(command_text)) { command_text[len]=(char)e->character; command_text[len+1]=0; }
        } else if (e->type==HOST_KEYDOWN && !e->key.keysym.mod) {
            if (e->key.keysym.sym==HOST_KEY_ESCAPE) console_open=false;
            else if (e->key.keysym.sym==HOST_KEY_BACKSPACE) { size_t len=strlen(command_text); if(len) command_text[len-1]=0; }
            else if (e->key.keysym.sym==HOST_KEY_RETURN) { debugger_command(command_text); console_open=false; }
        }
        return e->type==HOST_KEYDOWN || e->type==HOST_KEYUP || e->type==HOST_TEXTINPUT;
    }
    if (e->type==HOST_KEYDOWN && !e->key.keysym.mod && workspace_tab<=4 &&
        (e->key.keysym.sym==HOST_KEY_PAGEUP || e->key.keysym.sym==HOST_KEY_PAGEDOWN)) {
        int rows=workspace_tab==0?7:workspace_tab==1?12:workspace_tab==2?8:workspace_tab==3?16:10;
        scroll_workspace(e->key.keysym.sym==HOST_KEY_PAGEUP?-rows:rows); return true;
    }
    if (x<0 || y<0) return false;
    if (e->type==HOST_MOUSEWHEEL) {
        if(e->wheel.y) scroll_workspace(e->wheel.y>0?-1:1);
        return true;
    }
    if (e->type!=HOST_MOUSEBUTTONDOWN) return false;
    if (y>=70 && y<202) {
        debugger_selected_line=(y-70)/11;
        if (e->button.button==HOST_BUTTON_RIGHT) debugger_toggle_breakpoint(debugger_line_pcs[debugger_selected_line]);
        return true;
    }
    if (y>=248 && y<272) {
        unsigned button=(unsigned)x/64;
        if (button<8) {
            if (button==7) execution_request(nes_sys.execution,EXEC_PAUSE,1,0,0);
            else debugger_request(toolbar_modes[button],1,0,0);
        }
        return true;
    }
    if (y>=280 && y<302) { workspace_tab=(unsigned)x/84; if(workspace_tab>5) workspace_tab=5; return true; }
    if (y>=600) { console_open=true; command_text[0]=0; return true; }
    if (workspace_tab==1 && y>=330 && y<498 && x>=48) {
        unsigned column=(unsigned)(x-48)/24, row=(unsigned)(y-330)/14;
        uint16_t address=(uint16_t)(memory_address+row*16+column);
        if(column<16 && !memory_ppu && address<=0x1FFF) {
            snprintf(command_text,sizeof(command_text),"poke $%04X ",address); console_open=true;
        }
        return true;
    }
    if(workspace_tab==2 && y>=334 && y<558) {
        ExecutionBreakpoint *items=execution_breakpoints(nes_sys.execution);
        unsigned selected=break_offset+(unsigned)(y-334)/28, row=0;
        for(unsigned i=0;items && i<EXEC_BREAKPOINTS;++i) if(items[i].used && row++==selected) {
            snprintf(command_text,sizeof(command_text),"%s %u",items[i].enabled?"disable":"enable",i);
            console_open=true; break;
        }
        return true;
    }
    return true;
}
static void workspace_text(HostCanvas *c, const char *s, int y, uint32_t color) { draw_text_field(c,s,8,y,62,false,color); }
static void condition_text(char text[8], int value) {
    if(value<0) strcpy(text,"*");
    else snprintf(text,8,"%02X",(unsigned)(uint8_t)value);
}
void debugger_draw_workspace(HostCanvas *c) {
    static const char *buttons[]={"Into","Over","Out","Cycle","Dot","Line","Frame","Stop"};
    static const char *tabs[]={"Calls","Memory","Breaks","Trace","PPU","Help"};
    char text[192];
    ExecutionStatus status=execution_status(nes_sys.execution);
    snprintf(text,sizeof(text),"%s  %s  PPU %d:%03d",execution_stop_name(status.reason),
        status.boundary?"instruction boundary":"inside instruction",nes_sys.ppu.scanline,nes_sys.ppu.cycle);
    if(status.reason==EXEC_STOP_WATCH) snprintf(text,sizeof(text),"Watch #%d $%04X=$%02X  PPU %d:%03d",status.breakpoint,status.address,status.value,nes_sys.ppu.scanline,nes_sys.ppu.cycle);
    workspace_text(c,text,232,status.pending?0xFFCE70:0x8CD6FF);
    for(unsigned i=0;i<8;++i) {
        HostRect rect={(int)i*64+2,248,60,22}; host_color(c,36,52,73,255); host_fill_rect(c,&rect);
        draw_string(c,buttons[i],rect.x+6,255,0xDCE9F8);
    }
    for(unsigned i=0;i<6;++i) {
        HostRect rect={(int)i*84+2,280,80,22}; host_color(c,i==workspace_tab?50:25,i==workspace_tab?78:36,i==workspace_tab?106:51,255); host_fill_rect(c,&rect);
        draw_string(c,tabs[i],rect.x+12,287,i==workspace_tab?0xFFFFFF:0x91A6BD);
    }
    if(workspace_tab==0) {
        workspace_text(c,"CALL STACK  most recent first; wheel/PageUp/Down",314,0x8CD6FF);
        unsigned count; const ExecutionCall *calls=execution_calls(nes_sys.execution,&count);
        unsigned max=count>7?count-7:0;
        if(call_offset>max) call_offset=max;
        if(!count) workspace_text(c,"No tracked calls. Calls before attaching are unknown.",334,0x91A6BD);
        for(unsigned i=0;i+call_offset<count && i<7;++i) {
            const ExecutionCall *call=&calls[count-i-call_offset-1];
            static const char *kinds[]={"JSR","BRK","NMI","IRQ"};
            snprintf(text,sizeof(text),"%s $%04X -> $%04X  return $%04X  SP:%02X",kinds[call->kind],call->from,call->destination,call->return_pc,call->sp);
            workspace_text(c,text,334+(int)i*14,0xDCE9F8);
        }
        workspace_text(c,"WATCHED MEMORY  (watch $address; unwatch clears)",448,0x8CD6FF);
        for(unsigned i=0;i<watch_count;++i) {
            uint8_t value; bool valid=nes_cpu_peek(&nes_sys,watches[i],&value);
            if(valid) snprintf(text,sizeof(text),"$%04X  $%02X",watches[i],value);
            else snprintf(text,sizeof(text),"$%04X  ??",watches[i]);
            draw_string(c,text,8+(int)(i/6)*248,466+(int)(i%6)*14,0xDCE9F8);
        }
    } else if(workspace_tab==1) {
        snprintf(text,sizeof(text),"%s MEMORY $%04X  wheel or +/-: page",memory_ppu?"PPU":"CPU",memory_address);
        workspace_text(c,text,314,0x8CD6FF);
        uint8_t ppu_bytes[192]; bool valid_ppu=memory_ppu && nes_ppu_peek_range(&nes_sys,memory_address,ppu_bytes,sizeof(ppu_bytes));
        for(unsigned row=0;row<12;++row) {
            uint16_t addr=(uint16_t)((memory_address+row*16)&(memory_ppu?0x3FFF:0xFFFF)); snprintf(text,sizeof(text),"%04X",addr);
            draw_string(c,text,8,330+(int)row*14,0x91A6BD);
            for(unsigned col=0;col<16;++col) {
                uint8_t value=0; bool valid=memory_ppu?valid_ppu:nes_cpu_peek(&nes_sys,addr+col,&value);
                if(memory_ppu && valid) value=ppu_bytes[row*16+col];
                if(valid) snprintf(text,sizeof(text),"%02X",value); else snprintf(text,sizeof(text),"??");
                draw_string(c,text,48+(int)col*24,330+(int)row*14,valid?0xDCE9F8:0x64778C);
            }
        }
        workspace_text(c,"CPU I/O shows ??; inspection never reads live registers.",520,0x91A6BD);
        workspace_text(c,"Click RAM byte to edit. mem $addr / vram $addr",538,0x91A6BD);
    } else if(workspace_tab==2) {
        workspace_text(c,"BREAKPOINTS  wheel/PageUp/Down; click to enable/disable",314,0x8CD6FF);
        unsigned count=breakpoint_count(), max=count>8?count-8:0;
        if(break_offset>max) break_offset=max;
        ExecutionBreakpoint *items=execution_breakpoints(nes_sys.execution); unsigned row=0, skipped=0;
        for(unsigned i=0;items && i<EXEC_BREAKPOINTS && row<8;++i) if(items[i].used) {
            if(skipped++<break_offset) continue;
            ExecutionBreakpoint *b=&items[i];
            char access[8], value[8], a[8], x[8], y[8]; unsigned length=0;
            if(b->access&EXEC_BREAK_EXEC) access[length++]='x';
            if(b->access&EXEC_BREAK_READ) access[length++]='r';
            if(b->access&EXEC_BREAK_WRITE) access[length++]='w';
            if(b->access&(EXEC_BREAK_PPU_READ|EXEC_BREAK_PPU_WRITE)) access[length++]='p';
            if(b->access&EXEC_BREAK_PPU_READ) access[length++]='r';
            if(b->access&EXEC_BREAK_PPU_WRITE) access[length++]='w';
            access[length]=0;
            condition_text(value,b->value); condition_text(a,b->a);
            condition_text(x,b->x); condition_text(y,b->y);
            snprintf(text,sizeof(text),"#%u %s %-5s $%04X-$%04X value:%s%s",i,b->enabled?"on ":"off",access,b->first,b->last,value,b->once?" once":"");
            workspace_text(c,text,334+(int)row*28,b->enabled?0xDCE9F8:0x64778C);
            snprintf(text,sizeof(text),"   A:%s X:%s Y:%s hits:%llu ignore:%llu",a,x,y,(unsigned long long)b->hits,(unsigned long long)b->ignore);
            workspace_text(c,text,346+(int)row++*28,0x91A6BD);
        }
        if(!row) workspace_text(c,"F7/right-click code: toggle. Ctrl+G: bp/wp commands.",334,0x91A6BD);
    } else if(workspace_tab==3) {
        workspace_text(c,"RECENT EXECUTION / WATCH HITS  (wheel to scroll)",314,0x8CD6FF);
        unsigned count=execution_trace_count(nes_sys.execution), max=count>16?count-16:0;
        if(trace_offset>max) trace_offset=max;
        for(unsigned i=0;i<16;++i) {
            const ExecutionTrace *t=execution_trace(nes_sys.execution,trace_offset+i); if(!t) break;
            snprintf(text,sizeof(text),"%llu  PC:%04X A:%02X X:%02X Y:%02X %3d:%03d%s",
                (unsigned long long)t->cycle,t->pc,t->a,t->x,t->y,t->scanline,t->dot,t->access==EXEC_BREAK_EXEC?"":" watch");
            workspace_text(c,text,334+(int)i*14,0xDCE9F8);
        }
    } else if(workspace_tab==4) {
        snprintf(text,sizeof(text),"PPU CTRL:%02X MASK:%02X STATUS:%02X OAM:%02X",nes_sys.ppu.ppu_ctrl,nes_sys.ppu.ppu_mask,nes_sys.ppu.ppu_status,nes_sys.ppu.oam_addr);
        workspace_text(c,text,314,0x8CD6FF);
        snprintf(text,sizeof(text),"SCROLL v:%04X t:%04X fineX:%u latch:%u",nes_sys.ppu.v,nes_sys.ppu.t,nes_sys.ppu.x,nes_sys.ppu.w);
        workspace_text(c,text,334,0xDCE9F8);
        workspace_text(c,"PALETTES (raw bytes; use View > Nametable Viewer)",358,0x8CD6FF);
        for(unsigned row=0;row<2;++row) for(unsigned col=0;col<16;++col) {
            snprintf(text,sizeof(text),"%02X",nes_sys.ppu.palette_ram[row*16+col]);
            draw_string(c,text,48+(int)col*24,378+(int)row*14,0xDCE9F8);
        }
        draw_string(c,"OAM: Y TILE ATTR X",8,416,0x8CD6FF);
        for(unsigned i=0;i<10;++i) {
            unsigned index=oam_offset+i;
            const uint8_t *o=&nes_sys.ppu.oam_ram[index*4];
            snprintf(text,sizeof(text),"%2u %02X %02X %02X %02X",index,o[0],o[1],o[2],o[3]);
            workspace_text(c,text,434+(int)i*14,0xDCE9F8);
        }
        draw_string(c,"wheel/Page: OAM",240,570,0x91A6BD);
        draw_string(c,"CHR $0000",240,416,0x8CD6FF); draw_string(c,"CHR $1000",376,416,0x8CD6FF);
        uint8_t chr[8192];
        if(nes_ppu_peek_range(&nes_sys,0,chr,sizeof(chr))) {
            static const uint32_t colors[]={0xFF1D140F,0xFF826745,0xFFCBBE9C,0xFFF8E9DC};
            for(unsigned table=0;table<2;++table) for(unsigned tile=0;tile<256;++tile) {
                const uint8_t *bits=chr+table*4096+tile*16;
                for(unsigned row=0;row<8;++row) for(unsigned col=0;col<8;++col) {
                    unsigned value=((bits[row]>>(7-col))&1)|(((bits[row+8]>>(7-col))&1)<<1);
                    unsigned px=(table?376:240)+(tile%16)*8+col, py=434+(tile/16)*8+row;
                    c->pixels[py*HOST_PANEL_WIDTH+px]=colors[value];
                }
            }
        }
    } else {
        static const char *help[]={"Ctrl+G: command bar. Addresses/bytes: hex; counts: decimal.",
            "goto $8000      mem $0000       vram $2000",
            "step instruction|over|out|cycle|dot|scanline|frame [count]",
            "step nextline|nextframe|nmi|irq    line 120 17",
            "until $C000    watch $0010    back instruction|frame",
            "bp $8000       wp rw $0000-$07FF",
            "wp w $2000 value=$80 a=$01 ignore=3 once",
            "wp pr $0000-$1FFF   (pr/pw/prw: PPU bus)",
            "enable 0       disable 0       delete 0       clear",
            "set a $7F      set pc $8000    poke $0010 $FF",
            "input $81 $00  (held controller masks while stepping)",
            "labels game.lbl   label $8000 Reset   find FF 0000 07FF",
            "trace trace.csv    savebp breaks.bin    loadbp breaks.bin",
            "Edits and saves finish the current instruction first.",
            "Watchpoints stop AFTER the access; side effects occur once."};
        for(unsigned i=0;i<sizeof(help)/sizeof(*help);++i) workspace_text(c,help[i],314+(int)i*17,i?0xDCE9F8:0x8CD6FF);
    }
    workspace_text(c,command_result,586,0x91A6BD);
    HostRect rect={4,602,504,32}; host_color(c,console_open?35:22,console_open?57:33,console_open?78:48,255); host_fill_rect(c,&rect);
    snprintf(text,sizeof(text),"> %.186s%s",console_open?command_text:"Ctrl+G for commands / Help tab for syntax",console_open?"_":"");
    draw_text_field(c,text,8,612,62,false,0xDCE9F8);
}
