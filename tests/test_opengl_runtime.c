/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "opengl_host.h"

#include <stdbool.h>
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

static boxrig_gl_runtime_config runtime_config(opengl_host *host)
{
    boxrig_gl_runtime_config config;

    memset(&config, 0, sizeof(config));
    config.resolver_context = host;
    config.resolve = opengl_host_resolve;
    config.current_context_query_context = host;
    config.query_current_context = opengl_host_query_current;
    config.context_generation = 7;
    config.target_generation = 11;
    return config;
}

static bool rejects_unsupported_contexts(opengl_host *host)
{
    boxrig_gl_runtime_config config = runtime_config(host);
    boxrig_gl_runtime *runtime = NULL;
    boxrig_gl_diagnostic diagnostic;

    opengl_host_override_version(host, "1.1");
    CHECK(boxrig_gl_runtime_create(&config, &runtime, &diagnostic) ==
          BOXRIG_GL_UNAVAILABLE);
    CHECK(runtime == NULL);
    CHECK(strstr(diagnostic.message, "3.3") != NULL);
    opengl_host_override_version(host, "3.2");
    CHECK(boxrig_gl_runtime_create(&config, &runtime, &diagnostic) ==
          BOXRIG_GL_UNAVAILABLE);
    CHECK(runtime == NULL);
    opengl_host_override_version(host, "OpenGL ES 2.0");
    CHECK(boxrig_gl_runtime_create(&config, &runtime, &diagnostic) ==
          BOXRIG_GL_UNAVAILABLE);
    CHECK(runtime == NULL);
    opengl_host_override_version(host, NULL);

    opengl_host_block_function(host, "glDrawArraysInstanced");
    CHECK(boxrig_gl_runtime_create(&config, &runtime, &diagnostic) ==
          BOXRIG_GL_UNAVAILABLE);
    CHECK(runtime == NULL);
    CHECK(strstr(diagnostic.message, "glDrawArraysInstanced") != NULL);
    opengl_host_block_function(host, NULL);

    opengl_host_fail_shader_compile(host, true);
    CHECK(boxrig_gl_runtime_create(&config, &runtime, &diagnostic) ==
          BOXRIG_GL_SHADER_FAILURE);
    CHECK(runtime == NULL);
    CHECK(strstr(diagnostic.message, "injected shader") != NULL);
    opengl_host_fail_shader_compile(host, false);
    return true;
}

static bool validates_context_and_target(opengl_host *host, bool is_gles)
{
    boxrig_gl_runtime_config config = runtime_config(host);
    boxrig_gl_runtime *runtime = NULL;
    boxrig_gl_capabilities capabilities;
    boxrig_gl_diagnostic diagnostic;
    boxrig_gl_target target;
    boxrig_gl_clip clip = {0, 0, 64, 48};
    uint32_t texture;
    uint32_t framebuffer;
    uint32_t second_framebuffer;
    uint32_t second_texture;
    uint32_t srgb_framebuffer;
    uint32_t srgb_texture;
    uint32_t empty_framebuffer;

    CHECK(boxrig_gl_runtime_create(&config, &runtime, &diagnostic) ==
          BOXRIG_GL_OK);
    CHECK(boxrig_gl_runtime_get_capabilities(runtime, &capabilities) ==
          BOXRIG_GL_OK);
    CHECK(capabilities.major_version > 3 ||
          (capabilities.major_version == 3 && (is_gles || capabilities.minor_version >= 3)));
    CHECK(capabilities.core_profile == !is_gles);
    CHECK(capabilities.is_gles == is_gles);
    CHECK(capabilities.srgb_target);
    CHECK(capabilities.vendor[0] != '\0');
    CHECK(capabilities.renderer[0] != '\0');
    CHECK(capabilities.version[0] != '\0');

    memset(&target, 0, sizeof(target));
    framebuffer = opengl_host_create_framebuffer(
        host,
        BOXRIG_GL_TARGET_RGBA8,
        64,
        48,
        &texture);
    CHECK(framebuffer != 0);
    target.draw_framebuffer = framebuffer;
    target.width = 64;
    target.height = 48;
    target.generation = 11;
    target.format = BOXRIG_GL_TARGET_RGBA8;
    target.alpha_mode = BOXRIG_GL_ALPHA_PREMULTIPLIED;
    target.origin = BOXRIG_GL_TARGET_ORIGIN_LOWER_LEFT;
    target.samples = 0;

    opengl_host_override_identity(host, true);
    CHECK(boxrig_gl_runtime_begin_target(
              runtime,
              7,
              &target,
              &clip,
              &diagnostic) == BOXRIG_GL_CONTEXT_MISMATCH);
    opengl_host_override_identity(host, false);
    opengl_host_override_thread(host, true);
    CHECK(boxrig_gl_runtime_begin_target(
              runtime,
              7,
              &target,
              &clip,
              &diagnostic) == BOXRIG_GL_CONTEXT_MISMATCH);
    opengl_host_override_thread(host, false);
    CHECK(boxrig_gl_runtime_begin_target(
              runtime,
              6,
              &target,
              &clip,
              &diagnostic) == BOXRIG_GL_STALE_GENERATION);

    target.generation = 10;
    CHECK(boxrig_gl_runtime_begin_target(
              runtime,
              7,
              &target,
              &clip,
              &diagnostic) == BOXRIG_GL_STALE_GENERATION);
    target.generation = 11;
    target.origin = (boxrig_gl_target_origin)99;
    CHECK(boxrig_gl_runtime_begin_target(
              runtime,
              7,
              &target,
              &clip,
              &diagnostic) == BOXRIG_GL_TARGET_MISMATCH);
    target.origin = BOXRIG_GL_TARGET_ORIGIN_LOWER_LEFT;
    target.alpha_mode = (boxrig_gl_alpha_mode)99;
    CHECK(boxrig_gl_runtime_begin_target(
              runtime,
              7,
              &target,
              &clip,
              &diagnostic) == BOXRIG_GL_TARGET_MISMATCH);
    target.alpha_mode = BOXRIG_GL_ALPHA_PREMULTIPLIED;
    target.format = BOXRIG_GL_TARGET_SRGB8_ALPHA8;
    CHECK(boxrig_gl_runtime_begin_target(
              runtime,
              7,
              &target,
              &clip,
              &diagnostic) == BOXRIG_GL_TARGET_MISMATCH);
    target.format = BOXRIG_GL_TARGET_RGBA8;
    target.samples = 1;
    CHECK(boxrig_gl_runtime_begin_target(
              runtime,
              7,
              &target,
              &clip,
              &diagnostic) == BOXRIG_GL_TARGET_MISMATCH);
    target.samples = capabilities.maximum_samples + 1;
    CHECK(boxrig_gl_runtime_begin_target(
              runtime,
              7,
              &target,
              &clip,
              &diagnostic) == BOXRIG_GL_TARGET_MISMATCH);
    target.samples = 0;

    empty_framebuffer = opengl_host_create_empty_framebuffer(host);
    target.draw_framebuffer = empty_framebuffer;
    CHECK(boxrig_gl_runtime_begin_target(
              runtime,
              7,
              &target,
              &clip,
              &diagnostic) == BOXRIG_GL_TARGET_INCOMPLETE);
    opengl_host_delete_framebuffer(host, empty_framebuffer, 0);
    target.draw_framebuffer = 0;

    second_framebuffer = opengl_host_create_framebuffer(
        host,
        BOXRIG_GL_TARGET_RGBA8,
        64,
        48,
        &second_texture);
    CHECK(second_framebuffer != 0);
    target.draw_framebuffer = second_framebuffer;
    opengl_host_fail_next_gl_check(host, true);
    CHECK(boxrig_gl_runtime_begin_target(
              runtime,
              7,
              &target,
              &clip,
              &diagnostic) == BOXRIG_GL_API_FAILURE);
    CHECK(diagnostic.gl_code != 0);
    opengl_host_fail_next_gl_check(host, false);

    CHECK(boxrig_gl_runtime_begin_target(
              runtime,
              7,
              &target,
              &clip,
              &diagnostic) == BOXRIG_GL_OK);
    CHECK(boxrig_gl_runtime_begin_target(
              runtime,
              7,
              &target,
              &clip,
              &diagnostic) == BOXRIG_GL_BUSY);
    CHECK(boxrig_gl_runtime_end_target(runtime, 7, &diagnostic) == BOXRIG_GL_OK);

    CHECK(boxrig_gl_runtime_set_target_generation(runtime, 7, 12, &diagnostic) ==
          BOXRIG_GL_OK);
    CHECK(boxrig_gl_runtime_begin_target(
              runtime,
              7,
              &target,
              &clip,
              &diagnostic) == BOXRIG_GL_STALE_GENERATION);
    target.generation = 12;
    CHECK(opengl_host_test_state_contract(host, runtime, &target));
    opengl_host_delete_framebuffer(host, second_framebuffer, second_texture);

    srgb_framebuffer = opengl_host_create_framebuffer(
        host,
        BOXRIG_GL_TARGET_SRGB8_ALPHA8,
        64,
        48,
        &srgb_texture);
    CHECK(srgb_framebuffer != 0);
    target.draw_framebuffer = srgb_framebuffer;
    target.format = BOXRIG_GL_TARGET_SRGB8_ALPHA8;
    CHECK(boxrig_gl_runtime_begin_target(
              runtime,
              7,
              &target,
              &clip,
              &diagnostic) == BOXRIG_GL_OK);
    CHECK(boxrig_gl_runtime_end_target(runtime, 7, &diagnostic) == BOXRIG_GL_OK);
    opengl_host_delete_framebuffer(host, srgb_framebuffer, srgb_texture);

    target.draw_framebuffer = 0;
    target.format = BOXRIG_GL_TARGET_RGBA8;
    target.alpha_mode = BOXRIG_GL_ALPHA_OPAQUE;
    CHECK(boxrig_gl_runtime_begin_target(
              runtime,
              7,
              &target,
              &clip,
              &diagnostic) == BOXRIG_GL_OK);
    CHECK(boxrig_gl_runtime_end_target(runtime, 7, &diagnostic) == BOXRIG_GL_OK);
    {
        boxrig_gl_state_counters counters;

        CHECK(boxrig_gl_runtime_get_state_counters(runtime, &counters) ==
              BOXRIG_GL_OK);
        CHECK(counters.query_count != 0);
        CHECK(counters.change_count != 0);
        CHECK(counters.restore_count != 0);
    }
    CHECK(opengl_host_test_destroy_state(host, &runtime, 7));

    opengl_host_delete_framebuffer(host, framebuffer, texture);
    return true;
}

static bool abandons_objects_after_context_loss(opengl_host *host)
{
    boxrig_gl_runtime_config config = runtime_config(host);
    boxrig_gl_runtime *runtime = NULL;
    boxrig_gl_diagnostic diagnostic;

    CHECK(boxrig_gl_runtime_create(&config, &runtime, &diagnostic) ==
          BOXRIG_GL_OK);
    opengl_host_override_identity(host, true);
    CHECK(boxrig_gl_runtime_destroy(&runtime, 8, &diagnostic) ==
          BOXRIG_GL_CONTEXT_MISMATCH);
    CHECK(runtime == NULL);
    opengl_host_override_identity(host, false);
    return true;
}

static bool run_fixture(const char *wsi)
{
    opengl_host *host = NULL;
    opengl_host_report report;

    CHECK(opengl_host_create(wsi, &host, &report));
    CHECK(report.core_context == !report.is_gles);
    CHECK(report.resized);
    CHECK(report.context_recreated);
    CHECK(report.visual_id != 0);
    CHECK(report.visual_depth >= 24);
    CHECK(report.opaque_alpha_size == 0);
    CHECK(report.alpha_alpha_size == 8);
    if (strcmp(wsi, "glx") != 0) {
        CHECK(report.swap_interval);
        CHECK(strstr(report.wsi_extensions, "EGL_KHR_create_context") != NULL ||
              atoi(report.wsi_version) >= 1);
    } else {
        CHECK(strstr(report.wsi_extensions, "GLX_ARB_create_context") != NULL);
        CHECK(strstr(
                  report.wsi_extensions,
                  "GLX_ARB_create_context_profile") != NULL);
    }
    CHECK(validates_context_and_target(host, report.is_gles));
    CHECK(rejects_unsupported_contexts(host));
    CHECK(abandons_objects_after_context_loss(host));
    printf(
        "PASS OpenGL %s: WSI %s %s, GL %s, %s / %s, visual 0x%lx depth %d, alpha %d/%d, swap interval %s\n",
        wsi,
        report.wsi_vendor,
        report.wsi_version,
        report.gl_version,
        report.gl_vendor,
        report.gl_renderer,
        report.visual_id,
        report.visual_depth,
        report.opaque_alpha_size,
        report.alpha_alpha_size,
        report.swap_interval ? "yes" : "no");
    printf("WSI extensions: %s\n", report.wsi_extensions);
    opengl_host_destroy(host);
    return true;
}

int main(int argc, char **argv)
{
    if (argc != 2 || (strcmp(argv[1], "egl") != 0 &&
                      strcmp(argv[1], "egl-gles") != 0 &&
                      strcmp(argv[1], "glx") != 0)) {
        fprintf(stderr, "usage: %s egl|egl-gles|glx\n", argv[0]);
        return 2;
    }
    return run_fixture(argv[1]) ? 0 : 1;
}
