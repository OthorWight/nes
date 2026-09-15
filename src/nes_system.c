#include "nes_system.h"
#include "diagnostics.h"
#include <string.h>

void nes_init(NES *nes) {
    memset(nes, 0, sizeof(NES));
    cpu_init(&nes->cpu, CPU_MODEL_RICOH_2A03);
    ppu_init(&nes->ppu);
    apu_init(&nes->apu);
    nes->region_override = NES_REGION_AUTO;
    expansion_audio_reset(nes);
}

void nes_set_region(NES *nes, unsigned region) {
    if (region > NES_DENDY) region = NES_NTSC;
    if (nes->ppu.region != region) {
        nes->ppu.scanline = nes->ppu.cycle = 0;
        nes->ppu.odd_frame = nes->ppu.odd_skip_rendering = false;
        nes->clock.ppu_divider = 0;
    }
    nes->ppu.region = (uint8_t)region;
    apu_set_region(&nes->apu, region);
}

void nes_reset(NES *nes) {
    unsigned region = nes->region_override;
    if (region == NES_REGION_AUTO) {
        unsigned timing = nes->cart ? nes->cart->info.timing : 0;
        region = timing == 1 ? NES_PAL : timing == 3 ? NES_DENDY : NES_NTSC;
    }
    nes_set_region(nes, region);
    nes->oam_dma_pending = false;
    nes->dmc_dma_pending = false;
    apu_reset(nes);
    expansion_audio_reset(nes);
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

static uint8_t cpu_bus_read_value(NES *nes, uint16_t addr) {
    if (addr <= 0x1FFF) {
        return nes->wram[addr & 0x07FF];
    }

    if (addr >= 0x2000 && addr <= 0x3FFF) {
        return ppu_read_reg(nes, 0x2000 | (addr & 0x0007));
    }

    if (addr == 0x4015) return apu_read_reg(nes, addr);
    if (addr >= 0x4000 && addr < 0x4015) return nes->cpu_open_bus;

    if (addr == 0x4016) {
        uint8_t val = 0;
        if (nes->controller_strobe) {
            val = nes->controller_state[0] & 1;
        } else {
            val = nes->controller_shift[0] & 1;
            nes->controller_shift[0] >>= 1;
            nes->controller_shift[0] |= 0x80;
        }
        return val | (nes->cpu_open_bus & 0xE0);
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
            return (val & 0x01) | (nes->cpu_open_bus & 0xE0);
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

        return (val & 0x01) | zapper_bit3 | zapper_bit4 | (nes->cpu_open_bus & 0xE0);
    }

    uint8_t expansion_value;
    if (expansion_audio_read(nes, addr, &expansion_value)) return expansion_value;
    if (nes->cart && nes->cart->vtable && nes->cart->vtable->cpu_read) {
        bool handled = false;
        uint8_t data = nes->cart->vtable->cpu_read(nes->cart, addr, &handled);
        if (handled) return data;
    }

    return nes->cpu_open_bus;
}

uint8_t nes_cpu_bus_read(NES *nes, uint16_t addr) {
    uint8_t value = cpu_bus_read_value(nes, addr);
    expansion_audio_observe_read(nes, addr, value);
    if (nes->diagnostics && (addr == 0x4016 || addr == 0x4017)) {
        ++nes->diagnostics->polls[addr - 0x4016];
        diagnostics_event(nes, DIAG_INPUT_READ, addr, value);
    }
    if (nes->diagnostics && nes->diagnostics->tracing) diagnostics_lines(nes);
    // $4015 is internal to the CPU and does not drive the external data bus.
    if (addr != 0x4015) nes->cpu_open_bus = value;
    return value;
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
    bool nmi_before = nes->cpu.nmi_line;
    unsigned dots = 3;
    if (nes->ppu.region == NES_PAL && ++nes->clock.ppu_divider == 5) {
        nes->clock.ppu_divider = 0;
        dots = 4; // PAL master clock: CPU /16, PPU /5.
    }
    for (unsigned p = 0; p < dots; p++) {
        ppu_step(nes);
        // For this CPU/PPU alignment the NMI poll boundary falls after
        // the first dot. An edge here belongs to the preceding poll cycle;
        // later edges on an instruction's final cycle must be deferred.
        if (p == 0 && !nmi_before && nes->cpu.nmi_line && nes->cpu.cycle_count)
            nes->cpu.nmi_pulsed_cycle = nes->cpu.cycle_count - 1;
        // IRQ uses the same poll boundary. Include a mapper edge driven by
        // the first PPU dot, before the CPU's final-cycle status changes.
        if (p == 0)
            nes->cpu.irq_pending = nes->cpu.irq_lines &&
                !(nes->cpu.status_flags & FLAG_INTERRUPT_DISABLE);
    }
    apu_step(&nes->apu, nes);
    if (nes->cart && nes->cart->vtable && nes->cart->vtable->clock_m2) {
        nes->cart->vtable->clock_m2(nes->cart);
    }
    if (nes->diagnostics && nes->diagnostics->tracing) diagnostics_lines(nes);
}

void nes_request_dmc_dma(NES *nes, bool load) {
    if (!nes || nes->dmc_dma_pending) return;
    nes->dmc_dma_pending = true;
    uint64_t cycle = nes->cpu.cycle_count;
    // This machine powers on with even get / odd put cycles. Load DMA
    // halts on the second following APU get; reload DMA halts on a put.
    nes->dmc_dma_cycle = load ? cycle + ((cycle & 1) ? 3 : 4)
                              : cycle + ((cycle & 1) ? 0 : 1);
}

static void dma_clock(NES *nes) {
    nes->cpu.cycle_count++;
    nes_step_subsystems(nes);
}

static void dma_repeat_read(NES *nes, uint16_t address) {
    // The 2A07 gates controller strobes during DMA. The interrupted read
    // still happens once when the CPU resumes; NTSC retains the extra clocks.
    if (nes->apu.region == NES_PAL && (address == 0x4016 || address == 0x4017)) return;
    (void)nes_cpu_bus_read(nes, address);
}

// Called only on a CPU read, after its clock has advanced. DMA cannot halt
// writes. Once halted, OAM transfers and DMC setup share cycles; only DMC's
// get takes the bus away from OAM. No CPU instruction executes in this loop.
static void nes_run_dma(NES *nes, uint16_t cpu_address) {
    while (nes->oam_dma_pending ||
           (nes->dmc_dma_pending && nes->cpu.cycle_count >= nes->dmc_dma_cycle)) {
        bool oam = nes->oam_dma_pending;
        uint16_t source = (uint16_t)nes->oam_dma_page << 8;
        uint8_t start = nes->ppu.oam_addr;
        unsigned index = 0;
        uint8_t value = 0;
        bool have_value = false;
        unsigned dmc_phase = (nes->dmc_dma_pending &&
            nes->cpu.cycle_count >= nes->dmc_dma_cycle) ? 1 : 0;
        nes->oam_dma_pending = false;
        dma_repeat_read(nes, cpu_address);
        while (oam || dmc_phase) {
            dma_clock(nes);
            bool get = !(nes->cpu.cycle_count & 1);
            bool bus_used = false;
            if (dmc_phase == 1) {
                dmc_phase = 2; // Dummy cycle; OAM can still transfer.
            } else if (dmc_phase == 2 && get) {
                apu_dmc_dma_complete(&nes->apu, nes);
                nes->dmc_dma_pending = false;
                dmc_phase = 0;
                bus_used = true;
            } else if (!dmc_phase && nes->dmc_dma_pending &&
                       nes->cpu.cycle_count >= nes->dmc_dma_cycle) {
                dmc_phase = 1; // DMC halt overlaps the already halted CPU.
            }
            if (oam && !bus_used) {
                if (get && !have_value) {
                    value = nes_cpu_bus_read(nes, source + index);
                    have_value = true;
                    bus_used = true;
                } else if (!get && have_value) {
                    nes->ppu.oam_ram[(start + index) & 255] = value;
                    have_value = false;
                    bus_used = true;
                    if (++index == 256) oam = false;
                }
            }
            if (!bus_used) dma_repeat_read(nes, cpu_address);
        }
        dma_clock(nes); // The CPU finally completes its interrupted read.
    }
}

static void cpu_bus_write_value(NES *nes, uint16_t addr, uint8_t data) {
    nes->cpu_open_bus = data;
    if (addr <= 0x1FFF) {
        nes->wram[addr & 0x07FF] = data;
        return;
    }

    if (addr >= 0x2000 && addr <= 0x3FFF) {
        ppu_write_reg(nes, 0x2000 | (addr & 0x0007), data);
        return;
    }

    if (addr == 0x4014) {
        nes->oam_dma_page = data;
        nes->oam_dma_pending = true;
        return;
    }

    if (addr >= 0x4000 && addr <= 0x4015) {
        apu_write_reg(nes, addr, data);
        return;
    }

    if (addr == 0x4016) {
        bool was_high = nes->controller_strobe != 0;
        nes->controller_strobe = (data & 1);
        if (was_high || nes->controller_strobe) {
            nes->controller_shift[0] = nes->controller_state[0];
            nes->controller_shift[1] = nes->controller_state[1];
        }
        return;
    }

    if (addr == 0x4017) {
        apu_write_reg(nes, addr, data);
        return;
    }

    if (expansion_audio_write(nes, addr, data)) return;
    if (nes->cart && nes->cart->vtable && nes->cart->vtable->cpu_write) {
        nes->cart->vtable->cpu_write(nes->cart, addr, data);
    }
}

void nes_cpu_bus_write(NES *nes, uint16_t addr, uint8_t data) {
    if (nes->diagnostics) {
        if (addr >= 0x2000 && addr < 0x4000)
            diagnostics_event(nes, DIAG_PPU_WRITE, 0x2000 | (addr & 7), data);
        else if (addr >= 0x4020 && nes->cart)
            diagnostics_event(nes, DIAG_MAPPER_WRITE, addr, data);
        else if (addr == 0x4016 && nes->controller_strobe && !(data & 1)) {
            ++nes->diagnostics->latches;
            diagnostics_event(nes, DIAG_INPUT_LATCH, addr, nes->controller_state[0]);
        }
    }
    cpu_bus_write_value(nes, addr, data);
    if (nes->diagnostics && nes->diagnostics->tracing) diagnostics_lines(nes);
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

static void nes_cpu_cycle_tick_wrapper(void *context) {
    NES *nes = (NES*)context;
    nes_step_subsystems(nes);
}

static uint8_t nes_cpu_bus_read_wrapper(void *context, uint16_t addr) {
    NES *nes = (NES*)context;
    nes_run_dma(nes, addr);
    return nes_cpu_bus_read(nes, addr); 
}

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
    
    bus.cycle_tick = nes_cpu_cycle_tick_wrapper;

    cpu_step(&nes->cpu, &bus);
}
