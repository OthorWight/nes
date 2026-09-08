#include "save_state.h"
#include "state_io.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint8_t state_magic[8] = {'N','E','S','S','T','A','T','E'};

/* Version 1 field order. Changing this schema requires a version change.
   All integers are fixed-width little-endian; padding and pointers are omitted. */
static void machine_fields(NES *n, StateIO *io) {
    n->cpu.accumulator = state_u8(io, n->cpu.accumulator);
    n->cpu.index_x = state_u8(io, n->cpu.index_x);
    n->cpu.index_y = state_u8(io, n->cpu.index_y);
    n->cpu.stack_pointer = state_u8(io, n->cpu.stack_pointer);
    n->cpu.program_counter = state_u16(io, n->cpu.program_counter);
    n->cpu.status_flags = state_u8(io, n->cpu.status_flags);
    n->cpu.cycle_count = state_u64(io, n->cpu.cycle_count);
    n->cpu.stall_cycles = state_u32(io, n->cpu.stall_cycles);
    n->cpu.irq_lines = state_u8(io, n->cpu.irq_lines);
    n->cpu.nmi_line = state_bool(io, n->cpu.nmi_line);
    n->cpu.nmi_prev_line = state_bool(io, n->cpu.nmi_prev_line);
    n->cpu.nmi_edge = state_bool(io, n->cpu.nmi_edge);
    n->cpu.reset_pending = state_bool(io, n->cpu.reset_pending);
    n->cpu.rdy = state_bool(io, n->cpu.rdy);
    n->cpu.open_bus = state_u8(io, n->cpu.open_bus);
    n->cpu.nmi_active_count = state_i32(io, n->cpu.nmi_active_count);
    n->cpu.nmi_pulsed_cycle = state_u64(io, n->cpu.nmi_pulsed_cycle);
    n->cpu.nmi_delayed = state_bool(io, n->cpu.nmi_delayed);
    n->cpu.model = state_enum(io, n->cpu.model);
    state_bytes(io, n->ppu.oam_eval.secondary, sizeof(n->ppu.oam_eval.secondary));
    n->ppu.oam_eval.bus = state_u8(io, n->ppu.oam_eval.bus);
    n->ppu.oam_eval.n = state_u8(io, n->ppu.oam_eval.n);
    n->ppu.oam_eval.m = state_u8(io, n->ppu.oam_eval.m);
    n->ppu.oam_eval.secondary_index = state_u8(io, n->ppu.oam_eval.secondary_index);
    n->ppu.oam_eval.done = state_bool(io, n->ppu.oam_eval.done);
    state_bytes(io, n->ppu.palette_ram, sizeof(n->ppu.palette_ram));
    state_bytes(io, n->ppu.oam_ram, sizeof(n->ppu.oam_ram));
    n->ppu.v = state_u16(io, n->ppu.v);
    n->ppu.t = state_u16(io, n->ppu.t);
    n->ppu.x = state_u8(io, n->ppu.x);
    n->ppu.w = state_u8(io, n->ppu.w);
    n->ppu.ppu_ctrl = state_u8(io, n->ppu.ppu_ctrl);
    n->ppu.ppu_mask = state_u8(io, n->ppu.ppu_mask);
    n->ppu.ppu_status = state_u8(io, n->ppu.ppu_status);
    n->ppu.oam_addr = state_u8(io, n->ppu.oam_addr);
    n->ppu.buffered_data = state_u8(io, n->ppu.buffered_data);
    n->ppu.bus_address = state_u16(io, n->ppu.bus_address);
    n->ppu.scanline = state_i32(io, n->ppu.scanline);
    n->ppu.cycle = state_i32(io, n->ppu.cycle);
    n->ppu.nmi_occurred = state_bool(io, n->ppu.nmi_occurred);
    n->ppu.frame_complete = state_bool(io, n->ppu.frame_complete);
    n->ppu.nmi_suppressed = state_bool(io, n->ppu.nmi_suppressed);
    n->ppu.bg_shifter_pattern_low = state_u16(io, n->ppu.bg_shifter_pattern_low);
    n->ppu.bg_shifter_pattern_high = state_u16(io, n->ppu.bg_shifter_pattern_high);
    n->ppu.bg_shifter_attrib_low = state_u16(io, n->ppu.bg_shifter_attrib_low);
    n->ppu.bg_shifter_attrib_high = state_u16(io, n->ppu.bg_shifter_attrib_high);
    n->ppu.bg_next_tile_id = state_u8(io, n->ppu.bg_next_tile_id);
    n->ppu.bg_next_tile_attrib = state_u8(io, n->ppu.bg_next_tile_attrib);
    n->ppu.bg_next_tile_lsb = state_u8(io, n->ppu.bg_next_tile_lsb);
    n->ppu.bg_next_tile_msb = state_u8(io, n->ppu.bg_next_tile_msb);
    n->ppu.odd_frame = state_bool(io, n->ppu.odd_frame);
    n->ppu.bg_palette_index = state_u8(io, n->ppu.bg_palette_index);
    for (size_t i = 0; i < 8; ++i) {
        n->ppu.scanline_sprites[i].x = state_u8(io, n->ppu.scanline_sprites[i].x);
        n->ppu.scanline_sprites[i].low_byte = state_u8(io, n->ppu.scanline_sprites[i].low_byte);
        n->ppu.scanline_sprites[i].high_byte = state_u8(io, n->ppu.scanline_sprites[i].high_byte);
        n->ppu.scanline_sprites[i].attributes = state_u8(io, n->ppu.scanline_sprites[i].attributes);
        n->ppu.scanline_sprites[i].sprite_index = state_u8(io, n->ppu.scanline_sprites[i].sprite_index);
    }
    n->ppu.scanline_sprite_count = state_i32(io, n->ppu.scanline_sprite_count);
    n->ppu.overflow_cycle = state_i32(io, n->ppu.overflow_cycle);
    n->ppu.open_bus_value = state_u8(io, n->ppu.open_bus_value);
    for (size_t i = 0; i < 8; ++i) n->ppu.open_bus_decay_cycles[i] = state_u64(io, n->ppu.open_bus_decay_cycles[i]);
    for (size_t i = 0; i < 256 * 240; ++i) n->ppu.screen_buffer[i] = state_u32(io, n->ppu.screen_buffer[i]);
    for (size_t i = 0; i < 2; ++i) n->apu.pulse_enabled[i] = state_bool(io, n->apu.pulse_enabled[i]);
    state_bytes(io, n->apu.pulse_duty, sizeof(n->apu.pulse_duty));
    for (size_t i = 0; i < 2; ++i) n->apu.pulse_halt[i] = state_bool(io, n->apu.pulse_halt[i]);
    for (size_t i = 0; i < 2; ++i) n->apu.pulse_constant_volume[i] = state_bool(io, n->apu.pulse_constant_volume[i]);
    state_bytes(io, n->apu.pulse_volume, sizeof(n->apu.pulse_volume));
    for (size_t i = 0; i < 2; ++i) n->apu.pulse_timer[i] = state_u16(io, n->apu.pulse_timer[i]);
    for (size_t i = 0; i < 2; ++i) n->apu.pulse_timer_reload[i] = state_u16(io, n->apu.pulse_timer_reload[i]);
    state_bytes(io, n->apu.pulse_length_counter, sizeof(n->apu.pulse_length_counter));
    state_bytes(io, n->apu.pulse_sequence_idx, sizeof(n->apu.pulse_sequence_idx));
    state_bytes(io, n->apu.pulse_envelope_decay, sizeof(n->apu.pulse_envelope_decay));
    state_bytes(io, n->apu.pulse_envelope_divider, sizeof(n->apu.pulse_envelope_divider));
    for (size_t i = 0; i < 2; ++i) n->apu.pulse_envelope_start[i] = state_bool(io, n->apu.pulse_envelope_start[i]);
    for (size_t i = 0; i < 2; ++i) n->apu.pulse_sweep_enabled[i] = state_bool(io, n->apu.pulse_sweep_enabled[i]);
    state_bytes(io, n->apu.pulse_sweep_period, sizeof(n->apu.pulse_sweep_period));
    for (size_t i = 0; i < 2; ++i) n->apu.pulse_sweep_negate[i] = state_bool(io, n->apu.pulse_sweep_negate[i]);
    state_bytes(io, n->apu.pulse_sweep_shift, sizeof(n->apu.pulse_sweep_shift));
    state_bytes(io, n->apu.pulse_sweep_divider, sizeof(n->apu.pulse_sweep_divider));
    for (size_t i = 0; i < 2; ++i) n->apu.pulse_sweep_reload[i] = state_bool(io, n->apu.pulse_sweep_reload[i]);
    n->apu.triangle_enabled = state_bool(io, n->apu.triangle_enabled);
    n->apu.triangle_control_flag = state_bool(io, n->apu.triangle_control_flag);
    n->apu.triangle_linear_reload = state_u8(io, n->apu.triangle_linear_reload);
    n->apu.triangle_linear_counter = state_u8(io, n->apu.triangle_linear_counter);
    n->apu.triangle_linear_reload_flag = state_bool(io, n->apu.triangle_linear_reload_flag);
    n->apu.triangle_timer = state_u16(io, n->apu.triangle_timer);
    n->apu.triangle_timer_reload = state_u16(io, n->apu.triangle_timer_reload);
    n->apu.triangle_length_counter = state_u8(io, n->apu.triangle_length_counter);
    n->apu.triangle_sequence_idx = state_u8(io, n->apu.triangle_sequence_idx);
    n->apu.noise_enabled = state_bool(io, n->apu.noise_enabled);
    n->apu.noise_halt = state_bool(io, n->apu.noise_halt);
    n->apu.noise_constant_volume = state_bool(io, n->apu.noise_constant_volume);
    n->apu.noise_volume = state_u8(io, n->apu.noise_volume);
    n->apu.noise_timer = state_u16(io, n->apu.noise_timer);
    n->apu.noise_timer_reload = state_u16(io, n->apu.noise_timer_reload);
    n->apu.noise_length_counter = state_u8(io, n->apu.noise_length_counter);
    n->apu.noise_shift_reg = state_u16(io, n->apu.noise_shift_reg);
    n->apu.noise_mode = state_bool(io, n->apu.noise_mode);
    n->apu.noise_envelope_decay = state_u8(io, n->apu.noise_envelope_decay);
    n->apu.noise_envelope_divider = state_u8(io, n->apu.noise_envelope_divider);
    n->apu.noise_envelope_start = state_bool(io, n->apu.noise_envelope_start);
    n->apu.dmc_enabled = state_bool(io, n->apu.dmc_enabled);
    n->apu.dmc_irq_enable = state_bool(io, n->apu.dmc_irq_enable);
    n->apu.dmc_loop = state_bool(io, n->apu.dmc_loop);
    n->apu.dmc_irq_active = state_bool(io, n->apu.dmc_irq_active);
    n->apu.dmc_rate = state_u8(io, n->apu.dmc_rate);
    n->apu.dmc_timer = state_u16(io, n->apu.dmc_timer);
    n->apu.dmc_timer_reload = state_u16(io, n->apu.dmc_timer_reload);
    n->apu.dmc_value = state_u8(io, n->apu.dmc_value);
    n->apu.dmc_sample_addr = state_u16(io, n->apu.dmc_sample_addr);
    n->apu.dmc_current_addr = state_u16(io, n->apu.dmc_current_addr);
    n->apu.dmc_sample_len = state_u16(io, n->apu.dmc_sample_len);
    n->apu.dmc_bytes_remaining = state_u16(io, n->apu.dmc_bytes_remaining);
    n->apu.dmc_shift_reg = state_u8(io, n->apu.dmc_shift_reg);
    n->apu.dmc_bits_remaining = state_u8(io, n->apu.dmc_bits_remaining);
    n->apu.dmc_buffer = state_u8(io, n->apu.dmc_buffer);
    n->apu.dmc_buffer_empty = state_bool(io, n->apu.dmc_buffer_empty);
    n->apu.dmc_silent = state_bool(io, n->apu.dmc_silent);
    n->apu.frame_mode = state_bool(io, n->apu.frame_mode);
    n->apu.frame_irq_inhibit = state_bool(io, n->apu.frame_irq_inhibit);
    n->apu.frame_irq_active = state_bool(io, n->apu.frame_irq_active);
    n->apu.frame_cycles = state_u32(io, n->apu.frame_cycles);
    n->apu.frame_counter_reset_pending = state_bool(io, n->apu.frame_counter_reset_pending);
    n->apu.frame_counter_reset_delay = state_u8(io, n->apu.frame_counter_reset_delay);
    n->apu.audio_accumulator = state_f64(io, n->apu.audio_accumulator);
    for (size_t i = 0; i < 4096; ++i) n->apu.audio_buffer[i] = state_f32(io, n->apu.audio_buffer[i]);
    n->apu.audio_buffer_idx = state_u32(io, n->apu.audio_buffer_idx);
    n->apu.clock_toggle = state_bool(io, n->apu.clock_toggle);
    n->clock.master_ticks = state_u64(io, n->clock.master_ticks);
    n->clock.cpu_divider = state_u32(io, n->clock.cpu_divider);
    n->clock.ppu_divider = state_u32(io, n->clock.ppu_divider);
    n->lines.irq_line = state_bool(io, n->lines.irq_line);
    n->lines.nmi_line = state_bool(io, n->lines.nmi_line);
    n->lines.reset_line = state_bool(io, n->lines.reset_line);
    n->lines.rw_line = state_bool(io, n->lines.rw_line);
    state_bytes(io, n->wram, sizeof(n->wram));
    state_bytes(io, n->ciram, sizeof(n->ciram));
    n->cpu_open_bus = state_u8(io, n->cpu_open_bus);
    state_bytes(io, n->controller_state, sizeof(n->controller_state));
    state_bytes(io, n->controller_shift, sizeof(n->controller_shift));
    n->controller_strobe = state_u8(io, n->controller_strobe);
    n->zapper_enabled = state_bool(io, n->zapper_enabled);
    n->zapper_trigger = state_bool(io, n->zapper_trigger);
    n->zapper_light = state_bool(io, n->zapper_light);
    n->zapper_x = state_i32(io, n->zapper_x);
    n->zapper_y = state_i32(io, n->zapper_y);
    n->zapper_watchdog.reads = state_u16(io, n->zapper_watchdog.reads);
    n->zapper_watchdog.poll_pc = state_u16(io, n->zapper_watchdog.poll_pc);
    n->zapper_watchdog.stalled_frames = state_u16(io, n->zapper_watchdog.stalled_frames);
    n->zapper_watchdog.mixed_pcs = state_bool(io, n->zapper_watchdog.mixed_pcs);
    n->zapper_watchdog.stalled = state_bool(io, n->zapper_watchdog.stalled);
    n->frame_ready = state_bool(io, n->frame_ready);
}

static bool valid_machine(const NES *n) {
    const PPU2C02 *p = &n->ppu;
    const APU2A03 *a = &n->apu;
    if ((unsigned)n->cpu.model > CPU_MODEL_CMOS_65C02 || n->controller_strobe > 1 ||
        p->scanline < 0 || p->scanline > 261 || p->cycle < 0 || p->cycle > 340 ||
        p->x > 7 || p->w > 1 || p->v > 0x7FFF || p->t > 0x7FFF ||
        p->bus_address > 0x3FFF || p->scanline_sprite_count < 0 || p->scanline_sprite_count > 8 ||
        p->overflow_cycle < -1 || p->overflow_cycle > 340 ||
        p->oam_eval.n > 64 || p->oam_eval.m > 3 || p->oam_eval.secondary_index > 32 ||
        p->bg_palette_index > 31 || p->bg_next_tile_attrib > 3 ||
        a->triangle_sequence_idx > 31 || a->dmc_bits_remaining > 8 || a->dmc_rate > 15 ||
        a->dmc_value > 127 || a->frame_counter_reset_delay > 4 ||
        a->audio_buffer_idx > 4096 || a->audio_accumulator < 0 || a->audio_accumulator >= 1 ||
        (unsigned)n->cart->mirroring > MIRROR_ONE_SCREEN_HIGH) return false;
    for (unsigned i = 0; i < 2; ++i) {
        if (a->pulse_duty[i] > 3 || a->pulse_sequence_idx[i] > 7 || a->pulse_sweep_shift[i] > 7)
            return false;
    }
    return true;
}

static void payload(NES *n, StateIO *io, unsigned version) {
    Cartridge *c = n->cart;
    machine_fields(n, io);
    c->mirroring = state_enum(io, c->mirroring);
    uint32_t mapper = state_u32(io, c->mapper_id);
    uint32_t prg = state_u32(io, c->prg_rom_size);
    uint32_t chr = state_u32(io, c->chr_rom_size);
    uint32_t ram = state_u32(io, c->prg_ram_size);
    if (mapper != c->mapper_id || prg != c->prg_rom_size || chr != c->chr_rom_size || ram != c->prg_ram_size) {
        io->ok = false;
        return;
    }
    state_bytes(io, c->prg_ram, c->prg_ram_size);
    // Keep the version 1 layout. ROM bytes are verified on load, never restored
    // over the cartridge ROM; only CHR RAM is mutable.
    state_bytes(io, c->chr_rom, c->chr_rom_size);
    c->vtable->state(c, io);
    if (version >= 2) {
        n->cpu.irq_pending = state_bool(io, n->cpu.irq_pending);
        n->cpu.irq_poll_valid = state_bool(io, n->cpu.irq_poll_valid);
    } else if (io->reading) {
        // Version 1 used live IRQ levels at instruction boundaries and did
        // not preserve a poll result. Bootstrap once with that old behavior.
        n->cpu.irq_pending = false;
        n->cpu.irq_poll_valid = false;
    }
    if (!valid_machine(n)) io->ok = false;
}

static NES_StateResult available(NES *n) {
    if (!n || !n->cart) return NES_STATE_NO_CART;
    Cartridge *c = n->cart;
    if (!c->vtable || !c->vtable->state || (c->vtable->state_size && !c->mapper_data)) return NES_STATE_MAPPER;
    if (!c->prg_rom || !c->prg_rom_size || (c->chr_rom_size && !c->chr_rom) ||
        (c->prg_ram_size && !c->prg_ram)) return NES_STATE_CORRUPT;
    return NES_STATE_OK;
}

NES_StateResult nes_state_encode(NES *n, uint8_t **data, size_t *size) {
    *data = NULL;
    *size = 0;
    NES_StateResult result = available(n);
    if (result != NES_STATE_OK) return result;
    StateIO count = {NULL, NES_STATE_MAX_SIZE - NES_STATE_HEADER_SIZE, 0, false, true};
    payload(n, &count, NES_STATE_VERSION);
    if (!count.ok) return NES_STATE_CORRUPT;
    size_t total = NES_STATE_HEADER_SIZE + count.pos;
    uint8_t *bytes = malloc(total);
    if (!bytes) return NES_STATE_MEMORY;
    StateIO out = {bytes, total, NES_STATE_HEADER_SIZE, false, true};
    payload(n, &out, NES_STATE_VERSION);
    if (!out.ok || out.pos != total) { free(bytes); return NES_STATE_CORRUPT; }
    StateIO header = {bytes, NES_STATE_HEADER_SIZE, 0, false, true};
    memcpy(bytes, state_magic, 8);
    header.pos = 8;
    state_u32(&header, NES_STATE_VERSION);
    state_u32(&header, (uint32_t)count.pos);
    state_u32(&header, state_crc32(bytes + NES_STATE_HEADER_SIZE, count.pos));
    for (unsigned i = 0; i < 3; ++i) state_u32(&header, n->cart->rom_identity[i]);
    *data = bytes;
    *size = total;
    return NES_STATE_OK;
}

NES_StateResult nes_state_decode(NES *n, const uint8_t *data, size_t size) {
    NES_StateResult result = available(n);
    if (result != NES_STATE_OK) return result;
    if (!data) return NES_STATE_CORRUPT;
    if (size >= 4 && (!memcmp(data, "TATS", 4) || !memcmp(data, "STAT", 4))) return NES_STATE_LEGACY;
    if (size < NES_STATE_HEADER_SIZE || size > NES_STATE_MAX_SIZE || memcmp(data, state_magic, 8)) return NES_STATE_CORRUPT;
    StateIO in = {(uint8_t *)data, size, 8, true, true};
    unsigned version = state_u32(&in, 0);
    if (version != 1 && version != NES_STATE_VERSION) return NES_STATE_VERSION_ERROR;
    uint32_t length = state_u32(&in, 0);
    uint32_t checksum = state_u32(&in, 0);
    if (length != size - NES_STATE_HEADER_SIZE || checksum != state_crc32(data + NES_STATE_HEADER_SIZE, length))
        return NES_STATE_CORRUPT;
    for (unsigned i = 0; i < 3; ++i)
        if (state_u32(&in, 0) != n->cart->rom_identity[i]) return NES_STATE_WRONG_ROM;

    // Stage all mutable memory and mapper registers. Callbacks and the ROM
    // allocation remain owned by the live cartridge; decoding never runs them.
    NES *staged = malloc(sizeof(*staged));
    Cartridge c = *n->cart;
    void *mapper = calloc(1, c.vtable->state_size ? c.vtable->state_size : 1);
    uint8_t *ram = malloc(c.prg_ram_size ? c.prg_ram_size : 1);
    uint8_t *chr = malloc(c.chr_rom_size ? c.chr_rom_size : 1);
    if (!staged || !mapper || !ram || !chr) result = NES_STATE_MEMORY;
    else {
        *staged = *n;
        c.mapper_data = mapper;
        c.prg_ram = ram;
        c.chr_rom = chr;
        c.nes = staged;
        staged->cart = &c;
        payload(staged, &in, version);
        if (!in.ok || in.pos != size ||
            (!c.chr_is_ram && memcmp(chr, n->cart->chr_rom, c.chr_rom_size))) result = NES_STATE_CORRUPT;
        else {
            Cartridge *live = n->cart;
            if (c.prg_ram_size) memcpy(live->prg_ram, ram, c.prg_ram_size);
            if (c.chr_is_ram && c.chr_rom_size) memcpy(live->chr_rom, chr, c.chr_rom_size);
            if (c.vtable->state_size) memcpy(live->mapper_data, mapper, c.vtable->state_size);
            live->mirroring = c.mirroring;
            staged->cart = live;
            *n = *staged;
            live->nes = n;
            result = NES_STATE_OK;
        }
    }
    free(staged);
    free(mapper);
    free(ram);
    free(chr);
    return result;
}

NES_StateResult nes_state_save(NES *n, const char *path) {
    uint8_t *data;
    size_t size;
    NES_StateResult result = nes_state_encode(n, &data, &size);
    if (result != NES_STATE_OK) return result;
    if (!state_atomic_write(path, data, size)) result = NES_STATE_IO;
    free(data);
    return result;
}

NES_StateResult nes_state_load(NES *n, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NES_STATE_OPEN;
    NES_StateResult result = NES_STATE_CORRUPT;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NES_STATE_IO; }
    long len = ftell(f);
    if (len < 0 || (unsigned long)len > NES_STATE_MAX_SIZE || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NES_STATE_CORRUPT;
    }
    uint8_t *data = malloc(len ? (size_t)len : 1);
    if (!data) { fclose(f); return NES_STATE_MEMORY; }
    bool ok = fread(data, 1, (size_t)len, f) == (size_t)len && fgetc(f) == EOF && !ferror(f);
    if (fclose(f) != 0) ok = false;
    if (ok) result = nes_state_decode(n, data, (size_t)len);
    else result = NES_STATE_IO;
    free(data);
    return result;
}

const char *nes_state_message(NES_StateResult result) {
    switch (result) {
        case NES_STATE_OK: return "OK";
        case NES_STATE_NO_CART: return "NO GAME LOADED";
        case NES_STATE_MAPPER: return "MAPPER STATE UNSUPPORTED";
        case NES_STATE_OPEN: return "CANNOT OPEN STATE";
        case NES_STATE_IO: return "STATE FILE I/O FAILED";
        case NES_STATE_VERSION_ERROR: return "UNSUPPORTED STATE VERSION";
        case NES_STATE_LEGACY: return "OLD STATE: CREATE A NEW SAVE";
        case NES_STATE_WRONG_ROM: return "STATE IS FOR A DIFFERENT ROM";
        case NES_STATE_MEMORY: return "NOT ENOUGH MEMORY FOR STATE";
        default: return "INVALID OR DAMAGED STATE";
    }
}
