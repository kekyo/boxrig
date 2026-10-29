/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "boxrig/ui.h"

#if defined(BOXRIG_TEST_FRONTEND_X11)
#include "boxrig/x11.h"
#include "ui/renderer_lifecycle.h"
#include "x11/renderer_private.h"
#include <X11/Xutil.h>
#include <cairo/cairo.h>
#include <fontconfig/fontconfig.h>
#elif defined(BOXRIG_TEST_FRONTEND_D3D11)
#define COBJMACROS
#include "boxrig/render/d3d11.h"
static D3D_DRIVER_TYPE d3d11_fixture_driver;
#elif defined(BOXRIG_TEST_FRONTEND_WIN32)
#include "boxrig/win32.h"
#elif defined(BOXRIG_TEST_FRONTEND_PANGO_CAIRO)
#include "pangocairo/renderer.h"
#include "pangocairo/renderer_private.h"
#include <fontconfig/fontconfig.h>
#elif defined(BOXRIG_TEST_FRONTEND_OPENGL)
#include "opengl/renderer.h"
#include "opengl_host.h"
#include <cairo/cairo.h>
#include <fontconfig/fontconfig.h>
#else
#error A platform fixture frontend must be selected
#endif

#include <stdbool.h>
#include <stdint.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            return false;                                                       \
        }                                                                       \
    } while (0)

static boxrig_ui_fixed pixels(int64_t value)
{
    return value * BOXRIG_UI_FIXED_ONE;
}

static bool take_content_size_action(
    boxrig_ui_view *view,
    boxrig_ui_size_command *command_out)
{
    boxrig_ui_layout_action action;

    if (!boxrig_ui_view_take_layout_action(view, &action) ||
        action.kind != BOXRIG_UI_LAYOUT_ACTION_CONTENT_SIZE) {
        return false;
    }
    command_out->revision = action.revision;
    command_out->cause = action.value.content_size.cause;
    command_out->width = action.value.content_size.width;
    command_out->height = action.value.content_size.height;
    return true;
}

static bool take_grid_resize_action(
    boxrig_ui_view *view,
    boxrig_ui_grid_resize *resize_out)
{
    boxrig_ui_layout_action action;

    if (!boxrig_ui_view_take_layout_action(view, &action) ||
        action.kind != BOXRIG_UI_LAYOUT_ACTION_GRID_RESIZE) {
        return false;
    }
    resize_out->revision = action.revision;
    resize_out->rows = action.value.grid_resize.rows;
    resize_out->columns = action.value.grid_resize.columns;
    return true;
}

typedef struct platform_renderer_fixture {
#if defined(BOXRIG_TEST_FRONTEND_X11)
    Display *display;
    Window window;
    boxrig_x11_renderer *renderer;
#elif defined(BOXRIG_TEST_FRONTEND_D3D11)
    ID3D11Device *device;
    ID3D11DeviceContext *context;
    ID3D11Texture2D *texture;
    boxrig_d3d11_target target;
    boxrig_d3d11_renderer *renderer;
#elif defined(BOXRIG_TEST_FRONTEND_WIN32)
    HDC reference_context;
    boxrig_win32_renderer *renderer;
#elif defined(BOXRIG_TEST_FRONTEND_OPENGL)
    opengl_host *host;
    boxrig_ogl_renderer *renderer;
    boxrig_gl_target target;
    uint32_t target_texture;
    uint64_t context_generation;
#else
    boxrig_pango_cairo_renderer *renderer;
#endif
    unsigned int cell_width;
    unsigned int cell_height;
} platform_renderer_fixture;

typedef struct platform_rgb {
    uint8_t red;
    uint8_t green;
    uint8_t blue;
} platform_rgb;

static bool feed_bytes(
    boxrig_terminal *terminal,
    const char *bytes,
    size_t length)
{
    size_t consumed = 0;

    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)bytes,
              length,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == length);
    return true;
}

static bool create_platform_renderer(platform_renderer_fixture *fixture)
{
    memset(fixture, 0, sizeof(*fixture));
#if defined(BOXRIG_TEST_FRONTEND_X11)
    {
        int screen;
        Window root;

        fixture->display = XOpenDisplay(NULL);
        CHECK(fixture->display != NULL);
        screen = DefaultScreen(fixture->display);
        root = RootWindow(fixture->display, screen);
        fixture->window = XCreateSimpleWindow(
            fixture->display,
            root,
            0,
            0,
            64,
            64,
            0,
            BlackPixel(fixture->display, screen),
            BlackPixel(fixture->display, screen));
        CHECK(fixture->window != None);
        CHECK(boxrig_x11_renderer_create(
                  fixture->display,
                  fixture->window,
                  &fixture->renderer) == 0);
        fixture->cell_width =
            boxrig_x11_renderer_cell_width(fixture->renderer);
        fixture->cell_height =
            boxrig_x11_renderer_cell_height(fixture->renderer);
    }
#elif defined(BOXRIG_TEST_FRONTEND_D3D11)
    {
        boxrig_d3d11_renderer_config config = boxrig_d3d11_renderer_config_default();
        D3D11_TEXTURE2D_DESC desc = {0};
        boxrig_font_effective_metrics metrics;
        CHECK(SUCCEEDED(D3D11CreateDevice(NULL, d3d11_fixture_driver, NULL,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, NULL, 0, D3D11_SDK_VERSION,
            &fixture->device, NULL, &fixture->context)));
        desc.Width = 1; desc.Height = 1; desc.MipLevels = 1; desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count = 1;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        CHECK(SUCCEEDED(ID3D11Device_CreateTexture2D(fixture->device, &desc,
            NULL, &fixture->texture)));
        config.device = fixture->device;
        config.target.texture = fixture->texture;
        config.target.width = 1; config.target.height = 1;
        fixture->target = config.target;
        CHECK(SUCCEEDED(boxrig_d3d11_renderer_create(&config, &fixture->renderer)));
        CHECK(boxrig_d3d11_renderer_get_metrics(fixture->renderer, &metrics) == 0);
        fixture->cell_width = metrics.cell_width;
        fixture->cell_height = metrics.cell_height;
    }
#elif defined(BOXRIG_TEST_FRONTEND_WIN32)
    fixture->reference_context = CreateCompatibleDC(NULL);
    CHECK(fixture->reference_context != NULL);
    CHECK(boxrig_win32_renderer_create(
              fixture->reference_context,
              &fixture->renderer) == 0);
    fixture->cell_width =
        boxrig_win32_renderer_cell_width(fixture->renderer);
    fixture->cell_height =
        boxrig_win32_renderer_cell_height(fixture->renderer);
#elif defined(BOXRIG_TEST_FRONTEND_OPENGL)
    {
        boxrig_ogl_renderer_config config =
            boxrig_ogl_renderer_config_default();
        opengl_host_report report;

        const char *api = getenv("BOXRIG_TEST_GL_API");
        CHECK(api == NULL || strcmp(api, "gles") == 0);
        CHECK(opengl_host_create(api == NULL ? "egl" : "egl-gles", &fixture->host, &report));
        fixture->context_generation = 1;
        memset(&fixture->target, 0, sizeof(fixture->target));
        fixture->target.draw_framebuffer = opengl_host_create_framebuffer(
            fixture->host,
            BOXRIG_GL_TARGET_RGBA8,
            64,
            64,
            &fixture->target_texture);
        CHECK(fixture->target.draw_framebuffer != 0);
        fixture->target.width = 64;
        fixture->target.height = 64;
        fixture->target.generation = 1;
        fixture->target.format = BOXRIG_GL_TARGET_RGBA8;
        fixture->target.alpha_mode = BOXRIG_GL_ALPHA_PREMULTIPLIED;
        fixture->target.origin = BOXRIG_GL_TARGET_ORIGIN_LOWER_LEFT;
        config.runtime.resolver_context = fixture->host;
        config.runtime.resolve = opengl_host_resolve;
        config.runtime.current_context_query_context = fixture->host;
        config.runtime.query_current_context = opengl_host_query_current;
        config.runtime.context_generation = fixture->context_generation;
        config.runtime.target_generation = fixture->target.generation;
        config.initial_target = fixture->target;
        CHECK(boxrig_ogl_renderer_create(
                  &config,
                  &fixture->renderer,
                  NULL) == BOXRIG_GL_OK);
        CHECK(boxrig_ogl_renderer_set_composited_scrollbar(
                  fixture->renderer,
                  true,
                  2) == 0);
        CHECK(boxrig_ogl_renderer_set_composited_scrollbar_interaction(
                  fixture->renderer,
                  true,
                  false) == 0);
        fixture->cell_width =
            boxrig_ogl_renderer_cell_width(fixture->renderer);
        fixture->cell_height =
            boxrig_ogl_renderer_cell_height(fixture->renderer);
    }
#else
    CHECK(boxrig_pango_cairo_renderer_create(
              64,
              64,
              &fixture->renderer) == 0);
    fixture->cell_width =
        boxrig_pango_cairo_renderer_cell_width(fixture->renderer);
    fixture->cell_height =
        boxrig_pango_cairo_renderer_cell_height(fixture->renderer);
#endif
    CHECK(fixture->cell_width != 0 && fixture->cell_height != 0);
    return true;
}

static bool size_platform_renderer(
    platform_renderer_fixture *fixture,
    size_t rows,
    size_t columns)
{
    CHECK(columns <= UINT_MAX / fixture->cell_width);
    CHECK(rows <= UINT_MAX / fixture->cell_height);
#if defined(BOXRIG_TEST_FRONTEND_X11)
    XResizeWindow(
        fixture->display,
        fixture->window,
        (unsigned int)columns * fixture->cell_width,
        (unsigned int)rows * fixture->cell_height);
    CHECK(boxrig_x11_renderer_set_surface_size(
              fixture->renderer,
              (unsigned int)columns * fixture->cell_width,
              (unsigned int)rows * fixture->cell_height) == 0);
    XMapWindow(fixture->display, fixture->window);
    XSync(fixture->display, False);
#elif defined(BOXRIG_TEST_FRONTEND_D3D11)
    {
        ID3D11Texture2D *texture = NULL;
        D3D11_TEXTURE2D_DESC desc = {0};
        boxrig_d3d11_target target = fixture->target;
        target.width = (unsigned int)columns * fixture->cell_width;
        target.height = (unsigned int)rows * fixture->cell_height;
        target.generation++;
        desc.Width = target.width; desc.Height = target.height;
        desc.MipLevels = 1; desc.ArraySize = 1; desc.Format = target.format;
        desc.SampleDesc.Count = 1; desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        CHECK(SUCCEEDED(ID3D11Device_CreateTexture2D(fixture->device, &desc, NULL, &texture)));
        target.texture = texture;
        CHECK(SUCCEEDED(boxrig_d3d11_renderer_set_target(fixture->renderer, 1, &target)));
        ID3D11Texture2D_Release(fixture->texture);
        fixture->texture = texture;
        fixture->target = target;
    }
#elif defined(BOXRIG_TEST_FRONTEND_WIN32)
    CHECK(boxrig_win32_renderer_set_surface_size(
              fixture->renderer,
              (unsigned int)columns * fixture->cell_width,
              (unsigned int)rows * fixture->cell_height) == 0);
#elif defined(BOXRIG_TEST_FRONTEND_OPENGL)
    {
        boxrig_gl_target next = fixture->target;
        uint32_t next_texture = 0;

        next.width = (unsigned int)columns * fixture->cell_width;
        next.height = (unsigned int)rows * fixture->cell_height;
        if (next.width == fixture->target.width &&
            next.height == fixture->target.height) {
            return true;
        }
        next.draw_framebuffer = opengl_host_create_framebuffer(
            fixture->host,
            next.format,
            next.width,
            next.height,
            &next_texture);
        CHECK(next.draw_framebuffer != 0);
        next.generation++;
        CHECK(boxrig_ogl_renderer_set_target(
                  fixture->renderer,
                  fixture->context_generation,
                  &next,
                  NULL) == BOXRIG_GL_OK);
        opengl_host_delete_framebuffer(
            fixture->host,
            fixture->target.draw_framebuffer,
            fixture->target_texture);
        fixture->target = next;
        fixture->target_texture = next_texture;
    }
#else
    CHECK(boxrig_pango_cairo_renderer_set_surface_size(
              fixture->renderer,
              (unsigned int)columns * fixture->cell_width,
              (unsigned int)rows * fixture->cell_height) == 0);
#endif
    return true;
}

static bool draw_platform_view(
    platform_renderer_fixture *fixture,
    boxrig_ui_view *view,
    boxrig_composition *composition)
{
#if defined(BOXRIG_TEST_FRONTEND_X11)
    if (composition == NULL) {
        CHECK(boxrig_x11_renderer_draw_view(fixture->renderer, view) == 0);
    } else {
        CHECK(boxrig_x11_renderer_draw_view_composition(
                  fixture->renderer,
                  view,
                  composition) == 0);
    }
    XSync(fixture->display, False);
#elif defined(BOXRIG_TEST_FRONTEND_D3D11)
    {
        boxrig_renderer_frame_token token;
        CHECK(boxrig_d3d11_renderer_prepare_frame(fixture->renderer, view,
            composition, 1, fixture->target.generation, &token) == BOXRIG_RENDERER_OK);
        CHECK(boxrig_d3d11_renderer_accept_frame(fixture->renderer, &token) == BOXRIG_RENDERER_OK);
    }
#elif defined(BOXRIG_TEST_FRONTEND_WIN32)
    if (composition == NULL) {
        CHECK(boxrig_win32_renderer_update_view(fixture->renderer, view) == 0);
    } else {
        CHECK(boxrig_win32_renderer_update_view_composition(
                  fixture->renderer,
                  view,
                  composition) == 0);
    }
#elif defined(BOXRIG_TEST_FRONTEND_OPENGL)
    if (composition == NULL) {
        CHECK(boxrig_ogl_renderer_draw_view(
                  fixture->renderer,
                  view) == 0);
    } else {
        CHECK(boxrig_ogl_renderer_draw_view_composition(
                  fixture->renderer,
                  view,
                  composition) == 0);
    }
#else
    if (composition == NULL) {
        CHECK(boxrig_pango_cairo_renderer_draw_view(
                  fixture->renderer,
                  view) == 0);
    } else {
        CHECK(boxrig_pango_cairo_renderer_draw_view_composition(
                  fixture->renderer,
                  view,
                  composition) == 0);
    }
#endif
    return true;
}

static bool get_overlay_statistics(
    platform_renderer_fixture *fixture,
    size_t *annotation_out,
    size_t *search_out,
    size_t *selection_out,
    size_t *hyperlink_out,
    size_t *cursor_out,
    size_t *composition_out)
{
#if defined(BOXRIG_TEST_FRONTEND_X11)
    boxrig_x11_render_statistics statistics;

    CHECK(boxrig_x11_renderer_get_render_statistics(
              fixture->renderer,
              &statistics) == 0);
#elif defined(BOXRIG_TEST_FRONTEND_D3D11)
    boxrig_d3d11_statistics statistics;
    CHECK(boxrig_d3d11_renderer_get_statistics(fixture->renderer, &statistics) == 0);
#elif defined(BOXRIG_TEST_FRONTEND_WIN32)
    boxrig_win32_render_statistics statistics;

    CHECK(boxrig_win32_renderer_get_render_statistics(
              fixture->renderer,
              &statistics) == 0);
#elif defined(BOXRIG_TEST_FRONTEND_OPENGL)
    boxrig_ogl_render_statistics statistics;

    CHECK(boxrig_ogl_renderer_get_render_statistics(
              fixture->renderer,
              &statistics) == 0);
#else
    boxrig_pango_cairo_render_statistics statistics;

    CHECK(boxrig_pango_cairo_renderer_get_render_statistics(
              fixture->renderer,
              &statistics) == 0);
#endif
    *annotation_out = statistics.annotation_raster_count;
    *search_out = statistics.search_match_raster_count;
    *selection_out = statistics.selection_raster_count;
    *hyperlink_out = statistics.hyperlink_hover_raster_count;
    *cursor_out = statistics.cursor_raster_count;
    *composition_out = statistics.composition_raster_count;
    return true;
}

static bool get_platform_pixel(
    platform_renderer_fixture *fixture,
    int x,
    int y,
    platform_rgb *color_out)
{
#if defined(BOXRIG_TEST_FRONTEND_X11)
    XImage *image;
    XColor color;

    image = XGetImage(
        fixture->display,
        fixture->window,
        x,
        y,
        1,
        1,
        AllPlanes,
        ZPixmap);
    CHECK(image != NULL);
    color.pixel = XGetPixel(image, 0, 0);
    XDestroyImage(image);
    CHECK(XQueryColor(
        fixture->display,
        DefaultColormap(
            fixture->display,
            DefaultScreen(fixture->display)),
        &color));
    color_out->red = (uint8_t)(color.red >> 8);
    color_out->green = (uint8_t)(color.green >> 8);
    color_out->blue = (uint8_t)(color.blue >> 8);
#elif defined(BOXRIG_TEST_FRONTEND_D3D11)
    {
        ID3D11Texture2D *staging = NULL;
        D3D11_TEXTURE2D_DESC desc;
        D3D11_MAPPED_SUBRESOURCE mapped;
        const uint8_t *pixel;
        ID3D11Texture2D_GetDesc(fixture->texture, &desc);
        CHECK(x >= 0 && y >= 0 && (unsigned int)x < desc.Width && (unsigned int)y < desc.Height);
        desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        CHECK(SUCCEEDED(ID3D11Device_CreateTexture2D(fixture->device, &desc, NULL, &staging)));
        ID3D11DeviceContext_CopyResource(fixture->context,
            (ID3D11Resource *)staging, (ID3D11Resource *)fixture->texture);
        CHECK(SUCCEEDED(ID3D11DeviceContext_Map(fixture->context,
            (ID3D11Resource *)staging, 0, D3D11_MAP_READ, 0, &mapped)));
        pixel = (const uint8_t *)mapped.pData + (size_t)y * mapped.RowPitch + (size_t)x * 4;
        color_out->red = pixel[2]; color_out->green = pixel[1]; color_out->blue = pixel[0];
        ID3D11DeviceContext_Unmap(fixture->context, (ID3D11Resource *)staging, 0);
        ID3D11Texture2D_Release(staging);
    }
#elif defined(BOXRIG_TEST_FRONTEND_WIN32)
    HDC context;
    SIZE size;
    COLORREF color;

    CHECK(boxrig_win32_renderer_get_surface(
              fixture->renderer,
              &context,
              &size) == 0);
    CHECK(x >= 0 && y >= 0 && x < size.cx && y < size.cy);
    color = GetPixel(context, x, y);
    CHECK(color != CLR_INVALID);
    color_out->red = GetRValue(color);
    color_out->green = GetGValue(color);
    color_out->blue = GetBValue(color);
#elif defined(BOXRIG_TEST_FRONTEND_OPENGL)
    {
        uint8_t rgba[4];

        CHECK(opengl_host_read_pixel(
            fixture->host,
            fixture->target.draw_framebuffer,
            fixture->target.height,
            x,
            y,
            rgba));
        color_out->red = rgba[0];
        color_out->green = rgba[1];
        color_out->blue = rgba[2];
    }
#else
    cairo_surface_t *surface =
        boxrig_pango_cairo_renderer_surface(fixture->renderer);
    unsigned char *data;
    uint32_t pixel;
    int stride;

    CHECK(surface != NULL && x >= 0 && y >= 0 &&
          x < cairo_image_surface_get_width(surface) &&
          y < cairo_image_surface_get_height(surface));
    cairo_surface_flush(surface);
    data = cairo_image_surface_get_data(surface);
    stride = cairo_image_surface_get_stride(surface);
    CHECK(data != NULL && stride > 0);
    memcpy(
        &pixel,
        data + (size_t)y * (size_t)stride + (size_t)x * sizeof(pixel),
        sizeof(pixel));
    color_out->red = (uint8_t)(pixel >> 16);
    color_out->green = (uint8_t)(pixel >> 8);
    color_out->blue = (uint8_t)pixel;
#endif
    return true;
}

static bool get_image_statistics(
    platform_renderer_fixture *fixture,
    size_t *upload_out,
    size_t *composite_out,
    size_t *pixel_out)
{
#if defined(BOXRIG_TEST_FRONTEND_X11)
    boxrig_x11_render_statistics statistics;

    CHECK(boxrig_x11_renderer_get_render_statistics(
              fixture->renderer,
              &statistics) == 0);
#elif defined(BOXRIG_TEST_FRONTEND_D3D11)
    boxrig_d3d11_statistics statistics;
    CHECK(boxrig_d3d11_renderer_get_statistics(fixture->renderer, &statistics) == 0);
#elif defined(BOXRIG_TEST_FRONTEND_WIN32)
    boxrig_win32_render_statistics statistics;

    CHECK(boxrig_win32_renderer_get_render_statistics(
              fixture->renderer,
              &statistics) == 0);
#elif defined(BOXRIG_TEST_FRONTEND_OPENGL)
    boxrig_ogl_render_statistics statistics;

    CHECK(boxrig_ogl_renderer_get_render_statistics(
              fixture->renderer,
              &statistics) == 0);
#else
    boxrig_pango_cairo_render_statistics statistics;

    CHECK(boxrig_pango_cairo_renderer_get_render_statistics(
              fixture->renderer,
              &statistics) == 0);
#endif
    *upload_out = statistics.image_cache_upload_count;
    *composite_out = statistics.image_composite_count;
    *pixel_out = statistics.image_pixel_count;
    return true;
}

static void destroy_platform_renderer(platform_renderer_fixture *fixture)
{
#if defined(BOXRIG_TEST_FRONTEND_X11)
    boxrig_x11_renderer_destroy(fixture->renderer);
    if (fixture->window != None) {
        XDestroyWindow(fixture->display, fixture->window);
    }
    if (fixture->display != NULL) {
        XCloseDisplay(fixture->display);
    }
    cairo_debug_reset_static_data();
    FcFini();
#elif defined(BOXRIG_TEST_FRONTEND_D3D11)
    (void)boxrig_d3d11_renderer_destroy(&fixture->renderer);
    ID3D11Texture2D_Release(fixture->texture);
    ID3D11DeviceContext_Release(fixture->context);
    ID3D11Device_Release(fixture->device);
#elif defined(BOXRIG_TEST_FRONTEND_WIN32)
    boxrig_win32_renderer_destroy(fixture->renderer);
    if (fixture->reference_context != NULL) {
        DeleteDC(fixture->reference_context);
    }
#elif defined(BOXRIG_TEST_FRONTEND_OPENGL)
    (void)boxrig_ogl_renderer_destroy(
        &fixture->renderer,
        fixture->context_generation,
        NULL);
    opengl_host_delete_framebuffer(
        fixture->host,
        fixture->target.draw_framebuffer,
        fixture->target_texture);
    opengl_host_destroy(fixture->host);
    cairo_debug_reset_static_data();
    FcFini();
#else
    boxrig_pango_cairo_renderer_destroy(fixture->renderer);
    cairo_debug_reset_static_data();
    FcFini();
#endif
    memset(fixture, 0, sizeof(*fixture));
}

#if !defined(BOXRIG_TEST_FRONTEND_WIN32) && !defined(BOXRIG_TEST_FRONTEND_D3D11)
static bool color_glyph_policy_fixture(void)
{
    const char *families[] = {"Noto Color Emoji", "DejaVu Sans Mono"};
    const char content[] = "\033[?25l\033[38;2;255;255;255;48;2;0;0;0m"
        "\xf0\x9f\xa7\x91\033[2;1HA";
    boxrig_terminal_config config = boxrig_terminal_config_default();
    config.rows = 2;
    config.columns = 4;

    for (size_t family = 0; family < 2; family++) {
        platform_renderer_fixture renderer;
        boxrig_terminal *terminal = NULL;
        boxrig_ui_model *model = NULL;
        boxrig_ui_view *view = NULL;
        boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
        CHECK(create_platform_renderer(&renderer));
        CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
        CHECK(feed_bytes(terminal, content, sizeof(content) - 1));
        CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
        CHECK(boxrig_ui_view_create(model, &view_config, &view) == BOXRIG_UI_OK);

        /* Reuse the unchanged text while switching both directions, including
         * a glyph provided only by the system's bitmap color-font fallback. */
        for (size_t phase = 0; phase < 3; phase++) {
            boxrig_font_rendering_policy policy =
                boxrig_font_rendering_policy_default();
            policy.color_glyphs = phase != 1;
            policy.antialias = BOXRIG_FONT_ANTIALIAS_GRAYSCALE;
#if defined(BOXRIG_TEST_FRONTEND_OPENGL)
            boxrig_ogl_renderer_config font = boxrig_ogl_renderer_config_default();
            font.runtime.resolver_context = renderer.host;
            font.runtime.resolve = opengl_host_resolve;
            font.runtime.current_context_query_context = renderer.host;
            font.runtime.query_current_context = opengl_host_query_current;
            font.runtime.context_generation = renderer.context_generation;
            font.runtime.target_generation = renderer.target.generation;
            font.initial_target = renderer.target;
            font.font_family = families[family];
            font.font_family_length = strlen(families[family]);
            font.rendering_policy = policy;
            boxrig_ogl_renderer *candidate = NULL;
            CHECK(boxrig_ogl_renderer_create(&font, &candidate, NULL) == BOXRIG_GL_OK);
            CHECK(boxrig_ogl_renderer_destroy(&renderer.renderer,
                renderer.context_generation, NULL) == BOXRIG_GL_OK);
            renderer.renderer = candidate;
            renderer.cell_width = boxrig_ogl_renderer_cell_width(candidate);
            renderer.cell_height = boxrig_ogl_renderer_cell_height(candidate);
            if (phase == 1) {
                font.rendering_policy.color_glyphs = true;
                font.rendering_policy.dpi_y = 0;
                candidate = NULL;
                CHECK(boxrig_ogl_renderer_create(&font, &candidate, NULL) != BOXRIG_GL_OK);
                CHECK(candidate == NULL);
            }
#else
            boxrig_font_specification font = boxrig_font_specification_default();
            boxrig_font_family name = {families[family], strlen(families[family]), 0};
            font.families = &name;
            font.family_count = 1;
#if defined(BOXRIG_TEST_FRONTEND_X11)
            CHECK(boxrig_x11_renderer_set_font(renderer.renderer, &font, &policy) ==
                BOXRIG_FONT_CATALOG_OK);
            renderer.cell_width = boxrig_x11_renderer_cell_width(renderer.renderer);
            renderer.cell_height = boxrig_x11_renderer_cell_height(renderer.renderer);
            if (phase == 1) {
                policy.color_glyphs = true;
                policy.dpi_y = 0;
                CHECK(boxrig_x11_renderer_set_font(renderer.renderer, &font, &policy) ==
                    BOXRIG_FONT_CATALOG_INVALID_ARGUMENT);
                policy.color_glyphs = false;
            }
#else
            CHECK(boxrig_pango_cairo_renderer_set_font(renderer.renderer, &font,
                &policy) == BOXRIG_FONT_CATALOG_OK);
            renderer.cell_width = boxrig_pango_cairo_renderer_cell_width(renderer.renderer);
            renderer.cell_height = boxrig_pango_cairo_renderer_cell_height(renderer.renderer);
            if (phase == 1) {
                policy.color_glyphs = true;
                policy.dpi_y = 0;
                CHECK(boxrig_pango_cairo_renderer_set_font(renderer.renderer, &font,
                    &policy) == BOXRIG_FONT_CATALOG_INVALID_ARGUMENT);
                policy.color_glyphs = false;
            }
#endif
#endif
            CHECK(size_platform_renderer(&renderer, config.rows, config.columns));
            CHECK(draw_platform_view(&renderer, view, NULL));
            size_t ink = 0, colored = 0;
            for (unsigned int y = 0; y < renderer.cell_height; y++) {
                for (unsigned int x = 0; x < 2 * renderer.cell_width; x++) {
                    platform_rgb pixel;
                    CHECK(get_platform_pixel(&renderer, (int)x, (int)y, &pixel));
                    ink += pixel.red > 16 || pixel.green > 16 || pixel.blue > 16;
                    colored += pixel.red != pixel.green || pixel.green != pixel.blue;
                }
            }
            fprintf(stderr, "color glyph family=%s enabled=%d ink=%zu colored=%zu\n",
                families[family], policy.color_glyphs, ink, colored);
            CHECK(ink > 5);
            CHECK(policy.color_glyphs ? colored > 5 : colored == 0);
        }
        boxrig_ui_view_destroy(view);
        boxrig_ui_model_destroy(model);
        boxrig_terminal_destroy(terminal);
        destroy_platform_renderer(&renderer);
    }
    puts("PASS color glyph policy, bitmap fallback and cache replacement");
    return true;
}
#endif

static bool take_resize(
    boxrig_ui_view *view,
    size_t rows,
    size_t columns)
{
    boxrig_ui_grid_resize resize;

    CHECK(take_grid_resize_action(view, &resize));
    CHECK(resize.revision != 0);
    CHECK(resize.rows == rows && resize.columns == columns);
    CHECK(!take_grid_resize_action(view, &resize));
    return true;
}

static bool accept_allocation(
    boxrig_ui_view *view,
    boxrig_ui_allocation_input *input,
    boxrig_ui_window_state state,
    boxrig_ui_fixed width,
    boxrig_ui_fixed height,
    boxrig_ui_layout_result *result)
{
    input->window_state = state;
    input->allocation_width = width;
    input->allocation_height = height;
    CHECK(boxrig_ui_view_accept_allocation(view, input, result) ==
          BOXRIG_UI_OK);
    return true;
}

static bool common_layout_action_fixture(void)
{
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    boxrig_ui_allocation_input allocation;
    boxrig_ui_layout_change_input change;
    boxrig_ui_layout_result layout;
    boxrig_ui_layout_action action;
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *view = NULL;
    bool applied = false;

    terminal_config.rows = 24;
    terminal_config.columns = 80;
    CHECK(boxrig_terminal_create(&terminal_config, &terminal) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_create(model, &view_config, &view) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_acquire_layout_authority(view) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_set_geometry_capabilities(
              view,
              BOXRIG_UI_GEOMETRY_SIZE_COMMAND |
                  BOXRIG_UI_GEOMETRY_RESIZE_INCREMENTS) == BOXRIG_UI_OK);

    memset(&allocation, 0, sizeof(allocation));
    allocation.cell_width = pixels(10);
    allocation.cell_height = pixels(20);
    allocation.cell_metrics_revision = 1;
    CHECK(accept_allocation(
        view,
        &allocation,
        BOXRIG_UI_WINDOW_NORMAL,
        allocation.cell_width * terminal_config.columns,
        allocation.cell_height * terminal_config.rows,
        &layout));

    memset(&change, 0, sizeof(change));
    change.kind = BOXRIG_UI_LAYOUT_CHANGE_FONT_METRICS;
    change.cell_width = pixels(8);
    change.cell_height = pixels(16);
    change.cell_metrics_revision = 2;
    CHECK(boxrig_ui_view_apply_layout_change(view, &change, &layout) ==
          BOXRIG_UI_OK);
    CHECK(layout.rows == 24 && layout.columns == 80 && !layout.constrained);
    CHECK(boxrig_ui_view_peek_layout_action(view, &action));
    CHECK(action.kind == BOXRIG_UI_LAYOUT_ACTION_CONTENT_SIZE);
    {
        uint64_t peeked_revision = action.revision;

        CHECK(boxrig_ui_view_peek_layout_action(view, &action));
        CHECK(action.revision == peeked_revision);
    }
    CHECK(boxrig_ui_view_take_layout_action(view, &action));
    CHECK(action.kind == BOXRIG_UI_LAYOUT_ACTION_CONTENT_SIZE);
    CHECK(action.revision != 0);
    CHECK(action.value.content_size.cause ==
          BOXRIG_UI_LAYOUT_CHANGE_FONT_METRICS);
    CHECK(action.value.content_size.width == change.cell_width * 80);
    CHECK(action.value.content_size.height == change.cell_height * 24);
    CHECK(!boxrig_ui_view_take_layout_action(view, &action));
    CHECK(boxrig_ui_view_complete_size_command(
              view,
              action.revision,
              BOXRIG_UI_SIZE_ACCEPTED,
              change.cell_width * 80,
              change.cell_height * 24,
              &applied,
              &layout) == BOXRIG_UI_OK);
    CHECK(applied);

    allocation.cell_width = change.cell_width;
    allocation.cell_height = change.cell_height;
    allocation.cell_metrics_revision = change.cell_metrics_revision;
    CHECK(accept_allocation(
        view,
        &allocation,
        BOXRIG_UI_WINDOW_MAXIMIZED,
        pixels(960),
        pixels(640),
        &layout));
    CHECK(layout.rows == 40 && layout.columns == 120 && layout.constrained);
    CHECK(boxrig_ui_view_peek_layout_action(view, &action));
    CHECK(action.kind == BOXRIG_UI_LAYOUT_ACTION_GRID_RESIZE);
    {
        uint64_t peeked_revision = action.revision;

        CHECK(boxrig_ui_view_peek_layout_action(view, &action));
        CHECK(action.revision == peeked_revision);
    }
    CHECK(boxrig_ui_view_take_layout_action(view, &action));
    CHECK(action.kind == BOXRIG_UI_LAYOUT_ACTION_GRID_RESIZE);
    CHECK(action.value.grid_resize.rows == 40);
    CHECK(action.value.grid_resize.columns == 120);
    CHECK(boxrig_terminal_resize(terminal, 40, 120) == BOXRIG_STATUS_OK);

    change.cell_width = pixels(6);
    change.cell_height = pixels(12);
    change.cell_metrics_revision = 3;
    CHECK(boxrig_ui_view_apply_layout_change(view, &change, &layout) ==
          BOXRIG_UI_OK);
    CHECK(layout.rows == 53 && layout.columns == 160 && layout.constrained);
    change.cell_width = pixels(5);
    change.cell_height = pixels(10);
    change.cell_metrics_revision = 4;
    CHECK(boxrig_ui_view_apply_layout_change(view, &change, &layout) ==
          BOXRIG_UI_OK);
    CHECK(layout.rows == 64 && layout.columns == 192 && layout.constrained);
    CHECK(boxrig_ui_view_take_layout_action(view, &action));
    CHECK(action.kind == BOXRIG_UI_LAYOUT_ACTION_GRID_RESIZE);
    CHECK(action.value.grid_resize.rows == 64);
    CHECK(action.value.grid_resize.columns == 192);
    CHECK(!boxrig_ui_view_take_layout_action(view, &action));
    CHECK(boxrig_terminal_resize(terminal, 64, 192) == BOXRIG_STATUS_OK);

    allocation.cell_width = change.cell_width;
    allocation.cell_height = change.cell_height;
    allocation.cell_metrics_revision = change.cell_metrics_revision;
    CHECK(accept_allocation(
        view,
        &allocation,
        BOXRIG_UI_WINDOW_NORMAL,
        pixels(800),
        pixels(480),
        &layout));
    CHECK(layout.constrained && layout.rows == 48 && layout.columns == 160);
    CHECK(boxrig_ui_view_take_layout_action(view, &action));
    CHECK(action.kind == BOXRIG_UI_LAYOUT_ACTION_CONTENT_SIZE);
    CHECK(action.value.content_size.cause == BOXRIG_UI_LAYOUT_CHANGE_RESTORE);
    CHECK(action.value.content_size.width == pixels(5) * 80);
    CHECK(action.value.content_size.height == pixels(10) * 24);
    CHECK(boxrig_ui_view_complete_size_command(
              view,
              action.revision,
              BOXRIG_UI_SIZE_ACCEPTED,
              action.value.content_size.width,
              action.value.content_size.height,
              &applied,
              &layout) == BOXRIG_UI_OK);
    CHECK(applied && !layout.constrained);
    CHECK(layout.rows == 24 && layout.columns == 80);
    CHECK(boxrig_ui_view_take_layout_action(view, &action));
    CHECK(action.kind == BOXRIG_UI_LAYOUT_ACTION_GRID_RESIZE);
    CHECK(action.value.grid_resize.rows == 24);
    CHECK(action.value.grid_resize.columns == 80);
    CHECK(boxrig_terminal_resize(terminal, 24, 80) == BOXRIG_STATUS_OK);
    CHECK(!boxrig_ui_view_take_layout_action(view, &action));

    boxrig_ui_view_destroy(view);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool common_grid_negotiation_fixture(void)
{
    static const boxrig_ui_window_state constrained_states[] = {
        BOXRIG_UI_WINDOW_MAXIMIZED,
        BOXRIG_UI_WINDOW_FULLSCREEN,
        BOXRIG_UI_WINDOW_TILED,
        BOXRIG_UI_WINDOW_EMBEDDED,
    };
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    boxrig_ui_allocation_input input;
    boxrig_ui_layout_change_input change;
    boxrig_ui_layout_result result;
    boxrig_ui_size_command first_command;
    boxrig_ui_size_command current_command;
    boxrig_ui_size_command_state command_state;
    boxrig_ui_hit_test_result hit;
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *view = NULL;
    size_t index;
    bool applied = false;

    terminal_config.rows = 24;
    terminal_config.columns = 80;
    CHECK(boxrig_terminal_create(&terminal_config, &terminal) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_create(model, &view_config, &view) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_acquire_layout_authority(view) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_set_geometry_capabilities(
              view,
              BOXRIG_UI_GEOMETRY_SIZE_COMMAND |
                  BOXRIG_UI_GEOMETRY_RESIZE_INCREMENTS) == BOXRIG_UI_OK);
    memset(&input, 0, sizeof(input));
    input.cell_width = pixels(7) + BOXRIG_UI_FIXED_ONE / 2;
    input.cell_height = pixels(15) + BOXRIG_UI_FIXED_ONE / 4;
    input.cell_metrics_revision = 1;

    CHECK(accept_allocation(
        view,
        &input,
        BOXRIG_UI_WINDOW_NORMAL,
        input.cell_width * 80,
        input.cell_height * 24,
        &result));
    CHECK(result.rows == 24 && result.columns == 80);
    CHECK(!result.constrained);

    CHECK(accept_allocation(
        view,
        &input,
        BOXRIG_UI_WINDOW_NORMAL,
        input.cell_width * 83 + input.cell_width / 2,
        input.cell_height * 27 + input.cell_height - 1,
        &result));
    CHECK(result.rows == 27 && result.columns == 83);
    CHECK(result.excess_right == input.cell_width / 2);
    CHECK(result.excess_bottom == input.cell_height - 1);
    CHECK(result.constrained);
    CHECK(boxrig_ui_view_get_size_command_state(view, &command_state) ==
          BOXRIG_UI_OK);
    CHECK(command_state == BOXRIG_UI_SIZE_COMMAND_IDLE);
    CHECK(take_resize(view, 27, 83));

    CHECK(accept_allocation(
        view,
        &input,
        BOXRIG_UI_WINDOW_NORMAL,
        input.cell_width * 83 + input.cell_width / 2,
        input.cell_height * 27 + input.cell_height - 1,
        &result));
    CHECK(result.constrained && result.rows == 27 && result.columns == 83);
    CHECK(boxrig_ui_view_get_size_command_state(view, &command_state) ==
          BOXRIG_UI_OK);
    CHECK(command_state == BOXRIG_UI_SIZE_COMMAND_IDLE);
    CHECK(!take_grid_resize_action(
        view,
        &(boxrig_ui_grid_resize){0}));

    memset(&change, 0, sizeof(change));
    change.kind = BOXRIG_UI_LAYOUT_CHANGE_FONT_METRICS;
    change.cell_width = pixels(8) + BOXRIG_UI_FIXED_ONE / 4;
    change.cell_height = pixels(16) + BOXRIG_UI_FIXED_ONE / 2;
    change.cell_metrics_revision = 2;
    CHECK(boxrig_ui_view_apply_layout_change(view, &change, &result) ==
          BOXRIG_UI_OK);
    CHECK(result.rows == 27 && result.columns == 83);
    CHECK(take_content_size_action(view, &first_command));
    CHECK(first_command.width == change.cell_width * 83);
    CHECK(first_command.height == change.cell_height * 27);

    change.kind = BOXRIG_UI_LAYOUT_CHANGE_OUTPUT_METRICS;
    change.cell_width = pixels(8) + BOXRIG_UI_FIXED_ONE / 2;
    change.cell_height = pixels(17);
    change.cell_metrics_revision = 3;
    CHECK(boxrig_ui_view_apply_layout_change(view, &change, &result) ==
          BOXRIG_UI_OK);
    CHECK(take_content_size_action(view, &current_command));
    CHECK(current_command.revision != first_command.revision);
    CHECK(boxrig_ui_view_complete_size_command(
              view,
              first_command.revision,
              BOXRIG_UI_SIZE_ACCEPTED,
              0,
              0,
              &applied,
              &result) == BOXRIG_UI_OK);
    CHECK(!applied);
    CHECK(boxrig_ui_view_complete_size_command(
              view,
              current_command.revision,
              BOXRIG_UI_SIZE_ACCEPTED,
              current_command.width,
              current_command.height,
              &applied,
              &result) == BOXRIG_UI_OK);
    CHECK(applied && result.rows == 27 && result.columns == 83);

    for (index = 0;
         index < sizeof(constrained_states) / sizeof(constrained_states[0]);
         index++) {
        boxrig_ui_fixed width = change.cell_width * (100 + index) + index;
        boxrig_ui_fixed height = change.cell_height * (40 + index) + index;

        input.cell_width = change.cell_width;
        input.cell_height = change.cell_height;
        input.cell_metrics_revision = change.cell_metrics_revision;
        CHECK(accept_allocation(
            view,
            &input,
            constrained_states[index],
            width,
            height,
            &result));
        CHECK(result.constrained);
        CHECK(result.columns == 100 + index && result.rows == 40 + index);
        CHECK(result.excess_right == (boxrig_ui_fixed)index);
        CHECK(result.excess_bottom == (boxrig_ui_fixed)index);
        CHECK(boxrig_ui_view_get_size_command_state(view, &command_state) ==
              BOXRIG_UI_OK);
        CHECK(command_state == BOXRIG_UI_SIZE_COMMAND_IDLE);
        CHECK(take_resize(view, 40 + index, 100 + index));
    }

    change.kind = BOXRIG_UI_LAYOUT_CHANGE_LINE_HEIGHT;
    change.cell_width = pixels(9);
    change.cell_height = pixels(18);
    change.cell_metrics_revision = 4;
    CHECK(boxrig_ui_view_apply_layout_change(view, &change, &result) ==
          BOXRIG_UI_OK);
    input.cell_width = change.cell_width;
    input.cell_height = change.cell_height;
    input.cell_metrics_revision = change.cell_metrics_revision;
    CHECK(accept_allocation(
        view,
        &input,
        BOXRIG_UI_WINDOW_TILED,
        input.cell_width * 72 + 1,
        input.cell_height * 31 + 1,
        &result));
    CHECK(result.constrained && result.rows == 31 && result.columns == 72);
    CHECK(take_resize(view, 31, 72));

    CHECK(accept_allocation(
        view,
        &input,
        BOXRIG_UI_WINDOW_NORMAL,
        result.grid_width,
        result.grid_height,
        &result));
    change.kind = BOXRIG_UI_LAYOUT_CHANGE_RESTORE;
    CHECK(boxrig_ui_view_apply_layout_change(view, &change, &result) ==
          BOXRIG_UI_OK);
    CHECK(result.rows == 27 && result.columns == 83);
    CHECK(take_content_size_action(view, &current_command));
    CHECK(current_command.width == input.cell_width * 83);
    CHECK(current_command.height == input.cell_height * 27);
    CHECK(!take_grid_resize_action(
        view,
        &(boxrig_ui_grid_resize){0}));

    CHECK(boxrig_ui_view_complete_size_command(
              view,
              current_command.revision,
              BOXRIG_UI_SIZE_REJECTED,
              input.cell_width * 61 + input.cell_width / 3,
              input.cell_height * 22 + input.cell_height / 2,
              &applied,
              &result) == BOXRIG_UI_OK);
    CHECK(applied && result.constrained);
    CHECK(result.rows == 22 && result.columns == 61);
    CHECK(take_resize(view, 22, 61));

    CHECK(accept_allocation(
        view,
        &input,
        BOXRIG_UI_WINDOW_NORMAL,
        input.cell_width * 61 + input.cell_width / 3,
        input.cell_height * 22 + input.cell_height / 2,
        &result));
    CHECK(result.constrained && result.rows == 22 && result.columns == 61);
    CHECK(boxrig_ui_view_get_size_command_state(view, &command_state) ==
          BOXRIG_UI_OK);
    CHECK(command_state == BOXRIG_UI_SIZE_COMMAND_IDLE);
    CHECK(!take_grid_resize_action(
        view,
        &(boxrig_ui_grid_resize){0}));

    CHECK(boxrig_ui_view_hit_test(
              view,
              result.grid_width + 1,
              result.grid_height - 1,
              &hit) == BOXRIG_UI_OK);
    CHECK(!hit.inside_grid);

    CHECK(boxrig_ui_view_set_geometry_capabilities(
              view,
              BOXRIG_UI_GEOMETRY_RESIZE_INCREMENTS) == BOXRIG_UI_OK);
    CHECK(accept_allocation(
        view,
        &input,
        BOXRIG_UI_WINDOW_NORMAL,
        input.cell_width / 2,
        input.cell_height / 2,
        &result));
    change.kind = BOXRIG_UI_LAYOUT_CHANGE_CHROME;
    CHECK(boxrig_ui_view_apply_layout_change(view, &change, &result) ==
          BOXRIG_UI_OK);
    CHECK(result.constrained && result.rows == 1 && result.columns == 1);
    CHECK(boxrig_ui_view_get_size_command_state(view, &command_state) ==
          BOXRIG_UI_OK);
    CHECK(command_state == BOXRIG_UI_SIZE_COMMAND_IDLE);
    CHECK(take_resize(view, 1, 1));

    boxrig_ui_view_destroy(view);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool common_text_overlay_fixture(void)
{
    static const char content[] =
        "\033]8;id=fixture;https://example.test/\033\\"
        "link"
        "\033]8;;\033\\ "
        "\xd7\x90\xd7\x91"
        "\033[2;1Hm  m"
        "\033[3;1H\xf0\x9f\x98\x80"
        "\033[1;1H";
    static const uint8_t pattern[] = "link";
    static const char preedit[] = "x";
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    boxrig_ui_allocation_input layout_input;
    boxrig_ui_layout_result layout_result;
    boxrig_text_search_options search_options;
    boxrig_text_search_progress progress = BOXRIG_TEXT_SEARCH_IN_PROGRESS;
    boxrig_text_search_result search_result;
    boxrig_ui_selection_options selection_options;
    boxrig_text_layout_range selection_ranges[4];
    boxrig_text_layout_range search_ranges[4];
    boxrig_ui_text_annotation annotations[3];
    boxrig_ui_hit_test_result link_hit;
    boxrig_ui_hit_test_result annotation_hit;
    boxrig_ui_hit_test_result default_underline_hit;
    boxrig_ui_hit_test_result bidi_hit;
    boxrig_ui_hit_test_result composed_hit;
    boxrig_ui_hit_test_result rectangle_anchor_hit;
    boxrig_ui_hit_test_result rectangle_focus_hit;
    boxrig_ui_hyperlink_hover hover;
    boxrig_ui_dirty dirty;
    boxrig_composition_event composition_event;
    platform_rgb color;
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *view = NULL;
    boxrig_composition *composition = NULL;
    boxrig_text_snapshot *snapshot = NULL;
    const uint8_t *snapshot_bytes;
    platform_renderer_fixture renderer;
    size_t work_used;
    size_t selection_count;
    size_t search_count;
    size_t annotation_rasters;
    size_t search_rasters;
    size_t selection_rasters;
    size_t selection_rasters_before_rectangle;
    size_t hyperlink_rasters;
    size_t cursor_rasters;
    size_t composition_rasters;
    size_t snapshot_length;

    terminal_config.rows = 3;
    terminal_config.columns = 12;
    CHECK(create_platform_renderer(&renderer));
    CHECK(size_platform_renderer(
        &renderer,
        terminal_config.rows,
        terminal_config.columns));
    CHECK(boxrig_terminal_create(&terminal_config, &terminal) ==
          BOXRIG_STATUS_OK);
    CHECK(feed_bytes(terminal, content, sizeof(content) - 1));
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_create(model, &view_config, &view) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_acquire_layout_authority(view) == BOXRIG_UI_OK);
    memset(&layout_input, 0, sizeof(layout_input));
    layout_input.allocation_width =
        pixels((int64_t)renderer.cell_width * terminal_config.columns);
    layout_input.allocation_height =
        pixels((int64_t)renderer.cell_height * terminal_config.rows);
    layout_input.cell_width = pixels(renderer.cell_width);
    layout_input.cell_height = pixels(renderer.cell_height);
    layout_input.cell_metrics_revision = 1;
    layout_input.window_state = BOXRIG_UI_WINDOW_NORMAL;
    CHECK(boxrig_ui_view_accept_allocation(
              view,
              &layout_input,
              &layout_result) == BOXRIG_UI_OK);
    CHECK(layout_result.rows == terminal_config.rows &&
          layout_result.columns == terminal_config.columns);
    CHECK(draw_platform_view(&renderer, view, NULL));

    CHECK(boxrig_ui_view_hit_test(
              view,
              pixels(renderer.cell_width) + pixels(renderer.cell_width) / 2,
              pixels(renderer.cell_height) + pixels(renderer.cell_height) / 2,
              &annotation_hit) == BOXRIG_UI_OK);
    CHECK(annotation_hit.inside_grid && annotation_hit.text_position_valid);
    CHECK(boxrig_ui_view_hit_test(
              view,
              pixels(renderer.cell_width * 2u) +
                  pixels(renderer.cell_width) / 2,
              pixels(renderer.cell_height) + pixels(renderer.cell_height) / 2,
              &default_underline_hit) == BOXRIG_UI_OK);
    CHECK(default_underline_hit.inside_grid &&
          default_underline_hit.text_position_valid);
    memset(annotations, 0, sizeof(annotations));
    annotations[0].source_id = 41;
    annotations[0].item_id = 1;
    annotations[0].range.start = annotation_hit.leading_position;
    annotations[0].range.end = annotation_hit.trailing_position;
    annotations[0].priority = -1;
    annotations[0].style.field_mask =
        BOXRIG_UI_ANNOTATION_BACKGROUND |
        BOXRIG_UI_ANNOTATION_UNDERLINE_COLOR;
    annotations[0].style.background.kind = BOXRIG_COLOR_RGB;
    annotations[0].style.background.red = 0x12;
    annotations[0].style.background.green = 0x34;
    annotations[0].style.background.blue = 0x56;
    annotations[0].style.underline_color.kind = BOXRIG_COLOR_RGB;
    annotations[0].style.underline_color.red = 0x45;
    annotations[0].style.underline_color.green = 0x67;
    annotations[0].style.underline_color.blue = 0x89;
    annotations[1].source_id = 41;
    annotations[1].item_id = 2;
    annotations[1].range = annotations[0].range;
    annotations[1].priority = 1;
    annotations[1].style.field_mask =
        BOXRIG_UI_ANNOTATION_UNDERLINE_STYLE |
        BOXRIG_UI_ANNOTATION_FOREGROUND;
    annotations[1].style.underline_style = BOXRIG_UNDERLINE_SINGLE;
    annotations[1].style.foreground.kind = BOXRIG_COLOR_RGB;
    annotations[1].style.foreground.red = 0x20;
    annotations[1].style.foreground.green = 0xd0;
    annotations[1].style.foreground.blue = 0x40;
    annotations[2].source_id = 41;
    annotations[2].item_id = 3;
    annotations[2].range.start = default_underline_hit.leading_position;
    annotations[2].range.end = default_underline_hit.trailing_position;
    annotations[2].priority = 2;
    annotations[2].style.field_mask =
        BOXRIG_UI_ANNOTATION_UNDERLINE_STYLE |
        BOXRIG_UI_ANNOTATION_FOREGROUND;
    annotations[2].style.underline_style = BOXRIG_UNDERLINE_SINGLE;
    annotations[2].style.foreground.kind = BOXRIG_COLOR_RGB;
    annotations[2].style.foreground.red = 0x20;
    annotations[2].style.foreground.green = 0xd0;
    annotations[2].style.foreground.blue = 0x40;
    CHECK(boxrig_ui_view_replace_text_annotations(
              view,
              41,
              annotation_hit.leading_position.screen,
              annotation_hit.leading_position.line_id,
              annotation_hit.leading_position.line_revision,
              annotations,
              3) == BOXRIG_UI_OK);
    CHECK(draw_platform_view(&renderer, view, NULL));
    CHECK(get_platform_pixel(
        &renderer,
        (int)renderer.cell_width + (int)renderer.cell_width / 2,
        (int)renderer.cell_height + (int)renderer.cell_height / 2,
        &color));
    CHECK(color.red == 0x12 && color.green == 0x34 && color.blue == 0x56);
    CHECK(get_platform_pixel(
        &renderer,
        (int)renderer.cell_width + (int)renderer.cell_width / 2,
        (int)(renderer.cell_height * 2u) - 2,
        &color));
    CHECK(color.red == 0x45 && color.green == 0x67 && color.blue == 0x89);
    CHECK(get_platform_pixel(
        &renderer,
        (int)(renderer.cell_width * 2u) + (int)renderer.cell_width / 2,
        (int)(renderer.cell_height * 2u) - 2,
        &color));
    CHECK(color.red == 0x20 && color.green == 0xd0 && color.blue == 0x40);

    CHECK(boxrig_ui_view_hit_test(
              view,
              pixels(renderer.cell_width) / 4,
              pixels(renderer.cell_height) / 2,
              &link_hit) == BOXRIG_UI_OK);
    CHECK(link_hit.inside_grid && link_hit.text_position_valid);
    CHECK(link_hit.logical_column == 0);
    CHECK(link_hit.hyperlink_handle != 0);
    CHECK(link_hit.hyperlink_uri_length ==
          sizeof("https://example.test/") - 1);
    CHECK(memcmp(
              link_hit.hyperlink_uri,
              "https://example.test/",
              link_hit.hyperlink_uri_length) == 0);
    CHECK((link_hit.overlay_roles & BOXRIG_UI_OVERLAY_CURSOR) != 0);

    CHECK(boxrig_ui_view_begin_selection(
              view,
              BOXRIG_UI_SELECTION_WORD,
              &link_hit) == BOXRIG_UI_OK);
    memset(&search_options, 0, sizeof(search_options));
    search_options.screen = BOXRIG_TEXT_SCREEN_PRIMARY;
    search_options.pattern = pattern;
    search_options.pattern_length = sizeof(pattern) - 1;
    search_options.direction = BOXRIG_TEXT_SEARCH_FORWARD;
    CHECK(boxrig_ui_view_begin_search(view, &search_options) == BOXRIG_UI_OK);
    while (progress == BOXRIG_TEXT_SEARCH_IN_PROGRESS) {
        CHECK(boxrig_ui_view_resume_search(
                  view,
                  4,
                  &work_used,
                  &progress) == BOXRIG_UI_OK);
        CHECK(work_used <= 4);
    }
    CHECK(progress == BOXRIG_TEXT_SEARCH_MATCH);
    CHECK(boxrig_ui_view_get_search_result(view, &search_result));
    CHECK(search_result.screen == BOXRIG_TEXT_SCREEN_PRIMARY);
    memset(annotations, 0, sizeof(annotations));
    annotations[0].source_id = 42;
    annotations[0].item_id = 1;
    annotations[0].range = search_result.range;
    annotations[0].style.field_mask = BOXRIG_UI_ANNOTATION_BACKGROUND;
    annotations[0].style.background.kind = BOXRIG_COLOR_RGB;
    annotations[0].style.background.red = 0x80;
    annotations[0].style.background.green = 0x10;
    annotations[0].style.background.blue = 0x20;
    CHECK(boxrig_ui_view_replace_text_annotations(
              view,
              42,
              search_result.range.start.screen,
              search_result.range.start.line_id,
              search_result.range.start.line_revision,
              annotations,
              1) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_hit_test(
              view,
              pixels(renderer.cell_width) / 4,
              pixels(renderer.cell_height) / 2,
              &link_hit) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_update_hyperlink_hover(view, &link_hit) ==
          BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_get_hyperlink_hover(view, &hover));
    CHECK(hover.handle == link_hit.hyperlink_handle);

    selection_count = boxrig_ui_view_selection_visual_ranges(
        view,
        0,
        selection_ranges,
        sizeof(selection_ranges) / sizeof(selection_ranges[0]));
    search_count = boxrig_ui_view_search_visual_ranges(
        view,
        0,
        search_ranges,
        sizeof(search_ranges) / sizeof(search_ranges[0]));
    CHECK(selection_count == 1 && search_count == selection_count);
    CHECK(selection_ranges[0].column == 0 &&
          selection_ranges[0].column_count == 4);
    CHECK(memcmp(
              selection_ranges,
              search_ranges,
              selection_count * sizeof(selection_ranges[0])) == 0);
    CHECK(draw_platform_view(&renderer, view, NULL));
    CHECK(get_platform_pixel(
        &renderer,
        (int)renderer.cell_width + (int)renderer.cell_width / 2,
        0,
        &color));
    CHECK(color.red == 0x2e && color.green == 0x73 && color.blue == 0xf2);
    CHECK(get_platform_pixel(
        &renderer,
        (int)renderer.cell_width + (int)renderer.cell_width / 2,
        (int)renderer.cell_height - 1,
        &color));
    CHECK(color.red == 0x1a && color.green == 0xbf && color.blue == 0xe6);

    CHECK(boxrig_ui_view_hit_test(
              view,
              pixels(renderer.cell_width * 5u) +
                  pixels(renderer.cell_width) / 4,
              pixels(renderer.cell_height) / 2,
              &bidi_hit) == BOXRIG_UI_OK);
    CHECK(bidi_hit.text_position_valid && bidi_hit.logical_column == 6);
    CHECK(boxrig_ui_view_hit_test(
              view,
              pixels(renderer.cell_width) / 4,
              pixels(renderer.cell_height) / 2,
              &link_hit) == BOXRIG_UI_OK);
    CHECK((link_hit.overlay_roles & BOXRIG_UI_OVERLAY_SEARCH_MATCH) != 0);
    CHECK((link_hit.overlay_roles & BOXRIG_UI_OVERLAY_SELECTION) != 0);
    CHECK((link_hit.overlay_roles & BOXRIG_UI_OVERLAY_HYPERLINK_HOVER) != 0);
    CHECK((link_hit.overlay_roles & BOXRIG_UI_OVERLAY_CURSOR) != 0);

    CHECK(boxrig_composition_create(
              terminal,
              boxrig_composition_limits_default(),
              &composition) == BOXRIG_COMPOSITION_OK);
    memset(&composition_event, 0, sizeof(composition_event));
    composition_event.type = BOXRIG_COMPOSITION_PREEDIT_UPDATE;
    composition_event.text = preedit;
    composition_event.text_length = sizeof(preedit) - 1;
    composition_event.caret_offset = sizeof(preedit) - 1;
    composition_event.caret_visible = true;
    CHECK(boxrig_composition_apply_event(composition, &composition_event) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(draw_platform_view(&renderer, view, composition));
    CHECK(boxrig_ui_view_hit_test_with_composition(
              view,
              composition,
              pixels(renderer.cell_width) / 4,
              pixels(renderer.cell_height) / 2,
              &composed_hit) == BOXRIG_UI_OK);
    CHECK((composed_hit.overlay_roles & BOXRIG_UI_OVERLAY_SEARCH_MATCH) != 0);
    CHECK((composed_hit.overlay_roles & BOXRIG_UI_OVERLAY_SELECTION) != 0);
    CHECK((composed_hit.overlay_roles & BOXRIG_UI_OVERLAY_HYPERLINK_HOVER) !=
          0);
    CHECK((composed_hit.overlay_roles & BOXRIG_UI_OVERLAY_IME) != 0);
    CHECK((composed_hit.overlay_roles & BOXRIG_UI_OVERLAY_CURSOR) == 0);
    CHECK(get_overlay_statistics(
        &renderer,
        &annotation_rasters,
        &search_rasters,
        &selection_rasters,
        &hyperlink_rasters,
        &cursor_rasters,
        &composition_rasters));
    CHECK(annotation_rasters != 0 && search_rasters != 0 &&
          selection_rasters != 0 &&
          hyperlink_rasters != 0 && cursor_rasters != 0 &&
          composition_rasters != 0);
#if defined(BOXRIG_TEST_FRONTEND_OPENGL)
    {
        boxrig_ogl_render_statistics statistics;

        CHECK(boxrig_ogl_renderer_get_render_statistics(
                  renderer.renderer,
                  &statistics) == 0);
        CHECK(statistics.grayscale_glyph_upload_count != 0);
        CHECK(statistics.color_glyph_upload_count != 0);
    }
#endif

    CHECK(boxrig_ui_view_clear_search(view) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_get_row_dirty(
        view,
        BOXRIG_UI_DIRTY_OVERLAY,
        0,
        &dirty));
    CHECK(dirty.column == 0 && dirty.column_count >= 4);
    CHECK(boxrig_ui_view_clear_hyperlink_hover(view) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_update_selection(view, &bidi_hit) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_get_row_dirty(
        view,
        BOXRIG_UI_DIRTY_OVERLAY,
        0,
        &dirty));
    CHECK(dirty.column == 0 && dirty.column_count >= 7);

    CHECK(boxrig_ui_view_hit_test(
              view,
              pixels(renderer.cell_width) / 4,
              pixels(renderer.cell_height) / 2,
              &rectangle_anchor_hit) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_hit_test(
              view,
              pixels(renderer.cell_width) +
                  pixels(renderer.cell_width) * 3 / 4,
              pixels(renderer.cell_height) +
                  pixels(renderer.cell_height) / 2,
              &rectangle_focus_hit) == BOXRIG_UI_OK);
    selection_options.shape = BOXRIG_UI_SELECTION_RECTANGULAR;
    selection_options.kind = BOXRIG_UI_SELECTION_CHARACTER;
    CHECK(boxrig_ui_view_begin_selection_with_options(
              view,
              &selection_options,
              &rectangle_anchor_hit) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_update_selection(view, &rectangle_focus_hit) ==
          BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_selection_visual_ranges(
              view,
              0,
              selection_ranges,
              4) == 1);
    CHECK(selection_ranges[0].column == 0 &&
          selection_ranges[0].column_count == 2);
    CHECK(boxrig_ui_view_selection_visual_ranges(
              view,
              1,
              selection_ranges,
              4) == 1);
    CHECK(selection_ranges[0].column == 0 &&
          selection_ranges[0].column_count == 2);
    CHECK(boxrig_ui_view_create_selection_snapshot(view, &snapshot) ==
          BOXRIG_UI_OK);
    snapshot_bytes = boxrig_text_snapshot_data(snapshot, &snapshot_length);
    CHECK(snapshot_length == sizeof("li\nm ") - 1);
    CHECK(snapshot_bytes != NULL &&
          memcmp(snapshot_bytes, "li\nm ", snapshot_length) == 0);
    boxrig_text_snapshot_destroy(snapshot);
    snapshot = NULL;
    CHECK(boxrig_ui_view_hit_test(
              view,
              pixels(renderer.cell_width) +
                  pixels(renderer.cell_width) / 2,
              pixels(renderer.cell_height) +
                  pixels(renderer.cell_height) / 2,
              &annotation_hit) == BOXRIG_UI_OK);
    CHECK((annotation_hit.overlay_roles & BOXRIG_UI_OVERLAY_SELECTION) != 0);
    CHECK(get_overlay_statistics(
        &renderer,
        &annotation_rasters,
        &search_rasters,
        &selection_rasters_before_rectangle,
        &hyperlink_rasters,
        &cursor_rasters,
        &composition_rasters));
    CHECK(draw_platform_view(&renderer, view, NULL));
    CHECK(get_platform_pixel(
        &renderer,
        (int)renderer.cell_width + (int)renderer.cell_width / 2,
        (int)renderer.cell_height,
        &color));
    CHECK(color.red == 0x2e && color.green == 0x73 && color.blue == 0xf2);
    CHECK(get_overlay_statistics(
        &renderer,
        &annotation_rasters,
        &search_rasters,
        &selection_rasters,
        &hyperlink_rasters,
        &cursor_rasters,
        &composition_rasters));
    CHECK(selection_rasters == selection_rasters_before_rectangle + 2);

    boxrig_composition_destroy(composition);
    boxrig_ui_view_destroy(view);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    destroy_platform_renderer(&renderer);
    return true;
}

static bool common_image_alpha_fixture(void)
{
    static const char content[] =
        "\033[?25l"
        "\033]11;#204060\007"
        "\033_Ga=T,f=32,s=1,v=1,c=1,r=1,z=0,C=1;/wAA/w==\033\\";
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    boxrig_ui_allocation_input layout_input;
    boxrig_ui_layout_result layout_result;
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *view = NULL;
    platform_renderer_fixture renderer;
    size_t uploads;
    size_t composites;
    size_t pixels_composited;

    terminal_config.rows = 1;
    terminal_config.columns = 2;
    CHECK(create_platform_renderer(&renderer));
    CHECK(size_platform_renderer(
        &renderer,
        terminal_config.rows,
        terminal_config.columns));
    CHECK(boxrig_terminal_create(&terminal_config, &terminal) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_set_cell_pixel_geometry(
              terminal,
              renderer.cell_width,
              renderer.cell_height) == BOXRIG_STATUS_OK);
    CHECK(feed_bytes(terminal, content, sizeof(content) - 1));
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_create(model, &view_config, &view) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_acquire_layout_authority(view) == BOXRIG_UI_OK);
    memset(&layout_input, 0, sizeof(layout_input));
    layout_input.allocation_width = pixels(
        (int64_t)renderer.cell_width * terminal_config.columns);
    layout_input.allocation_height = pixels(renderer.cell_height);
    layout_input.cell_width = pixels(renderer.cell_width);
    layout_input.cell_height = pixels(renderer.cell_height);
    layout_input.cell_metrics_revision = 1;
    layout_input.window_state = BOXRIG_UI_WINDOW_NORMAL;
    CHECK(boxrig_ui_view_accept_allocation(
              view,
              &layout_input,
              &layout_result) == BOXRIG_UI_OK);
#if defined(BOXRIG_TEST_FRONTEND_PANGO_CAIRO) || \
    defined(BOXRIG_TEST_FRONTEND_OPENGL)
    {
        boxrig_ui_transparency_state transparency;

        memset(&transparency, 0, sizeof(transparency));
        transparency.effective =
            BOXRIG_UI_TRANSPARENCY_PREMULTIPLIED_ALPHA;
        transparency.effective_scopes =
            BOXRIG_UI_TRANSPARENCY_VIEW_SURFACE;
        transparency.effective_background_opacity = 128;
#if defined(BOXRIG_TEST_FRONTEND_OPENGL)
        CHECK(boxrig_ogl_renderer_apply_transparency_state(
                  renderer.renderer,
                  &transparency) == 0);
#else
        CHECK(boxrig_pango_cairo_renderer_apply_transparency_state(
                  renderer.renderer,
                  &transparency) == 0);
        CHECK(boxrig_pango_cairo_renderer_prepare_surface_candidate(
                  renderer.renderer,
                  renderer.cell_width * terminal_config.columns,
                  renderer.cell_height) == 0);
        CHECK(boxrig_pango_cairo_renderer_surface_candidate_active(
            renderer.renderer));
        CHECK(!boxrig_pango_cairo_renderer_surface_candidate_ready(
            renderer.renderer));
#endif
    }
#endif
    CHECK(draw_platform_view(&renderer, view, NULL));
    CHECK(get_image_statistics(
        &renderer,
        &uploads,
        &composites,
        &pixels_composited));
    CHECK(uploads == 1 && composites == 1 && pixels_composited != 0);
#if defined(BOXRIG_TEST_FRONTEND_PANGO_CAIRO)
    {
        cairo_surface_t *surface =
            boxrig_pango_cairo_renderer_surface(renderer.renderer);
        unsigned char *data;
        int stride;
        uint32_t image_pixel;
        uint32_t background_pixel;

        CHECK(surface != NULL);
        CHECK(boxrig_pango_cairo_renderer_surface_candidate_ready(
            renderer.renderer));
        cairo_surface_flush(surface);
        data = cairo_image_surface_get_data(surface);
        stride = cairo_image_surface_get_stride(surface);
        CHECK(data != NULL && stride > 0);
        memcpy(
            &image_pixel,
            data + (renderer.cell_height / 2) * (size_t)stride +
                (renderer.cell_width / 2) * sizeof(image_pixel),
            sizeof(image_pixel));
        memcpy(
            &background_pixel,
            data + (renderer.cell_height / 2) * (size_t)stride +
                (renderer.cell_width + renderer.cell_width / 2) *
                    sizeof(background_pixel),
            sizeof(background_pixel));
        CHECK((image_pixel >> 24) == 255);
        CHECK((background_pixel >> 24) == 128);
        CHECK(boxrig_pango_cairo_renderer_commit_surface_candidate(
                  renderer.renderer) == 0);
        CHECK(!boxrig_pango_cairo_renderer_surface_candidate_active(
            renderer.renderer));
    }
#endif

    boxrig_ui_view_destroy(view);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    destroy_platform_renderer(&renderer);
    return true;
}

#if defined(BOXRIG_TEST_FRONTEND_OPENGL)
static bool opengl_multiline_image_alpha_fixture(void)
{
    static const char content[] =
        "\033[?25l\033]11;#000000\007"
        "\033[2;1H\033_Ga=T,i=7001,f=32,s=1,v=1,c=2,r=2,z=0,C=1;MOBggA==\033\\"
        "\033[2;3H\033_Ga=p,i=7001,p=2,c=2,r=2,z=-1,C=1\033\\";
    platform_renderer_fixture renderer;
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *view = NULL;
    config.rows = 4;
    config.columns = 5;
    CHECK(create_platform_renderer(&renderer));
    CHECK(size_platform_renderer(&renderer, config.rows, config.columns));
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_set_cell_pixel_geometry(terminal,
        renderer.cell_width, renderer.cell_height) == BOXRIG_STATUS_OK);
    CHECK(feed_bytes(terminal, content, sizeof(content) - 1));
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_create(model, &view_config, &view) == BOXRIG_UI_OK);
    for (unsigned int frame = 0; frame < 3; frame++) {
        if (frame == 1) {
            boxrig_ui_dirty dirty = {1, 1, 0, 4};
            CHECK(boxrig_ui_view_invalidate(view, BOXRIG_UI_DIRTY_PRESENT, dirty) == BOXRIG_UI_OK);
        } else if (frame == 2) {
            CHECK(size_platform_renderer(&renderer, config.rows, config.columns));
        }
        CHECK(draw_platform_view(&renderer, view, NULL));
        for (unsigned int row = 0; row < 4; row++) {
            for (unsigned int column = 0; column < 4; column++) {
                platform_rgb actual;
                CHECK(get_platform_pixel(&renderer,
                    (int)(column * renderer.cell_width + renderer.cell_width / 2),
                    (int)(row * renderer.cell_height + renderer.cell_height / 2), &actual));
                platform_rgb expected = (row == 1 || row == 2)
                    ? (platform_rgb){24, 112, 48} : (platform_rgb){0, 0, 0};
                if (actual.red != expected.red || actual.green != expected.green ||
                    actual.blue != expected.blue) {
                    fprintf(stderr, "Image alpha frame=%u cell=%u,%u expected=%u,%u,%u actual=%u,%u,%u\n",
                        frame, column, row, expected.red, expected.green, expected.blue,
                        actual.red, actual.green, actual.blue);
                    CHECK(false);
                }
            }
        }
    }
    boxrig_ui_view_destroy(view);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    destroy_platform_renderer(&renderer);
    return true;
}
#endif

#if !defined(BOXRIG_TEST_FRONTEND_WIN32) && !defined(BOXRIG_TEST_FRONTEND_D3D11)
static bool common_renderer_lifecycle_fixture(void)
{
    static const char content[] = "\033[41m \033[0mX";
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    platform_renderer_fixture renderer;
    boxrig_renderer_target target;
    boxrig_renderer_frame_token token;
    boxrig_renderer_counters counters;
#if defined(BOXRIG_TEST_FRONTEND_OPENGL)
    boxrig_ogl_render_statistics statistics;
    boxrig_ui_dirty dirty;
    boxrig_gl_target next_target;
    uint32_t next_texture;
    uint8_t reference_rgba[4];
    uint8_t rgba[4];
#endif
    platform_rgb color;
    boxrig_renderer_lifecycle *lifecycle = NULL;
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *view = NULL;

    terminal_config.rows = 2;
    terminal_config.columns = 4;
    CHECK(create_platform_renderer(&renderer));
    CHECK(size_platform_renderer(
        &renderer,
        terminal_config.rows,
        terminal_config.columns));
    CHECK(boxrig_terminal_create(&terminal_config, &terminal) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_set_cell_pixel_geometry(
              terminal,
              renderer.cell_width,
              renderer.cell_height) == BOXRIG_STATUS_OK);
    CHECK(feed_bytes(terminal, content, sizeof(content) - 1));
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_create(model, &view_config, &view) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);

    memset(&target, 0, sizeof(target));
    target.generation = 1;
    target.width = renderer.cell_width * (unsigned int)terminal_config.columns;
    target.height = renderer.cell_height * (unsigned int)terminal_config.rows;
    target.scale_numerator = 1;
    target.scale_denominator = 1;
#if defined(BOXRIG_TEST_FRONTEND_OPENGL)
    target.premultiplied_alpha = true;
#endif
#if defined(BOXRIG_TEST_FRONTEND_X11)
    CHECK(boxrig_x11_renderer_lifecycle_create(
              renderer.renderer,
              &target,
              &lifecycle) == BOXRIG_RENDERER_OK);
#elif defined(BOXRIG_TEST_FRONTEND_OPENGL)
    CHECK(boxrig_ogl_renderer_lifecycle_create(
              renderer.renderer,
              &target,
              &lifecycle) == BOXRIG_RENDERER_OK);
#else
    CHECK(boxrig_pango_cairo_renderer_lifecycle_create(
              renderer.renderer,
              &target,
              &lifecycle) == BOXRIG_RENDERER_OK);
#endif

    CHECK(boxrig_renderer_lifecycle_prepare_frame(
              lifecycle,
              view,
              NULL,
              target.generation,
              &token) == BOXRIG_RENDERER_OK);
    CHECK(boxrig_ui_view_frame_requested(view));
    CHECK(boxrig_renderer_lifecycle_abort_frame(lifecycle, &token) ==
          BOXRIG_RENDERER_OK);
    CHECK(boxrig_ui_view_frame_requested(view));
    CHECK(boxrig_renderer_lifecycle_prepare_frame(
              lifecycle,
              view,
              NULL,
              target.generation,
              &token) == BOXRIG_RENDERER_OK);
    CHECK(boxrig_renderer_lifecycle_accept_frame(lifecycle, &token) ==
          BOXRIG_RENDERER_OK);
    CHECK(!boxrig_ui_view_frame_requested(view));
    CHECK(get_platform_pixel(
        &renderer,
        (int)renderer.cell_width / 2,
        (int)renderer.cell_height / 2,
        &color));
    CHECK(color.red > color.green && color.red > color.blue);
#if defined(BOXRIG_TEST_FRONTEND_OPENGL)
    CHECK(opengl_host_read_pixel(
        renderer.host,
        renderer.target.draw_framebuffer,
        renderer.target.height,
        (int)renderer.cell_width / 2,
        (int)renderer.cell_height / 2,
        reference_rgba));
#endif
    CHECK(boxrig_renderer_lifecycle_get_counters(lifecycle, &counters) ==
          BOXRIG_RENDERER_OK);
    CHECK(counters.frame_prepare_count == 2);
    CHECK(counters.frame_accept_count == 1);
    CHECK(counters.frame_abort_count == 1);
    CHECK(counters.draw_count != 0);
#if defined(BOXRIG_TEST_FRONTEND_X11)
    CHECK(counters.submission_count != 0);
#else
    CHECK(counters.submission_count == 0);
#endif

#if defined(BOXRIG_TEST_FRONTEND_OPENGL)
    CHECK(boxrig_ogl_renderer_get_render_statistics(
              renderer.renderer,
              &statistics) == 0);
    CHECK(statistics.full_frame_software_upload_count == 0);
    CHECK(statistics.readback_count == 0);
    CHECK(statistics.swap_count == 0);
    CHECK(statistics.finish_count == 0);
    CHECK(statistics.wait_count == 0);

    CHECK(boxrig_renderer_lifecycle_prepare_frame(
              lifecycle,
              view,
              NULL,
              target.generation + 1,
              &token) == BOXRIG_RENDERER_STALE);
    dirty.row = 0;
    dirty.row_count = terminal_config.rows;
    dirty.column = 0;
    dirty.column_count = terminal_config.columns;
    CHECK(boxrig_ui_view_invalidate(
              view,
              BOXRIG_UI_DIRTY_PRESENT,
              dirty) == BOXRIG_UI_OK);
    boxrig_ogl_renderer_fail_next(
        renderer.renderer,
        BOXRIG_OGL_FAILURE_DRAW);
    CHECK(boxrig_renderer_lifecycle_prepare_frame(
              lifecycle,
              view,
              NULL,
              target.generation,
              &token) == BOXRIG_RENDERER_UNAVAILABLE);
    CHECK(boxrig_ui_view_frame_requested(view));
    CHECK(boxrig_renderer_lifecycle_prepare_frame(
              lifecycle,
              view,
              NULL,
              target.generation,
              &token) == BOXRIG_RENDERER_OK);
    CHECK(boxrig_renderer_lifecycle_accept_frame(lifecycle, &token) ==
          BOXRIG_RENDERER_OK);
    CHECK(!boxrig_ui_view_frame_requested(view));

    next_target = renderer.target;
    next_texture = 0;
    next_target.draw_framebuffer = opengl_host_create_framebuffer(
        renderer.host,
        BOXRIG_GL_TARGET_SRGB8_ALPHA8,
        next_target.width,
        next_target.height,
        &next_texture);
    CHECK(next_target.draw_framebuffer != 0);
    next_target.generation++;
    next_target.format = BOXRIG_GL_TARGET_SRGB8_ALPHA8;
    CHECK(boxrig_ogl_renderer_set_target(
              renderer.renderer,
              renderer.context_generation,
              &next_target,
              NULL) == BOXRIG_GL_OK);
    opengl_host_delete_framebuffer(
        renderer.host,
        renderer.target.draw_framebuffer,
        renderer.target_texture);
    renderer.target = next_target;
    renderer.target_texture = next_texture;
    CHECK(boxrig_renderer_lifecycle_prepare_frame(
              lifecycle,
              view,
              NULL,
              target.generation,
              &token) == BOXRIG_RENDERER_OK);
    CHECK(boxrig_renderer_lifecycle_accept_frame(lifecycle, &token) ==
          BOXRIG_RENDERER_OK);
    CHECK(opengl_host_read_pixel(
        renderer.host,
        renderer.target.draw_framebuffer,
        renderer.target.height,
        (int)renderer.cell_width / 2,
        (int)renderer.cell_height / 2,
        rgba));
    CHECK(rgba[0] == reference_rgba[0] &&
          rgba[1] == reference_rgba[1] &&
          rgba[2] == reference_rgba[2] &&
          rgba[3] == reference_rgba[3]);

    CHECK(boxrig_ogl_renderer_target_lost(renderer.renderer, view) ==
          BOXRIG_RENDERER_OK);
    CHECK(boxrig_ui_view_frame_requested(view));
    CHECK(boxrig_renderer_lifecycle_prepare_frame(
              lifecycle,
              view,
              NULL,
              target.generation,
              &token) == BOXRIG_RENDERER_OK);
    CHECK(boxrig_renderer_lifecycle_accept_frame(lifecycle, &token) ==
          BOXRIG_RENDERER_OK);
    CHECK(!boxrig_ui_view_frame_requested(view));

    boxrig_renderer_lifecycle_destroy(lifecycle);
    lifecycle = NULL;

    next_target = renderer.target;
    next_texture = 0;
    next_target.draw_framebuffer = opengl_host_create_framebuffer(
        renderer.host,
        BOXRIG_GL_TARGET_RGBA8,
        next_target.width,
        next_target.height,
        &next_texture);
    CHECK(next_target.draw_framebuffer != 0);
    next_target.generation++;
    next_target.format = BOXRIG_GL_TARGET_RGBA8;
    next_target.alpha_mode = BOXRIG_GL_ALPHA_OPAQUE;
    CHECK(boxrig_ogl_renderer_set_target(
              renderer.renderer,
              renderer.context_generation,
              &next_target,
              NULL) == BOXRIG_GL_OK);
    opengl_host_delete_framebuffer(
        renderer.host,
        renderer.target.draw_framebuffer,
        renderer.target_texture);
    renderer.target = next_target;
    renderer.target_texture = next_texture;
    CHECK(boxrig_ogl_renderer_draw_view(renderer.renderer, view) == 0);
    CHECK(opengl_host_read_pixel(
        renderer.host,
        renderer.target.draw_framebuffer,
        renderer.target.height,
        (int)renderer.cell_width / 2,
        (int)renderer.cell_height / 2,
        rgba));
    CHECK(rgba[0] == reference_rgba[0] &&
          rgba[1] == reference_rgba[1] &&
          rgba[2] == reference_rgba[2] && rgba[3] == 255);

    CHECK(boxrig_ogl_renderer_destroy(
              &renderer.renderer,
              renderer.context_generation,
              NULL) == BOXRIG_GL_OK);
    CHECK(renderer.renderer == NULL);
    CHECK(opengl_host_read_pixel(
        renderer.host,
        renderer.target.draw_framebuffer,
        renderer.target.height,
        (int)renderer.cell_width / 2,
        (int)renderer.cell_height / 2,
        rgba));
    CHECK(opengl_host_read_pixel(
        renderer.host,
        renderer.target.draw_framebuffer,
        renderer.target.height,
        (int)renderer.target.width - 1,
        (int)renderer.target.height / 2,
        rgba));
    CHECK(rgba[0] == 0x48 && rgba[1] == 0x48 && rgba[2] == 0x48);
#endif

    boxrig_renderer_lifecycle_destroy(lifecycle);
    boxrig_ui_view_destroy(view);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    destroy_platform_renderer(&renderer);
    return true;
}
#endif

#if defined(BOXRIG_TEST_FRONTEND_PANGO_CAIRO)
static bool pango_cairo_retained_presentation_fixture(void)
{
    static const char content[] = "\033[41m \033[0mX";
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    platform_renderer_fixture renderer;
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *view = NULL;
    boxrig_renderer_lifecycle *lifecycle = NULL;
    boxrig_renderer_target target = {0};
    boxrig_ui_dirty dirty = {0};
    boxrig_pango_cairo_render_statistics before, after;
    platform_rgb *reference;
    platform_rgb color;
    size_t width, height, index, iteration;

    terminal_config.rows = 3;
    terminal_config.columns = 8;
    CHECK(create_platform_renderer(&renderer));
    CHECK(size_platform_renderer(
        &renderer, terminal_config.rows, terminal_config.columns));
    CHECK(boxrig_terminal_create(&terminal_config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(feed_bytes(terminal, content, sizeof(content) - 1));
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_create(model, &view_config, &view) == BOXRIG_UI_OK);
    CHECK(draw_platform_view(&renderer, view, NULL));
    CHECK(!boxrig_ui_view_frame_requested(view));
    width = renderer.cell_width * terminal_config.columns;
    height = renderer.cell_height * terminal_config.rows;
    reference = malloc(width * height * sizeof(*reference));
    CHECK(reference != NULL);
    for (index = 0; index < width * height; index++) {
        CHECK(get_platform_pixel(&renderer, (int)(index % width),
            (int)(index / width), &reference[index]));
    }
    dirty.row_count = terminal_config.rows;
    dirty.column_count = terminal_config.columns;
    for (iteration = 0; iteration < 3; iteration++) {
        CHECK(boxrig_pango_cairo_renderer_get_render_statistics(
            renderer.renderer, &before) == 0);
        CHECK(boxrig_ui_view_invalidate(view, BOXRIG_UI_DIRTY_PRESENT, dirty) ==
            BOXRIG_UI_OK);
        CHECK(boxrig_ui_view_frame_requested(view));
        CHECK(draw_platform_view(&renderer, view, NULL));
        CHECK(!boxrig_ui_view_frame_requested(view));
        CHECK(boxrig_pango_cairo_renderer_get_render_statistics(
            renderer.renderer, &after) == 0);
        fprintf(stderr, "PangoCairo retained present %zu: %zu rows, %zu cells, %zu cursors\n",
            iteration, after.row_raster_count - before.row_raster_count,
            after.cell_raster_count - before.cell_raster_count,
            after.cursor_raster_count - before.cursor_raster_count);
        CHECK(after.row_shape_count == before.row_shape_count);
        CHECK(after.row_raster_count == before.row_raster_count);
        CHECK(after.cell_raster_count == before.cell_raster_count);
        CHECK(after.cursor_raster_count == before.cursor_raster_count);
        for (index = 0; index < width * height; index++) {
            CHECK(get_platform_pixel(&renderer, (int)(index % width),
                (int)(index / width), &color));
            CHECK(color.red == reference[index].red &&
                color.green == reference[index].green &&
                color.blue == reference[index].blue);
        }
    }
    free(reference);

    /* Content dirty still changes accepted pixels after presentation reuse. */
    CHECK(feed_bytes(terminal, "\033[44m\033[H \033[0m", 13));
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(draw_platform_view(&renderer, view, NULL));
    CHECK(boxrig_pango_cairo_renderer_get_render_statistics(
        renderer.renderer, &after) == 0);
    CHECK(after.cell_raster_count > before.cell_raster_count);
    CHECK(get_platform_pixel(&renderer, (int)renderer.cell_width / 2,
        (int)renderer.cell_height / 2, &color));
    CHECK(color.blue > color.red && color.blue > color.green);
    before = after;

    /* Explicit renderer expose repairs the full retained raster. */
    CHECK(boxrig_pango_cairo_renderer_expose(
        renderer.renderer, 0, 0, (unsigned int)width, (unsigned int)height) == 0);
    CHECK(draw_platform_view(&renderer, view, NULL));
    CHECK(boxrig_pango_cairo_renderer_get_render_statistics(
        renderer.renderer, &after) == 0);
    CHECK(after.cell_raster_count - before.cell_raster_count ==
        terminal_config.rows * terminal_config.columns);
    before = after;

    /* A new resize candidate must receive a complete image before commit. */
    CHECK(boxrig_pango_cairo_renderer_prepare_surface_candidate(
        renderer.renderer, (unsigned int)width + renderer.cell_width,
        (unsigned int)height) == 0);
    CHECK(!boxrig_pango_cairo_renderer_surface_candidate_ready(renderer.renderer));
    CHECK(draw_platform_view(&renderer, view, NULL));
    CHECK(boxrig_pango_cairo_renderer_surface_candidate_ready(renderer.renderer));
    CHECK(boxrig_pango_cairo_renderer_get_render_statistics(
        renderer.renderer, &after) == 0);
    CHECK(after.cell_raster_count - before.cell_raster_count ==
        terminal_config.rows * terminal_config.columns);
    CHECK(boxrig_pango_cairo_renderer_commit_surface_candidate(renderer.renderer) == 0);
    before = after;

    /* Target loss invalidates accepted pixels even when content is unchanged. */
    target.generation = 1;
    target.width = (unsigned int)width + renderer.cell_width;
    target.height = (unsigned int)height;
    target.scale_numerator = 1;
    target.scale_denominator = 1;
    CHECK(boxrig_pango_cairo_renderer_lifecycle_create(
        renderer.renderer, &target, &lifecycle) == BOXRIG_RENDERER_OK);
    CHECK(boxrig_renderer_lifecycle_target_lost(lifecycle, view, 1) ==
        BOXRIG_RENDERER_OK);
    CHECK(draw_platform_view(&renderer, view, NULL));
    CHECK(boxrig_pango_cairo_renderer_get_render_statistics(
        renderer.renderer, &after) == 0);
    CHECK(after.cell_raster_count - before.cell_raster_count ==
        terminal_config.rows * terminal_config.columns);
    CHECK(get_platform_pixel(&renderer, (int)renderer.cell_width / 2,
        (int)renderer.cell_height / 2, &color));
    CHECK(color.blue > color.red && color.blue > color.green);
    CHECK(!boxrig_ui_view_frame_requested(view));
    puts("PASS PangoCairo retained present: zero raster, intact pixels, required redraws");
    boxrig_renderer_lifecycle_destroy(lifecycle);
    boxrig_ui_view_destroy(view);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    destroy_platform_renderer(&renderer);
    return true;
}
#endif

#if defined(BOXRIG_TEST_FRONTEND_OPENGL)
static bool opengl_composition_text_fixture(void)
{
    static const struct {
        const char *text;
        size_t selected_offset;
        size_t selected_length;
    } cases[] = {
        {"日本語", 0, 0}, {"日本語", 0, 3}, {"日本語", 3, 3},
        {"日本語", 6, 3}, {"変換", 0, 0}, {"AB", 0, 0}, {"e\314\201", 0, 0}
    };
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    platform_renderer_fixture renderer;
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *view = NULL;
    boxrig_composition *composition = NULL;
    platform_rgb *reference;
    size_t width;
    size_t index;

    config.rows = 3;
    config.columns = 12;
    CHECK(create_platform_renderer(&renderer));
    CHECK(size_platform_renderer(&renderer, config.rows, config.columns));
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_create(model, &view_config, &view) == BOXRIG_UI_OK);
    CHECK(boxrig_composition_create(terminal, boxrig_composition_limits_default(),
        &composition) == BOXRIG_COMPOSITION_OK);
    width = renderer.cell_width * 8;
    reference = calloc(width * renderer.cell_height, sizeof(*reference));
    CHECK(reference != NULL);
    for (index = 0; index < sizeof(cases) / sizeof(cases[0]); index++) {
        boxrig_composition_event event = {0};
        size_t x, y;
        size_t ink = 0;
        CHECK(feed_bytes(terminal, "\033[?25l\033[2;3H\033[K", 15));
        CHECK(feed_bytes(terminal, cases[index].text, cases[index].selected_offset));
        CHECK(feed_bytes(terminal, "\033[7m", 4));
        CHECK(feed_bytes(terminal, cases[index].text + cases[index].selected_offset,
            cases[index].selected_length));
        CHECK(feed_bytes(terminal, "\033[0m", 4));
        CHECK(feed_bytes(terminal,
            cases[index].text + cases[index].selected_offset + cases[index].selected_length,
            strlen(cases[index].text) - cases[index].selected_offset - cases[index].selected_length));
        CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
        CHECK(draw_platform_view(&renderer, view, NULL));
        for (y = 0; y < renderer.cell_height; y++) {
            for (x = 0; x < width; x++) {
                CHECK(get_platform_pixel(&renderer,
                    (int)(2 * renderer.cell_width + x),
                    (int)(renderer.cell_height + y), &reference[y * width + x]));
            }
        }
        CHECK(feed_bytes(terminal, "\033[2;3H\033[K", 9));
        CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
        event.type = BOXRIG_COMPOSITION_PREEDIT_UPDATE;
        event.text = cases[index].text;
        event.text_length = strlen(cases[index].text);
        event.selection_offset = cases[index].selected_offset;
        event.selection_length = cases[index].selected_length;
        event.caret_offset = event.text_length;
        CHECK(boxrig_composition_apply_event(composition, &event) == BOXRIG_COMPOSITION_OK);
        CHECK(draw_platform_view(&renderer, view, composition));
        for (y = 0; y < renderer.cell_height; y++) {
            for (x = 0; x < width; x++) {
                platform_rgb actual;
                const platform_rgb expected = reference[y * width + x];
                CHECK(get_platform_pixel(&renderer,
                    (int)(2 * renderer.cell_width + x),
                    (int)(renderer.cell_height + y), &actual));
                CHECK(actual.red == expected.red && actual.green == expected.green &&
                    actual.blue == expected.blue);
                if (actual.red > 100 && actual.green > 100 && actual.blue > 100) ink++;
            }
        }
        CHECK(ink > renderer.cell_width);
    }
    free(reference);
    boxrig_composition_destroy(composition);
    boxrig_ui_view_destroy(view);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    destroy_platform_renderer(&renderer);
    return true;
}

typedef struct partial_frame_operation {
    platform_renderer_fixture *renderer;
    boxrig_ui_view *view;
    bool fail;
} partial_frame_operation;

static bool draw_partial_frame_operation(void *context)
{
    partial_frame_operation *operation = context;
    if (operation->fail)
        boxrig_ogl_renderer_fail_next(operation->renderer->renderer, BOXRIG_OGL_FAILURE_DRAW);
    int result = boxrig_ogl_renderer_draw_view(operation->renderer->renderer, operation->view);
    return operation->fail ? result == -1 : result == 0;
}

static bool opengl_partial_frame_work_fixture(void)
{
    platform_renderer_fixture renderer;
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *view = NULL;
    config.rows = 5;
    config.columns = 8;
    CHECK(create_platform_renderer(&renderer));
    CHECK(size_platform_renderer(&renderer, config.rows, config.columns));
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_create(model, &view_config, &view) == BOXRIG_UI_OK);
    CHECK(feed_bytes(terminal, "\033[?25l", 6));
    CHECK(draw_platform_view(&renderer, view, NULL));
    static const char update[] = "\033[1;1H\033[41m  \033[3;1H\033[42m  \033[5;1H\033[44m  ";
    CHECK(feed_bytes(terminal, update, sizeof(update) - 1));
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    partial_frame_operation operation = {&renderer, view, false};
    opengl_host_work before = opengl_host_get_work(renderer.host);
    CHECK(opengl_host_test_operation_state(renderer.host,
        draw_partial_frame_operation, &operation));
    opengl_host_work after = opengl_host_get_work(renderer.host);
    fprintf(stderr, "OpenGL partial frame target checks: %zu\n",
        after.target_checks - before.target_checks);
    CHECK(after.target_checks - before.target_checks == 1);
    fprintf(stderr, "OpenGL partial frame attributes/uniform queries: %zu/%zu\n",
        after.attribute_pointers - before.attribute_pointers,
        after.uniform_lookups - before.uniform_lookups);
    CHECK(after.attribute_pointers == before.attribute_pointers);
    CHECK(after.uniform_lookups == before.uniform_lookups);
    /* Separated dirty rows must not erase the untouched rows between them. */
    for (size_t row = 0; row < config.rows; row++) {
        platform_rgb color;
        CHECK(get_platform_pixel(&renderer, renderer.cell_width / 2,
            row * renderer.cell_height + renderer.cell_height / 2, &color));
        if (row == 0) CHECK(color.red > color.green && color.red > color.blue);
        else if (row == 2) CHECK(color.green > color.red && color.green > color.blue);
        else if (row == 4) CHECK(color.blue > color.red && color.blue > color.green);
        else CHECK(color.red == color.green && color.green == color.blue);
    }
    boxrig_ui_dirty dirty = {0};
    dirty.row_count = config.rows;
    dirty.column_count = config.columns;
    CHECK(boxrig_ui_view_invalidate(view, BOXRIG_UI_DIRTY_PRESENT, dirty) == BOXRIG_UI_OK);
    operation.fail = true;
    CHECK(opengl_host_test_operation_state(renderer.host,
        draw_partial_frame_operation, &operation));
    CHECK(boxrig_ui_view_frame_requested(view));
    operation.fail = false;
    CHECK(opengl_host_test_operation_state(renderer.host,
        draw_partial_frame_operation, &operation));
    CHECK(!boxrig_ui_view_frame_requested(view));
    boxrig_ui_view_destroy(view);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    destroy_platform_renderer(&renderer);
    return true;
}

static bool opengl_glyph_position_reuse_fixture(void)
{
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    platform_renderer_fixture renderer;
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *view = NULL;
    boxrig_ogl_render_statistics before;
    boxrig_ogl_render_statistics after;
    char content[192];
    size_t index;

    CHECK(create_platform_renderer(&renderer));
    terminal_config.rows = 2;
    terminal_config.columns = sizeof(content) / terminal_config.rows;
    CHECK(size_platform_renderer(
        &renderer, terminal_config.rows, terminal_config.columns));
    CHECK(boxrig_terminal_create(&terminal_config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_create(model, &view_config, &view) == BOXRIG_UI_OK);
    for (index = 0; index < sizeof(content); index++) {
        content[index] = index % 2 == 0 ? 'X' : ' ';
    }
    CHECK(feed_bytes(terminal, content, sizeof(content)));
    CHECK(draw_platform_view(&renderer, view, NULL));
    CHECK(boxrig_ogl_renderer_get_render_statistics(renderer.renderer, &before) == 0);
    /* Position does not change the glyph image. All X cells reuse one upload,
     * including cells whose natural Pango advance has a fractional phase. */
    CHECK(before.grayscale_glyph_upload_count == 1);
    CHECK(before.color_glyph_upload_count == 0);
    for (index = 0; index < sizeof(content); index++) {
        content[index] = index % 2 == 0 ? ' ' : 'X';
    }
    CHECK(feed_bytes(terminal, "\033[H\033[31m", 8));
    CHECK(feed_bytes(terminal, content, sizeof(content)));
    CHECK(draw_platform_view(&renderer, view, NULL));
    CHECK(boxrig_ogl_renderer_get_render_statistics(renderer.renderer, &after) == 0);
    CHECK(after.grayscale_glyph_upload_count == before.grayscale_glyph_upload_count);
    CHECK(after.upload_bytes == before.upload_bytes);
    CHECK(after.glyph_cache_hit_count > before.glyph_cache_hit_count);

    boxrig_ui_view_destroy(view);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    destroy_platform_renderer(&renderer);
    return true;
}

static bool opengl_glyph_cache_pressure_fixture(void)
{
    size_t pass;

    /* The first limit exercises entry eviction; the second can retain the
     * entire alphabet, so its evictions must come from atlas page recycling. */
    for (pass = 0; pass < 2; pass++) {
        boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
        boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
        boxrig_ogl_renderer_config config = boxrig_ogl_renderer_config_default();
        platform_renderer_fixture renderer;
        boxrig_terminal *terminal = NULL;
        boxrig_ui_model *model = NULL;
        boxrig_ui_view *view = NULL;
        boxrig_ogl_render_statistics before;
        boxrig_ogl_render_statistics after;
        uint8_t *reference;
        uint8_t rgba[4];
        size_t pixel_count;
        size_t pixel;
        size_t iteration;

        CHECK(create_platform_renderer(&renderer));
        CHECK(boxrig_ogl_renderer_destroy(
            &renderer.renderer, renderer.context_generation, NULL) == BOXRIG_GL_OK);
        config.runtime.resolver_context = renderer.host;
        config.runtime.resolve = opengl_host_resolve;
        config.runtime.current_context_query_context = renderer.host;
        config.runtime.query_current_context = opengl_host_query_current;
        config.runtime.context_generation = renderer.context_generation;
        config.runtime.target_generation = renderer.target.generation;
        config.initial_target = renderer.target;
        config.limits.maximum_glyph_entries = pass == 0 ? 4 : 128;
        config.limits.maximum_glyph_pages = 1;
        config.limits.glyph_page_extent = 64;
        CHECK(boxrig_ogl_renderer_create(
            &config, &renderer.renderer, NULL) == BOXRIG_GL_OK);
        terminal_config.rows = 1;
        terminal_config.columns = 4;
        CHECK(size_platform_renderer(&renderer, 1, 4));
        CHECK(boxrig_terminal_create(&terminal_config, &terminal) == BOXRIG_STATUS_OK);
        CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
        CHECK(boxrig_ui_view_create(model, &view_config, &view) == BOXRIG_UI_OK);
        CHECK(feed_bytes(terminal, "\033[?25lABCD", 10));
        CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
        CHECK(draw_platform_view(&renderer, view, NULL));
        pixel_count = (size_t)renderer.target.width * renderer.target.height;
        reference = malloc(pixel_count * 4);
        CHECK(reference != NULL);
        for (pixel = 0; pixel < pixel_count; pixel++) {
            CHECK(opengl_host_read_pixel(
                renderer.host, renderer.target.draw_framebuffer,
                renderer.target.height, (int)(pixel % renderer.target.width),
                (int)(pixel / renderer.target.width), reference + pixel * 4));
        }
        for (iteration = 0; iteration < 95; iteration++) {
            /* Striding through the glyph set also exercises colliding keys in
             * the small index, rather than only adjacent font glyph numbers. */
            int character = ' ' + (int)((1 + iteration * 8) % 95);
            char content[] = "\033[H!!!!";

            memset(content + 3, character, 4);
            CHECK(feed_bytes(terminal, content, sizeof(content) - 1));
            CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
            if (iteration == 0) {
                CHECK(boxrig_ogl_renderer_get_render_statistics(
                    renderer.renderer, &before) == 0);
                boxrig_ogl_renderer_fail_next(renderer.renderer, BOXRIG_OGL_FAILURE_UPLOAD);
                CHECK(boxrig_ogl_renderer_draw_view(renderer.renderer, view) == -1);
                CHECK(boxrig_ui_view_frame_requested(view));
                CHECK(boxrig_ogl_renderer_get_render_statistics(
                    renderer.renderer, &after) == 0);
                CHECK(after.upload_bytes == before.upload_bytes);
                CHECK(opengl_host_read_pixel(
                    renderer.host, renderer.target.draw_framebuffer,
                    renderer.target.height, 0, 0, rgba));
            }
            CHECK(draw_platform_view(&renderer, view, NULL));
            /* Readback completes submitted work deterministically before the
             * next pressure frame; no timer or fence polling loop is needed. */
            CHECK(opengl_host_read_pixel(
                renderer.host, renderer.target.draw_framebuffer,
                renderer.target.height, 0, 0, rgba));
        }
        CHECK(boxrig_ogl_renderer_get_render_statistics(renderer.renderer, &after) == 0);
        CHECK(after.glyph_eviction_count != 0);
        if (pass == 0) {
            CHECK(after.glyph_lookup_comparison_count > after.glyph_cache_hit_count);
        }
        CHECK(after.grayscale_glyph_upload_count > config.limits.maximum_glyph_entries || pass == 1);
        CHECK(feed_bytes(terminal, "\033[HABCD", 7));
        CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
        CHECK(draw_platform_view(&renderer, view, NULL));
        for (pixel = 0; pixel < pixel_count; pixel++) {
            CHECK(opengl_host_read_pixel(
                renderer.host, renderer.target.draw_framebuffer,
                renderer.target.height, (int)(pixel % renderer.target.width),
                (int)(pixel / renderer.target.width), rgba));
            CHECK(memcmp(reference + pixel * 4, rgba, 4) == 0);
        }
        free(reference);
        if (pass == 0) {
            CHECK(boxrig_terminal_resize(terminal, 1, 5) == BOXRIG_STATUS_OK);
            CHECK(size_platform_renderer(&renderer, 1, 5));
            CHECK(feed_bytes(terminal, "\033[HABCDE", 8));
            CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
            CHECK(boxrig_ogl_renderer_get_render_statistics(renderer.renderer, &before) == 0);
            /* Five distinct glyphs cannot evict any of this frame's four
             * protected entries. A smaller retry must still render correctly. */
            CHECK(boxrig_ogl_renderer_draw_view(renderer.renderer, view) == -1);
            CHECK(boxrig_ogl_renderer_get_render_statistics(renderer.renderer, &after) == 0);
            CHECK(after.glyph_eviction_count == before.glyph_eviction_count);
            CHECK(opengl_host_read_pixel(
                renderer.host, renderer.target.draw_framebuffer,
                renderer.target.height, 0, 0, rgba));
            CHECK(boxrig_terminal_resize(terminal, 1, 4) == BOXRIG_STATUS_OK);
            CHECK(size_platform_renderer(&renderer, 1, 4));
            CHECK(feed_bytes(terminal, "\033[HABCD", 7));
            CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
            CHECK(draw_platform_view(&renderer, view, NULL));
        }
        boxrig_ui_view_destroy(view);
        boxrig_ui_model_destroy(model);
        boxrig_terminal_destroy(terminal);
        destroy_platform_renderer(&renderer);
    }
    return true;
}

static bool opengl_4k_terminal_grid_fixture(void)
{
    enum {
        OUTPUT_WIDTH = 3840,
        OUTPUT_HEIGHT = 2160
    };
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    boxrig_ui_allocation_input allocation;
    boxrig_ui_layout_result layout;
    platform_renderer_fixture renderer;
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *view = NULL;
    char *content;
    size_t cell_count;
    size_t index;
    boxrig_ogl_render_statistics before;
    boxrig_ogl_render_statistics after;

    CHECK(create_platform_renderer(&renderer));
    terminal_config.rows = OUTPUT_HEIGHT / renderer.cell_height;
    terminal_config.columns = OUTPUT_WIDTH / renderer.cell_width;
    CHECK(terminal_config.rows != 0 && terminal_config.columns != 0);
    CHECK(terminal_config.columns <= SIZE_MAX / terminal_config.rows);
    cell_count = terminal_config.rows * terminal_config.columns;
    CHECK(cell_count > 65536);
    content = malloc(cell_count);
    CHECK(content != NULL);
    for (index = 0; index < cell_count; index++) {
        content[index] = (char)(' ' + index % 95);
    }
    content[0] = '!';
    content[(terminal_config.rows - 1) * terminal_config.columns] = '!';
    CHECK(size_platform_renderer(
        &renderer,
        terminal_config.rows,
        terminal_config.columns));
    CHECK(boxrig_terminal_create(&terminal_config, &terminal) ==
          BOXRIG_STATUS_OK);
    CHECK(feed_bytes(terminal, content, cell_count));
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_create(model, &view_config, &view) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_acquire_layout_authority(view) == BOXRIG_UI_OK);
    memset(&allocation, 0, sizeof(allocation));
    allocation.allocation_width =
        pixels((int64_t)renderer.cell_width * terminal_config.columns);
    allocation.allocation_height =
        pixels((int64_t)renderer.cell_height * terminal_config.rows);
    allocation.cell_width = pixels(renderer.cell_width);
    allocation.cell_height = pixels(renderer.cell_height);
    allocation.cell_metrics_revision = 1;
    allocation.window_state = BOXRIG_UI_WINDOW_MAXIMIZED;
    CHECK(boxrig_ui_view_accept_allocation(view, &allocation, &layout) ==
          BOXRIG_UI_OK);
    CHECK(layout.rows == terminal_config.rows &&
          layout.columns == terminal_config.columns);
    CHECK(draw_platform_view(&renderer, view, NULL));
    CHECK(boxrig_ogl_renderer_get_render_statistics(renderer.renderer, &before) == 0);
    CHECK(before.row_signature_cell_count == cell_count);
    fprintf(stderr, "OpenGL 4K color hash work: %zu bytes for %zu cells\n",
        before.row_color_hash_byte_count, cell_count);
    CHECK(before.row_color_hash_byte_count >= cell_count * 3);
    CHECK(before.row_color_hash_byte_count <= cell_count * 48);
    CHECK(before.glyph_lookup_comparison_count <= cell_count * 4);
    fprintf(stderr, "OpenGL 4K cold work: %zu font keys, %zu cluster comparisons, %zu cells\n",
        before.font_key_count, before.cluster_lookup_comparison_count, cell_count);
    CHECK(before.font_key_count <= terminal_config.rows * 4);
    CHECK(before.cluster_lookup_comparison_count <= cell_count * 12);
    after = before;
    CHECK(draw_platform_view(&renderer, view, NULL));
    CHECK(boxrig_ogl_renderer_get_render_statistics(renderer.renderer, &after) == 0);
    CHECK(after.row_signature_cell_count == before.row_signature_cell_count);
    CHECK(after.row_color_hash_byte_count == before.row_color_hash_byte_count);
    CHECK(after.row_shape_count == before.row_shape_count);
    CHECK(after.upload_bytes == before.upload_bytes);
    CHECK(after.draw_batch_count == before.draw_batch_count);
    before = after;
    CHECK(feed_bytes(terminal, "\033[31m\033[H", 8));
    CHECK(feed_bytes(terminal, content, cell_count));
    CHECK(feed_bytes(terminal, "\033[H", 3));
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(draw_platform_view(&renderer, view, NULL));
    CHECK(boxrig_ogl_renderer_get_render_statistics(renderer.renderer, &after) == 0);
    CHECK(after.row_signature_cell_count - before.row_signature_cell_count ==
          cell_count);
    CHECK(after.row_color_hash_byte_count - before.row_color_hash_byte_count <=
          cell_count * 48);
    CHECK(after.row_shape_count == before.row_shape_count);
    CHECK(after.font_key_count == before.font_key_count);
    for (index = 0; index < 2; index++) {
        size_t row = index == 0 ? 0 : terminal_config.rows - 1;
        size_t pixel;
        bool has_red_ink = false;

        for (pixel = 0;
             pixel < (size_t)renderer.cell_width * renderer.cell_height;
             pixel++) {
            platform_rgb color;

            CHECK(get_platform_pixel(
                &renderer,
                (int)(pixel % renderer.cell_width),
                (int)(row * renderer.cell_height +
                      pixel / renderer.cell_width),
                &color));
            if (color.red > color.green && color.red > color.blue) {
                has_red_ink = true;
            }
        }
        CHECK(has_red_ink);
    }
    before = after;
    CHECK(feed_bytes(terminal, "\033[34m\033[H!", 9));
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(draw_platform_view(&renderer, view, NULL));
    CHECK(boxrig_ogl_renderer_get_render_statistics(renderer.renderer, &before) == 0);
    CHECK(feed_bytes(terminal, "\033[32m\033[H!", 9));
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(draw_platform_view(&renderer, view, NULL));
    CHECK(boxrig_ogl_renderer_get_render_statistics(renderer.renderer, &after) == 0);
    CHECK(after.row_shape_count == before.row_shape_count);
    fprintf(stderr,
        "OpenGL narrow paint work: %zu cells, %zu glyph comparisons, %zu instances\n",
        after.cell_raster_count - before.cell_raster_count,
        after.glyph_lookup_comparison_count - before.glyph_lookup_comparison_count,
        after.instance_count - before.instance_count);
    CHECK(after.cell_raster_count - before.cell_raster_count <= 8);
    CHECK(after.glyph_lookup_comparison_count - before.glyph_lookup_comparison_count <=
          16);
    CHECK(after.instance_count - before.instance_count <= 16);
    before = after;
    /* A second full-screen update reuses the same glyph set at new positions
     * and with different colors. Work stays proportional to displayed glyphs. */
    for (index = 0; index < cell_count; index++) {
        content[index] = (char)(' ' + (index + 1) % 95);
    }
    content[(terminal_config.rows - 1) * terminal_config.columns] = '!';
    CHECK(feed_bytes(terminal, "\033[H\033[31m", 8));
    CHECK(feed_bytes(terminal, content, cell_count));
    CHECK(feed_bytes(terminal, "\033[H", 3));
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(draw_platform_view(&renderer, view, NULL));
    CHECK(boxrig_ogl_renderer_get_render_statistics(renderer.renderer, &after) == 0);
    CHECK(after.row_signature_cell_count - before.row_signature_cell_count ==
          cell_count);
    CHECK(after.upload_bytes == before.upload_bytes);
    CHECK(after.glyph_lookup_comparison_count - before.glyph_lookup_comparison_count <=
          cell_count * 4);
    for (index = 0; index < 2; index++) {
        size_t row = index == 0 ? 0 : terminal_config.rows - 1;
        size_t pixel;
        bool has_red_ink = false;

        /* Both ends of the 4K viewport must show the newly colored glyph. */
        for (pixel = 0; pixel < (size_t)renderer.cell_width * renderer.cell_height; pixel++) {
            platform_rgb color;

            CHECK(get_platform_pixel(&renderer,
                (int)(pixel % renderer.cell_width),
                (int)(row * renderer.cell_height + pixel / renderer.cell_width), &color));
            if (color.red > color.green && color.red > color.blue) {
                has_red_ink = true;
            }
        }
        CHECK(has_red_ink);
    }
    printf("PASS OpenGL 4K cache: %zu comparisons for %zu updated cells\n",
        after.glyph_lookup_comparison_count - before.glyph_lookup_comparison_count,
        cell_count);
    before = after;
    CHECK(feed_bytes(terminal, "\033[32m@", 6));
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(draw_platform_view(&renderer, view, NULL));
    CHECK(boxrig_ogl_renderer_get_render_statistics(renderer.renderer, &after) == 0);
    CHECK(after.row_signature_cell_count - before.row_signature_cell_count ==
          terminal_config.columns);
    CHECK(after.row_shape_count == before.row_shape_count + 1);
    before = after;
    {
        boxrig_ui_dirty dirty = {0};

        dirty.row_count = terminal_config.rows;
        dirty.column_count = terminal_config.columns;
        CHECK(boxrig_ui_view_invalidate(view, BOXRIG_UI_DIRTY_PRESENT, dirty) == BOXRIG_UI_OK);
    }
    CHECK(draw_platform_view(&renderer, view, NULL));
    CHECK(boxrig_ogl_renderer_get_render_statistics(renderer.renderer, &after) == 0);
    CHECK(after.row_signature_cell_count == before.row_signature_cell_count);
    CHECK(after.row_shape_count == before.row_shape_count);
    CHECK(after.font_key_count == before.font_key_count);
    CHECK(after.cluster_lookup_comparison_count - before.cluster_lookup_comparison_count <=
          cell_count * 12);
    CHECK(after.glyph_cache_hit_count > before.glyph_cache_hit_count);
    printf("PASS OpenGL 4K shape reuse: %zu font keys, %zu cluster comparisons\n",
        after.font_key_count - before.font_key_count,
        after.cluster_lookup_comparison_count - before.cluster_lookup_comparison_count);
    {
        const struct {
            const char *sequence;
            unsigned char red;
            unsigned char green;
            unsigned char blue;
        } colors[] = {
            {"\033[48;2;128;0;0m\033[H", 128, 0, 0},
            {"\033[48;2;0;128;0m\033[H", 0, 128, 0},
            {"\033[48;2;0;0;128m\033[H", 0, 0, 128},
            {"\033[48;2;255;255;255m\033[H", 255, 255, 255},
            {"\033[48;2;1;2;3m\033[H", 1, 2, 3}
        };

        /* A uniform color change must update paint without changing glyph
         * segmentation. Sample a space so only the background contributes. */
        CHECK(terminal_config.columns > 95);
        /* Earlier cache scenarios replace the text, so create the blank
         * probe explicitly instead of assuming the original pattern remains. */
        content[95] = ' ';
        CHECK(feed_bytes(terminal, "\033[0m\033[H", 7));
        CHECK(feed_bytes(terminal, content, cell_count));
        CHECK(feed_bytes(terminal, "\033[H", 3));
        CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
        CHECK(draw_platform_view(&renderer, view, NULL));
        for (index = 0; index < sizeof(colors) / sizeof(colors[0]); index++) {
            platform_rgb color;
            boxrig_cell_view probe;

            CHECK(boxrig_ogl_renderer_get_render_statistics(renderer.renderer, &before) == 0);
            CHECK(feed_bytes(terminal, colors[index].sequence, strlen(colors[index].sequence)));
            CHECK(feed_bytes(terminal, content, cell_count));
            CHECK(feed_bytes(terminal, "\033[H", 3));
            CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
            CHECK(draw_platform_view(&renderer, view, NULL));
            CHECK(boxrig_ogl_renderer_get_render_statistics(renderer.renderer, &after) == 0);
            CHECK(after.row_shape_count == before.row_shape_count);
            CHECK(after.row_color_hash_byte_count - before.row_color_hash_byte_count <=
                  cell_count * 48);
            CHECK(boxrig_terminal_get_cell(terminal, 0, 95, &probe) == BOXRIG_STATUS_OK);
            CHECK(probe.text_length == 1 && probe.text[0] == ' ');
            CHECK(probe.background.kind == BOXRIG_COLOR_RGB &&
                  probe.background.red == colors[index].red &&
                  probe.background.green == colors[index].green &&
                  probe.background.blue == colors[index].blue);
            CHECK(get_platform_pixel(&renderer,
                (int)(95 * renderer.cell_width + renderer.cell_width / 2),
                (int)(renderer.cell_height / 2), &color));
            fprintf(stderr,
                "OpenGL 4K color %zu: expected %u,%u,%u, actual %u,%u,%u\n",
                index, (unsigned int)colors[index].red,
                (unsigned int)colors[index].green, (unsigned int)colors[index].blue,
                (unsigned int)color.red, (unsigned int)color.green,
                (unsigned int)color.blue);
            CHECK(color.red == colors[index].red &&
                  color.green == colors[index].green &&
                  color.blue == colors[index].blue);
        }
    }
    free(content);

    boxrig_ui_view_destroy(view);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    destroy_platform_renderer(&renderer);
    return true;
}
#endif

#if defined(BOXRIG_TEST_FRONTEND_D3D11)
int boxrig_test_d3d11_fixture(D3D_DRIVER_TYPE driver)
{
    d3d11_fixture_driver = driver;
    if (!common_layout_action_fixture() || !common_grid_negotiation_fixture() ||
        !common_text_overlay_fixture() || !common_image_alpha_fixture()) return 1;
    puts("PASS shared cell-grid fixture through D3D11 public API");
    return 0;
}
#else
int main(void)
{
#if !defined(BOXRIG_TEST_FRONTEND_WIN32) && !defined(BOXRIG_TEST_FRONTEND_D3D11)
    if (!color_glyph_policy_fixture()) return 1;
#endif
    if (!common_layout_action_fixture() || !common_grid_negotiation_fixture() ||
        !common_text_overlay_fixture() || !common_image_alpha_fixture()) return 1;
#if !defined(BOXRIG_TEST_FRONTEND_WIN32) && !defined(BOXRIG_TEST_FRONTEND_D3D11)
    if (!common_renderer_lifecycle_fixture()) return 1;
#endif
#if defined(BOXRIG_TEST_FRONTEND_PANGO_CAIRO)
    if (!pango_cairo_retained_presentation_fixture()) return 1;
#endif
#if defined(BOXRIG_TEST_FRONTEND_OPENGL)
    if (!opengl_partial_frame_work_fixture() ||
        !opengl_multiline_image_alpha_fixture() ||
        !opengl_composition_text_fixture() ||
        !opengl_glyph_position_reuse_fixture() ||
        !opengl_glyph_cache_pressure_fixture() ||
        !opengl_4k_terminal_grid_fixture()) return 1;
#endif
#if defined(BOXRIG_TEST_FRONTEND_X11)
    printf("PASS shared cell-grid fixture through X11 public API\n");
#elif defined(BOXRIG_TEST_FRONTEND_D3D11)
    printf("PASS shared cell-grid fixture through D3D11 public API\n");
#elif defined(BOXRIG_TEST_FRONTEND_WIN32)
    printf("PASS shared cell-grid fixture through Win32 public API\n");
#elif defined(BOXRIG_TEST_FRONTEND_OPENGL)
    printf("PASS shared cell-grid fixture through OpenGL renderer-only API\n");
#else
    printf("PASS shared cell-grid fixture through private PangoCairo API\n");
#endif
    return 0;
}

#endif
