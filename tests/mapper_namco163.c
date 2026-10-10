#include "save_fixture.h"
#include "save_state.h"

/* $E800.6/.7 independently disable CIRAM in the two pattern-table halves;
   they never affect the four nametable slots. Hardware register reference:
   https://www.nesdev.org/wiki/INES_Mapper_019 */
static void chr_ciram_controls(const char *path, bool chr_ram) {
    fixture_rom(path, 19, false, chr_ram, 0);
    NES *n = fixture_load(path);
    for (unsigned bank = 0; bank < n->cart->chr_rom_size / 1024; ++bank)
        memset(n->cart->chr_rom + bank * 1024, (uint8_t)(0x40 + bank), 1024);
    memset(n->ciram, 0xA0, 1024);
    memset(n->ciram + 1024, 0xB1, 1024);
    for (unsigned controls = 0; controls < 4; ++controls) {
        /* Use the mirrored end of the register range and keep PRG bank 3. */
        nes_cpu_bus_write(n, 0xEFFF, (uint8_t)((controls << 6) | 3));
        assert(nes_cpu_bus_read(n, 0xA000) == 3);
        for (unsigned slot = 0; slot < 8; ++slot) {
            bool ram_disabled = (controls & (slot < 4 ? 1u : 2u)) != 0;
            const uint8_t banks[] = {0xDF, 0xE0, 0xE1, 0xFE, 0xFF};
            for (unsigned b = 0; b < sizeof(banks); ++b) {
                uint8_t bank = banks[b];
                nes_cpu_bus_write(n, (uint16_t)(0x8000 + slot * 0x800), bank);
                bool ciram = bank >= 0xE0 && !ram_disabled;
                unsigned backing = bank % (n->cart->chr_rom_size / 1024);
                unsigned offset = slot * 1024 + 0x37;
                uint8_t *memory = ciram ? n->ciram + (bank & 1) * 1024 + 0x37 :
                    n->cart->chr_rom + backing * 1024 + 0x37;
                uint8_t before = *memory;
                assert(nes_ppu_bus_read(n, (uint16_t)offset) == before);
                nes_ppu_bus_write(n, (uint16_t)offset, 0x29);
                bool writable = ciram || chr_ram;
                assert(*memory == (writable ? 0x29 : before));
                assert(nes_ppu_bus_read(n, (uint16_t)offset) == (writable ? 0x29 : before));
                *memory = before;
            }
        }
        /* CIRAM-backed nametables ignore both pattern-table disable bits. */
        for (unsigned table = 0; table < 4; ++table) {
            uint16_t reg = (uint16_t)(0xC000 + table * 0x800);
            uint16_t addr = (uint16_t)(0x2000 + table * 1024 + 0x37);
            nes_cpu_bus_write(n, reg, (uint8_t)(0xE0 + (table & 1)));
            assert(nes_ppu_bus_read(n, addr) == ((table & 1) ? 0xB1 : 0xA0));
            nes_ppu_bus_write(n, addr, 0x29);
            assert(n->ciram[(table & 1) * 1024 + 0x37] == 0x29);
            n->ciram[(table & 1) * 1024 + 0x37] = (table & 1) ? 0xB1 : 0xA0;
            nes_cpu_bus_write(n, reg, 2);
            assert(nes_ppu_bus_read(n, addr) == n->cart->chr_rom[2 * 1024 + 0x37]);
        }
    }
    fixture_free(n);
}

static void state_and_reset(const char *path) {
    fixture_rom(path, 19, false, false, 0);
    NES *n = fixture_load(path);
    memset(n->cart->chr_rom, 0x63, n->cart->chr_rom_size);
    memset(n->ciram, 0xA0, sizeof(n->ciram));
    nes_cpu_bus_write(n, 0x8000, 0xE0);
    nes_cpu_bus_write(n, 0xA000, 0xE0);
    for (unsigned controls = 0; controls < 4; ++controls) {
        nes_cpu_bus_write(n, 0xE800, (uint8_t)((controls << 6) | 3));
        uint8_t *state; size_t size;
        assert(nes_state_encode(n, &state, &size) == NES_STATE_OK);
        nes_cpu_bus_write(n, 0xE800, (uint8_t)(((controls ^ 3) << 6) | 2));
        assert(nes_state_decode(n, state, size) == NES_STATE_OK);
        assert(nes_cpu_bus_read(n, 0xA000) == 3);
        assert(nes_ppu_bus_read(n, 0) == ((controls & 1) ? 0x63 : 0xA0));
        assert(nes_ppu_bus_read(n, 0x1000) == ((controls & 2) ? 0x63 : 0xA0));
        free(state);
    }
    nes_reset(n);
    nes_cpu_bus_write(n, 0x8000, 0xE0);
    nes_cpu_bus_write(n, 0xA000, 0xE0);
    assert(nes_cpu_bus_read(n, 0xA000) == 1);
    assert(nes_ppu_bus_read(n, 0) == 0xA0 && nes_ppu_bus_read(n, 0x1000) == 0xA0);
    fixture_free(n);
}

int main(void) {
    fixture_start("namco163");
    char path[512]; fixture_path(path, "fixture.nes");
    chr_ciram_controls(path, false);
    chr_ciram_controls(path, true);
    state_and_reset(path);
    assert(!remove(path)); assert(SAVE_RMDIR(fixture_dir) == 0);
    puts("Namco 163 pattern CIRAM controls, nametables, writes, save/load and reset passed.");
}
