#ifndef SAVE_FIXTURE_H
#define SAVE_FIXTURE_H
#include "nes_system.h"
#include "state_io.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#include <process.h>
#define SAVE_GETPID _getpid
#define SAVE_MKDIR(p) _mkdir(p)
#define SAVE_RMDIR _rmdir
#else
#include <unistd.h>
#define SAVE_GETPID getpid
#define SAVE_MKDIR(p) mkdir(p, 0700)
#define SAVE_RMDIR rmdir
#endif

static char fixture_dir[256];
static void fixture_start(const char *name) {
    snprintf(fixture_dir, sizeof(fixture_dir), "build/tests/%s-%ld", name, (long)SAVE_GETPID());
    assert(SAVE_MKDIR(fixture_dir) == 0);
}
static void fixture_path(char path[512], const char *name) {
    snprintf(path, 512, "%s/%s", fixture_dir, name);
}
static void fixture_rom(const char *path, unsigned mapper, bool battery, bool chr_ram, uint8_t variant) {
    // Synthetic data only. Every 8KB PRG bank has interrupt vectors into a
    // RAM RTI handler so tests can bank-switch freely while executing in WRAM.
    uint8_t header[16] = {'N','E','S',0x1A,8,8,0,0,0,0,0,0,0,0,0,0};
    header[5] = chr_ram ? 0 : 8;
    header[6] = (uint8_t)((mapper << 4) | (battery ? 2 : 0) | 1);
    header[7] = (uint8_t)(mapper & 0xF0);
    FILE *f = fopen(path, "wb");
    assert(f);
    assert(fwrite(header, 1, 16, f) == 16);
    uint8_t bank[8192];
    for (unsigned i = 0; i < 16; ++i) {
        memset(bank, (uint8_t)(i ^ variant), sizeof(bank));
        bank[8192 - 6] = bank[8192 - 4] = bank[8192 - 2] = 0;
        bank[8192 - 5] = bank[8192 - 3] = bank[8192 - 1] = 1;
        assert(fwrite(bank, 1, sizeof(bank), f) == sizeof(bank));
    }
    if (!chr_ram) {
        for (unsigned i = 0; i < 8; ++i) {
            for (unsigned j = 0; j < sizeof(bank); ++j) bank[j] = (uint8_t)(i * 19 + j + variant);
            assert(fwrite(bank, 1, sizeof(bank), f) == sizeof(bank));
        }
    }
    assert(fclose(f) == 0);
}
static NES *fixture_load(const char *path) {
    NES *n = malloc(sizeof(*n));
    assert(n);
    nes_init(n);
    n->cart = cartridge_load(n, path);
    assert(n->cart);
    return n;
}
static void fixture_free(NES *n) {
    cartridge_free(n->cart);
    free(n);
}
#endif
