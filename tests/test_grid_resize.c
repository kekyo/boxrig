/* boxrig - Under MIT. */
#include "boxrig/terminal.h"
#include "boxrig/ui/view.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); exit(1); \
} } while (0)

static void test_requests(void)
{
    const struct {
        const char *sequence;
        boxrig_grid_size_mode rows_mode, columns_mode;
        uint32_t rows, columns;
    } cases[] = {
        {"\033[8;24;80t", BOXRIG_GRID_SIZE_EXPLICIT, BOXRIG_GRID_SIZE_EXPLICIT, 24, 80},
        {"\033[8;;132t", BOXRIG_GRID_SIZE_KEEP, BOXRIG_GRID_SIZE_EXPLICIT, 0, 132},
        {"\033[8;40t", BOXRIG_GRID_SIZE_EXPLICIT, BOXRIG_GRID_SIZE_KEEP, 40, 0},
        {"\033[8;0;0t", BOXRIG_GRID_SIZE_DISPLAY, BOXRIG_GRID_SIZE_DISPLAY, 0, 0},
        {"\033[8t", BOXRIG_GRID_SIZE_KEEP, BOXRIG_GRID_SIZE_KEEP, 0, 0},
        {"\033[8;t", BOXRIG_GRID_SIZE_KEEP, BOXRIG_GRID_SIZE_KEEP, 0, 0},
        {"\033[8;;t", BOXRIG_GRID_SIZE_KEEP, BOXRIG_GRID_SIZE_KEEP, 0, 0},
        {"\033[8;00;001t", BOXRIG_GRID_SIZE_DISPLAY, BOXRIG_GRID_SIZE_EXPLICIT, 0, 1},
        {"\033[8;;0t", BOXRIG_GRID_SIZE_KEEP, BOXRIG_GRID_SIZE_DISPLAY, 0, 0},
        {"\033[8;4294967295;1t", BOXRIG_GRID_SIZE_EXPLICIT, BOXRIG_GRID_SIZE_EXPLICIT, UINT32_MAX, 1},
        {"\033[8;1;4294967295t", BOXRIG_GRID_SIZE_EXPLICIT, BOXRIG_GRID_SIZE_EXPLICIT, 1, UINT32_MAX}
    };
    for (size_t index = 0; index < sizeof(cases) / sizeof(cases[0]); index++) {
        size_t length = strlen(cases[index].sequence);
        for (size_t split = 0; split <= length; split++) {
            boxrig_terminal_config config = boxrig_terminal_config_default();
            config.rows = 2; config.columns = 5;
            boxrig_terminal *terminal = NULL;
            CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
            size_t consumed, rows, columns;
            CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)cases[index].sequence,
                split, &consumed) == BOXRIG_STATUS_OK && consumed == split);
            boxrig_host_item item;
            if (split != length) CHECK(!boxrig_terminal_host_item_peek(terminal, &item));
            CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)cases[index].sequence + split,
                length - split, &consumed) == BOXRIG_STATUS_OK && consumed == length - split);
            CHECK(boxrig_terminal_host_item_peek(terminal, &item));
            CHECK(item.type == BOXRIG_HOST_ITEM_GRID_RESIZE && item.request_id == 0 && item.flags == 0);
            CHECK(item.payload == NULL && item.payload_length == 0 && item.notification == NULL && item.graphics_transport == NULL);
            CHECK(item.grid_resize.rows.mode == cases[index].rows_mode && item.grid_resize.rows.cells == cases[index].rows);
            CHECK(item.grid_resize.columns.mode == cases[index].columns_mode && item.grid_resize.columns.cells == cases[index].columns);
            CHECK(boxrig_terminal_host_item_consume(terminal) == BOXRIG_STATUS_OK);
            CHECK(!boxrig_terminal_host_item_peek(terminal, &item));
            boxrig_terminal_size(terminal, &rows, &columns);
            CHECK(rows == 2 && columns == 5);
            const uint8_t *output;
            CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);
            boxrig_terminal_destroy(terminal);
        }
    }
    const char *invalid[] = {"\033[8;4294967296;1t", "\033[8;1;4294967296t",
        "\033[8:1;2t", "\033[8;1:2t", "\033[8;1;2;3t", "\033[?8;1;2t", "\033[8;1;2$t",
        "\033[4;1;2t", "\033[8;1;2\030t", "\033[8;1;2\032t"};
    for (size_t index = 0; index < sizeof(invalid) / sizeof(invalid[0]); index++) {
        boxrig_terminal *terminal = NULL;
        boxrig_terminal_config config = boxrig_terminal_config_default();
        CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
        size_t consumed;
        CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)invalid[index], strlen(invalid[index]), &consumed) == BOXRIG_STATUS_OK);
        boxrig_host_item item;
        CHECK(!boxrig_terminal_host_item_peek(terminal, &item));
        CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)"\033[8;;t", 6, &consumed) == BOXRIG_STATUS_OK);
        CHECK(boxrig_terminal_host_item_peek(terminal, &item) && item.type == BOXRIG_HOST_ITEM_GRID_RESIZE);
        boxrig_terminal_destroy(terminal);
    }
    puts("PASS typed grid requests, omitted/zero axes, split input and rejected syntax");
}

static void test_capacity(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    config.host_item_capacity = 1;
    boxrig_terminal *terminal = NULL;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    const char sequence[] = "\a\033[8;3;7t\033]2;after\a";
    size_t consumed;
    CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)sequence, sizeof(sequence) - 1, &consumed) == BOXRIG_STATUS_WOULD_BLOCK);
    CHECK(consumed == 8 && sequence[consumed] == 't');
    boxrig_host_item item;
    CHECK(boxrig_terminal_host_item_peek(terminal, &item) && item.type == BOXRIG_HOST_ITEM_BELL);
    size_t repeated;
    CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)sequence + consumed,
        sizeof(sequence) - 1 - consumed, &repeated) == BOXRIG_STATUS_WOULD_BLOCK && repeated == 0);
    CHECK(boxrig_terminal_host_item_consume(terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)sequence + consumed,
        sizeof(sequence) - 1 - consumed, &repeated) == BOXRIG_STATUS_WOULD_BLOCK);
    consumed += repeated;
    CHECK(boxrig_terminal_host_item_peek(terminal, &item) && item.type == BOXRIG_HOST_ITEM_GRID_RESIZE);
    CHECK(item.grid_resize.rows.cells == 3 && item.grid_resize.columns.cells == 7);
    boxrig_grid_size_request retained = item.grid_resize;
    CHECK(boxrig_terminal_host_item_consume(terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)sequence + consumed,
        sizeof(sequence) - 1 - consumed, &repeated) == BOXRIG_STATUS_OK);
    CHECK(consumed + repeated == sizeof(sequence) - 1);
    CHECK(boxrig_terminal_host_item_peek(terminal, &item) && item.type == BOXRIG_HOST_ITEM_TITLE);
    CHECK(item.payload_length == 5 && memcmp(item.payload, "after", 5) == 0);
    CHECK(retained.rows.mode == BOXRIG_GRID_SIZE_EXPLICIT && retained.rows.cells == 3);
    CHECK(retained.columns.mode == BOXRIG_GRID_SIZE_EXPLICIT && retained.columns.cells == 7);
    CHECK(item.grid_resize.rows.mode == BOXRIG_GRID_SIZE_KEEP && item.grid_resize.rows.cells == 0);
    CHECK(item.grid_resize.columns.mode == BOXRIG_GRID_SIZE_KEEP && item.grid_resize.columns.cells == 0);
    CHECK(boxrig_terminal_host_item_consume(terminal) == BOXRIG_STATUS_OK);
    CHECK(!boxrig_terminal_host_item_peek(terminal, &item));
    boxrig_terminal_destroy(terminal);
    puts("PASS bounded host queue retries without duplicate or reordered grid requests");
}

static size_t resolve_axis(boxrig_grid_size_axis axis, size_t current, size_t display)
{
    return axis.mode == BOXRIG_GRID_SIZE_KEEP ? current :
        axis.mode == BOXRIG_GRID_SIZE_DISPLAY ? display : axis.cells;
}

static void test_host_layout(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    config.rows = 2; config.columns = 5;
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *view = NULL;
    boxrig_ui_host_item_consumer *consumer = NULL;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    CHECK(boxrig_ui_view_create(model, &view_config, &view) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_acquire_layout_authority(view) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_host_item_consumer_create(model, &consumer) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_set_geometry_capabilities(view, BOXRIG_UI_GEOMETRY_SIZE_COMMAND) == BOXRIG_UI_OK);
    boxrig_ui_allocation_input allocation = {.allocation_width = 50 * BOXRIG_UI_FIXED_ONE,
        .allocation_height = 40 * BOXRIG_UI_FIXED_ONE, .cell_width = 10 * BOXRIG_UI_FIXED_ONE,
        .cell_height = 20 * BOXRIG_UI_FIXED_ONE, .cell_metrics_revision = 1,
        .window_state = BOXRIG_UI_WINDOW_NORMAL};
    boxrig_ui_layout_result layout;
    CHECK(boxrig_ui_view_accept_allocation(view, &allocation, &layout) == BOXRIG_UI_OK);
    boxrig_ui_layout_action action;
    CHECK(!boxrig_ui_view_take_layout_action(view, &action));
    const char *requests[] = {"\033[8;3;0t", "\033[8;4;9t", "\033[8;;2t"};
    for (unsigned int attempt = 0; attempt < 3; attempt++) {
        size_t consumed;
        CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)requests[attempt], strlen(requests[attempt]), &consumed) == BOXRIG_STATUS_OK);
        boxrig_host_item item;
        CHECK(boxrig_ui_host_item_consumer_peek(consumer, &item) && item.type == BOXRIG_HOST_ITEM_GRID_RESIZE);
        size_t rows, columns;
        boxrig_terminal_size(terminal, &rows, &columns);
        size_t target_rows = resolve_axis(item.grid_resize.rows, rows, 6);
        size_t target_columns = resolve_axis(item.grid_resize.columns, columns, 7);
        CHECK(boxrig_ui_host_item_consumer_consume(consumer) == BOXRIG_UI_OK);
        /* The host may reject the untrusted request before layout as well as
         * report a rejected or constrained native content-size command. */
        if (attempt == 2) {
            CHECK(!boxrig_ui_view_take_layout_action(view, &action));
            continue;
        }
        CHECK(boxrig_ui_view_set_preferred_grid(view, target_rows, target_columns) == BOXRIG_UI_OK);
        CHECK(boxrig_ui_view_take_layout_action(view, &action) && action.kind == BOXRIG_UI_LAYOUT_ACTION_CONTENT_SIZE);
        CHECK(action.value.content_size.width == (boxrig_ui_fixed)target_columns * allocation.cell_width);
        CHECK(action.value.content_size.height == (boxrig_ui_fixed)target_rows * allocation.cell_height);
        uint64_t revision = action.revision;
        bool applied;
        CHECK(boxrig_ui_view_complete_size_command(view, revision,
            attempt == 0 ? BOXRIG_UI_SIZE_ACCEPTED : BOXRIG_UI_SIZE_REJECTED,
            attempt == 0 ? action.value.content_size.width : 70 * BOXRIG_UI_FIXED_ONE,
            attempt == 0 ? action.value.content_size.height : 60 * BOXRIG_UI_FIXED_ONE,
            &applied, &layout) == BOXRIG_UI_OK && applied);
        CHECK(boxrig_ui_view_take_layout_action(view, &action) && action.kind == BOXRIG_UI_LAYOUT_ACTION_GRID_RESIZE);
        CHECK(action.value.grid_resize.rows == 3 && action.value.grid_resize.columns == 7);
        CHECK(boxrig_terminal_resize(terminal, action.value.grid_resize.rows, action.value.grid_resize.columns) == BOXRIG_STATUS_OK);
        CHECK(boxrig_ui_view_complete_size_command(view, revision, BOXRIG_UI_SIZE_ACCEPTED, 0, 0, &applied, &layout) == BOXRIG_UI_OK && !applied);
        boxrig_terminal_size(terminal, &rows, &columns);
        CHECK(rows == 3 && columns == 7);
    }
    const uint8_t *output;
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);
    boxrig_ui_host_item_consumer_destroy(consumer);
    boxrig_ui_view_destroy(view);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    puts("PASS host authorization and existing layout revisions control the actual grid");
}

int main(void)
{
    test_requests();
    test_capacity();
    test_host_layout();
    return 0;
}
