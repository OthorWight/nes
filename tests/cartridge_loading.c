#include "save_fixture.h"
#include "save_state.h"

static void emit(const char *path, const uint8_t h[16], uint32_t prg, uint32_t chr, unsigned trainer) {
    FILE *f = fopen(path, "wb"); assert(f);
    assert(fwrite(h, 1, 16, f) == 16);
    for (unsigned i = 0; i < trainer; ++i) assert(fputc((int)(i ^ 0xA5) & 255, f) != EOF);
    for (uint32_t i = 0; i < prg; ++i) assert(fputc((int)(i / 8192), f) != EOF);
    for (uint32_t i = 0; i < chr; ++i) assert(fputc((int)(i / 1024), f) != EOF);
    assert(fclose(f) == 0);
}
static void parse_sizes_and_ids(void) {
    puts("  iNES and NES 2.0 metadata, extended IDs and checked sizes");
    uint8_t h[16] = {'N','E','S',0x1A,2,1,0x40,0,0,0,0,0,0,0,0,0};
    CartridgeInfo info, before; char error[128];
    assert(cartridge_parse_header(h, &info, error, sizeof(error)));
    assert(!info.nes2 && info.mapper_id == 4 && info.prg_ram_size == 8192);
    h[7] = 0xF8; h[8] = 0xA1; h[9] = 1; h[10] = 0x70; h[11] = 0x07; h[6] |= 2;
    assert(cartridge_parse_header(h, &info, error, sizeof(error)));
    assert(info.nes2 && info.mapper_id == 0x1F4 && info.submapper == 10);
    assert(info.prg_rom_size == 258u * 16384 && info.chr_rom_size == 8192);
    assert(info.prg_ram_size == 0 && info.prg_nvram_size == 8192 && info.chr_ram_size == 8192);
    h[9] = 0xFF; h[4] = (13 << 2) | 1; h[5] = (11 << 2) | 1;
    assert(cartridge_parse_header(h, &info, error, sizeof(error)));
    assert(info.prg_rom_size == 24576 && info.chr_rom_size == 6144);
    before = info;
    h[4] = 0xFF;
    assert(!cartridge_parse_header(h, &info, error, sizeof(error)));
    assert(!memcmp(&info, &before, sizeof(info)) && *error); // No partial metadata.
    h[4] = 0; h[9] = 0;
    assert(!cartridge_parse_header(h, &info, error, sizeof(error)));
}
static void rejected_load(const char *path, const char *reason) {
    NES n; nes_init(&n); n.lines.irq_line = true; n.cpu.irq_lines = 7;
    NES *before = malloc(sizeof(n)); assert(before); *before = n;
    char error[128];
    assert(!cartridge_load_ex(&n, path, error, sizeof(error)));
    assert(strstr(error, reason));
    assert(!memcmp(before, &n, sizeof(n))); // Failure leaves the current machine alone.
    free(before);
}
static void invalid_and_unsupported_images(void) {
    puts("  malformed/truncated images and unsupported boards fail before attachment");
    char path[512]; fixture_path(path, "bad.nes");
    uint8_t h[16] = {'N','E','S',0x1A,1,1,0,8,0,0,0,0,0,0,0,0};
    assert(state_atomic_write(path, h, 7)); rejected_load(path, "header");
    h[0] = 'X'; emit(path, h, 0, 0, 0); rejected_load(path, "header"); h[0] = 'N';
    emit(path, h, 16383, 0, 0); rejected_load(path, "Truncated");
    emit(path, h, 16384, 8191, 0); rejected_load(path, "Truncated");
    h[6] = 4; h[10] = 7; emit(path, h, 16384, 8192, 511); rejected_load(path, "Truncated");
    h[6] = 0; h[10] = 0;
    h[8] = 1; emit(path, h, 0, 0, 0); rejected_load(path, "mapper 256"); h[8] = 0x10;
    emit(path, h, 0, 0, 0); rejected_load(path, "Submapper"); h[8] = 0;
    h[12] = 1; emit(path, h, 0, 0, 0); rejected_load(path, "timing"); h[12] = 3;
    emit(path, h, 0, 0, 0); rejected_load(path, "timing"); h[12] = 0;
    h[7] = 9; emit(path, h, 0, 0, 0); rejected_load(path, "Console"); h[7] = 8;
    h[14] = 1; emit(path, h, 0, 0, 0); rejected_load(path, "Miscellaneous"); h[14] = 0;
    h[13] = 1; emit(path, h, 0, 0, 0); rejected_load(path, "reserved"); h[13] = 0;
    h[11] = 7; emit(path, h, 0, 0, 0); rejected_load(path, "Mixed CHR"); h[11] = 0;
    h[6] = 2; h[10] = 0x77; emit(path, h, 0, 0, 0); rejected_load(path, "Mixed PRG");
    h[6] = 0; h[10] = 8; emit(path, h, 0, 0, 0); rejected_load(path, "RAM banking"); h[10] = 0x70;
    emit(path, h, 0, 0, 0); rejected_load(path, "battery");
    h[10] = 0; h[4] = 3; h[9] = 0;
    emit(path, h, 0, 0, 0); rejected_load(path, "Extended ROM");
    h[10] = 0; h[4] = 0xFF; h[9] = 15;
    emit(path, h, 0, 0, 0); rejected_load(path, "size limit");
    assert(remove(path) == 0);
}
static void padded_rom_and_absent_ram(void) {
    puts("  exponent-sized ROM padding, absent RAM and small RAM mirroring");
    char path[512]; fixture_path(path, "sizes.nes");
    uint8_t h[16] = {'N','E','S',0x1A,(13 << 2) | 1,(11 << 2) | 1,0,8,0,0xFF,0,0,0,0,0,0};
    emit(path, h, 24576, 6144, 0);
    NES *n = fixture_load(path);
    assert(n->cart->info.prg_rom_size == 24576 && n->cart->prg_rom_size == 32768);
    assert(n->cart->info.chr_rom_size == 6144 && n->cart->chr_rom_size == 8192 && !n->cart->chr_is_ram);
    assert(!n->cart->prg_ram && n->cart->prg_ram_size == 0);
    assert(nes_cpu_bus_read(n, 0xC000) == 2 && nes_cpu_bus_read(n, 0xE000) == 2);
    assert(nes_ppu_bus_read(n, 0x1000) == 4 && nes_ppu_bus_read(n, 0x1800) == 4);
    nes_cpu_bus_write(n, 0, 0xA7); assert(nes_cpu_bus_read(n, 0x6000) == 0xA7);
    fixture_free(n);
    h[4] = 1; h[5] = 0; h[9] = 0; h[10] = 5; h[11] = 7;
    emit(path, h, 16384, 0, 0); n = fixture_load(path);
    assert(n->cart->chr_is_ram && n->cart->prg_ram_size == 2048);
    nes_cpu_bus_write(n, 0x67FF, 0xB9); assert(nes_cpu_bus_read(n, 0x7FFF) == 0xB9);
    nes_ppu_bus_write(n, 0x1FFF, 0xD5); assert(nes_ppu_bus_read(n, 0x1FFF) == 0xD5);
    fixture_free(n); assert(remove(path) == 0);
}
static void trainer_and_nvram(void) {
    puts("  trainers survive canonical save selection; declared CHR NVRAM persists");
    char path[512], save[512]; fixture_path(path, "nvram.nes"); fixture_path(save, "nvram.sav");
    uint8_t h[16] = {'N','E','S',0x1A,1,0,6,8,0,0,0x70,0x70,0,0,0,0};
    emit(path, h, 16384, 0, 512);
    NES *n = fixture_load(path);
    for (unsigned i = 0; i < 512; ++i) assert(n->cart->prg_ram[0x1000 + i] == (uint8_t)(i ^ 0xA5));
    n->cart->prg_ram[2] = 0x32; n->cart->prg_ram[0x1000] = 0;
    nes_ppu_bus_write(n, 0x1FFF, 0xE9);
    assert(cartridge_save_battery(n->cart)); fixture_free(n);
    n = fixture_load(path);
    assert(n->cart->prg_ram[2] == 0x32 && n->cart->chr_rom[0x1FFF] == 0xE9);
    assert(cartridge_set_save_path(n->cart, save));
    assert(n->cart->prg_ram[0x1000] == 0xA5);
    fixture_free(n); assert(remove(path) == 0); assert(remove(save) == 0);
}
static void mapper78_wiring(void) {
    puts("  mapper 78 legacy flag and explicit submapper nametable wiring");
    // NESdev mapper 78: bit 3 selects H/V or lower/upper one-screen.
    // Both header mirroring bits are covered; explicit submappers take priority.
    char path[512]; fixture_path(path, "mapper78.nes");
    const int variants[] = {-1, 0, 1, 3}; // -1: legacy iNES
    for (unsigned v = 0; v < sizeof(variants) / sizeof(variants[0]); ++v) {
        for (unsigned flags = 0; flags < 4; ++flags) {
            int sub = variants[v];
            bool alt = (flags & 2) != 0;
            bool hv = sub == 3 || (sub <= 0 && alt);
            uint8_t h[16] = {'N','E','S',0x1A,8,16,0xE0,0x40,0,0,0,0,0,0,0,0};
            h[6] |= (flags & 1) | (alt ? 8 : 0);
            if (sub >= 0) { h[7] |= 8; h[8] = (uint8_t)(sub << 4); }
            emit(path, h, 131072, 131072, 0);
            NES *n = fixture_load(path);
            assert(n->cart->mirroring == (hv ? MIRROR_HORIZONTAL : MIRROR_ONE_SCREEN_LOW));
            for (unsigned pass = 0; pass < 4; ++pass) {
                unsigned high = pass & 1;
                // Exercise high bank bits at the same time as CIRAM selection.
                nes_cpu_bus_write(n, 0x8000, (uint8_t)(0xF7 | (high << 3)));
                assert(nes_cpu_bus_read(n, 0x8000) == 14);
                assert(nes_cpu_bus_read(n, 0xC000) == 14);
                assert(nes_ppu_bus_read(n, 0) == 120);
                memset(n->ciram, 0, sizeof(n->ciram));
                n->ciram[0x37] = 0xA1; n->ciram[0x437] = 0xB2;
                for (unsigned page = 0; page < 4; ++page) {
                    unsigned bank = hv ? (high ? (page & 1) : (page >> 1)) : high;
                    uint16_t addr = (uint16_t)(0x2037 + page * 0x400);
                    assert(nes_ppu_bus_read(n, addr) == (bank ? 0xB2 : 0xA1));
                    nes_ppu_bus_write(n, addr, 0xD3);
                    assert(n->ciram[bank * 0x400 + 0x37] == 0xD3);
                    n->ciram[bank * 0x400 + 0x37] = bank ? 0xB2 : 0xA1;
                }
            }
            nes_reset(n);
            assert(n->cart->mirroring == (hv ? MIRROR_HORIZONTAL : MIRROR_ONE_SCREEN_LOW));
            assert(nes_cpu_bus_read(n, 0x8000) == 0);
            assert(nes_cpu_bus_read(n, 0xC000) == 14);
            assert(nes_ppu_bus_read(n, 0) == 0);
            fixture_free(n);
        }
    }
    uint8_t h[16] = {'N','E','S',0x1A,8,16,0xE8,0x48,0x20,0,0,0,0,0,0,0};
    emit(path, h, 0, 0, 0); rejected_load(path, "Submapper");
    h[8] = 0x40; emit(path, h, 0, 0, 0); rejected_load(path, "Submapper");
    h[8] = 0x30; h[4] = 9; emit(path, h, 0, 0, 0); rejected_load(path, "Extended ROM");
    h[4] = 8; h[5] = 17; emit(path, h, 0, 0, 0); rejected_load(path, "Extended ROM");
    // The exception must not admit unrelated unsupported four-screen boards.
    h[4] = 1; h[5] = 1; h[6] = 0x28; h[7] = 8; h[8] = 0;
    emit(path, h, 0, 0, 0); rejected_load(path, "Four-screen");
    assert(remove(path) == 0);
}
int main(void) {
    fixture_start("cartridges");
    parse_sizes_and_ids(); invalid_and_unsupported_images(); padded_rom_and_absent_ram(); trainer_and_nvram();
    mapper78_wiring();
    // Exercise the shared synthetic iNES fixture too (also verifies old headers).
    char path[512]; fixture_path(path, "legacy.nes"); fixture_rom(path, 0, false, true, 0);
    NES *n = fixture_load(path); fixture_free(n); assert(remove(path) == 0);
    assert(SAVE_RMDIR(fixture_dir) == 0);
    puts("Cartridge header and loading checks passed.");
    return 0;
}
