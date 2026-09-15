#include "expansion_audio.h"
#include "nes_system.h"
#include "state_io.h"
#include <string.h>

enum { MMC5_AUDIO_IRQ = 3 };
static const uint8_t lengths[32] = {
    10,254,20,2,40,4,80,6,160,8,60,10,14,12,26,14,
    12,16,24,18,48,20,96,22,192,24,72,26,16,28,32,30
};
static const uint8_t duty[4] = {0x02,0x06,0x1E,0xF9};
static const float ay_volume[16] = {
    0,.004654f,.007721f,.010955f,.016998f,.025199f,.037416f,.055431f,
    .080289f,.112351f,.164149f,.231225f,.327061f,.462563f,.649702f,1
};
void expansion_audio_reset(NES *n) {
    memset(&n->expansion, 0, sizeof(n->expansion));
    n->expansion.s5b.noise = 1;
    n->expansion.n163.channel = 7;
    cpu_set_irq_line(&n->cpu, MMC5_AUDIO_IRQ, false);
}

static float vrc6_clock(Vrc6Audio *a) {
    if (!(a->control & 1)) for (unsigned ch = 0; ch < 3; ++ch) {
        if (!(a->regs[ch][2] & 0x80)) continue;
        unsigned period = a->regs[ch][1] | ((a->regs[ch][2] & 15) << 8);
        period >>= (a->control & 4) ? 8 : (a->control & 2) ? 4 : 0;
        if (a->timer[ch]) --a->timer[ch];
        else {
            a->timer[ch] = (uint16_t)period;
            if (ch < 2) a->phase[ch] = (a->phase[ch] + 1) & 15;
            else {
                if (!(a->phase[ch] & 1)) a->accumulator += a->regs[2][0] & 63;
                if (++a->phase[ch] == 14) { a->phase[ch] = 0; a->accumulator = 0; }
            }
        }
    }
    unsigned output = 0;
    for (unsigned ch = 0; ch < 2; ++ch)
        if ((a->regs[ch][2] & 0x80) && ((a->regs[ch][0] & 0x80) || a->phase[ch] <= ((a->regs[ch][0] >> 4) & 7)))
            output += a->regs[ch][0] & 15;
    if (a->regs[2][2] & 0x80) output += a->accumulator >> 3;
    return output * (0.5f / 61);
}

static float n163_clock(N163Audio *a) {
    if (a->disabled) return 0;
    if (++a->divider == 15) {
        a->divider = 0;
        unsigned first = 7 - ((a->ram[0x7F] >> 4) & 7);
        if (a->channel < first || a->channel > 7) a->channel = 7;
        unsigned base = 0x40 + a->channel * 8;
        uint32_t frequency = a->ram[base] | ((uint32_t)a->ram[base + 2] << 8) | ((uint32_t)(a->ram[base + 4] & 3) << 16);
        uint32_t phase = a->ram[base + 1] | ((uint32_t)a->ram[base + 3] << 8) | ((uint32_t)a->ram[base + 5] << 16);
        unsigned length = 256 - (a->ram[base + 4] & 0xFC);
        phase = (phase + frequency) % (length << 16);
        a->ram[base + 1] = (uint8_t)phase;
        a->ram[base + 3] = (uint8_t)(phase >> 8);
        a->ram[base + 5] = (uint8_t)(phase >> 16);
        unsigned position = ((phase >> 16) + a->ram[base + 6]) & 255;
        unsigned sample = (a->ram[position >> 1] >> ((position & 1) * 4)) & 15;
        a->output = (int16_t)(((int)sample - 8) * (a->ram[base + 7] & 15));
        a->channel = a->channel == first ? 7 : a->channel - 1;
    }
    return a->output * (0.4f / 120);
}

static void s5b_envelope(S5bAudio *a) {
    if (a->holding) return;
    int next = a->envelope + a->direction;
    if (next >= 0 && next <= 15) { a->envelope = (int8_t)next; return; }
    unsigned shape = a->regs[13];
    if (!(shape & 8)) { a->envelope = 0; a->holding = true; }
    else if (shape & 1) {
        if (shape & 2) a->direction = -a->direction;
        a->envelope = a->direction > 0 ? 15 : 0;
        a->holding = true;
    } else {
        if (shape & 2) a->direction = -a->direction;
        a->envelope = a->direction > 0 ? 0 : 15;
    }
}
static float s5b_clock(S5bAudio *a) {
    if (++a->divider == 8) {
        a->divider = 0;
        for (unsigned ch = 0; ch < 3; ++ch) {
            unsigned period = a->regs[ch * 2] | ((a->regs[ch * 2 + 1] & 15) << 8);
            if (a->timer[ch]) --a->timer[ch];
            else { a->timer[ch] = (uint16_t)(period ? period - 1 : 0); a->tone[ch] ^= 1; }
        }
        if (++a->noise_divider == 2) {
            a->noise_divider = 0;
            unsigned period = a->regs[6] & 31;
            if (a->noise_timer) --a->noise_timer;
            else {
                a->noise_timer = (uint16_t)(period ? period - 1 : 0);
                unsigned bit = (a->noise ^ (a->noise >> 3)) & 1;
                a->noise = (a->noise >> 1) | (bit << 16);
            }
            unsigned envelope = a->regs[11] | (a->regs[12] << 8);
            if (a->envelope_timer) --a->envelope_timer;
            else { a->envelope_timer = (uint16_t)(envelope ? envelope - 1 : 0); s5b_envelope(a); }
        }
    }
    float output = 0;
    for (unsigned ch = 0; ch < 3; ++ch) {
        bool tone = (a->regs[7] & (1u << ch)) || a->tone[ch];
        bool noise = (a->regs[7] & (8u << ch)) || (a->noise & 1);
        unsigned volume = (a->regs[8 + ch] & 16) ? (unsigned)a->envelope : a->regs[8 + ch] & 15;
        if (tone && noise) output += ay_volume[volume & 15];
    }
    return output * (0.4f / 3);
}

static float mmc5_clock(Mmc5Audio *a) {
    if (++a->frame >= 7457) {
        a->frame = 0; a->half = !a->half;
        for (unsigned ch = 0; ch < 2; ++ch) {
            unsigned volume = a->regs[ch][0] & 15;
            if (a->start[ch]) { a->envelope[ch] = 15; a->divider[ch] = volume; a->start[ch] = false; }
            else if (a->divider[ch]) --a->divider[ch];
            else {
                a->divider[ch] = volume;
                if (a->envelope[ch]) --a->envelope[ch];
                else if (a->regs[ch][0] & 32) a->envelope[ch] = 15;
            }
            if (!a->half && !(a->regs[ch][0] & 32) && a->length[ch]) --a->length[ch];
        }
    }
    a->toggle = !a->toggle;
    unsigned output = 0;
    for (unsigned ch = 0; ch < 2; ++ch) {
        unsigned period = a->regs[ch][2] | ((a->regs[ch][3] & 7) << 8);
        if (a->toggle) {
            if (a->timer[ch]) --a->timer[ch];
            else { a->timer[ch] = period; a->phase[ch] = (a->phase[ch] + 1) & 7; }
        }
        if (a->length[ch] && (duty[a->regs[ch][0] >> 6] & (1u << a->phase[ch])))
            output += a->regs[ch][0] & 16 ? a->regs[ch][0] & 15 : a->envelope[ch];
    }
    return output * (0.35f / 30) + a->pcm * (0.25f / 255);
}

static float fds_clock(FdsAudio *a) {
    if (!a->enabled) return 0;
    for (unsigned ch = 0; ch < 2; ++ch) {
        unsigned reg = a->regs[ch ? 4 : 0];
        if (!(reg & 0x80) && !(a->regs[3] & 0x40) && a->regs[10]) {
            if (a->envelope_timer[ch]) --a->envelope_timer[ch];
            else {
                a->envelope_timer[ch] = 8u * (reg % 64 + 1) * a->regs[10] - 1;
                if ((reg & 0x40) && a->gain[ch] < 32) ++a->gain[ch];
                else if (!(reg & 0x40) && a->gain[ch]) --a->gain[ch];
            }
        }
    }
    if (!(a->regs[7] & 0x80)) {
        unsigned phase = a->mod_phase + (a->regs[6] | ((a->regs[7] & 15) << 8));
        a->mod_phase = (uint16_t)phase;
        if (phase > 0xFFFF) {
            static const int8_t steps[8] = {0,1,2,4,0,-4,-2,-1};
            unsigned step = a->modulation[a->mod_position];
            a->mod_position = (a->mod_position + 1) & 63;
            a->bias = step == 4 ? 0 : (int16_t)((a->bias + steps[step & 7] + 64) & 127) - 64;
        }
    }
    if (a->regs[3] & 0x80) { a->position = 0; a->phase = 0; }
    else if (!(a->regs[9] & 0x80)) {
        int frequency = a->regs[2] | ((a->regs[3] & 15) << 8);
        int product = a->bias * a->gain[1];
        int remainder = product & 15;
        int modulation = product >> 4;
        if (remainder && !(modulation & 0x80)) modulation += a->bias < 0 ? -1 : 2;
        if (modulation >= 192) modulation -= 256;
        else if (modulation < -64) modulation += 256;
        product = frequency * modulation;
        int delta = (product >> 6) + ((product & 63) >= 32);
        unsigned phase = a->phase + (unsigned)(frequency + delta);
        a->position = (a->position + (phase >> 16)) & 63;
        a->phase = (uint16_t)phase;
    }
    unsigned volume = a->gain[0] > 32 ? 32 : a->gain[0];
    return (a->regs[9] & 0x80) ? 0 : (a->wave[a->position] * volume) *
        (0.4f / (63 * 32)) * (2.0f / ((a->regs[9] & 3) + 2));
}

float expansion_audio_clock(NES *n) {
    if (!n->cart) return 0;
    switch (n->cart->mapper_id) {
        case 5: return mmc5_clock(&n->expansion.mmc5);
        case 19: return n163_clock(&n->expansion.n163);
        case 20: return fds_clock(&n->expansion.fds);
        case 24: case 26: return vrc6_clock(&n->expansion.vrc6);
        case 69: return s5b_clock(&n->expansion.s5b);
        case 85: return vrc7_audio_clock(&n->expansion.vrc7, nes_region_cpu_hz(n->apu.region));
        default: return 0;
    }
}

bool expansion_audio_write(NES *n, uint16_t address, uint8_t value) {
    if (!n->cart) return false;
    ExpansionAudio *a = &n->expansion;
    switch (n->cart->mapper_id) {
        case 85:
            if ((address & 0xF030) == 0x9010 || (address & 0xF030) == 0x9030) {
                vrc7_audio_write(&a->vrc7, (address & 0x20) != 0, value, nes_region_cpu_hz(n->apu.region));
                return true;
            }
            if ((address & 0xF018) == 0xE000) a->vrc7.disabled = (value & 0x40) != 0;
            break;
        case 24: case 26: {
            if (n->cart->mapper_id == 26) address = (address & 0xFFFC) | ((address & 1) << 1) | ((address & 2) >> 1);
            unsigned bank = address >> 12, reg = address & 3;
            if (bank == 9 && reg == 3) { a->vrc6.control = value; return true; }
            if (bank >= 9 && bank <= 11 && reg < 3) {
                unsigned ch = bank - 9;
                a->vrc6.regs[ch][reg] = value;
                if (reg == 2 && !(value & 0x80)) {
                    a->vrc6.phase[ch] = 0;
                    if (ch == 2) a->vrc6.accumulator = 0;
                }
                return true;
            }
            break;
        }
        case 19:
            if (address >= 0xF800) { a->n163.address = value; return true; }
            if (address >= 0x4800 && address <= 0x4FFF) {
                a->n163.ram[a->n163.address & 127] = value;
                if (a->n163.address & 128) a->n163.address = 128 | ((a->n163.address + 1) & 127);
                return true;
            }
            if (address >= 0xE000 && address <= 0xE7FF) a->n163.disabled = (value & 0x40) != 0;
            break;
        case 69:
            if (address >= 0xC000 && address < 0xE000) { a->s5b.address = value & 15; return true; }
            if (address >= 0xE000) {
                a->s5b.regs[a->s5b.address] = value;
                if (a->s5b.address == 13) {
                    a->s5b.direction = value & 4 ? 1 : -1;
                    a->s5b.envelope = value & 4 ? 0 : 15;
                    a->s5b.holding = false; a->s5b.envelope_timer = 0;
                }
                return true;
            }
            break;
        case 5:
            if (address >= 0x5000 && address <= 0x5007) {
                unsigned ch = (address >> 2) & 1, reg = address & 3;
                a->mmc5.regs[ch][reg] = value;
                if (reg == 3) {
                    if (a->mmc5.enabled & (1u << ch)) a->mmc5.length[ch] = lengths[value >> 3];
                    a->mmc5.start[ch] = true;
                }
                return true;
            }
            if (address == 0x5010) {
                a->mmc5.read_mode = value & 1; a->mmc5.irq_enabled = value & 128;
                if (!a->mmc5.irq_enabled) { a->mmc5.irq = false; cpu_set_irq_line(&n->cpu, MMC5_AUDIO_IRQ, false); }
                return true;
            }
            if (address == 0x5011) { if (!a->mmc5.read_mode && value) a->mmc5.pcm = value; return true; }
            if (address == 0x5015) {
                a->mmc5.enabled = value & 3;
                for (unsigned ch = 0; ch < 2; ++ch) if (!(value & (1u << ch))) a->mmc5.length[ch] = 0;
                return true;
            }
            break;
        case 20:
            if (address == 0x4023) { a->fds.enabled = (value & 2) != 0; return false; }
            if (!a->fds.enabled) break;
            if (address >= 0x4040 && address <= 0x407F) {
                if (a->fds.regs[9] & 128) a->fds.wave[address & 63] = value & 63;
                return true;
            }
            if (address >= 0x4080 && address <= 0x408A) {
                unsigned reg = address - 0x4080;
                a->fds.regs[reg] = value;
                if ((reg == 0 || reg == 4) && (value & 128)) a->fds.gain[reg / 4] = value & 63;
                if (reg == 5) a->fds.bias = (int16_t)((value + 64) & 127) - 64;
                if (reg == 7 && (value & 128)) a->fds.mod_phase = 0;
                if (reg == 8 && (a->fds.regs[7] & 128)) {
                    for (unsigned i = 0; i < 2; ++i) {
                        a->fds.modulation[a->fds.mod_position] = value & 7;
                        a->fds.mod_position = (a->fds.mod_position + 1) & 63;
                    }
                }
                return true;
            }
            break;
    }
    return false;
}

bool expansion_audio_read(NES *n, uint16_t address, uint8_t *value) {
    if (!n->cart) return false;
    if (n->cart->mapper_id == 19 && address >= 0x4800 && address <= 0x4FFF) {
        N163Audio *a = &n->expansion.n163;
        *value = a->ram[a->address & 127];
        if (a->address & 128) a->address = 128 | ((a->address + 1) & 127);
        return true;
    }
    if (n->cart->mapper_id == 5) {
        Mmc5Audio *a = &n->expansion.mmc5;
        if (address == 0x5010) {
            *value = (n->cpu_open_bus & 0x7F) | (a->irq ? 128 : 0);
            a->irq = false; cpu_set_irq_line(&n->cpu, MMC5_AUDIO_IRQ, false); return true;
        }
        if (address == 0x5015) { *value = (a->length[0] ? 1 : 0) | (a->length[1] ? 2 : 0); return true; }
    }
    if (n->cart->mapper_id == 20 && n->expansion.fds.enabled) {
        if (address >= 0x4040 && address <= 0x407F) { *value = (n->cpu_open_bus & 0xC0) | n->expansion.fds.wave[address & 63]; return true; }
        if (address == 0x4090 || address == 0x4092) { *value = (n->cpu_open_bus & 0xC0) | n->expansion.fds.gain[(address >> 1) & 1]; return true; }
    }
    return false;
}

void expansion_audio_observe_read(NES *n, uint16_t address, uint8_t value) {
    if (!n->cart || n->cart->mapper_id != 5 || address < 0x8000 || address > 0xBFFF) return;
    Mmc5Audio *a = &n->expansion.mmc5;
    if (!a->read_mode) return;
    if (value) a->pcm = value;
    else if (a->irq_enabled) { a->irq = true; cpu_set_irq_line(&n->cpu, MMC5_AUDIO_IRQ, true); }
}

void expansion_audio_state(ExpansionAudio *a, StateIO *io) {
    Vrc6Audio *v = &a->vrc6;
    for (unsigned ch = 0; ch < 3; ++ch) {
        state_bytes(io, v->regs[ch], 3);
        v->phase[ch] = state_u8(io, v->phase[ch]);
        v->timer[ch] = state_u16(io, v->timer[ch]);
        if (v->phase[ch] >= (ch == 2 ? 14 : 16) || v->timer[ch] > 4095) io->ok = false;
    }
    v->control = state_u8(io, v->control);
    v->accumulator = state_u8(io, v->accumulator);
    N163Audio *n = &a->n163;
    state_bytes(io, n->ram, sizeof(n->ram));
    n->address = state_u8(io, n->address);
    n->divider = state_u8(io, n->divider);
    n->channel = state_u8(io, n->channel);
    n->disabled = state_bool(io, n->disabled);
    n->output = state_i16(io, n->output);
    if (n->divider >= 15 || n->channel > 7 || n->output < -120 || n->output > 105) io->ok = false;
    S5bAudio *s = &a->s5b;
    state_bytes(io, s->regs, sizeof(s->regs));
    s->address = state_u8(io, s->address);
    s->divider = state_u8(io, s->divider);
    s->noise_divider = state_u8(io, s->noise_divider);
    for (unsigned ch = 0; ch < 3; ++ch) {
        s->tone[ch] = state_u8(io, s->tone[ch]);
        s->timer[ch] = state_u16(io, s->timer[ch]);
        if (s->tone[ch] > 1 || s->timer[ch] > 4095) io->ok = false;
    }
    s->noise_timer = state_u16(io, s->noise_timer);
    s->envelope_timer = state_u16(io, s->envelope_timer);
    s->noise = state_u32(io, s->noise);
    s->envelope = (int8_t)state_u8(io, (uint8_t)s->envelope);
    s->direction = (int8_t)state_u8(io, (uint8_t)s->direction);
    s->holding = state_bool(io, s->holding);
    if (s->address > 15 || s->divider >= 8 || s->noise_divider >= 2 || s->noise_timer > 31 ||
        s->noise > 0x1FFFF || s->envelope < 0 || s->envelope > 15 ||
        s->direction < -1 || s->direction > 1) io->ok = false;
    Mmc5Audio *m = &a->mmc5;
    for (unsigned ch = 0; ch < 2; ++ch) {
        state_bytes(io, m->regs[ch], 4);
        m->length[ch] = state_u8(io, m->length[ch]);
        m->phase[ch] = state_u8(io, m->phase[ch]);
        m->envelope[ch] = state_u8(io, m->envelope[ch]);
        m->divider[ch] = state_u8(io, m->divider[ch]);
        m->start[ch] = state_bool(io, m->start[ch]);
        m->timer[ch] = state_u16(io, m->timer[ch]);
        if (m->phase[ch] > 7 || m->envelope[ch] > 15 || m->divider[ch] > 15 || m->timer[ch] > 2047) io->ok = false;
    }
    m->enabled = state_u8(io, m->enabled);
    m->half = state_bool(io, m->half);
    m->toggle = state_bool(io, m->toggle);
    m->read_mode = state_bool(io, m->read_mode);
    m->irq_enabled = state_bool(io, m->irq_enabled);
    m->irq = state_bool(io, m->irq);
    m->frame = state_u16(io, m->frame);
    m->pcm = state_u8(io, m->pcm);
    if (m->enabled > 3 || m->frame >= 7457) io->ok = false;
    FdsAudio *f = &a->fds;
    state_bytes(io, f->wave, sizeof(f->wave));
    state_bytes(io, f->modulation, sizeof(f->modulation));
    state_bytes(io, f->regs, sizeof(f->regs));
    for (unsigned ch = 0; ch < 2; ++ch) {
        f->gain[ch] = state_u8(io, f->gain[ch]);
        f->envelope_timer[ch] = state_u32(io, f->envelope_timer[ch]);
        if (f->gain[ch] > 63 || f->envelope_timer[ch] > 8u * 64 * 255) io->ok = false;
    }
    f->position = state_u8(io, f->position);
    f->mod_position = state_u8(io, f->mod_position);
    f->phase = state_u16(io, f->phase);
    f->mod_phase = state_u16(io, f->mod_phase);
    f->bias = state_i16(io, f->bias);
    f->enabled = state_bool(io, f->enabled);
    if (f->position > 63 || f->mod_position > 63 || f->bias < -64 || f->bias > 63) io->ok = false;
    for (unsigned i = 0; i < 64; ++i)
        if (f->wave[i] > 63 || f->modulation[i] > 7) io->ok = false;
    vrc7_audio_state(&a->vrc7, io);
}
