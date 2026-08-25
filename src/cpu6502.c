#include "cpu6502.h"
#include <stdio.h>

static const uint16_t VECTOR_NMI   = 0xFFFA;
static const uint16_t VECTOR_RESET = 0xFFFC;
static const uint16_t VECTOR_IRQ   = 0xFFFE;

static inline void set_flag(CPU6502 *cpu, uint8_t flag, bool condition) {
    if (condition) {
        cpu->status_flags |= flag;
    } else {
        cpu->status_flags &= ~flag;
    }
}

static inline bool get_flag(const CPU6502 *cpu, uint8_t flag) {
    return (cpu->status_flags & flag) != 0;
}

static inline void update_zero_and_negative_flags(CPU6502 *cpu, uint8_t value) {
    set_flag(cpu, FLAG_ZERO, value == 0);
    set_flag(cpu, FLAG_NEGATIVE, (value & 0x80) != 0);
}

static inline void bus_cycle(CPU6502 *cpu, CPUBus *bus) {
    cpu->cycle_count++;
    if (bus->tick) {
        bus->tick(bus->bus_context);
    }
}

static inline uint8_t read_byte(CPU6502 *cpu, CPUBus *bus, uint16_t address) {
    while (!cpu->rdy) {
        bus_cycle(cpu, bus);
    }
    bus_cycle(cpu, bus);
    uint8_t data = bus->read ? bus->read(bus->bus_context, address) : cpu->open_bus;
    cpu->open_bus = data;
    return data;
}

static inline void write_byte(CPU6502 *cpu, CPUBus *bus, uint16_t address, uint8_t data) {
    bus_cycle(cpu, bus);
    cpu->open_bus = data;
    if (bus->write) {
        bus->write(bus->bus_context, address, data);
    }
}

static inline void stack_push(CPU6502 *cpu, CPUBus *bus, uint8_t value) {
    write_byte(cpu, bus, (uint16_t)(0x0100 | cpu->stack_pointer), value);
    cpu->stack_pointer--;
}

static inline uint8_t stack_pull(CPU6502 *cpu, CPUBus *bus) {
    cpu->stack_pointer++;
    return read_byte(cpu, bus, (uint16_t)(0x0100 | cpu->stack_pointer));
}

static inline void do_adc(CPU6502 *cpu, uint8_t value) {
    uint8_t carry = get_flag(cpu, FLAG_CARRY) ? 1 : 0;

    if (get_flag(cpu, FLAG_DECIMAL_MODE) && cpu->model != CPU_MODEL_RICOH_2A03) {
        int low = (cpu->accumulator & 0x0F) + (value & 0x0F) + carry;
        int high = (cpu->accumulator >> 4) + (value >> 4);
        if (low > 9) {
            low += 6;
            high++;
        }

        uint16_t bin_sum = (uint16_t)cpu->accumulator + (uint16_t)value + (uint16_t)carry;
        set_flag(cpu, FLAG_OVERFLOW_V, (~(cpu->accumulator ^ value) & (cpu->accumulator ^ (high << 4)) & 0x80) != 0);
        set_flag(cpu, FLAG_ZERO, (bin_sum & 0xFF) == 0);
        set_flag(cpu, FLAG_NEGATIVE, (high & 0x08) != 0);

        if (high > 9) {
            high += 6;
        }
        set_flag(cpu, FLAG_CARRY, high > 15);

        cpu->accumulator = (uint8_t)(((high << 4) & 0xF0) | (low & 0x0F));
    } else {
        uint16_t sum = (uint16_t)cpu->accumulator + (uint16_t)value + (uint16_t)carry;
        set_flag(cpu, FLAG_CARRY, sum > 0xFF);
        set_flag(cpu, FLAG_OVERFLOW_V, (~(cpu->accumulator ^ value) & (cpu->accumulator ^ sum) & 0x80) != 0);
        cpu->accumulator = (uint8_t)(sum & 0xFF);
        update_zero_and_negative_flags(cpu, cpu->accumulator);
    }
}

static inline void do_sbc(CPU6502 *cpu, uint8_t value) {
    uint8_t carry = get_flag(cpu, FLAG_CARRY) ? 1 : 0;

    if (get_flag(cpu, FLAG_DECIMAL_MODE) && cpu->model != CPU_MODEL_RICOH_2A03) {
        int low = (cpu->accumulator & 0x0F) - (value & 0x0F) - (1 - carry);
        int high = (cpu->accumulator >> 4) - (value >> 4);
        if (low < 0) {
            low -= 6;
            high--;
        }
        if (high < 0) {
            high -= 6;
        }

        uint16_t bin_val = (uint16_t)value ^ 0x00FF;
        uint16_t bin_diff = (uint16_t)cpu->accumulator + bin_val + (uint16_t)carry;
        set_flag(cpu, FLAG_CARRY, bin_diff > 0xFF);
        set_flag(cpu, FLAG_OVERFLOW_V, ((cpu->accumulator ^ bin_diff) & (cpu->accumulator ^ value) & 0x80) != 0);
        set_flag(cpu, FLAG_ZERO, (bin_diff & 0xFF) == 0);
        set_flag(cpu, FLAG_NEGATIVE, (bin_diff & 0x80) != 0);

        cpu->accumulator = (uint8_t)(((high << 4) & 0xF0) | (low & 0x0F));
    } else {
        uint16_t bin_val = (uint16_t)value ^ 0x00FF;
        uint16_t diff = (uint16_t)cpu->accumulator + bin_val + (uint16_t)carry;
        set_flag(cpu, FLAG_CARRY, diff > 0xFF);
        set_flag(cpu, FLAG_OVERFLOW_V, ((cpu->accumulator ^ diff) & (cpu->accumulator ^ value) & 0x80) != 0);
        cpu->accumulator = (uint8_t)(diff & 0xFF);
        update_zero_and_negative_flags(cpu, cpu->accumulator);
    }
}

static inline uint8_t addr_imm(CPU6502 *cpu, CPUBus *bus) {
    return read_byte(cpu, bus, cpu->program_counter++);
}

static inline uint16_t addr_zp(CPU6502 *cpu, CPUBus *bus) {
    return read_byte(cpu, bus, cpu->program_counter++);
}

static inline uint16_t addr_zpx(CPU6502 *cpu, CPUBus *bus) {
    uint8_t base = read_byte(cpu, bus, cpu->program_counter++);
    read_byte(cpu, bus, base);
    return (uint8_t)(base + cpu->index_x);
}

static inline uint16_t addr_zpy(CPU6502 *cpu, CPUBus *bus) {
    uint8_t base = read_byte(cpu, bus, cpu->program_counter++);
    read_byte(cpu, bus, base);
    return (uint8_t)(base + cpu->index_y);
}

static inline uint16_t addr_abs(CPU6502 *cpu, CPUBus *bus) {
    uint8_t low = read_byte(cpu, bus, cpu->program_counter++);
    uint8_t high = read_byte(cpu, bus, cpu->program_counter++);
    return (uint16_t)(low | (high << 8));
}

static inline uint8_t read_abs_indexed(CPU6502 *cpu, CPUBus *bus, uint8_t index) {
    uint8_t low = read_byte(cpu, bus, cpu->program_counter++);
    uint8_t high = read_byte(cpu, bus, cpu->program_counter++);
    uint16_t uncorrected = (uint16_t)((high << 8) | ((low + index) & 0xFF));
    uint16_t corrected = (uint16_t)(((high << 8) | low) + index);
    uint8_t val = read_byte(cpu, bus, uncorrected);
    if (uncorrected != corrected) {
        val = read_byte(cpu, bus, corrected);
    }
    return val;
}

static inline uint16_t addr_abs_indexed_w(CPU6502 *cpu, CPUBus *bus, uint8_t index) {
    uint8_t low = read_byte(cpu, bus, cpu->program_counter++);
    uint8_t high = read_byte(cpu, bus, cpu->program_counter++);
    uint16_t uncorrected = (uint16_t)((high << 8) | ((low + index) & 0xFF));
    read_byte(cpu, bus, uncorrected);
    return (uint16_t)(((high << 8) | low) + index);
}

static inline uint16_t addr_indx(CPU6502 *cpu, CPUBus *bus) {
    uint8_t ptr = read_byte(cpu, bus, cpu->program_counter++);
    read_byte(cpu, bus, ptr);
    uint8_t low = read_byte(cpu, bus, (uint8_t)(ptr + cpu->index_x));
    uint8_t high = read_byte(cpu, bus, (uint8_t)(ptr + cpu->index_x + 1));
    return (uint16_t)(low | (high << 8));
}

static inline uint8_t read_indy(CPU6502 *cpu, CPUBus *bus) {
    uint8_t ptr = read_byte(cpu, bus, cpu->program_counter++);
    uint8_t low = read_byte(cpu, bus, ptr);
    uint8_t high = read_byte(cpu, bus, (uint8_t)(ptr + 1));
    uint16_t uncorrected = (uint16_t)((high << 8) | ((low + cpu->index_y) & 0xFF));
    uint16_t corrected = (uint16_t)(((high << 8) | low) + cpu->index_y);
    uint8_t val = read_byte(cpu, bus, uncorrected);
    if (uncorrected != corrected) {
        val = read_byte(cpu, bus, corrected);
    }
    return val;
}

static inline uint16_t addr_indy_w(CPU6502 *cpu, CPUBus *bus) {
    uint8_t ptr = read_byte(cpu, bus, cpu->program_counter++);
    uint8_t low = read_byte(cpu, bus, ptr);
    uint8_t high = read_byte(cpu, bus, (uint8_t)(ptr + 1));
    uint16_t uncorrected = (uint16_t)((high << 8) | ((low + cpu->index_y) & 0xFF));
    read_byte(cpu, bus, uncorrected);
    return (uint16_t)(((high << 8) | low) + cpu->index_y);
}

static inline void do_branch(CPU6502 *cpu, CPUBus *bus, bool condition) {
    int8_t offset = (int8_t)read_byte(cpu, bus, cpu->program_counter++);
    if (condition) {
        uint16_t old_pc = cpu->program_counter;
        uint16_t new_pc = (uint16_t)(old_pc + offset);
        uint16_t uncorrected = (uint16_t)((old_pc & 0xFF00) | (new_pc & 0x00FF));
        read_byte(cpu, bus, uncorrected);
        if (uncorrected != new_pc) {
            read_byte(cpu, bus, (uint16_t)((new_pc & 0xFF00) | (uncorrected & 0x00FF)));
        }
        cpu->program_counter = new_pc;
    }
}

static inline void rmw_asl(CPU6502 *cpu, CPUBus *bus, uint16_t addr) {
    uint8_t val = read_byte(cpu, bus, addr);
    write_byte(cpu, bus, addr, val);
    set_flag(cpu, FLAG_CARRY, (val & 0x80) != 0);
    val <<= 1;
    write_byte(cpu, bus, addr, val);
    update_zero_and_negative_flags(cpu, val);
}

static inline void rmw_lsr(CPU6502 *cpu, CPUBus *bus, uint16_t addr) {
    uint8_t val = read_byte(cpu, bus, addr);
    write_byte(cpu, bus, addr, val);
    set_flag(cpu, FLAG_CARRY, (val & 0x01) != 0);
    val >>= 1;
    write_byte(cpu, bus, addr, val);
    update_zero_and_negative_flags(cpu, val);
}

static inline void rmw_rol(CPU6502 *cpu, CPUBus *bus, uint16_t addr) {
    uint8_t val = read_byte(cpu, bus, addr);
    write_byte(cpu, bus, addr, val);
    uint8_t old_c = get_flag(cpu, FLAG_CARRY) ? 1 : 0;
    set_flag(cpu, FLAG_CARRY, (val & 0x80) != 0);
    val = (uint8_t)((val << 1) | old_c);
    write_byte(cpu, bus, addr, val);
    update_zero_and_negative_flags(cpu, val);
}

static inline void rmw_ror(CPU6502 *cpu, CPUBus *bus, uint16_t addr) {
    uint8_t val = read_byte(cpu, bus, addr);
    write_byte(cpu, bus, addr, val);
    uint8_t old_c = get_flag(cpu, FLAG_CARRY) ? 0x80 : 0x00;
    set_flag(cpu, FLAG_CARRY, (val & 0x01) != 0);
    val = (uint8_t)((val >> 1) | old_c);
    write_byte(cpu, bus, addr, val);
    update_zero_and_negative_flags(cpu, val);
}

static inline void rmw_inc(CPU6502 *cpu, CPUBus *bus, uint16_t addr) {
    uint8_t val = read_byte(cpu, bus, addr);
    write_byte(cpu, bus, addr, val);
    val++;
    write_byte(cpu, bus, addr, val);
    update_zero_and_negative_flags(cpu, val);
}

static inline void rmw_dec(CPU6502 *cpu, CPUBus *bus, uint16_t addr) {
    uint8_t val = read_byte(cpu, bus, addr);
    write_byte(cpu, bus, addr, val);
    val--;
    write_byte(cpu, bus, addr, val);
    update_zero_and_negative_flags(cpu, val);
}

static inline void rmw_slo(CPU6502 *cpu, CPUBus *bus, uint16_t addr) {
    uint8_t val = read_byte(cpu, bus, addr);
    write_byte(cpu, bus, addr, val);
    set_flag(cpu, FLAG_CARRY, (val & 0x80) != 0);
    val <<= 1;
    write_byte(cpu, bus, addr, val);
    cpu->accumulator |= val;
    update_zero_and_negative_flags(cpu, cpu->accumulator);
}

static inline void rmw_sre(CPU6502 *cpu, CPUBus *bus, uint16_t addr) {
    uint8_t val = read_byte(cpu, bus, addr);
    write_byte(cpu, bus, addr, val);
    set_flag(cpu, FLAG_CARRY, (val & 0x01) != 0);
    val >>= 1;
    write_byte(cpu, bus, addr, val);
    cpu->accumulator ^= val;
    update_zero_and_negative_flags(cpu, cpu->accumulator);
}

static inline void rmw_rla(CPU6502 *cpu, CPUBus *bus, uint16_t addr) {
    uint8_t val = read_byte(cpu, bus, addr);
    write_byte(cpu, bus, addr, val);
    uint8_t old_c = get_flag(cpu, FLAG_CARRY) ? 1 : 0;
    set_flag(cpu, FLAG_CARRY, (val & 0x80) != 0);
    val = (uint8_t)((val << 1) | old_c);
    write_byte(cpu, bus, addr, val);
    cpu->accumulator &= val;
    update_zero_and_negative_flags(cpu, cpu->accumulator);
}

static inline void rmw_rra(CPU6502 *cpu, CPUBus *bus, uint16_t addr) {
    uint8_t val = read_byte(cpu, bus, addr);
    write_byte(cpu, bus, addr, val);
    uint8_t old_c = get_flag(cpu, FLAG_CARRY) ? 0x80 : 0x00;
    set_flag(cpu, FLAG_CARRY, (val & 0x01) != 0);
    val = (uint8_t)((val >> 1) | old_c);
    write_byte(cpu, bus, addr, val);
    do_adc(cpu, val);
}

static inline void rmw_dcp(CPU6502 *cpu, CPUBus *bus, uint16_t addr) {
    uint8_t val = read_byte(cpu, bus, addr);
    write_byte(cpu, bus, addr, val);
    val--;
    write_byte(cpu, bus, addr, val);
    set_flag(cpu, FLAG_CARRY, cpu->accumulator >= val);
    update_zero_and_negative_flags(cpu, (uint8_t)(cpu->accumulator - val));
}

static inline void rmw_isc(CPU6502 *cpu, CPUBus *bus, uint16_t addr) {
    uint8_t val = read_byte(cpu, bus, addr);
    write_byte(cpu, bus, addr, val);
    val++;
    write_byte(cpu, bus, addr, val);
    do_sbc(cpu, val);
}

void cpu_init(CPU6502 *cpu, CPUModel model) {
    cpu->accumulator     = 0;
    cpu->index_x         = 0;
    cpu->index_y         = 0;
    cpu->stack_pointer   = 0xFD;
    cpu->program_counter = 0x0000;
    cpu->status_flags    = FLAG_UNUSED | FLAG_INTERRUPT_DISABLE;
    cpu->cycle_count     = 0;
    cpu->irq_lines       = 0;
    cpu->nmi_line        = false;
    cpu->nmi_edge        = false;
    cpu->reset_pending   = false;
    cpu->rdy             = true;
    cpu->open_bus        = 0;
    cpu->model           = model;
}

void cpu_reset(CPU6502 *cpu, CPUBus *bus) {
    read_byte(cpu, bus, cpu->program_counter);
    read_byte(cpu, bus, cpu->program_counter);
    read_byte(cpu, bus, (uint16_t)(0x0100 | cpu->stack_pointer));
    read_byte(cpu, bus, (uint16_t)(0x0100 | (uint8_t)(cpu->stack_pointer - 1)));
    read_byte(cpu, bus, (uint16_t)(0x0100 | (uint8_t)(cpu->stack_pointer - 2)));
    cpu->stack_pointer -= 3;
    set_flag(cpu, FLAG_INTERRUPT_DISABLE, true);
    uint8_t low = read_byte(cpu, bus, VECTOR_RESET);
    uint8_t high = read_byte(cpu, bus, (uint16_t)(VECTOR_RESET + 1));
    cpu->program_counter = (uint16_t)(low | (high << 8));
}

void cpu_set_irq_line(CPU6502 *cpu, uint8_t source_id, bool active) {
    if (active) {
        cpu->irq_lines |= (uint8_t)(1 << (source_id & 7));
    } else {
        cpu->irq_lines &= (uint8_t)~(1 << (source_id & 7));
    }
}

void cpu_set_nmi_line(CPU6502 *cpu, bool active) {
    if (active && !cpu->nmi_line) {
        cpu->nmi_edge = true;
    }
    cpu->nmi_line = active;
}

void cpu_pulse_nmi(CPU6502 *cpu) {
    cpu->nmi_edge = true;
}

void cpu_trigger_irq(CPU6502 *cpu) {
    cpu_set_irq_line(cpu, 0, true);
}

void cpu_trigger_reset(CPU6502 *cpu) {
    cpu->reset_pending = true;
}

void cpu_set_rdy(CPU6502 *cpu, bool rdy) {
    cpu->rdy = rdy;
}

void cpu_pulse_so(CPU6502 *cpu) {
    set_flag(cpu, FLAG_OVERFLOW_V, true);
}

static void do_hardware_interrupt(CPU6502 *cpu, CPUBus *bus, bool is_nmi) {
    read_byte(cpu, bus, cpu->program_counter);
    read_byte(cpu, bus, cpu->program_counter);
    stack_push(cpu, bus, (uint8_t)(cpu->program_counter >> 8));
    stack_push(cpu, bus, (uint8_t)(cpu->program_counter & 0xFF));
    stack_push(cpu, bus, (uint8_t)((cpu->status_flags & ~FLAG_BREAK_COMMAND) | FLAG_UNUSED));
    set_flag(cpu, FLAG_INTERRUPT_DISABLE, true);

    uint16_t vector = is_nmi ? VECTOR_NMI : VECTOR_IRQ;
    if (!is_nmi && cpu->nmi_edge) {
        cpu->nmi_edge = false;
        vector = VECTOR_NMI;
    }

    uint8_t low = read_byte(cpu, bus, vector);
    uint8_t high = read_byte(cpu, bus, (uint16_t)(vector + 1));
    cpu->program_counter = (uint16_t)(low | (high << 8));
}

int cpu_step(CPU6502 *cpu, CPUBus *bus) {
    uint64_t start_cycles = cpu->cycle_count;

    if (cpu->reset_pending) {
        cpu->reset_pending = false;
        cpu_reset(cpu, bus);
        return (int)(cpu->cycle_count - start_cycles);
    }

    if (cpu->nmi_edge) {
        cpu->nmi_edge = false;
        do_hardware_interrupt(cpu, bus, true);
        return (int)(cpu->cycle_count - start_cycles);
    }

    if (cpu->irq_lines != 0 && !get_flag(cpu, FLAG_INTERRUPT_DISABLE)) {
        do_hardware_interrupt(cpu, bus, false);
        return (int)(cpu->cycle_count - start_cycles);
    }

    uint8_t opcode = read_byte(cpu, bus, cpu->program_counter++);

    switch (opcode) {
        // --- ADC ---
        case 0x69: do_adc(cpu, addr_imm(cpu, bus)); break;
        case 0x65: do_adc(cpu, read_byte(cpu, bus, addr_zp(cpu, bus))); break;
        case 0x75: do_adc(cpu, read_byte(cpu, bus, addr_zpx(cpu, bus))); break;
        case 0x6D: do_adc(cpu, read_byte(cpu, bus, addr_abs(cpu, bus))); break;
        case 0x7D: do_adc(cpu, read_abs_indexed(cpu, bus, cpu->index_x)); break;
        case 0x79: do_adc(cpu, read_abs_indexed(cpu, bus, cpu->index_y)); break;
        case 0x61: do_adc(cpu, read_byte(cpu, bus, addr_indx(cpu, bus))); break;
        case 0x71: do_adc(cpu, read_indy(cpu, bus)); break;

        // --- AND ---
        case 0x29: cpu->accumulator &= addr_imm(cpu, bus); update_zero_and_negative_flags(cpu, cpu->accumulator); break;
        case 0x25: cpu->accumulator &= read_byte(cpu, bus, addr_zp(cpu, bus)); update_zero_and_negative_flags(cpu, cpu->accumulator); break;
        case 0x35: cpu->accumulator &= read_byte(cpu, bus, addr_zpx(cpu, bus)); update_zero_and_negative_flags(cpu, cpu->accumulator); break;
        case 0x2D: cpu->accumulator &= read_byte(cpu, bus, addr_abs(cpu, bus)); update_zero_and_negative_flags(cpu, cpu->accumulator); break;
        case 0x3D: cpu->accumulator &= read_abs_indexed(cpu, bus, cpu->index_x); update_zero_and_negative_flags(cpu, cpu->accumulator); break;
        case 0x39: cpu->accumulator &= read_abs_indexed(cpu, bus, cpu->index_y); update_zero_and_negative_flags(cpu, cpu->accumulator); break;
        case 0x21: cpu->accumulator &= read_byte(cpu, bus, addr_indx(cpu, bus)); update_zero_and_negative_flags(cpu, cpu->accumulator); break;
        case 0x31: cpu->accumulator &= read_indy(cpu, bus); update_zero_and_negative_flags(cpu, cpu->accumulator); break;

        // --- ASL ---
        case 0x0A:
            read_byte(cpu, bus, cpu->program_counter);
            set_flag(cpu, FLAG_CARRY, (cpu->accumulator & 0x80) != 0);
            cpu->accumulator <<= 1;
            update_zero_and_negative_flags(cpu, cpu->accumulator);
            break;
        case 0x06: rmw_asl(cpu, bus, addr_zp(cpu, bus)); break;
        case 0x16: rmw_asl(cpu, bus, addr_zpx(cpu, bus)); break;
        case 0x0E: rmw_asl(cpu, bus, addr_abs(cpu, bus)); break;
        case 0x1E: rmw_asl(cpu, bus, addr_abs_indexed_w(cpu, bus, cpu->index_x)); break;

        // --- Branch Instructions ---
        case 0x10: do_branch(cpu, bus, !get_flag(cpu, FLAG_NEGATIVE)); break;
        case 0x30: do_branch(cpu, bus, get_flag(cpu, FLAG_NEGATIVE)); break;
        case 0x50: do_branch(cpu, bus, !get_flag(cpu, FLAG_OVERFLOW_V)); break;
        case 0x70: do_branch(cpu, bus, get_flag(cpu, FLAG_OVERFLOW_V)); break;
        case 0x90: do_branch(cpu, bus, !get_flag(cpu, FLAG_CARRY)); break;
        case 0xB0: do_branch(cpu, bus, get_flag(cpu, FLAG_CARRY)); break;
        case 0xD0: do_branch(cpu, bus, !get_flag(cpu, FLAG_ZERO)); break;
        case 0xF0: do_branch(cpu, bus, get_flag(cpu, FLAG_ZERO)); break;

        // --- BIT ---
        case 0x24: {
            uint8_t val = read_byte(cpu, bus, addr_zp(cpu, bus));
            set_flag(cpu, FLAG_ZERO, (cpu->accumulator & val) == 0);
            set_flag(cpu, FLAG_NEGATIVE, (val & 0x80) != 0);
            set_flag(cpu, FLAG_OVERFLOW_V, (val & 0x40) != 0);
            break;
        }
        case 0x2C: {
            uint8_t val = read_byte(cpu, bus, addr_abs(cpu, bus));
            set_flag(cpu, FLAG_ZERO, (cpu->accumulator & val) == 0);
            set_flag(cpu, FLAG_NEGATIVE, (val & 0x80) != 0);
            set_flag(cpu, FLAG_OVERFLOW_V, (val & 0x40) != 0);
            break;
        }

        // --- BRK ---
        case 0x00: {
            read_byte(cpu, bus, cpu->program_counter++);
            stack_push(cpu, bus, (uint8_t)(cpu->program_counter >> 8));
            stack_push(cpu, bus, (uint8_t)(cpu->program_counter & 0xFF));
            stack_push(cpu, bus, (uint8_t)(cpu->status_flags | FLAG_BREAK_COMMAND | FLAG_UNUSED));
            set_flag(cpu, FLAG_INTERRUPT_DISABLE, true);

            uint16_t vector = VECTOR_IRQ;
            if (cpu->nmi_edge) {
                cpu->nmi_edge = false;
                vector = VECTOR_NMI;
            }
            uint8_t low = read_byte(cpu, bus, vector);
            uint8_t high = read_byte(cpu, bus, (uint16_t)(vector + 1));
            cpu->program_counter = (uint16_t)(low | (high << 8));
            break;
        }

        // --- CLC, CLD, CLI, CLV ---
        case 0x18: read_byte(cpu, bus, cpu->program_counter); set_flag(cpu, FLAG_CARRY, false); break;
        case 0xD8: read_byte(cpu, bus, cpu->program_counter); set_flag(cpu, FLAG_DECIMAL_MODE, false); break;
        case 0x58: read_byte(cpu, bus, cpu->program_counter); set_flag(cpu, FLAG_INTERRUPT_DISABLE, false); break;
        case 0xB8: read_byte(cpu, bus, cpu->program_counter); set_flag(cpu, FLAG_OVERFLOW_V, false); break;

        // --- CMP ---
        case 0xC9: { uint8_t v = addr_imm(cpu, bus); set_flag(cpu, FLAG_CARRY, cpu->accumulator >= v); update_zero_and_negative_flags(cpu, (uint8_t)(cpu->accumulator - v)); break; }
        case 0xC5: { uint8_t v = read_byte(cpu, bus, addr_zp(cpu, bus)); set_flag(cpu, FLAG_CARRY, cpu->accumulator >= v); update_zero_and_negative_flags(cpu, (uint8_t)(cpu->accumulator - v)); break; }
        case 0xD5: { uint8_t v = read_byte(cpu, bus, addr_zpx(cpu, bus)); set_flag(cpu, FLAG_CARRY, cpu->accumulator >= v); update_zero_and_negative_flags(cpu, (uint8_t)(cpu->accumulator - v)); break; }
        case 0xCD: { uint8_t v = read_byte(cpu, bus, addr_abs(cpu, bus)); set_flag(cpu, FLAG_CARRY, cpu->accumulator >= v); update_zero_and_negative_flags(cpu, (uint8_t)(cpu->accumulator - v)); break; }
        case 0xDD: { uint8_t v = read_abs_indexed(cpu, bus, cpu->index_x); set_flag(cpu, FLAG_CARRY, cpu->accumulator >= v); update_zero_and_negative_flags(cpu, (uint8_t)(cpu->accumulator - v)); break; }
        case 0xD9: { uint8_t v = read_abs_indexed(cpu, bus, cpu->index_y); set_flag(cpu, FLAG_CARRY, cpu->accumulator >= v); update_zero_and_negative_flags(cpu, (uint8_t)(cpu->accumulator - v)); break; }
        case 0xC1: { uint8_t v = read_byte(cpu, bus, addr_indx(cpu, bus)); set_flag(cpu, FLAG_CARRY, cpu->accumulator >= v); update_zero_and_negative_flags(cpu, (uint8_t)(cpu->accumulator - v)); break; }
        case 0xD1: { uint8_t v = read_indy(cpu, bus); set_flag(cpu, FLAG_CARRY, cpu->accumulator >= v); update_zero_and_negative_flags(cpu, (uint8_t)(cpu->accumulator - v)); break; }

        // --- CPX ---
        case 0xE0: { uint8_t v = addr_imm(cpu, bus); set_flag(cpu, FLAG_CARRY, cpu->index_x >= v); update_zero_and_negative_flags(cpu, (uint8_t)(cpu->index_x - v)); break; }
        case 0xE4: { uint8_t v = read_byte(cpu, bus, addr_zp(cpu, bus)); set_flag(cpu, FLAG_CARRY, cpu->index_x >= v); update_zero_and_negative_flags(cpu, (uint8_t)(cpu->index_x - v)); break; }
        case 0xEC: { uint8_t v = read_byte(cpu, bus, addr_abs(cpu, bus)); set_flag(cpu, FLAG_CARRY, cpu->index_x >= v); update_zero_and_negative_flags(cpu, (uint8_t)(cpu->index_x - v)); break; }

        // --- CPY ---
        case 0xC0: { uint8_t v = addr_imm(cpu, bus); set_flag(cpu, FLAG_CARRY, cpu->index_y >= v); update_zero_and_negative_flags(cpu, (uint8_t)(cpu->index_y - v)); break; }
        case 0xC4: { uint8_t v = read_byte(cpu, bus, addr_zp(cpu, bus)); set_flag(cpu, FLAG_CARRY, cpu->index_y >= v); update_zero_and_negative_flags(cpu, (uint8_t)(cpu->index_y - v)); break; }
        case 0xCC: { uint8_t v = read_byte(cpu, bus, addr_abs(cpu, bus)); set_flag(cpu, FLAG_CARRY, cpu->index_y >= v); update_zero_and_negative_flags(cpu, (uint8_t)(cpu->index_y - v)); break; }

        // --- DEC ---
        case 0xC6: rmw_dec(cpu, bus, addr_zp(cpu, bus)); break;
        case 0xD6: rmw_dec(cpu, bus, addr_zpx(cpu, bus)); break;
        case 0xCE: rmw_dec(cpu, bus, addr_abs(cpu, bus)); break;
        case 0xDE: rmw_dec(cpu, bus, addr_abs_indexed_w(cpu, bus, cpu->index_x)); break;

        // --- DEX, DEY ---
        case 0xCA: read_byte(cpu, bus, cpu->program_counter); cpu->index_x--; update_zero_and_negative_flags(cpu, cpu->index_x); break;
        case 0x88: read_byte(cpu, bus, cpu->program_counter); cpu->index_y--; update_zero_and_negative_flags(cpu, cpu->index_y); break;

        // --- EOR ---
        case 0x49: cpu->accumulator ^= addr_imm(cpu, bus); update_zero_and_negative_flags(cpu, cpu->accumulator); break;
        case 0x45: cpu->accumulator ^= read_byte(cpu, bus, addr_zp(cpu, bus)); update_zero_and_negative_flags(cpu, cpu->accumulator); break;
        case 0x55: cpu->accumulator ^= read_byte(cpu, bus, addr_zpx(cpu, bus)); update_zero_and_negative_flags(cpu, cpu->accumulator); break;
        case 0x4D: cpu->accumulator ^= read_byte(cpu, bus, addr_abs(cpu, bus)); update_zero_and_negative_flags(cpu, cpu->accumulator); break;
        case 0x5D: cpu->accumulator ^= read_abs_indexed(cpu, bus, cpu->index_x); update_zero_and_negative_flags(cpu, cpu->accumulator); break;
        case 0x59: cpu->accumulator ^= read_abs_indexed(cpu, bus, cpu->index_y); update_zero_and_negative_flags(cpu, cpu->accumulator); break;
        case 0x41: cpu->accumulator ^= read_byte(cpu, bus, addr_indx(cpu, bus)); update_zero_and_negative_flags(cpu, cpu->accumulator); break;
        case 0x51: cpu->accumulator ^= read_indy(cpu, bus); update_zero_and_negative_flags(cpu, cpu->accumulator); break;

        // --- INC ---
        case 0xE6: rmw_inc(cpu, bus, addr_zp(cpu, bus)); break;
        case 0xF6: rmw_inc(cpu, bus, addr_zpx(cpu, bus)); break;
        case 0xEE: rmw_inc(cpu, bus, addr_abs(cpu, bus)); break;
        case 0xFE: rmw_inc(cpu, bus, addr_abs_indexed_w(cpu, bus, cpu->index_x)); break;

        // --- INX, INY ---
        case 0xE8: read_byte(cpu, bus, cpu->program_counter); cpu->index_x++; update_zero_and_negative_flags(cpu, cpu->index_x); break;
        case 0xC8: read_byte(cpu, bus, cpu->program_counter); cpu->index_y++; update_zero_and_negative_flags(cpu, cpu->index_y); break;

        // --- JMP ---
        case 0x4C: {
            uint8_t low = read_byte(cpu, bus, cpu->program_counter++);
            uint8_t high = read_byte(cpu, bus, cpu->program_counter);
            cpu->program_counter = (uint16_t)(low | (high << 8));
            break;
        }
        case 0x6C: {
            uint8_t ptr_low = read_byte(cpu, bus, cpu->program_counter++);
            uint8_t ptr_high = read_byte(cpu, bus, cpu->program_counter++);
            uint16_t ptr = (uint16_t)(ptr_low | (ptr_high << 8));
            uint8_t low = read_byte(cpu, bus, ptr);
            uint16_t ptr_next;
            if (cpu->model == CPU_MODEL_CMOS_65C02) {
                ptr_next = (uint16_t)(ptr + 1);
            } else {
                ptr_next = (uint16_t)((ptr & 0xFF00) | ((ptr + 1) & 0x00FF));
            }
            uint8_t high = read_byte(cpu, bus, ptr_next);
            cpu->program_counter = (uint16_t)(low | (high << 8));
            break;
        }

        // --- JSR ---
        case 0x20: {
            uint8_t low = read_byte(cpu, bus, cpu->program_counter++);
            read_byte(cpu, bus, (uint16_t)(0x0100 | cpu->stack_pointer));
            stack_push(cpu, bus, (uint8_t)(cpu->program_counter >> 8));
            stack_push(cpu, bus, (uint8_t)(cpu->program_counter & 0xFF));
            uint8_t high = read_byte(cpu, bus, cpu->program_counter);
            cpu->program_counter = (uint16_t)(low | (high << 8));
            break;
        }

        // --- LDA ---
        case 0xA9: cpu->accumulator = addr_imm(cpu, bus); update_zero_and_negative_flags(cpu, cpu->accumulator); break;
        case 0xA5: cpu->accumulator = read_byte(cpu, bus, addr_zp(cpu, bus)); update_zero_and_negative_flags(cpu, cpu->accumulator); break;
        case 0xB5: cpu->accumulator = read_byte(cpu, bus, addr_zpx(cpu, bus)); update_zero_and_negative_flags(cpu, cpu->accumulator); break;
        case 0xAD: cpu->accumulator = read_byte(cpu, bus, addr_abs(cpu, bus)); update_zero_and_negative_flags(cpu, cpu->accumulator); break;
        case 0xBD: cpu->accumulator = read_abs_indexed(cpu, bus, cpu->index_x); update_zero_and_negative_flags(cpu, cpu->accumulator); break;
        case 0xB9: cpu->accumulator = read_abs_indexed(cpu, bus, cpu->index_y); update_zero_and_negative_flags(cpu, cpu->accumulator); break;
        case 0xA1: cpu->accumulator = read_byte(cpu, bus, addr_indx(cpu, bus)); update_zero_and_negative_flags(cpu, cpu->accumulator); break;
        case 0xB1: cpu->accumulator = read_indy(cpu, bus); update_zero_and_negative_flags(cpu, cpu->accumulator); break;

        // --- LDX ---
        case 0xA2: cpu->index_x = addr_imm(cpu, bus); update_zero_and_negative_flags(cpu, cpu->index_x); break;
        case 0xA6: cpu->index_x = read_byte(cpu, bus, addr_zp(cpu, bus)); update_zero_and_negative_flags(cpu, cpu->index_x); break;
        case 0xB6: cpu->index_x = read_byte(cpu, bus, addr_zpy(cpu, bus)); update_zero_and_negative_flags(cpu, cpu->index_x); break;
        case 0xAE: cpu->index_x = read_byte(cpu, bus, addr_abs(cpu, bus)); update_zero_and_negative_flags(cpu, cpu->index_x); break;
        case 0xBE: cpu->index_x = read_abs_indexed(cpu, bus, cpu->index_y); update_zero_and_negative_flags(cpu, cpu->index_x); break;

        // --- LDY ---
        case 0xA0: cpu->index_y = addr_imm(cpu, bus); update_zero_and_negative_flags(cpu, cpu->index_y); break;
        case 0xA4: cpu->index_y = read_byte(cpu, bus, addr_zp(cpu, bus)); update_zero_and_negative_flags(cpu, cpu->index_y); break;
        case 0xB4: cpu->index_y = read_byte(cpu, bus, addr_zpx(cpu, bus)); update_zero_and_negative_flags(cpu, cpu->index_y); break;
        case 0xAC: cpu->index_y = read_byte(cpu, bus, addr_abs(cpu, bus)); update_zero_and_negative_flags(cpu, cpu->index_y); break;
        case 0xBC: cpu->index_y = read_abs_indexed(cpu, bus, cpu->index_x); update_zero_and_negative_flags(cpu, cpu->index_y); break;

        // --- LSR ---
        case 0x4A:
            read_byte(cpu, bus, cpu->program_counter);
            set_flag(cpu, FLAG_CARRY, (cpu->accumulator & 0x01) != 0);
            cpu->accumulator >>= 1;
            update_zero_and_negative_flags(cpu, cpu->accumulator);
            break;
        case 0x46: rmw_lsr(cpu, bus, addr_zp(cpu, bus)); break;
        case 0x56: rmw_lsr(cpu, bus, addr_zpx(cpu, bus)); break;
        case 0x4E: rmw_lsr(cpu, bus, addr_abs(cpu, bus)); break;
        case 0x5E: rmw_lsr(cpu, bus, addr_abs_indexed_w(cpu, bus, cpu->index_x)); break;

        // --- NOP ---
        case 0xEA: case 0x1A: case 0x3A: case 0x5A: case 0x7A: case 0xDA: case 0xFA:
            read_byte(cpu, bus, cpu->program_counter);
            break;
        case 0x04: case 0x44: case 0x64:
            read_byte(cpu, bus, addr_zp(cpu, bus));
            break;
        case 0x14: case 0x34: case 0x54: case 0x74: case 0xD4: case 0xF4:
            read_byte(cpu, bus, addr_zpx(cpu, bus));
            break;
        case 0x80: case 0x82: case 0x89: case 0xC2: case 0xE2:
            addr_imm(cpu, bus);
            break;
        case 0x0C:
            read_byte(cpu, bus, addr_abs(cpu, bus));
            break;
        case 0x1C: case 0x3C: case 0x5C: case 0x7C: case 0xDC: case 0xFC:
            read_abs_indexed(cpu, bus, cpu->index_x);
            break;

        // --- ORA ---
        case 0x09: cpu->accumulator |= addr_imm(cpu, bus); update_zero_and_negative_flags(cpu, cpu->accumulator); break;
        case 0x05: cpu->accumulator |= read_byte(cpu, bus, addr_zp(cpu, bus)); update_zero_and_negative_flags(cpu, cpu->accumulator); break;
        case 0x15: cpu->accumulator |= read_byte(cpu, bus, addr_zpx(cpu, bus)); update_zero_and_negative_flags(cpu, cpu->accumulator); break;
        case 0x0D: cpu->accumulator |= read_byte(cpu, bus, addr_abs(cpu, bus)); update_zero_and_negative_flags(cpu, cpu->accumulator); break;
        case 0x1D: cpu->accumulator |= read_abs_indexed(cpu, bus, cpu->index_x); update_zero_and_negative_flags(cpu, cpu->accumulator); break;
        case 0x19: cpu->accumulator |= read_abs_indexed(cpu, bus, cpu->index_y); update_zero_and_negative_flags(cpu, cpu->accumulator); break;
        case 0x01: cpu->accumulator |= read_byte(cpu, bus, addr_indx(cpu, bus)); update_zero_and_negative_flags(cpu, cpu->accumulator); break;
        case 0x11: cpu->accumulator |= read_indy(cpu, bus); update_zero_and_negative_flags(cpu, cpu->accumulator); break;

        // --- PHA, PHP, PLA, PLP ---
        case 0x48: read_byte(cpu, bus, cpu->program_counter); stack_push(cpu, bus, cpu->accumulator); break;
        case 0x08: read_byte(cpu, bus, cpu->program_counter); stack_push(cpu, bus, (uint8_t)(cpu->status_flags | FLAG_BREAK_COMMAND | FLAG_UNUSED)); break;
        case 0x68:
            read_byte(cpu, bus, cpu->program_counter);
            read_byte(cpu, bus, (uint16_t)(0x0100 | cpu->stack_pointer));
            cpu->accumulator = stack_pull(cpu, bus);
            update_zero_and_negative_flags(cpu, cpu->accumulator);
            break;
        case 0x28:
            read_byte(cpu, bus, cpu->program_counter);
            read_byte(cpu, bus, (uint16_t)(0x0100 | cpu->stack_pointer));
            cpu->status_flags = (uint8_t)((stack_pull(cpu, bus) & ~FLAG_BREAK_COMMAND) | FLAG_UNUSED);
            break;

        // --- ROL ---
        case 0x2A: {
            read_byte(cpu, bus, cpu->program_counter);
            uint8_t old_c = get_flag(cpu, FLAG_CARRY) ? 1 : 0;
            set_flag(cpu, FLAG_CARRY, (cpu->accumulator & 0x80) != 0);
            cpu->accumulator = (uint8_t)((cpu->accumulator << 1) | old_c);
            update_zero_and_negative_flags(cpu, cpu->accumulator);
            break;
        }
        case 0x26: rmw_rol(cpu, bus, addr_zp(cpu, bus)); break;
        case 0x36: rmw_rol(cpu, bus, addr_zpx(cpu, bus)); break;
        case 0x2E: rmw_rol(cpu, bus, addr_abs(cpu, bus)); break;
        case 0x3E: rmw_rol(cpu, bus, addr_abs_indexed_w(cpu, bus, cpu->index_x)); break;

        // --- ROR ---
        case 0x6A: {
            read_byte(cpu, bus, cpu->program_counter);
            uint8_t old_c = get_flag(cpu, FLAG_CARRY) ? 0x80 : 0x00;
            set_flag(cpu, FLAG_CARRY, (cpu->accumulator & 0x01) != 0);
            cpu->accumulator = (uint8_t)((cpu->accumulator >> 1) | old_c);
            update_zero_and_negative_flags(cpu, cpu->accumulator);
            break;
        }
        case 0x66: rmw_ror(cpu, bus, addr_zp(cpu, bus)); break;
        case 0x76: rmw_ror(cpu, bus, addr_zpx(cpu, bus)); break;
        case 0x6E: rmw_ror(cpu, bus, addr_abs(cpu, bus)); break;
        case 0x7E: rmw_ror(cpu, bus, addr_abs_indexed_w(cpu, bus, cpu->index_x)); break;

        // --- RTI ---
        case 0x40: {
            read_byte(cpu, bus, cpu->program_counter);
            read_byte(cpu, bus, (uint16_t)(0x0100 | cpu->stack_pointer));
            cpu->status_flags = (uint8_t)((stack_pull(cpu, bus) & ~FLAG_BREAK_COMMAND) | FLAG_UNUSED);
            uint8_t low = stack_pull(cpu, bus);
            uint8_t high = stack_pull(cpu, bus);
            cpu->program_counter = (uint16_t)(low | (high << 8));
            break;
        }

        // --- RTS ---
        case 0x60: {
            read_byte(cpu, bus, cpu->program_counter);
            read_byte(cpu, bus, (uint16_t)(0x0100 | cpu->stack_pointer));
            uint8_t low = stack_pull(cpu, bus);
            uint8_t high = stack_pull(cpu, bus);
            cpu->program_counter = (uint16_t)(low | (high << 8));
            read_byte(cpu, bus, cpu->program_counter++);
            break;
        }

        // --- SBC ---
        case 0xE9: case 0xEB: do_sbc(cpu, addr_imm(cpu, bus)); break;
        case 0xE5: do_sbc(cpu, read_byte(cpu, bus, addr_zp(cpu, bus))); break;
        case 0xF5: do_sbc(cpu, read_byte(cpu, bus, addr_zpx(cpu, bus))); break;
        case 0xED: do_sbc(cpu, read_byte(cpu, bus, addr_abs(cpu, bus))); break;
        case 0xFD: do_sbc(cpu, read_abs_indexed(cpu, bus, cpu->index_x)); break;
        case 0xF9: do_sbc(cpu, read_abs_indexed(cpu, bus, cpu->index_y)); break;
        case 0xE1: do_sbc(cpu, read_byte(cpu, bus, addr_indx(cpu, bus))); break;
        case 0xF1: do_sbc(cpu, read_indy(cpu, bus)); break;

        // --- SEC, SED, SEI ---
        case 0x38: read_byte(cpu, bus, cpu->program_counter); set_flag(cpu, FLAG_CARRY, true); break;
        case 0xF8: read_byte(cpu, bus, cpu->program_counter); set_flag(cpu, FLAG_DECIMAL_MODE, true); break;
        case 0x78: read_byte(cpu, bus, cpu->program_counter); set_flag(cpu, FLAG_INTERRUPT_DISABLE, true); break;

        // --- STA ---
        case 0x85: write_byte(cpu, bus, addr_zp(cpu, bus), cpu->accumulator); break;
        case 0x95: write_byte(cpu, bus, addr_zpx(cpu, bus), cpu->accumulator); break;
        case 0x8D: write_byte(cpu, bus, addr_abs(cpu, bus), cpu->accumulator); break;
        case 0x9D: write_byte(cpu, bus, addr_abs_indexed_w(cpu, bus, cpu->index_x), cpu->accumulator); break;
        case 0x99: write_byte(cpu, bus, addr_abs_indexed_w(cpu, bus, cpu->index_y), cpu->accumulator); break;
        case 0x81: write_byte(cpu, bus, addr_indx(cpu, bus), cpu->accumulator); break;
        case 0x91: write_byte(cpu, bus, addr_indy_w(cpu, bus), cpu->accumulator); break;

        // --- STX ---
        case 0x86: write_byte(cpu, bus, addr_zp(cpu, bus), cpu->index_x); break;
        case 0x96: write_byte(cpu, bus, addr_zpy(cpu, bus), cpu->index_x); break;
        case 0x8E: write_byte(cpu, bus, addr_abs(cpu, bus), cpu->index_x); break;

        // --- STY ---
        case 0x84: write_byte(cpu, bus, addr_zp(cpu, bus), cpu->index_y); break;
        case 0x94: write_byte(cpu, bus, addr_zpx(cpu, bus), cpu->index_y); break;
        case 0x8C: write_byte(cpu, bus, addr_abs(cpu, bus), cpu->index_y); break;

        // --- Register Transfers ---
        case 0xAA: read_byte(cpu, bus, cpu->program_counter); cpu->index_x = cpu->accumulator; update_zero_and_negative_flags(cpu, cpu->index_x); break;
        case 0x8A: read_byte(cpu, bus, cpu->program_counter); cpu->accumulator = cpu->index_x; update_zero_and_negative_flags(cpu, cpu->accumulator); break;
        case 0xA8: read_byte(cpu, bus, cpu->program_counter); cpu->index_y = cpu->accumulator; update_zero_and_negative_flags(cpu, cpu->index_y); break;
        case 0x98: read_byte(cpu, bus, cpu->program_counter); cpu->accumulator = cpu->index_y; update_zero_and_negative_flags(cpu, cpu->accumulator); break;
        case 0xBA: read_byte(cpu, bus, cpu->program_counter); cpu->index_x = cpu->stack_pointer; update_zero_and_negative_flags(cpu, cpu->index_x); break;
        case 0x9A: read_byte(cpu, bus, cpu->program_counter); cpu->stack_pointer = cpu->index_x; break;

        // --- Unofficial Instructions ---
        case 0x03: rmw_slo(cpu, bus, addr_indx(cpu, bus)); break;
        case 0x07: rmw_slo(cpu, bus, addr_zp(cpu, bus)); break;
        case 0x17: rmw_slo(cpu, bus, addr_zpx(cpu, bus)); break;
        case 0x0F: rmw_slo(cpu, bus, addr_abs(cpu, bus)); break;
        case 0x1F: rmw_slo(cpu, bus, addr_abs_indexed_w(cpu, bus, cpu->index_x)); break;
        case 0x1B: rmw_slo(cpu, bus, addr_abs_indexed_w(cpu, bus, cpu->index_y)); break;
        case 0x13: rmw_slo(cpu, bus, addr_indy_w(cpu, bus)); break;

        case 0x23: rmw_rla(cpu, bus, addr_indx(cpu, bus)); break;
        case 0x27: rmw_rla(cpu, bus, addr_zp(cpu, bus)); break;
        case 0x37: rmw_rla(cpu, bus, addr_zpx(cpu, bus)); break;
        case 0x2F: rmw_rla(cpu, bus, addr_abs(cpu, bus)); break;
        case 0x3F: rmw_rla(cpu, bus, addr_abs_indexed_w(cpu, bus, cpu->index_x)); break;
        case 0x3B: rmw_rla(cpu, bus, addr_abs_indexed_w(cpu, bus, cpu->index_y)); break;
        case 0x33: rmw_rla(cpu, bus, addr_indy_w(cpu, bus)); break;

        case 0x43: rmw_sre(cpu, bus, addr_indx(cpu, bus)); break;
        case 0x47: rmw_sre(cpu, bus, addr_zp(cpu, bus)); break;
        case 0x57: rmw_sre(cpu, bus, addr_zpx(cpu, bus)); break;
        case 0x4F: rmw_sre(cpu, bus, addr_abs(cpu, bus)); break;
        case 0x5F: rmw_sre(cpu, bus, addr_abs_indexed_w(cpu, bus, cpu->index_x)); break;
        case 0x5B: rmw_sre(cpu, bus, addr_abs_indexed_w(cpu, bus, cpu->index_y)); break;
        case 0x53: rmw_sre(cpu, bus, addr_indy_w(cpu, bus)); break;

        case 0x63: rmw_rra(cpu, bus, addr_indx(cpu, bus)); break;
        case 0x67: rmw_rra(cpu, bus, addr_zp(cpu, bus)); break;
        case 0x77: rmw_rra(cpu, bus, addr_zpx(cpu, bus)); break;
        case 0x6F: rmw_rra(cpu, bus, addr_abs(cpu, bus)); break;
        case 0x7F: rmw_rra(cpu, bus, addr_abs_indexed_w(cpu, bus, cpu->index_x)); break;
        case 0x7B: rmw_rra(cpu, bus, addr_abs_indexed_w(cpu, bus, cpu->index_y)); break;
        case 0x73: rmw_rra(cpu, bus, addr_indy_w(cpu, bus)); break;

        case 0x83: write_byte(cpu, bus, addr_indx(cpu, bus), (uint8_t)(cpu->accumulator & cpu->index_x)); break;
        case 0x87: write_byte(cpu, bus, addr_zp(cpu, bus), (uint8_t)(cpu->accumulator & cpu->index_x)); break;
        case 0x97: write_byte(cpu, bus, addr_zpy(cpu, bus), (uint8_t)(cpu->accumulator & cpu->index_x)); break;
        case 0x8F: write_byte(cpu, bus, addr_abs(cpu, bus), (uint8_t)(cpu->accumulator & cpu->index_x)); break;

        case 0xA3: { uint8_t v = read_byte(cpu, bus, addr_indx(cpu, bus)); cpu->accumulator = v; cpu->index_x = v; update_zero_and_negative_flags(cpu, v); break; }
        case 0xA7: { uint8_t v = read_byte(cpu, bus, addr_zp(cpu, bus)); cpu->accumulator = v; cpu->index_x = v; update_zero_and_negative_flags(cpu, v); break; }
        case 0xB7: { uint8_t v = read_byte(cpu, bus, addr_zpy(cpu, bus)); cpu->accumulator = v; cpu->index_x = v; update_zero_and_negative_flags(cpu, v); break; }
        case 0xAF: { uint8_t v = read_byte(cpu, bus, addr_abs(cpu, bus)); cpu->accumulator = v; cpu->index_x = v; update_zero_and_negative_flags(cpu, v); break; }
        case 0xBF: { uint8_t v = read_abs_indexed(cpu, bus, cpu->index_y); cpu->accumulator = v; cpu->index_x = v; update_zero_and_negative_flags(cpu, v); break; }
        case 0xB3: { uint8_t v = read_indy(cpu, bus); cpu->accumulator = v; cpu->index_x = v; update_zero_and_negative_flags(cpu, v); break; }

        case 0xC3: rmw_dcp(cpu, bus, addr_indx(cpu, bus)); break;
        case 0xC7: rmw_dcp(cpu, bus, addr_zp(cpu, bus)); break;
        case 0xD7: rmw_dcp(cpu, bus, addr_zpx(cpu, bus)); break;
        case 0xCF: rmw_dcp(cpu, bus, addr_abs(cpu, bus)); break;
        case 0xDF: rmw_dcp(cpu, bus, addr_abs_indexed_w(cpu, bus, cpu->index_x)); break;
        case 0xDB: rmw_dcp(cpu, bus, addr_abs_indexed_w(cpu, bus, cpu->index_y)); break;
        case 0xD3: rmw_dcp(cpu, bus, addr_indy_w(cpu, bus)); break;

        case 0xE3: rmw_isc(cpu, bus, addr_indx(cpu, bus)); break;
        case 0xE7: rmw_isc(cpu, bus, addr_zp(cpu, bus)); break;
        case 0xF7: rmw_isc(cpu, bus, addr_zpx(cpu, bus)); break;
        case 0xEF: rmw_isc(cpu, bus, addr_abs(cpu, bus)); break;
        case 0xFF: rmw_isc(cpu, bus, addr_abs_indexed_w(cpu, bus, cpu->index_x)); break;
        case 0xFB: rmw_isc(cpu, bus, addr_abs_indexed_w(cpu, bus, cpu->index_y)); break;
        case 0xF3: rmw_isc(cpu, bus, addr_indy_w(cpu, bus)); break;

        case 0x0B: case 0x2B:
            cpu->accumulator &= addr_imm(cpu, bus);
            update_zero_and_negative_flags(cpu, cpu->accumulator);
            set_flag(cpu, FLAG_CARRY, get_flag(cpu, FLAG_NEGATIVE));
            break;

        case 0x4B:
            cpu->accumulator &= addr_imm(cpu, bus);
            set_flag(cpu, FLAG_CARRY, (cpu->accumulator & 0x01) != 0);
            cpu->accumulator >>= 1;
            update_zero_and_negative_flags(cpu, cpu->accumulator);
            break;

        case 0x6B: {
            uint8_t val = addr_imm(cpu, bus);
            cpu->accumulator &= val;
            uint8_t old_c = get_flag(cpu, FLAG_CARRY) ? 0x80 : 0x00;

            if (get_flag(cpu, FLAG_DECIMAL_MODE) && cpu->model != CPU_MODEL_RICOH_2A03) {
                uint8_t temp = (uint8_t)((cpu->accumulator >> 1) | old_c);
                update_zero_and_negative_flags(cpu, temp);
                set_flag(cpu, FLAG_OVERFLOW_V, ((temp ^ (temp << 1)) & 0x40) != 0);

                if ((cpu->accumulator & 0x0F) + (cpu->accumulator & 0x01) > 5) {
                    temp = (uint8_t)((temp & 0xF0) | ((temp + 6) & 0x0F));
                }
                if (((cpu->accumulator >> 4) + ((cpu->accumulator >> 4) & 0x01)) > 5) {
                    temp = (uint8_t)((temp & 0x0F) | ((temp + 0x60) & 0xF0));
                    set_flag(cpu, FLAG_CARRY, true);
                } else {
                    set_flag(cpu, FLAG_CARRY, false);
                }
                cpu->accumulator = temp;
            } else {
                cpu->accumulator = (uint8_t)((cpu->accumulator >> 1) | old_c);
                update_zero_and_negative_flags(cpu, cpu->accumulator);
                set_flag(cpu, FLAG_CARRY, (cpu->accumulator & 0x40) != 0);
                set_flag(cpu, FLAG_OVERFLOW_V, (((cpu->accumulator & 0x40) >> 6) ^ ((cpu->accumulator & 0x20) >> 5)) != 0);
            }
            break;
        }

        case 0xAB: {
            uint8_t v = addr_imm(cpu, bus);
            cpu->accumulator = (uint8_t)((cpu->accumulator | 0xEE) & v);
            cpu->index_x = cpu->accumulator;
            update_zero_and_negative_flags(cpu, cpu->accumulator);
            break;
        }

        case 0xCB: {
            uint8_t v = addr_imm(cpu, bus);
            uint8_t lhs = (uint8_t)(cpu->accumulator & cpu->index_x);
            set_flag(cpu, FLAG_CARRY, lhs >= v);
            cpu->index_x = (uint8_t)(lhs - v);
            update_zero_and_negative_flags(cpu, cpu->index_x);
            break;
        }

        case 0x8B: {
            uint8_t v = addr_imm(cpu, bus);
            cpu->accumulator = (uint8_t)((cpu->accumulator | 0xEE) & cpu->index_x & v);
            update_zero_and_negative_flags(cpu, cpu->accumulator);
            break;
        }

        case 0xBB: {
            uint8_t v = read_abs_indexed(cpu, bus, cpu->index_y);
            cpu->stack_pointer &= v;
            cpu->accumulator = cpu->stack_pointer;
            cpu->index_x = cpu->stack_pointer;
            update_zero_and_negative_flags(cpu, cpu->stack_pointer);
            break;
        }

        case 0x93: {
            uint8_t ptr = read_byte(cpu, bus, cpu->program_counter++);
            uint8_t low = read_byte(cpu, bus, ptr);
            uint8_t high = read_byte(cpu, bus, (uint8_t)(ptr + 1));
            uint16_t uncorrected = (uint16_t)((high << 8) | ((low + cpu->index_y) & 0xFF));
            read_byte(cpu, bus, uncorrected);
            uint16_t addr = (uint16_t)(((high << 8) | low) + cpu->index_y);
            uint8_t val = (uint8_t)(cpu->accumulator & cpu->index_x & (uint8_t)(high + 1));
            if ((low + cpu->index_y) >= 0x100) {
                addr = (uint16_t)((addr & 0x00FF) | (((high + 1) & val) << 8));
            }
            write_byte(cpu, bus, addr, val);
            break;
        }

        case 0x9F: {
            uint8_t low = read_byte(cpu, bus, cpu->program_counter++);
            uint8_t high = read_byte(cpu, bus, cpu->program_counter++);
            uint16_t uncorrected = (uint16_t)((high << 8) | ((low + cpu->index_y) & 0xFF));
            read_byte(cpu, bus, uncorrected);
            uint16_t addr = (uint16_t)(((high << 8) | low) + cpu->index_y);
            uint8_t val = (uint8_t)(cpu->accumulator & cpu->index_x & (uint8_t)(high + 1));
            if ((low + cpu->index_y) >= 0x100) {
                addr = (uint16_t)((addr & 0x00FF) | (((high + 1) & val) << 8));
            }
            write_byte(cpu, bus, addr, val);
            break;
        }

        case 0x9E: {
            uint8_t low = read_byte(cpu, bus, cpu->program_counter++);
            uint8_t high = read_byte(cpu, bus, cpu->program_counter++);
            uint16_t uncorrected = (uint16_t)((high << 8) | ((low + cpu->index_y) & 0xFF));
            read_byte(cpu, bus, uncorrected);
            uint16_t addr = (uint16_t)(((high << 8) | low) + cpu->index_y);
            uint8_t val = (uint8_t)(cpu->index_x & (uint8_t)(high + 1));
            if ((low + cpu->index_y) >= 0x100) {
                addr = (uint16_t)((addr & 0x00FF) | (((high + 1) & val) << 8));
            }
            write_byte(cpu, bus, addr, val);
            break;
        }

        case 0x9C: {
            uint8_t low = read_byte(cpu, bus, cpu->program_counter++);
            uint8_t high = read_byte(cpu, bus, cpu->program_counter++);
            uint16_t uncorrected = (uint16_t)((high << 8) | ((low + cpu->index_x) & 0xFF));
            read_byte(cpu, bus, uncorrected);
            uint16_t addr = (uint16_t)(((high << 8) | low) + cpu->index_x);
            uint8_t val = (uint8_t)(cpu->index_y & (uint8_t)(high + 1));
            if ((low + cpu->index_x) >= 0x100) {
                addr = (uint16_t)((addr & 0x00FF) | (((high + 1) & val) << 8));
            }
            write_byte(cpu, bus, addr, val);
            break;
        }

        case 0x9B: {
            cpu->stack_pointer = (uint8_t)(cpu->accumulator & cpu->index_x);
            uint8_t low = read_byte(cpu, bus, cpu->program_counter++);
            uint8_t high = read_byte(cpu, bus, cpu->program_counter++);
            uint16_t uncorrected = (uint16_t)((high << 8) | ((low + cpu->index_y) & 0xFF));
            read_byte(cpu, bus, uncorrected);
            uint16_t addr = (uint16_t)(((high << 8) | low) + cpu->index_y);
            uint8_t val = (uint8_t)(cpu->stack_pointer & (uint8_t)(high + 1));
            if ((low + cpu->index_y) >= 0x100) {
                addr = (uint16_t)((addr & 0x00FF) | (((high + 1) & val) << 8));
            }
            write_byte(cpu, bus, addr, val);
            break;
        }

        // --- JAM / KIL / HLT ---
        case 0x02: case 0x12: case 0x22: case 0x32: case 0x42: case 0x52:
        case 0x62: case 0x72: case 0x92: case 0xB2: case 0xD2: case 0xF2:
            read_byte(cpu, bus, cpu->program_counter);
            read_byte(cpu, bus, cpu->program_counter);
            cpu->program_counter--;
            break;

        default:
            fprintf(stderr, "[CPU ERROR] Unknown Opcode: 0x%02X at PC: 0x%04X\n",
                    opcode, (uint16_t)(cpu->program_counter - 1));
            break;
    }

    return (int)(cpu->cycle_count - start_cycles);
}