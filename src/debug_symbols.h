#ifndef NES_DEBUG_SYMBOLS_H
#define NES_DEBUG_SYMBOLS_H
#include <stdbool.h>
#include <stdint.h>
enum { DEBUG_SYMBOLS = 1024, DEBUG_SYMBOL_NAME = 48 };
typedef struct { uint16_t address; char name[DEBUG_SYMBOL_NAME]; } DebugSymbol;
typedef struct { unsigned count; DebugSymbol items[DEBUG_SYMBOLS]; } DebugSymbols;
/* Supports ca65/VICE .lbl (al 008000 .Reset) and plain 8000 Reset lines.
   Blank lines and lines beginning # or ; are ignored. Failed imports are atomic. */
bool debug_symbols_load(DebugSymbols *, const char *path);
bool debug_symbols_add(DebugSymbols *, uint16_t address, const char *name);
const char *debug_symbols_name(const DebugSymbols *, uint16_t address);
bool debug_symbols_address(const DebugSymbols *, const char *name, uint16_t *address);
#endif
