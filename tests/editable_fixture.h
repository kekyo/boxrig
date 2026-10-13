/* boxrig - Under MIT. */
#ifndef BOXRIG_TEST_EDITABLE_FIXTURE_H
#define BOXRIG_TEST_EDITABLE_FIXTURE_H

#include <stdbool.h>
#include <stddef.h>

/* Native protocol support is independent of these document-side states.
 * GTK, Wayland and supporting XIM route synchronous edits to the owner.
 * IMM exposes the same context but edits through explicit reconversion. */
static const struct {
    const char *name;
    bool context_available;
    bool synchronous_edit_available;
    const char *after_synchronous_edit;
    size_t terminal_bytes;
} boxrig_editable_cases[] = {
    {"none", false, false, "前かな後", 3},
    {"context", true, false, "前かな後", 0},
    {"editable", true, true, "前か語後", 0},
};

static const struct {
    const char *text;
    size_t cursor;
    size_t anchor;
    const char *commit;
} boxrig_editable_fixture = {"前かな後", 9, 9, "語"};

#endif
