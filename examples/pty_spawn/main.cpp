/* boxrig - Under MIT. Startup/cancellation GUI using only the public PTY APIs. */
#include "operation.h"
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <exception>
#if !defined(_WIN32)
#include <gtk/gtk.h>
#endif

namespace spawn = boxrig_spawn_example;

struct application {
    spawn::request command;
    std::shared_ptr<spawn::controller> owner;
    std::function<void()> quit;
    bool closing = false;
    bool once = false;
#if defined(_WIN32)
    HWND window = nullptr, label = nullptr, start_button = nullptr, stop_button = nullptr;
#else
    GtkWidget *window = nullptr, *label = nullptr, *start_button = nullptr, *stop_button = nullptr;
#endif
};

static void status(application *app, const char *text)
{
    if (app->closing) return;
#if defined(_WIN32)
    SetWindowTextA(app->label, text);
    EnableWindow(app->start_button, !spawn::active(app->owner));
    EnableWindow(app->stop_button, spawn::active(app->owner));
#else
    gtk_label_set_text(GTK_LABEL(app->label), text);
    gtk_widget_set_sensitive(app->start_button, !spawn::active(app->owner));
    gtk_widget_set_sensitive(app->stop_button, spawn::active(app->owner));
#endif
}

static void close_window(application *app)
{
    app->closing = true;
    if (spawn::cancel(app->owner)) {
#if defined(_WIN32)
        ShowWindow(app->window, SW_HIDE);
#else
        gtk_widget_hide(app->window);
#endif
    } else {
#if defined(_WIN32)
        DestroyWindow(app->window);
#else
        gtk_widget_destroy(app->window);
#endif
        app->quit();
    }
}

static void begin(application *app)
{
    try {
        if (spawn::start(app->owner, app->command)) status(app, "Starting process...");
    } catch (const std::exception &error) {
        status(app, error.what());
    }
}

static void stop(application *app)
{
    if (spawn::cancel(app->owner)) status(app, "Stopping process...");
}

static void notify(application *app, spawn::event event, const spawn::report &result)
{
    char text[256];
    if (event == spawn::event::ready) {
        std::snprintf(text, sizeof(text), "Process %s. PID=%llu", result.started ? "started" : "failed to start",
            static_cast<unsigned long long>(result.child_id));
        status(app, text);
        return;
    }
#if defined(_WIN32)
    const auto spawn_error = result.diagnostic.win32_error;
#else
    const auto spawn_error = static_cast<unsigned long>(result.diagnostic.error_number);
#endif
    std::snprintf(text, sizeof(text), "%s. Exit=%u signal=%d output=%llu error=%lu",
        result.cancelled ? "Cancelled" : result.started ? "Finished" : "Failed to start", result.exit_code,
        result.signal_number, static_cast<unsigned long long>(result.output_bytes),
        spawn_error != 0 ? spawn_error : result.operation_error);
    std::printf("%s; generation=%llu resources=recovered\n", text, static_cast<unsigned long long>(result.generation));
    status(app, text);
    if (app->closing || app->once) close_window(app);
}

#if defined(_WIN32)
static application *current;
static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_COMMAND && HIWORD(wparam) == BN_CLICKED) {
        if (LOWORD(wparam) == 1) begin(current);
        if (LOWORD(wparam) == 2) stop(current);
        return 0;
    }
    if (message == WM_CLOSE) { close_window(current); return 0; }
    return DefWindowProcW(window, message, wparam, lparam);
}

int wmain(int argc, wchar_t **argv)
{
    application app;
    const int first = argc > 1 && std::wcscmp(argv[1], L"--once") == 0 ? 2 : 1;
    if (argc != first + 1) {
        std::fputs("Usage: pty_spawn.exe [--once] \"command line\"\n", stderr);
        return 2;
    }
    app.once = first == 2;
    app.command.command_line = argv[first];
    cardio::dispatcher_group group(cardio::exit_condition::exit_by_manual);
    cardio::dispatcher_host_win32 dispatcher(group);
    cardio::set_current_dispatcher(&dispatcher);
    app.quit = [&] { group.shutdown(); };
    app.owner = spawn::create([&](spawn::event event, const spawn::report &result) { notify(&app, event, result); });
    current = &app;
    WNDCLASSW type{};
    type.lpfnWndProc = procedure;
    type.hInstance = GetModuleHandleW(nullptr);
    type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    type.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    type.lpszClassName = L"boxrig-pty-spawn";
    if (!RegisterClassW(&type)) return 1;
    app.window = CreateWindowW(type.lpszClassName, L"boxrig process startup", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 640, 180, nullptr, nullptr, type.hInstance, nullptr);
    if (!app.window) return 1;
    app.label = CreateWindowW(L"STATIC", L"Ready", WS_CHILD | WS_VISIBLE,
        16, 16, 600, 40, app.window, nullptr, type.hInstance, nullptr);
    app.start_button = CreateWindowW(L"BUTTON", L"Start", WS_CHILD | WS_VISIBLE,
        16, 64, 100, 32, app.window, reinterpret_cast<HMENU>(1), type.hInstance, nullptr);
    app.stop_button = CreateWindowW(L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE,
        128, 64, 100, 32, app.window, reinterpret_cast<HMENU>(2), type.hInstance, nullptr);
    ShowWindow(app.window, SW_SHOW);
    status(&app, "Ready");
    if (app.once) begin(&app);
    dispatcher.park();
    UnregisterClassW(type.lpszClassName, type.hInstance);
    return 0;
}
#else
int main(int argc, char **argv)
{
    application app;
    const int first = argc > 1 && std::strcmp(argv[1], "--once") == 0 ? 2 : 1;
    if (argc <= first) {
        std::fputs("Usage: pty_spawn [--once] command [arguments...]\n", stderr);
        return 2;
    }
    app.once = first == 2;
    for (int index = first; index < argc; index++) app.command.arguments.emplace_back(argv[index]);
    gtk_init(nullptr, nullptr);
    cardio::dispatcher_group_glib group;
    cardio::dispatcher_host_glib dispatcher(group);
    cardio::set_current_dispatcher(&dispatcher);
    app.quit = [&] { group.shutdown(); };
    app.owner = spawn::create([&](spawn::event event, const spawn::report &result) { notify(&app, event, result); });
    app.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(app.window), "boxrig process startup");
    gtk_window_set_default_size(GTK_WINDOW(app.window), 640, 180);
    auto box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_container_set_border_width(GTK_CONTAINER(box), 16);
    gtk_container_add(GTK_CONTAINER(app.window), box);
    app.label = gtk_label_new("Ready");
    app.start_button = gtk_button_new_with_label("Start");
    app.stop_button = gtk_button_new_with_label("Cancel");
    gtk_box_pack_start(GTK_BOX(box), app.label, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), app.start_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), app.stop_button, FALSE, FALSE, 0);
    g_signal_connect(app.start_button, "clicked", G_CALLBACK(+[](GtkButton *, gpointer context) { begin(static_cast<application *>(context)); }), &app);
    g_signal_connect(app.stop_button, "clicked", G_CALLBACK(+[](GtkButton *, gpointer context) { stop(static_cast<application *>(context)); }), &app);
    g_signal_connect(app.window, "delete-event", G_CALLBACK(+[](GtkWidget *, GdkEvent *, gpointer context) {
        close_window(static_cast<application *>(context)); return TRUE;
    }), &app);
    gtk_widget_show_all(app.window);
    status(&app, "Ready");
    if (app.once) begin(&app);
    dispatcher.park();
    return 0;
}
#endif
