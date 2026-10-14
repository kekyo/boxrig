/* boxrig - Under MIT. */
#define ATSPI_DISABLE_DEPRECATED
#include <atspi/atspi.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    exit(1); } } while (0)

typedef struct text_event {
    char kind;
    int offset, length;
    char *text;
} text_event;

static AtspiAccessible *terminal;
static text_event events[128];
static size_t event_count;
static bool acknowledged, ready;

static AtspiAccessible *find_terminal(AtspiAccessible *object)
{
    GError *error = NULL;
    if (atspi_accessible_get_role(object, &error) == ATSPI_ROLE_TERMINAL) return g_object_ref(object);
    if (error != NULL) { g_clear_error(&error); return NULL; }
    int count = atspi_accessible_get_child_count(object, &error);
    g_clear_error(&error);
    for (int i = 0; i < count; i++) {
        AtspiAccessible *child = atspi_accessible_get_child_at_index(object, i, &error);
        g_clear_error(&error);
        if (child == NULL) continue;
        AtspiAccessible *found = find_terminal(child);
        g_object_unref(child);
        if (found != NULL) return found;
    }
    return NULL;
}

static void event_received(AtspiEvent *event, void *context)
{
    (void)context;
    if (terminal == NULL) terminal = find_terminal(event->source);
    if (event->source == terminal && g_str_has_prefix(event->type, "object:text")) {
        CHECK(event_count < G_N_ELEMENTS(events));
        text_event *out = &events[event_count++];
        out->kind = g_str_has_prefix(event->type, "object:text-changed:insert") ? 'I' :
            g_str_has_prefix(event->type, "object:text-changed:delete") ? 'D' :
            g_str_has_prefix(event->type, "object:text-caret-moved") ? 'C' :
            g_str_has_prefix(event->type, "object:text-selection-changed") ? 'S' : 'A';
        out->offset = event->detail1;
        out->length = event->detail2;
        out->text = g_strdup(G_VALUE_HOLDS_STRING(&event->any_data) ? g_value_get_string(&event->any_data) : "");
        printf("EVENT %c %d %d %s\n", out->kind, out->offset, out->length, out->text);
    }
    g_boxed_free(ATSPI_TYPE_EVENT, event);
}

static gboolean driver_output(GIOChannel *channel, GIOCondition condition, gpointer context)
{
    (void)context;
    CHECK(!(condition & G_IO_HUP));
    char *line = NULL;
    GIOStatus status = g_io_channel_read_line(channel, &line, NULL, NULL, NULL);
    CHECK(status == G_IO_STATUS_NORMAL || status == G_IO_STATUS_AGAIN);
    if (line != NULL) {
        if (strcmp(line, "READY\n") == 0) ready = true;
        else if (strcmp(line, "ACK\n") == 0) acknowledged = true;
        else fprintf(stderr, "DRIVER %s", line);
    }
    g_free(line);
    return G_SOURCE_CONTINUE;
}

static gboolean timeout(gpointer context)
{
    (void)context;
    fputs("FAIL AT-SPI test did not reach its event barrier\n", stderr);
    exit(1);
}

static void clear_events(void)
{
    while (g_main_context_pending(NULL)) g_main_context_iteration(NULL, FALSE);
    for (size_t i = 0; i < event_count; i++) g_free(events[i].text);
    event_count = 0;
}

static void command(FILE *input, AtspiText *text, const char *command, size_t expected_events)
{
    clear_events();
    acknowledged = false;
    CHECK(fprintf(input, "%s\n", command) > 0 && fflush(input) == 0);
    while (!acknowledged || event_count < expected_events) g_main_context_iteration(NULL, TRUE);
    /* The same application's D-Bus reply follows its prior text signals.
     * Drain those callbacks before asserting a no-change frame, without a
     * sleep whose duration would depend on machine or emulator speed. */
    GError *error = NULL;
    char *barrier = atspi_text_get_text(text, 0, -1, &error);
    CHECK(error == NULL && barrier != NULL);
    g_free(barrier);
    while (g_main_context_pending(NULL)) g_main_context_iteration(NULL, FALSE);
}

static void feed(FILE *input, AtspiText *text, const char *bytes, size_t expected_events)
{
    char *encoded = g_base64_encode((const guchar *)bytes, strlen(bytes));
    char *line = g_strconcat("F ", encoded, NULL);
    command(input, text, line, expected_events);
    g_free(line);
    g_free(encoded);
}

static void snapshot(AtspiText *text, const char *expected, int caret)
{
    GError *error = NULL;
    char *actual = atspi_text_get_text(text, 0, -1, &error);
    CHECK(error == NULL && actual != NULL);
    if (strcmp(actual, expected) != 0) fprintf(stderr, "expected [%s], got [%s]\n", expected, actual);
    CHECK(strcmp(actual, expected) == 0);
    CHECK(atspi_text_get_character_count(text, &error) == g_utf8_strlen(expected, -1) && error == NULL);
    CHECK(atspi_text_get_caret_offset(text, &error) == caret && error == NULL);
    g_free(actual);
}

int main(int argc, char **argv)
{
    CHECK(argc == 2 || (argc == 3 && strcmp(argv[2], "--speech") == 0));
    CHECK(atspi_init() == 0);
    guint watchdog = g_timeout_add_seconds(180, timeout, NULL);
    GError *error = NULL;
    AtspiEventListener *listener = atspi_event_listener_new(event_received, NULL, NULL);
    CHECK(atspi_event_listener_register(listener, "object", &error) && error == NULL);
    int to_child[2], from_child[2];
    CHECK(pipe(to_child) == 0 && pipe(from_child) == 0);
    pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        if (dup2(to_child[0], 0) < 0 || dup2(from_child[1], 1) < 0) _exit(126);
        close(to_child[0]); close(to_child[1]); close(from_child[0]); close(from_child[1]);
        execl(argv[1], argv[1], (char *)NULL);
        _exit(127);
    }
    close(to_child[0]); close(from_child[1]);
    FILE *input = fdopen(to_child[1], "w");
    CHECK(input != NULL);
    GIOChannel *output = g_io_channel_unix_new(from_child[0]);
    g_io_channel_set_close_on_unref(output, TRUE);
    CHECK(g_io_channel_set_flags(output, G_IO_FLAG_NONBLOCK, &error) == G_IO_STATUS_NORMAL && error == NULL);
    guint output_source = g_io_add_watch(output, G_IO_IN | G_IO_HUP, driver_output, NULL);
    while (!ready || terminal == NULL) g_main_context_iteration(NULL, TRUE);
    AtspiText *text = atspi_accessible_get_text_iface(terminal);
    CHECK(text != NULL);
    snapshot(text, "\n\n\n\n", 0);
    if (argc == 3) {
        /* The isolated Orca launcher acknowledges focus processing before
         * output, then acknowledges its speech log before closing the app. */
        char barrier[16];
        CHECK(fgets(barrier, sizeof(barrier), stdin) != NULL);
        feed(input, text, "Hello boxrig.", 2);
        snapshot(text, "Hello boxrig.\n\n\n\n", 13);
        CHECK(fgets(barrier, sizeof(barrier), stdin) != NULL);
    } else {
    feed(input, text, "A日本é🙂\r\nline two", 3);
    snapshot(text, "A日本é🙂\nline two\n\n\n", 15);
    CHECK(event_count == 3 && events[0].kind == 'D' && events[1].kind == 'I' && events[2].kind == 'C');
    CHECK(events[0].offset == 0 && events[0].length == 1 && strcmp(events[0].text, "\n") == 0);
    CHECK(events[1].offset == 0 && events[1].length == 15 && strcmp(events[1].text, "A日本é🙂\nline two") == 0);
    CHECK(events[2].offset == 15);
    AtspiTextRange *word = atspi_text_get_string_at_offset(text, 9, ATSPI_TEXT_GRANULARITY_WORD, &error);
    CHECK(error == NULL && word != NULL && strcmp(word->content, "line ") == 0);
    CHECK(word->start_offset == 7 && word->end_offset == 12);
    g_boxed_free(ATSPI_TYPE_TEXT_RANGE, word);
    command(input, text, "S", 1);
    CHECK(event_count == 1 && events[0].kind == 'S');
    CHECK(atspi_text_get_n_selections(text, &error) == 1 && error == NULL);
    AtspiRange *range = atspi_text_get_selection(text, 0, &error);
    CHECK(error == NULL && range != NULL && range->start_offset == 1 && range->end_offset == 5);
    g_boxed_free(ATSPI_TYPE_RANGE, range);
    command(input, text, "U", 1);
    CHECK(atspi_text_get_n_selections(text, &error) == 0 && error == NULL);
    feed(input, text, "\rX\033[K", 3);
    snapshot(text, "A日本é🙂\nX\n\n\n", 8);
    CHECK(event_count == 3 && events[0].kind == 'D' && events[1].kind == 'I' && events[2].kind == 'C');
    CHECK(events[0].offset == 7 && events[0].length == 8 && strcmp(events[0].text, "line two") == 0);
    CHECK(events[1].offset == 7 && events[1].length == 1 && strcmp(events[1].text, "X") == 0);
    command(input, text, "R", 0);
    CHECK(event_count == 0);
    feed(input, text, "\033[2J\033[H0123456789abcdefghijK\r\nlast", 3);
    snapshot(text, "0123456789abcdefghijK\nlast\n\n", 26);
    command(input, text, "Z", 1);
    snapshot(text, "0123456789abcdefghijK\nlast\n", 26);
    feed(input, text, "\033[2J\033[Hrow1\r\nrow2\r\nrow3\r\nrow4\r\nrow5", 3);
    snapshot(text, "row2\nrow3\nrow4\nrow5\n", 19);
    command(input, text, "T", 3);
    snapshot(text, "row1\nrow2\nrow3\nrow4\n", 20);
    feed(input, text, "\033[?1049h\033[Halt日本", 3);
    snapshot(text, "alt日本\n\n\n\n", 5);
    feed(input, text, "\033[?1049l", 3);
    snapshot(text, "row1\nrow2\nrow3\nrow4\n", 20);
    }
    g_source_remove(output_source);
    CHECK(fputs("Q\n", input) >= 0 && fclose(input) == 0);
    int status;
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    clear_events();
    CHECK(atspi_event_listener_deregister(listener, "object", &error) && error == NULL);
    g_object_unref(listener);
    g_object_unref(text);
    g_object_unref(terminal);
    g_io_channel_unref(output);
    g_source_remove(watchdog);
    atspi_exit();
    puts(argc == 3 ? "PASS AT-SPI terminal output for Orca" :
        "PASS AT-SPI Unicode text, caret, selection, ordered events, history and reflow");
    return 0;
}
