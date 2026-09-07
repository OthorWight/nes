#include "state_io.h"
#include <assert.h>
#include <stdio.h>

// Original state-file algorithm: optimized checksums must remain compatible
// with existing saves, ROM identities, preferences and diagnostic captures.
static uint32_t original_crc32(const uint8_t *bytes, size_t size) {
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < size; ++i) {
        crc ^= bytes[i];
        for (unsigned b = 0; b < 8; ++b)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

int main(void) {
    assert(state_crc32(NULL, 0) == 0);
    assert(state_crc32("123456789", 9) == 0xCBF43926u);
    uint8_t bytes[256 * 240 * 4 + 8];
    for (size_t i = 0; i < sizeof(bytes); ++i) bytes[i] = (uint8_t)(i * 37 + i / 251);
    for (unsigned i = 0; i < 256; ++i) {
        uint8_t value = (uint8_t)i;
        assert(state_crc32(&value, 1) == original_crc32(&value, 1));
    }
    for (unsigned offset = 0; offset < 8; ++offset) {
        for (unsigned size = 0; size < 129; ++size)
            assert(state_crc32(bytes + offset, size) == original_crc32(bytes + offset, size));
        assert(state_crc32(bytes + offset, 256 * 240 * 4) ==
               original_crc32(bytes + offset, 256 * 240 * 4));
    }
    puts("CRC-32 known vectors, alignment, tails and framebuffer-sized compatibility passed.");
    return 0;
}
