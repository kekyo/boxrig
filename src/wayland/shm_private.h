/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#ifndef BOXRIG_WAYLAND_SHM_PRIVATE_H
#define BOXRIG_WAYLAND_SHM_PRIVATE_H

#include "context_private.h"

#include <cairo/cairo.h>

typedef struct boxrig_wayland_dirty_rectangle {
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
} boxrig_wayland_dirty_rectangle;

typedef struct boxrig_wayland_shm_buffer {
    struct wl_buffer *proxy;
    boxrig_wayland_dirty_rectangle *dirty_rectangles;
    size_t dirty_count;
    uint64_t generation;
    bool full_dirty;
    bool presented;
    bool busy;
} boxrig_wayland_shm_buffer;

typedef struct boxrig_wayland_shm_pool {
    struct wl_shm_pool *proxy;
    boxrig_wayland_shm_buffer *buffers;
    void *mapping;
    size_t mapping_size;
    size_t buffer_size;
    size_t buffer_count;
    size_t maximum_dirty_rectangles;
    size_t dirty_rectangle_high_water;
    size_t dirty_collapse_count;
    int fd;
    int32_t width;
    int32_t height;
    int32_t stride;
    uint32_t format;
} boxrig_wayland_shm_pool;

boxrig_ui_status boxrig_wayland_shm_pool_create(
    boxrig_wayland_context *context,
    size_t buffer_count,
    unsigned int width,
    unsigned int height,
    uint32_t format,
    size_t maximum_bytes,
    size_t maximum_dirty_rectangles,
    boxrig_wayland_shm_pool **pool_out);

void boxrig_wayland_shm_pool_destroy(boxrig_wayland_shm_pool *pool);

boxrig_ui_status boxrig_wayland_shm_pool_copy_cairo(
    boxrig_wayland_shm_pool *pool,
    cairo_surface_t *surface,
    uint64_t generation);

boxrig_ui_status boxrig_wayland_shm_buffer_copy_cairo(
    boxrig_wayland_shm_pool *pool,
    size_t buffer_index,
    cairo_surface_t *surface,
    uint64_t generation);

void boxrig_wayland_shm_pool_add_dirty(
    boxrig_wayland_shm_pool *pool,
    int32_t x,
    int32_t y,
    int32_t width,
    int32_t height);

void boxrig_wayland_shm_pool_add_full_dirty(
    boxrig_wayland_shm_pool *pool);

void boxrig_wayland_shm_buffer_clear_dirty(
    boxrig_wayland_shm_buffer *buffer);

#endif
