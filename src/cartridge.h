#ifndef CARTRIDGE_H
#define CARTRIDGE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NES NES;
typedef struct Cartridge Cartridge;
typedef struct StateIO StateIO;

typedef enum {
    MIRROR_HORIZONTAL,
    MIRROR_VERTICAL,
    MIRROR_FOUR_SCREEN,
    MIRROR_ONE_SCREEN_LOW,
    MIRROR_ONE_SCREEN_HIGH
} MirroringMode;

typedef struct {
    // Explicit field codec; reads operate on a staged cartridge, never the live one.
    void     (*state)(Cartridge *c, StateIO *io);
    size_t   state_size; // In-memory mapper allocation size, not the file layout.

    void     (*reset)(Cartridge *c);
    void     (*destroy)(Cartridge *c);

    uint8_t  (*cpu_read)(Cartridge *c, uint16_t addr, bool *handled);
    void     (*cpu_write)(Cartridge *c, uint16_t addr, uint8_t val);

    uint8_t  (*ppu_read)(Cartridge *c, uint16_t addr, bool *handled);
    void     (*ppu_write)(Cartridge *c, uint16_t addr, uint8_t val);

    void     (*ppu_addr_change)(Cartridge *c, uint16_t old_addr, uint16_t new_addr);
    void     (*ppu_dot)(Cartridge *c, uint16_t addr);
    void     (*clock_m2)(Cartridge *c);

    uint16_t (*remap_ciram_addr)(Cartridge *c, uint16_t addr, bool *ciram_ce);
} MapperInterface;

struct Cartridge {
    const MapperInterface *vtable;
    NES          *nes;

    uint8_t       mapper_id;
    MirroringMode mirroring;

    uint8_t      *prg_rom;
    uint32_t      prg_rom_size;
    uint8_t      *chr_rom;
    uint32_t      chr_rom_size;
    uint8_t      *prg_ram;
    uint32_t      prg_ram_size;

    uint32_t      rom_identity[3]; // Header, initial PRG and CHR CRC32, before writes.
    bool          has_battery;
    bool          battery_save_blocked; // Preserve an unreadable/corrupt existing save.
    char          save_filepath[512];

    void         *mapper_data;
};

Cartridge* cartridge_load(NES *nes, const char *filepath);
bool       cartridge_save_battery(Cartridge *cart);
// A missing save is OK; any other read failure leaves RAM intact and blocks writes.
bool       cartridge_load_battery(Cartridge *cart);
bool       cartridge_set_save_path(Cartridge *cart, const char *path);
// Destruction performs no I/O. Call cartridge_save_battery and check it first.
void       cartridge_free(Cartridge *cart);

uint16_t   cartridge_default_remap_ciram(MirroringMode mode, uint16_t addr);

#ifdef __cplusplus
}
#endif

#endif