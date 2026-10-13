/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#ifndef BOXRIG_TEST_FRONTEND_SAMPLE_WAYLAND_PROBE_H
#define BOXRIG_TEST_FRONTEND_SAMPLE_WAYLAND_PROBE_H

/* native_host.c enables GNU declarations before its normal includes. A forced
 * test include must preserve that ordering for memfd_create and related APIs. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <wayland-client.h>

/**
 * Records each sample-owned surface returned by the real API.
 * @param compositor Actual compositor used by the sample.
 * @return The newly created surface, or NULL on creation failure.
 */
struct wl_surface *boxrig_test_sample_wayland_surface_create(
    struct wl_compositor *compositor);

/**
 * Observes release of a sample-owned root or pointer surface through the real API.
 * @param surface Actual surface returned by the observed creation call.
 * @remarks Only the test sample translation units include this seam. The
 * library and external dependencies retain their original inline calls.
 */
void boxrig_test_sample_wayland_surface_destroy(struct wl_surface *surface);

/* The public generated functions are inline, so the ordinary linker seam
 * cannot observe them. Include their declarations before redirecting sample calls. */
#define wl_compositor_create_surface boxrig_test_sample_wayland_surface_create
#define wl_surface_destroy boxrig_test_sample_wayland_surface_destroy

#endif
