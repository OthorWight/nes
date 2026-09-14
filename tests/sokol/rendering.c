/* Read the real D3D11 render target to validate colors, orientation,
   crop, alpha overlays and black letterboxes. Windows only. */
#define COBJMACROS
#include <d3d11.h>
#include "../../src/host.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

static HostCanvas canvas;
static HostCanvas sidebar = {.width = HOST_PANEL_WIDTH, .height = HOST_PANEL_HEIGHT};
static unsigned stage;
static uint32_t pixels[256 * 240];
static void input_checks(void) {
    assert(host_key(SAPP_KEYCODE_Z) == 'z');
    assert(host_key(SAPP_KEYCODE_F5) == 0x4000003e);
    assert(host_key(SAPP_KEYCODE_LEFT_SHIFT) == 0x400000e1);
    assert(!strcmp(host_key_name(host_key(SAPP_KEYCODE_KP_ENTER)), "Keypad Enter"));
    host_event(&(sapp_event){.type = SAPP_EVENTTYPE_KEY_DOWN,
        .key_code = SAPP_KEYCODE_F4, .modifiers = SAPP_MODIFIER_CTRL, .key_repeat = true});
    HostEvent event;
    assert(host_poll_event(&event) && event.type == HOST_KEYDOWN);
    assert(event.key.keysym.sym == HOST_KEY_F4 && event.key.keysym.mod == HOST_MOD_CTRL && event.key.repeat);
    host_event(&(sapp_event){.type = SAPP_EVENTTYPE_UNFOCUSED});
    assert(host_poll_event(&event) && event.window.event == HOST_WINDOWEVENT_FOCUS_LOST);
    assert(!(host_window_flags() & HOST_WINDOW_INPUT_FOCUS));
    host_event(&(sapp_event){.type = SAPP_EVENTTYPE_FOCUSED});
    assert(host_poll_event(&event) && (host_window_flags() & HOST_WINDOW_INPUT_FOCUS));
    host_event(&(sapp_event){.type = SAPP_EVENTTYPE_MOUSE_DOWN,
        .mouse_button = SAPP_MOUSEBUTTON_LEFT, .mouse_x = 0, .mouse_y = 100});
    assert(host_poll_event(&event) && event.type == HOST_MOUSEBUTTONDOWN && event.button.x < 0);
    assert(!host_poll_event(&event));
}
static void init(void) {
    host_setup();
    for (int y = 0; y < 240; ++y) for (int x = 0; x < 256; ++x)
        pixels[y * 256 + x] = y < 120 ? (x < 128 ? 0xffff0000u : 0xff00ff00u) :
            (x < 128 ? 0xff0000ffu : 0xffffffffu);
}
static void check_pixel(const D3D11_MAPPED_SUBRESOURCE *map, int x, int y, uint32_t expected) {
    const uint32_t *row = (const uint32_t *)((const char *)map->pData + y * map->RowPitch);
    assert((row[x] & 0xffffff) == expected);
}
static uint32_t read_pixel(const D3D11_MAPPED_SUBRESOURCE *map, int x, int y) {
    return ((const uint32_t *)((const char *)map->pData + y * map->RowPitch))[x];
}
static void check_crt(const D3D11_MAPPED_SUBRESOURCE *map, HostRect r) {
    /* Flat colors remain recognizable, with a shallow scanline modulation. */
    int low = 255, high = 0;
    for (int y = r.y + r.h / 4; y < r.y + r.h / 4 + 20; ++y) {
        uint32_t p = read_pixel(map, r.x + r.w / 4, y);
        int red = (p >> 16) & 255;
        assert((p & 0xffff) == 0 && red >= 238);
        if (red < low) low = red;
        if (red > high) high = red;
    }
    assert(high > low && high - low <= 16);
    /* Straight edges retain contrast; the blend is narrower than these samples. */
    uint32_t left = read_pixel(map, r.x + r.w / 2 - 1, r.y + r.h / 4);
    uint32_t right = read_pixel(map, r.x + (r.w + 1) / 2, r.y + r.h / 4);
    assert(((left >> 16) & 255) >= 238 && ((left >> 8) & 255) <= 2);
    assert(((right >> 8) & 255) >= 238 && ((right >> 16) & 255) <= 2);
    assert((left & 255) == 0 && (right & 255) == 0);
    /* At these scales vertical blending stays narrower than an output pixel;
       only the tiny glow reaches the next row. */
    uint32_t above = read_pixel(map, r.x + r.w / 4, r.y + r.h / 2 - 1);
    assert(((above >> 16) & 255) >= 238 && (above & 255) <= 1);
}
static void frame(void) {
    if (stage == 0) input_checks();
    host_color(&sidebar, 17, 34, 51, 255); host_clear(&sidebar);
    bool crt = stage == 3 || stage == 4 || stage == 6 || stage == 8;
    if (stage == 6) {
        /* White beside black, plus a single-pixel black outline on white. */
        for (int y = 0; y < 240; ++y) for (int x = 0; x < 256; ++x)
            pixels[y * 256 + x] = y >= 180 ? 0xff808080u :
                x < 128 || x == 192 ? 0xff000000u : 0xffffffffu;
    }
    if (stage == 8) {
        for (int y = 0; y < 240; ++y) for (int x = 0; x < 256; ++x)
            pixels[y * 256 + x] = x >= 112 && x < 128 && y >= 48 && y < 64 ?
                0xff000000u : 0xffffffffu;
    }
    host_set_crt(crt);
    host_set_debug_panel(stage == 1 || stage == 4 ? &sidebar : NULL);
    host_color(&canvas, 0, 0, 0, 255); host_clear(&canvas);
    host_draw_frame(&canvas, pixels, stage == 8 ? &(HostRect){96, 32, 64, 64} : &(HostRect){8, 8, 240, 224});
    host_color(&canvas, 255, 0, 255, 255);
    host_fill_rect(&canvas, &(HostRect){120, 112, 16, 16});
    host_present(&canvas);
    sapp_environment env = sapp_get_environment();
    sapp_swapchain swapchain = sapp_acquire_swapchain();
    ID3D11Device *device = (ID3D11Device *)env.d3d11.device;
    ID3D11DeviceContext *context = (ID3D11DeviceContext *)env.d3d11.device_context;
    ID3D11Resource *resource;
    ID3D11RenderTargetView_GetResource((ID3D11RenderTargetView *)swapchain.d3d11.render_view, &resource);
    D3D11_TEXTURE2D_DESC desc;
    ID3D11Texture2D_GetDesc((ID3D11Texture2D *)resource, &desc);
    desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ; desc.MiscFlags = 0;
    ID3D11Texture2D *staging;
    assert(SUCCEEDED(ID3D11Device_CreateTexture2D(device, &desc, NULL, &staging)));
    ID3D11DeviceContext_CopyResource(context, (ID3D11Resource *)staging, resource);
    D3D11_MAPPED_SUBRESOURCE map;
    assert(SUCCEEDED(ID3D11DeviceContext_Map(context, (ID3D11Resource *)staging, 0, D3D11_MAP_READ, 0, &map)));
    HostRect r, panel; host_layout(sapp_width(), sapp_height(), &r, &panel);
    if (stage == 1 || stage == 4) {
        assert(r.x + r.w <= panel.x);
        check_pixel(&map, panel.x + panel.w / 2, panel.y + panel.h / 2, 0x112233);
        float x, y;
        host_to_logical(NULL, r.x + r.w / 2, r.y + r.h / 2, &x, &y);
        assert(fabsf(x - 128) < 1 && fabsf(y - 120) < 1);
        host_to_logical(NULL, panel.x + panel.w / 2, panel.y + panel.h / 2, &x, &y);
        assert(x >= 256);
        host_event(&(sapp_event){.type = SAPP_EVENTTYPE_MOUSE_DOWN,
            .mouse_button = SAPP_MOUSEBUTTON_RIGHT, .mouse_x = (float)(panel.x + panel.w / 2), .mouse_y = (float)(panel.y + panel.h / 2)});
        HostEvent ignored; assert(!host_poll_event(&ignored));
    }
    assert(desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM);
    check_pixel(&map, 0, 0, 0);
    if (stage == 8) {
        /* Zoom a contour: round its tip while keeping the pixel body black. */
        int corner_x = r.x + (int)(16.08f * r.w / 64);
        int corner_y = r.y + (int)(16.08f * r.h / 64);
        int center_x = r.x + (int)(16.5f * r.w / 64);
        int center_y = r.y + (int)(16.5f * r.h / 64);
        int edge_x = r.x + (int)(20.5f * r.w / 64);
        int edge_y = r.y + (int)(16.3f * r.h / 64);
        assert((read_pixel(&map, corner_x, corner_y) & 255) > 160);
        assert((read_pixel(&map, center_x, center_y) & 255) <= 2);
        assert((read_pixel(&map, edge_x, edge_y) & 255) <= 2);
    }
    else if (stage >= 6) {
        int y = r.y + r.h / 4;
        int dark = read_pixel(&map, r.x + r.w / 2 - 1, y) & 255;
        int bright = read_pixel(&map, r.x + r.w / 2, y) & 255;
        int white = read_pixel(&map, r.x + 3 * r.w / 4, y) & 255;
        int outline_x = r.x + (int)((192.5f - 8) * r.w / 240);
        int outline = read_pixel(&map, outline_x, y) & 255;
        uint32_t gray = read_pixel(&map, r.x + r.w / 4, r.y + 7 * r.h / 8) & 0xffffff;
        int level = gray & 255;
        assert(gray == (uint32_t)level * 0x010101u);
        assert(crt ? level >= 120 && level <= 128 : level == 128);
        check_pixel(&map, r.x + r.w / 4, y, 0);
        if (crt) {
            assert(white >= 238 && bright > 220);
            assert(dark <= 2);
            assert(outline <= 2); /* A one-pixel black outline stays black. */
        } else {
            assert(dark == 0 && bright == 255 && white == 255 && outline == 0);
        }
    }
    else if (crt) check_crt(&map, r);
    else {
        check_pixel(&map, r.x + r.w / 4, r.y + r.h / 4, 0xff0000);
        check_pixel(&map, r.x + 3 * r.w / 4, r.y + r.h / 4, 0x00ff00);
        check_pixel(&map, r.x + r.w / 4, r.y + 3 * r.h / 4, 0x0000ff);
        check_pixel(&map, r.x + 3 * r.w / 4, r.y + 3 * r.h / 4, 0xffffff);
    }
    check_pixel(&map, r.x + r.w / 2, r.y + r.h / 2, 0xff00ff);
    FILE *file = fopen(stage == 8 ? "crt-corners.bmp" : stage >= 6 ? (crt ? "crt-outlines.bmp" : "outlines.bmp") :
        crt ? "crt.bmp" : stage == 1 ? "sidebar.bmp" : "rendering.bmp", "wb"); assert(file);
    uint32_t size = 54 + desc.Width * desc.Height * 4;
    unsigned char header[54] = {'B','M'};
    memcpy(header + 2, &size, 4); header[10] = 54; header[14] = 40;
    memcpy(header + 18, &desc.Width, 4); memcpy(header + 22, &desc.Height, 4);
    header[26] = 1; header[28] = 32;
    assert(fwrite(header, 1, 54, file) == 54);
    for (int y = (int)desc.Height - 1; y >= 0; --y)
        assert(fwrite((const char *)map.pData + y * map.RowPitch, 4, desc.Width, file) == desc.Width);
    assert(!fclose(file));
    ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)staging, 0);
    ID3D11Texture2D_Release(staging); ID3D11Resource_Release(resource);
    if (++stage == 9) {
        puts("Sokol GPU colors, CRT shader, sidebar layout/toggling, mouse aim and letterbox checks passed");
        sapp_quit();
    }
}
sapp_desc sokol_main(int argc, char **argv) {
    (void)argc; (void)argv;
    return (sapp_desc){.init_cb = init, .frame_cb = frame, .cleanup_cb = host_shutdown,
        .width = 1000, .height = 600, .window_title = "Sokol rendering test", .disable_vsync = true};
}
