/* Exercise notification delivery through the installed shared SDK. */
#include <boxrig/notification/freedesktop.h>
#include <boxrig/ui.h>

#include <assert.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static uint64_t clock_read(void *context)
{
    (void)context;
    return 1000;
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    int service_fd = atoi(argv[1]);
    assert(service_fd >= 0);
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_host_item_consumer *consumer = NULL;
    boxrig_freedesktop_notifications *notifications = NULL;
    boxrig_notification_policy_config policy =
        boxrig_notification_policy_config_default(clock_read, NULL);
    policy.allow_notifications = true;
    assert(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    assert(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    assert(boxrig_ui_host_item_consumer_create(model, &consumer) == BOXRIG_UI_OK);
    assert(boxrig_freedesktop_notifications_create(consumer, &policy, &notifications) == 0);
    static const char sequence[] = "\033]99;i=plan17;notification shared SDK\033\\";
    size_t consumed = 0;
    assert(boxrig_terminal_feed(terminal, (const uint8_t *)sequence,
        sizeof(sequence) - 1, &consumed) == BOXRIG_STATUS_OK);
    assert(consumed == sizeof(sequence) - 1);
    assert(boxrig_freedesktop_notifications_process(notifications, false, true) == 0);
    bool delivered = false;
    while (!delivered) {
        const struct pollfd *borrowed = NULL;
        size_t count = 0;
        int timeout = -1;
        assert(boxrig_freedesktop_notifications_prepare_poll(notifications,
            &borrowed, &count, &timeout) == 0);
        struct pollfd *descriptors = calloc(count + 1, sizeof(*descriptors));
        assert(descriptors != NULL);
        if (count != 0) {
            memcpy(descriptors, borrowed, count * sizeof(*descriptors));
        }
        descriptors[count].fd = service_fd;
        descriptors[count].events = POLLIN;
        int ready = poll(descriptors, count + 1, timeout);
        assert(ready >= 0);
        assert(boxrig_freedesktop_notifications_dispatch_poll(notifications,
            descriptors, count) == 0);
        if ((descriptors[count].revents & POLLIN) != 0) {
            char marker = 0;
            assert(read(service_fd, &marker, 1) == 1);
            assert(marker == 'N');
            delivered = true;
        }
        free(descriptors);
    }
    boxrig_freedesktop_notifications_destroy(notifications);
    boxrig_ui_host_item_consumer_destroy(consumer);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    puts("PASS shared notification reached the D-Bus provider");
    return 0;
}
