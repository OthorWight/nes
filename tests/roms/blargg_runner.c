#include "nes_system.h"
#include "diagnostics.h"
#include <stdio.h>

// Optional ROM integration test. Poll the test's documented $6000 protocol
// without changing the emulated bus or importing/writing battery saves.
static NES nes;
static uint8_t peek(uint16_t address) {
    uint8_t value = 0;
    (void)nes_cpu_peek(&nes, address, &value);
    return value;
}
static int run(const char *path) {
    nes_init(&nes);
    nes.cart = cartridge_load(&nes, path);
    if (!nes.cart) return 1;
    nes_reset(&nes);
    bool started = false;
    int result = 1;
    for (unsigned frame = 0; frame < 1800; ++frame) {
        nes.frame_ready = false;
        unsigned instructions = 0;
        while (!nes.frame_ready && instructions++ < 100000) nes_clock_tick(&nes);
        if (!nes.frame_ready) break;
        nes.apu.audio_buffer_idx = 0;
        if (peek(0x6001) != 0xDE || peek(0x6002) != 0xB0 || peek(0x6003) != 0x61) continue;
        uint8_t status = peek(0x6000);
        if (status == 0x80) started = true;
        if (started && status < 0x80) {
            printf("%s: result %u\n", path, status);
            for (unsigned address = 0x6004; address < 0x8000; ++address) {
                uint8_t value = peek((uint16_t)address);
                if (!value) break;
                putchar(value);
            }
            putchar('\n');
            result = status ? 1 : 0;
            cartridge_free(nes.cart);
            return result;
        }
    }
    fprintf(stderr, "%s: timed out or did not complete a frame\n", path);
    cartridge_free(nes.cart);
    return result;
}
int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "Usage: %s ROM.nes [ROM.nes ...]\n", argv[0]); return 2; }
    int result = 0;
    for (int i = 1; i < argc; ++i) result |= run(argv[i]);
    return result;
}
