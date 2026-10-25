/* Borrowed offscreen renderer: no HWND, swapchain, or renderer readback. */
#define COBJMACROS
#include "boxrig/render/d3d11.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../../src/d3d11/renderer_private.h"
#include "boxrig/win32/renderer.h"
#include "../color_renderer_fixture.h"
#include "../blink_renderer_fixture.h"
#include "../selection_renderer_fixture.h"

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); return 1; \
} } while (0)

static int read_pixels(ID3D11Device *device, ID3D11DeviceContext *context,
    ID3D11Texture2D *texture, uint32_t *pixels)
{
    D3D11_TEXTURE2D_DESC desc;
    D3D11_MAPPED_SUBRESOURCE mapped;
    ID3D11Texture2D *staging = NULL;
    unsigned int y;
    ID3D11Texture2D_GetDesc(texture, &desc);
    desc.BindFlags = 0; desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    CHECK(SUCCEEDED(ID3D11Device_CreateTexture2D(device, &desc, NULL, &staging)));
    ID3D11DeviceContext_CopyResource(context, (ID3D11Resource *)staging,
        (ID3D11Resource *)texture);
    /* Blocking Map is the test's completion boundary, with no timing heuristic. */
    CHECK(SUCCEEDED(ID3D11DeviceContext_Map(context, (ID3D11Resource *)staging,
        0, D3D11_MAP_READ, 0, &mapped)));
    for (y = 0; y < desc.Height; y++) memcpy(pixels + y * desc.Width,
        (const uint8_t *)mapped.pData + y * mapped.RowPitch, desc.Width * 4);
    ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)staging, 0);
    ID3D11Texture2D_Release(staging);
    return 0;
}

/* Device/context lifetime belongs to main; fixtures own their target and renderer. */
static ID3D11Device *color_device;
static ID3D11DeviceContext *color_context;

typedef struct color_host {
    ID3D11Texture2D *texture;
    boxrig_d3d11_renderer *renderer;
    boxrig_d3d11_statistics baseline;
} color_host;

static int draw_colors(void *context, boxrig_ui_view *view,
    boxrig_composition *composition)
{
    color_host *host = context;
    boxrig_renderer_frame_token token;
    if (boxrig_d3d11_renderer_prepare_frame(host->renderer, view, composition,
            1, 1, &token) != BOXRIG_RENDERER_OK) return -1;
    return boxrig_d3d11_renderer_accept_frame(host->renderer, &token) ==
        BOXRIG_RENDERER_OK ? 0 : -1;
}

static uint32_t *capture_colors(void *context)
{
    color_host *host = context;
    size_t count = BOXRIG_TEST_COLOR_WIDTH * BOXRIG_TEST_COLOR_HEIGHT;
    uint32_t *pixels = malloc(count * sizeof(*pixels));
    if (pixels == NULL) return NULL;
    if (read_pixels(color_device, color_context, host->texture, pixels) != 0) {
        free(pixels);
        return NULL;
    }
    for (size_t i = 0; i < count; i++) pixels[i] &= 0xffffffu;
    return pixels;
}

static void reset_color_statistics(void *context)
{
    color_host *host = context;
    if (boxrig_d3d11_renderer_get_statistics(host->renderer, &host->baseline) == -1) abort();
}

static int color_statistics(void *context, boxrig_test_color_statistics *statistics)
{
    color_host *host = context;
    boxrig_d3d11_statistics actual;
    if (boxrig_d3d11_renderer_get_statistics(host->renderer, &actual) == -1) return -1;
    *statistics = (boxrig_test_color_statistics){
        actual.row_shape_count - host->baseline.row_shape_count,
        actual.row_raster_count - host->baseline.row_raster_count, 0};
    return 0;
}

static void destroy_colors(void *context)
{
    color_host *host = context;
    if (host->renderer != NULL &&
        FAILED(boxrig_d3d11_renderer_destroy(&host->renderer))) abort();
    if (host->texture != NULL) ID3D11Texture2D_Release(host->texture);
    free(host);
}

static bool cursor_style_scaled;
static bool font_fixture;

static bool create_colors(boxrig_test_color_renderer *renderer)
{
    color_host *host = calloc(1, sizeof(*host));
    D3D11_TEXTURE2D_DESC desc = {0};
    boxrig_d3d11_renderer_config config = boxrig_d3d11_renderer_config_default();
    boxrig_font_effective_metrics metrics;
    if (host == NULL) return false;
    desc.Width = BOXRIG_TEST_COLOR_WIDTH;
    desc.Height = BOXRIG_TEST_COLOR_HEIGHT;
    desc.MipLevels = 1; desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1; desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (FAILED(ID3D11Device_CreateTexture2D(color_device, &desc, NULL, &host->texture))) {
        destroy_colors(host);
        return false;
    }
    if (cursor_style_scaled) {
        config.target.dpi_x = 144;
        config.target.dpi_y = 144;
        config.rendering_policy.dpi_x = 144;
        config.rendering_policy.dpi_y = 144;
        config.rendering_policy.output_scale = 4.0f / 3.0f;
    }
    config.device = color_device;
    config.target.texture = host->texture;
    config.target.width = desc.Width; config.target.height = desc.Height;
    if (FAILED(boxrig_d3d11_renderer_create(&config, &host->renderer)) ||
        boxrig_d3d11_renderer_get_metrics(host->renderer, &metrics) == -1) {
        destroy_colors(host);
        return false;
    }
    if (font_fixture) {
        const char path[] = "tests/fixtures/width-fractional.otf";
        boxrig_font_resource_id resource;
        if (boxrig_d3d11_renderer_register_font_file(host->renderer, path, strlen(path), 0,
                &resource) != BOXRIG_FONT_CATALOG_OK) return false;
        boxrig_font_family family = {"Boxrig Width Fixture", 20, resource};
        boxrig_font_specification spec = boxrig_font_specification_default();
        spec.families = &family; spec.family_count = 1; spec.size_dip = 20;
        if (boxrig_d3d11_renderer_set_font(host->renderer, &spec, &config.rendering_policy) != BOXRIG_FONT_CATALOG_OK ||
            boxrig_d3d11_renderer_get_metrics(host->renderer, &metrics) == -1) return false;
    }
    *renderer = (boxrig_test_color_renderer){host,
        metrics.cell_width, metrics.cell_height, draw_colors, capture_colors,
        reset_color_statistics, color_statistics, destroy_colors};
    return true;
}

static int feed(boxrig_terminal *terminal, boxrig_ui_model *model, const char *text)
{
    size_t consumed;
    CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)text, strlen(text),
        &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == strlen(text));
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    return 0;
}

static int frame_contracts(boxrig_d3d11_renderer *renderer,
    boxrig_d3d11_renderer_config *config, ID3D11DeviceContext *context,
    boxrig_terminal *terminal, boxrig_ui_model *model, boxrig_ui_view *view)
{
    boxrig_renderer_frame_token token, stale;
    boxrig_d3d11_statistics before, after;
    boxrig_d3d11_diagnostic diagnostic;
    boxrig_font_effective_metrics metrics;
    boxrig_ui_dirty dirty;
    uint32_t pixels[256 * 128], original[256 * 128];
    unsigned int x, y, ink = 0;
    CHECK(boxrig_d3d11_renderer_get_metrics(renderer, &metrics) == 0);
    CHECK(boxrig_terminal_set_cell_pixel_geometry(terminal,
        metrics.cell_width, metrics.cell_height) == BOXRIG_STATUS_OK);
    /* Cursor movement marks both rows dirty even when hidden. Position it in the
     * edited row before accepting the baseline for the one-row update. */
    CHECK(feed(terminal, model, "\x1b[?25l\x1b]11;#ff0000\x07\x1b]10;#ffffff\x07"
        "Ab\x1b[2;1H") == 0);
    CHECK(boxrig_d3d11_renderer_prepare_frame(renderer, view, NULL, 1, 1, &token) == BOXRIG_RENDERER_OK);
    CHECK(boxrig_ui_view_peek_dirty(view, BOXRIG_UI_DIRTY_MODEL, &dirty));
    CHECK(read_pixels(config->device, context, config->target.texture, pixels) == 0);
    CHECK((pixels[100 * 256 + 200] >> 24) == 128);
    CHECK(((pixels[100 * 256 + 200] >> 16) & 255) == 128);
    for (y = 0; y < metrics.cell_height; y++) for (x = 0; x < 2 * metrics.cell_width; x++)
        if ((pixels[y * 256 + x] & 0xffff) != 0) ink++;
    CHECK(ink > 8);
    CHECK(boxrig_d3d11_renderer_accept_frame(renderer, &token) == BOXRIG_RENDERER_OK);
    memcpy(original, pixels, sizeof(pixels));
    CHECK(boxrig_ui_view_set_background_color(view, (boxrig_ui_rgba){0, 0, 255, 128}) == BOXRIG_UI_OK);
    CHECK(boxrig_d3d11_renderer_prepare_frame(renderer, view, NULL, 1, 1, &token) == BOXRIG_RENDERER_OK);
    CHECK(boxrig_d3d11_renderer_accept_frame(renderer, &token) == BOXRIG_RENDERER_OK);
    CHECK(read_pixels(config->device, context, config->target.texture, pixels) == 0);
    for (size_t i = 0; i < 256 * 128; i++) {
        uint32_t expected = boxrig_ui_argb32_source_over(original[i], UINT32_C(0x80000080));
        for (unsigned int shift = 0; shift < 32; shift += 8) {
            if (abs((int)((pixels[i] >> shift) & 255) - (int)((expected >> shift) & 255)) > 1)
                fprintf(stderr, "background color pixel=%zu original=%08lx expected=%08lx actual=%08lx\n",
                    i, (unsigned long)original[i], (unsigned long)expected, (unsigned long)pixels[i]);
            CHECK(abs((int)((pixels[i] >> shift) & 255) - (int)((expected >> shift) & 255)) <= 1);
        }
    }
    CHECK(boxrig_ui_view_set_background_color(view, (boxrig_ui_rgba){0}) == BOXRIG_UI_OK);
    CHECK(boxrig_d3d11_renderer_prepare_frame(renderer, view, NULL, 1, 1, &token) == BOXRIG_RENDERER_OK);
    CHECK(boxrig_d3d11_renderer_accept_frame(renderer, &token) == BOXRIG_RENDERER_OK);
    CHECK(read_pixels(config->device, context, config->target.texture, pixels) == 0);
    CHECK(memcmp(original, pixels, sizeof(pixels)) == 0);
    const uint8_t rgba[] = {0, 255, 0, 128};
    boxrig_ui_background_image background_image = {1, 1, 4, sizeof(rgba), rgba, 128, BOXRIG_UI_BACKGROUND_COVER};
    CHECK(boxrig_ui_view_set_background_image(view, &background_image) == BOXRIG_UI_OK);
    CHECK(boxrig_d3d11_renderer_prepare_frame(renderer, view, NULL, 1, 1, &token) == BOXRIG_RENDERER_OK);
    CHECK(boxrig_d3d11_renderer_accept_frame(renderer, &token) == BOXRIG_RENDERER_OK);
    CHECK(read_pixels(config->device, context, config->target.texture, pixels) == 0);
    for (size_t i = 0; i < 256 * 128; i++) {
        uint32_t expected = boxrig_ui_argb32_source_over(original[i], UINT32_C(0x40004000));
        for (unsigned int shift = 0; shift < 32; shift += 8)
            CHECK(abs((int)((pixels[i] >> shift) & 255) - (int)((expected >> shift) & 255)) <= 1);
    }
    for (int fit = BOXRIG_UI_BACKGROUND_CONTAIN; fit <= BOXRIG_UI_BACKGROUND_STRETCH; fit++) {
        background_image.fit = (boxrig_ui_background_fit)fit;
        CHECK(boxrig_ui_view_set_background_image(view, &background_image) == BOXRIG_UI_OK);
        CHECK(boxrig_ui_view_set_background_color(view, (boxrig_ui_rgba){0, 0, 255, 128}) == BOXRIG_UI_OK);
        for (unsigned int repeat = 0; repeat < 2; repeat++) {
            CHECK(boxrig_d3d11_renderer_prepare_frame(renderer, view, NULL, 1, 1, &token) == BOXRIG_RENDERER_OK);
            CHECK(boxrig_d3d11_renderer_accept_frame(renderer, &token) == BOXRIG_RENDERER_OK);
            CHECK(read_pixels(config->device, context, config->target.texture, pixels) == 0);
            for (size_t i = 0; i < 256 * 128; i++) {
                uint32_t expected = boxrig_ui_argb32_source_over(original[i], UINT32_C(0x80000080));
                if (fit == BOXRIG_UI_BACKGROUND_STRETCH || (i % 256 >= 64 && i % 256 < 192))
                    expected = boxrig_ui_argb32_source_over(expected, UINT32_C(0x40004000));
                for (unsigned int shift = 0; shift < 32; shift += 8)
                    CHECK(abs((int)((pixels[i] >> shift) & 255) - (int)((expected >> shift) & 255)) <= 1);
            }
        }
    }
    CHECK(boxrig_ui_view_set_background_color(view, (boxrig_ui_rgba){0}) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_set_background_image(view, NULL) == BOXRIG_UI_OK);
    CHECK(boxrig_d3d11_renderer_prepare_frame(renderer, view, NULL, 1, 1, &token) == BOXRIG_RENDERER_OK);
    CHECK(boxrig_d3d11_renderer_accept_frame(renderer, &token) == BOXRIG_RENDERER_OK);
    CHECK(read_pixels(config->device, context, config->target.texture, pixels) == 0);
    CHECK(memcmp(original, pixels, sizeof(pixels)) == 0);
    puts("PASS D3D11 background color and image behind terminal RGBA and removal");
    memcpy(original, pixels, sizeof(pixels));
    CHECK(boxrig_d3d11_renderer_get_statistics(renderer, &before) == 0);
    CHECK(feed(terminal, model, "\x1b[2;1H\x1b[44m \x1b[0m") == 0);
    CHECK(!boxrig_ui_view_get_row_dirty(view, BOXRIG_UI_DIRTY_MODEL, 0, &dirty));
    CHECK(boxrig_ui_view_get_row_dirty(view, BOXRIG_UI_DIRTY_MODEL, 1, &dirty));
    CHECK(!boxrig_ui_view_get_row_dirty(view, BOXRIG_UI_DIRTY_MODEL, 2, &dirty));
    CHECK(boxrig_d3d11_renderer_prepare_frame(renderer, view, NULL, 1, 1, &token) == BOXRIG_RENDERER_OK);
    CHECK(boxrig_d3d11_renderer_get_statistics(renderer, &after) == 0);
    CHECK(after.row_shape_count == before.row_shape_count + 1);
    CHECK(after.row_raster_count == before.row_raster_count + 1);
    CHECK(read_pixels(config->device, context, config->target.texture, pixels) == 0);
    CHECK(memcmp(original, pixels, metrics.cell_height * 256 * 4) == 0);
    CHECK(boxrig_d3d11_renderer_abort_frame(renderer, &token) == BOXRIG_RENDERER_OK);
    CHECK(boxrig_ui_view_peek_dirty(view, BOXRIG_UI_DIRTY_MODEL, &dirty));
    stale = token;
    CHECK(boxrig_d3d11_renderer_prepare_frame(renderer, view, NULL, 1, 1, &token) == BOXRIG_RENDERER_OK);
    CHECK(boxrig_d3d11_renderer_accept_frame(renderer, &stale) != BOXRIG_RENDERER_OK);
    CHECK(boxrig_d3d11_renderer_accept_frame(renderer, &token) == BOXRIG_RENDERER_OK);
    CHECK(boxrig_d3d11_renderer_get_statistics(renderer, &before) == 0);
    dirty = (boxrig_ui_dirty){0, 1, 0, 1};
    CHECK(boxrig_ui_view_invalidate(view, BOXRIG_UI_DIRTY_OVERLAY, dirty) == BOXRIG_UI_OK);
    CHECK(boxrig_d3d11_renderer_prepare_frame(renderer, view, NULL, 1, 1, &token) == BOXRIG_RENDERER_OK);
    CHECK(boxrig_d3d11_renderer_accept_frame(renderer, &token) == BOXRIG_RENDERER_OK);
    CHECK(boxrig_d3d11_renderer_get_statistics(renderer, &after) == 0);
    CHECK(after.row_shape_count == before.row_shape_count);
    CHECK(after.row_raster_count == before.row_raster_count + 1);
    CHECK(feed(terminal, model, "\x1b_Ga=t,i=91,f=32,s=1,v=1;AAD/gA==\x1b\\"
        "\x1b[3;1H\x1b_Ga=p,i=91,p=1,c=1,r=1,z=0,C=1\x1b\\") == 0);
    CHECK(boxrig_d3d11_renderer_get_statistics(renderer, &before) == 0);
    CHECK(boxrig_d3d11_renderer_prepare_frame(renderer, view, NULL, 1, 1, &token) == BOXRIG_RENDERER_OK);
    CHECK(boxrig_d3d11_renderer_accept_frame(renderer, &token) == BOXRIG_RENDERER_OK);
    CHECK(boxrig_d3d11_renderer_get_statistics(renderer, &after) == 0);
    CHECK(after.row_shape_count == before.row_shape_count);
    CHECK(after.image_cache_upload_count == before.image_cache_upload_count + 1);
    CHECK(after.image_composite_count > before.image_composite_count);
    CHECK(read_pixels(config->device, context, config->target.texture, pixels) == 0);
    x = metrics.cell_width / 2; y = 2 * metrics.cell_height + metrics.cell_height / 2;
    CHECK((pixels[y * 256 + x] & 255) >= 127);
    CHECK((pixels[y * 256 + x] >> 24) >= 191 && (pixels[y * 256 + x] >> 24) <= 193);
    CHECK(boxrig_ui_view_invalidate(view, BOXRIG_UI_DIRTY_PRESENT, dirty) == BOXRIG_UI_OK);
    CHECK(feed(terminal, model, "\x1b_Ga=t,i=92,f=32,s=1,v=1;AP8A/w==\x1b\\"
        "\x1b[3;2H\x1b_Ga=p,i=92,p=2,c=1,r=1,z=0,C=1\x1b\\") == 0);
    CHECK(boxrig_d3d11_renderer_prepare_frame(renderer, view, NULL, 1, 1, &token) == BOXRIG_RENDERER_OK);
    CHECK(boxrig_d3d11_renderer_accept_frame(renderer, &token) == BOXRIG_RENDERER_OK);
    CHECK(boxrig_d3d11_renderer_get_statistics(renderer, &after) == 0);
    CHECK(after.image_entries <= config->maximum_image_entries);
    CHECK(after.image_bytes <= config->maximum_image_bytes && after.image_eviction_count > 0);
    CHECK(after.peak_image_entries == 1 && after.peak_image_bytes == 4);
    CHECK(boxrig_d3d11_renderer_set_scrollbar(renderer, true, 8, false, false) == 0);
    CHECK(boxrig_d3d11_renderer_prepare_frame(renderer, view, NULL, 1, 1, &token) == BOXRIG_RENDERER_OK);
    CHECK(boxrig_d3d11_renderer_accept_frame(renderer, &token) == BOXRIG_RENDERER_OK);
    CHECK(read_pixels(config->device, context, config->target.texture, pixels) == 0);
    CHECK((pixels[64 * 256 + 252] >> 24) == 255);
    CHECK((pixels[64 * 256 + 252] & 0xffffff) != 0xff0000);
    CHECK(boxrig_d3d11_renderer_set_scrollbar(renderer, false, 8, false, false) == 0);
    CHECK(boxrig_d3d11_renderer_prepare_frame(renderer, view, NULL, 1, 1, &token) == BOXRIG_RENDERER_OK);
    CHECK(boxrig_d3d11_renderer_accept_frame(renderer, &token) == BOXRIG_RENDERER_OK);
    CHECK(read_pixels(config->device, context, config->target.texture, pixels) == 0);
    CHECK(pixels[64 * 256 + 252] == 0x80800000u);
    CHECK(boxrig_ui_view_invalidate(view, BOXRIG_UI_DIRTY_PRESENT, dirty) == BOXRIG_UI_OK);
    boxrig_d3d11_renderer_fail_next(renderer, E_OUTOFMEMORY);
    CHECK(boxrig_d3d11_renderer_prepare_frame(renderer, view, NULL, 1, 1, &token) != BOXRIG_RENDERER_OK);
    CHECK(boxrig_ui_view_peek_dirty(view, BOXRIG_UI_DIRTY_PRESENT, &dirty));
    CHECK(boxrig_d3d11_renderer_prepare_frame(renderer, view, NULL, 1, 1, &token) == BOXRIG_RENDERER_OK);
    CHECK(boxrig_d3d11_renderer_accept_frame(renderer, &token) == BOXRIG_RENDERER_OK);
    boxrig_d3d11_renderer_fail_next(renderer, D2DERR_RECREATE_TARGET);
    CHECK(boxrig_d3d11_renderer_prepare_frame(renderer, view, NULL, 1, 1, &token) != BOXRIG_RENDERER_OK);
    CHECK(boxrig_d3d11_renderer_get_diagnostic(renderer, &diagnostic) == 0);
    CHECK(diagnostic.result == (HRESULT)D2DERR_RECREATE_TARGET && diagnostic.loss == BOXRIG_D3D11_LOSS_TARGET);
    CHECK(boxrig_d3d11_renderer_get_statistics(renderer, &after) == 0);
    CHECK(after.image_entries == 0 && after.image_bytes == 0);
    CHECK(after.peak_image_entries == 1 && after.peak_image_bytes == 4);
    CHECK(boxrig_ui_view_peek_dirty(view, BOXRIG_UI_DIRTY_PRESENT, &dirty));
    config->target.generation++;
    CHECK(SUCCEEDED(boxrig_d3d11_renderer_set_target(renderer, 1, &config->target)));
    CHECK(boxrig_d3d11_renderer_prepare_frame(renderer, view, NULL, 1, 1, &token) != BOXRIG_RENDERER_OK);
    CHECK(boxrig_d3d11_renderer_prepare_frame(renderer, view, NULL, 1, 2, &token) == BOXRIG_RENDERER_OK);
    CHECK(boxrig_d3d11_renderer_accept_frame(renderer, &token) == BOXRIG_RENDERER_OK);
    boxrig_d3d11_renderer_fail_next(renderer, DXGI_ERROR_DEVICE_RESET);
    CHECK(boxrig_d3d11_renderer_prepare_frame(renderer, view, NULL, 1, 2, &token) != BOXRIG_RENDERER_OK);
    CHECK(boxrig_d3d11_renderer_get_diagnostic(renderer, &diagnostic) == 0);
    CHECK(diagnostic.loss == BOXRIG_D3D11_LOSS_DEVICE);
    config->target.generation++;
    CHECK(FAILED(boxrig_d3d11_renderer_set_target(renderer, 1, &config->target)));
    puts("PASS pixels, dirty, images, accept/abort, target and device loss");
    return 0;
}

static DWORD WINAPI wrong_thread(void *context)
{
    boxrig_d3d11_renderer *renderer = context;
    boxrig_font_effective_metrics metrics;
    return boxrig_d3d11_renderer_get_metrics(renderer, &metrics) == -1 &&
        boxrig_d3d11_renderer_destroy(&renderer) == RPC_E_WRONG_THREAD ? 0 : 1;
}

static int format_pixels(ID3D11Device *device, ID3D11DeviceContext *context,
    boxrig_ui_view *view)
{
    size_t f, a;
    const DXGI_FORMAT formats[] = {DXGI_FORMAT_B8G8R8A8_UNORM,
        DXGI_FORMAT_B8G8R8A8_UNORM_SRGB};
    const D2D1_ALPHA_MODE alphas[] = {D2D1_ALPHA_MODE_PREMULTIPLIED,
        D2D1_ALPHA_MODE_IGNORE};
    uint32_t pixels[256 * 128];
    for (f = 0; f < 2; f++) for (a = 0; a < 2; a++) {
        boxrig_d3d11_renderer_config config = boxrig_d3d11_renderer_config_default();
        boxrig_d3d11_renderer *renderer = NULL;
        boxrig_d3d11_diagnostic diagnostic;
        boxrig_renderer_frame_token token;
        ID3D11Texture2D *texture = NULL;
        ID3D11RenderTargetView *guard = NULL, *bound = NULL;
        D3D11_TEXTURE2D_DESC desc = {0};
        D3D11_VIEWPORT viewport = {3, 4, 29, 31, 0.2f, 0.8f}, after;
        D3D11_PRIMITIVE_TOPOLOGY topology;
        UINT count = 1;
        HANDLE thread;
        DWORD exit_code;
        unsigned int red;
        desc.Width = 256; desc.Height = 128; desc.MipLevels = 1; desc.ArraySize = 1;
        desc.Format = formats[f]; desc.SampleDesc.Count = 1;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        CHECK(SUCCEEDED(ID3D11Device_CreateTexture2D(device, &desc, NULL, &texture)));
        config.device = device;
        config.target.texture = texture;
        config.target.width = desc.Width; config.target.height = desc.Height;
        config.target.format = formats[f]; config.target.alpha = alphas[a];
        config.background_opacity = 128;
        config.rendering_policy.antialias = BOXRIG_FONT_ANTIALIAS_SUBPIXEL;
        CHECK(SUCCEEDED(boxrig_d3d11_renderer_create(&config, &renderer)));
        CHECK(boxrig_d3d11_renderer_get_diagnostic(renderer, &diagnostic) == 0);
        CHECK(diagnostic.text_antialias == (a == 0 ? D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE :
            D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE));
        thread = CreateThread(NULL, 0, wrong_thread, renderer, 0, NULL);
        CHECK(thread != NULL && WaitForSingleObject(thread, INFINITE) == WAIT_OBJECT_0);
        CHECK(GetExitCodeThread(thread, &exit_code) && exit_code == 0);
        CloseHandle(thread);
        CHECK(SUCCEEDED(ID3D11Device_CreateRenderTargetView(device,
            (ID3D11Resource *)texture, NULL, &guard)));
        ID3D11DeviceContext_OMSetRenderTargets(context, 1, &guard, NULL);
        ID3D11DeviceContext_RSSetViewports(context, 1, &viewport);
        ID3D11DeviceContext_IASetPrimitiveTopology(context, D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP);
        CHECK(boxrig_d3d11_renderer_prepare_frame(renderer, view, NULL, 1, 1, &token) == BOXRIG_RENDERER_OK);
        CHECK(boxrig_d3d11_renderer_accept_frame(renderer, &token) == BOXRIG_RENDERER_OK);
        ID3D11DeviceContext_RSGetViewports(context, &count, &after);
        CHECK(count == 1 && memcmp(&viewport, &after, sizeof(viewport)) == 0);
        ID3D11DeviceContext_IAGetPrimitiveTopology(context, &topology);
        CHECK(topology == D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP);
        ID3D11DeviceContext_OMGetRenderTargets(context, 1, &bound, NULL);
        CHECK(bound == guard);
        ID3D11RenderTargetView_Release(bound);
        ID3D11DeviceContext_OMSetRenderTargets(context, 0, NULL, NULL);
        ID3D11RenderTargetView_Release(guard);
        CHECK(read_pixels(device, context, texture, pixels) == 0);
        CHECK((pixels[100 * 256 + 200] >> 24) == (a == 0 ? 128u : 255u));
        red = (pixels[100 * 256 + 200] >> 16) & 255;
        CHECK(a == 1 ? red == 255 : f == 0 ? red == 128 : red >= 186 && red <= 189);
        printf("PASS format=%u alpha=%u red=%u caller state and thread affinity\n",
            (unsigned int)formats[f], (unsigned int)alphas[a], red);
        CHECK(SUCCEEDED(boxrig_d3d11_renderer_destroy(&renderer)));
        ID3D11Texture2D_Release(texture);
    }
    return 0;
}

static int invalid_targets(ID3D11Device *device, ID3D11Texture2D *texture,
    D3D_DRIVER_TYPE driver, boxrig_ui_view *view)
{
    boxrig_d3d11_renderer_config config = boxrig_d3d11_renderer_config_default();
    boxrig_d3d11_renderer_config invalid;
    boxrig_d3d11_renderer *renderer = NULL;
    ID3D11Device *other = NULL;
    IDXGISurface *surface = NULL;
    unsigned int i;
    config.device = device;
    config.target.texture = texture;
    config.target.width = 256; config.target.height = 128;
    CHECK(SUCCEEDED(D3D11CreateDevice(NULL, driver, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        NULL, 0, D3D11_SDK_VERSION, &other, NULL, NULL)));
    invalid = config; invalid.device = other;
    CHECK(boxrig_d3d11_renderer_create(&invalid, &renderer) == E_INVALIDARG && renderer == NULL);
    for (i = 0; i < 9; i++) {
        invalid = config;
        switch (i) {
        case 0: invalid.target.alpha = D2D1_ALPHA_MODE_UNKNOWN; break;
        case 1: invalid.target.alpha = D2D1_ALPHA_MODE_STRAIGHT; break;
        case 2: invalid.target.format = DXGI_FORMAT_B8G8R8A8_TYPELESS; break;
        case 3: invalid.target.width++; break;
        case 4: invalid.target.generation = 0; break;
        case 5: invalid.device_generation = 0; break;
        case 6: invalid.target.dpi_x = 0; break;
        case 7: invalid.maximum_image_entries = 0; break;
        default: invalid.target.texture = NULL; break;
        }
        CHECK(boxrig_d3d11_renderer_create(&invalid, &renderer) == E_INVALIDARG && renderer == NULL);
    }
    for (i = 0; i < 3; i++) {
        D3D11_TEXTURE2D_DESC desc = {0};
        ID3D11Texture2D *bad = NULL;
        desc.Width = 256; desc.Height = 128; desc.MipLevels = 1; desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = i == 0 ? 4 : 1;
        desc.BindFlags = i == 1 ? D3D11_BIND_SHADER_RESOURCE : D3D11_BIND_RENDER_TARGET;
        if (i == 2) { desc.BindFlags = 0; desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ; }
        CHECK(SUCCEEDED(ID3D11Device_CreateTexture2D(device, &desc, NULL, &bad)));
        invalid = config; invalid.target.texture = bad;
        CHECK(FAILED(boxrig_d3d11_renderer_create(&invalid, &renderer)) && renderer == NULL);
        ID3D11Texture2D_Release(bad);
    }
    CHECK(SUCCEEDED(ID3D11Texture2D_QueryInterface(texture, &IID_IDXGISurface, (void **)&surface)));
    config.target.texture = NULL; config.target.surface = surface;
    CHECK(SUCCEEDED(boxrig_d3d11_renderer_create(&config, &renderer)));
    IDXGISurface_Release(surface);
    config.target.generation = 1;
    CHECK(boxrig_d3d11_renderer_set_target(renderer, 1, &config.target) == E_INVALIDARG);
    CHECK(SUCCEEDED(boxrig_d3d11_renderer_destroy(&renderer)));
    /* The renderer retains its own references even if the caller violates the
     * documented teardown order and drops every device/texture reference first. */
    {
        D3D11_TEXTURE2D_DESC desc = {0};
        boxrig_renderer_frame_token token;
        ID3D11Texture2D *owned = NULL;
        desc.Width = 256; desc.Height = 128; desc.MipLevels = 1; desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1; desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        CHECK(SUCCEEDED(ID3D11Device_CreateTexture2D(other, &desc, NULL, &owned)));
        config.device = other; config.target.surface = NULL; config.target.texture = owned;
        CHECK(SUCCEEDED(boxrig_d3d11_renderer_create(&config, &renderer)));
        ID3D11Texture2D_Release(owned);
        ID3D11Device_Release(other);
        CHECK(boxrig_d3d11_renderer_prepare_frame(renderer, view, NULL, 1, 1, &token) == BOXRIG_RENDERER_OK);
        CHECK(boxrig_d3d11_renderer_accept_frame(renderer, &token) == BOXRIG_RENDERER_OK);
        CHECK(SUCCEEDED(boxrig_d3d11_renderer_destroy(&renderer)));
    }
    puts("PASS invalid target matrix, surface descriptor, retained COM ownership");
    return 0;
}

static int font_contracts(ID3D11Device *device, ID3D11Texture2D *texture,
    boxrig_terminal *terminal, boxrig_ui_model *model, boxrig_ui_view *view)
{
    boxrig_d3d11_renderer_config config = boxrig_d3d11_renderer_config_default();
    boxrig_d3d11_renderer *renderer = NULL;
    boxrig_win32_renderer *software = NULL;
    boxrig_win32_shape_statistics statistics;
    boxrig_text_layout *native = NULL, *gdi = NULL;
    boxrig_font_specification font = boxrig_font_specification_default();
    boxrig_font_resource_id file_resource, memory_resource;
    boxrig_font_family family = {0};
    boxrig_font_effective_metrics metrics;
    boxrig_renderer_frame_token token;
    HDC dc = CreateCompatibleDC(NULL);
    char path[MAX_PATH];
    FILE *input;
    long length;
    uint8_t *bytes;
    size_t column;
    config.device = device; config.target.texture = texture;
    config.target.width = 256; config.target.height = 128;
    CHECK(SUCCEEDED(boxrig_d3d11_renderer_create(&config, &renderer)));
    CHECK(dc != NULL && boxrig_win32_renderer_create(dc, &software) == 0);
    CHECK(feed(terminal, model, "\x1b[2J\x1b[1;1Habc \xd7\x90\xd7\x91\xd7\x92 \xe7\x95\x8c") == 0);
    CHECK(boxrig_d3d11_renderer_layout_line(renderer, terminal, 0, &native) == 0);
    CHECK(boxrig_win32_renderer_layout_line(software, terminal, 0, &gdi, &statistics) == 0);
    CHECK(boxrig_text_layout_column_count(native) == boxrig_text_layout_column_count(gdi));
    for (column = 0; column < boxrig_text_layout_column_count(native); column++) {
        size_t first, second;
        CHECK(boxrig_text_layout_logical_to_visual(native, column, &first) == BOXRIG_TEXT_LAYOUT_OK);
        CHECK(boxrig_text_layout_logical_to_visual(gdi, column, &second) == BOXRIG_TEXT_LAYOUT_OK);
        CHECK(first == second);
    }
    boxrig_text_layout_destroy(native); boxrig_text_layout_destroy(gdi);
    boxrig_win32_renderer_destroy(software); DeleteDC(dc);
    CHECK(GetWindowsDirectoryA(path, MAX_PATH) > 0 && strlen(path) < MAX_PATH - 20);
    strcat(path, "\\Fonts\\consola.ttf");
    input = fopen(path, "rb");
    CHECK(input != NULL && fseek(input, 0, SEEK_END) == 0);
    length = ftell(input);
    CHECK(length > 0 && fseek(input, 0, SEEK_SET) == 0);
    bytes = malloc((size_t)length);
    CHECK(bytes != NULL && fread(bytes, 1, (size_t)length, input) == (size_t)length);
    fclose(input);
    CHECK(boxrig_d3d11_renderer_register_font_file(renderer, path, strlen(path), 0,
        &file_resource) == BOXRIG_FONT_CATALOG_OK);
    CHECK(boxrig_d3d11_renderer_register_font_memory(renderer, bytes, (size_t)length, 0,
        &memory_resource) == BOXRIG_FONT_CATALOG_OK);
    free(bytes);
    CHECK(file_resource != BOXRIG_FONT_RESOURCE_NONE && memory_resource != BOXRIG_FONT_RESOURCE_NONE);
    family.resource_id = memory_resource;
    font.families = &family; font.family_count = 1;
    font.size_dip = 18;
    CHECK(boxrig_d3d11_renderer_set_font(renderer, &font, &config.rendering_policy) == BOXRIG_FONT_CATALOG_OK);
    CHECK(boxrig_d3d11_renderer_get_metrics(renderer, &metrics) == 0);
    CHECK(metrics.primary_resource_id == memory_resource);
    CHECK(boxrig_d3d11_renderer_prepare_frame(renderer, view, NULL, 1, 1, &token) == BOXRIG_RENDERER_OK);
    CHECK(boxrig_d3d11_renderer_accept_frame(renderer, &token) == BOXRIG_RENDERER_OK);
    CHECK(SUCCEEDED(boxrig_d3d11_renderer_destroy(&renderer)));
    puts("PASS shared DirectWrite bidi/fallback layout and file/memory fonts");
    return 0;
}

int boxrig_test_d3d11_fixture(D3D_DRIVER_TYPE driver);

int main(int argc, char **argv)
{
    ID3D11Device *device = NULL;
    ID3D11DeviceContext *context = NULL;
    ID3D11Texture2D *texture = NULL;
    D3D11_TEXTURE2D_DESC desc = {0};
    boxrig_d3d11_renderer_config config = boxrig_d3d11_renderer_config_default();
    boxrig_d3d11_renderer *renderer = NULL;
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *view = NULL;
    boxrig_renderer_frame_token token;
    boxrig_ui_dirty dirty;
    D3D_DRIVER_TYPE driver = argc > 1 && strcmp(argv[1], "--hardware") == 0 ?
        D3D_DRIVER_TYPE_HARDWARE : D3D_DRIVER_TYPE_WARP;
    const bool wine = GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version") != NULL;
    const bool physical = argc > 2 && strcmp(argv[2], "--physical") == 0;
    HRESULT creation_result;
    if (physical) {
        CHECK(!wine && GetSystemMetrics(SM_REMOTESESSION) == 0);
    }
    CHECK(SUCCEEDED(D3D11CreateDevice(NULL, driver, NULL,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, NULL, 0, D3D11_SDK_VERSION,
        &device, NULL, &context)));
    {
        IDXGIDevice *dxgi = NULL;
        IDXGIAdapter *adapter = NULL;
        DXGI_ADAPTER_DESC adapter_desc;
        CHECK(SUCCEEDED(ID3D11Device_QueryInterface(device, &IID_IDXGIDevice, (void **)&dxgi)));
        CHECK(SUCCEEDED(IDXGIDevice_GetAdapter(dxgi, &adapter)));
        CHECK(SUCCEEDED(IDXGIAdapter_GetDesc(adapter, &adapter_desc)));
        printf("adapter vendor=0x%x device=0x%x\n", adapter_desc.VendorId, adapter_desc.DeviceId);
        IDXGIAdapter_Release(adapter); IDXGIDevice_Release(dxgi);
    }
    desc.Width = 256; desc.Height = 128;
    desc.MipLevels = 1; desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1; desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    CHECK(SUCCEEDED(ID3D11Device_CreateTexture2D(device, &desc, NULL, &texture)));
    config.device = device;
    config.background_opacity = 128;
    config.maximum_image_entries = 1;
    config.maximum_image_bytes = 4;
    config.target.texture = texture;
    config.target.width = desc.Width; config.target.height = desc.Height;
    creation_result = boxrig_d3d11_renderer_create(&config, &renderer);
    if (FAILED(creation_result)) {
        CHECK(!physical && renderer == NULL);
        printf("SKIP D3D11 raster acceptance: resource creation returned 0x%08lx; physical P2 requires success\n",
            (unsigned long)creation_result);
    } else {
        {
            boxrig_d3d11_diagnostic capabilities;
            CHECK(boxrig_d3d11_renderer_get_diagnostic(renderer, &capabilities) == 0);
            CHECK(capabilities.feature_level == ID3D11Device_GetFeatureLevel(device));
            CHECK(capabilities.bgra8_target && capabilities.srgb_target);
            CHECK(capabilities.opaque_target && capabilities.premultiplied_target);
            CHECK(capabilities.maximum_target_extent >= desc.Width);
            CHECK(capabilities.maximum_target_extent <= D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION);
        }
        terminal_config.rows = 3; terminal_config.columns = 12;
        CHECK(boxrig_terminal_create(&terminal_config, &terminal) == BOXRIG_STATUS_OK);
        CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
        CHECK(boxrig_ui_view_create(model, &view_config, &view) == BOXRIG_UI_OK);
        CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
        CHECK(boxrig_d3d11_renderer_prepare_frame(renderer, view, NULL, 1, 1, &token) == BOXRIG_RENDERER_OK);
        CHECK(boxrig_ui_view_peek_dirty(view, BOXRIG_UI_DIRTY_MODEL, &dirty));
        CHECK(boxrig_d3d11_renderer_accept_frame(renderer, &token) == BOXRIG_RENDERER_OK);
        CHECK(!boxrig_ui_view_peek_dirty(view, BOXRIG_UI_DIRTY_MODEL, &dirty));
        CHECK(boxrig_d3d11_renderer_accept_frame(renderer, &token) != BOXRIG_RENDERER_OK);
        CHECK(frame_contracts(renderer, &config, context, terminal, model, view) == 0);
        CHECK(SUCCEEDED(boxrig_d3d11_renderer_destroy(&renderer)));
        CHECK(format_pixels(device, context, view) == 0);
        CHECK(invalid_targets(device, texture, driver, view) == 0);
        CHECK(font_contracts(device, texture, terminal, model, view) == 0);
        boxrig_ui_view_destroy(view);
        boxrig_ui_model_destroy(model);
        boxrig_terminal_destroy(terminal);
        CHECK(boxrig_test_d3d11_fixture(driver) == 0);
        color_device = device;
        color_context = context;
        const boxrig_test_decoration_factory factories[] = {
            boxrig_test_create_win32_decoration, create_colors,
            boxrig_test_create_win32_decoration};
        const char *const names[] = {"GDI", "D3D11", "GDI restored"};
        CHECK(boxrig_test_renderer_decoration_views(factories, names, 3));
        CHECK(boxrig_test_renderer_colors(create_colors));
        font_fixture = true;
        CHECK(boxrig_test_renderer_font_positions(create_colors));
        font_fixture = false;
        puts("PASS D3D11 display color fixture");
        CHECK(boxrig_test_renderer_filled_cursor(create_colors, 1));
        CHECK(boxrig_test_renderer_selection_style(create_colors, 3, 3));
        CHECK((boxrig_test_renderer_cursor_style(create_colors) && boxrig_test_renderer_cursor_blink(create_colors)));
        CHECK(boxrig_test_renderer_text_blink(create_colors));
        CHECK(boxrig_test_renderer_blink_composition(create_colors));
        puts("PASS D3D11 cursor blink fixture");
        cursor_style_scaled = true;
        CHECK(boxrig_test_renderer_filled_cursor(create_colors, 2));
        CHECK(boxrig_test_renderer_selection_style(create_colors, 6, 6));
        CHECK(boxrig_test_renderer_cursor_style_at_scale(create_colors, 6, 6));
        cursor_style_scaled = false;
        color_device = NULL;
        color_context = NULL;
        puts("PASS d3d11 borrowed renderer");
    }
    ID3D11Texture2D_Release(texture);
    ID3D11DeviceContext_Release(context);
    ID3D11Device_Release(device);
    return 0;
}
