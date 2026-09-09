#include "save_fixture.h"
#include "save_state.h"

static void drive(NES *n, uint8_t value) { nes_cpu_bus_write(n, 0, value); }
static void serial(NES *n, uint16_t address, uint8_t value) {
    for (unsigned i = 0; i < 5; ++i) {
        n->cpu.cycle_count += 2;
        nes_cpu_bus_write(n, address, (value >> i) & 1);
    }
}
static void open_bus_and_partial_reads(void) {
    puts("  CPU reads/writes, controller bits, APU status and DMA drive open bus");
    NES *n = malloc(sizeof(*n)); assert(n); nes_init(n);
    drive(n, 0xA6);
    assert(nes_cpu_bus_read(n, 0x5000) == 0xA6);
    assert(nes_cpu_bus_read(n, 0x4000) == 0xA6);
    assert(nes_cpu_bus_read(n, 0x4018) == 0xA6);
    n->wram[3] = 0x79; assert(nes_cpu_bus_read(n, 3) == 0x79);
    assert(nes_cpu_bus_read(n, 0x6000) == 0x79);
    n->controller_state[0] = 1;
    nes_cpu_bus_write(n, 0x4016, 1); drive(n, 0xE0);
    assert(nes_cpu_bus_read(n, 0x4016) == 0xE1);
    n->controller_state[0] = 0xA6; // Changes while latch high must survive falling edge.
    nes_cpu_bus_write(n, 0x4016, 0);
    for (unsigned i = 0; i < 8; ++i) {
        drive(n, 0xA0);
        assert(nes_cpu_bus_read(n, 0x4016) == (0xA0 | ((0xA6 >> i) & 1)));
    }
    n->apu.frame_irq_active = true; drive(n, 0x20);
    assert(nes_cpu_bus_read(n, 0x4015) == 0x60);
    assert(n->cpu_open_bus == 0x20 && nes_cpu_bus_read(n, 0x5000) == 0x20);
    assert(!n->apu.frame_irq_active);
    assert(nes_cpu_bus_read(n, 0x4015) == 0x20);
    n->wram[0x2FF] = 0xB9;
    nes_cpu_bus_write(n, 0x4014, 2);
    assert(nes_cpu_bus_read(n, 0x5000) == 0xB9);
    // Indexed absolute open bus sees the operand's high byte, not effective address.
    const uint8_t code[] = {0xBD, 0xFF, 0x50};
    memcpy(n->wram + 0x100, code, sizeof(code));
    n->cpu.program_counter = 0x100; n->cpu.index_x = 1;
    nes_clock_tick(n); assert(n->cpu.accumulator == 0x50);
    free(n);
}
static void all_mapper_rom_ram_boundaries(void) {
    puts("  all mapper IDs protect ROM, write CHR RAM and float absent PRG RAM");
    static const unsigned ids[] = {0,1,2,3,4,5,7,9,10,11,19,23,24,26,34,64,66,69,71,78,118,206,227};
    char path[512]; fixture_path(path, "banks.nes");
    for (unsigned j = 0; j < sizeof(ids)/sizeof(ids[0]); ++j) {
        fixture_rom(path, ids[j], false, false, 0);
        NES *n = fixture_load(path);
        uint32_t chr_crc = state_crc32(n->cart->chr_rom, n->cart->chr_rom_size);
        uint32_t prg_crc = state_crc32(n->cart->prg_rom, n->cart->prg_rom_size);
        for (unsigned addr = 0; addr < 8192; addr += 37) {
            nes_ppu_bus_write(n, (uint16_t)addr, 0xEF);
            n->cart->vtable->ppu_write(n->cart, (uint16_t)addr, 0x45);
        }
        assert(state_crc32(n->cart->chr_rom, n->cart->chr_rom_size) == chr_crc);
        // Extreme register values must still resolve to a populated bank.
        n->cpu.cycle_count = 100;
        for (unsigned addr = 0x8000; addr < 0x10000; addr += 0x1000) {
            nes_cpu_bus_write(n, (uint16_t)addr, 0xFF);
            (void)nes_cpu_bus_read(n, (uint16_t)addr);
            (void)nes_ppu_bus_read(n, (uint16_t)(addr & 0x1FFF));
        }
        assert(state_crc32(n->cart->prg_rom, n->cart->prg_rom_size) == prg_crc);
        free(n->cart->prg_ram); n->cart->prg_ram = NULL; n->cart->prg_ram_size = 0;
        if (ids[j] == 69) { nes_cpu_bus_write(n, 0x8000, 8); nes_cpu_bus_write(n, 0xA000, 0xC0); }
        drive(n, 0xD2); assert(nes_cpu_bus_read(n, 0x7FFF) == 0xD2);
        nes_cpu_bus_write(n, 0x7FFF, 0x18);
        fixture_free(n);
        fixture_rom(path, ids[j], false, true, 0); n = fixture_load(path);
        nes_ppu_bus_write(n, 0x0037, 0x59);
        assert(nes_ppu_bus_read(n, 0x0037) == 0x59);
        fixture_free(n);
    }
    assert(remove(path) == 0);
}
static void mmc1_and_mmc3_protection(void) {
    puts("  MMC1 serial reset/RAM disable, MMC3 RAM protection and bank modes");
    char path[512]; fixture_path(path, "mmc.nes");
    fixture_rom(path, 1, false, true, 0); NES *n = fixture_load(path);
    serial(n, 0xE000, 0x10);
    nes_cpu_bus_write(n, 0x6000, 0x5A); assert(n->cart->prg_ram[0] == 0);
    drive(n, 0xB6); assert(nes_cpu_bus_read(n, 0x6000) == 0xB6);
    serial(n, 0xE000, 0); nes_cpu_bus_write(n, 0x6000, 0x37);
    assert(nes_cpu_bus_read(n, 0x6000) == 0x37);
    n->cpu.cycle_count += 2; nes_cpu_bus_write(n, 0x8000, 0x80);
    bool ce; assert(n->cart->vtable->remap_ciram_addr(n->cart, 0x2C00, &ce) == 0);
    fixture_free(n);
    const unsigned ids[] = {4,118};
    for (unsigned j = 0; j < 2; ++j) {
        fixture_rom(path, ids[j], false, true, 0); n = fixture_load(path);
        nes_cpu_bus_write(n, 0x6000, 0x43);
        nes_cpu_bus_write(n, 0xA001, 0xC0); nes_cpu_bus_write(n, 0x6000, 0x88);
        assert(nes_cpu_bus_read(n, 0x6000) == 0x43);
        nes_cpu_bus_write(n, 0xA001, 0); drive(n, 0xB7);
        assert(nes_cpu_bus_read(n, 0x6000) == 0xB7);
        nes_cpu_bus_write(n, 0x8000, 6); nes_cpu_bus_write(n, 0x8001, 3);
        assert(nes_cpu_bus_read(n, 0x8000) == 3 && nes_cpu_bus_read(n, 0xC000) == 14);
        nes_cpu_bus_write(n, 0x8000, 0x46);
        assert(nes_cpu_bus_read(n, 0x8000) == 14 && nes_cpu_bus_read(n, 0xC000) == 3);
        n->lines.irq_line = true; cpu_set_irq_line(&n->cpu, 0, true);
        n->cart->vtable->reset(n->cart);
        assert(!n->lines.irq_line && !(n->cpu.irq_lines & 1));
        assert(nes_cpu_bus_read(n, 0x6000) == 0x43);
        fixture_free(n);
    }
    assert(remove(path) == 0);
}
static void wiring_and_optional_ram(void) {
    puts("  MMC3 four-screen vs TxSROM CIRAM wiring; VRC6 and FME-7 RAM enable");
    char path[512]; fixture_path(path, "wiring.nes");
    fixture_rom(path, 4, false, false, 0);
    FILE *f = fopen(path, "r+b"); assert(f); assert(fseek(f, 6, SEEK_SET) == 0);
    assert(fputc(0x49, f) != EOF); assert(fclose(f) == 0);
    NES *n = fixture_load(path); bool ce;
    nes_cpu_bus_write(n, 0xA000, 1);
    for (unsigned page = 0; page < 4; ++page)
        assert(n->cart->vtable->remap_ciram_addr(n->cart, (uint16_t)(0x2000 + page * 1024), &ce) == page * 1024);
    nes_reset(n); assert(n->cart->mirroring == MIRROR_FOUR_SCREEN); fixture_free(n);
    fixture_rom(path, 118, false, false, 0); n = fixture_load(path);
    for (unsigned mode = 0; mode < 2; ++mode) {
        for (unsigned pattern = 0; pattern < 16; ++pattern) {
            for (unsigned reg = 0; reg < 6; ++reg) {
                nes_cpu_bus_write(n, 0x8000, (uint8_t)((mode << 7) | reg));
                nes_cpu_bus_write(n, 0x8001, (uint8_t)(((pattern >> (reg & 3)) & 1) << 7));
            }
            for (unsigned page = 0; page < 4; ++page) {
                unsigned reg = mode ? page + 2 : page / 2;
                unsigned expected = ((pattern >> (reg & 3)) & 1) * 1024;
                assert(n->cart->vtable->remap_ciram_addr(n->cart, (uint16_t)(0x2000 + page * 1024), &ce) == expected);
            }
        }
    }
    fixture_free(n);
    fixture_rom(path, 24, false, false, 0); n = fixture_load(path);
    nes_cpu_bus_write(n, 0xB003, 0); drive(n, 0xA4); assert(nes_cpu_bus_read(n, 0x6000) == 0xA4);
    nes_cpu_bus_write(n, 0xB003, 0x80); nes_cpu_bus_write(n, 0x6000, 0x62);
    assert(nes_cpu_bus_read(n, 0x6000) == 0x62); fixture_free(n);
    fixture_rom(path, 69, false, false, 0); n = fixture_load(path);
    nes_cpu_bus_write(n, 0x8000, 8); nes_cpu_bus_write(n, 0xA000, 3);
    assert(nes_cpu_bus_read(n, 0x6000) == 3);
    nes_cpu_bus_write(n, 0xA000, 0x40); drive(n, 0xA8); assert(nes_cpu_bus_read(n, 0x6000) == 0xA8);
    nes_cpu_bus_write(n, 0xA000, 0xC0); nes_cpu_bus_write(n, 0x6000, 0x53);
    assert(nes_cpu_bus_read(n, 0x6000) == 0x53);
    fixture_free(n); assert(remove(path) == 0);
}
static void mmc5_reads_and_writes_agree(void) {
    puts("  MMC5 RAM selection, protection, absent RAM and banked CHR writes");
    char path[512]; fixture_path(path, "mmc5.nes"); fixture_rom(path, 5, false, true, 0);
    NES *n = fixture_load(path);
    nes_cpu_bus_write(n, 0x5102, 2); nes_cpu_bus_write(n, 0x5103, 1);
    for (unsigned mode = 1; mode < 4; ++mode) {
        nes_cpu_bus_write(n, 0x5100, (uint8_t)mode);
        nes_cpu_bus_write(n, 0x5114, 1); nes_cpu_bus_write(n, 0x5115, 4); nes_cpu_bus_write(n, 0x5116, 7);
        for (unsigned slot = 0; slot < (mode == 1 ? 2u : 3u); ++slot) {
            uint16_t address = (uint16_t)(0x8007 + slot * 8192);
            nes_cpu_bus_write(n, address, (uint8_t)(0x31 + slot));
            assert(nes_cpu_bus_read(n, address) == 0x31 + slot);
        }
    }
    nes_cpu_bus_write(n, 0x5102, 0); nes_cpu_bus_write(n, 0x8007, 0xFF);
    assert(nes_cpu_bus_read(n, 0x8007) == 0x31);
    nes_cpu_bus_write(n, 0x5101, 3); nes_cpu_bus_write(n, 0x5120, 3);
    nes_ppu_bus_write(n, 0x0007, 0xC4);
    assert(n->cart->chr_rom[3 * 1024 + 7] == 0xC4 && nes_ppu_bus_read(n, 7) == 0xC4);
    free(n->cart->prg_ram); n->cart->prg_ram = NULL; n->cart->prg_ram_size = 0;
    drive(n, 0xB5); assert(nes_cpu_bus_read(n, 0x8007) == 0xB5);
    fixture_free(n); assert(remove(path) == 0);
}
static void gxrom_banks_and_restore(void) {
    puts("  GxROM bank wrapping, CHR RAM, reset and state-load bank restoration");
    char path[512]; fixture_path(path, "gxrom.nes");
    for (unsigned banks = 1; banks <= 4; ++banks) {
        for (unsigned chr_banks = 0; chr_banks <= 4; ++chr_banks) {
            uint8_t header[16] = {'N','E','S',0x1A,0,0,0x20,0x40};
            header[4] = (uint8_t)(banks * 2);
            header[5] = (uint8_t)chr_banks;
            FILE *f = fopen(path, "wb"); assert(f);
            assert(fwrite(header, 1, sizeof(header), f) == sizeof(header));
            uint8_t data[32768];
            for (unsigned b = 0; b < banks; ++b) {
                memset(data, (int)(0x40 + b), sizeof(data));
                assert(fwrite(data, 1, sizeof(data), f) == sizeof(data));
            }
            for (unsigned b = 0; b < chr_banks; ++b) {
                memset(data, (int)(0x80 + b), 8192);
                assert(fwrite(data, 1, 8192, f) == 8192);
            }
            assert(fclose(f) == 0);
            NES *n = fixture_load(path);
            for (unsigned value = 0; value < 256; ++value) {
                nes_cpu_bus_write(n, (uint16_t)(0x8000 + value), (uint8_t)value);
                // Three populated banks are padded by mirroring the last bank.
                unsigned prg = ((value >> 4) & 3) % (banks == 3 ? 4 : banks);
                if (prg == banks) --prg;
                assert(nes_cpu_bus_read(n, 0x8000) == 0x40 + prg);
                assert(nes_cpu_bus_read(n, 0xBFFF) == 0x40 + prg);
                assert(nes_cpu_bus_read(n, 0xFFFF) == 0x40 + prg);
                if (chr_banks) {
                    unsigned chr = (value & 3) % (chr_banks == 3 ? 4 : chr_banks);
                    if (chr == chr_banks) --chr;
                    assert(nes_ppu_bus_read(n, 0) == 0x80 + chr);
                    assert(nes_ppu_bus_read(n, 0x1FFF) == 0x80 + chr);
                } else {
                    nes_ppu_bus_write(n, 0x1FFF, (uint8_t)value);
                    assert(nes_ppu_bus_read(n, 0x1FFF) == value);
                }
            }
            uint8_t registers[2] = {0};
            StateIO io = {registers, sizeof(registers), 0, false, true};
            n->cart->vtable->state(n->cart, &io);
            assert(io.ok && io.pos == 2 && registers[0] == 3 && registers[1] == 3);
            uint8_t prg = nes_cpu_bus_read(n, 0xFFFF), chr = nes_ppu_bus_read(n, 0x1FFF);
            uint8_t *save; size_t size;
            assert(nes_state_encode(n, &save, &size) == NES_STATE_OK);
            n->cart->vtable->reset(n->cart);
            assert(nes_cpu_bus_read(n, 0xFFFF) == 0x40);
            if (chr_banks) assert(nes_ppu_bus_read(n, 0x1FFF) == 0x80);
            assert(nes_state_decode(n, save, size) == NES_STATE_OK);
            assert(nes_cpu_bus_read(n, 0xFFFF) == prg);
            assert(nes_ppu_bus_read(n, 0x1FFF) == chr);
            free(save);
            fixture_free(n);
        }
    }
    assert(remove(path) == 0);
}
int main(void) {
    fixture_start("cartridge-bus");
    open_bus_and_partial_reads(); all_mapper_rom_ram_boundaries(); mmc1_and_mmc3_protection();
    wiring_and_optional_ram(); mmc5_reads_and_writes_agree(); gxrom_banks_and_restore();
    assert(SAVE_RMDIR(fixture_dir) == 0);
    puts("Cartridge bus and mapper checks passed.");
    return 0;
}
