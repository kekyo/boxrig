/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#define _GNU_SOURCE

#include "shm_private.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef __linux__
#include <linux/memfd.h>
#endif

static int create_fallback_shm_file(void)
{
    uint64_t random_value;
    char name[64];
    ssize_t received;
    size_t attempt;
    int fd;

    for (attempt = 0; attempt < 16; attempt++) {
        do {
            received = getrandom(
                &random_value,
                sizeof(random_value),
                0);
        } while (received == -1 && errno == EINTR);
        if (received != (ssize_t)sizeof(random_value)) {
            return -1;
        }
        if (snprintf(
                name,
                sizeof(name),
                "/boxrig-%016" PRIx64,
                random_value) < 0) {
            return -1;
        }
        fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd >= 0) {
            if (shm_unlink(name) == -1) {
                close(fd);
                return -1;
            }
            return fd;
        }
        if (errno != EEXIST) {
            return -1;
        }
    }
    errno = EEXIST;
    return -1;
}

static int create_shm_file(void)
{
    int fd = -1;

#ifdef __linux__
    fd = memfd_create("boxrig-wayland", MFD_CLOEXEC);
#endif
    return fd >= 0 ? fd : create_fallback_shm_file();
}

static void buffer_release(void *data, struct wl_buffer *buffer)
{
    boxrig_wayland_shm_buffer *entry = data;

    (void)buffer;
    entry->busy = false;
}

static const struct wl_buffer_listener buffer_listener = {
    .release = buffer_release,
};

boxrig_ui_status boxrig_wayland_shm_pool_create(
    boxrig_wayland_context *context,
    size_t buffer_count,
    unsigned int width,
    unsigned int height,
    uint32_t format,
    size_t maximum_bytes,
    size_t maximum_dirty_rectangles,
    boxrig_wayland_shm_pool **pool_out)
{
    boxrig_wayland_shm_pool *pool;
    size_t stride;
    size_t buffer_size;
    size_t mapping_size;
    size_t aggregate_size;
    size_t index;

    if (context == NULL || context->shm == NULL || pool_out == NULL ||
        buffer_count < BOXRIG_WAYLAND_BUFFERS_MINIMUM ||
        buffer_count > BOXRIG_WAYLAND_BUFFERS_MAXIMUM || width == 0 ||
        height == 0 || width > INT_MAX || height > INT_MAX ||
        maximum_bytes == 0 || maximum_bytes > BOXRIG_WAYLAND_BYTES_MAXIMUM ||
        maximum_dirty_rectangles == 0 ||
        maximum_dirty_rectangles > BOXRIG_WAYLAND_DIRTY_RECTS_MAXIMUM) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    *pool_out = NULL;
#if SIZE_MAX < UINT_MAX
    if (width > SIZE_MAX / 4u) {
        return BOXRIG_UI_LIMIT_EXCEEDED;
    }
#endif
    stride = (size_t)width * 4;
    if (stride > INT_MAX || height > SIZE_MAX / stride) {
        return BOXRIG_UI_LIMIT_EXCEEDED;
    }
    buffer_size = stride * height;
    if (buffer_count > SIZE_MAX / buffer_size) {
        return BOXRIG_UI_LIMIT_EXCEEDED;
    }
    mapping_size = buffer_size * buffer_count;
    if (mapping_size > SIZE_MAX - buffer_size) {
        return BOXRIG_UI_LIMIT_EXCEEDED;
    }
    aggregate_size = mapping_size + buffer_size;
    if (aggregate_size > maximum_bytes || mapping_size > INT_MAX) {
        return BOXRIG_UI_LIMIT_EXCEEDED;
    }
    pool = calloc(1, sizeof(*pool));
    if (pool == NULL) {
        return BOXRIG_UI_OUT_OF_MEMORY;
    }
    pool->fd = -1;
    pool->buffers = calloc(buffer_count, sizeof(*pool->buffers));
    if (pool->buffers == NULL) {
        boxrig_wayland_shm_pool_destroy(pool);
        return BOXRIG_UI_OUT_OF_MEMORY;
    }
    pool->buffer_count = buffer_count;
    pool->maximum_dirty_rectangles = maximum_dirty_rectangles;
    for (index = 0; index < buffer_count; index++) {
        pool->buffers[index].dirty_rectangles = calloc(
            maximum_dirty_rectangles,
            sizeof(*pool->buffers[index].dirty_rectangles));
        if (pool->buffers[index].dirty_rectangles == NULL) {
            boxrig_wayland_shm_pool_destroy(pool);
            return BOXRIG_UI_OUT_OF_MEMORY;
        }
        pool->buffers[index].full_dirty = true;
    }
    pool->fd = create_shm_file();
    if (pool->fd == -1 || ftruncate(pool->fd, (off_t)mapping_size) == -1) {
        boxrig_wayland_shm_pool_destroy(pool);
        return BOXRIG_UI_UNAVAILABLE;
    }
    pool->mapping = mmap(
        NULL,
        mapping_size,
        PROT_READ | PROT_WRITE,
        MAP_SHARED,
        pool->fd,
        0);
    if (pool->mapping == MAP_FAILED) {
        pool->mapping = NULL;
        boxrig_wayland_shm_pool_destroy(pool);
        return BOXRIG_UI_OUT_OF_MEMORY;
    }
    pool->proxy = wl_shm_create_pool(
        context->shm,
        pool->fd,
        (int32_t)mapping_size);
    if (pool->proxy == NULL) {
        boxrig_wayland_shm_pool_destroy(pool);
        return BOXRIG_UI_OUT_OF_MEMORY;
    }
    wl_proxy_set_queue((struct wl_proxy *)pool->proxy, context->queue);
    pool->mapping_size = mapping_size;
    pool->buffer_size = buffer_size;
    pool->width = (int32_t)width;
    pool->height = (int32_t)height;
    pool->stride = (int32_t)stride;
    pool->format = format;
    for (index = 0; index < buffer_count; index++) {
        pool->buffers[index].proxy = wl_shm_pool_create_buffer(
            pool->proxy,
            (int32_t)(index * buffer_size),
            pool->width,
            pool->height,
            pool->stride,
            format);
        if (pool->buffers[index].proxy == NULL) {
            boxrig_wayland_shm_pool_destroy(pool);
            return BOXRIG_UI_OUT_OF_MEMORY;
        }
        wl_proxy_set_queue(
            (struct wl_proxy *)pool->buffers[index].proxy,
            context->queue);
        if (wl_buffer_add_listener(
                pool->buffers[index].proxy,
                &buffer_listener,
                &pool->buffers[index]) != 0) {
            boxrig_wayland_shm_pool_destroy(pool);
            return BOXRIG_UI_UNAVAILABLE;
        }
    }
    context->flush_required = true;
    *pool_out = pool;
    return BOXRIG_UI_OK;
}

void boxrig_wayland_shm_pool_destroy(boxrig_wayland_shm_pool *pool)
{
    size_t index;

    if (pool == NULL) {
        return;
    }
    if (pool->buffers != NULL) {
        for (index = pool->buffer_count; index != 0; index--) {
            if (pool->buffers[index - 1].proxy != NULL) {
                wl_buffer_destroy(pool->buffers[index - 1].proxy);
            }
            free(pool->buffers[index - 1].dirty_rectangles);
        }
    }
    if (pool->proxy != NULL) {
        wl_shm_pool_destroy(pool->proxy);
    }
    if (pool->mapping != NULL) {
        munmap(pool->mapping, pool->mapping_size);
    }
    if (pool->fd != -1) {
        close(pool->fd);
    }
    free(pool->buffers);
    free(pool);
}

boxrig_ui_status boxrig_wayland_shm_pool_copy_cairo(
    boxrig_wayland_shm_pool *pool,
    cairo_surface_t *surface,
    uint64_t generation)
{
    size_t index;

    if (pool == NULL) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    for (index = 0; index < pool->buffer_count; index++) {
        boxrig_ui_status status = boxrig_wayland_shm_buffer_copy_cairo(
            pool,
            index,
            surface,
            generation);

        if (status != BOXRIG_UI_OK) {
            return status;
        }
    }
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_wayland_shm_buffer_copy_cairo(
    boxrig_wayland_shm_pool *pool,
    size_t buffer_index,
    cairo_surface_t *surface,
    uint64_t generation)
{
    unsigned char *destination;
    unsigned char *source;
    boxrig_wayland_shm_buffer *buffer;
    size_t dirty_index;
    int source_stride;
    int32_t row;

    if (pool == NULL || buffer_index >= pool->buffer_count ||
        surface == NULL || generation == 0 ||
        cairo_surface_get_type(surface) != CAIRO_SURFACE_TYPE_IMAGE ||
        cairo_image_surface_get_width(surface) != pool->width ||
        cairo_image_surface_get_height(surface) != pool->height) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    cairo_surface_flush(surface);
    source = cairo_image_surface_get_data(surface);
    source_stride = cairo_image_surface_get_stride(surface);
    if (source == NULL || source_stride < pool->stride) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    buffer = &pool->buffers[buffer_index];
    destination = (unsigned char *)pool->mapping +
                  buffer_index * pool->buffer_size;
    if (buffer->full_dirty || !buffer->presented ||
        buffer->dirty_count == 0) {
        for (row = 0; row < pool->height; row++) {
            memcpy(
                destination + (size_t)row * (size_t)pool->stride,
                source + (size_t)row * (size_t)source_stride,
                (size_t)pool->stride);
        }
    } else {
        for (dirty_index = 0;
             dirty_index < buffer->dirty_count;
             dirty_index++) {
            boxrig_wayland_dirty_rectangle *rectangle =
                &buffer->dirty_rectangles[dirty_index];
            size_t x_offset = (size_t)rectangle->x * 4u;
            size_t bytes = (size_t)rectangle->width * 4u;
            int32_t last_row = rectangle->y + rectangle->height;

            for (row = rectangle->y; row < last_row; row++) {
                memcpy(
                    destination + (size_t)row * (size_t)pool->stride +
                        x_offset,
                    source + (size_t)row * (size_t)source_stride + x_offset,
                    bytes);
            }
        }
    }
    buffer->generation = generation;
    return BOXRIG_UI_OK;
}

static bool rectangles_overlap(
    const boxrig_wayland_dirty_rectangle *left,
    const boxrig_wayland_dirty_rectangle *right)
{
    int64_t left_right = (int64_t)left->x + left->width;
    int64_t left_bottom = (int64_t)left->y + left->height;
    int64_t right_right = (int64_t)right->x + right->width;
    int64_t right_bottom = (int64_t)right->y + right->height;

    return left->x < right_right && right->x < left_right &&
           left->y < right_bottom && right->y < left_bottom;
}

static void merge_rectangle(
    boxrig_wayland_dirty_rectangle *target,
    const boxrig_wayland_dirty_rectangle *addition)
{
    int32_t left = target->x < addition->x ? target->x : addition->x;
    int32_t top = target->y < addition->y ? target->y : addition->y;
    int64_t target_right = (int64_t)target->x + target->width;
    int64_t addition_right = (int64_t)addition->x + addition->width;
    int64_t target_bottom = (int64_t)target->y + target->height;
    int64_t addition_bottom = (int64_t)addition->y + addition->height;
    int64_t right = target_right > addition_right
                        ? target_right
                        : addition_right;
    int64_t bottom = target_bottom > addition_bottom
                         ? target_bottom
                         : addition_bottom;

    target->x = left;
    target->y = top;
    target->width = (int32_t)(right - left);
    target->height = (int32_t)(bottom - top);
}

void boxrig_wayland_shm_pool_add_dirty(
    boxrig_wayland_shm_pool *pool,
    int32_t x,
    int32_t y,
    int32_t width,
    int32_t height)
{
    boxrig_wayland_dirty_rectangle rectangle;
    int64_t right;
    int64_t bottom;
    size_t buffer_index;

    if (pool == NULL || width <= 0 || height <= 0) {
        return;
    }
    right = (int64_t)x + width;
    bottom = (int64_t)y + height;
    if (right <= 0 || bottom <= 0 || x >= pool->width || y >= pool->height) {
        return;
    }
    rectangle.x = x < 0 ? 0 : x;
    rectangle.y = y < 0 ? 0 : y;
    right = right > pool->width ? pool->width : right;
    bottom = bottom > pool->height ? pool->height : bottom;
    rectangle.width = (int32_t)(right - rectangle.x);
    rectangle.height = (int32_t)(bottom - rectangle.y);
    for (buffer_index = 0;
         buffer_index < pool->buffer_count;
         buffer_index++) {
        boxrig_wayland_shm_buffer *buffer = &pool->buffers[buffer_index];
        size_t dirty_index;

        if (buffer->full_dirty) {
            continue;
        }
        for (dirty_index = 0;
             dirty_index < buffer->dirty_count;
             dirty_index++) {
            if (rectangles_overlap(
                    &buffer->dirty_rectangles[dirty_index],
                    &rectangle)) {
                merge_rectangle(
                    &buffer->dirty_rectangles[dirty_index],
                    &rectangle);
                break;
            }
        }
        if (dirty_index < buffer->dirty_count) {
            continue;
        }
        if (buffer->dirty_count == pool->maximum_dirty_rectangles) {
            buffer->dirty_count = 0;
            buffer->full_dirty = true;
            pool->dirty_collapse_count++;
            continue;
        }
        buffer->dirty_rectangles[buffer->dirty_count++] = rectangle;
        if (buffer->dirty_count > pool->dirty_rectangle_high_water) {
            pool->dirty_rectangle_high_water = buffer->dirty_count;
        }
    }
}

void boxrig_wayland_shm_pool_add_full_dirty(
    boxrig_wayland_shm_pool *pool)
{
    size_t index;

    if (pool == NULL) {
        return;
    }
    for (index = 0; index < pool->buffer_count; index++) {
        pool->buffers[index].dirty_count = 0;
        pool->buffers[index].full_dirty = true;
    }
}

void boxrig_wayland_shm_buffer_clear_dirty(
    boxrig_wayland_shm_buffer *buffer)
{
    if (buffer == NULL) {
        return;
    }
    buffer->dirty_count = 0;
    buffer->full_dirty = false;
    buffer->presented = true;
}
