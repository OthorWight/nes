#include "nes_system.h"
#include <string.h>

void nes_init(NES *nes) {
    memset(nes, 0, sizeof(NES));
    cpu_init(&nes->cpu, CPU_MODEL_RICOH_2A03);
    ppu_init(&nes->ppu);
    apu_init(&nes->apu);
}

void nes_reset(NES *nes) {
    nes_reset_zapper_watchdog(nes);
    cpu_trigger_reset(&nes->cpu);
    if (nes->cart && nes->cart->vtable && nes->cart->vtable->reset) {
        nes->cart->vtable->reset(nes->cart);
    }
    nes->lines.irq_line = false;
    nes->lines.nmi_line = false;
    nes->lines.reset_line = false;
    cpu_set_irq_line(&nes->cpu, 0, false);
}

uint8_t nes_cpu_bus_read(NES *nes, uint16_t addr) {
    if (addr <= 0x1FFF) {
        return nes->wram[addr & 0x07FF];
    }

    if (addr >= 0x2000 && addr <= 0x3FFF) {
        return ppu_read_reg(nes, 0x2000 | (addr & 0x0007));
    }

    if (addr >= 0x4000 && addr <= 0x4015) {
        return apu_read_reg(nes, addr);
    }

    if (addr == 0x4016) {
        uint8_t val = 0;
        if (nes->controller_strobe) {
            val = nes->controller_state[0] & 1;
        } else {
            val = nes->controller_shift[0] & 1;
            nes->controller_shift[0] >>= 1;
            nes->controller_shift[0] |= 0x80;
        }
        return val | 0x40;
    }

    if (addr == 0x4017) {
        uint8_t val = 0;
        if (nes->controller_strobe) {
            val = nes->controller_state[1] & 1;
        } else {
            val = nes->controller_shift[1] & 1;
            nes->controller_shift[1] >>= 1;
            nes->controller_shift[1] |= 0x80;
        }

        // Standard controllers do not drive the Zapper light/trigger bits.
        if (!nes->zapper_enabled) {
            return (val & 0x01) | 0x40;
        }

        NES_ZapperWatchdog *watch = &nes->zapper_watchdog;
        if (watch->reads == 0) {
            if (watch->stalled_frames && watch->poll_pc != nes->cpu.program_counter) {
                watch->stalled_frames = 0;
            }
            watch->poll_pc = nes->cpu.program_counter;
        } else if (watch->poll_pc != nes->cpu.program_counter) {
            watch->mixed_pcs = true;
        }
        if (watch->reads < UINT16_MAX) watch->reads++;

        // Update zapper_light based on screen buffer color at (zapper_x, zapper_y)
        bool light_detected = false;
        int x = nes->zapper_x;
        int y = nes->zapper_y;
        if (x >= 0 && x < 256 && y >= 0 && y < 240) {
            uint32_t pixel = nes->ppu.screen_buffer[y * 256 + x];
            uint8_t r = (pixel >> 16) & 0xFF;
            uint8_t g = (pixel >> 8) & 0xFF;
            uint8_t b = pixel & 0xFF;
            if (r > 150 && g > 150 && b > 150) {
                light_detected = true;
            }
        }
        nes->zapper_light = light_detected;

        uint8_t zapper_bit3 = nes->zapper_light ? 0x00 : 0x08;
        uint8_t zapper_bit4 = nes->zapper_trigger ? 0x10 : 0x00;

        return (val & 0x01) | zapper_bit3 | zapper_bit4 | 0x40;
    }

    if (nes->cart && nes->cart->vtable && nes->cart->vtable->cpu_read) {
        bool handled = false;
        uint8_t data = nes->cart->vtable->cpu_read(nes->cart, addr, &handled);
        if (handled) return data;
    }

    return (addr >> 8);
}

void nes_reset_zapper_watchdog(NES *nes) {
    memset(&nes->zapper_watchdog, 0, sizeof(nes->zapper_watchdog));
}

void nes_check_zapper_stall(NES *nes) {
    NES_ZapperWatchdog *watch = &nes->zapper_watchdog;
    // Normal controller reads and short Zapper detection/shot sequences should
    // not warn. Require sustained tight polling at one instruction, with the
    // light bit high and a black screen while rendering remains disabled.
    bool suspect = nes->zapper_enabled && !nes->zapper_trigger &&
        !nes->zapper_light && !(nes->ppu.ppu_mask & 0x18) &&
        watch->reads >= 256 && !watch->mixed_pcs;
    if (suspect) {
        for (size_t i = 0; i < sizeof(nes->ppu.screen_buffer) /
                               sizeof(nes->ppu.screen_buffer[0]); i++) {
            if (nes->ppu.screen_buffer[i] & 0x00FFFFFF) {
                suspect = false;
                break;
            }
        }
    }
    if (!suspect) watch->stalled_frames = 0;
    else if (watch->stalled_frames < 180) watch->stalled_frames++;
    watch->stalled = (watch->stalled_frames >= 180);
    watch->reads = 0;
    watch->mixed_pcs = false;
}

static inline void nes_step_subsystems(NES *nes) {
    for (int p = 0; p < 3; p++) {
        ppu_step(nes);
    }
    apu_step(&nes->apu, nes);
    if (nes->cart && nes->cart->vtable && nes->cart->vtable->clock_m2) {
        nes->cart->vtable->clock_m2(nes->cart);
    }
}

void nes_cpu_bus_write(NES *nes, uint16_t addr, uint8_t data) {
    if (addr <= 0x1FFF) {
        nes->wram[addr & 0x07FF] = data;
        return;
    }

    if (addr >= 0x2000 && addr <= 0x3FFF) {
        ppu_write_reg(nes, 0x2000 | (addr & 0x0007), data);
        return;
    }

    if (addr == 0x4014) {
        uint16_t dma_addr = (uint16_t)(data << 8);
        uint8_t oam_start = nes->ppu.oam_addr;

        // OAM DMA always has one halt cycle, plus one alignment cycle
        // when the $4014 write finishes on an odd CPU cycle.  Capture the
        // parity before advancing the clock, then keep the PPU/APU synchronized
        // for every stalled CPU cycle.
        bool needs_alignment_cycle = (nes->cpu.cycle_count & 1u) != 0;

        nes->cpu.cycle_count++;
        nes_step_subsystems(nes);
        if (needs_alignment_cycle) {
            nes->cpu.cycle_count++;
            nes_step_subsystems(nes);
        }

        for (int i = 0; i < 256; i++) {
            uint8_t val = nes_cpu_bus_read(nes, dma_addr + i);
            nes->cpu.cycle_count++;
            nes_step_subsystems(nes);

            nes->ppu.oam_ram[(oam_start + i) & 0xFF] = val;
            nes->cpu.cycle_count++;
            nes_step_subsystems(nes);
        }
        return;
    }

    if (addr >= 0x4000 && addr <= 0x4015) {
        apu_write_reg(nes, addr, data);
        return;
    }

    if (addr == 0x4016) {
        nes->controller_strobe = (data & 1);
        if (nes->controller_strobe) {
            nes->controller_shift[0] = nes->controller_state[0];
            nes->controller_shift[1] = nes->controller_state[1];
        }
        return;
    }

    if (addr == 0x4017) {
        apu_write_reg(nes, addr, data);
        return;
    }

    if (nes->cart && nes->cart->vtable && nes->cart->vtable->cpu_write) {
        nes->cart->vtable->cpu_write(nes->cart, addr, data);
    }
}

void nes_ppu_bus_set_address(NES *nes, uint16_t addr) {
    addr &= 0x3FFF;
    uint16_t old_addr = nes->ppu.bus_address;
    nes->ppu.bus_address = addr;
    if (nes->cart && nes->cart->vtable && nes->cart->vtable->ppu_addr_change) {
        nes->cart->vtable->ppu_addr_change(nes->cart, old_addr, addr);
    }
}

uint8_t nes_ppu_bus_read(NES *nes, uint16_t addr) {
    addr &= 0x3FFF;
    nes_ppu_bus_set_address(nes, addr);

    if (addr < 0x2000) {
        if (nes->cart && nes->cart->vtable && nes->cart->vtable->ppu_read) {
            bool handled = false;
            uint8_t val = nes->cart->vtable->ppu_read(nes->cart, addr, &handled);
            if (handled) return val;
        }
        return 0;
    }

    if (addr < 0x3F00) {
        bool ciram_ce = true;
        uint16_t mapped = addr;
        if (nes->cart && nes->cart->vtable && nes->cart->vtable->remap_ciram_addr) {
            mapped = nes->cart->vtable->remap_ciram_addr(nes->cart, addr, &ciram_ce);
        } else {
            mapped = cartridge_default_remap_ciram(nes->cart ? nes->cart->mirroring : MIRROR_HORIZONTAL, addr);
        }

        if (ciram_ce) {
            return nes->ciram[mapped & 0x0FFF];
        }
        return (uint8_t)mapped;
    }

    uint16_t pal_addr = addr & 0x001F;
    if (pal_addr == 0x0010 || pal_addr == 0x0014 || pal_addr == 0x0018 || pal_addr == 0x001C) {
        pal_addr &= ~0x0010;
    }
    return nes->ppu.palette_ram[pal_addr];
}

void nes_ppu_bus_write(NES *nes, uint16_t addr, uint8_t data) {
    addr &= 0x3FFF;
    nes_ppu_bus_set_address(nes, addr);

    if (addr < 0x2000) {
        if (nes->cart && nes->cart->vtable && nes->cart->vtable->ppu_write) {
            nes->cart->vtable->ppu_write(nes->cart, addr, data);
        }
        return;
    }

    if (addr < 0x3F00) {
        bool ciram_ce = true;
        uint16_t mapped = addr;
        if (nes->cart && nes->cart->vtable && nes->cart->vtable->remap_ciram_addr) {
            mapped = nes->cart->vtable->remap_ciram_addr(nes->cart, addr, &ciram_ce);
        } else {
            mapped = cartridge_default_remap_ciram(nes->cart ? nes->cart->mirroring : MIRROR_HORIZONTAL, addr);
        }

        if (ciram_ce) {
            nes->ciram[mapped & 0x0FFF] = data;
        }
        return;
    }

    uint16_t pal_addr = addr & 0x001F;
    if (pal_addr == 0x0010 || pal_addr == 0x0014 || pal_addr == 0x0018 || pal_addr == 0x001C) {
        pal_addr &= ~0x0010;
    }
    nes->ppu.palette_ram[pal_addr] = data;
}

// ----------------------------------------------------------------------
// Cycle-accurate CPU/PPU synchronization hooks
// ----------------------------------------------------------------------

// Ticks the PPU exactly 3 times for every 1 CPU cycle, including dummy cycles.
static void nes_cpu_cycle_tick_wrapper(void *context) {
    NES *nes = (NES*)context;
    nes_step_subsystems(nes);
}

// Emits a memory read WITHOUT batching PPU ticks (handled by cycle_tick now)
static uint8_t nes_cpu_bus_read_wrapper(void *context, uint16_t addr) {
    NES *nes = (NES*)context;
    return nes_cpu_bus_read(nes, addr); 
}

// Emits a memory write WITHOUT batching PPU ticks (handled by cycle_tick now)
static void nes_cpu_bus_write_wrapper(void *context, uint16_t addr, uint8_t data) {
    NES *nes = (NES*)context;
    nes_cpu_bus_write(nes, addr, data);
}

void nes_clock_tick(NES *nes) {
    cpu_set_irq_line(&nes->cpu, 0, nes->lines.irq_line);

    CPUBus bus;
    bus.bus_context = nes;
    bus.read = nes_cpu_bus_read_wrapper;
    bus.write = nes_cpu_bus_write_wrapper;
    
    // This locks the PPU execution to the internal CPU sub-cycles (dummy and memory ticks)
    bus.cycle_tick = nes_cpu_cycle_tick_wrapper; 

    cpu_step(&nes->cpu, &bus);
}
