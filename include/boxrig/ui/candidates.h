/* boxrig - Under MIT. */
#ifndef BOXRIG_UI_CANDIDATES_H
#define BOXRIG_UI_CANDIDATES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Maximum candidates copied into one view-local input-method snapshot. */
#define BOXRIG_UI_CANDIDATES_MAXIMUM 1024u
/** Maximum candidates on one engine-specified page. */
#define BOXRIG_UI_CANDIDATE_PAGE_MAXIMUM 16u
/** Maximum copied UTF-8 bytes, including every terminating NUL. */
#define BOXRIG_UI_CANDIDATE_BYTES_MAXIMUM 65536u

/** Candidate presentation preference, independent of the input owner. */
typedef enum boxrig_ui_candidate_policy {
    BOXRIG_UI_CANDIDATE_AUTO = 0, /**< Prefer system display; direct Wayland IBus uses client display as a fallback. */
    BOXRIG_UI_CANDIDATE_SYSTEM, /**< Leave candidate display with the system input method. */
    BOXRIG_UI_CANDIDATE_CLIENT /**< Request client display without overriding explicit input routes. */
} boxrig_ui_candidate_policy;

/** Actual owner of candidate presentation, separate from input routing. */
typedef enum boxrig_ui_candidate_owner {
    BOXRIG_UI_CANDIDATE_OWNER_NONE = 0, /**< Input route is still being established. */
    BOXRIG_UI_CANDIDATE_OWNER_SYSTEM, /**< The selected system input method owns its popup. */
    BOXRIG_UI_CANDIDATE_OWNER_CLIENT /**< The view owns a viewport-local candidate overlay. */
} boxrig_ui_candidate_owner;

/** Reason for the current candidate display owner. */
typedef enum boxrig_ui_candidate_reason {
    BOXRIG_UI_CANDIDATE_PENDING = 0, /**< Input selection or connection is pending. */
    BOXRIG_UI_CANDIDATE_READY, /**< Direct IBus and a candidate renderer are ready. */
    BOXRIG_UI_CANDIDATE_SYSTEM_REQUEST, /**< The caller requested system display. */
    BOXRIG_UI_CANDIDATE_STANDARD_INPUT, /**< The selected input route owns system candidates. */
    BOXRIG_UI_CANDIDATE_NO_RENDERER, /**< The selected renderer cannot present client candidates. */
    BOXRIG_UI_CANDIDATE_IBUS_UNAVAILABLE /**< The direct IBus connection is unavailable. */
} boxrig_ui_candidate_reason;

/** Engine preference; horizontal pages may become vertical in a narrow view. */
typedef enum boxrig_ui_candidate_orientation {
    BOXRIG_UI_CANDIDATE_SYSTEM_ORIENTATION = 0, /**< Use the client's vertical default. */
    BOXRIG_UI_CANDIDATE_VERTICAL, /**< Place candidates in separate rows. */
    BOXRIG_UI_CANDIDATE_HORIZONTAL /**< Place a page on one row when it fits. */
} boxrig_ui_candidate_orientation;

/** Plain UTF-8 candidate data. Applying it never writes terminal text. */
typedef struct boxrig_ui_candidate_snapshot {
    size_t count; /**< Number of entries, at most BOXRIG_UI_CANDIDATES_MAXIMUM. */
    size_t page_size; /**< Engine page size from 1 to 16; zero is allowed when empty. */
    size_t cursor; /**< Absolute selected entry, or zero for an empty table. */
    boxrig_ui_candidate_orientation orientation; /**< Requested page direction. */
    bool visible; /**< Whether the lookup table is shown. */
    bool cursor_visible; /**< Whether the selected entry is highlighted. */
    bool auxiliary_visible; /**< Show auxiliary text while the retained table is nonempty. */
    const char *const *items; /**< count non-NULL NUL-terminated strings. */
    const char *const *labels; /**< Optional page_size labels; individual labels may be NULL. */
    const char *auxiliary; /**< Optional NUL-terminated supplementary text. */
} boxrig_ui_candidate_snapshot;

/** Rectangle in view-local output pixels, independent of desktop coordinates. */
typedef struct boxrig_ui_candidate_rectangle {
    int32_t x; /**< Left edge. */
    int32_t y; /**< Top edge. */
    int32_t width; /**< Nonnegative width. */
    int32_t height; /**< Nonnegative height. */
} boxrig_ui_candidate_rectangle;

/** Last renderer-computed placement of one bounded candidate page. */
typedef struct boxrig_ui_candidate_layout {
    uint64_t revision; /**< Snapshot revision used by the renderer. */
    boxrig_ui_candidate_rectangle bounds; /**< Entire panel; zero size means hidden. */
    boxrig_ui_candidate_rectangle items[BOXRIG_UI_CANDIDATE_PAGE_MAXIMUM]; /**< Visible entry rectangles. */
    boxrig_ui_candidate_rectangle auxiliary; /**< At most two lines of supplementary text. */
    size_t first; /**< Absolute index of the first displayed candidate. */
    size_t count; /**< Number of displayed candidates. */
    bool horizontal; /**< Effective row arrangement. */
    bool hidden_before; /**< Earlier entries of this page do not fit. */
    bool hidden_after; /**< Later entries of this page do not fit. */
} boxrig_ui_candidate_layout;

/** Borrowed candidate content and geometry owned by one terminal view. */
typedef struct boxrig_ui_candidate_view {
    boxrig_ui_candidate_snapshot snapshot; /**< Valid until the next update or view destruction. */
    uint64_t revision; /**< Nonzero after the first update, including clear operations. */
    boxrig_ui_candidate_layout layout; /**< Zero until a renderer has placed this snapshot. */
} boxrig_ui_candidate_view;

#endif
