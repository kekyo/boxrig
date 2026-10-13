/* Custom event-loop animation integration - Under MIT. */
#include "boxrig/ui/animation.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

/* A real host keeps this registration in its event-loop registry. Native
 * queued messages contain the registration ID and generation, never a
 * borrowed component pointer. This example advances the host clock explicitly
 * so the same delivery and teardown behavior can be observed without sleeps. */
typedef struct host_loop {
    boxrig_ui_animation_runtime *runtime;
    uint64_t registration;
    uint64_t now;
    uint64_t deadline;
    uint64_t generation;
    bool armed;
    unsigned int requested_frames;
} host_loop;

static boxrig_ui_status read_time(void *context, uint64_t *now_out)
{
    *now_out = ((host_loop *)context)->now;
    return BOXRIG_UI_OK;
}

static boxrig_ui_status reserve(void *context, uint64_t deadline,
    uint64_t generation)
{
    host_loop *host = context;
    /* Replace the native one-shot only after its allocation succeeds. A real
     * adapter returns an error without changing the old reservation on failure. */
    host->deadline = deadline;
    host->generation = generation;
    host->armed = true;
    return BOXRIG_UI_OK;
}

static void cancel(void *context)
{
    ((host_loop *)context)->armed = false;
}

static void request_frame(void *context)
{
    /* Schedule the ordinary frame path; never render inside timer delivery. */
    ((host_loop *)context)->requested_frames++;
}

static boxrig_ui_status deliver(host_loop *host, uint64_t registration,
    uint64_t generation)
{
    /* Look up a live registration before touching its runtime. */
    if (host->runtime == NULL || registration != host->registration)
        return BOXRIG_UI_OK;
    if (generation == host->generation) host->armed = false;
    return boxrig_ui_animation_runtime_dispatch(host->runtime, generation);
}

static void accept_frame(boxrig_ui_view *view)
{
    /* A real renderer consumes these after presenting the requested frame. */
    for (int layer = 0; layer < BOXRIG_UI_DIRTY_LAYER_COUNT; layer++) {
        boxrig_ui_dirty dirty;
        while (boxrig_ui_view_peek_dirty(view, (boxrig_ui_dirty_layer)layer,
                &dirty))
            (void)boxrig_ui_view_consume_dirty(view, (boxrig_ui_dirty_layer)layer);
    }
}

int main(void)
{
    host_loop host = {0};
    host.registration = 1;
    host.now = 1000;
    boxrig_ui_animation_scheduler scheduler = {&host, read_time, reserve, cancel};
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *first = NULL, *second = NULL;
    boxrig_ui_animation_runtime *runtime = NULL;
    int result = 1;
    config.rows = 2;
    config.columns = 8;
    const char text[] = "\033[5mS\033[6mR\033[0m\033[2 q";
    size_t consumed = 0;
    if (boxrig_terminal_create(&config, &terminal) != BOXRIG_STATUS_OK ||
        boxrig_terminal_feed(terminal, (const uint8_t *)text, sizeof(text) - 1,
            &consumed) != BOXRIG_STATUS_OK || consumed != sizeof(text) - 1 ||
        boxrig_ui_model_create(terminal, &model) != BOXRIG_UI_OK ||
        boxrig_ui_view_create(model, &view_config, &first) != BOXRIG_UI_OK ||
        boxrig_ui_view_create(model, &view_config, &second) != BOXRIG_UI_OK ||
        boxrig_ui_animation_runtime_create(&scheduler, &runtime) != BOXRIG_UI_OK)
        goto cleanup;
    host.runtime = runtime;
    boxrig_ui_cursor_style style = boxrig_ui_cursor_style_default();
    style.blink = BOXRIG_UI_CURSOR_BLINK_ENABLED;
    if (boxrig_ui_view_set_cursor_style(first, &style) != BOXRIG_UI_OK ||
        boxrig_ui_view_set_focused(first, true) != BOXRIG_UI_OK ||
        boxrig_ui_view_set_frame_request_callback(first, request_frame, &host) != BOXRIG_UI_OK ||
        boxrig_ui_animation_runtime_attach(runtime, first) != BOXRIG_UI_OK ||
        boxrig_ui_animation_runtime_attach(runtime, second) != BOXRIG_UI_OK ||
        !host.armed || host.deadline != 1150)
        goto cleanup;
    uint64_t obsolete_generation = host.generation;
    accept_frame(first);
    host.requested_frames = 0;
    host.now = 1150;
    if (deliver(&host, host.registration, host.generation) != BOXRIG_UI_OK ||
        boxrig_ui_view_text_blink_visible(first, BOXRIG_CELL_BLINK | BOXRIG_CELL_RAPID_BLINK) ||
        boxrig_ui_view_text_blink_visible(second, BOXRIG_CELL_BLINK | BOXRIG_CELL_RAPID_BLINK) ||
        !boxrig_ui_view_text_blink_visible(first, BOXRIG_CELL_BLINK) ||
        host.requested_frames != 1 || host.deadline != 1300)
        goto cleanup;
    printf("shared deadline=%" PRIu64 " requested-frames=%u\n",
        host.deadline, host.requested_frames);
    unsigned int requests = host.requested_frames;
    if (deliver(&host, host.registration, obsolete_generation) != BOXRIG_UI_OK ||
        host.requested_frames != requests)
        goto cleanup;
    /* One notification after a long pause selects current phases directly. */
    host.now = UINT64_C(1000000001500);
    if (deliver(&host, host.registration, host.generation) != BOXRIG_UI_OK ||
        boxrig_ui_view_cursor_blink_visible(first) ||
        !boxrig_ui_view_cursor_blink_visible(second))
        goto cleanup;
    if (boxrig_ui_view_set_visible(first, false) != BOXRIG_UI_OK ||
        boxrig_ui_view_set_visible(second, false) != BOXRIG_UI_OK || host.armed)
        goto cleanup;
    printf("hidden armed=%u\n", host.armed ? 1u : 0u);
    result = 0;
cleanup:
    /* Invalidate host delivery first, then release every borrowed resource. */
    host.runtime = NULL;
    host.registration++;
    boxrig_ui_animation_runtime_destroy(runtime);
    if (deliver(&host, 1, host.generation) != BOXRIG_UI_OK) result = 1;
    boxrig_ui_view_destroy(first);
    boxrig_ui_view_destroy(second);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    if (result == 0) puts("PASS custom scheduler, shared views and late delivery");
    return result;
}
