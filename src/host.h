#ifndef NES_HOST_H
#define NES_HOST_H
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "../third_party/sokol/sokol_app.h"

/* Stable persisted key/button IDs, compatible with existing settings files. */
typedef int32_t HostKey;
#define HOST_KEY_UNKNOWN 0
#define HOST_KEY_BACKSPACE 8
#define HOST_KEY_RETURN 13
#define HOST_KEY_ESCAPE 27
#define HOST_KEY_SPACE 32
#define HOST_KEY_f 'f'
#define HOST_KEY_x 'x'
#define HOST_KEY_z 'z'
#define HOST_KEY_F1 (0x40000000 + 58)
#define HOST_KEY_F2 (HOST_KEY_F1 + 1)
#define HOST_KEY_F3 (HOST_KEY_F1 + 2)
#define HOST_KEY_F4 (HOST_KEY_F1 + 3)
#define HOST_KEY_F5 (HOST_KEY_F1 + 4)
#define HOST_KEY_F6 (HOST_KEY_F1 + 5)
#define HOST_KEY_F7 (HOST_KEY_F1 + 6)
#define HOST_KEY_F8 (HOST_KEY_F1 + 7)
#define HOST_KEY_F9 (HOST_KEY_F1 + 8)
#define HOST_KEY_F10 (HOST_KEY_F1 + 9)
#define HOST_KEY_F11 (HOST_KEY_F1 + 10)
#define HOST_KEY_F12 (HOST_KEY_F1 + 11)
#define HOST_KEY_RIGHT (0x40000000 + 79)
#define HOST_KEY_LEFT (0x40000000 + 80)
#define HOST_KEY_DOWN (0x40000000 + 81)
#define HOST_KEY_UP (0x40000000 + 82)
#define HOST_MOD_CTRL 1
typedef int32_t HostButton;
enum {
    HOST_CONTROLLER_BUTTON_A, HOST_CONTROLLER_BUTTON_B,
    HOST_CONTROLLER_BUTTON_X, HOST_CONTROLLER_BUTTON_Y,
    HOST_CONTROLLER_BUTTON_BACK, HOST_CONTROLLER_BUTTON_GUIDE,
    HOST_CONTROLLER_BUTTON_START, HOST_CONTROLLER_BUTTON_LEFTSTICK,
    HOST_CONTROLLER_BUTTON_RIGHTSTICK, HOST_CONTROLLER_BUTTON_LEFTSHOULDER,
    HOST_CONTROLLER_BUTTON_RIGHTSHOULDER, HOST_CONTROLLER_BUTTON_DPAD_UP,
    HOST_CONTROLLER_BUTTON_DPAD_DOWN, HOST_CONTROLLER_BUTTON_DPAD_LEFT,
    HOST_CONTROLLER_BUTTON_DPAD_RIGHT
};
enum { HOST_QUIT = 1, HOST_KEYDOWN, HOST_KEYUP, HOST_MOUSEMOTION,
    HOST_MOUSEBUTTONDOWN, HOST_MOUSEBUTTONUP, HOST_MOUSEWHEEL, HOST_WINDOWEVENT,
    HOST_CONTROLLERDEVICEADDED, HOST_CONTROLLERDEVICEREMOVED,
    HOST_CONTROLLERBUTTONDOWN, HOST_CONTROLLERBUTTONUP, HOST_CONTROLLERAXISMOTION };
enum { HOST_WINDOWEVENT_FOCUS_LOST, HOST_WINDOWEVENT_FOCUS_GAINED, HOST_WINDOWEVENT_LEAVE };
enum { HOST_BUTTON_LEFT = 1, HOST_BUTTON_RIGHT = 3 };
enum { HOST_CONTROLLER_AXIS_LEFTX, HOST_CONTROLLER_AXIS_LEFTY };
enum { HOST_WINDOW_INPUT_FOCUS = 1, HOST_WINDOW_MOUSE_FOCUS = 2 };
#define HOST_MOUSEWHEEL_FLIPPED 1
#define HOST_PRESSED 1
#define HOST_RELEASED 0
#define HOST_ENABLE 1
#define HOST_DISABLE 0
#define host_zero(v) memset(&(v), 0, sizeof(v))
typedef struct HostEvent {
    uint32_t type;
    struct { int state; bool repeat; struct { HostKey sym; int mod; } keysym; } key;
    struct { int x, y; } motion;
    struct { int x, y, button; } button;
    struct { float y; int direction; } wheel;
    struct { int event; } window;
    struct { int which; } cdevice;
    struct { int which, button; } cbutton;
    struct { int which, axis, value; } caxis;
} HostEvent;
typedef struct HostRect { int x, y, w, h; } HostRect;
typedef struct HostPoint { int x, y; } HostPoint;
typedef struct HostCanvas {
    uint32_t pixels[256 * 240], frame[256 * 240];
    uint32_t color;
    bool has_frame;
    HostRect crop;
} HostCanvas;

void host_setup(void);
void host_shutdown(void);
void host_event(const sapp_event *event);
bool host_poll_event(HostEvent *event);
void host_push_event(const HostEvent *event);
void host_poll_gamepads(void);
HostKey host_key(sapp_keycode key);
const char *host_key_name(HostKey key);
uint64_t host_counter(void);
uint64_t host_frequency(void);
uint32_t host_ticks(void);
void host_delay(uint32_t ms);
char *host_base_path(void); /* malloc-owned, with trailing separator */
void host_display(int scale, bool fullscreen);
void host_mouse_position(int *x, int *y);
void host_to_logical(HostCanvas *canvas, int x, int y, float *lx, float *ly);
void host_viewport(int width, int height, HostRect *rect);
uint32_t host_window_flags(void);
void host_show_cursor(bool show);
bool host_point_in_rect(const HostPoint *point, const HostRect *rect);
void host_color(HostCanvas *canvas, int r, int g, int b, int a);
void host_clear(HostCanvas *canvas);
void host_fill_rect(HostCanvas *canvas, const HostRect *rect);
void host_draw_rect(HostCanvas *canvas, const HostRect *rect);
void host_draw_points(HostCanvas *canvas, const HostPoint *points, int count);
void host_draw_frame(HostCanvas *canvas, const uint32_t *argb, const HostRect *crop);
void host_present(HostCanvas *canvas);
bool host_audio_valid(void);
double host_audio_latency_ms(void);
void host_audio_pause(bool pause);
void host_audio_clear(void);
uint32_t host_audio_queued_bytes(void);
int host_audio_queue(const float *samples, uint32_t bytes);
#endif
