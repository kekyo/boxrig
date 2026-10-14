/* boxrig - Under MIT. */
#include "boxrig/ui.h"
#include "../src/ui/candidates_private.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(value) do { if (!(value)) { \
    fprintf(stderr, "FAIL candidates %s:%d: %s\n", __FILE__, __LINE__, #value); \
    abort(); } } while (0)

static void owned_state(void)
{
    boxrig_terminal_config tc = boxrig_terminal_config_default();
    boxrig_ui_view_config vc = boxrig_ui_view_config_default();
    boxrig_terminal *terminal;
    boxrig_ui_model *model;
    boxrig_ui_view *view, *other;
    CHECK(boxrig_terminal_create(&tc, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_create(model, &vc, &view) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_create(model, &vc, &other) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_set_focused(view, true) == BOXRIG_UI_OK);
    boxrig_ui_dirty dirty;
    if (boxrig_ui_view_peek_dirty(view, BOXRIG_UI_DIRTY_OVERLAY, &dirty))
        CHECK(boxrig_ui_view_consume_dirty(view, BOXRIG_UI_DIRTY_OVERLAY) == BOXRIG_UI_OK);
    char item[] = "汽車🙂", label[] = "s", auxiliary[] = "説明";
    const char *items[] = {item}, *labels[] = {label};
    boxrig_ui_candidate_snapshot snapshot = {.count = 1, .page_size = 1,
        .visible = true, .cursor_visible = true, .auxiliary_visible = true,
        .items = items, .labels = labels, .auxiliary = auxiliary};
    CHECK(boxrig_ui_view_set_candidates(view, &snapshot) == BOXRIG_UI_OK);
    item[0] = label[0] = auxiliary[0] = 0;
    boxrig_ui_candidate_view actual;
    CHECK(boxrig_ui_view_get_candidates(view, &actual) == BOXRIG_UI_OK);
    CHECK(actual.revision != 0 && actual.snapshot.count == 1);
    CHECK(strcmp(actual.snapshot.items[0], "汽車🙂") == 0);
    CHECK(strcmp(actual.snapshot.labels[0], "s") == 0);
    CHECK(strcmp(actual.snapshot.auxiliary, "説明") == 0);
    uint64_t revision = actual.revision;
    CHECK(boxrig_ui_view_peek_dirty(view, BOXRIG_UI_DIRTY_OVERLAY, &dirty));
    CHECK(boxrig_ui_view_consume_dirty(view, BOXRIG_UI_DIRTY_OVERLAY) == BOXRIG_UI_OK);
    snapshot.cursor = 1;
    CHECK(boxrig_ui_view_set_candidates(view, &snapshot) == BOXRIG_UI_INVALID_ARGUMENT);
    snapshot.cursor = 0;
    items[0] = "\xc0\xaf";
    CHECK(boxrig_ui_view_set_candidates(view, &snapshot) == BOXRIG_UI_INVALID_ARGUMENT);
    char *large = malloc(BOXRIG_UI_CANDIDATE_BYTES_MAXIMUM + 1);
    CHECK(large != NULL);
    memset(large, 'x', BOXRIG_UI_CANDIDATE_BYTES_MAXIMUM);
    large[BOXRIG_UI_CANDIDATE_BYTES_MAXIMUM] = 0;
    items[0] = large;
    CHECK(boxrig_ui_view_set_candidates(view, &snapshot) == BOXRIG_UI_LIMIT_EXCEEDED);
    free(large);
    CHECK(!boxrig_ui_view_peek_dirty(view, BOXRIG_UI_DIRTY_OVERLAY, &dirty));
    CHECK(boxrig_ui_view_get_candidates(view, &actual) == BOXRIG_UI_OK);
    CHECK(actual.revision == revision && strcmp(actual.snapshot.items[0], "汽車🙂") == 0);
    CHECK(boxrig_ui_view_get_candidates(other, &actual) == BOXRIG_UI_OK);
    CHECK(actual.snapshot.count == 0);
    const uint8_t *bytes;
    CHECK(boxrig_terminal_output_peek(terminal, &bytes) == 0);
    CHECK(boxrig_ui_view_set_focused(view, false) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_get_candidates(view, &actual) == BOXRIG_UI_OK);
    CHECK(actual.revision > revision && actual.snapshot.count == 0 && actual.snapshot.auxiliary == NULL);
    CHECK(boxrig_ui_view_peek_dirty(view, BOXRIG_UI_DIRTY_OVERLAY, &dirty));
    items[0] = "帰社";
    CHECK(boxrig_ui_view_set_candidates(view, &snapshot) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_set_visible(view, false) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_get_candidates(view, &actual) == BOXRIG_UI_OK);
    CHECK(actual.snapshot.count == 0);
    char above[] = "読み";
    snapshot = (boxrig_ui_candidate_snapshot){.format = BOXRIG_UI_CANDIDATE_PAGE,
        .count = 1, .page_size = 1, .items = items, .visible = true,
        .has_previous_page = true, .has_next_page = true,
        .auxiliary_above = above, .auxiliary_above_visible = true};
    CHECK(boxrig_ui_view_set_candidates(view, &snapshot) == BOXRIG_UI_OK);
    above[0] = 0;
    CHECK(boxrig_ui_view_get_candidates(view, &actual) == BOXRIG_UI_OK);
    CHECK(actual.snapshot.format == BOXRIG_UI_CANDIDATE_PAGE);
    CHECK(actual.snapshot.has_previous_page && actual.snapshot.has_next_page);
    CHECK(strcmp(actual.snapshot.auxiliary_above, "読み") == 0);
    snapshot.page_size = 2;
    CHECK(boxrig_ui_view_set_candidates(view, &snapshot) == BOXRIG_UI_INVALID_ARGUMENT);
    snapshot = (boxrig_ui_candidate_snapshot){.format = BOXRIG_UI_CANDIDATE_PAGE,
        .auxiliary_above = "候補なし", .auxiliary_above_visible = true};
    CHECK(boxrig_ui_view_set_candidates(view, &snapshot) == BOXRIG_UI_OK);
    boxrig_ui_view_clear_candidates(view);
    CHECK(boxrig_ui_view_get_candidates(view, &actual) == BOXRIG_UI_OK);
    CHECK(actual.snapshot.auxiliary_above == NULL);
    char panel[] = "1. 汽車 2号 2. 記者 🙂", preedit[] = "きしゃ🙂";
    snapshot = (boxrig_ui_candidate_snapshot){.format = BOXRIG_UI_CANDIDATE_STRING_PANEL,
        .visible = true, .panel_text = panel, .panel_preedit = preedit, .panel_cursor = 9};
    CHECK(boxrig_ui_view_set_candidates(view, &snapshot) == BOXRIG_UI_OK);
    panel[0] = preedit[0] = 0;
    CHECK(boxrig_ui_view_get_candidates(view, &actual) == BOXRIG_UI_OK);
    CHECK(actual.snapshot.count == 0 && actual.snapshot.page_size == 0);
    CHECK(strcmp(actual.snapshot.panel_text, "1. 汽車 2号 2. 記者 🙂") == 0);
    CHECK(strcmp(actual.snapshot.panel_preedit, "きしゃ🙂") == 0 && actual.snapshot.panel_cursor == 9);
    snapshot.panel_text = "候補";
    snapshot.panel_preedit = "きしゃ🙂";
    snapshot.panel_cursor = 10;
    CHECK(boxrig_ui_view_set_candidates(view, &snapshot) == BOXRIG_UI_INVALID_ARGUMENT);
    snapshot.panel_cursor = 14;
    CHECK(boxrig_ui_view_set_candidates(view, &snapshot) == BOXRIG_UI_INVALID_ARGUMENT);
    snapshot.panel_cursor = 13;
    CHECK(boxrig_ui_view_set_candidates(view, &snapshot) == BOXRIG_UI_OK);
    snapshot.panel_text = "\xed\xa0\x80";
    CHECK(boxrig_ui_view_set_candidates(view, &snapshot) == BOXRIG_UI_INVALID_ARGUMENT);
    snapshot.panel_text = "候補";
    snapshot.has_next_page = true;
    CHECK(boxrig_ui_view_set_candidates(view, &snapshot) == BOXRIG_UI_INVALID_ARGUMENT);
    snapshot.has_next_page = false;
    snapshot.count = snapshot.page_size = 1;
    snapshot.items = items;
    CHECK(boxrig_ui_view_set_candidates(view, &snapshot) == BOXRIG_UI_INVALID_ARGUMENT);
    snapshot.count = snapshot.page_size = 0;
    snapshot.items = NULL;
    large = malloc(BOXRIG_UI_CANDIDATE_BYTES_MAXIMUM);
    CHECK(large != NULL);
    memset(large, 'x', BOXRIG_UI_CANDIDATE_BYTES_MAXIMUM - 1);
    large[BOXRIG_UI_CANDIDATE_BYTES_MAXIMUM - 1] = 0;
    snapshot.panel_text = large;
    CHECK(boxrig_ui_view_set_candidates(view, &snapshot) == BOXRIG_UI_LIMIT_EXCEEDED);
    snapshot.panel_preedit = NULL;
    snapshot.panel_cursor = 0;
    CHECK(boxrig_ui_view_set_candidates(view, &snapshot) == BOXRIG_UI_OK);
    free(large);
    CHECK(boxrig_ui_view_set_focused(view, true) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_set_focused(view, false) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_get_candidates(view, &actual) == BOXRIG_UI_OK);
    CHECK(actual.snapshot.panel_text == NULL && actual.snapshot.panel_preedit == NULL);
    CHECK(boxrig_terminal_output_peek(terminal, &bytes) == 0);
    boxrig_ui_view_destroy(other);
    boxrig_ui_view_destroy(view);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
}

static void placement(void)
{
    boxrig_ui_candidate_view candidates = {.revision = 17,
        .snapshot = {.count = 25, .page_size = 16, .visible = true,
            .cursor_visible = true, .auxiliary_visible = true, .auxiliary = "説明"}};
    boxrig_ui_candidate_measurement m = {.cell_width = 9, .line_height = 21,
        .auxiliary_width = 173, .auxiliary_lines = 2};
    for (size_t i = 0; i < 16; i++) m.items[i] = i == 7 ? 20000 : 60;
    const int dimensions[] = {1, 2, 13, 25, 61, 200, 900};
    const int origins[] = {-100, 0, 70, 1000, INT_MAX};
    for (size_t wi = 0; wi < sizeof(dimensions) / sizeof(*dimensions); wi++)
    for (size_t hi = 0; hi < sizeof(dimensions) / sizeof(*dimensions); hi++)
    for (size_t ai = 0; ai < sizeof(origins) / sizeof(*origins); ai++)
    for (size_t cursor = 0; cursor < 25; cursor++) {
        m.width = dimensions[wi]; m.height = dimensions[hi];
        m.anchor = (boxrig_ui_candidate_rectangle){origins[ai], origins[ai], 9, 21};
        candidates.snapshot.cursor = cursor;
        boxrig_ui_candidate_layout result;
        CHECK(boxrig_ui_candidates_place(&candidates, &m, &result) == BOXRIG_UI_OK);
        CHECK(result.revision == candidates.revision);
        CHECK(result.bounds.x >= 0 && result.bounds.y >= 0);
        CHECK(result.bounds.width > 0 && result.bounds.height > 0);
        CHECK(result.bounds.x + result.bounds.width <= m.width);
        CHECK(result.bounds.y + result.bounds.height <= m.height);
        CHECK(result.count > 0 && result.count <= candidates.snapshot.page_size);
        CHECK(result.first <= cursor && result.first + result.count > cursor);
        CHECK(result.first / 16 == cursor / 16);
        CHECK(result.first + result.count <= candidates.snapshot.count);
        CHECK(result.auxiliary.height <= m.line_height * 2 + m.cell_width);
        for (size_t i = 0; i < result.count; i++) {
            boxrig_ui_candidate_rectangle item = result.items[i];
            if (item.width == 0 || item.height == 0) continue;
            CHECK(item.x >= 0 && item.y >= 0);
            CHECK(item.x + item.width <= m.width && item.y + item.height <= m.height);
        }
    }
    candidates.snapshot.count = 3; candidates.snapshot.cursor = 1;
    candidates.snapshot.orientation = BOXRIG_UI_CANDIDATE_HORIZONTAL;
    m.width = m.height = 500;
    m.anchor = (boxrig_ui_candidate_rectangle){495, 490, 9, 21};
    boxrig_ui_candidate_layout result;
    CHECK(boxrig_ui_candidates_place(&candidates, &m, &result) == BOXRIG_UI_OK);
    CHECK(result.horizontal && result.count == 3 && result.bounds.y < m.anchor.y);
    m.width = 100;
    CHECK(boxrig_ui_candidates_place(&candidates, &m, &result) == BOXRIG_UI_OK);
    CHECK(!result.horizontal && result.count == 3);
    candidates.snapshot.visible = false;
    CHECK(boxrig_ui_candidates_place(&candidates, &m, &result) == BOXRIG_UI_OK);
    CHECK(result.count == 0 && result.auxiliary.height > 0);
    candidates.snapshot.count = 0;
    CHECK(boxrig_ui_candidates_place(&candidates, &m, &result) == BOXRIG_UI_OK);
    CHECK(result.bounds.width == 0 && result.bounds.height == 0);

    candidates.snapshot = (boxrig_ui_candidate_snapshot){.format = BOXRIG_UI_CANDIDATE_PAGE,
        .count = 3, .page_size = 3, .cursor = 2, .visible = true,
        .has_previous_page = true, .has_next_page = true,
        .auxiliary_above = "読み", .auxiliary_above_visible = true,
        .auxiliary = "説明", .auxiliary_visible = true};
    m.width = m.height = 500;
    m.anchor = (boxrig_ui_candidate_rectangle){300, 150, 9, 21};
    m.auxiliary_above_width = 100;
    m.auxiliary_above_lines = 2;
    CHECK(boxrig_ui_candidates_place(&candidates, &m, &result) == BOXRIG_UI_OK);
    CHECK(result.first == 0 && result.count == 3);
    CHECK(result.has_previous_page && result.has_next_page);
    CHECK(!result.hidden_before && !result.hidden_after);
    CHECK(result.auxiliary_above.height > 0 && result.auxiliary.height > 0);
    CHECK(result.auxiliary_above.y + result.auxiliary_above.height <= result.items[0].y);
    CHECK(result.items[2].y + result.items[2].height <= result.auxiliary.y);
    for (size_t hi = 0; hi < sizeof(dimensions) / sizeof(*dimensions); hi++) {
        m.height = dimensions[hi];
        CHECK(boxrig_ui_candidates_place(&candidates, &m, &result) == BOXRIG_UI_OK);
        CHECK(result.first <= 2 && result.first + result.count > 2);
        CHECK(result.bounds.y >= 0 && result.bounds.y + result.bounds.height <= m.height);
    }
    candidates.snapshot.count = candidates.snapshot.page_size = candidates.snapshot.cursor = 0;
    candidates.snapshot.visible = false;
    CHECK(boxrig_ui_candidates_place(&candidates, &m, &result) == BOXRIG_UI_OK);
    CHECK(result.count == 0 && result.bounds.height > 0);
    CHECK(result.auxiliary_above.height > 0 && result.auxiliary.height > 0);
    candidates.snapshot.auxiliary_above_visible = candidates.snapshot.auxiliary_visible = false;
    CHECK(boxrig_ui_candidates_place(&candidates, &m, &result) == BOXRIG_UI_OK);
    CHECK(result.bounds.height == 0);
}

static void string_panel_placement(void)
{
    boxrig_ui_candidate_view candidates = {.revision = 29, .snapshot = {
        .format = BOXRIG_UI_CANDIDATE_STRING_PANEL, .visible = true,
        .panel_text = "1. 汽車 2号 2. 記者 🙂", .panel_preedit = "きしゃ", .panel_cursor = 6,
        .auxiliary_above = "上", .auxiliary_above_visible = true,
        .auxiliary = "下", .auxiliary_visible = true}};
    boxrig_ui_candidate_measurement m = {.width = 600, .height = 400, .cell_width = 10,
        .line_height = 24, .anchor = {580, 100, 10, 24},
        .panel_text_width = 240, .panel_text_lines = 3, .panel_preedit_width = 72,
        .auxiliary_above_width = 24, .auxiliary_above_lines = 2,
        .auxiliary_width = 24, .auxiliary_lines = 2};
    boxrig_ui_candidate_layout result;
    CHECK(boxrig_ui_candidates_place(&candidates, &m, &result) == BOXRIG_UI_OK);
    CHECK(result.revision == 29 && result.count == 0 && result.first == 0);
    CHECK(!result.has_previous_page && !result.has_next_page && !result.horizontal);
    CHECK(!result.hidden_before && !result.hidden_after);
    CHECK(result.auxiliary_above.y + result.auxiliary_above.height <= result.panel_preedit.y);
    CHECK(result.panel_preedit.y + result.panel_preedit.height <= result.panel_text.y);
    CHECK(result.panel_text.y + result.panel_text.height <= result.auxiliary.y);
    CHECK(result.panel_text.height >= m.line_height * 3);
    const int sizes[] = {1, 2, 10, 35, 80, 200, 500};
    for (size_t w = 0; w < sizeof(sizes) / sizeof(*sizes); w++)
    for (size_t h = 0; h < sizeof(sizes) / sizeof(*sizes); h++) {
        m.width = sizes[w]; m.height = sizes[h];
        m.anchor.x = m.anchor.y = INT_MAX;
        CHECK(boxrig_ui_candidates_place(&candidates, &m, &result) == BOXRIG_UI_OK);
        CHECK(result.bounds.x >= 0 && result.bounds.y >= 0);
        CHECK(result.bounds.width > 0 && result.bounds.height > 0);
        CHECK(result.bounds.x + result.bounds.width <= m.width);
        CHECK(result.bounds.y + result.bounds.height <= m.height);
        const boxrig_ui_candidate_rectangle sections[] = {result.panel_preedit,
            result.panel_text, result.auxiliary_above, result.auxiliary};
        for (size_t i = 0; i < 4; i++) {
            boxrig_ui_candidate_rectangle r = sections[i];
            if (r.width == 0 || r.height == 0) continue;
            CHECK(r.x >= result.bounds.x && r.y >= result.bounds.y);
            CHECK(r.x + r.width <= result.bounds.x + result.bounds.width);
            CHECK(r.y + r.height <= result.bounds.y + result.bounds.height);
        }
    }
    m.width = 600; m.height = 400;
    candidates.snapshot.panel_text = "";
    candidates.snapshot.auxiliary_above_visible = candidates.snapshot.auxiliary_visible = false;
    CHECK(boxrig_ui_candidates_place(&candidates, &m, &result) == BOXRIG_UI_OK);
    CHECK(result.panel_preedit.height > 0 && result.panel_text.height == 0);
    candidates.snapshot.panel_preedit = NULL;
    candidates.snapshot.panel_cursor = 0;
    CHECK(boxrig_ui_candidates_place(&candidates, &m, &result) == BOXRIG_UI_OK);
    CHECK(result.bounds.height == 0);
    candidates.snapshot.auxiliary_visible = true;
    CHECK(boxrig_ui_candidates_place(&candidates, &m, &result) == BOXRIG_UI_OK);
    CHECK(result.auxiliary.height > 0 && result.panel_preedit.height == 0 && result.count == 0);
}

int main(void)
{
    owned_state();
    placement();
    string_panel_placement();
    puts("PASS owned candidate state, transactional limits, focus clearing and bounded selected-row placement");
    return 0;
}
