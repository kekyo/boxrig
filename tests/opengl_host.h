/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#ifndef BOXRIG_TEST_OPENGL_HOST_H
#define BOXRIG_TEST_OPENGL_HOST_H

#include "opengl/runtime.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct opengl_host opengl_host;

typedef struct opengl_host_report {
    char wsi_version[64];
    char wsi_vendor[128];
    char gl_version[128];
    char gl_vendor[128];
    char gl_renderer[256];
    char wsi_extensions[2048];
    unsigned long visual_id;
    int visual_depth;
    int opaque_alpha_size;
    int alpha_alpha_size;
    bool core_context;
    bool is_gles;
    bool swap_interval;
    bool resized;
    bool context_recreated;
} opengl_host_report;

bool opengl_host_create(
    const char *wsi,
    opengl_host **host_out,
    opengl_host_report *report_out);

void opengl_host_destroy(opengl_host *host);

boxrig_gl_function opengl_host_resolve(void *context, const char *name);

bool opengl_host_query_current(
    void *context,
    boxrig_gl_current_context *current_out);

void opengl_host_override_identity(opengl_host *host, bool enabled);

void opengl_host_override_thread(opengl_host *host, bool enabled);

void opengl_host_block_function(opengl_host *host, const char *name);

void opengl_host_override_version(opengl_host *host, const char *version);

void opengl_host_fail_shader_compile(opengl_host *host, bool enabled);

void opengl_host_fail_next_gl_check(opengl_host *host, bool enabled);

uint32_t opengl_host_create_framebuffer(
    opengl_host *host,
    boxrig_gl_target_format format,
    uint32_t width,
    uint32_t height,
    uint32_t *texture_out);

uint32_t opengl_host_create_empty_framebuffer(opengl_host *host);

void opengl_host_delete_framebuffer(
    opengl_host *host,
    uint32_t framebuffer,
    uint32_t texture);

bool opengl_host_read_pixel(
    opengl_host *host,
    uint32_t framebuffer,
    uint32_t target_height,
    int x,
    int y,
    uint8_t rgba_out[4]);

bool opengl_host_test_state_contract(
    opengl_host *host,
    boxrig_gl_runtime *runtime,
    const boxrig_gl_target *target);

bool opengl_host_test_destroy_state(
    opengl_host *host,
    boxrig_gl_runtime **runtime,
    uint64_t context_generation);

#endif
