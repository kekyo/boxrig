/* boxrig - Under MIT. Installed optional adapter consumer. */
#ifdef BOXRIG_TEST_GTK3
#include <boxrig/gtk3/background_effect.h>
#define EFFECT(name) boxrig_gtk3_background_effect_##name
#define Effect boxrig_gtk3_background_effect
#else
#include <boxrig/gtk4/background_effect.h>
#define EFFECT(name) boxrig_gtk4_background_effect_##name
#define Effect boxrig_gtk4_background_effect
#endif
#include <assert.h>
#include <stdio.h>

int main(void)
{
    g_log_set_always_fatal(G_LOG_FATAL_MASK | G_LOG_LEVEL_CRITICAL);
#ifdef BOXRIG_TEST_GTK3
    gtk_init(NULL, NULL);
    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
#else
    gtk_init();
    GtkWidget *window = gtk_window_new();
#endif
    Effect *effect;
    assert(EFFECT(create)(GTK_WINDOW(window), NULL, NULL, &effect) == BOXRIG_UI_OK);
    boxrig_ui_background_effect_rectangle region = {0, 0, 100, 100};
    assert(EFFECT(set)(effect, BOXRIG_UI_BACKGROUND_EFFECT_PREFER, &region, 1) == BOXRIG_UI_OK);
    gtk_window_present(GTK_WINDOW(window));
    while (g_main_context_iteration(NULL, FALSE)) {}
    /* Xvfb has neither an active compositor nor a blur advertisement. */
    assert(!EFFECT(get_state)(effect).accepted);
    assert(EFFECT(set)(effect, BOXRIG_UI_BACKGROUND_EFFECT_REQUIRE, &region, 1) == BOXRIG_UI_UNAVAILABLE);
    assert(EFFECT(get_state)(effect).request == BOXRIG_UI_BACKGROUND_EFFECT_PREFER);
    EFFECT(destroy)(effect);
#ifdef BOXRIG_TEST_GTK3
    gtk_widget_destroy(window);
#else
    gtk_window_destroy(GTK_WINDOW(window));
#endif
    puts("PASS installed optional GTK background adapter links and reports unavailable X11 blur");
    return 0;
}
