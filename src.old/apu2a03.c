#include "apu2a03.h"
#include <string.h>

static const uint8_t LENGTH_TABLE[32] = {
    10, 254, 20,  2, 40,  4, 80,  6, 30,  8, 60, 10, 14, 12, 26, 14,
    12,  16, 24, 18, 48, 20, 96, 22, 72, 24, 80, 26, 16, 28, 30, 30
};

static const uint8_t DUTY_TABLE[4][8] = {
    {0, 1, 0, 0, 0, 0, 0, 0},
    {0, 1, 1, 0, 0, 0, 0, 0},
    {0, 1, 1, 1, 1, 0, 0, 0},
    {1, 0, 0, 1, 1, 1, 1, 1}
};

static const uint8_t TRIANGLE_TABLE[32] = {
    15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0,
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15
};

static const uint16_t NOISE_PERIOD[16] = {
    4, 8, 16, 32, 64, 96, 128, 160, 202, 254, 380, 508, 762, 1016, 2034, 4068
};

static const uint16_t DMC_RATE_TABLE[16] = {
    428, 380, 340, 320, 286, 254, 226, 202, 180, 166, 150, 134, 116, 100, 84, 54
};

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

static void dmc_fetch(APU2A03 *apu, CPUBus *bus) {
    if (apu->dmc_bytes_remaining > 0) {
        apu->dmc_buffer = bus->read(bus->bus_context, apu->dmc_current_addr);
        apu->dmc_buffer_empty = false;
        apu->dmc_current_addr = (apu->dmc_current_addr + 1) | 0x8000;
        apu->dmc_bytes_remaining--;
        if (apu->dmc_bytes_remaining == 0) {
            if (apu->dmc_loop) {
                apu->dmc_current_addr = apu->dmc_sample_addr;
                apu->dmc_bytes_remaining = apu->dmc_sample_len;
            }
        }
    }
}

static void apu_clock_quarter_frame(APU2A03 *apu) {
    // Linear counter (Triangle)
    if (apu->triangle_linear_reload_flag) {
        apu->triangle_linear_counter = apu->triangle_linear_reload;
    } else if (apu->triangle_linear_counter > 0) {
        apu->triangle_linear_counter--;
    }
    if (!apu->triangle_control_flag) {
        apu->triangle_linear_reload_flag = false;
    }

    // Envelopes (Pulse 1 & 2)
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

    // Envelope (Noise)
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

    // Pulse channel lengths and sweeps
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

    // Triangle length
    if (apu->triangle_length_counter > 0 && !apu->triangle_control_flag) {
        apu->triangle_length_counter--;
    }

    // Noise length
    if (apu->noise_length_counter > 0 && !apu->noise_halt) {
        apu->noise_length_counter--;
    }
}

void apu_init(APU2A03 *apu) {
    memset(apu, 0, sizeof(APU2A03));
    apu->noise_shift_reg = 1;
    apu->dmc_buffer_empty = true;
    apu->dmc_bits_remaining = 8;
    apu->dmc_timer_reload = DMC_RATE_TABLE[0];
    apu->audio_accumulator = 0.0;
    apu->audio_buffer_idx = 0;
}

void apu_write_reg(APU2A03 *apu, uint16_t address, uint8_t data) {
    if (address >= 0x4000 && address <= 0x4003) {
        int ch = 0;
        switch (address & 0x03) {
            case 0:
                apu->pulse_duty[ch] = (data >> 6) & 0x03;
                apu->pulse_halt[ch] = (data & 0x20) != 0;
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
                    apu->pulse_length_counter[ch] = LENGTH_TABLE[data >> 3];
                }
                apu->pulse_sequence_idx[ch] = 0;
                apu->pulse_envelope_start[ch] = true;
                break;
        }
    } else if (address >= 0x4004 && address <= 0x4007) {
        int ch = 1;
        switch (address & 0x03) {
            case 0:
                apu->pulse_duty[ch] = (data >> 6) & 0x03;
                apu->pulse_halt[ch] = (data & 0x20) != 0;
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
                    apu->pulse_length_counter[ch] = LENGTH_TABLE[data >> 3];
                }
                apu->pulse_sequence_idx[ch] = 0;
                apu->pulse_envelope_start[ch] = true;
                break;
        }
    } else if (address >= 0x4008 && address <= 0x400B) {
        switch (address & 0x03) {
            case 0:
                apu->triangle_control_flag = (data & 0x80) != 0;
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
                    apu->triangle_length_counter = LENGTH_TABLE[data >> 3];
                }
                apu->triangle_linear_reload_flag = true;
                break;
        }
    } else if (address >= 0x400C && address <= 0x400F) {
        switch (address & 0x03) {
            case 0:
                apu->noise_halt = (data & 0x20) != 0;
                apu->noise_constant_volume = (data & 0x10) != 0;
                apu->noise_volume = data & 0x0F;
                break;
            case 1:
                break;
            case 2:
                apu->noise_mode = (data & 0x80) != 0;
                apu->noise_timer_reload = NOISE_PERIOD[data & 0x0F];
                break;
            case 3:
                if (apu->noise_enabled) {
                    apu->noise_length_counter = LENGTH_TABLE[data >> 3];
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
                apu->dmc_timer_reload = DMC_RATE_TABLE[apu->dmc_rate];
                if (!apu->dmc_irq_enable) {
                    apu->dmc_irq_active = false;
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

        if (!apu->pulse_enabled[0]) apu->pulse_length_counter[0] = 0;
        if (!apu->pulse_enabled[1]) apu->pulse_length_counter[1] = 0;
        if (!apu->triangle_enabled) apu->triangle_length_counter = 0;
        if (!apu->noise_enabled) apu->noise_length_counter = 0;
        if (apu->dmc_enabled) {
            if (apu->dmc_bytes_remaining == 0) {
                apu->dmc_current_addr = apu->dmc_sample_addr;
                apu->dmc_bytes_remaining = apu->dmc_sample_len;
            }
        } else {
            apu->dmc_bytes_remaining = 0;
        }
    } else if (address == 0x4017) {
        apu->frame_cycles = 0;
        if (data & 0x80) {
            apu_clock_half_frame(apu);
        }
    }
}

uint8_t apu_read_reg(APU2A03 *apu, uint16_t address) {
    if (address == 0x4015) {
        uint8_t data = 0;
        if (apu->pulse_length_counter[0] > 0) data |= 0x01;
        if (apu->pulse_length_counter[1] > 0) data |= 0x02;
        if (apu->triangle_length_counter > 0) data |= 0x04;
        if (apu->noise_length_counter > 0) data |= 0x08;
        if (apu->dmc_bytes_remaining > 0) data |= 0x10;
        if (apu->dmc_irq_active) data |= 0x80;
        return data;
    }
    return 0;
}

void apu_step(APU2A03 *apu, uint32_t cpu_cycles, CPUBus *bus, CPU6502 *cpu) {
    for (uint32_t cycle = 0; cycle < cpu_cycles; cycle++) {
        apu->frame_cycles++;
        if (apu->frame_cycles == 7457) {
            apu_clock_quarter_frame(apu);
        } else if (apu->frame_cycles == 14913) {
            apu_clock_half_frame(apu);
        } else if (apu->frame_cycles == 22371) {
            apu_clock_quarter_frame(apu);
        } else if (apu->frame_cycles >= 29829) {
            apu_clock_half_frame(apu);
            apu->frame_cycles = 0;
        }

        // DMC timer tick
        if (apu->dmc_enabled) {
            if (apu->dmc_buffer_empty && apu->dmc_bytes_remaining > 0) {
                dmc_fetch(apu, bus);
            }

            if (apu->dmc_timer == 0) {
                apu->dmc_timer = apu->dmc_timer_reload;
                if (!apu->dmc_silent) {
                    if (apu->dmc_shift_reg & 1) {
                        if (apu->dmc_value <= 125) {
                            apu->dmc_value += 2;
                        }
                    } else {
                        if (apu->dmc_value >= 2) {
                            apu->dmc_value -= 2;
                        }
                    }
                }
                apu->dmc_shift_reg >>= 1;
                apu->dmc_bits_remaining--;
                if (apu->dmc_bits_remaining == 0) {
                    apu->dmc_bits_remaining = 8;
                    if (apu->dmc_buffer_empty) {
                        apu->dmc_silent = true;
                    } else {
                        apu->dmc_silent = false;
                        apu->dmc_shift_reg = apu->dmc_buffer;
                        apu->dmc_buffer_empty = true;
                        if (apu->dmc_bytes_remaining > 0) {
                            dmc_fetch(apu, bus);
                        } else if (apu->dmc_irq_enable && !apu->dmc_loop) {
                            apu->dmc_irq_active = true;
                            if (cpu != NULL) {
                                cpu_trigger_irq(cpu);
                            }
                        }
                    }
                }
            } else {
                apu->dmc_timer--;
            }
        }

        // Triangle timer
        if (apu->triangle_enabled && apu->triangle_length_counter > 0 && apu->triangle_linear_counter > 0) {
            if (apu->triangle_timer == 0) {
                apu->triangle_timer = apu->triangle_timer_reload;
                if (apu->triangle_timer_reload >= 2) {
                    apu->triangle_sequence_idx = (apu->triangle_sequence_idx + 1) & 31;
                }
            } else {
                apu->triangle_timer--;
            }
        }

        // Pulse and noise dividers (CPU clock / 2)
        apu->clock_toggle = !apu->clock_toggle;
        if (apu->clock_toggle) {
            for (int ch = 0; ch < 2; ch++) {
                if (apu->pulse_timer[ch] == 0) {
                    apu->pulse_timer[ch] = apu->pulse_timer_reload[ch];
                    apu->pulse_sequence_idx[ch] = (apu->pulse_sequence_idx[ch] + 1) & 0x07;
                } else {
                    apu->pulse_timer[ch]--;
                }
            }

            if (apu->noise_enabled && apu->noise_length_counter > 0) {
                if (apu->noise_timer == 0) {
                    apu->noise_timer = apu->noise_timer_reload;
                    uint16_t feedback = (apu->noise_shift_reg & 1) ^ ((apu->noise_shift_reg >> (apu->noise_mode ? 6 : 1)) & 1);
                    apu->noise_shift_reg = (apu->noise_shift_reg >> 1) | (feedback << 14);
                } else {
                    apu->noise_timer--;
                }
            }
        }

        // Downsample to 44.1 kHz
        apu->audio_accumulator += (44100.0 / 1789773.0);
        if (apu->audio_accumulator >= 1.0) {
            apu->audio_accumulator -= 1.0;

            float pulse_out = 0.0f;
            float tnd_out = 0.0f;
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

            if (apu->triangle_enabled && apu->triangle_length_counter > 0 && apu->triangle_linear_counter > 0) {
                tri_out = (float)TRIANGLE_TABLE[apu->triangle_sequence_idx];
            }

            if (apu->noise_enabled && apu->noise_length_counter > 0 && !(apu->noise_shift_reg & 1)) {
                noise_out = apu->noise_constant_volume
                    ? (float)apu->noise_volume
                    : (float)apu->noise_envelope_decay;
            }

            if (ch_out[0] != 0.0f || ch_out[1] != 0.0f) {
                pulse_out = 95.88f / ((8128.0f / (ch_out[0] + ch_out[1])) + 100.0f);
            }

            if (tri_out != 0.0f || noise_out != 0.0f || dmc_out != 0.0f) {
                tnd_out = 159.79f / ((1.0f / ((tri_out / 8227.0f) + (noise_out / 12241.0f) + (dmc_out / 22638.0f))) + 100.0f);
            }

            if (apu->audio_buffer_idx < 4096) {
                apu->audio_buffer[apu->audio_buffer_idx++] = pulse_out + tnd_out;
            }
        }
    }
}

void apu_clock_frame(APU2A03 *apu) {
    (void)apu;
}