/* boxrig - Under MIT. */
#include "uri_launcher.h"
#include "cardio.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using context_owner = std::unique_ptr<GMainContext, decltype(&g_main_context_unref)>;
using process_owner = std::shared_ptr<GSubprocess>;

struct launch_request {
    cardio::cancellation_source cancellation;
    cardio::promise<void> completion;
};

struct boxrig_term_uri_launcher {
    context_owner context{g_main_context_new(), g_main_context_unref};
    std::unique_ptr<cardio::dispatcher_group_glib> group;
    std::unique_ptr<cardio::dispatcher_host_glib_auto> dispatcher;
    std::vector<std::unique_ptr<launch_request>> requests;
    std::vector<GPollFD> glib_descriptors;
    std::vector<pollfd> descriptors;
    FILE *diagnostics = nullptr;
    int priority = 0;
    bool prepared = false;
    bool closing = false;
};

static cardio::dispatcher *current_dispatcher()
{
    try {
        return &cardio::get_current_dispatcher();
    } catch (const std::runtime_error &) {
        // The checked public getter reports an uninstalled dispatcher.
        return nullptr;
    }
}

struct launch_scope {
    cardio::dispatcher *previous;
    GMainContext *context;

    explicit launch_scope(boxrig_term_uri_launcher *launcher)
        : previous(current_dispatcher()), context(launcher == nullptr ? nullptr : launcher->context.get())
    {
        cardio::set_current_dispatcher(launcher == nullptr ? nullptr : launcher->dispatcher.get());
        if (context != nullptr) g_main_context_push_thread_default(context);
    }

    ~launch_scope()
    {
        if (context != nullptr) g_main_context_pop_thread_default(context);
        cardio::set_current_dispatcher(previous);
    }
};

static void report_failure(boxrig_term_uri_launcher *launcher, const char *message)
{
    if (!launcher->closing)
        fprintf(launcher->diagnostics, "boxrig_term: could not open URL: %s.\n", message);
}

static void collect_completed(boxrig_term_uri_launcher *launcher)
{
    std::erase_if(launcher->requests, [](const auto &request) {
        return request->completion.is_ready();
    });
}

static cardio::promise<void> observe_process(
    boxrig_term_uri_launcher *launcher, process_owner process, cardio::cancellation cancellation)
{
    try {
        co_await cardio::gio::submit<gboolean>(
            [process](GCancellable *cancel, GAsyncReadyCallback callback, gpointer data) {
                g_subprocess_wait_async(process.get(), cancel, callback, data);
            },
            [](GObject *object, GAsyncResult *result, GError **error) {
                return g_subprocess_wait_finish(G_SUBPROCESS(object), result, error);
            },
            cancellation);
        if (!g_subprocess_get_successful(process.get()))
            report_failure(launcher, "xdg-open exited unsuccessfully");
    } catch (const cardio::canceled_exception &) {
        // Closing a view cancels its notification, not the external browser.
    } catch (const std::exception &error) {
        report_failure(launcher, error.what());
    }
}

int boxrig_term_uri_launcher_create(FILE *diagnostics, boxrig_term_uri_launcher **launcher_out)
{
    if (launcher_out == nullptr) { errno = EINVAL; return -1; }
    *launcher_out = nullptr;
    if (diagnostics == nullptr) { errno = EINVAL; return -1; }
    try {
        launch_scope scope(nullptr);
        auto launcher = std::make_unique<boxrig_term_uri_launcher>();
        launcher->diagnostics = diagnostics;
        launcher->group = std::make_unique<cardio::dispatcher_group_glib>(launcher->context.get());
        launcher->dispatcher = std::make_unique<cardio::dispatcher_host_glib_auto>(*launcher->group);
        *launcher_out = launcher.release();
        return 0;
    } catch (const std::bad_alloc &) {
        errno = ENOMEM;
    } catch (const std::exception &) {
        errno = EIO;
    }
    return -1;
}

int boxrig_term_uri_launcher_open(boxrig_term_uri_launcher *launcher, const char *uri, size_t length)
{
    if (launcher == nullptr || uri == nullptr || length == 0 ||
        length > 8192 || std::strlen(uri) != length) {
        errno = EINVAL;
        return -1;
    }
    if (launcher->closing) { errno = EBUSY; return -1; }
    try {
        launch_scope scope(launcher);
        collect_completed(launcher);
        if (launcher->requests.size() >= 16) {
            report_failure(launcher, "too many pending launch requests");
            errno = EBUSY;
            return -1;
        }
        auto request = std::make_unique<launch_request>();
        // Allocate retention before starting a process so allocation failure
        // cannot leave an unobserved request attributed to this gesture.
        launcher->requests.reserve(launcher->requests.size() + 1);
        const char *arguments[] = {"xdg-open", uri, nullptr};
        GError *error = nullptr;
        auto process = process_owner(g_subprocess_newv(arguments, G_SUBPROCESS_FLAGS_NONE, &error),
            [](GSubprocess *value) { if (value != nullptr) g_object_unref(value); });
        if (process == nullptr) {
            report_failure(launcher, error == nullptr ? "xdg-open could not start" : error->message);
            if (error != nullptr) g_error_free(error);
            errno = ENOENT;
            return -1;
        }
        request->completion = observe_process(launcher, std::move(process), request->cancellation.get_cancellation());
        launcher->requests.push_back(std::move(request));
        return 0;
    } catch (const std::bad_alloc &) {
        report_failure(launcher, "not enough memory");
        errno = ENOMEM;
    } catch (const std::exception &error) {
        report_failure(launcher, error.what());
        errno = EIO;
    }
    return -1;
}

size_t boxrig_term_uri_launcher_pending(const boxrig_term_uri_launcher *launcher)
{
    if (launcher == nullptr) return 0;
    return static_cast<size_t>(std::count_if(launcher->requests.begin(), launcher->requests.end(),
        [](const auto &request) { return !request->completion.is_ready(); }));
}

static void release_preparation(boxrig_term_uri_launcher *launcher)
{
    if (launcher->prepared) {
        g_main_context_release(launcher->context.get());
        launcher->prepared = false;
    }
}

int boxrig_term_uri_launcher_prepare_poll(boxrig_term_uri_launcher *launcher,
    const struct pollfd **descriptors_out, size_t *count_out, int *timeout_inout)
{
    if (launcher == nullptr || descriptors_out == nullptr || count_out == nullptr || timeout_inout == nullptr) {
        errno = EINVAL;
        return -1;
    }
    release_preparation(launcher);
    try {
        launch_scope scope(launcher);
        if (!g_main_context_acquire(launcher->context.get())) { errno = EBUSY; return -1; }
        launcher->prepared = true;
        bool ready = g_main_context_prepare(launcher->context.get(), &launcher->priority);
        int timeout = -1;
        int count;
        for (;;) {
            count = g_main_context_query(launcher->context.get(), launcher->priority,
                &timeout, launcher->glib_descriptors.data(), static_cast<int>(launcher->glib_descriptors.size()));
            if (count < 0) throw std::runtime_error("invalid context descriptor count");
            if (static_cast<size_t>(count) <= launcher->glib_descriptors.size()) break;
            launcher->glib_descriptors.resize(static_cast<size_t>(count));
        }
        launcher->descriptors.resize(static_cast<size_t>(count));
        for (int index = 0; index < count; index++) {
            const auto &source = launcher->glib_descriptors[static_cast<size_t>(index)];
            launcher->descriptors[static_cast<size_t>(index)] = {source.fd, static_cast<short>(source.events), 0};
        }
        if (ready) timeout = 0;
        if (timeout >= 0 && (*timeout_inout < 0 || timeout < *timeout_inout)) *timeout_inout = timeout;
        *descriptors_out = launcher->descriptors.data();
        *count_out = launcher->descriptors.size();
        return 0;
    } catch (const std::bad_alloc &) {
        errno = ENOMEM;
    } catch (const std::exception &) {
        errno = EIO;
    }
    release_preparation(launcher);
    return -1;
}

int boxrig_term_uri_launcher_process_poll(boxrig_term_uri_launcher *launcher,
    const struct pollfd *descriptors, size_t count)
{
    if (launcher == nullptr || !launcher->prepared || count != launcher->descriptors.size() ||
        (count != 0 && descriptors == nullptr)) {
        errno = EINVAL;
        return -1;
    }
    try {
        launch_scope scope(launcher);
        for (size_t index = 0; index < count; index++) {
            if (descriptors[index].fd != launcher->descriptors[index].fd ||
                descriptors[index].events != launcher->descriptors[index].events)
                throw std::invalid_argument("poll descriptors changed after preparation");
            launcher->glib_descriptors[index].revents = static_cast<gushort>(descriptors[index].revents);
        }
        if (g_main_context_check(launcher->context.get(), launcher->priority,
                launcher->glib_descriptors.data(), static_cast<int>(count)))
            g_main_context_dispatch(launcher->context.get());
        collect_completed(launcher);
        release_preparation(launcher);
        return 0;
    } catch (const std::exception &) {
        errno = EINVAL;
    }
    release_preparation(launcher);
    return -1;
}

void boxrig_term_uri_launcher_destroy(boxrig_term_uri_launcher *launcher)
{
    if (launcher == nullptr) return;
    release_preparation(launcher);
    {
        launch_scope scope(launcher);
        launcher->closing = true;
        for (const auto &request : launcher->requests) request->cancellation.cancel();
        // Cancellation completes through GIO's private-context callbacks.
        // The process itself is neither waited for nor terminated here.
        while (boxrig_term_uri_launcher_pending(launcher) != 0)
            g_main_context_iteration(launcher->context.get(), TRUE);
        collect_completed(launcher);
        launcher->group->shutdown();
    }
    delete launcher;
}

#include "uri_launcher_glib.h"

struct launcher_source_state {
    boxrig_term_uri_launcher *launcher;
    std::vector<pollfd> descriptors;
    std::vector<gpointer> tags;
    gint64 due = -1;
    bool failed = false;
};

struct launcher_source {
    GSource source;
    launcher_source_state *state;
};

static gboolean launcher_source_prepare(GSource *base, gint *timeout)
{
    auto &state = *reinterpret_cast<launcher_source *>(base)->state;
    *timeout = -1;
    try {
        const pollfd *descriptors;
        size_t count;
        if (boxrig_term_uri_launcher_prepare_poll(state.launcher, &descriptors, &count, timeout) != 0)
            throw std::runtime_error("could not prepare URI completions");
        bool changed = count != state.descriptors.size();
        for (size_t index = 0; !changed && index < count; index++)
            changed = descriptors[index].fd != state.descriptors[index].fd ||
                descriptors[index].events != state.descriptors[index].events;
        if (changed) {
            // Changing an unchanged poll set can restart the enclosing loop
            // indefinitely. Allocate replacements before removing old tags.
            std::vector<pollfd> next(descriptors, descriptors + count);
            std::vector<gpointer> tags(count);
            for (auto tag : state.tags) g_source_remove_unix_fd(base, tag);
            for (size_t index = 0; index < count; index++)
                tags[index] = g_source_add_unix_fd(base, descriptors[index].fd,
                    static_cast<GIOCondition>(descriptors[index].events));
            state.descriptors.swap(next);
            state.tags.swap(tags);
        }
        for (auto &descriptor : state.descriptors) descriptor.revents = 0;
        state.due = *timeout < 0 ? -1 : g_get_monotonic_time() + static_cast<gint64>(*timeout) * 1000;
        return *timeout == 0;
    } catch (const std::exception &error) {
        report_failure(state.launcher, error.what());
        state.failed = true;
        return TRUE;
    }
}

static gboolean launcher_source_check(GSource *base)
{
    auto &state = *reinterpret_cast<launcher_source *>(base)->state;
    if (state.failed || (state.due >= 0 && g_get_monotonic_time() >= state.due)) return TRUE;
    for (auto tag : state.tags)
        if (g_source_query_unix_fd(base, tag) != 0) return TRUE;
    return FALSE;
}

static gboolean launcher_source_dispatch(GSource *base, GSourceFunc callback, gpointer context)
{
    auto &state = *reinterpret_cast<launcher_source *>(base)->state;
    g_source_set_ready_time(base, -1);
    if (state.failed) return G_SOURCE_REMOVE;
    for (size_t index = 0; index < state.descriptors.size(); index++)
        state.descriptors[index].revents = static_cast<short>(g_source_query_unix_fd(base, state.tags[index]));
    if (boxrig_term_uri_launcher_process_poll(state.launcher, state.descriptors.data(), state.descriptors.size()) != 0) {
        report_failure(state.launcher, "could not dispatch URI completions");
        return G_SOURCE_REMOVE;
    }
    return callback == nullptr ? G_SOURCE_CONTINUE : callback(context);
}

static void launcher_source_finalize(GSource *base)
{
    delete reinterpret_cast<launcher_source *>(base)->state;
}

GSource *boxrig_term_uri_launcher_source_new(boxrig_term_uri_launcher *launcher)
{
    if (launcher == nullptr) { errno = EINVAL; return nullptr; }
    static GSourceFuncs functions = {
        launcher_source_prepare, launcher_source_check, launcher_source_dispatch,
        launcher_source_finalize, nullptr, nullptr};
    try {
        auto state = std::make_unique<launcher_source_state>();
        state->launcher = launcher;
        auto *base = g_source_new(&functions, sizeof(launcher_source));
        reinterpret_cast<launcher_source *>(base)->state = state.release();
        g_source_set_name(base, "boxrig URL requests");
        g_source_set_priority(base, G_PRIORITY_DEFAULT_IDLE);
        return base;
    } catch (const std::bad_alloc &) {
        errno = ENOMEM;
        return nullptr;
    }
}
