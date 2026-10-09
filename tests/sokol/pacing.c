/* Optional real-ROM pacing probe. Run from an isolated output directory so
   emulator preferences, battery files and diagnostics stay away from user saves. */
#include "../../src/host.h"
#include <assert.h>
static bool pacing_poll(HostEvent *);
#define host_poll_event pacing_poll
#define sokol_main emulator_desc
#include "../../src/gui_main.c"
#undef sokol_main
#undef host_poll_event

static const char *rom_path;
static unsigned iteration, frames=1800;
static bool delivered;
static double measured_start;
static uint64_t measured_cycle;
static uint64_t measured_frame;
static unsigned initial_empty, initial_trims;
static unsigned stall_ms;
static unsigned reported_empty, reported_trims;

static bool pacing_poll(HostEvent *event) {
    if(delivered) { delivered=false; ++iteration; return false; }
    delivered=true; host_zero(*event);
    if(iteration>120 && (reported_empty!=audio_monitor.underruns || reported_trims!=audio_monitor.trims)) {
        reported_empty=audio_monitor.underruns; reported_trims=audio_monitor.trims;
        char path[128]; snprintf(path,sizeof(path),"rom-audio-event-%u-%u.log",reported_empty,reported_trims);
        assert(diagnostics_write(&nes_sys,path,rom_path,reported_empty,reported_trims,audio_monitor.errors,audio_device_ms));
        printf("Audio event callback=%u frame=%llu empty=%u trims=%u debt=%.2fms capture=%s\n",
            iteration,(unsigned long long)diagnostics.frame,reported_empty,reported_trims,
            (host_time()-frame_scheduler.deadline)*1000,path);
        fflush(stdout);
    }
    if(stall_ms && iteration>120 && iteration%120==0) host_delay(stall_ms);
    if(iteration==0) {
        assert(frontend_load_rom(rom_path));
        performance_visible=true; debug_panel_enabled=false;
        nametable_viewer_enabled=apu_viewer_enabled=false;
        crt_enabled=true; host_set_crt(true);
        draw_debug_panel(); apply_display();
    }
    if(iteration==120) {
        measured_start=host_time(); measured_cycle=nes_sys.cpu.cycle_count;
        measured_frame=diagnostics.frame;
        initial_empty=audio_monitor.underruns; initial_trims=audio_monitor.trims;
        reported_empty=initial_empty; reported_trims=initial_trims;
    }
    /* Start/title confirmations through the normal host input path. */
    if(iteration==180 || iteration==360 || iteration==720 || iteration==900) {
        event->type=HOST_KEYDOWN; event->key.keysym.sym=control_mappings[3];
    } else if(iteration==182 || iteration==362 || iteration==722 || iteration==902) {
        event->type=HOST_KEYUP; event->key.keysym.sym=control_mappings[3];
    }
    if(iteration==120+frames) {
        double elapsed=host_time()-measured_start;
        double speed=(double)(nes_sys.cpu.cycle_count-measured_cycle)/nes_region_cpu_hz(nes_sys.apu.region)/elapsed*100;
        unsigned empty=audio_monitor.underruns-initial_empty, trims=audio_monitor.trims-initial_trims;
        DiagnosticSummary summary=diagnostics_summary(&diagnostics);
        printf("ROM pacing callbacks=%u frames=%llu size=%dx%d CRT=on stall=%ums FPS=%.2f speed=%.2f%% max=%.2fms empty=%u trims=%u errors=%u\n",
            frames,(unsigned long long)(diagnostics.frame-measured_frame),sapp_width(),sapp_height(),stall_ms,
            (diagnostics.frame-measured_frame)/elapsed,speed,summary.max_ms,empty,trims,audio_monitor.errors);
        fflush(stdout);
        assert(diagnostics_write(&nes_sys,"rom-pacing.log",rom_path,empty,trims,audio_monitor.errors,audio_device_ms));
        assert(speed>95 && speed<105 && !audio_monitor.errors);
        if(audio_device) assert(!empty && !trims);
        event->type=HOST_QUIT;
    }
    return true;
}

sapp_desc sokol_main(int argc, char **argv) {
    assert(argc>=2 && argc<=3);
    rom_path=argv[1];
    if(argc==3) {
        char *end; unsigned long count=strtoul(argv[2],&end,10);
        assert(*argv[2] && !*end && count>=120 && count<=36000); frames=(unsigned)count;
    }
    const char *stall=getenv("NES_SOKOL_PACING_STALL_MS");
    if(stall) {
        char *end; unsigned long delay=strtoul(stall,&end,10);
        assert(*stall && !*end && delay<=40); stall_ms=(unsigned)delay;
    }
    sapp_desc desc=emulator_desc(argc,argv);
    global_preferences=(RomPreferences){4,false,false}; window_scale=4; fullscreen=false;
    desc.event_cb=NULL; desc.width=1792; desc.height=1040; desc.fullscreen=false;
    return desc;
}
