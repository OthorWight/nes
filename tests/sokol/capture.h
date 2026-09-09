#ifndef NES_TEST_CAPTURE_H
#define NES_TEST_CAPTURE_H
#ifdef _WIN32
#define COBJMACROS
#include <d3d11.h>
/* Test-only readback of the current Sokol window, before its next swap. */
static void capture_window(const char *path) {
    sapp_environment env = sapp_get_environment();
    sapp_swapchain swap = sapp_acquire_swapchain();
    ID3D11Device *device = (ID3D11Device *)env.d3d11.device;
    ID3D11DeviceContext *context = (ID3D11DeviceContext *)env.d3d11.device_context;
    ID3D11Resource *resource;
    ID3D11RenderTargetView_GetResource((ID3D11RenderTargetView *)swap.d3d11.render_view, &resource);
    D3D11_TEXTURE2D_DESC desc;
    ID3D11Texture2D_GetDesc((ID3D11Texture2D *)resource, &desc);
    desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = desc.MiscFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D *staging;
    assert(SUCCEEDED(ID3D11Device_CreateTexture2D(device, &desc, NULL, &staging)));
    ID3D11DeviceContext_CopyResource(context, (ID3D11Resource *)staging, resource);
    D3D11_MAPPED_SUBRESOURCE map;
    assert(SUCCEEDED(ID3D11DeviceContext_Map(context, (ID3D11Resource *)staging, 0, D3D11_MAP_READ, 0, &map)));
    assert(desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM);
    FILE *file = fopen(path, "wb"); assert(file);
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
}
#else
static void capture_window(const char *path) { (void)path; }
#endif
#endif
