#ifndef CARTRIDGE_H
#define CARTRIDGE_H

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    MIRROR_HORIZONTAL,
    MIRROR_VERTICAL,
    MIRROR_FOUR_SCREEN,
    MIRROR_ONE_SCREEN_LOW,
    MIRROR_ONE_SCREEN_HIGH
} MirroringMode;

typedef struct {
    uint8_t *prg_rom;
    uint32_t prg_rom_size;

    uint8_t *chr_rom;
    uint32_t chr_rom_size;

    uint8_t *prg_ram;
    uint32_t prg_ram_size;
    bool has_battery;
    char save_filepath[512];

    uint8_t mapper_id;
    MirroringMode mirroring;

    uint8_t (*read_prg)(void *cart, uint16_t address);
    void (*write_prg)(void *cart, uint16_t address, uint8_t data);
    uint8_t (*read_chr)(void *cart, uint16_t address);
    void (*write_chr)(void *cart, uint16_t address, uint8_t data);
    void (*clock_irq)(void *cart, void *cpu);
    void (*reset_irq)(void *cart);

    uint8_t mapper_state[16];

    bool ppu_sprite_fetch;
    bool ppu_sprite_size_8x16;
} Cartridge;

Cartridge* cartridge_load(const char *filepath);
void cartridge_free(Cartridge *cart);
void cartridge_save_battery(Cartridge *cart);

#endif