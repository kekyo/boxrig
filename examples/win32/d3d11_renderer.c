/* boxrig - Offscreen rendering through an installed Direct3D 11 SDK. Under MIT. */
#define COBJMACROS
#include <boxrig/render/d3d11.h>
#include <objbase.h>
#include <stdio.h>
#include <string.h>

int main(int count, char **arguments)
{
    const char text[] = "\033[?25l\033[44mInstalled Direct3D 11 renderer\033[0m\r\n";
    ID3D11Device *device = NULL;
    ID3D11DeviceContext *context = NULL;
    ID3D11Texture2D *texture = NULL, *readback = NULL;
    D3D11_TEXTURE2D_DESC description = {0};
    D3D11_MAPPED_SUBRESOURCE mapped = {0};
    boxrig_d3d11_renderer_config config = boxrig_d3d11_renderer_config_default();
    boxrig_d3d11_renderer *renderer = NULL;
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *view = NULL;
    boxrig_renderer_frame_token token;
    HRESULT result = E_FAIL;
    const char *stage = "COM initialization";
    size_t consumed;
    bool check = false, warp = false, prepared = false, apartment = false;
    unsigned int x, y, colored = 0;
    int index, exit_code = 1;
    for (index = 1; index < count; index++) {
        if (strcmp(arguments[index], "--check") == 0) check = true;
        else if (strcmp(arguments[index], "--warp") == 0) warp = true;
        else { fprintf(stderr, "usage: d3d11_renderer [--check] [--warp]\n"); return 2; }
    }
    result = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(result)) goto cleanup;
    apartment = true;
    stage = "device creation";
    result = D3D11CreateDevice(NULL, warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE,
        NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT, NULL, 0, D3D11_SDK_VERSION,
        &device, NULL, &context);
    if (FAILED(result)) goto cleanup;
    description.Width = 640; description.Height = 120;
    description.MipLevels = description.ArraySize = description.SampleDesc.Count = 1;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_RENDER_TARGET;
    stage = "target creation";
    result = ID3D11Device_CreateTexture2D(device, &description, NULL, &texture);
    if (FAILED(result)) goto cleanup;
    config.device = device;
    config.device_generation = 1;
    config.target.texture = texture;
    config.target.width = description.Width; config.target.height = description.Height;
    config.target.format = description.Format;
    config.target.alpha = D2D1_ALPHA_MODE_IGNORE;
    config.target.generation = 1;
    stage = "renderer creation";
    result = boxrig_d3d11_renderer_create(&config, &renderer);
    if (FAILED(result)) goto cleanup;
    stage = "terminal/model/view initialization and frame preparation";
    terminal_config.rows = 6; terminal_config.columns = 64;
    if (boxrig_terminal_create(&terminal_config, &terminal) != BOXRIG_STATUS_OK ||
        boxrig_ui_model_create(terminal, &model) != BOXRIG_UI_OK ||
        boxrig_ui_view_create(model, &view_config, &view) != BOXRIG_UI_OK ||
        boxrig_terminal_feed(terminal, (const uint8_t *)text, sizeof(text) - 1, &consumed) != BOXRIG_STATUS_OK ||
        consumed != sizeof(text) - 1 || boxrig_ui_model_synchronize(model) != BOXRIG_UI_OK ||
        boxrig_d3d11_renderer_prepare_frame(renderer, view, NULL, 1, 1, &token) != BOXRIG_RENDERER_OK)
        goto cleanup;
    prepared = true;
    /* Only the application consumes or presents this borrowed target. Readback
     * here is the offscreen consumer; managed components do not need readback. */
    description.Usage = D3D11_USAGE_STAGING;
    description.BindFlags = 0; description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stage = "readback texture creation";
    result = ID3D11Device_CreateTexture2D(device, &description, NULL, &readback);
    if (FAILED(result)) goto cleanup;
    ID3D11DeviceContext_CopyResource(context, (ID3D11Resource *)readback, (ID3D11Resource *)texture);
    stage = "readback mapping";
    result = ID3D11DeviceContext_Map(context, (ID3D11Resource *)readback, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(result)) goto cleanup;
    for (y = 0; y < description.Height; y++) for (x = 0; x < description.Width; x++) {
        const uint8_t *pixel = (const uint8_t *)mapped.pData + y * mapped.RowPitch + x * 4;
        if (pixel[0] > pixel[1] + 30 && pixel[0] > pixel[2] + 30) colored++;
    }
    stage = "PPM output";
    if (!check) {
        FILE *output = fopen("boxrig-d3d11.ppm", "wb");
        bool written = output != NULL;
        if (output != NULL) {
            written = fprintf(output, "P6\n%u %u\n255\n", description.Width, description.Height) > 0;
            for (y = 0; written && y < description.Height; y++) for (x = 0; written && x < description.Width; x++) {
                const uint8_t *pixel = (const uint8_t *)mapped.pData + y * mapped.RowPitch + x * 4;
                const uint8_t rgb[3] = {pixel[2], pixel[1], pixel[0]};
                written = fwrite(rgb, 1, 3, output) == 3;
            }
            if (fclose(output) != 0) written = false;
        }
        if (!written) { ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)readback, 0); goto cleanup; }
    }
    ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)readback, 0);
    stage = "pixel verification and frame acceptance";
    if (colored == 0 || boxrig_d3d11_renderer_accept_frame(renderer, &token) != BOXRIG_RENDERER_OK)
        goto cleanup;
    prepared = false;
    puts("PASS installed Direct3D 11 offscreen renderer and caller-owned frame acceptance");
    exit_code = 0;
cleanup:
    if (exit_code != 0) fprintf(stderr, "Offscreen rendering failed at %s (last native HRESULT=0x%08lx).\n", stage, (unsigned long)result);
    if (prepared) (void)boxrig_d3d11_renderer_abort_frame(renderer, &token);
    if (renderer != NULL && FAILED(boxrig_d3d11_renderer_destroy(&renderer))) exit_code = 1;
    boxrig_ui_view_destroy(view);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    if (readback != NULL) ID3D11Texture2D_Release(readback);
    if (texture != NULL) ID3D11Texture2D_Release(texture);
    if (context != NULL) ID3D11DeviceContext_Release(context);
    if (device != NULL) ID3D11Device_Release(device);
    if (apartment) CoUninitialize();
    return exit_code;
}
