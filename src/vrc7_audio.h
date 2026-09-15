#ifndef VRC7_AUDIO_H
#define VRC7_AUDIO_H
#include <stdbool.h>
#include "../third_party/emu2413/emu2413.h"
typedef struct StateIO StateIO;
typedef struct {
    OPLL core;
    bool initialized, disabled;
    uint8_t address, divider;
    int16_t output;
} Vrc7Audio;
void vrc7_audio_write(Vrc7Audio *a, bool data, uint8_t value, unsigned cpu_hz);
float vrc7_audio_clock(Vrc7Audio *a, unsigned cpu_hz);
void vrc7_audio_state(Vrc7Audio *a, StateIO *io);
#endif
