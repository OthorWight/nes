#include "apu2a03.h"
#include "nes_system.h"
#include <string.h>

static const uint8_t LENGTH_TABLE[32] = {
    10, 254, 20,  2, 40,  4, 80,  6,160,  8, 60, 10, 14, 12, 26, 14,
    12,  16, 24, 18, 48, 20, 96, 22,192, 24, 72, 26, 16, 28, 32, 30
};

static const uint8_t DUTY_TABLE[4][8] = {
    {0, 1, 0, 0, 0, 0, 0, 0},
    {0, 1, 1, 0, 0, 0, 0, 0},
    {0, 1, 1, 1, 1, 0, 0, 0},
    {1, 0, 0, 1, 1, 1, 1, 1}
};

static const uint8_t TRIANGLE_TABLE[32] = {
    15, 14, 13, 12, 11, 10,  9,  8,  7,  6,  5,  4,  3,  2,  1,  0,
     0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15
};

static const uint16_t NOISE_PERIOD[16] = {
    4, 8, 16, 32, 64, 96, 128, 160, 202, 254, 380, 508, 762, 1016, 2034, 4068
};

// NTSC periods in CPU cycles.
static const uint16_t DMC_RATE_TABLE[16] = {
    428, 380, 340, 320, 286, 254, 226, 214,
    190, 160, 142, 128, 106,  84,  72,  54
};
static const uint16_t PAL_NOISE_PERIOD[16] = {
    4,8,14,30,60,88,118,148,188,236,354,472,708,944,1890,3778
};
static const uint16_t PAL_DMC_PERIOD[16] = {
    398,354,316,298,276,236,210,198,176,148,132,118,98,78,66,50
};

void apu_set_region(APU2A03 *a, unsigned region) {
    a->region = (uint8_t)region;
    a->noise_timer_reload = (region == NES_PAL ? PAL_NOISE_PERIOD : NOISE_PERIOD)[a->noise_rate & 15];
    a->dmc_timer_reload = (region == NES_PAL ? PAL_DMC_PERIOD : DMC_RATE_TABLE)[a->dmc_rate & 15];
}

static bool is_sweep_muting(APU2A03 *apu, int ch) {
    uint16_t period = apu->pulse_timer_reload[ch];
    if (period < 8 || period > 0x07FF) return true;

    int16_t change = period >> apu->pulse_sweep_shift[ch];
    int16_t target;
    if (apu->pulse_sweep_negate[ch]) {
        target = period - change;
        if (ch == 0) {
            target -= 1;
        }
    } else {
        target = period + change;
    }
    return (target > 0x07FF);
}

void apu_dmc_dma_complete(APU2A03 *apu, NES *nes) {
    if (apu->dmc_bytes_remaining > 0 && nes) {
        apu->dmc_buffer = nes_cpu_bus_read(nes, apu->dmc_current_addr);
        apu->dmc_buffer_empty = false;
        apu->dmc_current_addr = (apu->dmc_current_addr + 1) | 0x8000;
        apu->dmc_bytes_remaining--;
        if (apu->dmc_bytes_remaining == 0) {
            if (apu->dmc_loop) {
                apu->dmc_current_addr = apu->dmc_sample_addr;
                apu->dmc_bytes_remaining = apu->dmc_sample_len;
            } else if (apu->dmc_irq_enable) {
                /* Hardware asserts the DMC IRQ when the final sample byte is
                   fetched into the sample buffer, not after it is played. */
                apu->dmc_irq_active = true;
                cpu_set_irq_line(&nes->cpu, APU_IRQ_SOURCE_DMC, true);
            }
        }
    }
}

static void apu_clock_quarter_frame(APU2A03 *apu) {
    if (apu->triangle_linear_reload_flag) {
        apu->triangle_linear_counter = apu->triangle_linear_reload;
    } else if (apu->triangle_linear_counter > 0) {
        apu->triangle_linear_counter--;
    }
    if (!apu->triangle_control_flag) {
        apu->triangle_linear_reload_flag = false;
    }

    for (int ch = 0; ch < 2; ch++) {
        if (apu->pulse_envelope_start[ch]) {
            apu->pulse_envelope_decay[ch] = 15;
            apu->pulse_envelope_divider[ch] = apu->pulse_volume[ch];
            apu->pulse_envelope_start[ch] = false;
        } else if (apu->pulse_envelope_divider[ch] == 0) {
            apu->pulse_envelope_divider[ch] = apu->pulse_volume[ch];
            if (apu->pulse_envelope_decay[ch] > 0) {
                apu->pulse_envelope_decay[ch]--;
            } else if (apu->pulse_halt[ch]) {
                apu->pulse_envelope_decay[ch] = 15;
            }
        } else {
            apu->pulse_envelope_divider[ch]--;
        }
    }

    if (apu->noise_envelope_start) {
        apu->noise_envelope_decay = 15;
        apu->noise_envelope_divider = apu->noise_volume;
        apu->noise_envelope_start = false;
    } else if (apu->noise_envelope_divider == 0) {
        apu->noise_envelope_divider = apu->noise_volume;
        if (apu->noise_envelope_decay > 0) {
            apu->noise_envelope_decay--;
        } else if (apu->noise_halt) {
            apu->noise_envelope_decay = 15;
        }
    } else {
        apu->noise_envelope_divider--;
    }
}

static void apu_clock_half_frame(APU2A03 *apu) {
    apu_clock_quarter_frame(apu);

    for (int ch = 0; ch < 2; ch++) {
        if (!apu->pulse_halt[ch] && apu->pulse_length_counter[ch] > 0) {
            apu->pulse_length_counter[ch]--;
        }

        bool sweep_muted = is_sweep_muting(apu, ch);
        if (apu->pulse_sweep_divider[ch] == 0 && apu->pulse_sweep_enabled[ch] && apu->pulse_sweep_shift[ch] > 0 && !sweep_muted) {
            uint16_t period = apu->pulse_timer_reload[ch];
            int16_t change = period >> apu->pulse_sweep_shift[ch];
            if (apu->pulse_sweep_negate[ch]) {
                period -= change;
                if (ch == 0) {
                    period -= 1;
                }
            } else {
                period += change;
            }
            apu->pulse_timer_reload[ch] = period;
        }

        if (apu->pulse_sweep_divider[ch] == 0 || apu->pulse_sweep_reload[ch]) {
            apu->pulse_sweep_divider[ch] = apu->pulse_sweep_period[ch];
            apu->pulse_sweep_reload[ch] = false;
        } else {
            apu->pulse_sweep_divider[ch]--;
        }
    }

    if (apu->triangle_length_counter > 0 && !apu->triangle_control_flag) {
        apu->triangle_length_counter--;
    }

    if (apu->noise_length_counter > 0 && !apu->noise_halt) {
        apu->noise_length_counter--;
    }
}

static uint8_t *length_counter(APU2A03 *a, unsigned ch) {
    return ch < 2 ? &a->pulse_length_counter[ch] : ch == 2 ?
        &a->triangle_length_counter : &a->noise_length_counter;
}
static bool *halt_flag(APU2A03 *a, unsigned ch) {
    return ch < 2 ? &a->pulse_halt[ch] : ch == 2 ?
        &a->triangle_control_flag : &a->noise_halt;
}
static void load_length(APU2A03 *a, unsigned ch, uint8_t data) {
    if (!(a->length_pending & (1u << ch))) a->length_previous[ch] = *length_counter(a, ch);
    a->length_pending |= 1u << ch;
    *length_counter(a, ch) = LENGTH_TABLE[data >> 3];
}
static void set_halt(APU2A03 *a, unsigned ch, bool value) {
    if (!(a->halt_pending & (1u << ch))) a->halt_previous[ch] = *halt_flag(a, ch);
    a->halt_pending |= 1u << ch;
    *halt_flag(a, ch) = value;
}

void apu_init(APU2A03 *apu) {
    memset(apu, 0, sizeof(APU2A03));
    apu->noise_shift_reg = 1;
    apu->noise_timer_reload = NOISE_PERIOD[0];
    apu->dmc_buffer_empty = true;
    apu->dmc_silent = true;
    apu->dmc_bits_remaining = 8;
    apu->dmc_sample_addr = 0xC000;
    apu->dmc_sample_len = 1;
    apu->dmc_timer_reload = DMC_RATE_TABLE[0];
    apu->dmc_timer = 0;
    apu->audio_accumulator = 0.0;
    apu->audio_buffer_idx = 0;
    apu->frame_counter_reset_pending = false;
    apu->frame_counter_reset_delay = 0;
}

static void apu_write_pulse_reg(APU2A03 *apu, int ch, uint16_t offset, uint8_t data) {
    switch (offset) {
        case 0:
            apu->pulse_duty[ch] = (data >> 6) & 0x03;
            set_halt(apu, ch, (data & 0x20) != 0);
            apu->pulse_constant_volume[ch] = (data & 0x10) != 0;
            apu->pulse_volume[ch] = data & 0x0F;
            break;
        case 1:
            apu->pulse_sweep_enabled[ch] = (data & 0x80) != 0;
            apu->pulse_sweep_period[ch] = (data >> 4) & 0x07;
            apu->pulse_sweep_negate[ch] = (data & 0x08) != 0;
            apu->pulse_sweep_shift[ch] = data & 0x07;
            apu->pulse_sweep_reload[ch] = true;
            break;
        case 2:
            apu->pulse_timer_reload[ch] = (apu->pulse_timer_reload[ch] & 0x0700) | data;
            break;
        case 3:
            apu->pulse_timer_reload[ch] = (apu->pulse_timer_reload[ch] & 0x00FF) | ((uint16_t)(data & 0x07) << 8);
            if (apu->pulse_enabled[ch]) {
                load_length(apu, ch, data);
            }
            apu->pulse_sequence_idx[ch] = 0;
            apu->pulse_envelope_start[ch] = true;
            break;
    }
}

void apu_reset(NES *nes) {
    APU2A03 *a = &nes->apu;
    apu_write_reg(nes, 0x4015, 0);
    a->pulse_halt[0] = a->pulse_halt[1] = a->noise_halt = false;
    a->length_pending = a->halt_pending = 0;
    a->frame_irq_active = false;
    cpu_set_irq_line(&nes->cpu, APU_IRQ_SOURCE_FRAME, false);
    a->frame_cycles = 0;
    a->frame_clock_block = 0;
    apu_write_reg(nes, 0x4017, (a->frame_next_mode ? 0x80 : 0) | (a->frame_irq_inhibit ? 0x40 : 0));
}

void apu_write_reg(NES *nes, uint16_t address, uint8_t data) {
    APU2A03 *apu = &nes->apu;
    if (address >= 0x4000 && address <= 0x4003) {
        apu_write_pulse_reg(apu, 0, address & 0x03, data);
    } else if (address >= 0x4004 && address <= 0x4007) {
        apu_write_pulse_reg(apu, 1, address & 0x03, data);
    } else if (address >= 0x4008 && address <= 0x400B) {
        switch (address & 0x03) {
            case 0:
                set_halt(apu, 2, (data & 0x80) != 0);
                apu->triangle_linear_reload = data & 0x7F;
                break;
            case 1:
                break;
            case 2:
                apu->triangle_timer_reload = (apu->triangle_timer_reload & 0x0700) | data;
                break;
            case 3:
                apu->triangle_timer_reload = (apu->triangle_timer_reload & 0x00FF) | ((uint16_t)(data & 0x07) << 8);
                if (apu->triangle_enabled) {
                    load_length(apu, 2, data);
                }
                apu->triangle_linear_reload_flag = true;
                break;
        }
    } else if (address >= 0x400C && address <= 0x400F) {
        switch (address & 0x03) {
            case 0:
                set_halt(apu, 3, (data & 0x20) != 0);
                apu->noise_constant_volume = (data & 0x10) != 0;
                apu->noise_volume = data & 0x0F;
                break;
            case 1:
                break;
            case 2:
                apu->noise_mode = (data & 0x80) != 0;
                apu->noise_rate = data & 15;
                apu->noise_timer_reload = (apu->region == NES_PAL ? PAL_NOISE_PERIOD : NOISE_PERIOD)[data & 15];
                break;
            case 3:
                if (apu->noise_enabled) {
                    load_length(apu, 3, data);
                }
                apu->noise_envelope_start = true;
                break;
        }
    } else if (address >= 0x4010 && address <= 0x4013) {
        switch (address & 0x03) {
            case 0:
                apu->dmc_irq_enable = (data & 0x80) != 0;
                apu->dmc_loop = (data & 0x40) != 0;
                apu->dmc_rate = data & 0x0F;
                apu->dmc_timer_reload = (apu->region == NES_PAL ? PAL_DMC_PERIOD : DMC_RATE_TABLE)[apu->dmc_rate];
                if (!apu->dmc_irq_enable) {
                    apu->dmc_irq_active = false;
                    cpu_set_irq_line(&nes->cpu, APU_IRQ_SOURCE_DMC, false);
                }
                break;
            case 1:
                apu->dmc_value = data & 0x7F;
                break;
            case 2:
                apu->dmc_sample_addr = 0xC000 | ((uint16_t)data << 6);
                break;
            case 3:
                apu->dmc_sample_len = ((uint16_t)data << 4) + 1;
                break;
        }
    } else if (address == 0x4015) {
        apu->pulse_enabled[0] = (data & 0x01) != 0;
        apu->pulse_enabled[1] = (data & 0x02) != 0;
        apu->triangle_enabled = (data & 0x04) != 0;
        apu->noise_enabled = (data & 0x08) != 0;
        apu->dmc_enabled = (data & 0x10) != 0;

        apu->dmc_irq_active = false;
        cpu_set_irq_line(&nes->cpu, APU_IRQ_SOURCE_DMC, false);

        if (!apu->pulse_enabled[0]) apu->pulse_length_counter[0] = 0;
        if (!apu->pulse_enabled[1]) apu->pulse_length_counter[1] = 0;
        if (!apu->triangle_enabled) apu->triangle_length_counter = 0;
        if (!apu->noise_enabled)    apu->noise_length_counter = 0;
        apu->length_pending &= data & 15;

        if (apu->dmc_enabled) {
            if (apu->dmc_bytes_remaining == 0) {
                apu->dmc_current_addr = apu->dmc_sample_addr;
                apu->dmc_bytes_remaining = apu->dmc_sample_len;
            }
            if (apu->dmc_buffer_empty && apu->dmc_bytes_remaining)
                nes_request_dmc_dma(nes, true);
        } else {
            apu->dmc_bytes_remaining = 0;
            nes->dmc_dma_pending = false;
        }
    } else if (address == 0x4017) {
        apu->frame_next_mode = (data & 0x80) != 0;
        apu->frame_irq_inhibit = (data & 0x40) != 0;

        if (apu->frame_irq_inhibit) {
            apu->frame_irq_active = false;
            cpu_set_irq_line(&nes->cpu, APU_IRQ_SOURCE_FRAME, false);
        }

        // The frame counter reset does not occur on the write cycle itself.
        // The hardware delays the reset until the next appropriate APU phase,
        // giving a 3- or 4-CPU-cycle delay depending on the current phase.
        apu->frame_counter_reset_pending = true;
        apu->frame_counter_reset_delay = apu->clock_toggle ? 3 : 4;
    }
}

uint8_t apu_read_reg(NES *nes, uint16_t address) {
    APU2A03 *apu = &nes->apu;
    if (address == 0x4015) {
        uint8_t data = nes->cpu_open_bus & 0x20;
        if (apu->pulse_length_counter[0] > 0) data |= 0x01;
        if (apu->pulse_length_counter[1] > 0) data |= 0x02;
        if (apu->triangle_length_counter > 0) data |= 0x04;
        if (apu->noise_length_counter > 0)    data |= 0x08;
        if (apu->dmc_bytes_remaining > 0)     data |= 0x10;
        if (apu->frame_irq_active)            data |= 0x40;
        if (apu->dmc_irq_active)              data |= 0x80;

        apu->frame_irq_active = false;
        cpu_set_irq_line(&nes->cpu, APU_IRQ_SOURCE_FRAME, false);

        return data;
    }
    return nes->cpu_open_bus;
}

static void apu_frame_counter_reset(APU2A03 *apu) {
    apu->frame_counter_reset_pending = false;
    apu->frame_counter_reset_delay = 0;
    apu->frame_cycles = 0;
    apu->frame_mode = apu->frame_next_mode;

    // In 5-step mode, the reset event immediately generates the first
    // quarter+half-frame clock. In 4-step mode it only resets the sequence.
    if (apu->frame_mode && !apu->frame_clock_block) {
        apu_clock_half_frame(apu);
        apu->frame_clock_block = 2;
    }
}

static void apu_step_frame_sequencer(APU2A03 *apu, NES *nes) {
    bool pal = apu->region == NES_PAL;
    unsigned q1 = pal ? 8313 : 7457, h1 = pal ? 16627 : 14913;
    unsigned q2 = pal ? 24939 : 22371;
    unsigned h2 = apu->frame_mode ? (pal ? 41565 : 37281) : (pal ? 33253 : 29829);
    unsigned cycle = ++apu->frame_cycles;
    if (!apu->frame_mode && cycle >= h2 - 1 && !apu->frame_irq_inhibit) {
        apu->frame_irq_active = true;
        cpu_set_irq_line(&nes->cpu, APU_IRQ_SOURCE_FRAME, true);
    }
    if (!apu->frame_clock_block) {
        if (cycle == h1 || cycle == h2) {
            apu_clock_half_frame(apu);
            apu->frame_clock_block = 2;
        } else if (cycle == q1 || cycle == q2) {
            apu_clock_quarter_frame(apu);
            apu->frame_clock_block = 2;
        }
    }
    if (cycle == h2 + 1) apu->frame_cycles = 0;
    if (apu->frame_counter_reset_pending && apu->frame_counter_reset_delay &&
        --apu->frame_counter_reset_delay == 0) apu_frame_counter_reset(apu);
    if (apu->frame_clock_block) --apu->frame_clock_block;
}

static void apu_step_dmc(APU2A03 *apu, NES *nes) {
    /*
     * The memory reader is controlled by bytes_remaining.  Clearing $4015.4
     * stops new DMA reads, but it does not stop the DMC timer/output unit or
     * discard a byte already in the sample buffer.
     */
    if (apu->dmc_buffer_empty && apu->dmc_bytes_remaining > 0) {
        nes_request_dmc_dma(nes, false);
    }

    /* The table entries are complete CPU-cycle periods. */
    if (apu->dmc_timer == 0) {
        apu->dmc_timer = (apu->dmc_timer_reload > 0)
            ? (uint16_t)(apu->dmc_timer_reload - 1u)
            : 0;

        if (!apu->dmc_silent) {
            if (apu->dmc_shift_reg & 1u) {
                if (apu->dmc_value <= 125u) {
                    apu->dmc_value += 2u;
                }
            } else if (apu->dmc_value >= 2u) {
                apu->dmc_value -= 2u;
            }
        }

        apu->dmc_shift_reg >>= 1;
        if (apu->dmc_bits_remaining > 0) {
            apu->dmc_bits_remaining--;
        }

        if (apu->dmc_bits_remaining == 0) {
            apu->dmc_bits_remaining = 8;
            if (apu->dmc_buffer_empty) {
                apu->dmc_silent = true;
            } else {
                apu->dmc_silent = false;
                apu->dmc_shift_reg = apu->dmc_buffer;
                apu->dmc_buffer_empty = true;

                /* The bus arbiter performs the refill on a DMA get cycle. */
                if (apu->dmc_bytes_remaining > 0) {
                    nes_request_dmc_dma(nes, false);
                }
            }
        }
    } else {
        apu->dmc_timer--;
    }
}

static void apu_step_timers(APU2A03 *apu) {
    // Oscillator timers free-run even when their channel is silenced.
    if (apu->triangle_timer == 0) {
        apu->triangle_timer = apu->triangle_timer_reload;
        if (apu->triangle_length_counter && apu->triangle_linear_counter)
            apu->triangle_sequence_idx = (apu->triangle_sequence_idx + 1) & 31;
    } else apu->triangle_timer--;

    if (apu->noise_timer == 0) {
        apu->noise_timer = apu->noise_timer_reload ? apu->noise_timer_reload - 1 : 0;
        uint16_t feedback = (apu->noise_shift_reg & 1) ^
            ((apu->noise_shift_reg >> (apu->noise_mode ? 6 : 1)) & 1);
        apu->noise_shift_reg = (apu->noise_shift_reg >> 1) | (feedback << 14);
    } else apu->noise_timer--;

    apu->clock_toggle = !apu->clock_toggle;
    if (apu->clock_toggle) {
        for (int ch = 0; ch < 2; ch++) {
            if (apu->pulse_timer[ch] == 0) {
                apu->pulse_timer[ch] = apu->pulse_timer_reload[ch];
                apu->pulse_sequence_idx[ch] = (apu->pulse_sequence_idx[ch] + 1) & 7;
            } else apu->pulse_timer[ch]--;
        }
    }
}

static float apu_mix_audio_output(APU2A03 *apu) {
    float ch_out[2] = {0.0f, 0.0f};
    float tri_out = 0.0f;
    float noise_out = 0.0f;
    float dmc_out = (float)apu->dmc_value;

    for (int ch = 0; ch < 2; ch++) {
        if (apu->pulse_length_counter[ch] > 0 && !is_sweep_muting(apu, ch)) {
            if (DUTY_TABLE[apu->pulse_duty[ch]][apu->pulse_sequence_idx[ch]]) {
                ch_out[ch] = apu->pulse_constant_volume[ch]
                    ? (float)apu->pulse_volume[ch]
                    : (float)apu->pulse_envelope_decay[ch];
            }
        }
    }

    // A stopped triangle holds its DAC value; it does not output zero.
    tri_out = (float)TRIANGLE_TABLE[apu->triangle_sequence_idx];

    if (apu->noise_enabled && apu->noise_length_counter > 0 && !(apu->noise_shift_reg & 1)) {
        noise_out = apu->noise_constant_volume
            ? (float)apu->noise_volume
            : (float)apu->noise_envelope_decay;
    }

    return apu_mix_dac((unsigned)ch_out[0], (unsigned)ch_out[1], (unsigned)tri_out,
        (unsigned)noise_out, (unsigned)dmc_out);
}

void apu_step(APU2A03 *apu, NES *nes) {
    // A coincident length clock sees the old halt/load values. A reload
    // loses to a decrement of a nonzero counter, but loads an idle counter.
    uint8_t loaded[4] = {0}; bool halted[4] = {0};
    for (unsigned ch = 0; ch < 4; ++ch) {
        if (apu->length_pending & (1u << ch)) {
            loaded[ch] = *length_counter(apu, ch);
            *length_counter(apu, ch) = apu->length_previous[ch];
        }
        if (apu->halt_pending & (1u << ch)) {
            halted[ch] = *halt_flag(apu, ch);
            *halt_flag(apu, ch) = apu->halt_previous[ch];
        }
    }
    apu_step_frame_sequencer(apu, nes);
    for (unsigned ch = 0; ch < 4; ++ch) {
        if ((apu->length_pending & (1u << ch)) &&
            *length_counter(apu, ch) == apu->length_previous[ch])
            *length_counter(apu, ch) = loaded[ch];
        if (apu->halt_pending & (1u << ch)) *halt_flag(apu, ch) = halted[ch];
    }
    apu->length_pending = apu->halt_pending = 0;
    apu_step_dmc(apu, nes);
    apu_step_timers(apu);

    apu_audio_clock(apu, apu_mix_audio_output(apu) + expansion_audio_clock(nes));
}
