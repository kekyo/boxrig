/* boxrig - Under MIT. */
#ifdef BOXRIG_TEST_GTK3
#include "boxrig/gtk3/background_effect.h"
#define EFFECT(name) boxrig_gtk3_background_effect_##name
#define Effect boxrig_gtk3_background_effect
#else
#include "boxrig/gtk4/background_effect.h"
#define EFFECT(name) boxrig_gtk4_background_effect_##name
#define Effect boxrig_gtk4_background_effect
#endif
#include "ime_server.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static int channel;
static ime_server_state state(void)
{
    gdk_display_sync(gdk_display_get_default());
    return ime_server_exchange(channel, (ime_server_command){.operation = IME_SERVER_QUERY});
}
#if defined(BOXRIG_TEST_GTK3) || GTK_CHECK_VERSION(4,18,0)
static void wait_blur(bool enabled, uint32_t count)
{
    while (true) {
        while (g_main_context_iteration(NULL, FALSE)) {}
        ime_server_state observed = state();
        if (observed.blur_enabled == (uint32_t)enabled && observed.blur_commits >= count) return;
        g_main_context_iteration(NULL, TRUE);
    }
}
#endif
int main(void)
{
    alarm(60); /* Deadlock watchdog; success is driven by observed commits. */
    g_log_set_always_fatal(G_LOG_FATAL_MASK | G_LOG_LEVEL_CRITICAL);
    pid_t server;
    channel = ime_server_start(2, 1, &server);
#ifdef BOXRIG_TEST_GTK3
    gtk_init(NULL, NULL);
    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
#else
    gtk_init();
    GtkWidget *window = gtk_window_new();
#endif
    gtk_window_set_default_size(GTK_WINDOW(window), 320, 200);
    Effect *effect;
    assert(EFFECT(create)(GTK_WINDOW(window), NULL, NULL, &effect) == BOXRIG_UI_OK);
    Effect *duplicate;
    assert(EFFECT(create)(GTK_WINDOW(window), NULL, NULL, &duplicate) == BOXRIG_UI_BUSY);
    boxrig_ui_background_effect_rectangle region = {20, 30, 100, 50};
    assert(EFFECT(set)(effect, BOXRIG_UI_BACKGROUND_EFFECT_PREFER, &region, 1) == BOXRIG_UI_OK);
    gtk_window_present(GTK_WINDOW(window));
#if !defined(BOXRIG_TEST_GTK3) && !GTK_CHECK_VERSION(4,18,0)
    while (g_main_context_iteration(NULL, FALSE)) {}
    assert(!EFFECT(get_state)(effect).accepted);
    assert(EFFECT(set)(effect, BOXRIG_UI_BACKGROUND_EFFECT_REQUIRE, &region, 1) == BOXRIG_UI_UNAVAILABLE);
    assert(state().blur_objects == 0);
    puts("PASS GTK4 before 4.18 reports unsupported without committing GDK's surface");
#else
    wait_blur(true, 1);
    ime_server_state first = state();
    assert(first.blur_x == 20 && first.blur_y == 30 && first.blur_width == 100 && first.blur_height == 50);
    assert(EFFECT(set)(effect, BOXRIG_UI_BACKGROUND_EFFECT_NONE, NULL, 0) == BOXRIG_UI_OK);
    wait_blur(false, first.blur_commits + 1);
    assert(EFFECT(set)(effect, BOXRIG_UI_BACKGROUND_EFFECT_PREFER, &region, 1) == BOXRIG_UI_OK);
    wait_blur(true, first.blur_commits + 2);
    gtk_widget_set_visible(window, FALSE);
    assert(state().blur_destroyed == 1);
    gtk_window_present(GTK_WINDOW(window));
    wait_blur(true, first.blur_commits + 3);
    assert(state().blur_objects == 2);
    (void)ime_server_exchange(channel, (ime_server_command){.operation = IME_SERVER_BLUR_CAPABILITY, .begin = 0});
    wait_blur(false, first.blur_commits + 4);
    assert(!EFFECT(get_state)(effect).accepted);
    /* Explicitly transfer host authority to a newly created toplevel. */
    EFFECT(destroy)(effect);
#ifdef BOXRIG_TEST_GTK3
    gtk_widget_destroy(window);
    (void)state();
    window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
#else
    gtk_window_destroy(GTK_WINDOW(window));
    (void)state();
    window = gtk_window_new();
#endif
    assert(EFFECT(create)(GTK_WINDOW(window), NULL, NULL, &effect) == BOXRIG_UI_OK);
    region = (boxrig_ui_background_effect_rectangle){3, 5, 60, 40};
    assert(EFFECT(set)(effect, BOXRIG_UI_BACKGROUND_EFFECT_PREFER, &region, 1) == BOXRIG_UI_OK);
    gtk_window_present(GTK_WINDOW(window));
    wait_blur(true, first.blur_commits + 5);
    assert(state().blur_x == 3 && state().blur_width == 60);
    puts("PASS GTK background effect commits on static content, unmap/remap and capability loss");
#endif
#ifdef BOXRIG_TEST_GTK3
    gtk_widget_destroy(window);
#else
    gtk_window_destroy(GTK_WINDOW(window));
#endif
    /* Hosts may dispose their window before releasing the retained adapter. */
    EFFECT(destroy)(effect);
    while (g_main_context_iteration(NULL, FALSE)) {}
    ime_server_stop(channel, server);
    alarm(0);
    return 0;
}
