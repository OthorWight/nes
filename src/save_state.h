#ifndef SAVE_STATE_H
#define SAVE_STATE_H
#include "nes_system.h"

#define NES_STATE_VERSION 3u
#define NES_STATE_HEADER_SIZE 32u
#define NES_STATE_MAX_SIZE (16u * 1024u * 1024u)

typedef enum {
    NES_STATE_OK, NES_STATE_NO_CART, NES_STATE_MAPPER,
    NES_STATE_OPEN, NES_STATE_IO, NES_STATE_VERSION_ERROR, NES_STATE_LEGACY,
    NES_STATE_WRONG_ROM, NES_STATE_CORRUPT, NES_STATE_MEMORY
} NES_StateResult;

/* Call between nes_clock_tick calls (CPU instruction or stall boundaries).
   The returned buffer belongs to the caller. No host pointers are encoded. */
NES_StateResult nes_state_encode(NES *nes, uint8_t **data, size_t *size);
NES_StateResult nes_state_decode(NES *nes, const uint8_t *data, size_t size);
NES_StateResult nes_state_save(NES *nes, const char *path);
NES_StateResult nes_state_load(NES *nes, const char *path);
const char *nes_state_message(NES_StateResult result);
#endif
