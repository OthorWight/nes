#include "save_fixture.h"
#include "save_state.h"

static void write_mapper(NES *n, uint16_t address, uint8_t value) {
    n->cpu.cycle_count += 2; // MMC1 serial writes cannot be consecutive cycles.
    nes_cpu_bus_write(n, address, value);
}
static void setup(NES *n) {
    static const uint8_t program[] = {
        0xAD,0x16,0x40, 0x8D,0x01,0x03, 0xEE,0x00,0x03,
        0xAD,0x00,0x03, 0x8D,0x05,0x20, 0x4C,0x00,0x02
    };
    memcpy(n->wram + 0x200, program, sizeof(program));
    n->wram[0x100] = 0x40; // RTI
    n->cpu.program_counter = 0x200;
    n->ppu.ppu_ctrl = 0x08;
    n->ppu.ppu_mask = 0x1E;
    memset(n->ppu.oam_ram, 0x20, sizeof(n->ppu.oam_ram));
    for (unsigned i = 0; i < sizeof(n->ciram); ++i) n->ciram[i] = (uint8_t)(i * 7);
    n->controller_state[0] = 0xA5;
    n->controller_state[1] = 0x3C;
    nes_cpu_bus_write(n, 0x4016, 1);
    nes_cpu_bus_write(n, 0x4016, 0);
    (void)nes_cpu_bus_read(n, 0x4016);
    apu_write_reg(n, 0x4015, 0x0F);
    apu_write_reg(n, 0x4000, 0xBF);
    apu_write_reg(n, 0x4002, 0x31);
    apu_write_reg(n, 0x4003, 0x18);
    apu_write_reg(n, 0x4008, 0xFF);
    apu_write_reg(n, 0x400A, 0x28);
    apu_write_reg(n, 0x400B, 0x18);
    apu_write_reg(n, 0x400C, 0x1F);
    apu_write_reg(n, 0x400E, 0x84);
    apu_write_reg(n, 0x400F, 0x18);
    for (unsigned i = 0; i < 1800; ++i) nes_clock_tick(n);
    switch (n->cart->mapper_id) {
        case 1:
            write_mapper(n, 0x8000, 0x80);
            write_mapper(n, 0xE000, 1);
            write_mapper(n, 0xE000, 0); // Partway through a serial bank change.
            break;
        case 4: case 118: case 64: case 206:
            write_mapper(n, 0x8000, 6);
            write_mapper(n, 0x8001, 3);
            write_mapper(n, 0x8000, 0x80);
            write_mapper(n, 0x8001, 0x84);
            write_mapper(n, 0xC000, 3);
            write_mapper(n, 0xC001, 0);
            write_mapper(n, 0xE001, 0);
            break;
        case 5:
            write_mapper(n, 0x5100, 3);
            write_mapper(n, 0x5114, 0x83);
            write_mapper(n, 0x5105, 0xAA);
            write_mapper(n, 0x5C07, 0x65);
            write_mapper(n, 0x5203, 80);
            write_mapper(n, 0x5204, 0x80);
            break;
        case 9: case 10:
            write_mapper(n, 0xB000, 1);
            write_mapper(n, 0xC000, 2);
            (void)nes_ppu_bus_read(n, 0x0FD8);
            break;
        case 19:
            write_mapper(n, 0x8000, 3);
            write_mapper(n, 0xC000, 0xE1);
            write_mapper(n, 0x5000, 0x42);
            write_mapper(n, 0x5800, 0x83);
            break;
        case 23: case 24: case 26:
            write_mapper(n, 0x8000, 3);
            write_mapper(n, 0xB000, 2);
            write_mapper(n, 0xF000, 0x83);
            write_mapper(n, 0xF001, 3);
            break;
        case 34:
            write_mapper(n, 0x7FFE, 3);
            break;
        case 69:
            write_mapper(n, 0x8000, 9);
            write_mapper(n, 0xA000, 3);
            write_mapper(n, 0x8000, 14);
            write_mapper(n, 0xA000, 0x57);
            break;
        default: write_mapper(n, 0x8000, 3); break;
    }
    // Save at a visible fetch boundary with DMA/APU and controller work pending.
    for (unsigned i = 0; i < 37; ++i) nes_clock_tick(n);
    apu_write_reg(n, 0x4010, 0x8F);
    apu_write_reg(n, 0x4012, 0x10);
    apu_write_reg(n, 0x4013, 2);
    apu_write_reg(n, 0x4015, 0x1F);
    apu_write_reg(n, 0x4017, 0x80);
    nes_ppu_bus_write(n, 0x3F01, 0xC5); // Current bus backing retains upper palette bits.
    if (n->cart->chr_is_ram) n->cart->chr_rom[17] ^= 0x5A; // Restore mutable backing, not initial ROM.
    n->cart->prg_ram[n->cart->prg_ram_size - 1] = 0x8A;
}

static void equal_machine(NES *a, NES *b) {
    // Compare in-memory fields too: comparing only encoded bytes could hide a
    // field accidentally omitted by both the writer and reader.
    NES *ac = malloc(sizeof(*ac)), *bc = malloc(sizeof(*bc));
    assert(ac && bc);
    *ac = *a; *bc = *b;
    ac->cart = bc->cart = NULL;
    assert(memcmp(ac, bc, sizeof(*ac)) == 0);
    free(ac); free(bc);
    assert(a->cart->mirroring == b->cart->mirroring);
    assert(!memcmp(a->cart->prg_ram, b->cart->prg_ram, a->cart->prg_ram_size));
    assert(!memcmp(a->cart->chr_rom, b->cart->chr_rom, a->cart->chr_rom_size));
    if (a->cart->vtable->state_size)
        assert(!memcmp(a->cart->mapper_data, b->cart->mapper_data, a->cart->vtable->state_size));
    assert(a->cart->nes == a && b->cart->nes == b);
}
static void inputs_and_step(NES *n, unsigned step) {
    if (step % 97 == 0) {
        n->controller_state[0] = (uint8_t)(step * 11);
        nes_cpu_bus_write(n, 0x4016, 1);
        nes_cpu_bus_write(n, 0x4016, 0);
    }
    if (step < 3 && n->cart->mapper_id == 1) write_mapper(n, 0xE000, (uint8_t)(step & 1));
    if (n->frame_ready) { n->frame_ready = false; n->apu.audio_buffer_idx = 0; }
    nes_clock_tick(n);
}
static void all_mapper_replay(void) {
    static const unsigned ids[] = {0,1,2,3,4,5,7,9,10,11,19,23,24,26,34,64,66,69,71,78,118,206,227};
    char rom[512]; fixture_path(rom, "replay.nes");
    for (unsigned m = 0; m < sizeof(ids) / sizeof(ids[0]); ++m) {
        printf("  mapper %u: mid-frame restore and fixed-input replay\n", ids[m]); fflush(stdout);
        fixture_rom(rom, ids[m], false, ids[m] == 0 || ids[m] == 2, 0);
        NES *a = fixture_load(rom), *b = fixture_load(rom);
        setup(a);
        uint8_t *save; size_t size;
        assert(nes_state_encode(a, &save, &size) == NES_STATE_OK);
        assert(!memcmp(save, "NESSTATE\3\0\0\0", 12));
        assert(nes_state_decode(b, save, size) == NES_STATE_OK);
        equal_machine(a, b);
        for (unsigned i = 0; i < 24000; ++i) {
            inputs_and_step(a, i);
            inputs_and_step(b, i);
            if (i % 1000 == 0) equal_machine(a, b);
        }
        equal_machine(a, b);
        // Reload into an already advanced machine, not only a fresh instance.
        assert(nes_state_decode(a, save, size) == NES_STATE_OK);
        for (unsigned i = 0; i < 24000; ++i) inputs_and_step(a, i);
        equal_machine(a, b);
        free(save);
        fixture_free(a); fixture_free(b);
    }
    assert(remove(rom) == 0);
}

static void rejection_keeps_machine(NES *n, uint8_t *bad, size_t bad_size, NES_StateResult expected) {
    uint8_t *before, *after; size_t before_size, after_size;
    assert(nes_state_encode(n, &before, &before_size) == NES_STATE_OK);
    assert(nes_state_decode(n, bad, bad_size) == expected);
    assert(nes_state_encode(n, &after, &after_size) == NES_STATE_OK);
    assert(before_size == after_size && !memcmp(before, after, before_size));
    free(before); free(after);
}
static void refresh_checksum(uint8_t *data, size_t size) {
    StateIO io = {data, size, 16, false, true};
    state_u32(&io, state_crc32(data + NES_STATE_HEADER_SIZE, size - NES_STATE_HEADER_SIZE));
}
static void corrupt_and_wrong_states(void) {
    puts("  invalid states leave all live state unchanged");
    char rom[512]; fixture_path(rom, "invalid.nes");
    fixture_rom(rom, 118, false, false, 0);
    NES *n = fixture_load(rom);
    setup(n);
    uint8_t *good; size_t size;
    assert(nes_state_encode(n, &good, &size) == NES_STATE_OK);
    uint8_t *bad = malloc(size + 1); assert(bad);
    memcpy(bad, good, size);
    const size_t cuts[] = {0,1,4,8,31,32,100,1000};
    for (unsigned i = 0; i < sizeof(cuts)/sizeof(cuts[0]); ++i)
        rejection_keeps_machine(n, bad, cuts[i], NES_STATE_CORRUPT);
    rejection_keeps_machine(n, bad, size - 1, NES_STATE_CORRUPT);
    rejection_keeps_machine(n, bad, size + 1, NES_STATE_CORRUPT);
    bad[8] = 99;
    rejection_keeps_machine(n, bad, size, NES_STATE_VERSION_ERROR);
    memcpy(bad, good, size); bad[20] ^= 1;
    rejection_keeps_machine(n, bad, size, NES_STATE_WRONG_ROM);
    memcpy(bad, good, size); bad[size - 1] ^= 1;
    rejection_keeps_machine(n, bad, size, NES_STATE_CORRUPT);
    // Reject an invalid PPU skip latch even with a valid checksum.
    memcpy(bad, good, size); bad[size - 1] = 2;
    refresh_checksum(bad, size);
    rejection_keeps_machine(n, bad, size, NES_STATE_CORRUPT);
    // Valid checksum but an invalid bool in the CPU state (offset 20).
    memcpy(bad, good, size); bad[NES_STATE_HEADER_SIZE + 20] = 2;
    refresh_checksum(bad, size);
    rejection_keeps_machine(n, bad, size, NES_STATE_CORRUPT);
    // Reject an out-of-range enum even on hosts using signed enum storage.
    memcpy(bad, good, size); memset(bad + NES_STATE_HEADER_SIZE + 39, 0xFF, 4);
    refresh_checksum(bad, size);
    rejection_keeps_machine(n, bad, size, NES_STATE_CORRUPT);
    // Mapper A12 filter precedes the two IRQ booleans and PPU skip latch.
    memcpy(bad, good, size); memset(bad + size - 7, 0xFF, 4);
    refresh_checksum(bad, size);
    rejection_keeps_machine(n, bad, size, NES_STATE_CORRUPT);
    // Version 1 still carries CHR bytes, but cannot overwrite cartridge ROM.
    StateIO mapper_count = {NULL, SIZE_MAX, 0, false, true};
    n->cart->vtable->state(n->cart, &mapper_count);
    memcpy(bad, good, size);
    bad[size - mapper_count.pos - n->cart->chr_rom_size + 17] ^= 0x80;
    refresh_checksum(bad, size);
    rejection_keeps_machine(n, bad, size, NES_STATE_CORRUPT);
    rejection_keeps_machine(n, (uint8_t *)"TATS", 4, NES_STATE_LEGACY);
    // Same mapper and sizes, different ROM contents must still be rejected.
    fixture_rom(rom, 118, false, false, 1);
    NES *other = fixture_load(rom);
    rejection_keeps_machine(other, good, size, NES_STATE_WRONG_ROM);
    fixture_free(other);
    free(good); free(bad); fixture_free(n);
    assert(remove(rom) == 0);
}

static void state_files_and_failed_replace(void) {
    puts("  state file replacement, reload, and failed writes");
    char rom[512], path[512], missing[512], directory[512], sentinel[512];
    fixture_path(rom, "file.nes"); fixture_path(path, "quick.state");
    fixture_path(missing, "absent/state"); fixture_path(directory, "occupied.state");
    fixture_path(sentinel, "occupied.state/progress");
    fixture_rom(rom, 4, false, false, 0);
    NES *n = fixture_load(rom), *other = fixture_load(rom);
    setup(n);
    assert(nes_state_save(n, path) == NES_STATE_OK);
    assert(nes_state_load(other, path) == NES_STATE_OK);
    equal_machine(n, other);
    inputs_and_step(n, 0);
    assert(nes_state_save(n, path) == NES_STATE_OK);
    assert(nes_state_load(other, path) == NES_STATE_OK);
    equal_machine(n, other);
    assert(nes_state_save(n, missing) == NES_STATE_IO);
    assert(nes_state_load(n, missing) == NES_STATE_OPEN);
    assert(SAVE_MKDIR(directory) == 0);
    assert(state_atomic_write(sentinel, "progress", 8));
    assert(nes_state_save(n, directory) == NES_STATE_IO);
    FILE *f = fopen(sentinel, "rb"); assert(f);
    char bytes[8]; assert(fread(bytes, 1, 8, f) == 8 && !memcmp(bytes, "progress", 8));
    assert(fclose(f) == 0);
    fixture_free(n); fixture_free(other);
    assert(remove(path) == 0); assert(remove(rom) == 0);
    assert(remove(sentinel) == 0); assert(SAVE_RMDIR(directory) == 0);
}

static void irq_poll_state_and_older_import(void) {
    puts("  IRQ poll state survives restore; versions 1 and 2 still load");
    char rom[512]; fixture_path(rom, "irq-state.nes");
    fixture_rom(rom, 0, false, true, 0);
    NES *n = fixture_load(rom);
    n->cpu.status_flags = FLAG_UNUSED | FLAG_INTERRUPT_DISABLE;
    n->cpu.irq_pending = true; n->cpu.irq_poll_valid = true;
    uint8_t *save; size_t size;
    assert(nes_state_encode(n, &save, &size) == NES_STATE_OK);
    n->cpu.irq_pending = false; n->cpu.irq_poll_valid = false;
    assert(nes_state_decode(n, save, size) == NES_STATE_OK);
    assert(n->cpu.irq_pending && n->cpu.irq_poll_valid && !n->cpu.irq_lines);
    uint64_t before = n->cpu.cycle_count;
    nes_clock_tick(n);
    assert(n->cpu.cycle_count - before == 7 && n->cpu.program_counter == 0x0100);

    // Version 3 appends the PPU skip latch; version 2 appends IRQ booleans.
    // Recreate version 2 without letting omitted data inherit live history.
    size -= 1;
    StateIO header = {save, size, 8, false, true};
    state_u32(&header, 2);
    state_u32(&header, (uint32_t)(size - NES_STATE_HEADER_SIZE));
    refresh_checksum(save, size);
    n->ppu.odd_skip_rendering = true;
    assert(nes_state_decode(n, save, size) == NES_STATE_OK);
    assert(!n->ppu.odd_skip_rendering); // Saved PPUMASK is zero.
    assert(n->cpu.irq_pending && n->cpu.irq_poll_valid);

    // Recreate the version 1 layout
    // and header rather than letting omitted data inherit from the live CPU.
    size -= 2;
    header = (StateIO){save, size, 8, false, true};
    state_u32(&header, 1);
    state_u32(&header, (uint32_t)(size - NES_STATE_HEADER_SIZE));
    refresh_checksum(save, size);
    n->cpu.irq_pending = true; n->cpu.irq_poll_valid = true;
    assert(nes_state_decode(n, save, size) == NES_STATE_OK);
    assert(!n->cpu.irq_pending && !n->cpu.irq_poll_valid);
    free(save); fixture_free(n); assert(remove(rom) == 0);
}

static void odd_frame_skip_latch_survives_restore(void) {
    puts("  late rendering toggle retains the skip decision across save/load");
    char rom[512]; fixture_path(rom, "ppu-skip-state.nes");
    fixture_rom(rom, 0, false, true, 0);
    NES *n = fixture_load(rom);
    for (unsigned enabled = 0; enabled < 2; ++enabled) {
        n->ppu.scanline = 261; n->ppu.cycle = 338; n->ppu.odd_frame = true;
        nes_cpu_bus_write(n, 0x2001, enabled ? 0x08 : 0);
        ppu_step(n); // Sample the skip circuit's rendering enable.
        nes_cpu_bus_write(n, 0x2001, enabled ? 0 : 0x08);
        uint8_t *save; size_t size;
        assert(nes_state_encode(n, &save, &size) == NES_STATE_OK);
        n->ppu.odd_skip_rendering = !enabled;
        assert(nes_state_decode(n, save, size) == NES_STATE_OK);
        assert(n->ppu.odd_skip_rendering == !!enabled);
        ppu_step(n);
        assert(n->ppu.scanline == (enabled ? 0 : 261));
        assert(n->ppu.cycle == (enabled ? 0 : 340));
        free(save);
    }
    fixture_free(n); assert(remove(rom) == 0);
}

int main(void) {
    fixture_start("states");
    all_mapper_replay();
    corrupt_and_wrong_states();
    state_files_and_failed_replace();
    irq_poll_state_and_older_import();
    odd_frame_skip_latch_survives_restore();
    assert(SAVE_RMDIR(fixture_dir) == 0);
    puts("Save-state replay, validation and file I/O checks passed.");
    return 0;
}
