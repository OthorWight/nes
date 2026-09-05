#ifndef NES_SYSTEM_H
#define NES_SYSTEM_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct NES NES;
typedef struct Cartridge Cartridge;

typedef struct {
    bool irq_line;      // Low-active wire-OR line (Cartridge + APU)
    bool nmi_line;      // Low-active edge-sensitive line (PPU -> CPU)
    bool reset_line;    // Active low
    bool rw_line;       // 1 = Read, 0 = Write
} NES_Lines;

typedef struct {
    uint64_t master_ticks;
    uint32_t cpu_divider;
    uint32_t ppu_divider;
} NES_Clock;

typedef struct {
    uint16_t reads;
    uint16_t poll_pc;
    uint16_t stalled_frames;
    bool mixed_pcs;
    bool stalled;
} NES_ZapperWatchdog;

#include "cpu6502.h"
#include "ppu2c02.h"
#include "apu2a03.h"
#include "cartridge.h"

struct NES {
    CPU6502    cpu;
    PPU2C02    ppu;
    APU2A03    apu;
    Cartridge *cart;

    NES_Clock  clock;
    NES_Lines  lines;

    // Physical RAM on board
    uint8_t    wram[2048];       // 2KB CPU Internal RAM ($0000-$07FF mirrored)
    uint8_t    ciram[4096];      // 4KB PPU Internal Nametable RAM (Expanded to support 4-screen mirroring natively)

    // Controller & Open Bus State
    uint8_t    cpu_open_bus;
    uint8_t    controller_state[2];
    uint8_t    controller_shift[2];
    uint8_t    controller_strobe;

    // Zapper Light Gun State
    bool       zapper_enabled; // Port 2 light gun; false selects standard controllers
    bool       zapper_trigger;
    bool       zapper_light;
    int        zapper_x;
    int        zapper_y;
    NES_ZapperWatchdog zapper_watchdog;

    // Frame completion flag for frontend vsync
    bool       frame_ready;
};

// Bus Interface
uint8_t nes_cpu_bus_read(NES *nes, uint16_t addr);
void    nes_cpu_bus_write(NES *nes, uint16_t addr, uint8_t data);
uint8_t nes_ppu_bus_read(NES *nes, uint16_t addr);
void    nes_ppu_bus_write(NES *nes, uint16_t addr, uint8_t data);
void    nes_ppu_bus_set_address(NES *nes, uint16_t addr);

// Clock Driver
void    nes_init(NES *nes);
void    nes_reset(NES *nes);
void    nes_clock_tick(NES *nes);
// Call once per completed frame; this detects a suspected polling hang.
void    nes_check_zapper_stall(NES *nes);
void    nes_reset_zapper_watchdog(NES *nes);

#endif
