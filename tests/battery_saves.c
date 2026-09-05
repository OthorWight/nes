#include "save_fixture.h"

static void canonical_paths_and_reload(void) {
    puts("  legacy import, canonical precedence, reset/reload and ROM switching");
    char rom[512], legacy[512], canonical[512], other_rom[512], other_save[512];
    fixture_path(rom, "game.nes"); fixture_path(legacy, "game.sav");
    fixture_path(canonical, "canonical.sav"); fixture_path(other_rom, "other.nes");
    fixture_path(other_save, "other-progress.sav");
    fixture_rom(rom, 1, true, true, 0);
    fixture_rom(other_rom, 4, true, false, 1);
    uint8_t old_ram[8192]; memset(old_ram, 0x31, sizeof(old_ram));
    assert(state_atomic_write(legacy, old_ram, sizeof(old_ram)));
    NES *n = fixture_load(rom);
    assert(n->cart->prg_ram[0] == 0x31 && n->cart->prg_ram[8191] == 0x31);
    assert(cartridge_set_save_path(n->cart, canonical));
    assert(n->cart->prg_ram[0] == 0x31); // Missing canonical retains legacy RAM.
    n->cart->prg_ram[0] = 0xA7; n->cart->prg_ram[8191] = 0xC4;
    nes_reset(n);
    assert(n->cart->prg_ram[0] == 0xA7);
    assert(cartridge_save_battery(n->cart));
    fixture_free(n); // Destruction must not write to a second path.
    FILE *f = fopen(legacy, "rb"); assert(f);
    assert(fgetc(f) == 0x31); assert(fclose(f) == 0);
    n = fixture_load(other_rom);
    assert(cartridge_set_save_path(n->cart, other_save));
    assert(n->cart->prg_ram[0] == 0);
    n->cart->prg_ram[0] = 0x42;
    assert(cartridge_save_battery(n->cart));
    fixture_free(n);
    n = fixture_load(rom);
    assert(cartridge_set_save_path(n->cart, canonical));
    assert(n->cart->prg_ram[0] == 0xA7 && n->cart->prg_ram[8191] == 0xC4);
    fixture_free(n);
    n = fixture_load(other_rom);
    assert(cartridge_set_save_path(n->cart, other_save));
    assert(n->cart->prg_ram[0] == 0x42);
    fixture_free(n);
    assert(remove(rom) == 0); assert(remove(legacy) == 0);
    assert(remove(canonical) == 0); assert(remove(other_rom) == 0); assert(remove(other_save) == 0);
}

static void mmc5_full_ram_is_loaded_after_allocation(void) {
    puts("  MMC5 saves restore every RAM bank after mapper initialization");
    char rom[512], save[512]; fixture_path(rom, "mmc5.nes"); fixture_path(save, "mmc5.sav");
    fixture_rom(rom, 5, true, false, 0);
    NES *n = fixture_load(rom);
    assert(n->cart->prg_ram_size == 65536);
    for (unsigned i = 0; i < 65536; ++i) n->cart->prg_ram[i] = (uint8_t)(i * 13 + i / 8192);
    assert(cartridge_save_battery(n->cart));
    fixture_free(n);
    n = fixture_load(rom);
    for (unsigned i = 0; i < 65536; ++i) assert(n->cart->prg_ram[i] == (uint8_t)(i * 13 + i / 8192));
    fixture_free(n);
    assert(remove(rom) == 0); assert(remove(save) == 0);
}

static void damaged_battery_files_are_preserved(void) {
    puts("  truncated/oversized battery files cannot partially replace RAM or be overwritten");
    char rom[512], save[512], missing[512]; fixture_path(rom, "bad.nes");
    fixture_path(save, "bad.sav"); fixture_path(missing, "no-directory/game.sav");
    fixture_rom(rom, 0, true, true, 0);
    NES *n = fixture_load(rom);
    memset(n->cart->prg_ram, 0xB4, 8192);
    assert(state_atomic_write(save, "damaged", 7));
    assert(!cartridge_load_battery(n->cart));
    for (unsigned i = 0; i < 8192; ++i) assert(n->cart->prg_ram[i] == 0xB4);
    assert(!cartridge_save_battery(n->cart));
    fixture_free(n);
    FILE *f = fopen(save, "rb"); assert(f);
    char contents[8]; assert(fread(contents, 1, 8, f) == 7 && !memcmp(contents, "damaged", 7));
    assert(fclose(f) == 0);
    n = fixture_load(rom);
    assert(n->cart->battery_save_blocked);
    fixture_free(n);
    uint8_t oversized[8193]; memset(oversized, 0x12, sizeof(oversized));
    assert(state_atomic_write(save, oversized, sizeof(oversized)));
    n = fixture_load(rom);
    assert(n->cart->battery_save_blocked && n->cart->prg_ram[0] == 0);
    assert(!cartridge_save_battery(n->cart));
    // Repair the file and explicitly reload; successful validation unblocks it.
    assert(state_atomic_write(save, oversized, 8192));
    assert(cartridge_load_battery(n->cart));
    assert(n->cart->prg_ram[0] == 0x12 && !n->cart->battery_save_blocked);
    assert(cartridge_set_save_path(n->cart, missing));
    assert(!cartridge_save_battery(n->cart));
    fixture_free(n);
    assert(remove(rom) == 0); assert(remove(save) == 0);
}

static void no_battery_means_no_automatic_file(void) {
    puts("  non-battery cartridges do not create battery files");
    char rom[512], save[512]; fixture_path(rom, "volatile.nes"); fixture_path(save, "volatile.sav");
    fixture_rom(rom, 2, false, true, 0);
    NES *n = fixture_load(rom);
    n->cart->prg_ram[0] = 0xFF;
    assert(cartridge_save_battery(n->cart));
    fixture_free(n);
    FILE *f = fopen(save, "rb"); assert(!f);
    assert(remove(rom) == 0);
}
int main(void) {
    fixture_start("battery");
    canonical_paths_and_reload();
    mmc5_full_ram_is_loaded_after_allocation();
    damaged_battery_files_are_preserved();
    no_battery_means_no_automatic_file();
    assert(SAVE_RMDIR(fixture_dir) == 0);
    puts("Battery persistence and failure checks passed.");
    return 0;
}
