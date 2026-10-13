/* boxrig - Under MIT. */
#include "boxrig/ui/animation_poll.h"

#include <limits.h>
#include <stdlib.h>

struct boxrig_ui_animation_poll {
    boxrig_ui_animation_runtime *runtime;
    boxrig_ui_animation_time_reader read_time;
    void *clock_context;
    bool time_set;
    uint64_t last_time;
    bool armed;
    uint64_t deadline;
    uint64_t generation;
};

static boxrig_ui_status poll_read_time(void *context, uint64_t *time_out)
{
    boxrig_ui_animation_poll *adapter = context;
    uint64_t now = 0;
    boxrig_ui_status status = adapter->read_time(adapter->clock_context, &now);
    if (status != BOXRIG_UI_OK) return status;
    if (adapter->time_set && now < adapter->last_time) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    adapter->time_set = true;
    adapter->last_time = now;
    *time_out = now;
    return BOXRIG_UI_OK;
}

static boxrig_ui_status poll_reserve(
    void *context, uint64_t deadline, uint64_t generation)
{
    boxrig_ui_animation_poll *adapter = context;
    adapter->armed = true;
    adapter->deadline = deadline;
    adapter->generation = generation;
    return BOXRIG_UI_OK;
}

static void poll_cancel(void *context)
{
    ((boxrig_ui_animation_poll *)context)->armed = false;
}

boxrig_ui_status boxrig_ui_animation_poll_create(
    boxrig_ui_animation_time_reader read_time, void *context,
    boxrig_ui_animation_poll **adapter_out)
{
    if (adapter_out == NULL) return BOXRIG_UI_INVALID_ARGUMENT;
    *adapter_out = NULL;
    if (read_time == NULL) return BOXRIG_UI_INVALID_ARGUMENT;
    boxrig_ui_animation_poll *adapter = calloc(1, sizeof(*adapter));
    if (adapter == NULL) return BOXRIG_UI_OUT_OF_MEMORY;
    adapter->read_time = read_time;
    adapter->clock_context = context;
    boxrig_ui_animation_scheduler scheduler = {
        adapter, poll_read_time, poll_reserve, poll_cancel
    };
    boxrig_ui_status status = boxrig_ui_animation_runtime_create(
        &scheduler, &adapter->runtime);
    if (status != BOXRIG_UI_OK) {
        free(adapter);
        return status;
    }
    *adapter_out = adapter;
    return BOXRIG_UI_OK;
}

void boxrig_ui_animation_poll_destroy(boxrig_ui_animation_poll *adapter)
{
    if (adapter == NULL) return;
    boxrig_ui_animation_runtime *runtime = adapter->runtime;
    adapter->runtime = NULL;
    boxrig_ui_animation_runtime_destroy(runtime);
    free(adapter);
}

boxrig_ui_animation_runtime *boxrig_ui_animation_poll_runtime(
    boxrig_ui_animation_poll *adapter)
{
    return adapter == NULL ? NULL : adapter->runtime;
}

boxrig_ui_status boxrig_ui_animation_poll_prepare_wait(
    boxrig_ui_animation_poll *adapter, int host_timeout, int *timeout_out)
{
    if (adapter == NULL || timeout_out == NULL || host_timeout < -1) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    boxrig_ui_status status = boxrig_ui_animation_runtime_status(adapter->runtime);
    if (status != BOXRIG_UI_OK) return status;
    int timeout = host_timeout;
    if (adapter->armed) {
        uint64_t now = 0;
        status = poll_read_time(adapter, &now);
        if (status != BOXRIG_UI_OK) return status;
        uint64_t remaining = adapter->deadline > now ? adapter->deadline - now : 0;
        int animation_timeout = remaining > (uint64_t)INT_MAX ? INT_MAX : (int)remaining;
        if (timeout < 0 || animation_timeout < timeout) timeout = animation_timeout;
    }
    *timeout_out = timeout;
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_ui_animation_poll_dispatch(boxrig_ui_animation_poll *adapter)
{
    if (adapter == NULL) return BOXRIG_UI_INVALID_ARGUMENT;
    boxrig_ui_status status = boxrig_ui_animation_runtime_status(adapter->runtime);
    if (status != BOXRIG_UI_OK || !adapter->armed) return status;
    uint64_t now = 0;
    status = poll_read_time(adapter, &now);
    if (status != BOXRIG_UI_OK || now < adapter->deadline) return status;
    uint64_t generation = adapter->generation;
    adapter->armed = false;
    return boxrig_ui_animation_runtime_dispatch(adapter->runtime, generation);
}
