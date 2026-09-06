#ifndef ROM_PREFERENCES_H
#define ROM_PREFERENCES_H
#include <stdbool.h>
#include <stdint.h>
typedef struct { unsigned scale; bool fullscreen, zapper; } RomPreferences;
// Missing files and disabled overrides resolve to the supplied global defaults.
bool rom_preferences_load(const char *path, const uint32_t identity[3],
                          RomPreferences defaults, RomPreferences *out, bool *overridden);
bool rom_preferences_save(const char *path, const uint32_t identity[3],
                          RomPreferences value, bool overridden);
#endif
