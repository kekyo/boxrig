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
}

int main(void)
{
    owned_state();
    placement();
    puts("PASS owned candidate state, transactional limits, focus clearing and bounded selected-row placement");
    return 0;
}
