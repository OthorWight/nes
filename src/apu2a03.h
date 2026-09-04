#ifndef APU2A03_H
#define APU2A03_H

#include <stdint.h>
#include <stdbool.h>

#define APU_IRQ_SOURCE_FRAME 1
#define APU_IRQ_SOURCE_DMC   2

typedef struct NES NES;

typedef struct {
    // Pulse 1 & Pulse 2
    bool     pulse_enabled[2];
    uint8_t  pulse_duty[2];
    bool     pulse_halt[2];
    bool     pulse_constant_volume[2];
    uint8_t  pulse_volume[2];

    uint16_t pulse_timer[2];
    uint16_t pulse_timer_reload[2];
    uint8_t  pulse_length_counter[2];
    uint8_t  pulse_sequence_idx[2];

    // Pulse Envelopes
    uint8_t  pulse_envelope_decay[2];
    uint8_t  pulse_envelope_divider[2];
    bool     pulse_envelope_start[2];

    // Sweep Unit
    bool     pulse_sweep_enabled[2];
    uint8_t  pulse_sweep_period[2];
    bool     pulse_sweep_negate[2];
    uint8_t  pulse_sweep_shift[2];
    uint8_t  pulse_sweep_divider[2];
    bool     pulse_sweep_reload[2];

    // Triangle Channel
    bool     triangle_enabled;
    bool     triangle_control_flag;
    uint8_t  triangle_linear_reload;
    uint8_t  triangle_linear_counter;
    bool     triangle_linear_reload_flag;
    uint16_t triangle_timer;
    uint16_t triangle_timer_reload;
    uint8_t  triangle_length_counter;
    uint8_t  triangle_sequence_idx;

    // Noise Channel
    bool     noise_enabled;
    bool     noise_halt;
    bool     noise_constant_volume;
    uint8_t  noise_volume;
    uint16_t noise_timer;
    uint16_t noise_timer_reload;
    uint8_t  noise_length_counter;
    uint16_t noise_shift_reg;
    bool     noise_mode;
    uint8_t  noise_envelope_decay;
    uint8_t  noise_envelope_divider;
    bool     noise_envelope_start;

    // DMC Channel
    bool     dmc_enabled;
    bool     dmc_irq_enable;
    bool     dmc_loop;
    bool     dmc_irq_active;
    uint8_t  dmc_rate;
    uint16_t dmc_timer;
    uint16_t dmc_timer_reload;
    uint8_t  dmc_value;
    uint16_t dmc_sample_addr;
    uint16_t dmc_current_addr;
    uint16_t dmc_sample_len;
    uint16_t dmc_bytes_remaining;
    uint8_t  dmc_shift_reg;
    uint8_t  dmc_bits_remaining;
    uint8_t  dmc_buffer;
    bool     dmc_buffer_empty;
    bool     dmc_silent;

    // Frame Counter Sequencer
    bool     frame_mode;
    bool     frame_irq_inhibit;
    bool     frame_irq_active;
    uint32_t frame_cycles;

    // $4017 reset is delayed by 3 or 4 CPU cycles depending on APU phase.
    bool     frame_counter_reset_pending;
    uint8_t  frame_counter_reset_delay;

    // Audio Output Buffer & Downsampler
    double   audio_accumulator;
    float    audio_buffer[4096];
    uint32_t audio_buffer_idx;

    // CPU / 2 Divider Toggle
    bool     clock_toggle;
} APU2A03;

void    apu_init(APU2A03 *apu);
void    apu_write_reg(NES *nes, uint16_t address, uint8_t data);
uint8_t apu_read_reg(NES *nes, uint16_t address);
void    apu_step(APU2A03 *apu, NES *nes);

#endif