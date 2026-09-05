#ifndef STATE_IO_H
#define STATE_IO_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Little-endian field stream. NULL output counts bytes without storing them.
   The caller must only decode into temporary storage until ok is checked. */
typedef struct StateIO {
    uint8_t *data;
    size_t size;
    size_t pos;
    bool reading;
    bool ok;
} StateIO;
void state_bytes(StateIO *io, uint8_t *data, size_t size);
uint8_t state_u8(StateIO *io, uint8_t value);
uint16_t state_u16(StateIO *io, uint16_t value);
uint32_t state_u32(StateIO *io, uint32_t value);
uint64_t state_u64(StateIO *io, uint64_t value);
int state_i32(StateIO *io, int value);
int16_t state_i16(StateIO *io, int16_t value);
bool state_bool(StateIO *io, bool value);
unsigned state_enum(StateIO *io, unsigned value);
float state_f32(StateIO *io, float value);
double state_f64(StateIO *io, double value);
uint32_t state_crc32(const void *data, size_t size);
/* Same-directory temporary file, flushed/closed before replacement. */
bool state_atomic_write(const char *path, const void *data, size_t size);
#endif
