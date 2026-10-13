/* boxrig - Under MIT. Installed notification SDK consumer. */
#include <boxrig/notification/glib.h>
#include <boxrig/ui.h>
#include <assert.h>
#include <stdio.h>

static uint64_t clock_read(void *context)
{
    (void)context;
    return 0;
}

static void failed(void *context)
{
    (void)context;
    assert(!"unexpected notification dispatch failure");
}

int main(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal;
    boxrig_ui_model *model;
    boxrig_ui_host_item_consumer *consumer;
    boxrig_freedesktop_notifications *notifications;
    assert(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    assert(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    assert(boxrig_ui_host_item_consumer_create(model, &consumer) == BOXRIG_UI_OK);
    boxrig_notification_policy_config policy = boxrig_notification_policy_config_default(clock_read, NULL);
    assert(boxrig_freedesktop_notifications_create(consumer, &policy, &notifications) == 0);
    GSource *source = boxrig_freedesktop_notifications_source_new(notifications, failed, NULL);
    assert(source != NULL);
    g_source_destroy(source);
    g_source_unref(source);
    boxrig_freedesktop_notifications_destroy(notifications);
    boxrig_ui_host_item_consumer_destroy(consumer);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    puts("PASS installed notification SDK lifecycle without GTK or a session bus");
    return 0;
}
