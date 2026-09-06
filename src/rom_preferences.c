#include "rom_preferences.h"
#include "state_io.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>

bool rom_preferences_load(const char *path, const uint32_t id[3],
                          RomPreferences defaults, RomPreferences *out, bool *overridden) {
    *out = defaults; *overridden = false;
    FILE *f = fopen(path, "rb");
    if (!f) return errno == ENOENT;
    uint8_t data[24];
    bool ok = fread(data, 1, sizeof(data), f) == sizeof(data);
    if (fgetc(f) != EOF || ferror(f)) ok = false;
    if (fclose(f)) ok = false;
    if (!ok || memcmp(data, "NRP1", 4)) return false;
    StateIO io = {data, sizeof(data), 4, true, true};
    for (unsigned i = 0; i < 3; ++i) if (state_u32(&io, 0) != id[i]) return false;
    RomPreferences value;
    value.scale = state_u8(&io, 0);
    value.fullscreen = state_bool(&io, false);
    value.zapper = state_bool(&io, false);
    bool enabled = state_bool(&io, false);
    uint32_t crc = state_u32(&io, 0);
    if (!io.ok || value.scale < 1 || value.scale > 5 || crc != state_crc32(data, 20)) return false;
    if (enabled) { *out = value; *overridden = true; }
    return true;
}
bool rom_preferences_save(const char *path, const uint32_t id[3], RomPreferences value, bool overridden) {
    if (value.scale < 1 || value.scale > 5) return false;
    uint8_t data[24] = {'N','R','P','1'};
    StateIO io = {data, sizeof(data), 4, false, true};
    for (unsigned i = 0; i < 3; ++i) state_u32(&io, id[i]);
    state_u8(&io, (uint8_t)value.scale);
    state_bool(&io, value.fullscreen);
    state_bool(&io, value.zapper);
    state_bool(&io, overridden);
    state_u32(&io, state_crc32(data, 20));
    return io.ok && state_atomic_write(path, data, sizeof(data));
}
