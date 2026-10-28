/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#ifndef BOXRIG_TERMINAL_H
#define BOXRIG_TERMINAL_H

#include "boxrig/regex.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque terminal emulator state. */
typedef struct boxrig_terminal boxrig_terminal;

/** Exclusive host ownership of a terminal animation clock. */
typedef struct boxrig_animation_clock boxrig_animation_clock;

/** One copied override for a legacy cursor key. */
typedef struct boxrig_key_mapping boxrig_key_mapping;

/** Maximum entries in a terminal's key mapping table. */
#define BOXRIG_KEY_MAPPING_COUNT_MAX ((size_t)64u)
/** Maximum bytes in one configured key sequence, including embedded NUL. */
#define BOXRIG_KEY_MAPPING_BYTES_MAX ((size_t)4096u)
/** Maximum aggregate bytes in all configured key sequences. */
#define BOXRIG_KEY_MAPPING_TOTAL_BYTES_MAX ((size_t)65536u)

/** Maximum supported primary scrollback capacity in physical rows. */
#define BOXRIG_SCROLLBACK_CAPACITY_MAX ((size_t)1048576u)

/** Maximum UTF-8 bytes retained in one terminal text cluster. */
#define BOXRIG_TEXT_CLUSTER_CAPACITY_MAX ((size_t)4096u)

/** Maximum supported literal search pattern length in UTF-8 bytes. */
#define BOXRIG_TEXT_SEARCH_PATTERN_CAPACITY_MAX ((size_t)65536u)

/** Maximum supported text-monitor pattern length in UTF-8 bytes. */
#define BOXRIG_TEXT_MONITOR_PATTERN_CAPACITY_MAX \
    BOXRIG_TEXT_SEARCH_PATTERN_CAPACITY_MAX

/** Maximum bounded-regular-expression pattern length in UTF-8 bytes. */
#define BOXRIG_TEXT_RECOGNIZER_PATTERN_CAPACITY_MAX ((size_t)65536u)

/** Maximum Thompson NFA states owned by one text recognizer. */
#define BOXRIG_TEXT_RECOGNIZER_STATE_CAPACITY_MAX ((size_t)65536u)

/** Maximum upper bound accepted by one finite regex quantifier. */
#define BOXRIG_TEXT_RECOGNIZER_QUANTIFIER_CAPACITY_MAX ((size_t)4096u)

/** Measurement of the first terminal text cluster in a UTF-8 buffer. */
typedef struct boxrig_text_cluster_measurement {
    /** Number of input bytes belonging to the returned cluster. */
    size_t text_length;
    /** Intrinsic terminal column width before viewport placement. */
    uint8_t column_width;
    /** True when the storage limit, rather than Unicode, ended the cluster. */
    bool split_by_limit;
    /** Preserve natural font advance and center within the occupied columns. */
    bool font_dependent;
} boxrig_text_cluster_measurement;

/** Logical text document namespace. */
typedef enum boxrig_text_screen {
    /** Retained primary history followed by the current primary grid. */
    BOXRIG_TEXT_SCREEN_PRIMARY = 0,
    /** Current alternate grid, independent of primary history. */
    BOXRIG_TEXT_SCREEN_ALTERNATE
} boxrig_text_screen;

/** Opaque logical-line identity within one terminal lifetime. */
typedef struct boxrig_text_line_id {
    /** Text identity epoch. */
    uint64_t epoch;
    /** Monotonic line serial within the epoch. */
    uint64_t serial;
} boxrig_text_line_id;

/** Stable grapheme boundary in one retained logical text document. */
typedef struct boxrig_text_position {
    /** Primary or alternate document namespace. */
    boxrig_text_screen screen;
    /** Logical line containing the boundary. */
    boxrig_text_line_id line_id;
    /** Terminal-column offset from the logical line start. */
    size_t column;
    /** Content revision of the logical line when this position was made. */
    uint64_t line_revision;
    /** Terminal text revision when this position was made. */
    uint64_t text_revision;
} boxrig_text_position;

/** Half-open range between two logical text positions. */
typedef struct boxrig_text_range {
    /** Inclusive start boundary. */
    boxrig_text_position start;
    /** Exclusive end boundary. */
    boxrig_text_position end;
} boxrig_text_range;

/** Metadata for one retained physical fragment of a logical line. */
typedef struct boxrig_text_fragment_view {
    /** Document namespace containing the fragment. */
    boxrig_text_screen screen;
    /** Logical line shared by all soft-wrapped fragments. */
    boxrig_text_line_id line_id;
    /** Terminal-column offset of this fragment within the logical line. */
    size_t logical_column_offset;
    /** Current content revision shared by the logical line. */
    uint64_t line_revision;
    /** True when this fragment continues without a line-break byte. */
    bool soft_wrapped;
} boxrig_text_fragment_view;

/** Opaque immutable logical text snapshot. */
typedef struct boxrig_text_snapshot boxrig_text_snapshot;

/** Opaque resumable literal text search. */
typedef struct boxrig_text_search_cursor boxrig_text_search_cursor;

/** Limits and scope used to create an owned logical text snapshot. */
typedef struct boxrig_text_snapshot_options {
    /** Primary or alternate source document. */
    boxrig_text_screen screen;
    /** Optional half-open range; NULL snapshots the complete document. */
    const boxrig_text_range *range;
    /** Maximum projected UTF-8 bytes owned by the snapshot. */
    size_t byte_capacity;
    /** Maximum logical lines owned by the snapshot. */
    size_t line_capacity;
} boxrig_text_snapshot_options;

/** One logical range belonging to a projected output row. */
typedef struct boxrig_text_snapshot_row_range {
    /** Zero-based top-to-bottom output row sequence. */
    size_t row_sequence;
    /** Complete grapheme-boundary range in logical document order. */
    boxrig_text_range range;
} boxrig_text_snapshot_row_range;

/** Bounded row-wise source used to create an owned text snapshot. */
typedef struct boxrig_text_snapshot_rows_options {
    /** Primary or alternate source document. */
    boxrig_text_screen screen;
    /** Ranges ordered by row sequence and logical position; empty ranges are allowed. */
    const boxrig_text_snapshot_row_range *ranges;
    /** Number of entries in ranges. */
    size_t range_count;
    /** Number of output rows represented by ranges, including empty rows. */
    size_t row_count;
    /** Maximum projected UTF-8 bytes owned by the snapshot. */
    size_t byte_capacity;
} boxrig_text_snapshot_rows_options;

/** Direction used to traverse one logical text document. */
typedef enum boxrig_text_search_direction {
    /** Return the first match at or after the start boundary. */
    BOXRIG_TEXT_SEARCH_FORWARD = 0,
    /** Return the last match ending at or before the start boundary. */
    BOXRIG_TEXT_SEARCH_BACKWARD
} boxrig_text_search_direction;

/** Observable progress of one resumable literal search. */
typedef enum boxrig_text_search_progress {
    /** More bounded dispatches are required. */
    BOXRIG_TEXT_SEARCH_IN_PROGRESS = 0,
    /** One complete grapheme-boundary match was returned. */
    BOXRIG_TEXT_SEARCH_MATCH,
    /** The requested traversal, including one optional wrap, is complete. */
    BOXRIG_TEXT_SEARCH_NOT_FOUND
} boxrig_text_search_progress;

/** Immutable inputs copied when one literal search is created. */
typedef struct boxrig_text_search_options {
    /** Primary or alternate logical text namespace. */
    boxrig_text_screen screen;
    /** Nonempty length-delimited valid UTF-8 literal bytes. */
    const uint8_t *pattern;
    /** Number of bytes in pattern. */
    size_t pattern_length;
    /** Forward or backward document traversal. */
    boxrig_text_search_direction direction;
    /**
     * Optional projected grapheme or hard-line boundary. NULL selects the
     * document beginning for forward search and the end for backward search.
     */
    const boxrig_text_position *start;
    /** Whether to inspect the other side of an explicit start exactly once. */
    bool wrap;
} boxrig_text_search_options;

/** One immutable literal match in logical document order. */
typedef struct boxrig_text_search_result {
    /** Namespace containing the match. */
    boxrig_text_screen screen;
    /** Half-open grapheme-boundary range containing the literal bytes. */
    boxrig_text_range range;
    /** Terminal text revision at which the match was observed. */
    uint64_t text_revision;
    /** Nonzero terminal-lifetime result identity. */
    uint64_t result_id;
} boxrig_text_search_result;

/** Logical text namespaces observed by one text monitor. */
typedef enum boxrig_text_monitor_scope {
    /** Observe only retained primary text. */
    BOXRIG_TEXT_MONITOR_SCOPE_PRIMARY = 0,
    /** Observe only alternate-screen text. */
    BOXRIG_TEXT_MONITOR_SCOPE_ALTERNATE,
    /** Observe primary text followed by alternate-screen text. */
    BOXRIG_TEXT_MONITOR_SCOPE_BOTH
} boxrig_text_monitor_scope;

/** Baseline policy used when a text monitor is registered or rescanned. */
typedef enum boxrig_text_monitor_policy {
    /** Report only matches created by later semantic screen mutations. */
    BOXRIG_TEXT_MONITOR_FUTURE_ONLY = 0,
    /** Report an immutable current baseline before later matches. */
    BOXRIG_TEXT_MONITOR_EXISTING_AND_FUTURE
} boxrig_text_monitor_policy;

/** Kind of application-pulled text-monitor event. */
typedef enum boxrig_text_monitor_event_kind {
    /** A new grapheme-boundary literal match was observed. */
    BOXRIG_TEXT_MONITOR_EVENT_MATCH = 0,
    /** Bounded bookkeeping lost events and a current rescan is required. */
    BOXRIG_TEXT_MONITOR_EVENT_RESCAN_REQUIRED
} boxrig_text_monitor_event_kind;

/** Origin of one text-monitor match. */
typedef enum boxrig_text_monitor_event_origin {
    /** Match came from an immutable registration or rescan baseline. */
    BOXRIG_TEXT_MONITOR_EVENT_EXISTING = 0,
    /** Match was created by a later semantic screen mutation. */
    BOXRIG_TEXT_MONITOR_EVENT_FUTURE
} boxrig_text_monitor_event_origin;

/** Immutable inputs copied when one text monitor is registered. */
typedef struct boxrig_text_monitor_options {
    /** Primary, alternate, or both logical text namespaces. */
    boxrig_text_monitor_scope scope;
    /** Whether registration begins with an immutable current baseline. */
    boxrig_text_monitor_policy policy;
    /** Nonempty length-delimited valid UTF-8 literal bytes. */
    const uint8_t *pattern;
    /** Number of bytes in pattern. */
    size_t pattern_length;
} boxrig_text_monitor_options;

/** One immutable value copied from the terminal monitor event queue. */
typedef struct boxrig_text_monitor_event {
    /** Match or sticky rescan-required notification. */
    boxrig_text_monitor_event_kind kind;
    /** Existing or future source; ignored for rescan-required events. */
    boxrig_text_monitor_event_origin origin;
    /** Terminal-lifetime monitor identity. */
    uint64_t monitor_id;
    /** Nonzero terminal-lifetime event identity. */
    uint64_t event_id;
    /** Semantic mutation identity, or zero for an existing match. */
    uint64_t mutation_serial;
    /** Namespace containing a match; ignored for rescan-required events. */
    boxrig_text_screen screen;
    /** Revision at the match or first known loss. */
    uint64_t text_revision;
    /** Match range; cleared for rescan-required events. */
    boxrig_text_range range;
} boxrig_text_monitor_event;

/** Logical text namespaces scanned by one regular-expression recognizer. */
typedef enum boxrig_text_recognizer_scope {
    /** Scan only retained primary text. */
    BOXRIG_TEXT_RECOGNIZER_SCOPE_PRIMARY = 0,
    /** Scan only alternate-screen text. */
    BOXRIG_TEXT_RECOGNIZER_SCOPE_ALTERNATE,
    /** Scan primary text followed by alternate-screen text. */
    BOXRIG_TEXT_RECOGNIZER_SCOPE_BOTH
} boxrig_text_recognizer_scope;

/**
 * Strict per-recognizer resource limits.
 *
 * @remarks Every member must be positive; zero never means unlimited.
 */
typedef struct boxrig_text_recognizer_limits {
    /** Maximum compiled Thompson NFA states. */
    size_t state_capacity;
    /** Maximum upper bound of one finite quantifier. */
    size_t quantifier_capacity;
    /** Maximum numbered capture groups when compiling a pattern. */
    size_t capture_capacity;
    /** Maximum capture-register bytes when compiling a pattern. */
    size_t capture_byte_capacity;
    /** Maximum projected UTF-8 bytes in one logical line. */
    size_t line_byte_capacity;
    /** Maximum terminal columns in one logical line. */
    size_t line_column_capacity;
    /** Maximum non-overlapping matches retained for one logical line. */
    size_t line_match_capacity;
    /** Maximum matches retained across both configured namespaces. */
    size_t result_capacity;
    /** Maximum matched bytes and capture records per current/staging set.
     * Both sets may coexist; externally retained results are caller-owned. */
    size_t result_byte_capacity;
    /** Maximum logical lines retained by current result bookkeeping.
     * Committed notification storage is bounded by twice this many events. */
    size_t dirty_line_capacity;
    /** Maximum projected bytes captured by one immutable baseline. */
    size_t snapshot_byte_capacity;
    /** Maximum logical lines captured by one immutable baseline. */
    size_t snapshot_line_capacity;
} boxrig_text_recognizer_limits;

/** Immutable inputs copied when one text recognizer is registered. */
typedef struct boxrig_text_recognizer_options {
    /** Primary, alternate, or both logical text namespaces. */
    boxrig_text_recognizer_scope scope;
    /** Nonempty length-delimited bounded regular expression in UTF-8. */
    const uint8_t *pattern;
    /** Number of bytes in pattern. */
    size_t pattern_length;
    /** Explicit finite compiler, scan, snapshot, and result limits. */
    boxrig_text_recognizer_limits limits;
} boxrig_text_recognizer_options;

/** Kind of application-pulled recognizer result-set notification. */
typedef enum boxrig_text_recognizer_event_kind {
    /** One logical line's complete current result set was replaced. */
    BOXRIG_TEXT_RECOGNIZER_EVENT_LINE_REPLACED = 0,
    /** A line with previous results left retention. */
    BOXRIG_TEXT_RECOGNIZER_EVENT_LINE_EVICTED,
    /** Undelivered notifications were invalidated by a text/layout change;
     * clear application annotations and explicitly request a current rescan. */
    BOXRIG_TEXT_RECOGNIZER_EVENT_RESCAN_REQUIRED,
    /** A scan limit prevented publication of any partial current set. */
    BOXRIG_TEXT_RECOGNIZER_EVENT_SCOPE_INCOMPLETE
} boxrig_text_recognizer_event_kind;

/** Immutable value copied from the terminal recognizer event queue. */
typedef struct boxrig_text_recognizer_event {
    /** Result-set replacement, eviction, rescan request, or incomplete scope. */
    boxrig_text_recognizer_event_kind kind;
    /** Nonzero terminal-lifetime recognizer identity. */
    uint64_t recognizer_id;
    /** Nonzero revision of the recognizer's complete current result set. */
    uint64_t result_set_revision;
    /** Namespace containing the line. */
    boxrig_text_screen screen;
    /** Stable logical-line identity, cleared for scope-wide events. */
    boxrig_text_line_id line_id;
    /** Content revision of line, or zero for scope-wide events. */
    uint64_t line_revision;
    /** Terminal text revision observed by the completed scan. */
    uint64_t text_revision;
    /** Number of current results for a replaced line. */
    size_t result_count;
} boxrig_text_recognizer_event;

/** One whole-match result in stable recognizer and match identity order. */
typedef struct boxrig_text_recognition_result {
    /** Recognizer that owns this match. */
    uint64_t recognizer_id;
    /** Nonzero terminal-lifetime match identity. */
    uint64_t match_id;
    /** Revision of the complete result set containing this match. */
    uint64_t result_set_revision;
    /** Namespace containing the match. */
    boxrig_text_screen screen;
    /** Whole-match half-open grapheme-boundary range. */
    boxrig_text_range range;
    /** Terminal text revision observed by the completed scan. */
    uint64_t text_revision;
} boxrig_text_recognition_result;

/** Column-width policy for characters with East Asian Width class Ambiguous. */
typedef enum boxrig_ambiguous_width {
    /** Treat ambiguous characters as one terminal column. */
    BOXRIG_AMBIGUOUS_WIDTH_NARROW = 0,
    /** Treat ambiguous characters as two terminal columns. */
    BOXRIG_AMBIGUOUS_WIDTH_WIDE,
    /** Round the resolved font advance up to whole terminal columns. */
    BOXRIG_AMBIGUOUS_WIDTH_FONT
} boxrig_ambiguous_width;

/**
 * Measures an ambiguous grapheme using the resolved font.
 * @param context Borrowed application context.
 * @param text Length-delimited UTF-8 grapheme, including combining marks.
 * @param text_length Number of UTF-8 bytes.
 * @param attributes Cell rendition flags, including bold and italic.
 * @return Natural advance divided by the cell width. Nonpositive or nonfinite
 * values request the Unicode narrow fallback. Values above 255 exceed the
 * supported cell-span limit.
 * @remarks Called synchronously. Do not mutate the terminal from this callback.
 */
typedef double (*boxrig_font_width_resolver)(void *context,
    const uint8_t *text, size_t text_length, uint32_t attributes);

/** Unit used by horizontal cursor positions received from the client. */
typedef enum boxrig_cursor_column_mode {
    /** Interpret cursor positions as terminal display columns. */
    BOXRIG_CURSOR_COLUMN_MODE_TERMINAL = 0,
    /** Map UTF-16 code-unit cursor movement onto terminal display columns. */
    BOXRIG_CURSOR_COLUMN_MODE_UTF16
} boxrig_cursor_column_mode;

/** Outbound newline encoding selected by input configuration. */
typedef enum boxrig_newline {
    /** One carriage return byte. */
    BOXRIG_NEWLINE_CR = 0,
    /** One line feed byte. */
    BOXRIG_NEWLINE_LF,
    /** Carriage return followed by line feed, added atomically. */
    BOXRIG_NEWLINE_CRLF,
    /** Enter follows mode 20: CR when reset and CRLF when set. */
    BOXRIG_NEWLINE_TERMINAL
} boxrig_newline;

/** Legacy encoding selected independently for Backspace and Delete. */
typedef enum boxrig_erase_binding {
    /** Backspace sends DEL; Delete sends CSI 3 ~. */
    BOXRIG_ERASE_AUTO = 0,
    /** Send the single ASCII BS byte (0x08). */
    BOXRIG_ERASE_ASCII_BACKSPACE,
    /** Send the single ASCII DEL byte (0x7f). */
    BOXRIG_ERASE_ASCII_DELETE,
    /** Send CSI 3 ~, with the existing Delete modifier parameter when needed. */
    BOXRIG_ERASE_DELETE_SEQUENCE,
    /** Send the current communication channel's erase byte; use AUTO if unknown. */
    BOXRIG_ERASE_TTY
} boxrig_erase_binding;

/** Erase information supplied by the owner of the current communication channel. */
typedef struct boxrig_tty_erase {
    /** True when byte was obtained; NUL and disabled erase values remain available. */
    bool available;
    /** Unmodified one-byte value, ignored when unavailable. */
    uint8_t byte;
} boxrig_tty_erase;

/** Input settings validated and copied atomically at construction or runtime. */
typedef struct boxrig_input_config {
    /** Legacy Enter encoding. Defaults to TERMINAL; fixed CR, LF and CRLF
     * ignore mode 20. Numeric keypad Enter follows this value. Kitty reports
     * and application-keypad sequences retain their protocol encoding. */
    boxrig_newline enter_newline;
    /** Independent legacy Backspace choice; defaults to AUTO. */
    boxrig_erase_binding backspace_binding;
    /** Independent legacy Delete/keypad Delete choice; defaults to AUTO. */
    boxrig_erase_binding delete_binding;
    /** Copied legacy cursor-key overrides, or NULL for an empty table. */
    const boxrig_key_mapping *key_mappings;
    /** Table entries, at most BOXRIG_KEY_MAPPING_COUNT_MAX. */
    size_t key_mapping_count;
} boxrig_input_config;

/** Interpretation of received display CR and LF controls. */
typedef enum boxrig_receive_newline {
    /** CR returns; LF advances and returns only when mode 20 is set. */
    BOXRIG_RECEIVE_NEWLINE_TERMINAL = 0,
    /** CR returns; LF advances without returning, independently of mode 20. */
    BOXRIG_RECEIVE_NEWLINE_STANDARD,
    /** CR returns; LF advances to the next line's first column. */
    BOXRIG_RECEIVE_NEWLINE_LF,
    /** CR advances and returns; LF advances without returning. */
    BOXRIG_RECEIVE_NEWLINE_CR,
    /** CR and LF advance and return; an immediately adjacent CRLF counts once. */
    BOXRIG_RECEIVE_NEWLINE_CRLF
} boxrig_receive_newline;

/** Text encoding used for the terminal client connection. */
typedef enum boxrig_encoding {
    /** Unicode UTF-8, the default encoding. */
    BOXRIG_ENCODING_UTF8 = 0,
    /** WHATWG Shift_JIS, including its index and replacement rules. */
    BOXRIG_ENCODING_SHIFT_JIS
} boxrig_encoding;

/** Terminal construction settings copied or owned by the terminal. */
typedef struct boxrig_terminal_config {
    /** Initial number of rows. */
    size_t rows;
    /** Initial number of columns. */
    size_t columns;
    /** Maximum number of queued host events and requests. */
    size_t host_item_capacity;
    /** Maximum aggregate payload bytes retained by queued host items. */
    size_t host_payload_capacity;
    /** Maximum number of queued bytes destined for the terminal client. */
    size_t output_capacity;
    /** Maximum content bytes accepted after one control-string introducer. */
    size_t control_string_capacity;
    /** Maximum aggregate URI and id bytes retained for OSC 8 hyperlinks. */
    size_t hyperlink_capacity;
    /**
     * Maximum decoded bytes accepted from or returned to OSC 52.
     * Zero disables clipboard integration.
     */
    size_t clipboard_capacity;
    /**
     * Maximum decoded bytes retained by one OSC 99 notification.
     * The default is 32 KiB. Zero disables OSC 99, including queries.
     */
    size_t notification_capacity;
    /**
     * Maximum number of incomplete OSC 99 notification transactions.
     * The default is 16. This must be zero exactly when
     * notification_capacity is zero.
     */
    size_t notification_pending_capacity;
    /**
     * Maximum number of physical primary-screen rows retained in history.
     * The default is 10,000, zero disables history, and the hard limit is
     * BOXRIG_SCROLLBACK_CAPACITY_MAX.
     */
    size_t scrollback_capacity;
    /** Maximum registered text monitors. The default is 32. */
    size_t text_monitor_capacity;
    /** Aggregate monitor-pattern bytes. The default is 256 KiB. */
    size_t text_monitor_pattern_capacity;
    /** Bytes in one immutable monitor baseline. The default is 8 MiB. */
    size_t text_monitor_snapshot_byte_capacity;
    /** Logical lines in one monitor baseline. The default is 262,144. */
    size_t text_monitor_snapshot_line_capacity;
    /** Staged baseline and future events per monitor. The default is 1,024. */
    size_t text_monitor_staging_capacity;
    /** Application-visible unconsumed monitor events. The default is 4,096. */
    size_t text_monitor_event_capacity;
    /** Projected bytes captured around one mutation. The default is 1 MiB. */
    size_t text_monitor_change_byte_capacity;
    /** Logical lines captured around one mutation. The default is 4,096. */
    size_t text_monitor_change_line_capacity;
    /** Maximum registered regular-expression recognizers. The default is 32. */
    size_t text_recognizer_capacity;
    /** Aggregate copied recognizer-pattern bytes. The default is 256 KiB. */
    size_t text_recognizer_pattern_capacity;
    /** Application-visible recognizer events. The default is 4,096.
     * Additional committed notifications wait in bounded per-recognizer
     * storage and become visible as application events are consumed. */
    size_t text_recognizer_event_capacity;
    /**
     * Maximum aggregate bytes of committed normalized RGBA image pixels.
     * The default is 128 MiB. Kitty graphics is disabled only when this and
     * all other graphics capacities are zero.
     */
    size_t graphics_storage_capacity;
    /**
     * Maximum bytes owned by one active graphics decode transaction.
     * The default is 192 MiB. This must be zero exactly when all other
     * graphics capacities are zero.
     */
    size_t graphics_decode_capacity;
    /**
     * Maximum number of committed Kitty graphics image resources.
     * The default is 4,096. This must be zero exactly when all other graphics
     * capacities are zero.
     */
    size_t graphics_image_capacity;
    /**
     * Maximum number of protocol image placements across all screen buffers.
     * The default is 8,192. This must be zero exactly when all other graphics
     * capacities are zero.
     */
    size_t graphics_placement_capacity;
    /**
     * Maximum decoded bytes in one external graphics path or object name.
     * The default is 4 KiB. Zero disables external graphics transports.
     */
    size_t graphics_transport_name_capacity;
    /**
     * Maximum aggregate bytes of normalized animation frames other than each
     * image's root frame. The default is 640 MiB. This must be zero exactly
     * when graphics_frame_capacity is zero.
     */
    size_t graphics_animation_storage_capacity;
    /**
     * Maximum number of animation frames other than image root frames.
     * The default is 16,384. This must be zero exactly when
     * graphics_animation_storage_capacity is zero.
     */
    size_t graphics_frame_capacity;
    /** Width policy for East Asian Width class Ambiguous characters. Defaults to NARROW. */
    boxrig_ambiguous_width ambiguous_width;
    /** Unit used by horizontal cursor positions received from the client. */
    boxrig_cursor_column_mode cursor_column_mode;
    /** Copied input settings, retained across RIS and DECSTR. */
    boxrig_input_config input;
    /**
     * Encoding of LF bytes in paste chunks. Defaults to LF; only CR, LF and
     * CRLF are accepted. Other bytes and bracketed-paste markers are unchanged.
     * UI adapters normalize incoming clipboard CR and CRLF before this step.
     * The terminal copies this value and retains it across resets.
     */
    boxrig_newline paste_newline;
    /**
     * Received display CR/LF policy. Defaults to TERMINAL. Fixed policies do
     * not change mode 20 reports or the handling of VT, FF, IND, NEL and string
     * payloads. CRLF merges only adjacent accepted bytes, including across feed
     * boundaries; any intervening byte ends the pair. Retained across resets.
     */
    boxrig_receive_newline receive_newline;
    /** Text codec, fixed at construction and retained across terminal resets. */
    boxrig_encoding encoding;
} boxrig_terminal_config;

/** The representation used by a terminal color. */
typedef enum boxrig_color_kind {
    BOXRIG_COLOR_DEFAULT = 0,
    BOXRIG_COLOR_INDEXED,
    BOXRIG_COLOR_RGB
} boxrig_color_kind;

/** A logical terminal color independent of a rendering backend. */
typedef struct boxrig_color {
    /** Color representation. */
    boxrig_color_kind kind;
    /** Palette index when kind is BOXRIG_COLOR_INDEXED. */
    uint8_t index;
    /** Red component when kind is BOXRIG_COLOR_RGB. */
    uint8_t red;
    /** Green component when kind is BOXRIG_COLOR_RGB. */
    uint8_t green;
    /** Blue component when kind is BOXRIG_COLOR_RGB. */
    uint8_t blue;
} boxrig_color;

/** An eight-bit-per-channel RGB color owned by terminal palette state. */
typedef struct boxrig_rgb {
    /** Red component. */
    uint8_t red;
    /** Green component. */
    uint8_t green;
    /** Blue component. */
    uint8_t blue;
} boxrig_rgb;

/** Dynamic non-indexed colors configurable by OSC. */
typedef enum boxrig_dynamic_color {
    BOXRIG_DYNAMIC_COLOR_FOREGROUND = 0,
    BOXRIG_DYNAMIC_COLOR_BACKGROUND,
    BOXRIG_DYNAMIC_COLOR_CURSOR
} boxrig_dynamic_color;

/** Bit flags describing terminal cell rendition. */
typedef enum boxrig_cell_attribute {
    BOXRIG_CELL_BOLD = 1u << 0,
    BOXRIG_CELL_FAINT = 1u << 1,
    BOXRIG_CELL_ITALIC = 1u << 2,
    BOXRIG_CELL_UNDERLINE = 1u << 3,
    BOXRIG_CELL_BLINK = 1u << 4,
    BOXRIG_CELL_INVERSE = 1u << 5,
    BOXRIG_CELL_CONCEALED = 1u << 6,
    BOXRIG_CELL_STRIKETHROUGH = 1u << 7,
    /** Rapid text blink; always accompanied by BOXRIG_CELL_BLINK. */
    BOXRIG_CELL_RAPID_BLINK = 1u << 8
} boxrig_cell_attribute;

/** Underline presentation selected for a terminal cell. */
typedef enum boxrig_underline_style {
    BOXRIG_UNDERLINE_NONE = 0,
    BOXRIG_UNDERLINE_SINGLE,
    BOXRIG_UNDERLINE_DOUBLE,
    BOXRIG_UNDERLINE_CURLY,
    BOXRIG_UNDERLINE_DOTTED,
    BOXRIG_UNDERLINE_DASHED
} boxrig_underline_style;

/** A read-only view of one terminal cell. */
typedef struct boxrig_cell_view {
    /** Length-delimited UTF-8 extended grapheme cluster. */
    const char *text;
    /** Number of bytes in text. */
    size_t text_length;
    /** Number of terminal columns occupied by the originating cell. */
    uint8_t column_width;
    /** True when this cell continues a preceding wide cell. */
    bool continuation;
    /** Preserve the font advance and center it in the allocated columns. */
    bool font_dependent;
    /** Nonzero terminal-lifetime identity of the OSC 8 hyperlink. */
    uint32_t hyperlink_handle;
    /** Untrusted length-delimited UTF-8 hyperlink URI, or NULL when absent. */
    const char *hyperlink_uri;
    /** Number of bytes in hyperlink_uri. */
    size_t hyperlink_uri_length;
    /** Length-delimited OSC 8 id parameter, or NULL when absent. */
    const char *hyperlink_id;
    /** Number of bytes in hyperlink_id. */
    size_t hyperlink_id_length;
    /** Bitwise combination of boxrig_cell_attribute values. */
    uint32_t attributes;
    /** Underline presentation, or BOXRIG_UNDERLINE_NONE. */
    boxrig_underline_style underline_style;
    /** Logical underline color. */
    boxrig_color underline_color;
    /** Whether DEC selective erase preserves this cell. */
    bool protected_cell;
    /** Logical foreground color. */
    boxrig_color foreground;
    /** Logical background color. */
    boxrig_color background;
} boxrig_cell_view;

/** Resolution of a viewport or retained-fragment cell to a complete grapheme. */
typedef struct boxrig_viewport_cell_resolution {
    /** Metadata for the physical soft-wrap fragment containing the cell. */
    boxrig_text_fragment_view fragment;
    /** Zero-based document-order index of fragment. */
    size_t fragment_index;
    /** Leading storage column after normalizing a continuation cell. */
    size_t canonical_storage_column;
    /** Number of storage columns occupied by the complete grapheme. */
    size_t column_span;
    /** True when the leading storage cell contains no text. */
    bool blank;
    /** Stable half-open logical range of the complete grapheme. */
    boxrig_text_range range;
    /** Semantic view of the leading cell, borrowed until the next mutation. */
    boxrig_cell_view cell;
} boxrig_viewport_cell_resolution;

/** Shape used to present the terminal cursor. */
typedef enum boxrig_cursor_shape {
    BOXRIG_CURSOR_BLOCK = 0,
    BOXRIG_CURSOR_UNDERLINE,
    BOXRIG_CURSOR_BAR
} boxrig_cursor_shape;

/** Current terminal cursor presentation. */
typedef struct boxrig_cursor_view {
    /** Zero-based cursor row. */
    size_t row;
    /** Zero-based cursor column. */
    size_t column;
    /** Whether the cursor should be rendered. */
    bool visible;
    /** Cursor presentation shape. */
    boxrig_cursor_shape shape;
    /** Whether the cursor should blink. */
    bool blinking;
} boxrig_cursor_view;

/** A changed interval within a terminal row. */
typedef struct boxrig_dirty {
    /** Zero-based changed row. */
    size_t row;
    /** First changed column. */
    size_t column;
    /** Number of changed columns. */
    size_t column_count;
} boxrig_dirty;

/** Opaque identity of one committed image resource. */
typedef uint64_t boxrig_image_handle;

/** A read-only view of one normalized image resource. */
typedef struct boxrig_image_view {
    /** Nonzero core-owned resource identity used as a renderer cache key. */
    boxrig_image_handle handle;
    /** Nonzero revision of this resource's normalized pixel data. */
    uint64_t revision;
    /** Kitty protocol image id, including zero for anonymous resources. */
    uint32_t image_id;
    /** Kitty protocol image number, including zero when absent. */
    uint32_t image_number;
    /** Pixel width. */
    size_t width;
    /** Pixel height. */
    size_t height;
    /** Number of bytes between successive RGBA rows. */
    size_t stride;
    /** Eight-bit non-premultiplied sRGB RGBA bytes. */
    const uint8_t *rgba;
    /** Number of bytes in rgba. */
    size_t rgba_length;
} boxrig_image_view;

/** Origin of one image drawing instance in the active viewport. */
typedef enum boxrig_image_placement_kind {
    /** Placement anchored directly to a terminal cell. */
    BOXRIG_IMAGE_PLACEMENT_NORMAL = 0,
    /** Placement positioned relative to another placement. */
    BOXRIG_IMAGE_PLACEMENT_RELATIVE,
    /** Instance derived from a Unicode placeholder cell. */
    BOXRIG_IMAGE_PLACEMENT_PLACEHOLDER
} boxrig_image_placement_kind;

/** A read-only resolved image drawing instance in the active viewport. */
typedef struct boxrig_image_placement_view {
    /** Resource rendered by this instance. */
    boxrig_image_handle image_handle;
    /** Nonzero revision of placement geometry and clipping. */
    uint64_t revision;
    /** Monotonic creation serial of the referenced resource. */
    uint64_t resource_creation_serial;
    /** Monotonic creation serial of the placement prototype. */
    uint64_t placement_creation_serial;
    /** Placement origin kind. */
    boxrig_image_placement_kind kind;
    /** Kitty protocol image id. */
    uint32_t image_id;
    /** Kitty protocol image number. */
    uint32_t image_number;
    /** Kitty protocol placement id. */
    uint32_t placement_id;
    /** Zero-based logical anchor row in its screen buffer. */
    size_t anchor_row;
    /** Zero-based logical anchor column in its screen buffer. */
    size_t anchor_column;
    /** Zero-based active-viewport row containing the resolved upper-left. */
    size_t visual_row;
    /** Zero-based visual column containing the resolved upper-left. */
    size_t visual_column;
    /** Left source pixel after intersection with the image. */
    size_t source_x;
    /** Top source pixel after intersection with the image. */
    size_t source_y;
    /** Intersected source width in pixels. */
    size_t source_width;
    /** Intersected source height in pixels. */
    size_t source_height;
    /** Horizontal pixel offset within the first cell. */
    uint32_t cell_x_offset;
    /** Vertical pixel offset within the first cell. */
    uint32_t cell_y_offset;
    /** Resolved destination width in terminal cells. */
    size_t columns;
    /** Resolved destination height in terminal cells. */
    size_t rows;
    /** Resolved destination width in pixels before clipping. */
    size_t destination_pixel_width;
    /** Resolved destination height in pixels before clipping. */
    size_t destination_pixel_height;
    /** Left clip offset within the resolved destination, in pixels. */
    size_t clip_x;
    /** Top clip offset within the resolved destination, in pixels. */
    size_t clip_y;
    /** Visible clip width in pixels. */
    size_t clip_width;
    /** Visible clip height in pixels. */
    size_t clip_height;
    /** Signed Kitty graphics stacking index. */
    int32_t z_index;
} boxrig_image_placement_view;

/** External resource medium requested by Kitty graphics. */
typedef enum boxrig_graphics_transport_medium {
    /** Read a regular file without deleting it. */
    BOXRIG_GRAPHICS_TRANSPORT_FILE = 0,
    /** Read and delete a protocol temporary file under host policy. */
    BOXRIG_GRAPHICS_TRANSPORT_TEMPORARY_FILE,
    /** Read a POSIX shared-memory object or Win32 named file mapping. */
    BOXRIG_GRAPHICS_TRANSPORT_SHARED_MEMORY
} boxrig_graphics_transport_medium;

/** A read-only external graphics data request owned by the terminal. */
typedef struct boxrig_graphics_transport_view {
    /** Requested external resource medium. */
    boxrig_graphics_transport_medium medium;
    /** Length-delimited decoded path or object name. */
    const uint8_t *name;
    /** Number of bytes in name. */
    size_t name_length;
    /** Byte offset from the beginning of the external resource. */
    uint64_t offset;
    /** Exact requested byte count, or zero to read through end of resource. */
    uint64_t size;
    /** Maximum payload bytes accepted by request completion. */
    size_t data_capacity;
    /** Whether host policy must authorize deletion after the read attempt. */
    bool delete_after_read;
} boxrig_graphics_transport_view;

/** Host-visible item kinds produced by terminal interpretation. */
typedef enum boxrig_host_item_type {
    /** Audible or visible bell request without a payload. */
    BOXRIG_HOST_ITEM_BELL = 0,
    /** UTF-8 icon and/or window title selected by boxrig_title_target flags. */
    BOXRIG_HOST_ITEM_TITLE,
    /** Untrusted UTF-8 working-directory URI reported by the terminal client. */
    BOXRIG_HOST_ITEM_WORKING_DIRECTORY,
    /** Clipboard write request whose payload contains decoded bytes. */
    BOXRIG_HOST_ITEM_CLIPBOARD_WRITE,
    /** Clipboard read request without a payload. */
    BOXRIG_HOST_ITEM_CLIPBOARD_READ,
    /** Desktop notification display or update request. */
    BOXRIG_HOST_ITEM_NOTIFICATION,
    /** External Kitty graphics data read request. */
    BOXRIG_HOST_ITEM_GRAPHICS_DATA,
    /** Untrusted terminal grid-size request; no completion or reply required. */
    BOXRIG_HOST_ITEM_GRID_RESIZE
} boxrig_host_item_type;

/** Meaning of one axis in a CSI 8 terminal grid-size request. */
typedef enum boxrig_grid_size_mode {
    /** An omitted parameter retains the host's current dimension. */
    BOXRIG_GRID_SIZE_KEEP = 0,
    /** A positive parameter specifies a number of character cells. */
    BOXRIG_GRID_SIZE_EXPLICIT,
    /** A zero parameter requests the display dimension, resolved by the host. */
    BOXRIG_GRID_SIZE_DISPLAY
} boxrig_grid_size_mode;

/** One independent axis of an untrusted terminal grid-size request. */
typedef struct boxrig_grid_size_axis {
    /** Whether to retain, explicitly set, or use the display dimension. */
    boxrig_grid_size_mode mode;
    /** Positive count for EXPLICIT, otherwise zero. */
    uint32_t cells;
} boxrig_grid_size_axis;

/**
 * Requested character-cell dimensions without changing the terminal grid.
 * @remarks The host authorizes the request and resolves KEEP/DISPLAY using
 * its current allocation and display. Explicit counts span 1..UINT32_MAX;
 * the host must validate its own size and resource limits before applying
 * them. CSI 8 has no completion response. Use existing layout revisions to
 * manage native sizing, then resize the core and transport to the actual grid.
 */
typedef struct boxrig_grid_size_request {
    /** Height in character cells. */
    boxrig_grid_size_axis rows;
    /** Width in character cells. */
    boxrig_grid_size_axis columns;
} boxrig_grid_size_request;

/** Occasion on which a desktop notification may be displayed. */
typedef enum boxrig_notification_occasion {
    /** Display regardless of terminal window state. */
    BOXRIG_NOTIFICATION_ALWAYS = 0,
    /** Display only while the terminal window lacks keyboard focus. */
    BOXRIG_NOTIFICATION_UNFOCUSED,
    /** Display only while the terminal window is unfocused and invisible. */
    BOXRIG_NOTIFICATION_INVISIBLE
} boxrig_notification_occasion;

/** Urgency requested for a desktop notification. */
typedef enum boxrig_notification_urgency {
    /** Low urgency. */
    BOXRIG_NOTIFICATION_URGENCY_LOW = 0,
    /** Normal urgency. */
    BOXRIG_NOTIFICATION_URGENCY_NORMAL,
    /** Critical urgency. */
    BOXRIG_NOTIFICATION_URGENCY_CRITICAL
} boxrig_notification_urgency;

/** A read-only length-delimited byte string in a notification. */
typedef struct boxrig_notification_text_view {
    /** UTF-8 bytes, or NULL when length is zero. */
    const uint8_t *bytes;
    /** Number of bytes. */
    size_t length;
} boxrig_notification_text_view;

/** A read-only normalized desktop notification request. */
typedef struct boxrig_notification_view {
    /** Whether the terminal client supplied a nonzero identifier. */
    bool has_identifier;
    /** Validated notification identifier. */
    boxrig_notification_text_view identifier;
    /** Plain-text notification title. */
    boxrig_notification_text_view title;
    /** Plain-text notification body, with line endings normalized to LF. */
    boxrig_notification_text_view body;
    /** Optional application name. */
    boxrig_notification_text_view application_name;
    /** Notification types in input order, or NULL when type_count is zero. */
    const boxrig_notification_text_view *types;
    /** Number of entries in types. */
    size_t type_count;
    /** Requested display occasion. */
    boxrig_notification_occasion occasion;
    /** Requested urgency. */
    boxrig_notification_urgency urgency;
} boxrig_notification_view;

/** Bit flags selecting the title property affected by a title host item. */
typedef enum boxrig_title_target {
    /** Change or stack the icon title. */
    BOXRIG_TITLE_ICON = 1u << 0,
    /** Change or stack the window title. */
    BOXRIG_TITLE_WINDOW = 1u << 1,
    /** Save the selected current title on the host adapter's stack. */
    BOXRIG_TITLE_STACK_PUSH = 1u << 2,
    /** Restore the selected title from the host adapter's stack. */
    BOXRIG_TITLE_STACK_POP = 1u << 3
} boxrig_title_target;

/** Bit flags identifying OSC 52 clipboard selections. */
typedef enum boxrig_clipboard_selection {
    BOXRIG_CLIPBOARD_SELECTION_CLIPBOARD = 1u << 0,
    BOXRIG_CLIPBOARD_SELECTION_PRIMARY = 1u << 1,
    BOXRIG_CLIPBOARD_SELECTION_SECONDARY = 1u << 2,
    BOXRIG_CLIPBOARD_SELECTION_SELECT = 1u << 3,
    BOXRIG_CLIPBOARD_SELECTION_CUT_BUFFER_0 = 1u << 4,
    BOXRIG_CLIPBOARD_SELECTION_CUT_BUFFER_1 = 1u << 5,
    BOXRIG_CLIPBOARD_SELECTION_CUT_BUFFER_2 = 1u << 6,
    BOXRIG_CLIPBOARD_SELECTION_CUT_BUFFER_3 = 1u << 7,
    BOXRIG_CLIPBOARD_SELECTION_CUT_BUFFER_4 = 1u << 8,
    BOXRIG_CLIPBOARD_SELECTION_CUT_BUFFER_5 = 1u << 9,
    BOXRIG_CLIPBOARD_SELECTION_CUT_BUFFER_6 = 1u << 10,
    BOXRIG_CLIPBOARD_SELECTION_CUT_BUFFER_7 = 1u << 11
} boxrig_clipboard_selection;

/** Host result supplied for an asynchronous terminal request. */
typedef enum boxrig_host_request_result {
    BOXRIG_HOST_REQUEST_SUCCESS = 0,
    BOXRIG_HOST_REQUEST_DENIED,
    BOXRIG_HOST_REQUEST_UNAVAILABLE
} boxrig_host_request_result;

/** A read-only view of one queued host event or request. */
typedef struct boxrig_host_item {
    /** Event or request kind. */
    boxrig_host_item_type type;
    /** Nonzero identifier for a request requiring completion, otherwise zero. */
    uint64_t request_id;
    /** Item-specific flags, such as boxrig_title_target bits. */
    uint32_t flags;
    /** Length-delimited payload, or NULL when payload_length is zero. */
    const uint8_t *payload;
    /** Number of bytes in payload. */
    size_t payload_length;
    /**
     * Typed notification data for BOXRIG_HOST_ITEM_NOTIFICATION, otherwise
     * NULL. The view remains valid until the terminal is next modified.
     */
    const boxrig_notification_view *notification;
    /**
     * Typed request data for BOXRIG_HOST_ITEM_GRAPHICS_DATA, otherwise NULL.
     * The view remains valid until the terminal is next modified.
     */
    const boxrig_graphics_transport_view *graphics_transport;
    /**
     * Inline request for GRID_RESIZE, otherwise zero-initialized. Copying
     * this value retains both axes independently of the queued item's lifetime.
     */
    boxrig_grid_size_request grid_resize;
} boxrig_host_item;

/** Logical key identifiers accepted by the input encoder. */
typedef enum boxrig_key {
    BOXRIG_KEY_NONE = 0,
    BOXRIG_KEY_ENTER,
    BOXRIG_KEY_BACKSPACE,
    BOXRIG_KEY_TAB,
    BOXRIG_KEY_ESCAPE,
    BOXRIG_KEY_UP,
    BOXRIG_KEY_DOWN,
    BOXRIG_KEY_RIGHT,
    BOXRIG_KEY_LEFT,
    BOXRIG_KEY_HOME,
    BOXRIG_KEY_END,
    BOXRIG_KEY_INSERT,
    BOXRIG_KEY_DELETE,
    BOXRIG_KEY_PAGE_UP,
    BOXRIG_KEY_PAGE_DOWN,
    BOXRIG_KEY_F1,
    BOXRIG_KEY_F2,
    BOXRIG_KEY_F3,
    BOXRIG_KEY_F4,
    BOXRIG_KEY_F5,
    BOXRIG_KEY_F6,
    BOXRIG_KEY_F7,
    BOXRIG_KEY_F8,
    BOXRIG_KEY_F9,
    BOXRIG_KEY_F10,
    BOXRIG_KEY_F11,
    BOXRIG_KEY_F12,
    BOXRIG_KEY_F13,
    BOXRIG_KEY_F14,
    BOXRIG_KEY_F15,
    BOXRIG_KEY_F16,
    BOXRIG_KEY_F17,
    BOXRIG_KEY_F18,
    BOXRIG_KEY_F19,
    BOXRIG_KEY_F20,
    BOXRIG_KEY_F21,
    BOXRIG_KEY_F22,
    BOXRIG_KEY_F23,
    BOXRIG_KEY_F24,
    BOXRIG_KEY_F25,
    BOXRIG_KEY_F26,
    BOXRIG_KEY_F27,
    BOXRIG_KEY_F28,
    BOXRIG_KEY_F29,
    BOXRIG_KEY_F30,
    BOXRIG_KEY_F31,
    BOXRIG_KEY_F32,
    BOXRIG_KEY_F33,
    BOXRIG_KEY_F34,
    BOXRIG_KEY_F35,
    BOXRIG_KEY_CAPS_LOCK,
    BOXRIG_KEY_SCROLL_LOCK,
    BOXRIG_KEY_NUM_LOCK,
    BOXRIG_KEY_PRINT_SCREEN,
    BOXRIG_KEY_PAUSE,
    BOXRIG_KEY_MENU,
    BOXRIG_KEY_KP_0,
    BOXRIG_KEY_KP_1,
    BOXRIG_KEY_KP_2,
    BOXRIG_KEY_KP_3,
    BOXRIG_KEY_KP_4,
    BOXRIG_KEY_KP_5,
    BOXRIG_KEY_KP_6,
    BOXRIG_KEY_KP_7,
    BOXRIG_KEY_KP_8,
    BOXRIG_KEY_KP_9,
    BOXRIG_KEY_KP_DECIMAL,
    BOXRIG_KEY_KP_DIVIDE,
    BOXRIG_KEY_KP_MULTIPLY,
    BOXRIG_KEY_KP_SUBTRACT,
    BOXRIG_KEY_KP_ADD,
    BOXRIG_KEY_KP_ENTER,
    BOXRIG_KEY_KP_EQUAL,
    BOXRIG_KEY_KP_SEPARATOR,
    BOXRIG_KEY_KP_LEFT,
    BOXRIG_KEY_KP_RIGHT,
    BOXRIG_KEY_KP_UP,
    BOXRIG_KEY_KP_DOWN,
    BOXRIG_KEY_KP_PAGE_UP,
    BOXRIG_KEY_KP_PAGE_DOWN,
    BOXRIG_KEY_KP_HOME,
    BOXRIG_KEY_KP_END,
    BOXRIG_KEY_KP_INSERT,
    BOXRIG_KEY_KP_DELETE,
    BOXRIG_KEY_KP_BEGIN,
    BOXRIG_KEY_MEDIA_PLAY,
    BOXRIG_KEY_MEDIA_PAUSE,
    BOXRIG_KEY_MEDIA_PLAY_PAUSE,
    BOXRIG_KEY_MEDIA_REVERSE,
    BOXRIG_KEY_MEDIA_STOP,
    BOXRIG_KEY_MEDIA_FAST_FORWARD,
    BOXRIG_KEY_MEDIA_REWIND,
    BOXRIG_KEY_MEDIA_TRACK_NEXT,
    BOXRIG_KEY_MEDIA_TRACK_PREVIOUS,
    BOXRIG_KEY_MEDIA_RECORD,
    BOXRIG_KEY_LOWER_VOLUME,
    BOXRIG_KEY_RAISE_VOLUME,
    BOXRIG_KEY_MUTE_VOLUME,
    BOXRIG_KEY_LEFT_SHIFT,
    BOXRIG_KEY_LEFT_CONTROL,
    BOXRIG_KEY_LEFT_ALT,
    BOXRIG_KEY_LEFT_SUPER,
    BOXRIG_KEY_LEFT_HYPER,
    BOXRIG_KEY_LEFT_META,
    BOXRIG_KEY_RIGHT_SHIFT,
    BOXRIG_KEY_RIGHT_CONTROL,
    BOXRIG_KEY_RIGHT_ALT,
    BOXRIG_KEY_RIGHT_SUPER,
    BOXRIG_KEY_RIGHT_HYPER,
    BOXRIG_KEY_RIGHT_META,
    BOXRIG_KEY_ISO_LEVEL3_SHIFT,
    BOXRIG_KEY_ISO_LEVEL5_SHIFT
} boxrig_key;

/** Logical modifier bits accepted by the input encoder. */
typedef enum boxrig_modifier {
    BOXRIG_MODIFIER_SHIFT = 1u << 0,
    BOXRIG_MODIFIER_ALT = 1u << 1,
    BOXRIG_MODIFIER_CONTROL = 1u << 2,
    BOXRIG_MODIFIER_SUPER = 1u << 3,
    BOXRIG_MODIFIER_HYPER = 1u << 4,
    BOXRIG_MODIFIER_META = 1u << 5,
    BOXRIG_MODIFIER_CAPS_LOCK = 1u << 6,
    BOXRIG_MODIFIER_NUM_LOCK = 1u << 7
} boxrig_modifier;

/**
 * Length-delimited byte sequence for one legacy key and modifier combination.
 *
 * @remarks Kitty-generated events take precedence. Otherwise press and repeat
 * use the exact configured bytes, overriding application cursor mode without
 * adding Alt prefixes or applying a text codec. Legacy keypad navigation
 * aliases use the corresponding cursor-key mapping. Unmatched combinations
 * retain their standard encoding; legacy release sends nothing. Duplicate
 * key/modifier pairs and unsupported keys or modifier bits are rejected.
 */
struct boxrig_key_mapping {
    /** One of BOXRIG_KEY_UP, DOWN, RIGHT or LEFT. Enter uses enter_newline. */
    boxrig_key key;
    /** Exact SHIFT/ALT/CONTROL/SUPER/HYPER/META combination; lock bits forbidden. */
    uint32_t modifiers;
    /** Completed output bytes; NULL is permitted exactly when length is zero. */
    const uint8_t *bytes;
    /** Byte length; zero intentionally suppresses this legacy key event. */
    size_t length;
};

/**
 * Returns default runtime input settings, also used by terminal construction.
 * @return Terminal-mode Enter, Auto erase bindings, and no arrow overrides.
 */
boxrig_input_config boxrig_input_config_default(void);

/**
 * Atomically validates, copies, and replaces runtime input settings.
 * @param terminal Terminal to configure.
 * @param config Complete replacement; mapping payloads are copied.
 * @return OK, INVALID_ARGUMENT, LIMIT_EXCEEDED, or OUT_OF_MEMORY.
 * @remarks Failure preserves the old settings. Success preserves screen,
 * history, selection, protocol modes, and already queued output. Settings
 * survive RIS/DECSTR. Kitty encoding precedes all legacy settings. For a
 * one-byte BS/DEL binding Ctrl exchanges BS and DEL and Alt prefixes ESC;
 * DELETE_SEQUENCE encodes all modifiers in CSI without an extra Alt prefix.
 * Legacy releases produce no bytes. Core applies changes at the call boundary;
 * a frontend owner must use its guarded setter to avoid changing a held-key or
 * pending IME series. Calls and borrowed settings must be externally serialized.
 */
boxrig_status boxrig_terminal_set_input_config(
    boxrig_terminal *terminal, const boxrig_input_config *config);

/**
 * Reads all current settings without allocating.
 * @param terminal Terminal to inspect.
 * @param config_out Required output; mapping pointers are borrowed until the
 *        next successful exchange or terminal destruction.
 * @return OK or INVALID_ARGUMENT.
 * @remarks The returned value may itself be passed to set_input_config(); all
 * replacement data is copied before any old storage is released.
 */
boxrig_status boxrig_terminal_get_input_config(
    const boxrig_terminal *terminal, boxrig_input_config *config_out);

/**
 * Replaces erase information for the current communication channel.
 * @param terminal Terminal to update.
 * @param erase Current value; an unavailable value discards any previous byte.
 * @return OK or INVALID_ARGUMENT for a NULL terminal.
 * @remarks Initially unavailable. The channel owner updates this immediately
 * before sending a TTY-bound erase key, and clears it on disconnect or channel
 * replacement. Core performs no OS query. NUL and disabled values are sent as
 * bytes. Ctrl exchanges only BS/DEL; other bytes are unchanged. Alt prefixes ESC.
 * This information is independent of input settings, survives screen resets,
 * and may be refreshed during a held-key series. Calls must be serialized.
 */
boxrig_status boxrig_terminal_set_tty_erase(
    boxrig_terminal *terminal, boxrig_tty_erase erase);

/**
 * Reads the last erase information supplied by the communication channel owner.
 * @param terminal Terminal to inspect.
 * @param erase_out Required output; byte is zero when unavailable.
 * @return OK or INVALID_ARGUMENT for a NULL argument.
 * @remarks This is a snapshot, not an OS query. Unknown TTY bindings use the
 * corresponding key's AUTO behavior, including on Win32 or remote transports.
 */
boxrig_status boxrig_terminal_get_tty_erase(
    const boxrig_terminal *terminal, boxrig_tty_erase *erase_out);

/** Logical key transition type. */
typedef enum boxrig_key_action {
    BOXRIG_KEY_PRESS = 0,
    BOXRIG_KEY_REPEAT,
    BOXRIG_KEY_RELEASE
} boxrig_key_action;

/** A backend-independent logical key event. */
typedef struct boxrig_key_event {
    /** Logical non-text key, or BOXRIG_KEY_NONE for a text key. */
    boxrig_key key;
    /**
     * Active-layout unshifted Unicode key identity before Control mapping,
     * or zero when unavailable.
     */
    uint32_t codepoint;
    /**
     * Active-layout shifted Unicode key identity, or zero when unavailable
     * or identical to codepoint.
     */
    uint32_t shifted_codepoint;
    /**
     * Unshifted PC-101 base-layout Unicode key identity supplied by the
     * native keymap, or zero when unavailable or ambiguous.
     */
    uint32_t base_layout_codepoint;
    /** Key transition type. */
    boxrig_key_action action;
    /** Bitwise combination of boxrig_modifier values. */
    uint32_t modifiers;
    /**
     * Length-delimited UTF-8 text generated by the OS or IME. The text may
     * be empty or contain C0 or DEL control code points.
     */
    const char *text;
    /** Number of bytes in text. */
    size_t text_length;
} boxrig_key_event;

/** Phase of a streamed paste transaction. */
typedef enum boxrig_paste_phase {
    BOXRIG_PASTE_BEGIN = 0,
    BOXRIG_PASTE_CHUNK,
    BOXRIG_PASTE_END
} boxrig_paste_phase;

/** One phase of a backend-independent paste transaction. */
typedef struct boxrig_paste_event {
    /** Transaction phase. */
    boxrig_paste_phase phase;
    /** Length-delimited normalized UTF-8 text for a chunk. */
    const char *text;
    /** Number of bytes in text. */
    size_t text_length;
} boxrig_paste_event;

/** A backend-independent focus transition. */
typedef struct boxrig_focus_event {
    /** True when the terminal view gained keyboard focus. */
    bool focused;
} boxrig_focus_event;

/** Logical mouse buttons accepted by the input encoder. */
typedef enum boxrig_mouse_button {
    BOXRIG_MOUSE_BUTTON_NONE = 0,
    BOXRIG_MOUSE_BUTTON_LEFT,
    BOXRIG_MOUSE_BUTTON_MIDDLE,
    BOXRIG_MOUSE_BUTTON_RIGHT,
    BOXRIG_MOUSE_BUTTON_WHEEL_UP,
    BOXRIG_MOUSE_BUTTON_WHEEL_DOWN,
    BOXRIG_MOUSE_BUTTON_WHEEL_LEFT,
    BOXRIG_MOUSE_BUTTON_WHEEL_RIGHT,
    BOXRIG_MOUSE_BUTTON_8,
    BOXRIG_MOUSE_BUTTON_9,
    BOXRIG_MOUSE_BUTTON_10,
    BOXRIG_MOUSE_BUTTON_11
} boxrig_mouse_button;

/** Bit flags describing mouse buttons held during an event. */
typedef enum boxrig_mouse_button_mask {
    BOXRIG_MOUSE_BUTTON_MASK_LEFT = 1u << 0,
    BOXRIG_MOUSE_BUTTON_MASK_MIDDLE = 1u << 1,
    BOXRIG_MOUSE_BUTTON_MASK_RIGHT = 1u << 2,
    BOXRIG_MOUSE_BUTTON_MASK_8 = 1u << 3,
    BOXRIG_MOUSE_BUTTON_MASK_9 = 1u << 4,
    BOXRIG_MOUSE_BUTTON_MASK_10 = 1u << 5,
    BOXRIG_MOUSE_BUTTON_MASK_11 = 1u << 6
} boxrig_mouse_button_mask;

/** Logical mouse transition type. */
typedef enum boxrig_mouse_action {
    BOXRIG_MOUSE_PRESS = 0,
    BOXRIG_MOUSE_RELEASE,
    BOXRIG_MOUSE_MOTION
} boxrig_mouse_action;

/** A backend-independent mouse event in content-local physical pixels and cells. */
typedef struct boxrig_mouse_event {
    /** Mouse transition type. */
    boxrig_mouse_action action;
    /** Button changed by press/release, or BOXRIG_MOUSE_BUTTON_NONE. */
    boxrig_mouse_button button;
    /** Bitwise combination of boxrig_mouse_button_mask values held afterward. */
    uint32_t pressed_buttons;
    /** Bitwise combination of boxrig_modifier values. */
    uint32_t modifiers;
    /** Signed horizontal physical pixel coordinate from the content's left edge.
     * Native subpixels are rounded toward negative infinity after scaling. */
    int64_t x;
    /** Signed vertical physical pixel coordinate from the content's top edge.
     * Coordinates outside the content are retained, including negative values. */
    int64_t y;
    /** Zero-based terminal row. */
    size_t row;
    /** Zero-based terminal column. */
    size_t column;
    /** Consecutive native click count for a non-wheel press, otherwise zero. */
    uint8_t click_count;
} boxrig_mouse_event;

/**
 * Returns a configuration containing conservative defaults.
 *
 * @return A configuration for an 80 by 24 terminal.
 */
boxrig_terminal_config boxrig_terminal_config_default(void);

/**
 * Creates a terminal emulator.
 *
 * @param config Initial dimensions and queue limits.
 * @param terminal_out Receives the newly allocated terminal.
 * @return BOXRIG_STATUS_OK on success, or an error status.
 */
boxrig_status boxrig_terminal_create(
    const boxrig_terminal_config *config,
    boxrig_terminal **terminal_out);

/**
 * Destroys a terminal emulator.
 *
 * @param terminal Terminal to destroy. NULL is accepted.
 */
void boxrig_terminal_destroy(boxrig_terminal *terminal);

/**
 * Returns the immutable ambiguous-width policy.
 * @param terminal Terminal to inspect, or NULL for the narrow default.
 * @return The configured ambiguous-width policy.
 */
boxrig_ambiguous_width boxrig_terminal_ambiguous_width(const boxrig_terminal *terminal);

/**
 * Installs a font measurement callback and reflows text when column widths change.
 * @param terminal Terminal to update.
 * @param resolver Callback, or NULL to detach without changing existing text.
 * @param context Borrowed context, valid until the callback is replaced or
 * detached. Must be NULL when resolver is NULL.
 * @return OK, INVALID_ARGUMENT, OUT_OF_MEMORY, or LIMIT_EXCEEDED.
 * @remarks Reinstall after font metrics change. Detach before destroying the
 * context. Without a callback, new ambiguous text uses one column. Explicit
 * narrow/wide policies do not invoke the callback. If all occupied column widths
 * remain equal, existing viewport, cursor, and image positions are preserved.
 */
boxrig_status boxrig_terminal_set_font_width_resolver(boxrig_terminal *terminal,
    boxrig_font_width_resolver resolver, void *context);

/**
 * Measures the first terminal text cluster in a UTF-8 buffer.
 *
 * @param terminal Terminal whose text-width policy and font resolver are used.
 * @param text Nonempty length-delimited UTF-8 input.
 * @param text_length Number of bytes available in text.
 * @param measurement_out Receives one complete measurement on success.
 * @return BOXRIG_STATUS_OK, BOXRIG_STATUS_INVALID_ARGUMENT, or
 * BOXRIG_STATUS_LIMIT_EXCEEDED when the font advance exceeds 255 columns.
 *
 * @remarks The returned byte length is always nonzero and the intrinsic
 * column width is one or two, or up to 255 in font-dependent mode. A cluster longer than
 * BOXRIG_TEXT_CLUSTER_CAPACITY_MAX is split at a scalar boundary and reported
 * through split_by_limit. Malformed or truncated UTF-8 leaves measurement_out
 * unchanged. The core does not allocate or mutate terminal state; a registered
 * font resolver may allocate temporary font-engine resources.
 */
boxrig_status boxrig_terminal_measure_text_cluster(
    const boxrig_terminal *terminal,
    const uint8_t *text,
    size_t text_length,
    boxrig_text_cluster_measurement *measurement_out);

/**
 * Completes one input stream, replacing incomplete text and cancelling syntax.
 * @param terminal Terminal receiving the stream-end notification.
 * @return OK on completion, or a retryable/error status without discarding input.
 * @remarks Repeated successful calls have no effect. A subsequent feed starts
 * a fresh input interval. Screen, modes, settings and outbound order survive.
 */
boxrig_status boxrig_terminal_finish_input(boxrig_terminal *terminal);

/**
 * Interprets bytes received from a terminal client.
 *
 * @param terminal Terminal to mutate.
 * @param bytes Input byte sequence.
 * @param length Number of input bytes.
 * @param consumed_out Receives the number of consumed bytes.
 * @return BOXRIG_STATUS_OK when all bytes were consumed, or an error status.
 *
 * @remarks Input may end at any parser state and can be resumed by a later call.
 */
boxrig_status boxrig_terminal_feed(
    boxrig_terminal *terminal,
    const uint8_t *bytes,
    size_t length,
    size_t *consumed_out);

/**
 * Changes the terminal grid size and reflows primary-screen logical lines.
 *
 * @param terminal Terminal to resize.
 * @param rows New nonzero row count.
 * @param columns New nonzero column count.
 * @return BOXRIG_STATUS_OK on success, or an error status.
 *
 * @remarks Primary scrollback and visible rows are joined using soft-wrap
 * metadata before reflow. The alternate screen preserves its intersecting
 * cells without splitting wide cells. A primary viewport at live output stays
 * live. A viewport in history keeps its logical top anchor when retained and
 * clamps to the oldest retained fragment only when reflow evicts that anchor.
 */
boxrig_status boxrig_terminal_resize(
    boxrig_terminal *terminal,
    size_t rows,
    size_t columns);

/**
 * Reads the current terminal dimensions.
 *
 * @param terminal Terminal to inspect.
 * @param rows_out Receives the row count.
 * @param columns_out Receives the column count.
 */
void boxrig_terminal_size(
    const boxrig_terminal *terminal,
    size_t *rows_out,
    size_t *columns_out);

/**
 * Reads the number of primary-screen rows retained in scrollback.
 *
 * @param terminal Terminal to inspect.
 * @return Retained physical row count, or zero for NULL.
 */
size_t boxrig_terminal_scrollback_rows(const boxrig_terminal *terminal);

/**
 * Reads the configured primary-screen scrollback capacity.
 *
 * @param terminal Terminal to inspect.
 * @return Capacity in physical rows, or zero for NULL.
 */
size_t boxrig_terminal_scrollback_capacity(
    const boxrig_terminal *terminal);

/**
 * Changes the primary-screen scrollback capacity transactionally.
 *
 * @param terminal Terminal to mutate.
 * @param row_capacity New physical-row capacity from zero through
 *        BOXRIG_SCROLLBACK_CAPACITY_MAX.
 * @return BOXRIG_STATUS_OK, BOXRIG_STATUS_INVALID_ARGUMENT for NULL,
 *         BOXRIG_STATUS_LIMIT_EXCEEDED above the hard limit, or
 *         BOXRIG_STATUS_OUT_OF_MEMORY when replacement storage cannot be
 *         prepared.
 *
 * @remarks Increasing the capacity retains all history. Decreasing it
 * immediately evicts the oldest rows, and zero clears history. Failure leaves
 * capacity, history, viewport, image placements, revisions, and dirty
 * unchanged. Setting the current value is a no-op.
 */
boxrig_status boxrig_terminal_set_scrollback_capacity(
    boxrig_terminal *terminal,
    size_t row_capacity);

/**
 * Returns the revision of projected text and retained logical-line topology.
 *
 * @param terminal Terminal to inspect.
 * @return A nonzero revision, or zero for NULL.
 *
 * @remarks Style, cursor, image, viewport, and active-buffer-only changes do
 * not advance this revision. Reflow preserves it when projected logical text
 * and retained identities are unchanged.
 */
uint64_t boxrig_terminal_text_revision(const boxrig_terminal *terminal);

/**
 * Returns the physical fragment count in one logical text document.
 *
 * @param terminal Terminal to inspect.
 * @param screen Primary or alternate namespace.
 * @return Primary history plus visible rows, alternate rows, or zero for an
 *         invalid argument.
 */
size_t boxrig_terminal_text_fragment_count(
    const boxrig_terminal *terminal,
    boxrig_text_screen screen);

/**
 * Reads metadata for one physical logical-text fragment.
 *
 * @param terminal Terminal to inspect.
 * @param screen Primary or alternate namespace.
 * @param fragment_index Zero-based document-order fragment index.
 * @param fragment_out Receives fragment metadata.
 * @return BOXRIG_STATUS_OK or BOXRIG_STATUS_INVALID_ARGUMENT.
 */
boxrig_status boxrig_terminal_get_text_fragment(
    const boxrig_terminal *terminal,
    boxrig_text_screen screen,
    size_t fragment_index,
    boxrig_text_fragment_view *fragment_out);

/**
 * Resolves a terminal-column boundary in one physical fragment.
 *
 * @param terminal Terminal to inspect.
 * @param screen Primary or alternate namespace.
 * @param fragment_index Zero-based document-order fragment index.
 * @param column_boundary Boundary from zero through the grid column count.
 * @param position_out Receives a stable logical position.
 * @return BOXRIG_STATUS_OK, or BOXRIG_STATUS_INVALID_ARGUMENT when the
 *         boundary is outside the fragment or inside a wide grapheme.
 */
boxrig_status boxrig_terminal_get_text_position(
    const boxrig_terminal *terminal,
    boxrig_text_screen screen,
    size_t fragment_index,
    size_t column_boundary,
    boxrig_text_position *position_out);

/**
 * Rebases a retained range to the current terminal text revision.
 *
 * @param terminal Terminal to inspect.
 * @param range Range created by this terminal.
 * @param range_out Receives the current equivalent range.
 * @return BOXRIG_STATUS_OK, BOXRIG_STATUS_INVALID_ARGUMENT, or
 *         BOXRIG_STATUS_STALE when referenced text changed or was retired.
 */
boxrig_status boxrig_terminal_rebase_text_range(
    const boxrig_terminal *terminal,
    const boxrig_text_range *range,
    boxrig_text_range *range_out);

/**
 * Creates an immutable owned projection of a document or range.
 *
 * @param terminal Terminal to inspect.
 * @param options Source namespace, optional range, and strict capacities.
 * @param snapshot_out Receives the snapshot and is set to NULL on failure.
 * @return BOXRIG_STATUS_OK, BOXRIG_STATUS_INVALID_ARGUMENT,
 *         BOXRIG_STATUS_STALE, BOXRIG_STATUS_LIMIT_EXCEEDED without
 *         truncation, or BOXRIG_STATUS_OUT_OF_MEMORY.
 */
boxrig_status boxrig_terminal_create_text_snapshot(
    const boxrig_terminal *terminal,
    const boxrig_text_snapshot_options *options,
    boxrig_text_snapshot **snapshot_out);

/**
 * Creates an immutable owned projection of explicit logical row ranges.
 *
 * @param terminal Terminal to inspect.
 * @param options Source ranges, output row count, and strict byte capacity.
 * @param snapshot_out Receives the snapshot and is set to NULL on failure.
 * @return BOXRIG_STATUS_OK, BOXRIG_STATUS_INVALID_ARGUMENT,
 *         BOXRIG_STATUS_STALE, BOXRIG_STATUS_LIMIT_EXCEEDED without
 *         truncation, or BOXRIG_STATUS_OUT_OF_MEMORY.
 *
 * @remarks Every output row must contain at least one range; a zero-length
 * range represents an empty output row at a current grapheme boundary. Ranges within a
 * row must name one current logical line in document order. Empty terminal
 * cells project as ASCII spaces, wide graphemes project once, one LF is
 * inserted between output rows, and no final LF is appended. The complete
 * snapshot storage is obtained with one allocation after exact measurement.
 */
boxrig_status boxrig_terminal_create_text_snapshot_rows(
    const boxrig_terminal *terminal,
    const boxrig_text_snapshot_rows_options *options,
    boxrig_text_snapshot **snapshot_out);

/**
 * Destroys an owned logical text snapshot.
 *
 * @param snapshot Snapshot to destroy. NULL is accepted.
 */
void boxrig_text_snapshot_destroy(boxrig_text_snapshot *snapshot);

/**
 * Reads immutable projected UTF-8 bytes.
 *
 * @param snapshot Snapshot to inspect.
 * @param length_out Receives the byte length.
 * @return Owned bytes valid until snapshot destruction, or NULL for an empty
 *         or invalid snapshot.
 */
const uint8_t *boxrig_text_snapshot_data(
    const boxrig_text_snapshot *snapshot,
    size_t *length_out);

/**
 * Reads the number of projected logical lines.
 *
 * @param snapshot Snapshot to inspect.
 * @return Logical line count, or zero for NULL.
 */
size_t boxrig_text_snapshot_line_count(
    const boxrig_text_snapshot *snapshot);

/**
 * Reads the terminal text revision captured by a snapshot.
 *
 * @param snapshot Snapshot to inspect.
 * @return Captured revision, or zero for NULL.
 */
uint64_t boxrig_text_snapshot_revision(
    const boxrig_text_snapshot *snapshot);

/**
 * Resolves a projected UTF-8 byte boundary to its logical position.
 *
 * @param snapshot Snapshot to inspect.
 * @param byte_offset Boundary from zero through the snapshot byte length.
 * @param position_out Receives the corresponding position.
 * @return BOXRIG_STATUS_OK or BOXRIG_STATUS_INVALID_ARGUMENT when the offset
 *         is not a grapheme or hard-line boundary.
 */
boxrig_status boxrig_text_snapshot_position_at_byte_offset(
    const boxrig_text_snapshot *snapshot,
    size_t byte_offset,
    boxrig_text_position *position_out);

/**
 * Resolves a captured logical position to its projected UTF-8 byte boundary.
 *
 * @param snapshot Snapshot to inspect.
 * @param position Captured position to resolve.
 * @param byte_offset_out Receives the byte offset.
 * @return BOXRIG_STATUS_OK or BOXRIG_STATUS_INVALID_ARGUMENT when the
 *         position is outside this snapshot.
 */
boxrig_status boxrig_text_snapshot_byte_offset_at_position(
    const boxrig_text_snapshot *snapshot,
    const boxrig_text_position *position,
    size_t *byte_offset_out);

/**
 * Creates a bounded resumable byte-exact literal search.
 *
 * @param terminal Terminal whose logical document remains borrowed by the
 *        cursor.
 * @param options Scope, literal pattern, traversal, start, and wrap policy.
 * @param cursor_out Receives the cursor and is set to NULL on failure.
 * @return BOXRIG_STATUS_OK, BOXRIG_STATUS_INVALID_ARGUMENT,
 *         BOXRIG_STATUS_STALE for an obsolete start,
 *         BOXRIG_STATUS_LIMIT_EXCEEDED, or BOXRIG_STATUS_OUT_OF_MEMORY.
 *
 * @remarks The cursor owns a copy of pattern and state proportional to its
 * length, not a document snapshot. Pattern matching is UTF-8 byte exact, but
 * both match ends must coincide with projected grapheme or hard-line
 * boundaries. Soft wraps contribute no bytes and hard breaks contribute LF.
 * The terminal must outlive the cursor.
 */
boxrig_status boxrig_terminal_create_text_search(
    boxrig_terminal *terminal,
    const boxrig_text_search_options *options,
    boxrig_text_search_cursor **cursor_out);

/**
 * Advances one literal search by an integer work budget.
 *
 * @param cursor Cursor to advance.
 * @param work_budget Positive maximum number of terminal cells inspected plus
 *        projected UTF-8 bytes compared by this dispatch.
 * @param work_used_out Receives work no greater than work_budget.
 * @param progress_out Receives in-progress, match, or not-found.
 * @param result_out Receives a match and is cleared for other progress values.
 * @return BOXRIG_STATUS_OK, BOXRIG_STATUS_INVALID_ARGUMENT,
 *         BOXRIG_STATUS_STALE if projected text changed between dispatches,
 *         or BOXRIG_STATUS_LIMIT_EXCEEDED when an identity cannot be assigned.
 *
 * @remarks A text-preserving resize remaps the physical iterator within the
 * same integer budget. A completed cursor cannot be resumed. Backward search
 * streams forward internally and retains only the last eligible match, so its
 * result is identical to a complete reverse traversal without unbounded state.
 */
boxrig_status boxrig_text_search_resume(
    boxrig_text_search_cursor *cursor,
    size_t work_budget,
    size_t *work_used_out,
    boxrig_text_search_progress *progress_out,
    boxrig_text_search_result *result_out);

/**
 * Destroys one literal search cursor.
 *
 * @param cursor Cursor to destroy. NULL is accepted.
 */
void boxrig_text_search_destroy(boxrig_text_search_cursor *cursor);

/**
 * Registers one bounded byte-exact literal text monitor.
 *
 * @param terminal Terminal that owns the monitor and its event queue.
 * @param options Scope, baseline policy, and literal pattern to copy.
 * @param monitor_id_out Receives a nonzero identity and is cleared on failure.
 * @return BOXRIG_STATUS_OK, BOXRIG_STATUS_INVALID_ARGUMENT,
 *         BOXRIG_STATUS_LIMIT_EXCEEDED without partial registration, or
 *         BOXRIG_STATUS_OUT_OF_MEMORY.
 *
 * @remarks Match ends must coincide with the common logical projection's
 * grapheme or hard-line boundaries. Existing-and-future registration first
 * takes bounded immutable snapshots; application-visible events remain hidden
 * until their budgeted scan completes. The terminal is single-threaded and
 * does not invoke an application callback.
 */
boxrig_status boxrig_terminal_register_text_monitor(
    boxrig_terminal *terminal,
    const boxrig_text_monitor_options *options,
    uint64_t *monitor_id_out);

/**
 * Unregisters a text monitor and removes all of its unconsumed events.
 *
 * @param terminal Terminal that owns the monitor.
 * @param monitor_id Existing monitor identity.
 * @return BOXRIG_STATUS_OK or BOXRIG_STATUS_INVALID_ARGUMENT.
 */
boxrig_status boxrig_terminal_unregister_text_monitor(
    boxrig_terminal *terminal,
    uint64_t monitor_id);

/**
 * Reads the number of registered text monitors.
 *
 * @param terminal Terminal to inspect.
 * @return Current monitor count, or zero for NULL.
 */
size_t boxrig_terminal_text_monitor_count(
    const boxrig_terminal *terminal);

/**
 * Advances immutable existing-match scans within an integer work budget.
 *
 * @param terminal Terminal whose initializing monitors are advanced.
 * @param work_budget Positive maximum projected UTF-8 bytes inspected.
 * @param work_used_out Receives work no greater than work_budget.
 * @param more_work_out Receives whether another dispatch is required.
 * @return BOXRIG_STATUS_OK or BOXRIG_STATUS_INVALID_ARGUMENT.
 *
 * @remarks Completed baselines are published in primary then alternate
 * document order, followed by future events staged during initialization.
 */
boxrig_status boxrig_terminal_dispatch_text_monitors(
    boxrig_terminal *terminal,
    size_t work_budget,
    size_t *work_used_out,
    bool *more_work_out);

/**
 * Copies the oldest application-visible text-monitor event without removing
 * it.
 *
 * @param terminal Terminal to inspect.
 * @param event_out Receives the oldest event when true is returned.
 * @return True when an event was copied.
 */
bool boxrig_terminal_text_monitor_event_peek(
    const boxrig_terminal *terminal,
    boxrig_text_monitor_event *event_out);

/**
 * Removes the event returned by boxrig_terminal_text_monitor_event_peek().
 *
 * @param terminal Terminal whose oldest event is removed.
 * @return BOXRIG_STATUS_OK or BOXRIG_STATUS_INVALID_ARGUMENT when empty.
 */
boxrig_status boxrig_terminal_text_monitor_event_consume(
    boxrig_terminal *terminal);

/**
 * Tests whether bounded bookkeeping for one monitor requires a current scan.
 *
 * @param terminal Terminal to inspect.
 * @param monitor_id Monitor identity to inspect.
 * @return True only for a registered monitor with sticky overflow.
 */
bool boxrig_terminal_text_monitor_needs_rescan(
    const boxrig_terminal *terminal,
    uint64_t monitor_id);

/**
 * Begins a bounded immutable current rescan for a monitor with sticky loss.
 *
 * @param terminal Terminal that owns the monitor.
 * @param monitor_id Monitor identity whose current match set is requested.
 * @return BOXRIG_STATUS_OK, BOXRIG_STATUS_INVALID_ARGUMENT when no rescan is
 *         pending, BOXRIG_STATUS_LIMIT_EXCEEDED without partial recovery, or
 *         BOXRIG_STATUS_OUT_OF_MEMORY.
 *
 * @remarks A successful call removes that monitor's queued loss notification
 * and initializes a current baseline. Events lost after appearing and then
 * disappearing cannot be reconstructed.
 */
boxrig_status boxrig_terminal_rescan_text_monitor(
    boxrig_terminal *terminal,
    uint64_t monitor_id);

/**
 * Returns the documented finite defaults for one text recognizer.
 *
 * @return Positive compiler, scan, result, and baseline capacities.
 */
boxrig_text_recognizer_limits boxrig_text_recognizer_limits_default(void);

/**
 * Compiles and registers one bounded regular-expression text recognizer.
 *
 * @param terminal Terminal that owns the recognizer and its results.
 * @param options Scope, copied pattern, and strict resource limits.
 * @param diagnostic_out Receives a compile result and byte offset.
 * @param recognizer_id_out Receives a nonzero identity and is cleared on
 *        failure.
 * @return BOXRIG_STATUS_OK, BOXRIG_STATUS_INVALID_ARGUMENT,
 *         BOXRIG_STATUS_LIMIT_EXCEEDED without a partial registration, or
 *         BOXRIG_STATUS_OUT_OF_MEMORY.
 *
 * @remarks The bounded language contains UTF-8 literals, scalar dot and
 * classes, grouping, alternation, finite or unary quantifiers, logical-line
 * anchors, documented escapes, and ASCII-only d/s/w classes. Matching uses
 * decoded Unicode scalars, leftmost-longest selection, non-overlapping whole
 * matches, and grapheme-boundary match ends. Nullable expressions and syntax
 * requiring backtracking are rejected. The terminal never invokes an
 * application callback.
 */
boxrig_status boxrig_terminal_register_text_recognizer(
    boxrig_terminal *terminal,
    const boxrig_text_recognizer_options *options,
    boxrig_text_regex_diagnostic *diagnostic_out,
    uint64_t *recognizer_id_out);

/**
 * Registers a shared immutable regex program without compiling it again.
 * @param terminal Terminal that owns the new recognizer.
 * @param program Program retained on success; the caller keeps its reference.
 * @param scope Logical text namespaces to scan.
 * @param limits Positive scan/snapshot/result bounds; compiler bounds are
 *        already fixed by program and are ignored here.
 * @param recognizer_id_out Required nonzero identity, cleared on failure.
 * @return OK, INVALID_ARGUMENT, LIMIT_EXCEEDED, or OUT_OF_MEMORY.
 * @remarks Uses the same captures and matching rules as standalone matchers.
 */
boxrig_status boxrig_terminal_register_compiled_text_recognizer(
    boxrig_terminal *terminal, boxrig_regex_program *program,
    boxrig_text_recognizer_scope scope,
    const boxrig_text_recognizer_limits *limits, uint64_t *recognizer_id_out);

/**
 * Retains the immutable captures recorded when a screen match was recognized.
 * @param terminal Terminal to inspect.
 * @param recognizer_id Recognizer identity from a current recognition result.
 * @param match_id Match identity from that result.
 * @param result_set_revision Required current result-set revision.
 * @param result_out Required owned result, cleared on failure.
 * @return OK, STALE for invalidated/missing results, INVALID_ARGUMENT,
 *         or LIMIT_EXCEEDED on reference overflow.
 * @remarks Does not rematch. The reference survives terminal updates, rescans,
 * unregistering, and destruction. Capture offsets address the result's matched
 * bytes, including positions inside a grapheme; they are not terminal columns.
 */
boxrig_status boxrig_terminal_text_recognizer_result(
    const boxrig_terminal *terminal, uint64_t recognizer_id, uint64_t match_id,
    uint64_t result_set_revision, boxrig_regex_result **result_out);

/**
 * Unregisters a recognizer and removes all of its unconsumed events.
 *
 * @param terminal Terminal that owns the recognizer.
 * @param recognizer_id Existing recognizer identity.
 * @return BOXRIG_STATUS_OK or BOXRIG_STATUS_INVALID_ARGUMENT.
 */
boxrig_status boxrig_terminal_unregister_text_recognizer(
    boxrig_terminal *terminal,
    uint64_t recognizer_id);

/**
 * Reads the number of registered regular-expression recognizers.
 *
 * @param terminal Terminal to inspect.
 * @return Current recognizer count, or zero for NULL.
 */
size_t boxrig_terminal_text_recognizer_count(
    const boxrig_terminal *terminal);

/**
 * Advances recognizer line preparation and NFA cursors by a positive integer
 * work budget.
 *
 * @param terminal Terminal whose pending baselines and dirty lines advance.
 * @param work_budget Maximum explicit line preparation, NFA, and scan work in
 *        this dispatch.
 * @param work_used_out Receives work no greater than work_budget.
 * @param more_work_out Receives whether another dispatch is required.
 * @return BOXRIG_STATUS_OK, BOXRIG_STATUS_INVALID_ARGUMENT, or
 *         BOXRIG_STATUS_OUT_OF_MEMORY while preparing a bounded rescan.
 *
 * @remarks more_work_out concerns scanning, not delivery of committed events.
 * Drain events with peek/consume even when more_work_out is false. If text or
 * layout changes before all prior notifications are consumed, this call
 * replaces them with RESCAN_REQUIRED; no incomplete delta is published.
 */
boxrig_status boxrig_terminal_resume_text_recognizers(
    boxrig_terminal *terminal,
    size_t work_budget,
    size_t *work_used_out,
    bool *more_work_out);

/**
 * Copies the oldest application-visible recognizer event without consuming it.
 *
 * @param terminal Terminal to inspect.
 * @param event_out Receives the event. NULL performs a non-copying presence
 *        test.
 * @return True when an event is present.
 */
bool boxrig_terminal_text_recognizer_event_peek(
    const boxrig_terminal *terminal,
    boxrig_text_recognizer_event *event_out);

/**
 * Removes the oldest event returned by the recognizer event peek operation.
 *
 * @param terminal Terminal whose event is removed.
 * @return BOXRIG_STATUS_OK or BOXRIG_STATUS_INVALID_ARGUMENT when empty.
 *
 * @remarks A vacant queue slot is filled from committed pending notifications
 * in round-robin recognizer order. No additional recognition dispatch is
 * required to drain a result set larger than the visible event capacity.
 * Consuming an event invalidated by a text/layout change cancels the rest of
 * that recognizer's notifications and leaves a new RESCAN_REQUIRED event.
 * This also covers consumers that drain events before calling resume again.
 */
boxrig_status boxrig_terminal_text_recognizer_event_consume(
    boxrig_terminal *terminal);

/**
 * Copies one line's complete current recognition results without allocation.
 *
 * @param terminal Terminal to inspect.
 * @param recognizer_id Recognizer selected by an event.
 * @param screen Namespace selected by an event.
 * @param line_id Logical line selected by an event.
 * @param line_revision Required current content revision.
 * @param result_set_revision Required complete result-set revision.
 * @param results Caller-owned output array, or NULL when capacity is zero.
 * @param capacity Number of result elements available.
 * @param required_count_out Receives the exact required element count.
 * @return BOXRIG_STATUS_OK, BOXRIG_STATUS_LIMIT_EXCEEDED without a partial
 *         prefix, BOXRIG_STATUS_INCOMPLETE, BOXRIG_STATUS_STALE, or
 *         BOXRIG_STATUS_INVALID_ARGUMENT.
 */
boxrig_status boxrig_terminal_query_text_recognizer_line_results(
    const boxrig_terminal *terminal,
    uint64_t recognizer_id,
    boxrig_text_screen screen,
    boxrig_text_line_id line_id,
    uint64_t line_revision,
    uint64_t result_set_revision,
    boxrig_text_recognition_result *results,
    size_t capacity,
    size_t *required_count_out);

/**
 * Copies complete current matches intersecting one logical grapheme range.
 *
 * @param terminal Terminal to inspect.
 * @param grapheme_range Nonempty current range of exactly one grapheme.
 * @param recognizer_id_filter Recognizer to query, or zero to query all
 *        recognizers whose scope contains the range.
 * @param results Caller-owned output array, or NULL when capacity is zero.
 * @param capacity Number of result elements available.
 * @param required_count_out Receives the exact required element count.
 * @return BOXRIG_STATUS_OK, BOXRIG_STATUS_LIMIT_EXCEEDED without a partial
 *         prefix, BOXRIG_STATUS_INCOMPLETE, BOXRIG_STATUS_STALE, or
 *         BOXRIG_STATUS_INVALID_ARGUMENT.
 *
 * @remarks Results are ordered by recognizer identity and then match identity.
 * This query performs no allocation, regular-expression scan, callback, or
 * result-set update.
 */
boxrig_status boxrig_terminal_query_recognition_hits(
    const boxrig_terminal *terminal,
    const boxrig_text_range *grapheme_range,
    uint64_t recognizer_id_filter,
    boxrig_text_recognition_result *results,
    size_t capacity,
    size_t *required_count_out);

/**
 * Tests whether event loss requires application annotation to be cleared.
 *
 * @param terminal Terminal to inspect.
 * @param recognizer_id Recognizer identity to inspect.
 * @return True only while a registered recognizer has sticky event loss.
 */
bool boxrig_terminal_text_recognizer_needs_rescan(
    const boxrig_terminal *terminal,
    uint64_t recognizer_id);

/**
 * Begins a new bounded current scan after sticky recognizer event loss.
 *
 * @param terminal Terminal that owns the recognizer.
 * @param recognizer_id Recognizer whose complete current set is requested.
 * @return BOXRIG_STATUS_OK, BOXRIG_STATUS_INVALID_ARGUMENT,
 *         BOXRIG_STATUS_LIMIT_EXCEEDED, or BOXRIG_STATUS_OUT_OF_MEMORY.
 */
boxrig_status boxrig_terminal_rescan_text_recognizer(
    boxrig_terminal *terminal,
    uint64_t recognizer_id);

/**
 * Reads how many rows the primary viewport is displaced from live output.
 *
 * @param terminal Terminal to inspect.
 * @return Viewport row offset, or zero for NULL or the alternate screen.
 */
size_t boxrig_terminal_viewport_offset(const boxrig_terminal *terminal);

/**
 * Returns the revision of primary/alternate active-buffer transitions.
 *
 * @param terminal Terminal to inspect.
 * @return A nonzero revision, or zero for NULL.
 *
 * @remarks The revision changes only when the active screen buffer changes. It
 * is preserved across resize so frontend-owned transient overlays can detect a
 * switch even when both buffers expose the same cursor and cells.
 */
uint64_t boxrig_terminal_active_buffer_revision(
    const boxrig_terminal *terminal);

/**
 * Returns the logical text namespace shown by the active viewport.
 *
 * @param terminal Terminal to inspect.
 * @return The primary or alternate text namespace. NULL is treated as the
 *         primary namespace.
 */
boxrig_text_screen boxrig_terminal_active_text_screen(
    const boxrig_terminal *terminal);

/**
 * Moves the primary viewport relative to live output.
 *
 * @param terminal Terminal to mutate.
 * @param row_offset Number of retained rows above the live viewport.
 * @return BOXRIG_STATUS_OK, or BOXRIG_STATUS_INVALID_ARGUMENT when the offset
 *         exceeds retained history or the alternate screen is active.
 */
boxrig_status boxrig_terminal_set_viewport_offset(
    boxrig_terminal *terminal,
    size_t row_offset);

/**
 * Reads a cell from the active viewport.
 *
 * @param terminal Terminal to inspect.
 * @param row Zero-based row.
 * @param column Zero-based column.
 * @param cell_out Receives a view valid until the terminal is next mutated.
 * @return BOXRIG_STATUS_OK or BOXRIG_STATUS_INVALID_ARGUMENT.
 */
boxrig_status boxrig_terminal_get_cell(
    const boxrig_terminal *terminal,
    size_t row,
    size_t column,
    boxrig_cell_view *cell_out);

/**
 * Reads a storage cell from any retained physical text fragment.
 * @param terminal Terminal to inspect without changing its viewport.
 * @param screen Logical text namespace, including retained primary history.
 * @param fragment_index Current document-order physical fragment index.
 * @param column Zero-based logical storage column within the fragment.
 * @param cell_out Borrowed semantic cell, valid until the next terminal mutation.
 * @return OK or INVALID_ARGUMENT. No output is written on failure.
 * @remarks Indices may change after mutation; use get_text_fragment to verify
 * stable line identity and revision before reusing an index. Continuation cells
 * are returned as stored; resolve_text_fragment_cell finds the whole grapheme.
 */
boxrig_status boxrig_terminal_get_text_fragment_cell(
    const boxrig_terminal *terminal, boxrig_text_screen screen,
    size_t fragment_index, size_t column, boxrig_cell_view *cell_out);

/**
 * Resolves a cell in any retained physical fragment to its complete grapheme.
 * @param terminal Terminal to inspect without mutation or viewport movement.
 * @param screen Logical text namespace.
 * @param fragment_index Current physical fragment index in that namespace.
 * @param storage_column Logical storage column within the fragment.
 * @param resolution_out Receives the normalized cell, fragment and text range.
 * Nested pointers remain valid until the next terminal mutation.
 * @return OK, INVALID_ARGUMENT, or LIMIT_EXCEEDED when text identity is unavailable.
 * @remarks Uses the same cell and grapheme semantics as resolve_viewport_cell.
 * Output remains unchanged on failure.
 */
boxrig_status boxrig_terminal_resolve_text_fragment_cell(
    const boxrig_terminal *terminal, boxrig_text_screen screen,
    size_t fragment_index, size_t storage_column,
    boxrig_viewport_cell_resolution *resolution_out);

/**
 * Resolves an active-viewport storage cell to its complete logical grapheme.
 *
 * @param terminal Terminal to inspect.
 * @param row Zero-based active-viewport row.
 * @param storage_column Zero-based column in the terminal's logical cell
 *        storage, not a bidi visual column.
 * @param resolution_out Receives fragment identity, the normalized leading
 *        column, complete grapheme range, and current semantic cell metadata.
 *        Pointers nested in the cell view remain valid until the terminal is
 *        next mutated.
 * @return BOXRIG_STATUS_OK, BOXRIG_STATUS_INVALID_ARGUMENT, or
 *         BOXRIG_STATUS_LIMIT_EXCEEDED when logical text identity is
 *         unavailable.
 */
boxrig_status boxrig_terminal_resolve_viewport_cell(
    const boxrig_terminal *terminal,
    size_t row,
    size_t storage_column,
    boxrig_viewport_cell_resolution *resolution_out);

/**
 * Reads one logical color from the 256-entry terminal palette.
 *
 * @param terminal Terminal to inspect.
 * @param index Palette index.
 * @param color_out Receives eight-bit RGB components.
 * @return BOXRIG_STATUS_OK or BOXRIG_STATUS_INVALID_ARGUMENT.
 */
boxrig_status boxrig_terminal_get_palette_color(
    const boxrig_terminal *terminal,
    uint8_t index,
    boxrig_rgb *color_out);

/**
 * Reads a dynamic default or cursor color.
 *
 * @param terminal Terminal to inspect.
 * @param color Color role to read.
 * @param color_out Receives eight-bit RGB components.
 * @return BOXRIG_STATUS_OK or BOXRIG_STATUS_INVALID_ARGUMENT.
 */
boxrig_status boxrig_terminal_get_dynamic_color(
    const boxrig_terminal *terminal,
    boxrig_dynamic_color color,
    boxrig_rgb *color_out);

/**
 * Returns the revision of all palette and dynamic color state.
 *
 * @param terminal Terminal to inspect.
 * @return A nonzero revision, or zero for NULL.
 *
 * @remarks Renderers can cache platform colors until this value changes.
 */
uint64_t boxrig_terminal_color_revision(const boxrig_terminal *terminal);

/**
 * Reads the current cursor state.
 *
 * @param terminal Terminal to inspect.
 * @param cursor_out Receives the cursor state.
 */
void boxrig_terminal_get_cursor(
    const boxrig_terminal *terminal,
    boxrig_cursor_view *cursor_out);

/**
 * Reports whether the client is delimiting a synchronized output update.
 *
 * @param terminal Terminal to inspect.
 * @return True while DEC private mode 2026 is set, or false for NULL.
 *
 * @remarks Renderers should retain the last rendered frame and leave terminal
 * dirty unconsumed while this mode is set. Terminal parsing and host event
 * processing must continue. A batch expires after 1000 monotonic milliseconds
 * even if the client omits its end marker. Hosts must advance the clock and
 * service boxrig_terminal_next_animation_deadline() while rendering is held.
 */
bool boxrig_terminal_synchronized_output(
    const boxrig_terminal *terminal);

/**
 * Removes and returns the next changed row interval.
 *
 * @param terminal Terminal to inspect and mutate.
 * @param dirty_out Receives the changed interval.
 * @return True when an interval was returned.
 */
bool boxrig_terminal_take_dirty(
    boxrig_terminal *terminal,
    boxrig_dirty *dirty_out);

/**
 * Validates terminal-cell pixel dimensions without changing terminal state.
 *
 * @param terminal Terminal to inspect.
 * @param width Candidate cell width in physical pixels.
 * @param height Candidate cell height in physical pixels.
 * @return BOXRIG_STATUS_OK when every current placement can be resolved, or
 * BOXRIG_STATUS_INVALID_ARGUMENT otherwise.
 *
 * @remarks This operation neither allocates nor changes dirty, placement
 * revisions, or protocol geometry. A single-threaded caller may use it before
 * committing the same dimensions with boxrig_terminal_set_cell_pixel_geometry().
 */
boxrig_status boxrig_terminal_validate_cell_pixel_geometry(
    const boxrig_terminal *terminal,
    uint32_t width,
    uint32_t height);

/**
 * Sets the current positive terminal-cell pixel dimensions.
 *
 * @param terminal Terminal to mutate.
 * @param width Cell width in physical pixels.
 * @param height Cell height in physical pixels.
 * @return BOXRIG_STATUS_OK, or BOXRIG_STATUS_INVALID_ARGUMENT.
 *
 * @remarks Frontends must update this after a renderer metric change commits.
 * Existing placements retain their logical parameters and are re-resolved.
 */
boxrig_status boxrig_terminal_set_cell_pixel_geometry(
    boxrig_terminal *terminal,
    uint32_t width,
    uint32_t height);

/**
 * Advances terminal-driven image animations and synchronized output recovery.
 *
 * @param terminal Terminal to mutate.
 * @param milliseconds Absolute milliseconds from a host monotonic clock.
 * @return BOXRIG_STATUS_OK, BOXRIG_STATUS_INVALID_ARGUMENT for NULL or time
 * rollback, BOXRIG_STATUS_BUSY while an exclusive clock owner is attached,
 * or BOXRIG_STATUS_OUT_OF_MEMORY when dirty cannot be prepared.
 *
 * @remarks The first call establishes the time origin without advancing a
 * frame. A synchronized batch received before that call starts its 1000 ms
 * recovery interval at the first sample. Repeating the latest value does not
 * advance animations. The core never reads an OS clock and does not infer
 * clock wraparound.
 */
boxrig_status boxrig_terminal_update_monotonic_time(
    boxrig_terminal *terminal,
    uint64_t milliseconds);

/**
 * Acquires exclusive ownership of a terminal's animation clock.
 * @param terminal Terminal whose clock is to be driven by one host runtime.
 * @param clock_out Receives an owned clock handle; NULL on failure.
 * @return OK, BUSY when another owner exists, OUT_OF_MEMORY, or
 * INVALID_ARGUMENT. The previous owner and clock value survive failure.
 * @remarks While acquired, direct terminal clock updates return BUSY. All
 * operations must be serialized on the terminal's owning thread. Release the
 * handle when its runtime detaches. Terminal destruction invalidates the
 * handle without freeing it; subsequent updates return STALE. New image
 * animation starts and synchronized-output batches acquire their time origin
 * at the next owner update. Receiving bytes never invokes a host callback.
 */
boxrig_status boxrig_terminal_acquire_animation_clock(
    boxrig_terminal *terminal, boxrig_animation_clock **clock_out);

/**
 * Releases a clock handle and permits direct or newly acquired updates.
 * @param clock Owned handle to release, or NULL.
 * @remarks The last clock value is retained and future updates must remain
 * in the same monotonic clock domain. Safe after terminal destruction.
 */
void boxrig_animation_clock_release(boxrig_animation_clock *clock);

/**
 * Advances animation time through the exclusive clock owner.
 * @param clock Live clock handle acquired from a terminal.
 * @param milliseconds Absolute monotonic milliseconds in the terminal domain.
 * @return The terminal time-update status, or STALE for a destroyed terminal.
 * @remarks Initial samples, rollback, allocation failure, and repeated values
 * have the same semantics as boxrig_terminal_update_monotonic_time().
 */
boxrig_status boxrig_animation_clock_update_monotonic_time(
    boxrig_animation_clock *clock, uint64_t milliseconds);

/**
 * Reads the earliest image animation or synchronized output recovery deadline.
 *
 * @param terminal Terminal to inspect.
 * @param deadline_out Receives an absolute monotonic millisecond deadline.
 * @return True when an animation or synchronized output batch has a deadline.
 *
 * @remarks Stopped animations and loading animations waiting for a new frame
 * do not contribute a deadline. A synchronized batch contributes its recovery
 * deadline even while rendering is held. Reading does not mutate state.
 */
bool boxrig_terminal_next_animation_deadline(
    const boxrig_terminal *terminal,
    uint64_t *deadline_out);

/**
 * Returns the revision of image resources and active placement results.
 *
 * @param terminal Terminal to inspect.
 * @return A nonzero revision, or zero for NULL.
 *
 * @remarks The revision changes for image data, placement, cell metric, and
 * viewport changes that can affect image rendering. Reading views does not
 * change it or resource eviction order.
 */
uint64_t boxrig_terminal_image_revision(const boxrig_terminal *terminal);

/**
 * Returns the number of committed image resources.
 *
 * @param terminal Terminal to inspect.
 * @return Resource count, or zero for NULL.
 */
size_t boxrig_terminal_image_count(const boxrig_terminal *terminal);

/**
 * Reads the opaque handle at one stable resource-enumeration index.
 *
 * @param terminal Terminal to inspect.
 * @param index Zero-based index smaller than boxrig_terminal_image_count().
 * @param handle_out Receives a nonzero resource handle.
 * @return BOXRIG_STATUS_OK or BOXRIG_STATUS_INVALID_ARGUMENT.
 */
boxrig_status boxrig_terminal_image_handle_at(
    const boxrig_terminal *terminal,
    size_t index,
    boxrig_image_handle *handle_out);

/**
 * Reads one committed normalized image by opaque handle.
 *
 * @param terminal Terminal to inspect.
 * @param handle Nonzero handle obtained from resource enumeration or a
 * placement view.
 * @param image_out Receives a view valid until the terminal is next mutated.
 * @return BOXRIG_STATUS_OK or BOXRIG_STATUS_INVALID_ARGUMENT.
 */
boxrig_status boxrig_terminal_get_image(
    const boxrig_terminal *terminal,
    boxrig_image_handle handle,
    boxrig_image_view *image_out);

/**
 * Returns the number of drawing instances resolved in the active viewport.
 *
 * @param terminal Terminal to inspect.
 * @return Placement instance count, or zero for NULL.
 *
 * @remarks Unicode placeholder instances are included but do not consume the
 * configured protocol placement capacity.
 */
size_t boxrig_terminal_image_placement_count(
    const boxrig_terminal *terminal);

/**
 * Reads one resolved active-viewport image drawing instance.
 *
 * @param terminal Terminal to inspect.
 * @param index Zero-based index smaller than the placement instance count.
 * @param placement_out Receives a view valid until the terminal is next
 * mutated.
 * @return BOXRIG_STATUS_OK or BOXRIG_STATUS_INVALID_ARGUMENT.
 */
boxrig_status boxrig_terminal_get_image_placement(
    const boxrig_terminal *terminal,
    size_t index,
    boxrig_image_placement_view *placement_out);

/**
 * Removes and returns the next changed image-layer row interval.
 *
 * @param terminal Terminal to inspect and mutate.
 * @param dirty_out Receives the changed active-viewport interval.
 * @return True when an interval was returned.
 *
 * @remarks Image dirty is independent from terminal-cell dirty and remains
 * unconsumed while synchronized output is active.
 */
bool boxrig_terminal_take_image_dirty(
    boxrig_terminal *terminal,
    boxrig_dirty *dirty_out);

/**
 * Marks the entire screen as needing redraw.
 *
 * @param terminal Terminal to invalidate.
 */
void boxrig_terminal_invalidate(boxrig_terminal *terminal);

/**
 * Reads the next host event or request without removing it.
 *
 * @param terminal Terminal to inspect.
 * @param item_out Receives a view valid until the terminal is next mutated.
 * @return True when an item was returned.
 */
bool boxrig_terminal_host_item_peek(
    const boxrig_terminal *terminal,
    boxrig_host_item *item_out);

/**
 * Removes the host item returned by boxrig_terminal_host_item_peek().
 *
 * @param terminal Terminal to mutate.
 * @return BOXRIG_STATUS_OK, or BOXRIG_STATUS_INVALID_ARGUMENT when empty.
 */
boxrig_status boxrig_terminal_host_item_consume(boxrig_terminal *terminal);

/**
 * Completes a consumed host request exactly once.
 *
 * @param terminal Terminal awaiting the request.
 * @param request_id Nonzero identifier obtained from a host item.
 * @param result Host operation result.
 * @param payload Clipboard or graphics bytes for a successful read,
 * otherwise NULL.
 * @param payload_length Number of bytes in payload.
 * @return BOXRIG_STATUS_OK on completion, or an error status.
 *
 * @remarks A successful read payload must not exceed the capacity published
 * by its request. Denied and unavailable reads resolve the request's outbound
 * barrier. Notification completions require an empty payload and produce no
 * outbound bytes. Unknown, queued, or already-completed identifiers are
 * rejected. Payload bytes are borrowed only for the duration of this call.
 */
boxrig_status boxrig_terminal_complete_host_request(
    boxrig_terminal *terminal,
    uint64_t request_id,
    boxrig_host_request_result result,
    const uint8_t *payload,
    size_t payload_length);

/**
 * Encodes a logical key event using the current terminal modes.
 *
 * @param terminal Terminal whose input modes are used.
 * @param event Logical key event.
 * @return BOXRIG_STATUS_OK on success, or an error status.
 *
 * @remarks Text must be valid length-delimited UTF-8. A text key uses
 * BOXRIG_KEY_NONE and its Unicode identity fields; a functional key leaves
 * those fields zero. Kitty keyboard modes are encoded before legacy key
 * handling. Modifier key events, including Caps Lock, Num Lock, and Scroll
 * Lock, queue bytes only in Kitty report-all-keys mode; their releases also
 * require event reporting.
 * In the legacy path, Control text presses and repeats are mapped
 * from the Shift-effective logical identity to C0 or DEL; an unshifted event
 * may fall back to a supplied PC-101 base-layout identity. Text releases do
 * not queue bytes. When this function returns BOXRIG_STATUS_WOULD_BLOCK, no
 * bytes are queued and the same event can be retried.
 */
boxrig_status boxrig_terminal_send_key(
    boxrig_terminal *terminal,
    const boxrig_key_event *event);

/**
 * Encodes one phase of a streamed paste transaction.
 *
 * @param terminal Terminal whose input modes are used.
 * @param event Paste phase and normalized UTF-8 chunk.
 * @return BOXRIG_STATUS_OK on success, or an error status.
 *
 * @remarks A transaction starts with BOXRIG_PASTE_BEGIN, contains zero or
 * more BOXRIG_PASTE_CHUNK events, and ends with BOXRIG_PASTE_END. Begin and
 * end events must have empty text. Each chunk must contain complete, valid
 * UTF-8; characters may not span chunks. Text is encoded using the configured
 * codec and unrepresentable Shift_JIS scalars become ASCII question marks.
 * ESC bytes are rejected from chunks so a
 * pasted payload cannot forge the bracketed-paste end marker. When this
 * function returns BOXRIG_STATUS_WOULD_BLOCK, no bytes or transaction state
 * are changed and the same event can be retried.
 */
boxrig_status boxrig_terminal_send_paste(
    boxrig_terminal *terminal,
    const boxrig_paste_event *event);

/**
 * Encodes a logical terminal focus transition when reporting is enabled.
 *
 * @param terminal Terminal whose input modes are used.
 * @param event Focus transition.
 * @return BOXRIG_STATUS_OK on success, or an error status.
 *
 * @remarks When this function returns BOXRIG_STATUS_WOULD_BLOCK, no bytes
 * are queued and the same event can be retried.
 */
boxrig_status boxrig_terminal_send_focus(
    boxrig_terminal *terminal,
    const boxrig_focus_event *event);

/**
 * Reports whether a terminal client currently requests mouse events.
 *
 * @param terminal Terminal whose input modes are inspected.
 * @return True for any enabled X10, VT200, button-event, or any-event mode;
 * otherwise false, including for NULL.
 *
 * @remarks Frontends can use this state for host mouse-override policy. It
 * does not report the selected encoding or motion granularity.
 */
bool boxrig_terminal_mouse_reporting_enabled(
    const boxrig_terminal *terminal);

/**
 * Reports the Alternate Scroll (DEC private mode 1007) setting.
 * @param terminal Terminal whose input modes are inspected.
 * @return True when mode 1007 is enabled, false for the default state or NULL.
 * @remarks The setting applies only in the alternate screen and when mouse
 * tracking is disabled. It survives buffer switches and soft reset; a full
 * reset disables it. Host selection, scrolling and zoom overrides remain the
 * responsibility of the caller's input router.
 */
bool boxrig_terminal_alternate_scroll_enabled(const boxrig_terminal *terminal);

/**
 * Encodes a logical mouse event using the active tracking and encoding modes.
 *
 * @param terminal Terminal whose input modes and dimensions are used.
 * @param event Mouse transition in zero-based content pixels and terminal cells.
 * @return BOXRIG_STATUS_OK on success, or an error status.
 *
 * @remarks With tracking disabled and mode 1007 enabled in the alternate
 * screen, vertical wheel presses emit one unmodified DECCKM Up/Down control.
 * These controls do not use physical-key mappings or Kitty keyboard reports.
 * Other events not selected by the active tracking mode produce no bytes.
 * Their coordinates may belong to an older grid; current grid bounds are
 * validated only for cell-based events selected for encoding. Mode 1016 emits
 * SGR with x/y plus one, preserving signed positions outside the content and
 * ignoring row/column. INT64_MAX in x or y returns LIMIT_EXCEEDED atomically.
 * Modes 1006 and 1016 are mutually exclusive; the last enabled format wins,
 * and resetting another format does not change the active one. Action, button, and
 * modifier validation applies even to unreported events.
 * Legacy mouse encoding returns BOXRIG_STATUS_LIMIT_EXCEEDED for coordinates
 * beyond its 223-by-223 range. When this function returns
 * BOXRIG_STATUS_WOULD_BLOCK, no bytes are queued and the same event can be
 * retried.
 */
boxrig_status boxrig_terminal_send_mouse(
    boxrig_terminal *terminal,
    const boxrig_mouse_event *event);

/**
 * Queues already encoded bytes without text or newline conversion.
 * @param terminal Terminal that owns the outbound queue.
 * @param bytes Length-delimited bytes; may be NULL when length is zero.
 * @param length Byte count, including any NUL bytes.
 * @return OK, WOULD_BLOCK for temporary capacity, or an argument/limit error.
 * @remarks The operation is atomic and preserves existing host barriers.
 */
boxrig_status boxrig_terminal_send_bytes(
    boxrig_terminal *terminal, const uint8_t *bytes, size_t length);

/**
 * Validates complete UTF-8 and encodes it for the terminal client.
 *
 * @param terminal Terminal whose output queue receives the text.
 * @param text Length-delimited UTF-8, including any NUL or control characters.
 * @param length Number of bytes in text.
 * @return OK, INVALID_ARGUMENT for malformed UTF-8, or a capacity/error status.
 * @remarks Unrepresentable Shift_JIS scalars become ASCII question marks.
 * No newline conversion is performed; a failed event queues no prefix.
 */
boxrig_status boxrig_terminal_send_text(
    boxrig_terminal *terminal,
    const char *text,
    size_t length);

/**
 * Reads the next contiguous bytes destined for the terminal client.
 *
 * @param terminal Terminal to inspect.
 * @param bytes_out Receives a pointer valid until the terminal is mutated.
 * @return Number of available bytes.
 */
size_t boxrig_terminal_output_peek(
    const boxrig_terminal *terminal,
    const uint8_t **bytes_out);

/**
 * Removes bytes previously returned by boxrig_terminal_output_peek().
 *
 * @param terminal Terminal to mutate.
 * @param length Number of bytes to consume.
 * @return BOXRIG_STATUS_OK or BOXRIG_STATUS_INVALID_ARGUMENT.
 */
boxrig_status boxrig_terminal_output_consume(
    boxrig_terminal *terminal,
    size_t length);

#ifdef __cplusplus
}
#endif

#endif
