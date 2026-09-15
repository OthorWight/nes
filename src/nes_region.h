#ifndef NES_REGION_H
#define NES_REGION_H
#include <stdint.h>
enum { NES_NTSC, NES_PAL, NES_DENDY, NES_REGION_AUTO };
static inline uint32_t nes_region_cpu_hz(unsigned region) {
    return region == NES_PAL ? 1662607u : region == NES_DENDY ? 1773448u : 1789773u;
}
static inline const char *nes_region_name(unsigned region) {
    return region == NES_PAL ? "PAL" : region == NES_DENDY ? "Dendy" : "NTSC";
}
#endif
