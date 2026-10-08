#include "nes_system.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* AccuracyCoin's menu catalog lives at $8100. Read names and result addresses
   from the supplied ROM, rather than assuming one revision's test order.
   Only this optional integration runner knows about the test ROM's format. */
static NES nes;
typedef struct {
    char name[80];
    uint16_t result;
    unsigned page;
    unsigned row;
    uint8_t previous;
} CoinTest;
static CoinTest tests[256];
static unsigned count;

static uint8_t rom(uint16_t address) {
    return nes.cart->prg_rom[(address - 0x8000u) % nes.cart->prg_rom_size];
}
static uint16_t word(uint16_t address) {
    return (uint16_t)(rom(address) | (rom(address + 1) << 8));
}
static bool name(uint16_t *address, char *out, size_t capacity) {
    size_t length = 0;
    while (*address >= 0x8000 && length + 1 < capacity) {
        uint8_t c = rom((*address)++);
        if (c == 0xFF) { out[length] = 0; return length != 0; }
        /* The menu uses single-byte tokens for repeated addressing modes. */
        if (c >= 0xF0 && c <= 0xF3) {
            static const char *modes[] = {"Indirect", "Zero Page", "Absolute", "Immediate"};
            const char *mode = modes[c - 0xF0];
            size_t size = strlen(mode);
            if (length + size >= capacity) return false;
            memcpy(out + length, mode, size); length += size; continue;
        }
        if (c < 32 || c > 126) return false;
        out[length++] = (char)c;
    }
    return false;
}
static bool catalog(void) {
    if (nes.cart->mapper_id != 0 || nes.cart->prg_rom_size != 32768) return false;
    uint16_t first = word(0x8100);
    if (first <= 0x8100 || first > 0x8180 || ((first - 0x8100) & 1)) return false;
    unsigned pages = (first - 0x8100) / 2;
    for (unsigned page = 0; page < pages; ++page) {
        uint16_t address = word((uint16_t)(0x8100 + page * 2));
        char title[80];
        if (!name(&address, title, sizeof(title))) return false;
        if (!page && strcmp(title, "CPU Behavior")) return false;
        for (unsigned row = 0; rom(address) != 0xFF; ++row) {
            if (row >= 32 || count >= 256) return false;
            CoinTest t = { .page = page + 1, .row = row + 1 };
            if (!name(&address, t.name, sizeof(t.name))) return false;
            t.result = word(address);
            address += 4; /* Result address and test entry point. */
            if (t.result == 0x03FF) continue; /* DRAW entries. */
            if (t.result < 0x0400 || t.result >= 0x0500) return false;
            tests[count++] = t;
        }
    }
    return count != 0;
}
static bool number(const char *text, unsigned *out) {
    char *end;
    unsigned long value = strtoul(text, &end, 10);
    if (!*text || *end || value > 64) return false;
    *out = (unsigned)value;
    return true;
}

int main(int argc, char **argv) {
    if (argc < 2 || argc > 4) { fprintf(stderr, "Usage: %s AccuracyCoin.nes [page [row]]\n", argv[0]); return 2; }
    nes_init(&nes);
    nes.cart = cartridge_load(&nes, argv[1]);
    if (!nes.cart) return 2;
    if (!catalog()) {
        fprintf(stderr, "Unsupported AccuracyCoin menu catalog\n");
        cartridge_free(nes.cart);
        return 2;
    }
    nes_reset(&nes);
    unsigned page = 0, row = 0;
    if ((argc >= 3 && !number(argv[2], &page)) || (argc >= 4 && !number(argv[3], &row))) {
        cartridge_free(nes.cart); return 2;
    }
    unsigned selected = 0;
    for (unsigned i = 0; i < count; ++i)
        if ((!page || tests[i].page == page) && (!row || tests[i].row == row)) ++selected;
    if (!selected || (!page && row)) { cartridge_free(nes.cart); return 2; }
    unsigned start_frame = page ? 120 + (page - 1 + row) * 8 : 120;
    unsigned done = 0, frame, last_progress = 122;
    for (frame = 0; frame < 30000; ++frame) {
        /* Use normal controller input to select Run All from the initial menu. */
        nes.controller_state[0] = 0;
        if (frame >= 120 && frame < start_frame && (frame - 120) % 8 < 2) {
            nes.controller_state[0] = (frame - 120) / 8 < page - 1 ? 0x80 : 0x20;
        } else if (frame >= start_frame && frame < start_frame + 2) {
            nes.controller_state[0] = page ? 0x01 : 0x08;
        }
        nes.frame_ready = false;
        unsigned instructions = 0;
        while (!nes.frame_ready && instructions++ < 100000) nes_clock_tick(&nes);
        if (!nes.frame_ready) break;
        nes.apu.audio_buffer_idx = 0;
        if (getenv("NES_COIN_TRACE") && frame % 100 == 0)
            fprintf(stderr, "frame=%u pc=%04X x=%02X y=%02X cycle=%llu pending=%u/%u read=%u flags=%02X\n", frame, nes.cpu.program_counter, nes.cpu.index_x, nes.cpu.index_y, (unsigned long long)nes.cpu.cycle_count, nes.dmc_dma_pending, nes.oam_dma_pending, nes.ppu.data_read_pipeline, nes.ppu.ppu_status);
        if (frame < start_frame + 2) continue;
        done = 0;
        for (unsigned i = 0; i < count; ++i) {
            CoinTest *t = &tests[i];
            if ((page && t->page != page) || (row && t->row != row)) continue;
            uint8_t value = nes.wram[t->result];
            if (value != 3 && (value & 3)) ++done;
            if (value != t->previous) {
                last_progress = frame;
                const char *status = (value & 3) == 1 ? "PASS" :
                    (value & 3) == 2 ? "FAIL" : value == 0xFF ? "SKIP" : "RUN ";
                printf("%s page %u %-28s code %u ($%02X)\n", status,
                    t->page, t->name, value >> 2, value);
                fflush(stdout);
                if ((value & 3) == 2 && getenv("NES_COIN_DUMP")) {
                    for (unsigned a = 0x50; a < 0x70; ++a) {
                        if (!(a & 15)) printf("%04X:", a);
                        printf(" %02X", nes.wram[a]);
                        if ((a & 15) == 15) putchar('\n');
                    }
                    for (unsigned a = 0x500; a < 0x700; ++a) {
                        if (!(a & 15)) printf("%04X:", a);
                        printf(" %02X", nes.wram[a]);
                        if ((a & 15) == 15) putchar('\n');
                    }
                }
                t->previous = value;
            }
        }
        if (done == selected) break;
        if (frame - last_progress > 1800) {
            fprintf(stderr, "No test completed for 1800 frames; PC=$%04X, result pointer=$%02X%02X, menu=%u/%u\n",
                nes.cpu.program_counter, nes.wram[0x1F], nes.wram[0x1E], nes.wram[0x14] + 1, nes.wram[0x16]);
            break;
        }
    }
    unsigned passed = 0, failed = 0, skipped = 0;
    for (unsigned i = 0; i < count; ++i) {
        if ((page && tests[i].page != page) || (row && tests[i].row != row)) continue;
        unsigned result = nes.wram[tests[i].result] == 3 ? 0 : nes.wram[tests[i].result] & 3;
        passed += result == 1; failed += result == 2; skipped += result == 3;
        if (!result) fprintf(stderr, "UNFINISHED page %u %s\n", tests[i].page, tests[i].name);
    }
    printf("AccuracyCoin: %u/%u passed, %u failed, %u skipped; %u frames; PC=$%04X\n",
        passed, selected, failed, skipped, frame, nes.cpu.program_counter);
    cartridge_free(nes.cart);
    return done == selected && !failed && !skipped ? 0 : 1;
}
