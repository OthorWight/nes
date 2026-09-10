#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#define SOKOL_IMPL
#if defined(_WIN32)
#define SOKOL_D3D11
#elif defined(__APPLE__)
#define SOKOL_METAL
#else
#define SOKOL_GLCORE
#endif
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Warray-bounds"
#endif
#include "host.h"
#include "menu_bar.h"
#include "../third_party/sokol/sokol_gfx.h"
#include "../third_party/sokol/sokol_glue.h"
#include "../third_party/sokol/sokol_gl.h"
#include "../third_party/sokol/sokol_audio.h"
#include "../third_party/sokol/sokol_time.h"
#include "../third_party/sokol/sokol_log.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#ifdef _WIN32
#include <xinput.h>
#else
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#endif
#ifdef __APPLE__
#include <mach-o/dyld.h>
#import <GameController/GameController.h>
#endif
#ifdef __linux__
#include <fcntl.h>
#include <linux/joystick.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <errno.h>
#endif

static sg_image frame_image;
static sg_view frame_view;
static sg_image overlay_image;
static sg_view overlay_view;
static sg_image panel_image;
static sg_view panel_view;
static HostCanvas *debug_panel;
static void (*chrome)(void);
static sg_image chrome_image;
static sg_view chrome_view;
static int windowed_scale;
static sg_sampler frame_sampler;
static sgl_pipeline overlay_pipeline;
static bool audio_initialized;
static HostEvent events[1024];
static unsigned event_read, event_write;
static int mouse_x, mouse_y;
static uint32_t window_flags = HOST_WINDOW_INPUT_FOCUS | HOST_WINDOW_MOUSE_FOCUS;
enum { AUDIO_CAPACITY = 16384 };
static struct {
    float samples[AUDIO_CAPACITY];
    unsigned read, count;
    bool paused;
#ifdef _WIN32
    CRITICAL_SECTION mutex;
#else
    pthread_mutex_t mutex;
#endif
} audio;

static void audio_lock(void) {
#ifdef _WIN32
    EnterCriticalSection(&audio.mutex);
#else
    pthread_mutex_lock(&audio.mutex);
#endif
}
static void audio_unlock(void) {
#ifdef _WIN32
    LeaveCriticalSection(&audio.mutex);
#else
    pthread_mutex_unlock(&audio.mutex);
#endif
}
static void audio_stream(float *buffer, int frames, int channels) {
    audio_lock();
    for (int i = 0; i < frames; ++i) {
        float sample = 0;
        if (!audio.paused && audio.count) {
            sample = audio.samples[audio.read];
            audio.read = (audio.read + 1) % AUDIO_CAPACITY;
            --audio.count;
        }
        for (int c = 0; c < channels; ++c) buffer[i * channels + c] = sample;
    }
    audio_unlock();
}
void host_audio_pause(bool pause) { audio_lock(); audio.paused = pause; audio_unlock(); }
void host_audio_clear(void) { audio_lock(); audio.read = audio.count = 0; audio_unlock(); }
uint32_t host_audio_queued_bytes(void) {
    audio_lock(); uint32_t bytes = audio.count * sizeof(float); audio_unlock(); return bytes;
}
int host_audio_queue(const float *samples, uint32_t bytes) {
    unsigned count = bytes / sizeof(float);
    audio_lock();
    if (count > AUDIO_CAPACITY - audio.count) { audio_unlock(); return -1; }
    for (unsigned i = 0; i < count; ++i)
        audio.samples[(audio.read + audio.count + i) % AUDIO_CAPACITY] = samples[i];
    audio.count += count;
    audio_unlock();
    return 0;
}
bool host_audio_valid(void) { return saudio_isvalid(); }
double host_audio_latency_ms(void) {
    return saudio_isvalid() ? saudio_buffer_frames() * 1000.0 / saudio_sample_rate() : 0;
}
void host_setup(void) {
    stm_setup();
    sg_setup(&(sg_desc){.environment = sglue_environment(), .logger.func = slog_func});
    sgl_setup(&(sgl_desc_t){.logger.func = slog_func});
    frame_image = sg_make_image(&(sg_image_desc){.width = 256, .height = 240,
        .pixel_format = SG_PIXELFORMAT_RGBA8, .usage.dynamic_update = true});
    frame_view = sg_make_view(&(sg_view_desc){.texture.image = frame_image});
    overlay_image = sg_make_image(&(sg_image_desc){.width = 256, .height = 240,
        .pixel_format = SG_PIXELFORMAT_RGBA8, .usage.dynamic_update = true});
    overlay_view = sg_make_view(&(sg_view_desc){.texture.image = overlay_image});
    panel_image = sg_make_image(&(sg_image_desc){.width = HOST_PANEL_WIDTH, .height = HOST_PANEL_HEIGHT,
        .pixel_format = SG_PIXELFORMAT_RGBA8, .usage.dynamic_update = true});
    panel_view = sg_make_view(&(sg_view_desc){.texture.image = panel_image});
    overlay_pipeline = sgl_make_pipeline(&(sg_pipeline_desc){.colors[0].blend = {
        .enabled = true, .src_factor_rgb = SG_BLENDFACTOR_SRC_ALPHA,
        .dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
        .src_factor_alpha = SG_BLENDFACTOR_ONE, .dst_factor_alpha = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA}});
    frame_sampler = sg_make_sampler(&(sg_sampler_desc){.min_filter = SG_FILTER_NEAREST,
        .mag_filter = SG_FILTER_NEAREST, .wrap_u = SG_WRAP_CLAMP_TO_EDGE, .wrap_v = SG_WRAP_CLAMP_TO_EDGE});
#ifdef _WIN32
    InitializeCriticalSection(&audio.mutex);
#else
    pthread_mutex_init(&audio.mutex, NULL);
#endif
    audio.paused = true;
    if (!getenv("NES_DISABLE_AUDIO")) {
        saudio_setup(&(saudio_desc){.sample_rate = 44100, .num_channels = 1,
            .buffer_frames = 1024, .stream_cb = audio_stream, .logger.func = slog_func});
        audio_initialized = true;
    }
}
void host_shutdown(void) {
    if (audio_initialized) saudio_shutdown();
#ifdef _WIN32
    DeleteCriticalSection(&audio.mutex);
#else
    pthread_mutex_destroy(&audio.mutex);
#endif
    sgl_shutdown();
    sg_shutdown();
}
uint64_t host_counter(void) { return stm_now(); }
uint64_t host_frequency(void) { return 1000000000; }
uint32_t host_ticks(void) { return (uint32_t)stm_ms(stm_now()); }
void host_delay(uint32_t ms) {
#ifdef _WIN32
    Sleep(ms);
#else
    struct timespec delay = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000};
    nanosleep(&delay, NULL);
#endif
}
char *host_base_path(void) {
    char path[4096];
#ifdef _WIN32
    DWORD count = GetModuleFileNameA(NULL, path, sizeof(path));
    if (!count || count >= sizeof(path)) return NULL;
#elif defined(__APPLE__)
    uint32_t size = sizeof(path);
    if (_NSGetExecutablePath(path, &size)) return NULL;
#else
    ssize_t count = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (count <= 0 || count >= (ssize_t)sizeof(path) - 1) return NULL;
    path[count] = 0;
#endif
    char *last = NULL;
    for (char *p = path; *p; ++p) if (*p == '/' || *p == '\\') last = p;
    if (!last) return NULL;
    last[1] = 0;
    char *result = malloc(strlen(path) + 1);
    if (result) strcpy(result, path);
    return result;
}
void host_display(int scale, bool fullscreen) {
    windowed_scale = scale;
    if (fullscreen != sapp_is_fullscreen()) sapp_toggle_fullscreen();
    if (fullscreen) return;
    int content_width = (debug_panel ? 448 : 256) * scale;
    int content_height = 240 * scale;
    if (chrome) {
        int w, h;
        float ui_scale = host_chrome_layout(content_width, content_height, &w, &h);
        content_height += (int)ceilf((MENU_BAR_HEIGHT + STATUS_BAR_HEIGHT) * ui_scale);
    }
#ifdef _WIN32
    HWND window = (HWND)sapp_win32_get_hwnd();
    if (scale == 5) { ShowWindow(window, SW_MAXIMIZE); return; }
    ShowWindow(window, SW_RESTORE);
    RECT rect = {0, 0, content_width, content_height};
    AdjustWindowRectEx(&rect, (DWORD)GetWindowLongPtr(window, GWL_STYLE), FALSE,
        (DWORD)GetWindowLongPtr(window, GWL_EXSTYLE));
    SetWindowPos(window, NULL, 0, 0, rect.right - rect.left, rect.bottom - rect.top,
        SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
#elif defined(__APPLE__)
    NSWindow *window = (__bridge NSWindow *)sapp_macos_get_window();
    if (scale == 5) { if (![window isZoomed]) [window zoom:nil]; }
    else { if ([window isZoomed]) [window zoom:nil]; [window setContentSize:NSMakeSize(content_width, content_height)]; }
#else
    Display *display = (Display *)sapp_x11_get_display();
    Window window = (Window)(uintptr_t)sapp_x11_get_window();
    XEvent event = {0};
    event.type = ClientMessage; event.xclient.window = window; event.xclient.format = 32;
    event.xclient.message_type = XInternAtom(display, "_NET_WM_STATE", False);
    event.xclient.data.l[0] = scale == 5 ? 1 : 0;
    event.xclient.data.l[1] = (long)XInternAtom(display, "_NET_WM_STATE_MAXIMIZED_VERT", False);
    event.xclient.data.l[2] = (long)XInternAtom(display, "_NET_WM_STATE_MAXIMIZED_HORZ", False);
    XSendEvent(display, DefaultRootWindow(display), False, SubstructureRedirectMask | SubstructureNotifyMask, &event);
    if (scale != 5) XResizeWindow(display, window, content_width, content_height);
    XFlush(display);
#endif
}
void host_viewport(int width, int height, HostRect *rect) {
    double scale = fmin(width / 256.0, height / 240.0);
    rect->w = (int)(256 * scale); rect->h = (int)(240 * scale);
    rect->x = (width - rect->w) / 2; rect->y = (height - rect->h) / 2;
}
void host_to_logical(HostCanvas *canvas, int x, int y, float *lx, float *ly) {
    (void)canvas;
    HostRect rect, panel; host_layout(sapp_width(), sapp_height(), &rect, &panel);
    *lx = rect.w ? (x - rect.x) * 256.0f / rect.w : -1;
    *ly = rect.h ? (y - rect.y) * 240.0f / rect.h : -1;
}
void host_set_debug_panel(HostCanvas *panel) {
    bool changed = (debug_panel != NULL) != (panel != NULL);
    debug_panel = panel;
    if (changed && windowed_scale > 0 && windowed_scale < 5 && !sapp_is_fullscreen())
        host_display(windowed_scale, false);
}
void host_layout(int width, int height, HostRect *game, HostRect *panel) {
    int top = 0;
    if (chrome) {
        int w, h;
        float scale = host_chrome_layout(width, height, &w, &h);
        top = (int)ceilf(MENU_BAR_HEIGHT * scale);
        height -= top + (int)ceilf(STATUS_BAR_HEIGHT * scale);
        if (height < 0) height = 0;
    }
    int game_width = debug_panel ? width * 4 / 7 : width;
    host_viewport(game_width, height, game);
    game->y += top;
    *panel = (HostRect){0};
    if (debug_panel) {
        double scale = fmin((width - game_width) / (double)HOST_PANEL_WIDTH,
                            height / (double)HOST_PANEL_HEIGHT);
        panel->w = (int)(HOST_PANEL_WIDTH * scale);
        panel->h = (int)(HOST_PANEL_HEIGHT * scale);
        panel->x = game_width + (width - game_width - panel->w) / 2;
        panel->y = top + (height - panel->h) / 2;
    }
}
void host_set_chrome(void (*draw)(void), const uint8_t font[95][8]) {
    chrome = draw;
    if (!draw || chrome_image.id) return;
    uint32_t pixels[128 * 48] = {0};
    for (int c = 0; c < 95; ++c)
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x)
                if (font[c][y] & (0x80 >> x)) pixels[((c / 16) * 8 + y) * 128 + (c % 16) * 8 + x] = 0xFFFFFFFF;
    chrome_image = sg_make_image(&(sg_image_desc){.width = 128, .height = 48,
        .pixel_format = SG_PIXELFORMAT_RGBA8, .data.mip_levels[0] = {pixels, sizeof(pixels)}});
    chrome_view = sg_make_view(&(sg_view_desc){.texture.image = chrome_image});
}
float host_chrome_layout(int width, int height, int *logical_width, int *logical_height) {
    /* Desktop controls follow monitor DPI only, never NES zoom or window size. */
    float scale = fmaxf(1.0f, sapp_dpi_scale());
    *logical_width = (int)ceilf(width / scale);
    *logical_height = (int)ceilf(height / scale);
    return scale;
}
void host_ui_rect(HostRect r, uint32_t color) {
    sgl_disable_texture(); sgl_c4b((color >> 16) & 255, (color >> 8) & 255, color & 255, 255);
    sgl_begin_quads();
    sgl_v2f((float)r.x, (float)r.y); sgl_v2f((float)(r.x + r.w), (float)r.y);
    sgl_v2f((float)(r.x + r.w), (float)(r.y + r.h)); sgl_v2f((float)r.x, (float)(r.y + r.h));
    sgl_end();
}
void host_ui_text(const char *text, int x, int y, uint32_t color) {
    sgl_enable_texture(); sgl_texture(chrome_view, frame_sampler);
    sgl_c4b((color >> 16) & 255, (color >> 8) & 255, color & 255, 255);
    sgl_begin_quads();
    for (; *text; ++text, x += 8) {
        unsigned c = (unsigned char)*text;
        if (c < 32 || c > 126) c = '?';
        c -= 32;
        float u = (c % 16) / 16.0f, v = (c / 16) / 6.0f;
        sgl_v2f_t2f((float)x, (float)y, u, v);
        sgl_v2f_t2f((float)(x + 8), (float)y, u + 1/16.0f, v);
        sgl_v2f_t2f((float)(x + 8), (float)(y + 8), u + 1/16.0f, v + 1/6.0f);
        sgl_v2f_t2f((float)x, (float)(y + 8), u, v + 1/6.0f);
    }
    sgl_end();
}
void host_mouse_position(int *x, int *y) { *x = mouse_x; *y = mouse_y; }
uint32_t host_window_flags(void) { return window_flags; }
void host_show_cursor(bool show) { sapp_show_mouse(show); }
bool host_point_in_rect(const HostPoint *p, const HostRect *r) {
    return p->x >= r->x && p->y >= r->y && p->x < r->x + r->w && p->y < r->y + r->h;
}
void host_color(HostCanvas *c, int r, int g, int b, int a) {
    c->color = (uint32_t)r | (uint32_t)g << 8 | (uint32_t)b << 16 | (uint32_t)a << 24;
}
void host_clear(HostCanvas *c) {
    c->has_frame = false;
    int count = (c->width ? c->width : 256) * (c->height ? c->height : 240);
    for (int i = 0; i < count; ++i) c->pixels[i] = c->color;
}
void host_draw_points(HostCanvas *c, const HostPoint *points, int count) {
    int width = c->width ? c->width : 256, height = c->height ? c->height : 240;
    for (int i = 0; i < count; ++i) {
        int x = points[i].x, y = points[i].y;
        if (x >= 0 && x < width && y >= 0 && y < height) c->pixels[y * width + x] = c->color;
    }
}
void host_fill_rect(HostCanvas *c, const HostRect *r) {
    int width = c->width ? c->width : 256, height = c->height ? c->height : 240;
    for (int y = r->y; y < r->y + r->h; ++y)
        for (int x = r->x; x < r->x + r->w; ++x)
            if (x >= 0 && x < width && y >= 0 && y < height) c->pixels[y * width + x] = c->color;
}
void host_draw_rect(HostCanvas *c, const HostRect *r) {
    host_fill_rect(c, &(HostRect){r->x, r->y, r->w, 1});
    host_fill_rect(c, &(HostRect){r->x, r->y + r->h - 1, r->w, 1});
    host_fill_rect(c, &(HostRect){r->x, r->y, 1, r->h});
    host_fill_rect(c, &(HostRect){r->x + r->w - 1, r->y, 1, r->h});
}
void host_draw_frame(HostCanvas *c, const uint32_t *argb, const HostRect *crop) {
    c->has_frame = true;
    c->crop = *crop;
    memset(c->pixels, 0, 256 * 240 * sizeof(uint32_t));
    for (unsigned i = 0; i < 256 * 240; ++i) {
        uint32_t p = argb[i];
        c->frame[i] = 0xff000000u | ((p & 255) << 16) | (p & 0xff00) | ((p >> 16) & 255);
    }
}
static void textured_quad(sg_view view, float u0, float v0, float u1, float v1) {
    sgl_texture(view, frame_sampler);
    sgl_begin_quads();
    sgl_v2f_t2f(-1, 1, u0, v0); sgl_v2f_t2f(1, 1, u1, v0);
    sgl_v2f_t2f(1, -1, u1, v1); sgl_v2f_t2f(-1, -1, u0, v1);
    sgl_end();
}
void host_present(HostCanvas *c) {
    sg_update_image(overlay_image, &(sg_image_data){.mip_levels[0] = {c->pixels, 256 * 240 * sizeof(uint32_t)}});
    HostRect rect, panel; host_layout(sapp_width(), sapp_height(), &rect, &panel);
    sgl_defaults(); sgl_viewport(rect.x, rect.y, rect.w, rect.h, true);
    sgl_enable_texture();
    if (c->has_frame) {
        sg_update_image(frame_image, &(sg_image_data){.mip_levels[0] = {c->frame, sizeof(c->frame)}});
        textured_quad(frame_view, c->crop.x / 256.0f, c->crop.y / 240.0f,
            (c->crop.x + c->crop.w) / 256.0f, (c->crop.y + c->crop.h) / 240.0f);
        sgl_load_pipeline(overlay_pipeline);
    }
    textured_quad(overlay_view, 0, 0, 1, 1);
    if (debug_panel) {
        sg_update_image(panel_image, &(sg_image_data){.mip_levels[0] = {debug_panel->pixels, sizeof(debug_panel->pixels)}});
        sgl_defaults();
        sgl_viewport(panel.x, panel.y, panel.w, panel.h, true);
        sgl_enable_texture();
        textured_quad(panel_view, 0, 0, 1, 1);
    }
    if (chrome) {
        int w, h;
        float scale = host_chrome_layout(sapp_width(), sapp_height(), &w, &h);
        sgl_defaults(); sgl_viewport(0, 0, sapp_width(), sapp_height(), true);
        sgl_matrix_mode_projection(); sgl_ortho(0, sapp_width() / scale, sapp_height() / scale, 0, -1, 1);
        sgl_matrix_mode_modelview(); sgl_load_pipeline(overlay_pipeline);
        chrome();
    }
    sg_begin_pass(&(sg_pass){.swapchain = sglue_swapchain(),
        .action.colors[0] = {.load_action = SG_LOADACTION_CLEAR, .clear_value = {0, 0, 0, 1}}});
    sgl_draw(); sg_end_pass(); sg_commit();
}
void host_push_event(const HostEvent *event) {
    if (event_write - event_read == 1024) {
        /* Clear held inputs if an extreme event burst overflows the queue. */
        event_read = event_write;
        events[event_write++ % 1024] = (HostEvent){.type = HOST_WINDOWEVENT,
            .window.event = HOST_WINDOWEVENT_FOCUS_LOST};
    }
    events[event_write++ % 1024] = *event;
}
bool host_poll_event(HostEvent *event) {
    if (event_read == event_write) return false;
    *event = events[event_read++ % 1024]; return true;
}
HostKey host_key(sapp_keycode key) {
    if (key >= SAPP_KEYCODE_A && key <= SAPP_KEYCODE_Z) return 'a' + key - SAPP_KEYCODE_A;
    if (key >= SAPP_KEYCODE_F1 && key <= SAPP_KEYCODE_F24) return key <= SAPP_KEYCODE_F12 ? HOST_KEY_F1 + key - SAPP_KEYCODE_F1 : 0x40000000 + 104 + key - SAPP_KEYCODE_F13;
    if (key >= SAPP_KEYCODE_KP_1 && key <= SAPP_KEYCODE_KP_9) return 0x40000000 + 89 + key - SAPP_KEYCODE_KP_1;
    switch (key) {
        case SAPP_KEYCODE_ESCAPE: return HOST_KEY_ESCAPE;
        case SAPP_KEYCODE_ENTER: return HOST_KEY_RETURN;
        case SAPP_KEYCODE_BACKSPACE: return HOST_KEY_BACKSPACE;
        case SAPP_KEYCODE_TAB: return 9;
        case SAPP_KEYCODE_DELETE: return 127;
        case SAPP_KEYCODE_RIGHT: return HOST_KEY_RIGHT;
        case SAPP_KEYCODE_LEFT: return HOST_KEY_LEFT;
        case SAPP_KEYCODE_DOWN: return HOST_KEY_DOWN;
        case SAPP_KEYCODE_UP: return HOST_KEY_UP;
        case SAPP_KEYCODE_INSERT: return 0x40000000 + 73;
        case SAPP_KEYCODE_HOME: return 0x40000000 + 74;
        case SAPP_KEYCODE_PAGE_UP: return 0x40000000 + 75;
        case SAPP_KEYCODE_END: return 0x40000000 + 77;
        case SAPP_KEYCODE_PAGE_DOWN: return 0x40000000 + 78;
        case SAPP_KEYCODE_LEFT_SHIFT: return 0x40000000 + 225;
        case SAPP_KEYCODE_RIGHT_SHIFT: return 0x40000000 + 229;
        case SAPP_KEYCODE_LEFT_CONTROL: return 0x40000000 + 224;
        case SAPP_KEYCODE_RIGHT_CONTROL: return 0x40000000 + 228;
        case SAPP_KEYCODE_LEFT_ALT: return 0x40000000 + 226;
        case SAPP_KEYCODE_RIGHT_ALT: return 0x40000000 + 230;
        case SAPP_KEYCODE_LEFT_SUPER: return 0x40000000 + 227;
        case SAPP_KEYCODE_RIGHT_SUPER: return 0x40000000 + 231;
        case SAPP_KEYCODE_CAPS_LOCK: return 0x40000000 + 57;
        case SAPP_KEYCODE_PRINT_SCREEN: return 0x40000000 + 70;
        case SAPP_KEYCODE_SCROLL_LOCK: return 0x40000000 + 71;
        case SAPP_KEYCODE_PAUSE: return 0x40000000 + 72;
        case SAPP_KEYCODE_NUM_LOCK: return 0x40000000 + 83;
        case SAPP_KEYCODE_KP_DIVIDE: return 0x40000000 + 84;
        case SAPP_KEYCODE_KP_MULTIPLY: return 0x40000000 + 85;
        case SAPP_KEYCODE_KP_SUBTRACT: return 0x40000000 + 86;
        case SAPP_KEYCODE_KP_ADD: return 0x40000000 + 87;
        case SAPP_KEYCODE_KP_0: return 0x40000000 + 98;
        case SAPP_KEYCODE_KP_DECIMAL: return 0x40000000 + 99;
        case SAPP_KEYCODE_KP_EQUAL: return 0x40000000 + 103;
        case SAPP_KEYCODE_KP_ENTER: return 0x40000000 + 88;
        case SAPP_KEYCODE_MENU: return 0x40000000 + 118;
        default: return key >= 32 && key <= 96 ? (int)key : HOST_KEY_UNKNOWN;
    }
}
const char *host_key_name(HostKey key) {
    static char name[32];
    if (key >= 33 && key <= 126) { snprintf(name, sizeof(name), "%c", key >= 'a' && key <= 'z' ? key - 32 : key); return name; }
    if (key >= HOST_KEY_F1 && key <= HOST_KEY_F12) { snprintf(name, sizeof(name), "F%d", key - HOST_KEY_F1 + 1); return name; }
    if (key >= 0x40000000 + 104 && key <= 0x40000000 + 115) {
        snprintf(name, sizeof(name), "F%d", key - (0x40000000 + 104) + 13); return name;
    }
    if (key >= 0x40000000 + 89 && key <= 0x40000000 + 97) {
        snprintf(name, sizeof(name), "Keypad %d", key - (0x40000000 + 89) + 1); return name;
    }
    static const struct { int code; const char *name; } special[] = {
        {57,"Caps Lock"}, {70,"Print Screen"}, {71,"Scroll Lock"}, {72,"Pause"},
        {73,"Insert"}, {74,"Home"}, {75,"Page Up"}, {77,"End"}, {78,"Page Down"},
        {83,"Num Lock"}, {84,"Keypad /"}, {85,"Keypad *"}, {86,"Keypad -"},
        {87,"Keypad +"}, {88,"Keypad Enter"}, {98,"Keypad 0"}, {99,"Keypad ."},
        {103,"Keypad ="}, {118,"Menu"}, {224,"Left Ctrl"}, {225,"Left Shift"},
        {226,"Left Alt"}, {227,"Left Super"}, {228,"Right Ctrl"}, {229,"Right Shift"},
        {230,"Right Alt"}, {231,"Right Super"}
    };
    for (unsigned i = 0; i < sizeof(special) / sizeof(special[0]); ++i)
        if (key == 0x40000000 + special[i].code) return special[i].name;
    switch (key) {
        case HOST_KEY_UNKNOWN: return "Unknown"; case HOST_KEY_SPACE: return "Space";
        case HOST_KEY_RETURN: return "Enter"; case HOST_KEY_ESCAPE: return "Escape";
        case HOST_KEY_BACKSPACE: return "Backspace"; case HOST_KEY_UP: return "Up";
        case 9: return "Tab"; case 127: return "Delete";
        case HOST_KEY_DOWN: return "Down"; case HOST_KEY_LEFT: return "Left"; case HOST_KEY_RIGHT: return "Right";
        default: snprintf(name, sizeof(name), "Key %d", key); return name;
    }
}
void host_event(const sapp_event *event) {
    HostEvent e = {0};
    switch (event->type) {
        case SAPP_EVENTTYPE_CHAR:
            e.type = HOST_TEXTINPUT; e.character = event->char_code; break;
        case SAPP_EVENTTYPE_QUIT_REQUESTED: sapp_cancel_quit(); e.type = HOST_QUIT; break;
        case SAPP_EVENTTYPE_KEY_DOWN: case SAPP_EVENTTYPE_KEY_UP:
            e.type = event->type == SAPP_EVENTTYPE_KEY_DOWN ? HOST_KEYDOWN : HOST_KEYUP;
            e.key.keysym.sym = host_key(event->key_code);
            if (!e.key.keysym.sym) return;
            e.key.repeat = event->key_repeat;
            e.key.keysym.mod = (event->modifiers & SAPP_MODIFIER_CTRL ? HOST_MOD_CTRL : 0) |
                (event->modifiers & SAPP_MODIFIER_SHIFT ? HOST_MOD_SHIFT : 0) |
                (event->modifiers & SAPP_MODIFIER_ALT ? HOST_MOD_ALT : 0); break;
        case SAPP_EVENTTYPE_MOUSE_MOVE: case SAPP_EVENTTYPE_MOUSE_DOWN: case SAPP_EVENTTYPE_MOUSE_UP:
        case SAPP_EVENTTYPE_MOUSE_SCROLL: {
            mouse_x = (int)event->mouse_x; mouse_y = (int)event->mouse_y;
            e.window_mouse.x = mouse_x; e.window_mouse.y = mouse_y; e.window_mouse.valid = true;
            if (!chrome && debug_panel && mouse_x >= sapp_width() * 4 / 7 &&
                (event->type == SAPP_EVENTTYPE_MOUSE_DOWN || event->type == SAPP_EVENTTYPE_MOUSE_SCROLL)) return;
            float x, y; host_to_logical(NULL, mouse_x, mouse_y, &x, &y);
            /* floor keeps slightly negative letterbox coordinates offscreen. */
            e.motion.x = e.button.x = (int)floorf(x); e.motion.y = e.button.y = (int)floorf(y);
            if (event->type == SAPP_EVENTTYPE_MOUSE_MOVE) e.type = HOST_MOUSEMOTION;
            else if (event->type == SAPP_EVENTTYPE_MOUSE_SCROLL) { e.type = HOST_MOUSEWHEEL; e.wheel.y = event->scroll_y; }
            else { e.type = event->type == SAPP_EVENTTYPE_MOUSE_DOWN ? HOST_MOUSEBUTTONDOWN : HOST_MOUSEBUTTONUP;
                e.button.button = event->mouse_button == SAPP_MOUSEBUTTON_LEFT ? HOST_BUTTON_LEFT : event->mouse_button == SAPP_MOUSEBUTTON_RIGHT ? HOST_BUTTON_RIGHT : 2; }
            break;
        }
        case SAPP_EVENTTYPE_UNFOCUSED: case SAPP_EVENTTYPE_SUSPENDED:
            window_flags &= ~HOST_WINDOW_INPUT_FOCUS;
            e.type = HOST_WINDOWEVENT; e.window.event = HOST_WINDOWEVENT_FOCUS_LOST; break;
        case SAPP_EVENTTYPE_FOCUSED: case SAPP_EVENTTYPE_RESUMED:
            window_flags |= HOST_WINDOW_INPUT_FOCUS;
            e.type = HOST_WINDOWEVENT; e.window.event = HOST_WINDOWEVENT_FOCUS_GAINED; break;
        case SAPP_EVENTTYPE_MOUSE_LEAVE:
            window_flags &= ~HOST_WINDOW_MOUSE_FOCUS;
            e.type = HOST_WINDOWEVENT; e.window.event = HOST_WINDOWEVENT_LEAVE; break;
        case SAPP_EVENTTYPE_MOUSE_ENTER: window_flags |= HOST_WINDOW_MOUSE_FOCUS; return;
        default: return;
    }
    host_push_event(&e);
}

/* Sokol has no gamepad API. Native backends normalize into stable button IDs. */
static int active_pad = -1;
static uint32_t pad_buttons;
static int pad_x, pad_y;
static void gamepad_state(int id, uint32_t buttons, int x, int y) {
    if (id != active_pad) {
        if (active_pad >= 0) host_push_event(&(HostEvent){.type = HOST_CONTROLLERDEVICEREMOVED, .cdevice.which = active_pad});
        active_pad = id; pad_buttons = 0; pad_x = pad_y = 0;
        if (id >= 0) host_push_event(&(HostEvent){.type = HOST_CONTROLLERDEVICEADDED, .cdevice.which = id});
    }
    if (id < 0) return;
    for (int i = 0; i < 15; ++i) if ((buttons ^ pad_buttons) & (1u << i))
        host_push_event(&(HostEvent){.type = buttons & (1u << i) ? HOST_CONTROLLERBUTTONDOWN : HOST_CONTROLLERBUTTONUP,
            .cbutton = {.which = id, .button = i}});
    if (x != pad_x) host_push_event(&(HostEvent){.type = HOST_CONTROLLERAXISMOTION, .caxis = {id, HOST_CONTROLLER_AXIS_LEFTX, x}});
    if (y != pad_y) host_push_event(&(HostEvent){.type = HOST_CONTROLLERAXISMOTION, .caxis = {id, HOST_CONTROLLER_AXIS_LEFTY, y}});
    pad_buttons = buttons; pad_x = x; pad_y = y;
}
void host_poll_gamepads(void) {
#ifdef _WIN32
    static bool loaded;
    static DWORD (WINAPI *get_state)(DWORD, XINPUT_STATE *);
    if (!loaded) {
        loaded = true;
        HMODULE module = LoadLibraryA("xinput1_4.dll");
        if (!module) module = LoadLibraryA("xinput9_1_0.dll");
        if (module) { FARPROC proc = GetProcAddress(module, "XInputGetState"); memcpy(&get_state, &proc, sizeof(get_state)); }
    }
    if (!get_state) return;
    int id = -1; XINPUT_STATE state = {0};
    if (active_pad >= 0 && get_state((DWORD)active_pad, &state) == ERROR_SUCCESS) id = active_pad;
    else for (DWORD i = 0; i < 4; ++i) if (get_state(i, &state) == ERROR_SUCCESS) { id = (int)i; break; }
    const WORD masks[15] = {XINPUT_GAMEPAD_A, XINPUT_GAMEPAD_B, XINPUT_GAMEPAD_X, XINPUT_GAMEPAD_Y,
        XINPUT_GAMEPAD_BACK, 0, XINPUT_GAMEPAD_START, XINPUT_GAMEPAD_LEFT_THUMB, XINPUT_GAMEPAD_RIGHT_THUMB,
        XINPUT_GAMEPAD_LEFT_SHOULDER, XINPUT_GAMEPAD_RIGHT_SHOULDER, XINPUT_GAMEPAD_DPAD_UP,
        XINPUT_GAMEPAD_DPAD_DOWN, XINPUT_GAMEPAD_DPAD_LEFT, XINPUT_GAMEPAD_DPAD_RIGHT};
    uint32_t buttons = 0;
    for (int i = 0; i < 15; ++i) if (state.Gamepad.wButtons & masks[i]) buttons |= 1u << i;
    gamepad_state(id, buttons, state.Gamepad.sThumbLX, -(int)state.Gamepad.sThumbLY);
#elif defined(__APPLE__)
    GCExtendedGamepad *pad = nil;
    for (GCController *controller in [GCController controllers]) if (controller.extendedGamepad) { pad = controller.extendedGamepad; break; }
    if (!pad) { gamepad_state(-1, 0, 0, 0); return; }
    GCControllerButtonInput *inputs[15] = {pad.buttonA, pad.buttonB, pad.buttonX, pad.buttonY,
        pad.buttonOptions, pad.buttonHome, pad.buttonMenu, pad.leftThumbstickButton, pad.rightThumbstickButton,
        pad.leftShoulder, pad.rightShoulder, pad.dpad.up, pad.dpad.down, pad.dpad.left, pad.dpad.right};
    uint32_t buttons = 0;
    for (int i = 0; i < 15; ++i) if (inputs[i].pressed) buttons |= 1u << i;
    gamepad_state(0, buttons, (int)(pad.leftThumbstick.xAxis.value * 32767), (int)(-pad.leftThumbstick.yAxis.value * 32767));
#else
    static int fd = -1;
    static uint16_t button_map[KEY_MAX - BTN_MISC + 1];
    static uint8_t axis_map[ABS_CNT];
    static uint32_t buttons;
    static int x, y;
    static uint32_t last_scan;
    if (fd < 0) {
        uint32_t now = host_ticks();
        if ((uint32_t)(now - last_scan) < 1000) return;
        last_scan = now;
        for (int i = 0; i < 16 && fd < 0; ++i) {
            char path[64]; snprintf(path, sizeof(path), "/dev/input/js%d", i);
            fd = open(path, O_RDONLY | O_NONBLOCK);
        }
        if (fd < 0) return;
        memset(button_map, 0, sizeof(button_map)); memset(axis_map, 0xff, sizeof(axis_map));
        ioctl(fd, JSIOCGBTNMAP, button_map); ioctl(fd, JSIOCGAXMAP, axis_map);
        buttons = 0; x = y = 0;
    }
    const uint16_t codes[15] = {BTN_SOUTH, BTN_EAST, BTN_NORTH, BTN_WEST, BTN_SELECT, BTN_MODE, BTN_START,
        BTN_THUMBL, BTN_THUMBR, BTN_TL, BTN_TR, BTN_DPAD_UP, BTN_DPAD_DOWN, BTN_DPAD_LEFT, BTN_DPAD_RIGHT};
    struct js_event e;
    ssize_t result;
    while ((result = read(fd, &e, sizeof(e))) == sizeof(e)) {
        int type = e.type & ~JS_EVENT_INIT;
        /* The 512-entry button map covers every uint8_t event number. */
        if (type == JS_EVENT_BUTTON) {
            for (int i = 0; i < 15; ++i) if (button_map[e.number] == codes[i]) {
                if (e.value) buttons |= 1u << i; else buttons &= ~(1u << i);
            }
        } else if (type == JS_EVENT_AXIS && e.number < ABS_CNT) {
            int axis = axis_map[e.number];
            if (axis == ABS_X) x = e.value;
            if (axis == ABS_Y) y = e.value;
            if (axis == ABS_HAT0X || axis == ABS_HAT0Y) {
                int shift = axis == ABS_HAT0X ? 13 : 11;
                buttons &= ~(3u << shift);
                if (e.value < 0) buttons |= 1u << shift;
                if (e.value > 0) buttons |= 2u << shift;
            }
        }
        gamepad_state(0, buttons, x, y);
    }
    if (result == 0 || (result < 0 && errno != EAGAIN && errno != EINTR)) {
        close(fd); fd = -1; gamepad_state(-1, 0, 0, 0);
    } else gamepad_state(0, buttons, x, y);
#endif
}
