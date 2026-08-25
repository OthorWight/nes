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

typedef struct Cartridge Cartridge;

struct Cartridge {
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

    uint8_t mapper_state[32];
    bool cpu_clocked_irq;
    uint64_t last_write_cycle;

    // MMC5 State
    uint8_t  exram[1024];
    uint8_t  mmc5_prg_mode;
    uint8_t  mmc5_chr_mode;
    uint8_t  mmc5_ram_protect[2];
    uint8_t  mmc5_exram_mode;
    uint8_t  mmc5_nametable_ctrl;
    uint8_t  mmc5_fill_tile;
    uint8_t  mmc5_fill_attr;
    uint8_t  mmc5_prg_regs[5];       // $5113-$5117
    uint16_t mmc5_chr_regs_a[8];     // $5120-$5127 (10-bit banks)
    uint16_t mmc5_chr_regs_b[4];     // $5128-$512B (10-bit banks)
    uint8_t  mmc5_chr_high;          // $5130
    uint8_t  mmc5_mult_a;            // $5205
    uint8_t  mmc5_mult_b;            // $5206
    uint8_t  mmc5_irq_target;        // $5203
    bool     mmc5_irq_enabled;       // $5204 (write bit 7)
    bool     mmc5_irq_pending;       // $5204 (read bit 7)
    bool     mmc5_in_frame;          // $5204 (read bit 6)
    uint8_t  mmc5_scanline;
    bool     mmc5_last_chr_a;

    bool ppu_sprite_fetch;
    bool ppu_sprite_size_8x16;
    void *cpu_context;
};

Cartridge* cartridge_load(const char *filepath);
void cartridge_free(Cartridge *cart);
void cartridge_save_battery(Cartridge *cart);

#endif