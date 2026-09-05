#include "cartridge.h"
#include <stdio.h>
#include <string.h>

static bool fail(char *error, size_t size, const char *message) {
    if (error && size) snprintf(error, size, "%s", message);
    return false;
}

// Checked in 64 bits before narrowing or allocating. Limits are emulator
// resource limits, not limits imposed by the NES 2.0 file format.
static bool rom_size(uint8_t low, uint8_t high, uint32_t unit,
                     uint32_t limit, uint32_t *out) {
    uint64_t bytes;
    if (high != 15) bytes = ((uint32_t)high * 256 + low) * (uint64_t)unit;
    else {
        unsigned exponent = low >> 2;
        unsigned multiplier = (low & 3) * 2 + 1;
        if (exponent >= 32) return false;
        bytes = (UINT64_C(1) << exponent) * multiplier;
    }
    if (bytes > limit) return false;
    *out = (uint32_t)bytes;
    return true;
}
static uint32_t ram_size(unsigned shift) { return shift ? 64u << shift : 0; }

bool cartridge_parse_header(const uint8_t h[16], CartridgeInfo *out,
                            char *error, size_t error_size) {
    CartridgeInfo i = {0};
    if (error && error_size) *error = '\0';
    if (!h || !out || memcmp(h, "NES\x1A", 4)) return fail(error, error_size, "Invalid NES header");
    if ((h[7] & 0x0C) != 0 && (h[7] & 0x0C) != 8)
        return fail(error, error_size, "Unknown NES header format");
    i.nes2 = (h[7] & 0x0C) == 8;
    i.mapper_id = (uint16_t)((h[6] >> 4) | (h[7] & 0xF0));
    i.mirroring = (h[6] & 8) ? MIRROR_FOUR_SCREEN : ((h[6] & 1) ? MIRROR_VERTICAL : MIRROR_HORIZONTAL);
    i.battery = (h[6] & 2) != 0;
    i.trainer = (h[6] & 4) != 0;
    if (h[7] & 3) return fail(error, error_size, "Console type unsupported");
    if (i.nes2) {
        i.mapper_id |= (uint16_t)(h[8] & 15) << 8;
        i.submapper = h[8] >> 4;
        if (!rom_size(h[4], h[9] & 15, 16384, 64u * 1024 * 1024, &i.prg_rom_size) ||
            !rom_size(h[5], h[9] >> 4, 8192, 8u * 1024 * 1024, &i.chr_rom_size))
            return fail(error, error_size, "ROM exceeds size limit");
        i.prg_ram_size = ram_size(h[10] & 15);
        i.prg_nvram_size = ram_size(h[10] >> 4);
        i.chr_ram_size = ram_size(h[11] & 15);
        i.chr_nvram_size = ram_size(h[11] >> 4);
        i.timing = h[12] & 3;
        if ((h[12] & 0xFC) || h[13] || (h[14] & 0xFC) || (h[15] & 0xC0))
            return fail(error, error_size, "Invalid NES 2.0 reserved bits");
        if (h[14] & 3) return fail(error, error_size, "Miscellaneous ROM unsupported");
        // Expansion device metadata is advisory; selection stays in Settings.
        if ((i.prg_nvram_size || i.chr_nvram_size) && !i.battery)
            return fail(error, error_size, "NVRAM requires battery flag");
    } else {
        i.prg_rom_size = h[4] * 16384u;
        i.chr_rom_size = h[5] * 8192u;
        uint32_t ram = h[8] ? h[8] * 8192u : (i.mapper_id == 5 ? 65536u : 8192u);
        if (i.battery) i.prg_nvram_size = ram;
        else i.prg_ram_size = ram;
        if (!i.chr_rom_size) i.chr_ram_size = 8192;
        i.timing = h[9] & 1;
    }
    if (!i.prg_rom_size) return fail(error, error_size, "PRG ROM is empty");
    *out = i;
    return true;
}
