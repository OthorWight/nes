/* cpu6502.c */
#include "cpu6502.h"
#include <stdio.h>
#include <stddef.h>

static const uint16_t VECTOR_NMI   = 0xFFFA;
static const uint16_t VECTOR_RESET = 0xFFFC;
static const uint16_t VECTOR_IRQ   = 0xFFFE;

typedef struct {
    int (*operate)(CPU6502 *cpu, CPUBus *bus, uint16_t address);
    uint16_t (*address_mode)(CPU6502 *cpu, CPUBus *bus);
    uint8_t cycles;
    bool page_boundary_extra_cycle;
} Instruction;

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

static inline uint8_t read_byte(CPUBus *bus, uint16_t address) {
    return bus->read(bus->bus_context, address);
}

static inline void write_byte(CPUBus *bus, uint16_t address, uint8_t data) {
    bus->write(bus->bus_context, address, data);
}

static inline uint16_t read_word(CPUBus *bus, uint16_t address) {
    uint8_t low_byte  = read_byte(bus, address);
    uint8_t high_byte = read_byte(bus, (uint16_t)(address + 1));
    return (uint16_t)(low_byte | (high_byte << 8));
}

static inline bool page_crossed(uint16_t addr1, uint16_t addr2) {
    return (addr1 & 0xFF00) != (addr2 & 0xFF00);
}

static inline void stack_push_byte(CPU6502 *cpu, CPUBus *bus, uint8_t value) {
    write_byte(bus, (uint16_t)(0x0100 | cpu->stack_pointer), value);
    cpu->stack_pointer--;
}

static inline uint8_t stack_pull_byte(CPU6502 *cpu, CPUBus *bus) {
    cpu->stack_pointer++;
    return read_byte(bus, (uint16_t)(0x0100 | cpu->stack_pointer));
}

static inline void stack_push_word(CPU6502 *cpu, CPUBus *bus, uint16_t value) {
    stack_push_byte(cpu, bus, (uint8_t)((value >> 8) & 0xFF));
    stack_push_byte(cpu, bus, (uint8_t)(value & 0xFF));
}

static inline uint16_t stack_pull_word(CPU6502 *cpu, CPUBus *bus) {
    uint8_t low_byte  = stack_pull_byte(cpu, bus);
    uint8_t high_byte = stack_pull_byte(cpu, bus);
    return (uint16_t)(low_byte | (high_byte << 8));
}

static uint16_t addr_imp(CPU6502 *cpu, CPUBus *bus) {
    (void)cpu; (void)bus;
    return 0;
}

static uint16_t addr_imm(CPU6502 *cpu, CPUBus *bus) {
    (void)bus;
    uint16_t address = cpu->program_counter;
    cpu->program_counter++;
    return address;
}

static uint16_t addr_zp(CPU6502 *cpu, CPUBus *bus) {
    uint16_t address = read_byte(bus, cpu->program_counter);
    cpu->program_counter++;
    return address;
}

static uint16_t addr_abs(CPU6502 *cpu, CPUBus *bus) {
    uint16_t address = read_word(bus, cpu->program_counter);
    cpu->program_counter += 2;
    return address;
}

static uint16_t addr_rel(CPU6502 *cpu, CPUBus *bus) {
    int8_t offset = (int8_t)read_byte(bus, cpu->program_counter);
    cpu->program_counter++;
    return (uint16_t)(cpu->program_counter + offset);
}

static uint16_t addr_indx(CPU6502 *cpu, CPUBus *bus) {
    uint8_t temp = read_byte(bus, cpu->program_counter);
    cpu->program_counter++;
    uint8_t ptr = (uint8_t)(temp + cpu->index_x);
    uint8_t low = read_byte(bus, ptr);
    uint8_t high = read_byte(bus, (uint8_t)(ptr + 1));
    return (uint16_t)(low | (high << 8));
}

static uint16_t addr_zpx(CPU6502 *cpu, CPUBus *bus) {
    uint8_t base = read_byte(bus, cpu->program_counter);
    cpu->program_counter++;
    return (uint16_t)((base + cpu->index_x) & 0xFF);
}

static uint16_t addr_zpy(CPU6502 *cpu, CPUBus *bus) {
    uint8_t base = read_byte(bus, cpu->program_counter);
    cpu->program_counter++;
    return (uint16_t)((base + cpu->index_y) & 0xFF);
}

static uint16_t addr_absx(CPU6502 *cpu, CPUBus *bus) {
    uint16_t base = read_word(bus, cpu->program_counter);
    cpu->program_counter += 2;
    uint16_t target = base + cpu->index_x;
    cpu->page_crossed = page_crossed(base, target);
    return target;
}

static uint16_t addr_absy(CPU6502 *cpu, CPUBus *bus) {
    uint16_t base = read_word(bus, cpu->program_counter);
    cpu->program_counter += 2;
    uint16_t target = base + cpu->index_y;
    cpu->page_crossed = page_crossed(base, target);
    return target;
}

static uint16_t addr_indy(CPU6502 *cpu, CPUBus *bus) {
    uint8_t ptr = read_byte(bus, cpu->program_counter);
    cpu->program_counter++;
    uint8_t low = read_byte(bus, ptr);
    uint8_t high = read_byte(bus, (uint8_t)(ptr + 1));
    uint16_t base = (uint16_t)(low | (high << 8));
    uint16_t target = base + cpu->index_y;
    cpu->page_crossed = page_crossed(base, target);
    return target;
}

static uint16_t addr_ind(CPU6502 *cpu, CPUBus *bus) {
    uint16_t ptr = read_word(bus, cpu->program_counter);
    cpu->program_counter += 2;
    uint16_t target;
    if ((ptr & 0x00FF) == 0x00FF) {
        uint8_t low = read_byte(bus, ptr);
        uint8_t high = read_byte(bus, ptr & 0xFF00);
        target = (uint16_t)(low | (high << 8));
    } else {
        target = read_word(bus, ptr);
    }
    return target;
}

static int inst_nop(CPU6502 *cpu, CPUBus *bus, uint16_t address) {
    (void)cpu; (void)bus; (void)address;
    return 0;
}

static int inst_jam(CPU6502 *cpu, CPUBus *bus, uint16_t address) {
    (void)bus; (void)address;
    cpu->program_counter--;
    return 0;
}

static int inst_php(CPU6502 *cpu, CPUBus *bus, uint16_t address) {
    (void)address;
    stack_push_byte(cpu, bus, cpu->status_flags | FLAG_BREAK_COMMAND | FLAG_UNUSED);
    return 0;
}

static int inst_brk(CPU6502 *cpu, CPUBus *bus, uint16_t address) {
    (void)address;
    // BRK is a 2-byte instruction; skip the padding byte
    cpu->program_counter++;
    stack_push_word(cpu, bus, cpu->program_counter);
    stack_push_byte(cpu, bus, cpu->status_flags | FLAG_BREAK_COMMAND | FLAG_UNUSED);
    set_flag(cpu, FLAG_INTERRUPT_DISABLE, true);
    cpu->program_counter = read_word(bus, VECTOR_IRQ);
    return 0;
}

static int inst_ora(CPU6502 *cpu, CPUBus *bus, uint16_t address) {
    cpu->accumulator |= read_byte(bus, address);
    update_zero_and_negative_flags(cpu, cpu->accumulator);
    return 0;
}

static int inst_slo(CPU6502 *cpu, CPUBus *bus, uint16_t address) {
    uint8_t value = read_byte(bus, address);
    set_flag(cpu, FLAG_CARRY, (value & 0x80) != 0);
    value <<= 1;
    write_byte(bus, address, value);
    cpu->accumulator |= value;
    update_zero_and_negative_flags(cpu, cpu->accumulator);
    return 0;
}

static int inst_asl(CPU6502 *cpu, CPUBus *bus, uint16_t address) {
    uint8_t value = read_byte(bus, address);
    set_flag(cpu, FLAG_CARRY, (value & 0x80) != 0);
    value <<= 1;
    write_byte(bus, address, value);
    update_zero_and_negative_flags(cpu, value);
    return 0;
}

static int inst_asl_acc(CPU6502 *cpu, CPUBus *bus, uint16_t address) {
    (void)bus; (void)address;
    set_flag(cpu, FLAG_CARRY, (cpu->accumulator & 0x80) != 0);
    cpu->accumulator <<= 1;
    update_zero_and_negative_flags(cpu, cpu->accumulator);
    return 0;
}

static int inst_anc(CPU6502 *cpu, CPUBus *bus, uint16_t address) {
    cpu->accumulator &= read_byte(bus, address);
    update_zero_and_negative_flags(cpu, cpu->accumulator);
    set_flag(cpu, FLAG_CARRY, get_flag(cpu, FLAG_NEGATIVE));
    return 0;
}

static void do_adc(CPU6502 *cpu, uint8_t value) {
    uint8_t carry = get_flag(cpu, FLAG_CARRY) ? 1 : 0;

    if (get_flag(cpu, FLAG_DECIMAL_MODE) && cpu->decimal_mode) {
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

        cpu->accumulator = ((high << 4) & 0xF0) | (low & 0x0F);
    } else {
        uint16_t sum = (uint16_t)cpu->accumulator + (uint16_t)value + (uint16_t)carry;
        set_flag(cpu, FLAG_CARRY, sum > 0xFF);
        set_flag(cpu, FLAG_OVERFLOW_V, (~(cpu->accumulator ^ value) & (cpu->accumulator ^ sum) & 0x80) != 0);
        cpu->accumulator = sum & 0xFF;
        update_zero_and_negative_flags(cpu, cpu->accumulator);
    }
}

static int inst_adc(CPU6502 *cpu, CPUBus *bus, uint16_t address) {
    uint8_t value = read_byte(bus, address);
    do_adc(cpu, value);
    return 0;
}

static void do_sbc(CPU6502 *cpu, uint8_t value) {
    uint8_t carry = get_flag(cpu, FLAG_CARRY) ? 1 : 0;

    if (get_flag(cpu, FLAG_DECIMAL_MODE) && cpu->decimal_mode) {
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

        cpu->accumulator = ((high << 4) & 0xF0) | (low & 0x0F);
    } else {
        uint16_t bin_val = (uint16_t)value ^ 0x00FF;
        uint16_t diff = (uint16_t)cpu->accumulator + bin_val + (uint16_t)carry;
        set_flag(cpu, FLAG_CARRY, diff > 0xFF);
        set_flag(cpu, FLAG_OVERFLOW_V, ((cpu->accumulator ^ diff) & (cpu->accumulator ^ value) & 0x80) != 0);
        cpu->accumulator = diff & 0xFF;
        update_zero_and_negative_flags(cpu, cpu->accumulator);
    }
}

static int inst_sbc(CPU6502 *cpu, CPUBus *bus, uint16_t address) {
    uint8_t value = read_byte(bus, address);
    do_sbc(cpu, value);
    return 0;
}

static int inst_lda(CPU6502 *cpu, CPUBus *bus, uint16_t address) {
    cpu->accumulator = read_byte(bus, address);
    update_zero_and_negative_flags(cpu, cpu->accumulator);
    return 0;
}

static int inst_sta(CPU6502 *cpu, CPUBus *bus, uint16_t address) {
    write_byte(bus, address, cpu->accumulator);
    return 0;
}

static int inst_sty(CPU6502 *cpu, CPUBus *bus, uint16_t address) {
    write_byte(bus, address, cpu->index_y);
    return 0;
}

static int inst_sax(CPU6502 *cpu, CPUBus *bus, uint16_t address) {
    write_byte(bus, address, cpu->accumulator & cpu->index_x);
    return 0;
}

static int inst_ldx(CPU6502 *cpu, CPUBus *bus, uint16_t address) {
    cpu->index_x = read_byte(bus, address);
    update_zero_and_negative_flags(cpu, cpu->index_x);
    return 0;
}

static int inst_dex(CPU6502 *cpu, CPUBus *bus, uint16_t address) {
    (void)bus; (void)address;
    cpu->index_x--;
    update_zero_and_negative_flags(cpu, cpu->index_x);
    return 0;
}

static int inst_bne(CPU6502 *cpu, CPUBus *bus, uint16_t address) {
    (void)bus;
    if (!get_flag(cpu, FLAG_ZERO)) {
        uint16_t old_pc = cpu->program_counter;
        cpu->program_counter = address;
        return page_crossed(old_pc, address) ? 2 : 1;
    }
    return 0;
}

static int inst_bpl(CPU6502 *cpu, CPUBus *bus, uint16_t address) {
    (void)bus;
    if (!get_flag(cpu, FLAG_NEGATIVE)) {
        uint16_t old_pc = cpu->program_counter;
        cpu->program_counter = address;
        return page_crossed(old_pc, address) ? 2 : 1;
    }
    return 0;
}

static int inst_bmi(CPU6502 *cpu, CPUBus *bus, uint16_t address) {
    (void)bus;
    if (get_flag(cpu, FLAG_NEGATIVE)) {
        uint16_t old_pc = cpu->program_counter;
        cpu->program_counter = address;
        return page_crossed(old_pc, address) ? 2 : 1;
    }
    return 0;
}

static int inst_bvc(CPU6502 *cpu, CPUBus *bus, uint16_t address) {
    (void)bus;
    if (!get_flag(cpu, FLAG_OVERFLOW_V)) {
        uint16_t old_pc = cpu->program_counter;
        cpu->program_counter = address;
        return page_crossed(old_pc, address) ? 2 : 1;
    }
    return 0;
}

static int inst_clc(CPU6502 *cpu, CPUBus *bus, uint16_t address) {
    (void)bus; (void)address;
    set_flag(cpu, FLAG_CARRY, false);
    return 0;
}

static int inst_sec(CPU6502 *cpu, CPUBus *bus, uint16_t address) {
    (void)bus; (void)address;
    set_flag(cpu, FLAG_CARRY, true);
    return 0;
}

static int inst_cli(CPU6502 *cpu, CPUBus *bus, uint16_t address) {
    (void)bus; (void)address;
    set_flag(cpu, FLAG_INTERRUPT_DISABLE, false);
    return 0;
}

static int inst_sei(CPU6502 *cpu, CPUBus *bus, uint16_t address) {
    (void)bus; (void)address;
    set_flag(cpu, FLAG_INTERRUPT_DISABLE, true);
    return 0;
}

static int inst_jsr(CPU6502 *cpu, CPUBus *bus, uint16_t address) {
    (void)address;
    uint8_t low = read_byte(bus, cpu->program_counter);
    cpu->program_counter++;
    stack_push_word(cpu, bus, cpu->program_counter);
    uint8_t high = read_byte(bus, cpu->program_counter);
    cpu->program_counter = (uint16_t)(low | (high << 8));
    return 0;
}

static int inst_bit(CPU6502 *cpu, CPUBus *bus, uint16_t address) {
    uint8_t value = read_byte(bus, address);
    set_flag(cpu, FLAG_ZERO, (cpu->accumulator & value) == 0);
    set_flag(cpu, FLAG_NEGATIVE, (value & 0x80) != 0);
    set_flag(cpu, FLAG_OVERFLOW_V, (value & 0x40) != 0);
    return 0;
}

static int inst_rol(CPU6502 *cpu, CPUBus *bus, uint16_t address) {
    uint8_t value = read_byte(bus, address);
    uint8_t old_carry = get_flag(cpu, FLAG_CARRY) ? 1 : 0;
    set_flag(cpu, FLAG_CARRY, (value & 0x80) != 0);
    value = (uint8_t)((value << 1) | old_carry);
    write_byte(bus, address, value);
    update_zero_and_negative_flags(cpu, value);
    return 0;
}

static int inst_rol_acc(CPU6502 *cpu, CPUBus *bus, uint16_t address) {
    (void)bus; (void)address;
    uint8_t old_carry = get_flag(cpu, FLAG_CARRY) ? 1 : 0;
    set_flag(cpu, FLAG_CARRY, (cpu->accumulator & 0x80) != 0);
    cpu->accumulator = (uint8_t)((cpu->accumulator << 1) | old_carry);
    update_zero_and_negative_flags(cpu, cpu->accumulator);
    return 0;
}

static int inst_plp(CPU6502 *cpu, CPUBus *bus, uint16_t address) {
    (void)address;
    cpu->status_flags = (uint8_t)((stack_pull_byte(cpu, bus) & ~FLAG_BREAK_COMMAND) | FLAG_UNUSED);
    return 0;
}

static int inst_and(CPU6502 *cpu, CPUBus *bus, uint16_t address) {
    cpu->accumulator &= read_byte(bus, address);
    update_zero_and_negative_flags(cpu, cpu->accumulator);
    return 0;
}

static int inst_rla(CPU6502 *cpu, CPUBus *bus, uint16_t address) {
    uint8_t value = read_byte(bus, address);
    uint8_t old_carry = get_flag(cpu, FLAG_CARRY) ? 1 : 0;
    set_flag(cpu, FLAG_CARRY, (value & 0x80) != 0);
    value = (uint8_t)((value << 1) | old_carry);
    write_byte(bus, address, value);
    cpu->accumulator &= value;
    update_zero_and_negative_flags(cpu, cpu->accumulator);
    return 0;
}

static int inst_eor(CPU6502 *cpu, CPUBus *bus, uint16_t address) {
    cpu->accumulator ^= read_byte(bus, address);
    update_zero_and_negative_flags(cpu, cpu->accumulator);
    return 0;
}

static int inst_sre(CPU6502 *cpu, CPUBus *bus, uint16_t address) {
    uint8_t value = read_byte(bus, address);
    set_flag(cpu, FLAG_CARRY, (value & 0x01) != 0);
    value >>= 1;
    write_byte(bus, address, value);
    cpu->accumulator ^= value;
    update_zero_and_negative_flags(cpu, cpu->accumulator);
    return 0;
}

static int inst_rti(CPU6502 *cpu, CPUBus *bus, uint16_t address) {
    (void)address;
    cpu->status_flags = (uint8_t)((stack_pull_byte(cpu, bus) & ~FLAG_BREAK_COMMAND) | FLAG_UNUSED);
    cpu->program_counter = stack_pull_word(cpu, bus);
    return 0;
}

static int inst_lsr(CPU6502 *cpu, CPUBus *bus, uint16_t address) {
    uint8_t value = read_byte(bus, address);
    set_flag(cpu, FLAG_CARRY, (value & 0x01) != 0);
    value >>= 1;
    write_byte(bus, address, value);
    update_zero_and_negative_flags(cpu, value);
    return 0;
}

static int inst_lsr_acc(CPU6502 *cpu, CPUBus *bus, uint16_t address) {
    (void)bus; (void)address;
    set_flag(cpu, FLAG_CARRY, (cpu->accumulator & 0x01) != 0);
    cpu->accumulator >>= 1;
    update_zero_and_negative_flags(cpu, cpu->accumulator);
    return 0;
}

static int inst_pha(CPU6502 *cpu, CPUBus *bus, uint16_t address) {
    (void)address;
    stack_push_byte(cpu, bus, cpu->accumulator);
    return 0;
}

static int inst_alr(CPU6502 *cpu, CPUBus *bus, uint16_t address) {
    cpu->accumulator &= read_byte(bus, address);
    set_flag(cpu, FLAG_CARRY, (cpu->accumulator & 0x01) != 0);
    cpu->accumulator >>= 1;
    update_zero_and_negative_flags(cpu, cpu->accumulator);
    return 0;
}

static int inst_jmp(CPU6502 *cpu, CPUBus *bus, uint16_t address) {
    (void)bus;
    cpu->program_counter = address;
    return 0;
}

static int inst_rts(CPU6502 *cpu, CPUBus *bus, uint16_t address) {
    (void)address;
    cpu->program_counter = stack_pull_word(cpu, bus);
    cpu->program_counter++;
    return 0;
}

static int inst_ror(CPU6502 *cpu, CPUBus *bus, uint16_t address) {
    uint8_t value = read_byte(bus, address);
    uint8_t old_carry = get_flag(cpu, FLAG_CARRY) ? 0x80 : 0x00;
    set_flag(cpu, FLAG_CARRY, (value & 0x01) != 0);
    value = (uint8_t)((value >> 1) | old_carry);
    write_byte(bus, address, value);
    update_zero_and_negative_flags(cpu, value);
    return 0;
}

static int inst_ror_acc(CPU6502 *cpu, CPUBus *bus, uint16_t address) {
    (void)bus; (void)address;
    uint8_t old_carry = get_flag(cpu, FLAG_CARRY) ? 0x80 : 0x00;
    set_flag(cpu, FLAG_CARRY, (cpu->accumulator & 0x01) != 0);
    cpu->accumulator = (uint8_t)((cpu->accumulator >> 1) | old_carry);
    update_zero_and_negative_flags(cpu, cpu->accumulator);
    return 0;
}

static int inst_pla(CPU6502 *cpu, CPUBus *bus, uint16_t address) {
    (void)address;
    cpu->accumulator = stack_pull_byte(cpu, bus);
    update_zero_and_negative_flags(cpu, cpu->accumulator);
    return 0;
}

static int inst_bvs(CPU6502 *cpu, CPUBus *bus, uint16_t address) {
    (void)bus;
    if (get_flag(cpu, FLAG_OVERFLOW_V)) {
        uint16_t old_pc = cpu->program_counter;
        cpu->program_counter = address;
        return page_crossed(old_pc, address) ? 2 : 1;
    }
    return 0;
}

static int inst_arr(CPU6502 *cpu, CPUBus *bus, uint16_t address) {
    uint8_t value = read_byte(bus, address);
    cpu->accumulator &= value;
    uint8_t old_carry = get_flag(cpu, FLAG_CARRY) ? 0x80 : 0x00;

    if (get_flag(cpu, FLAG_DECIMAL_MODE) && cpu->decimal_mode) {
        uint8_t temp = (cpu->accumulator >> 1) | old_carry;
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
        cpu->accumulator = (cpu->accumulator >> 1) | old_carry;
        update_zero_and_negative_flags(cpu, cpu->accumulator);
        set_flag(cpu, FLAG_CARRY, (cpu->accumulator & 0x40) != 0);
        set_flag(cpu, FLAG_OVERFLOW_V, (((cpu->accumulator & 0x40) >> 6) ^ ((cpu->accumulator & 0x20) >> 5)) != 0);
    }
    return 0;
}

static int inst_rra(CPU6502 *cpu, CPUBus *bus, uint16_t address) {
    uint8_t value = read_byte(bus, address);
    uint8_t old_carry = get_flag(cpu, FLAG_CARRY) ? 0x80 : 0x00;
    set_flag(cpu, FLAG_CARRY, (value & 0x01) != 0);
    value = (uint8_t)((value >> 1) | old_carry);
    write_byte(bus, address, value);
    do_adc(cpu, value);
    return 0;
}

static int inst_stx(CPU6502 *cpu, CPUBus *bus, uint16_t address) {
    write_byte(bus, address, cpu->index_x);
    return 0;
}

static int inst_dey(CPU6502 *cpu, CPUBus *bus, uint16_t address) {
    (void)bus; (void)address;
    cpu->index_y--;
    update_zero_and_negative_flags(cpu, cpu->index_y);
    return 0;
}

static int inst_txa(CPU6502 *cpu, CPUBus *bus, uint16_t address) {
    (void)bus; (void)address;
    cpu->accumulator = cpu->index_x;
    update_zero_and_negative_flags(cpu, cpu->accumulator);
    return 0;
}

static int inst_xaa(CPU6502 *cpu, CPUBus *bus, uint16_t address) {
    uint8_t value = read_byte(bus, address);
    cpu->accumulator = (cpu->accumulator | 0xEE) & cpu->index_x & value;
    update_zero_and_negative_flags(cpu, cpu->accumulator);
    return 0;
}

static int inst_bcc(CPU6502 *cpu, CPUBus *bus, uint16_t address) {
    (void)bus;
    if (!get_flag(cpu, FLAG_CARRY)) {
        uint16_t old_pc = cpu->program_counter;
        cpu->program_counter = address;
        return page_crossed(old_pc, address) ? 2 : 1;
    }
    return 0;
}

static int inst_sha_indy(CPU6502 *cpu, CPUBus *bus, uint16_t address) {
    uint16_t base = (uint16_t)(address - cpu->index_y);
    uint8_t high = (uint8_t)(base >> 8);
    uint8_t val = cpu->accumulator & cpu->index_x & (uint8_t)(high + 1);
    if (page_crossed(base, address)) {
        address = (uint16_t)((address & 0x00FF) | ((uint16_t)val << 8));
    }
    write_byte(bus, address, val);
    return 0;
}

static int inst_sha_absy(CPU6502 *cpu, CPUBus *bus, uint16_t address) {
    uint16_t base = (uint16_t)(address - cpu->index_y);
    uint8_t high = (uint8_t)(base >> 8);
    uint8_t val = cpu->accumulator & cpu->index_x & (uint8_t)(high + 1);
    if (page_crossed(base, address)) {
        address = (uint16_t)((address & 0x00FF) | ((uint16_t)val << 8));
    }
    write_byte(bus, address, val);
    return 0;
}

static int inst_shx(CPU6502 *cpu, CPUBus *bus, uint16_t address) {
    uint16_t base = (uint16_t)(address - cpu->index_y);
    uint8_t high = (uint8_t)(base >> 8);
    uint8_t val = cpu->index_x & (uint8_t)(high + 1);
    if (page_crossed(base, address)) {
        address = (uint16_t)((address & 0x00FF) | ((uint16_t)val << 8));
    }
    write_byte(bus, address, val);
    return 0;
}

static int inst_shy(CPU6502 *cpu, CPUBus *bus, uint16_t address) {
    uint16_t base = (uint16_t)(address - cpu->index_x);
    uint8_t high = (uint8_t)(base >> 8);
    uint8_t val = cpu->index_y & (uint8_t)(high + 1);
    if (page_crossed(base, address)) {
        address = (uint16_t)((address & 0x00FF) | ((uint16_t)val << 8));
    }
    write_byte(bus, address, val);
    return 0;
}

static int inst_tas(CPU6502 *cpu, CPUBus *bus, uint16_t address) {
    cpu->stack_pointer = cpu->accumulator & cpu->index_x;
    uint16_t base = (uint16_t)(address - cpu->index_y);
    uint8_t high = (uint8_t)(base >> 8);
    uint8_t val = cpu->stack_pointer & (uint8_t)(high + 1);
    if (page_crossed(base, address)) {
        address = (uint16_t)((address & 0x00FF) | ((uint16_t)val << 8));
    }
    write_byte(bus, address, val);
    return 0;
}

static int inst_tya(CPU6502 *cpu, CPUBus *bus, uint16_t address) {
    (void)bus; (void)address;
    cpu->accumulator = cpu->index_y;
    update_zero_and_negative_flags(cpu, cpu->accumulator);
    return 0;
}

static int inst_txs(CPU6502 *cpu, CPUBus *bus, uint16_t address) {
    (void)bus; (void)address;
    cpu->stack_pointer = cpu->index_x;
    return 0;
}

static int inst_lax(CPU6502 *cpu, CPUBus *bus, uint16_t address) {
    uint8_t value = read_byte(bus, address);
    cpu->accumulator = value;
    cpu->index_x = value;
    update_zero_and_negative_flags(cpu, value);
    return 0;
}

static int inst_tay(CPU6502 *cpu, CPUBus *bus, uint16_t address) {
    (void)bus; (void)address;
    cpu->index_y = cpu->accumulator;
    update_zero_and_negative_flags(cpu, cpu->index_y);
    return 0;
}

static int inst_tax(CPU6502 *cpu, CPUBus *bus, uint16_t address) {
    (void)bus; (void)address;
    cpu->index_x = cpu->accumulator;
    update_zero_and_negative_flags(cpu, cpu->index_x);
    return 0;
}

static int inst_atx(CPU6502 *cpu, CPUBus *bus, uint16_t address) {
    uint8_t value = read_byte(bus, address);
    cpu->accumulator = (cpu->accumulator | 0xEE) & value;
    cpu->index_x = cpu->accumulator;
    update_zero_and_negative_flags(cpu, cpu->accumulator);
    return 0;
}

static int inst_bcs(CPU6502 *cpu, CPUBus *bus, uint16_t address) {
    (void)bus;
    if (get_flag(cpu, FLAG_CARRY)) {
        uint16_t old_pc = cpu->program_counter;
        cpu->program_counter = address;
        return page_crossed(old_pc, address) ? 2 : 1;
    }
    return 0;
}

static int inst_clv(CPU6502 *cpu, CPUBus *bus, uint16_t address) {
    (void)bus; (void)address;
    set_flag(cpu, FLAG_OVERFLOW_V, false);
    return 0;
}

static int inst_tsx(CPU6502 *cpu, CPUBus *bus, uint16_t address) {
    (void)bus; (void)address;
    cpu->index_x = cpu->stack_pointer;
    update_zero_and_negative_flags(cpu, cpu->index_x);
    return 0;
}

static int inst_las(CPU6502 *cpu, CPUBus *bus, uint16_t address) {
    uint8_t value = read_byte(bus, address);
    cpu->stack_pointer &= value;
    cpu->accumulator = cpu->stack_pointer;
    cpu->index_x = cpu->stack_pointer;
    update_zero_and_negative_flags(cpu, cpu->stack_pointer);
    return 0;
}

static int inst_cpy(CPU6502 *cpu, CPUBus *bus, uint16_t address) {
    uint8_t value = read_byte(bus, address);
    uint16_t result = (uint16_t)cpu->index_y - (uint16_t)value;
    set_flag(cpu, FLAG_CARRY, cpu->index_y >= value);
    update_zero_and_negative_flags(cpu, (uint8_t)result);
    return 0;
}

static int inst_cmp(CPU6502 *cpu, CPUBus *bus, uint16_t address) {
    uint8_t value = read_byte(bus, address);
    uint16_t result = (uint16_t)cpu->accumulator - (uint16_t)value;
    set_flag(cpu, FLAG_CARRY, cpu->accumulator >= value);
    update_zero_and_negative_flags(cpu, (uint8_t)result);
    return 0;
}

static int inst_dec(CPU6502 *cpu, CPUBus *bus, uint16_t address) {
    (void)cpu;
    uint8_t value = read_byte(bus, address);
    value--;
    write_byte(bus, address, value);
    update_zero_and_negative_flags(cpu, value);
    return 0;
}

static int inst_dcp(CPU6502 *cpu, CPUBus *bus, uint16_t address) {
    uint8_t value = read_byte(bus, address);
    value--;
    write_byte(bus, address, value);
    uint16_t result = (uint16_t)cpu->accumulator - (uint16_t)value;
    set_flag(cpu, FLAG_CARRY, cpu->accumulator >= value);
    update_zero_and_negative_flags(cpu, (uint8_t)result);
    return 0;
}

static int inst_iny(CPU6502 *cpu, CPUBus *bus, uint16_t address) {
    (void)bus; (void)address;
    cpu->index_y++;
    update_zero_and_negative_flags(cpu, cpu->index_y);
    return 0;
}

static int inst_axs(CPU6502 *cpu, CPUBus *bus, uint16_t address) {
    uint8_t value = read_byte(bus, address);
    uint8_t lhs = cpu->accumulator & cpu->index_x;
    uint16_t result = (uint16_t)lhs - (uint16_t)value;
    set_flag(cpu, FLAG_CARRY, lhs >= value);
    cpu->index_x = (uint8_t)result;
    update_zero_and_negative_flags(cpu, cpu->index_x);
    return 0;
}

static int inst_cld(CPU6502 *cpu, CPUBus *bus, uint16_t address) {
    (void)bus; (void)address;
    set_flag(cpu, FLAG_DECIMAL_MODE, false);
    return 0;
}

static int inst_cpx(CPU6502 *cpu, CPUBus *bus, uint16_t address) {
    uint8_t value = read_byte(bus, address);
    uint16_t result = (uint16_t)cpu->index_x - (uint16_t)value;
    set_flag(cpu, FLAG_CARRY, cpu->index_x >= value);
    update_zero_and_negative_flags(cpu, (uint8_t)result);
    return 0;
}

static int inst_inc(CPU6502 *cpu, CPUBus *bus, uint16_t address) {
    (void)cpu;
    uint8_t value = read_byte(bus, address);
    value++;
    write_byte(bus, address, value);
    update_zero_and_negative_flags(cpu, value);
    return 0;
}

static int inst_isc(CPU6502 *cpu, CPUBus *bus, uint16_t address) {
    uint8_t value = read_byte(bus, address);
    value++;
    write_byte(bus, address, value);
    do_sbc(cpu, value);
    return 0;
}

static int inst_inx(CPU6502 *cpu, CPUBus *bus, uint16_t address) {
    (void)bus; (void)address;
    cpu->index_x++;
    update_zero_and_negative_flags(cpu, cpu->index_x);
    return 0;
}

static int inst_beq(CPU6502 *cpu, CPUBus *bus, uint16_t address) {
    (void)bus;
    if (get_flag(cpu, FLAG_ZERO)) {
        uint16_t old_pc = cpu->program_counter;
        cpu->program_counter = address;
        return page_crossed(old_pc, address) ? 2 : 1;
    }
    return 0;
}

static int inst_sed(CPU6502 *cpu, CPUBus *bus, uint16_t address) {
    (void)bus; (void)address;
    set_flag(cpu, FLAG_DECIMAL_MODE, true);
    return 0;
}

static int inst_ldy(CPU6502 *cpu, CPUBus *bus, uint16_t address) {
    cpu->index_y = read_byte(bus, address);
    update_zero_and_negative_flags(cpu, cpu->index_y);
    return 0;
}

static const Instruction INSTRUCTION_TABLE[256] = {
    [0x00] = { inst_brk, addr_imp, 7, false },      // BRK Implied
    [0x01] = { inst_ora, addr_indx, 6, false },     // ORA (Indirect,X)
    [0x02] = { inst_jam, addr_imp, 3, false },      // JAM Implied (Unofficial)
    [0x03] = { inst_slo, addr_indx, 8, false },     // SLO (Indirect,X) (Unofficial)
    [0x04] = { inst_nop, addr_zp, 3, false },       // NOP Zero Page (Unofficial)
    [0x05] = { inst_ora, addr_zp, 3, false },       // ORA Zero Page
    [0x06] = { inst_asl, addr_zp, 5, false },       // ASL Zero Page
    [0x07] = { inst_slo, addr_zp, 5, false },       // SLO Zero Page (Unofficial)
    [0x08] = { inst_php, addr_imp, 3, false },      // PHP Implied
    [0x09] = { inst_ora, addr_imm, 2, false },      // ORA Immediate
    [0x0A] = { inst_asl_acc, addr_imp, 2, false },  // ASL Accumulator
    [0x0B] = { inst_anc, addr_imm, 2, false },      // ANC Immediate (Unofficial)
    [0x0C] = { inst_nop, addr_abs, 4, false },      // NOP Absolute (Unofficial)
    [0x0D] = { inst_ora, addr_abs, 4, false },      // ORA Absolute
    [0x0E] = { inst_asl, addr_abs, 6, false },      // ASL Absolute
    [0x0F] = { inst_slo, addr_abs, 6, false },      // SLO Absolute (Unofficial)
    [0x10] = { inst_bpl, addr_rel, 2, false },      // BPL Relative
    [0x11] = { inst_ora, addr_indy, 5, true },      // ORA (Indirect),Y
    [0x12] = { inst_jam, addr_imp, 3, false },      // JAM Implied (Unofficial)
    [0x13] = { inst_slo, addr_indy, 8, false },     // SLO (Indirect),Y (Unofficial)
    [0x14] = { inst_nop, addr_zpx, 4, false },      // NOP Zero Page,X (Unofficial)
    [0x15] = { inst_ora, addr_zpx, 4, false },      // ORA Zero Page,X
    [0x16] = { inst_asl, addr_zpx, 6, false },      // ASL Zero Page,X
    [0x17] = { inst_slo, addr_zpx, 6, false },      // SLO Zero Page,X (Unofficial)
    [0x18] = { inst_clc, addr_imp, 2, false },      // CLC Implied
    [0x19] = { inst_ora, addr_absy, 4, true },      // ORA Absolute,Y
    [0x1A] = { inst_nop, addr_imp, 2, false },      // NOP Implied (Unofficial)
    [0x1B] = { inst_slo, addr_absy, 7, false },     // SLO Absolute,Y (Unofficial)
    [0x1C] = { inst_nop, addr_absx, 4, true },      // NOP Absolute,X (Unofficial)
    [0x1D] = { inst_ora, addr_absx, 4, true },      // ORA Absolute,X
    [0x1E] = { inst_asl, addr_absx, 7, false },     // ASL Absolute,X
    [0x1F] = { inst_slo, addr_absx, 7, false },     // SLO Absolute,X (Unofficial)
    [0x20] = { inst_jsr, addr_imp, 6, false },      // JSR Absolute
    [0x21] = { inst_and, addr_indx, 6, false },     // AND (Indirect,X)
    [0x22] = { inst_jam, addr_imp, 3, false },      // JAM Implied (Unofficial)
    [0x23] = { inst_rla, addr_indx, 8, false },     // RLA (Indirect,X) (Unofficial)
    [0x24] = { inst_bit, addr_zp, 3, false },       // BIT Zero Page
    [0x25] = { inst_and, addr_zp, 3, false },       // AND Zero Page
    [0x26] = { inst_rol, addr_zp, 5, false },       // ROL Zero Page
    [0x27] = { inst_rla, addr_zp, 5, false },       // RLA Zero Page (Unofficial)
    [0x28] = { inst_plp, addr_imp, 4, false },      // PLP Implied
    [0x29] = { inst_and, addr_imm, 2, false },      // AND Immediate
    [0x2A] = { inst_rol_acc, addr_imp, 2, false },  // ROL Accumulator
    [0x2B] = { inst_anc, addr_imm, 2, false },      // ANC Immediate (Unofficial)
    [0x2C] = { inst_bit, addr_abs, 4, false },      // BIT Absolute
    [0x2D] = { inst_and, addr_abs, 4, false },      // AND Absolute
    [0x2E] = { inst_rol, addr_abs, 6, false },      // ROL Absolute
    [0x2F] = { inst_rla, addr_abs, 6, false },      // RLA Absolute (Unofficial)
    [0x30] = { inst_bmi, addr_rel, 2, false },      // BMI Relative
    [0x31] = { inst_and, addr_indy, 5, true },      // AND (Indirect),Y
    [0x32] = { inst_jam, addr_imp, 3, false },      // JAM Implied (Unofficial)
    [0x33] = { inst_rla, addr_indy, 8, false },     // RLA (Indirect),Y (Unofficial)
    [0x34] = { inst_nop, addr_zpx, 4, false },      // NOP Zero Page,X (Unofficial)
    [0x35] = { inst_and, addr_zpx, 4, false },      // AND Zero Page,X
    [0x36] = { inst_rol, addr_zpx, 6, false },      // ROL Zero Page,X
    [0x37] = { inst_rla, addr_zpx, 6, false },      // RLA Zero Page,X (Unofficial)
    [0x38] = { inst_sec, addr_imp, 2, false },      // SEC Implied
    [0x39] = { inst_and, addr_absy, 4, true },      // AND Absolute,Y
    [0x3A] = { inst_nop, addr_imp, 2, false },      // NOP Implied (Unofficial)
    [0x3B] = { inst_rla, addr_absy, 7, false },     // RLA Absolute,Y (Unofficial)
    [0x3C] = { inst_nop, addr_absx, 4, true },      // NOP Absolute,X (Unofficial)
    [0x3D] = { inst_and, addr_absx, 4, true },      // AND Absolute,X
    [0x3E] = { inst_rol, addr_absx, 7, false },     // ROL Absolute,X
    [0x3F] = { inst_rla, addr_absx, 7, false },     // RLA Absolute,X (Unofficial)
    [0x40] = { inst_rti, addr_imp, 6, false },      // RTI Implied
    [0x41] = { inst_eor, addr_indx, 6, false },     // EOR (Indirect,X)
    [0x42] = { inst_jam, addr_imp, 3, false },      // JAM Implied (Unofficial)
    [0x43] = { inst_sre, addr_indx, 8, false },     // SRE (Indirect,X) (Unofficial)
    [0x44] = { inst_nop, addr_zp, 3, false },       // NOP Zero Page (Unofficial)
    [0x45] = { inst_eor, addr_zp, 3, false },       // EOR Zero Page
    [0x46] = { inst_lsr, addr_zp, 5, false },       // LSR Zero Page
    [0x47] = { inst_sre, addr_zp, 5, false },       // SRE Zero Page (Unofficial)
    [0x48] = { inst_pha, addr_imp, 3, false },      // PHA Implied
    [0x49] = { inst_eor, addr_imm, 2, false },      // EOR Immediate
    [0x4A] = { inst_lsr_acc, addr_imp, 2, false },  // LSR Accumulator
    [0x4B] = { inst_alr, addr_imm, 2, false },      // ALR Immediate (Unofficial)
    [0x4C] = { inst_jmp, addr_abs, 3, false },      // JMP Absolute
    [0x4D] = { inst_eor, addr_abs, 4, false },      // EOR Absolute
    [0x4E] = { inst_lsr, addr_abs, 6, false },      // LSR Absolute
    [0x4F] = { inst_sre, addr_abs, 6, false },      // SRE Absolute (Unofficial)
    [0x50] = { inst_bvc, addr_rel, 2, false },      // BVC Relative
    [0x51] = { inst_eor, addr_indy, 5, true },      // EOR (Indirect),Y
    [0x52] = { inst_jam, addr_imp, 3, false },      // JAM Implied (Unofficial)
    [0x53] = { inst_sre, addr_indy, 8, false },     // SRE (Indirect),Y (Unofficial)
    [0x54] = { inst_nop, addr_zpx, 4, false },      // NOP Zero Page,X (Unofficial)
    [0x55] = { inst_eor, addr_zpx, 4, false },      // EOR Zero Page,X
    [0x56] = { inst_lsr, addr_zpx, 6, false },      // LSR Zero Page,X
    [0x57] = { inst_sre, addr_zpx, 6, false },      // SRE Zero Page,X (Unofficial)
    [0x58] = { inst_cli, addr_imp, 2, false },      // CLI Implied
    [0x59] = { inst_eor, addr_absy, 4, true },      // EOR Absolute,Y
    [0x5A] = { inst_nop, addr_imp, 2, false },      // NOP Implied (Unofficial)
    [0x5B] = { inst_sre, addr_absy, 7, false },     // SRE Absolute,Y (Unofficial)
    [0x5C] = { inst_nop, addr_absx, 4, true },      // NOP Absolute,X (Unofficial)
    [0x5D] = { inst_eor, addr_absx, 4, true },      // EOR Absolute,X
    [0x5E] = { inst_lsr, addr_absx, 7, false },     // LSR Absolute,X
    [0x5F] = { inst_sre, addr_absx, 7, false },     // SRE Absolute,X (Unofficial)
    [0x60] = { inst_rts, addr_imp, 6, false },      // RTS Implied
    [0x61] = { inst_adc, addr_indx, 6, false },     // ADC (Indirect,X)
    [0x62] = { inst_jam, addr_imp, 3, false },      // JAM Implied (Unofficial)
    [0x63] = { inst_rra, addr_indx, 8, false },     // RRA (Indirect,X) (Unofficial)
    [0x64] = { inst_nop, addr_zp, 3, false },       // NOP Zero Page (Unofficial)
    [0x65] = { inst_adc, addr_zp, 3, false },       // ADC Zero Page
    [0x66] = { inst_ror, addr_zp, 5, false },       // ROR Zero Page
    [0x67] = { inst_rra, addr_zp, 5, false },       // RRA Zero Page (Unofficial)
    [0x68] = { inst_pla, addr_imp, 4, false },      // PLA Implied
    [0x69] = { inst_adc, addr_imm, 2, false },      // ADC Immediate
    [0x6A] = { inst_ror_acc, addr_imp, 2, false },  // ROR Accumulator
    [0x6B] = { inst_arr, addr_imm, 2, false },      // ARR Immediate (Unofficial)
    [0x6C] = { inst_jmp, addr_ind, 5, false },      // JMP Indirect
    [0x6D] = { inst_adc, addr_abs, 4, false },      // ADC Absolute
    [0x6E] = { inst_ror, addr_abs, 6, false },      // ROR Absolute
    [0x6F] = { inst_rra, addr_abs, 6, false },      // RRA Absolute (Unofficial)
    [0x70] = { inst_bvs, addr_rel, 2, false },      // BVS Relative
    [0x71] = { inst_adc, addr_indy, 5, true },      // ADC (Indirect),Y
    [0x72] = { inst_jam, addr_imp, 3, false },      // JAM Implied (Unofficial)
    [0x73] = { inst_rra, addr_indy, 8, false },     // RRA (Indirect),Y (Unofficial)
    [0x74] = { inst_nop, addr_zpx, 4, false },      // NOP Zero Page,X (Unofficial)
    [0x75] = { inst_adc, addr_zpx, 4, false },      // ADC Zero Page,X
    [0x76] = { inst_ror, addr_zpx, 6, false },      // ROR Zero Page,X
    [0x77] = { inst_rra, addr_zpx, 6, false },      // RRA Zero Page,X (Unofficial)
    [0x78] = { inst_sei, addr_imp, 2, false },      // SEI Implied
    [0x79] = { inst_adc, addr_absy, 4, true },      // ADC Absolute,Y
    [0x7A] = { inst_nop, addr_imp, 2, false },      // NOP Implied (Unofficial)
    [0x7B] = { inst_rra, addr_absy, 7, false },     // RRA Absolute,Y (Unofficial)
    [0x7C] = { inst_nop, addr_absx, 4, true },      // NOP Absolute,X (Unofficial)
    [0x7D] = { inst_adc, addr_absx, 4, true },      // ADC Absolute,X
    [0x7E] = { inst_ror, addr_absx, 7, false },     // ROR Absolute,X
    [0x7F] = { inst_rra, addr_absx, 7, false },     // RRA Absolute,X (Unofficial)
    [0x80] = { inst_nop, addr_imm, 2, false },      // NOP Immediate (Unofficial)
    [0x81] = { inst_sta, addr_indx, 6, false },     // STA (Indirect,X)
    [0x82] = { inst_nop, addr_imm, 2, false },      // NOP Immediate (Unofficial)
    [0x83] = { inst_sax, addr_indx, 6, false },     // SAX (Indirect,X) (Unofficial)
    [0x84] = { inst_sty, addr_zp, 3, false },       // STY Zero Page
    [0x85] = { inst_sta, addr_zp, 3, false },       // STA Zero Page
    [0x86] = { inst_stx, addr_zp, 3, false },       // STX Zero Page
    [0x87] = { inst_sax, addr_zp, 3, false },       // SAX Zero Page (Unofficial)
    [0x88] = { inst_dey, addr_imp, 2, false },      // DEY Implied
    [0x89] = { inst_nop, addr_imm, 2, false },      // NOP Immediate (Unofficial)
    [0x8A] = { inst_txa, addr_imp, 2, false },      // TXA Implied
    [0x8B] = { inst_xaa, addr_imm, 2, false },      // XAA Immediate (Unofficial)
    [0x8C] = { inst_sty, addr_abs, 4, false },      // STY Absolute
    [0x8D] = { inst_sta, addr_abs, 4, false },      // STA Absolute
    [0x8E] = { inst_stx, addr_abs, 4, false },      // STX Absolute
    [0x8F] = { inst_sax, addr_abs, 4, false },      // SAX Absolute (Unofficial)
    [0x90] = { inst_bcc, addr_rel, 2, false },      // BCC Relative
    [0x91] = { inst_sta, addr_indy, 6, false },     // STA (Indirect),Y
    [0x92] = { inst_jam, addr_imp, 3, false },      // JAM Implied (Unofficial)
    [0x93] = { inst_sha_indy, addr_indy, 6, false }, // SHA (Indirect),Y (Unofficial)
    [0x94] = { inst_sty, addr_zpx, 4, false },      // STY Zero Page,X
    [0x95] = { inst_sta, addr_zpx, 4, false },      // STA Zero Page,X
    [0x96] = { inst_stx, addr_zpy, 4, false },      // STX Zero Page,Y
    [0x97] = { inst_sax, addr_zpy, 4, false },      // SAX Zero Page,Y (Unofficial)
    [0x98] = { inst_tya, addr_imp, 2, false },      // TYA Implied
    [0x99] = { inst_sta, addr_absy, 5, false },     // STA Absolute,Y
    [0x9A] = { inst_txs, addr_imp, 2, false },      // TXS Implied
    [0x9B] = { inst_tas, addr_absy, 5, false },     // TAS Absolute,Y (Unofficial)
    [0x9C] = { inst_shy, addr_absx, 5, false },     // SHY Absolute,X (Unofficial)
    [0x9D] = { inst_sta, addr_absx, 5, false },     // STA Absolute,X
    [0x9E] = { inst_shx, addr_absy, 5, false },     // SHX Absolute,Y (Unofficial)
    [0x9F] = { inst_sha_absy, addr_absy, 5, false }, // SHA Absolute,Y (Unofficial)
    [0xA0] = { inst_ldy, addr_imm, 2, false },      // LDY Immediate
    [0xA1] = { inst_lda, addr_indx, 6, false },     // LDA (Indirect,X)
    [0xA2] = { inst_ldx, addr_imm, 2, false },      // LDX Immediate
    [0xA3] = { inst_lax, addr_indx, 6, false },     // LAX (Indirect,X) (Unofficial)
    [0xA4] = { inst_ldy, addr_zp, 3, false },       // LDY Zero Page
    [0xA5] = { inst_lda, addr_zp, 3, false },       // LDA Zero Page
    [0xA6] = { inst_ldx, addr_zp, 3, false },       // LDX Zero Page
    [0xA7] = { inst_lax, addr_zp, 3, false },       // LAX Zero Page (Unofficial)
    [0xA8] = { inst_tay, addr_imp, 2, false },      // TAY Implied
    [0xA9] = { inst_lda, addr_imm, 2, false },      // LDA Immediate
    [0xAA] = { inst_tax, addr_imp, 2, false },      // TAX Implied
    [0xAB] = { inst_atx, addr_imm, 2, false },      // ATX Immediate (Unofficial)
    [0xAC] = { inst_ldy, addr_abs, 4, false },      // LDY Absolute
    [0xAD] = { inst_lda, addr_abs, 4, false },      // LDA Absolute
    [0xAE] = { inst_ldx, addr_abs, 4, false },      // LDX Absolute
    [0xAF] = { inst_lax, addr_abs, 4, false },      // LAX Absolute (Unofficial)
    [0xB0] = { inst_bcs, addr_rel, 2, false },      // BCS Relative
    [0xB1] = { inst_lda, addr_indy, 5, true },      // LDA (Indirect),Y
    [0xB2] = { inst_jam, addr_imp, 3, false },      // JAM Implied (Unofficial)
    [0xB3] = { inst_lax, addr_indy, 5, true },      // LAX (Indirect),Y (Unofficial)
    [0xB4] = { inst_ldy, addr_zpx, 4, false },      // LDY Zero Page,X
    [0xB5] = { inst_lda, addr_zpx, 4, false },      // LDA Zero Page,X
    [0xB6] = { inst_ldx, addr_zpy, 4, false },      // LDX Zero Page,Y
    [0xB7] = { inst_lax, addr_zpy, 4, false },      // LAX Zero Page,Y (Unofficial)
    [0xB8] = { inst_clv, addr_imp, 2, false },      // CLV Implied
    [0xB9] = { inst_lda, addr_absy, 4, true },      // LDA Absolute,Y
    [0xBA] = { inst_tsx, addr_imp, 2, false },      // TSX Implied
    [0xBB] = { inst_las, addr_absy, 4, true },      // LAS Absolute,Y (Unofficial)
    [0xBC] = { inst_ldy, addr_absx, 4, true },      // LDY Absolute,X
    [0xBD] = { inst_lda, addr_absx, 4, true },      // LDA Absolute,X
    [0xBE] = { inst_ldx, addr_absy, 4, true },      // LDX Absolute,Y
    [0xBF] = { inst_lax, addr_absy, 4, true },      // LAX Absolute,Y (Unofficial)
    [0xC0] = { inst_cpy, addr_imm, 2, false },      // CPY Immediate
    [0xC1] = { inst_cmp, addr_indx, 6, false },     // CMP (Indirect,X)
    [0xC2] = { inst_nop, addr_imm, 2, false },      // NOP Immediate (Unofficial)
    [0xC3] = { inst_dcp, addr_indx, 8, false },     // DCP (Indirect,X) (Unofficial)
    [0xC4] = { inst_cpy, addr_zp, 3, false },       // CPY Zero Page
    [0xC5] = { inst_cmp, addr_zp, 3, false },       // CMP Zero Page
    [0xC6] = { inst_dec, addr_zp, 5, false },       // DEC Zero Page
    [0xC7] = { inst_dcp, addr_zp, 5, false },       // DCP Zero Page (Unofficial)
    [0xC8] = { inst_iny, addr_imp, 2, false },      // INY Implied
    [0xC9] = { inst_cmp, addr_imm, 2, false },      // CMP Immediate
    [0xCA] = { inst_dex, addr_imp, 2, false },      // DEX Implied
    [0xCB] = { inst_axs, addr_imm, 2, false },      // AXS/SBX Immediate (Unofficial)
    [0xCC] = { inst_cpy, addr_abs, 4, false },      // CPY Absolute
    [0xCD] = { inst_cmp, addr_abs, 4, false },      // CMP Absolute
    [0xCE] = { inst_dec, addr_abs, 6, false },      // DEC Absolute
    [0xCF] = { inst_dcp, addr_abs, 6, false },      // DCP Absolute (Unofficial)
    [0xD0] = { inst_bne, addr_rel, 2, false },      // BNE Relative
    [0xD1] = { inst_cmp, addr_indy, 5, true },      // CMP (Indirect),Y
    [0xD2] = { inst_jam, addr_imp, 3, false },      // JAM Implied (Unofficial)
    [0xD3] = { inst_dcp, addr_indy, 8, false },     // DCP (Indirect),Y (Unofficial)
    [0xD4] = { inst_nop, addr_zpx, 4, false },      // NOP Zero Page,X (Unofficial)
    [0xD5] = { inst_cmp, addr_zpx, 4, false },      // CMP Zero Page,X
    [0xD6] = { inst_dec, addr_zpx, 6, false },      // DEC Zero Page,X
    [0xD7] = { inst_dcp, addr_zpx, 6, false },      // DCP Zero Page,X (Unofficial)
    [0xD8] = { inst_cld, addr_imp, 2, false },      // CLD Implied
    [0xD9] = { inst_cmp, addr_absy, 4, true },      // CMP Absolute,Y
    [0xDA] = { inst_nop, addr_imp, 2, false },      // NOP Implied (Unofficial)
    [0xDB] = { inst_dcp, addr_absy, 7, false },     // DCP Absolute,Y (Unofficial)
    [0xDC] = { inst_nop, addr_absx, 4, true },      // NOP Absolute,X (Unofficial)
    [0xDD] = { inst_cmp, addr_absx, 4, true },      // CMP Absolute,X
    [0xDE] = { inst_dec, addr_absx, 7, false },     // DEC Absolute,X
    [0xDF] = { inst_dcp, addr_absx, 7, false },     // DCP Absolute,X (Unofficial)
    [0xE0] = { inst_cpx, addr_imm, 2, false },      // CPX Immediate
    [0xE1] = { inst_sbc, addr_indx, 6, false },     // SBC (Indirect,X)
    [0xE2] = { inst_nop, addr_imm, 2, false },      // NOP Immediate (Unofficial)
    [0xE3] = { inst_isc, addr_indx, 8, false },     // ISC (Indirect,X) (Unofficial)
    [0xE4] = { inst_cpx, addr_zp, 3, false },       // CPX Zero Page
    [0xE5] = { inst_sbc, addr_zp, 3, false },       // SBC Zero Page
    [0xE6] = { inst_inc, addr_zp, 5, false },       // INC Zero Page
    [0xE7] = { inst_isc, addr_zp, 5, false },       // ISC Zero Page (Unofficial)
    [0xE8] = { inst_inx, addr_imp, 2, false },      // INX Implied
    [0xE9] = { inst_sbc, addr_imm, 2, false },      // SBC Immediate
    [0xEA] = { inst_nop, addr_imp, 2, false },      // NOP Implied
    [0xEB] = { inst_sbc, addr_imm, 2, false },      // SBC Immediate (Unofficial)
    [0xEC] = { inst_cpx, addr_abs, 4, false },      // CPX Absolute
    [0xED] = { inst_sbc, addr_abs, 4, false },      // SBC Absolute
    [0xEE] = { inst_inc, addr_abs, 6, false },      // INC Absolute
    [0xEF] = { inst_isc, addr_abs, 6, false },      // ISC Absolute (Unofficial)
    [0xF0] = { inst_beq, addr_rel, 2, false },      // BEQ Relative
    [0xF1] = { inst_sbc, addr_indy, 5, true },      // SBC (Indirect),Y
    [0xF2] = { inst_jam, addr_imp, 3, false },      // JAM Implied (Unofficial)
    [0xF3] = { inst_isc, addr_indy, 8, false },     // ISC (Indirect),Y (Unofficial)
    [0xF4] = { inst_nop, addr_zpx, 4, false },      // NOP Zero Page,X (Unofficial)
    [0xF5] = { inst_sbc, addr_zpx, 4, false },      // SBC Zero Page,X
    [0xF6] = { inst_inc, addr_zpx, 6, false },      // INC Zero Page,X
    [0xF7] = { inst_isc, addr_zpx, 6, false },      // ISC Zero Page,X (Unofficial)
    [0xF8] = { inst_sed, addr_imp, 2, false },      // SED Implied
    [0xF9] = { inst_sbc, addr_absy, 4, true },      // SBC Absolute,Y
    [0xFA] = { inst_nop, addr_imp, 2, false },      // NOP Implied (Unofficial)
    [0xFB] = { inst_isc, addr_absy, 7, false },     // ISC Absolute,Y (Unofficial)
    [0xFC] = { inst_nop, addr_absx, 4, true },      // NOP Absolute,X (Unofficial)
    [0xFD] = { inst_sbc, addr_absx, 4, true },      // SBC Absolute,X
    [0xFE] = { inst_inc, addr_absx, 7, false },     // INC Absolute,X
    [0xFF] = { inst_isc, addr_absx, 7, false }      // ISC Absolute,X (Unofficial)
};

static int execute_instruction(CPU6502 *cpu, CPUBus *bus, uint8_t opcode) {
    Instruction inst = INSTRUCTION_TABLE[opcode];

    // Fallback to a basic 2-cycle NOP if unimplemented, but log a warning
    if (inst.operate == NULL) {
        fprintf(stderr, "\n[CPU ERROR] Unimplemented opcode executed: 0x%02X at PC: 0x%04X\n",
                opcode, (uint16_t)(cpu->program_counter - 1));
        fflush(stderr);
        return 2;
    }

    uint16_t address = 0;
    if (inst.address_mode != NULL) {
        address = inst.address_mode(cpu, bus);
    }

    int extra_cycles = inst.operate(cpu, bus, address);
    int total_cycles = inst.cycles + extra_cycles;

    if (inst.page_boundary_extra_cycle && cpu->page_crossed) {
        total_cycles += 1;
    }

    return total_cycles;
}

void cpu_init(CPU6502 *cpu) {
    cpu->accumulator     = 0;
    cpu->index_x         = 0;
    cpu->index_y         = 0;
    cpu->stack_pointer   = 0xFD;
    cpu->program_counter = 0x0000;
    cpu->status_flags    = FLAG_UNUSED | FLAG_INTERRUPT_DISABLE;
    cpu->cycle_count     = 0;
    cpu->nmi_pending     = false;
    cpu->irq_pending     = false;
    cpu->reset_pending   = false;
    cpu->page_crossed    = false;
    cpu->decimal_mode    = true;
}

void cpu_reset(CPU6502 *cpu, CPUBus *bus) {
    cpu->accumulator     = 0;
    cpu->index_x         = 0;
    cpu->index_y         = 0;
    cpu->stack_pointer   = 0xFD;
    cpu->status_flags    = FLAG_UNUSED | FLAG_INTERRUPT_DISABLE;
    cpu->program_counter = read_word(bus, VECTOR_RESET);
    cpu->cycle_count    += 7;
    cpu->page_crossed    = false;
}

void cpu_trigger_nmi(CPU6502 *cpu) {
    cpu->nmi_pending = true;
}

void cpu_trigger_irq(CPU6502 *cpu) {
    cpu->irq_pending = true;
}

void cpu_trigger_reset(CPU6502 *cpu) {
    cpu->reset_pending = true;
}

bool cpu_is_opcode_implemented(uint8_t opcode) {
    return INSTRUCTION_TABLE[opcode].operate != NULL;
}

int cpu_step(CPU6502 *cpu, CPUBus *bus) {
    int cycles_taken = 0;

    // Handle pending interrupts (Reset > NMI > IRQ)
    if (cpu->reset_pending) {
        cpu_reset(cpu, bus);
        cpu->reset_pending = false;
        return 7;
    }

    if (cpu->nmi_pending) {
        cycles_taken = 7;
        stack_push_word(cpu, bus, cpu->program_counter);
        stack_push_byte(cpu, bus, (cpu->status_flags & ~FLAG_BREAK_COMMAND) | FLAG_UNUSED);
        set_flag(cpu, FLAG_INTERRUPT_DISABLE, true);
        cpu->program_counter = read_word(bus, VECTOR_NMI);
        cpu->nmi_pending = false;
    } else if (cpu->irq_pending && !get_flag(cpu, FLAG_INTERRUPT_DISABLE)) {
        cycles_taken = 7;
        stack_push_word(cpu, bus, cpu->program_counter);
        stack_push_byte(cpu, bus, (cpu->status_flags & ~FLAG_BREAK_COMMAND) | FLAG_UNUSED);
        set_flag(cpu, FLAG_INTERRUPT_DISABLE, true);
        cpu->program_counter = read_word(bus, VECTOR_IRQ);
        cpu->irq_pending = false;
    }

    if (cycles_taken == 0) {
        uint8_t opcode = read_byte(bus, cpu->program_counter);
        cpu->program_counter++;
        cycles_taken = execute_instruction(cpu, bus, opcode);
    }

    cpu->cycle_count += cycles_taken;
    return cycles_taken;
}