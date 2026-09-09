#define _POSIX_C_SOURCE 200809L
#include "state_io.h"
#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#include <sys/stat.h>
#else
#include <unistd.h>
#include <fcntl.h>
#endif

_Static_assert(sizeof(float) == 4 && FLT_RADIX == 2 && FLT_MANT_DIG == 24,
               "States require IEEE binary32 floats");
_Static_assert(sizeof(double) == 8 && DBL_MANT_DIG == 53,
               "States require IEEE binary64 doubles");
_Static_assert(INT_MAX >= INT32_MAX, "States require at least 32-bit int");

void state_bytes(StateIO *io, uint8_t *data, size_t size) {
    if (!io->ok || io->pos > io->size || size > io->size - io->pos) {
        io->ok = false;
        return;
    }
    if (size && io->data) {
        if (io->reading) memcpy(data, io->data + io->pos, size);
        else memcpy(io->data + io->pos, data, size);
    }
    io->pos += size;
}
static uint64_t integer(StateIO *io, uint64_t value, unsigned size) {
    uint8_t bytes[8] = {0};
    if (!io->reading) for (unsigned i = 0; i < size; ++i) bytes[i] = (uint8_t)(value >> (8 * i));
    state_bytes(io, bytes, size);
    if (io->reading) {
        value = 0;
        for (unsigned i = 0; i < size; ++i) value |= (uint64_t)bytes[i] << (8 * i);
    }
    return value;
}
uint8_t state_u8(StateIO *io, uint8_t v) { return (uint8_t)integer(io, v, 1); }
uint16_t state_u16(StateIO *io, uint16_t v) { return (uint16_t)integer(io, v, 2); }
uint32_t state_u32(StateIO *io, uint32_t v) { return (uint32_t)integer(io, v, 4); }
uint64_t state_u64(StateIO *io, uint64_t v) { return integer(io, v, 8); }
int state_i32(StateIO *io, int v) {
    uint32_t bits = state_u32(io, (uint32_t)v);
    return bits <= INT32_MAX ? (int)bits : (int)((int64_t)bits - INT64_C(4294967296));
}
int16_t state_i16(StateIO *io, int16_t v) {
    uint16_t bits = state_u16(io, (uint16_t)v);
    return (int16_t)(bits <= INT16_MAX ? (int)bits : (int)bits - 65536);
}
bool state_bool(StateIO *io, bool v) {
    uint8_t b = state_u8(io, v ? 1 : 0);
    if (b > 1) io->ok = false;
    return b != 0;
}
unsigned state_enum(StateIO *io, unsigned v) { return state_u32(io, v); }
float state_f32(StateIO *io, float v) {
    uint32_t bits;
    memcpy(&bits, &v, 4);
    bits = state_u32(io, bits);
    memcpy(&v, &bits, 4);
    if (!isfinite(v)) io->ok = false;
    return v;
}
double state_f64(StateIO *io, double v) {
    uint64_t bits;
    memcpy(&bits, &v, 8);
    bits = state_u64(io, bits);
    memcpy(&v, &bits, 8);
    if (!isfinite(v)) io->ok = false;
    return v;
}
// Slicing-by-8 removes the byte-at-a-time dependency chain when checksumming
// the 240 KiB framebuffer. The IEEE polynomial and save-file results are unchanged.
#include "crc32_table.h"

uint32_t state_crc32(const void *data, size_t size) {
    const uint8_t *bytes = data;
    uint32_t crc = UINT32_MAX;
    while (size >= 8) {
        // Explicit little-endian assembly permits unaligned input on any host
        // without aliasing violations or reading past the end of the buffer.
        uint32_t word = crc ^ ((uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 |
                              (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24);
        crc = crc32_table[7][word & 0xFFu] ^
              crc32_table[6][(word >> 8) & 0xFFu] ^
              crc32_table[5][(word >> 16) & 0xFFu] ^
              crc32_table[4][word >> 24] ^
              crc32_table[3][bytes[4]] ^ crc32_table[2][bytes[5]] ^
              crc32_table[1][bytes[6]] ^ crc32_table[0][bytes[7]];
        bytes += 8;
        size -= 8;
    }
    while (size--) crc = (crc >> 8) ^ crc32_table[0][(crc ^ *bytes++) & 0xFFu];
    return ~crc;
}

bool state_atomic_write(const char *path, const void *data, size_t size) {
    if (!path || !*path || (!data && size)) return false;
    size_t len = strlen(path);
    char *temp = malloc(len + 40);
    if (!temp) return false;
    FILE *f = NULL;
#ifdef _WIN32
    // Exclusive creation prevents competing instances from sharing a temporary file.
    for (unsigned i = 0; i < 100; ++i) {
        snprintf(temp, len + 40, "%s.tmp.%lu.%u", path, (unsigned long)GetCurrentProcessId(), i);
        int fd = _open(temp, _O_CREAT | _O_EXCL | _O_WRONLY | _O_BINARY, _S_IREAD | _S_IWRITE);
        if (fd >= 0) {
            f = _fdopen(fd, "wb");
            if (!f) { _close(fd); remove(temp); }
            break;
        }
    }
#else
    snprintf(temp, len + 40, "%s.tmp.XXXXXX", path);
    int fd = mkstemp(temp);
    if (fd >= 0) {
        f = fdopen(fd, "wb");
        if (!f) { close(fd); remove(temp); }
    }
#endif
    if (!f) { free(temp); return false; }
    bool ok = fwrite(data, 1, size, f) == size && fflush(f) == 0;
#ifdef _WIN32
    if (ok && _commit(_fileno(f)) != 0) ok = false;
#else
    if (ok && fsync(fileno(f)) != 0) ok = false;
#endif
    if (fclose(f) != 0) ok = false;
#ifdef _WIN32
    if (ok) ok = MoveFileExA(temp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    if (ok) ok = rename(temp, path) == 0;
    if (ok) {
        // Persist the rename as well as the file contents. Failure is reported,
        // although the complete replacement has already become visible.
        char *slash = strrchr(temp, '/');
        if (slash) { if (slash == temp) slash[1] = '\0'; else *slash = '\0'; }
        int dir = open(slash ? temp : ".", O_RDONLY);
        if (dir < 0) ok = false;
        else { if (fsync(dir) != 0) ok = false; close(dir); }
        free(temp);
        return ok;
    }
#endif
    if (!ok) remove(temp);
    free(temp);
    return ok;
}
