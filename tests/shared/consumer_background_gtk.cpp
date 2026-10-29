/* Exercise the installed GTK background effect API from C++20. */
#if defined(BOXRIG_TEST_GTK3)
#include <boxrig/gtk3/background_effect.h>
#define EFFECT(name) boxrig_gtk3_background_effect_##name
#define Effect boxrig_gtk3_background_effect
#else
#include <boxrig/gtk4/background_effect.h>
#define EFFECT(name) boxrig_gtk4_background_effect_##name
#define Effect boxrig_gtk4_background_effect
#endif

#include <cassert>
#include <cstdio>

int main()
{
#if defined(BOXRIG_TEST_GTK3)
    gtk_init(nullptr, nullptr);
    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
#else
    gtk_init();
    GtkWidget *window = gtk_window_new();
#endif
    Effect *effect = nullptr;
    assert(EFFECT(create)(GTK_WINDOW(window), nullptr, nullptr, &effect) == BOXRIG_UI_OK);
    boxrig_ui_background_effect_rectangle region = {0, 0, 100, 100};
    assert(EFFECT(set)(effect, BOXRIG_UI_BACKGROUND_EFFECT_PREFER, &region, 1) == BOXRIG_UI_OK);
    gtk_window_present(GTK_WINDOW(window));
    while (g_main_context_iteration(nullptr, FALSE)) {}
    assert(!EFFECT(get_state)(effect).accepted);
    assert(EFFECT(set)(effect, BOXRIG_UI_BACKGROUND_EFFECT_REQUIRE, &region, 1) == BOXRIG_UI_UNAVAILABLE);
    assert(EFFECT(get_state)(effect).request == BOXRIG_UI_BACKGROUND_EFFECT_PREFER);
    EFFECT(destroy)(effect);
#if defined(BOXRIG_TEST_GTK3)
    gtk_widget_destroy(window);
#else
    gtk_window_destroy(GTK_WINDOW(window));
#endif
    std::puts("PASS installed GTK background effect from C++20");
    return 0;
}
