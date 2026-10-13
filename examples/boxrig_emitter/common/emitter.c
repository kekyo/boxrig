/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "emitter.h"

#include <stdio.h>
#include <string.h>

typedef int (*case_runner)(const boxrig_emitter_io *io);

typedef struct emitter_case {
    boxrig_emitter_case_info info;
    case_runner run;
} emitter_case;

typedef struct emitter_cleanup {
    case_runner run;
    case_runner cleanup;
} emitter_cleanup;

typedef enum response_terminator {
    RESPONSE_CSI = 0,
    RESPONSE_OSC,
    RESPONSE_DCS,
    RESPONSE_APC
} response_terminator;

typedef struct query_step {
    const char *label;
    const uint8_t *request;
    size_t request_length;
    response_terminator terminator;
    size_t response_limit;
} query_step;

typedef enum interactive_terminator {
    INTERACTIVE_SEQUENCE = 0,
    INTERACTIVE_PASTE,
    INTERACTIVE_LINE
} interactive_terminator;

typedef enum graphics_palette {
    GRAPHICS_PALETTE_RED = 0,
    GRAPHICS_PALETTE_GREEN,
    GRAPHICS_PALETTE_BLUE,
    GRAPHICS_PALETTE_PURPLE,
    GRAPHICS_PALETTE_YELLOW,
    GRAPHICS_PALETTE_CYAN,
    GRAPHICS_PALETTE_ORANGE,
    GRAPHICS_PALETTE_MAGENTA,
    GRAPHICS_PALETTE_ALPHA,
    GRAPHICS_PALETTE_COUNT
} graphics_palette;

enum {
    INTERACTIVE_CAPTURE_CAPACITY = 4112,
    GRAPHICS_FIXTURE_ENCODED_CAPACITY =
        ((BOXRIG_EMITTER_GRAPHICS_NAME_CAPACITY + 2) / 3) * 4 + 1,
    GRAPHICS_FIXTURE_REQUEST_CAPACITY =
        GRAPHICS_FIXTURE_ENCODED_CAPACITY + 192,
    GRAPHICS_ZLIB_LENGTH =
        BOXRIG_EMITTER_GRAPHICS_FIXTURE_RGBA_LENGTH + 11,
    GRAPHICS_DATA_ENCODED_CAPACITY =
        ((GRAPHICS_ZLIB_LENGTH + 2) / 3) * 4 + 1,
    GRAPHICS_SCREEN_PAYLOAD_CAPACITY = 16384,
    GRAPHICS_SCROLL_WIDTH = 16,
    GRAPHICS_SCROLL_HEIGHT = 8,
    GRAPHICS_SCROLL_RGBA_LENGTH =
        GRAPHICS_SCROLL_WIDTH * GRAPHICS_SCROLL_HEIGHT * 4
};

static const uint8_t cursor_position_query[] = "\x1b[6n";

static const char *const graphics_pattern[
    BOXRIG_EMITTER_GRAPHICS_FIXTURE_HEIGHT] = {
    "....SS..........",
    "...SSSS.........",
    "...SSSS.........",
    "....SS..........",
    "................",
    ".............W..",
    "............WWW.",
    "...........WWWWW",
    "..........WMMWMM",
    ".........WMMMMMM",
    "........WMMMMMMM",
    ".......WMMMMMMMM",
    "......WMMMMMMMMM",
    "GGGGGGGGGGGGGGGG",
    "GGGGGGRRRRGGGGGG",
    "GGGGGRRRRRRGGGGG",
};

static int make_graphics_landscape(
    graphics_palette palette,
    uint8_t *rgba,
    size_t rgba_capacity)
{
    static const uint8_t mountain[GRAPHICS_PALETTE_ALPHA][4] = {
        {224, 48, 48, 255},
        {48, 200, 88, 255},
        {48, 96, 224, 255},
        {144, 64, 224, 255},
        {224, 176, 40, 255},
        {32, 192, 208, 255},
        {240, 112, 32, 255},
        {224, 48, 176, 255},
    };
    static const uint8_t sky[] = {12, 24, 48, 255};
    static const uint8_t sun[] = {255, 220, 64, 255};
    static const uint8_t snow[] = {240, 248, 255, 255};
    static const uint8_t ground[] = {48, 160, 80, 255};
    static const uint8_t river[] = {40, 120, 240, 255};
    static const uint8_t alpha_sky[] = {0, 0, 0, 0};
    static const uint8_t alpha_sun[] = {255, 220, 64, 192};
    static const uint8_t alpha_snow[] = {240, 248, 255, 192};
    static const uint8_t alpha_mountain[] = {48, 224, 96, 128};
    static const uint8_t alpha_ground[] = {48, 160, 80, 160};
    static const uint8_t alpha_river[] = {40, 120, 240, 192};
    size_t y;

    if (palette < GRAPHICS_PALETTE_RED ||
        palette >= GRAPHICS_PALETTE_COUNT || rgba == NULL ||
        rgba_capacity < BOXRIG_EMITTER_GRAPHICS_FIXTURE_RGBA_LENGTH) {
        return -1;
    }
    for (y = 0; y < BOXRIG_EMITTER_GRAPHICS_FIXTURE_HEIGHT; y++) {
        size_t x;

        for (x = 0; x < BOXRIG_EMITTER_GRAPHICS_FIXTURE_WIDTH; x++) {
            char symbol = graphics_pattern[y][x];
            const uint8_t *color =
                palette == GRAPHICS_PALETTE_ALPHA ? alpha_sky : sky;
            size_t offset =
                (y * BOXRIG_EMITTER_GRAPHICS_FIXTURE_WIDTH + x) * 4;

            if (symbol == 'S') {
                color = palette == GRAPHICS_PALETTE_ALPHA
                            ? alpha_sun
                            : sun;
            } else if (symbol == 'W') {
                color = palette == GRAPHICS_PALETTE_ALPHA
                            ? alpha_snow
                            : snow;
            } else if (symbol == 'M') {
                color = palette == GRAPHICS_PALETTE_ALPHA
                            ? alpha_mountain
                            : mountain[palette];
            } else if (symbol == 'G') {
                color = palette == GRAPHICS_PALETTE_ALPHA
                            ? alpha_ground
                            : ground;
            } else if (symbol == 'R') {
                color = palette == GRAPHICS_PALETTE_ALPHA
                            ? alpha_river
                            : river;
            }
            memcpy(rgba + offset, color, 4);
        }
    }
    return 0;
}

int boxrig_emitter_make_graphics_fixture_pixels(
    boxrig_emitter_graphics_fixture_kind kind,
    uint8_t *rgba,
    size_t rgba_capacity)
{
    graphics_palette palette;

    switch (kind) {
    case BOXRIG_EMITTER_GRAPHICS_FIXTURE_REGULAR:
        palette = GRAPHICS_PALETTE_RED;
        break;
    case BOXRIG_EMITTER_GRAPHICS_FIXTURE_TEMPORARY:
        palette = GRAPHICS_PALETTE_GREEN;
        break;
    case BOXRIG_EMITTER_GRAPHICS_FIXTURE_SHARED:
        palette = GRAPHICS_PALETTE_BLUE;
        break;
    default:
        return -1;
    }
    return make_graphics_landscape(palette, rgba, rgba_capacity);
}

static int run_backspace(const boxrig_emitter_io *io);
static int run_transport_bytes(const boxrig_emitter_io *io);
static int run_text_ascii(const boxrig_emitter_io *io);
static int run_text_cjk(const boxrig_emitter_io *io);
static int run_text_combining(const boxrig_emitter_io *io);
static int run_text_long_grapheme(const boxrig_emitter_io *io);
static int run_text_ligatures(const boxrig_emitter_io *io);
static int run_text_complex_scripts(const boxrig_emitter_io *io);
static int run_text_bidi(const boxrig_emitter_io *io);
static int run_text_emoji(const boxrig_emitter_io *io);
static int run_text_ambiguous_width(const boxrig_emitter_io *io);
static int run_text_invalid_utf8(const boxrig_emitter_io *io);
static int run_c0_controls(const boxrig_emitter_io *io);
static int run_cancel_recovery(const boxrig_emitter_io *io);
static int run_esc_indexing(const boxrig_emitter_io *io);
static int run_character_sets(const boxrig_emitter_io *io);
static int run_cursor_movement(const boxrig_emitter_io *io);
static int run_character_editing(const boxrig_emitter_io *io);
static int run_line_scrolling(const boxrig_emitter_io *io);
static int run_tab_stops(const boxrig_emitter_io *io);
static int run_erase_display(const boxrig_emitter_io *io);
static int run_erase_line(const boxrig_emitter_io *io);
static int run_selective_erase(const boxrig_emitter_io *io);
static int run_margins(const boxrig_emitter_io *io);
static int run_save_restore_soft_reset(const boxrig_emitter_io *io);
static int run_terminal_reset(const boxrig_emitter_io *io);
static int run_standard_modes(const boxrig_emitter_io *io);
static int run_private_modes(const boxrig_emitter_io *io);
static int run_alternate_screen_modes(const boxrig_emitter_io *io);
static int run_cursor_style_protection(const boxrig_emitter_io *io);
static int run_title_stack(const boxrig_emitter_io *io);
static int run_sgr_attributes(const boxrig_emitter_io *io);
static int run_sgr_ansi_colors(const boxrig_emitter_io *io);
static int run_sgr_extended_colors(const boxrig_emitter_io *io);
static int run_sgr_underline_styles(const boxrig_emitter_io *io);
static int run_osc_titles(const boxrig_emitter_io *io);
static int cleanup_osc_titles(const boxrig_emitter_io *io);
static int run_osc_working_directory(const boxrig_emitter_io *io);
static int run_osc_hyperlink(const boxrig_emitter_io *io);
static int run_osc_palette(const boxrig_emitter_io *io);
static int cleanup_osc_palette(const boxrig_emitter_io *io);
static int run_osc_dynamic_colors(const boxrig_emitter_io *io);
static int cleanup_osc_dynamic_colors(const boxrig_emitter_io *io);
static int run_clipboard_write(const boxrig_emitter_io *io);
static int run_desktop_notification(const boxrig_emitter_io *io);
static int run_desktop_notification_options(const boxrig_emitter_io *io);
static int run_kitty_image_static(const boxrig_emitter_io *io);
static int run_kitty_image_external(const boxrig_emitter_io *io);
static int run_kitty_animation_direct(const boxrig_emitter_io *io);
static int run_kitty_animation_client(const boxrig_emitter_io *io);
static int run_kitty_animation_loop(const boxrig_emitter_io *io);
static int run_kitty_animation_compose(const boxrig_emitter_io *io);
static int run_query_cursor_position(const boxrig_emitter_io *io);
static int run_query_device_status(const boxrig_emitter_io *io);
static int run_query_device_attributes(const boxrig_emitter_io *io);
static int run_query_modes(const boxrig_emitter_io *io);
static int run_query_decrqss(const boxrig_emitter_io *io);
static int run_query_colors(const boxrig_emitter_io *io);
static int run_clipboard_read(const boxrig_emitter_io *io);
static int run_notification_query(const boxrig_emitter_io *io);
static int run_query_kitty_graphics(const boxrig_emitter_io *io);
static int run_query_kitty_keyboard(const boxrig_emitter_io *io);
static int run_input_key(const boxrig_emitter_io *io);
static int run_input_application_cursor(const boxrig_emitter_io *io);
static int run_input_application_keypad(const boxrig_emitter_io *io);
static int run_input_kitty_keyboard(const boxrig_emitter_io *io);
static int run_input_paste(const boxrig_emitter_io *io);
static int run_input_focus(const boxrig_emitter_io *io);
static int run_input_mouse(const boxrig_emitter_io *io);
static int run_input_ime_commit(const boxrig_emitter_io *io);
static int run_frontend_resize(const boxrig_emitter_io *io);
static int run_frontend_scrollback(const boxrig_emitter_io *io);
static int run_frontend_font_zoom(const boxrig_emitter_io *io);
static int run_frontend_ime_preedit(const boxrig_emitter_io *io);
static int run_safe_no_op(const boxrig_emitter_io *io);
static int run_unsupported_recovery(const boxrig_emitter_io *io);

static const emitter_case cases[] = {
    {
        {
            "backspace",
            "C0",
            "Moves left, erases C with a space, and leaves the cursor after B.",
            "visible result is \"AB\"",
            "character-and-c0",
            "BS",
            BOXRIG_EMITTER_CASE_OUTPUT,
            false,
            false,
            false,
        },
        run_backspace,
    },
    {
        {
            "transport-bytes",
            "Transport",
            "Emits representative terminal protocols and UTF-8 as one "
            "binary-safe fixture.",
            "every fixture byte reaches the PTY unchanged",
            "transport-contract",
            "C0/CSI/OSC/DCS/APC/UTF-8",
            BOXRIG_EMITTER_CASE_OUTPUT,
            false,
            false,
            false,
        },
        run_transport_bytes,
    },
    {
        {
            "text-ascii",
            "Text",
            "Emits printable ASCII letters, digits, spaces, and punctuation.",
            "the complete ASCII sample appears on one line",
            "character-and-c0",
            "UTF-8/ASCII",
            BOXRIG_EMITTER_CASE_OUTPUT,
            false,
            false,
            false,
        },
        run_text_ascii,
    },
    {
        {
            "text-cjk",
            "Text",
            "Emits Japanese, simplified and traditional Chinese, Korean, and "
            "a marked wide-cell boundary.",
            "every CJK sample appears and the boundary marker follows a "
            "two-column ideograph",
            "character-and-c0",
            "UTF-8/East Asian Width",
            BOXRIG_EMITTER_CASE_OUTPUT,
            false,
            false,
            false,
        },
        run_text_cjk,
    },
    {
        {
            "text-combining",
            "Text",
            "Emits one and multiple combining marks plus a text-presentation "
            "variation selector.",
            "each labeled cluster occupies one logical cell without losing "
            "marks",
            "character-and-c0",
            "UTF-8/grapheme",
            BOXRIG_EMITTER_CASE_OUTPUT,
            false,
            false,
            false,
        },
        run_text_combining,
    },
    {
        {
            "text-long-grapheme",
            "Text",
            "Emits a 5,001-byte combining sequence to exercise bounded "
            "grapheme storage.",
            "the marks remain visible and processing continues after the "
            "4,096-byte cell boundary",
            "character-and-c0",
            "UTF-8/grapheme limit",
            BOXRIG_EMITTER_CASE_OUTPUT,
            false,
            false,
            false,
        },
        run_text_long_grapheme,
    },
    {
        {
            "text-ligatures",
            "Text",
            "Emits shaping candidates and Unicode presentation-form "
            "ligatures.",
            "all source characters remain present; glyph joining depends on "
            "the selected font",
            "unicode-rendering",
            "UTF-8/font shaping",
            BOXRIG_EMITTER_CASE_OUTPUT,
            false,
            false,
            false,
        },
        run_text_ligatures,
    },
    {
        {
            "text-complex-scripts",
            "Text",
            "Emits Arabic, Hebrew, Devanagari, and Thai shaping fixtures.",
            "all four labeled scripts render without missing or displaced "
            "text",
            "unicode-rendering",
            "UTF-8/complex script",
            BOXRIG_EMITTER_CASE_OUTPUT,
            false,
            false,
            false,
        },
        run_text_complex_scripts,
    },
    {
        {
            "text-bidi",
            "Text",
            "Emits mixed left-to-right and right-to-left logical lines.",
            "the Hebrew and Arabic runs reorder visually while surrounding "
            "Latin markers remain intact",
            "unicode-rendering",
            "UTF-8/bidirectional text",
            BOXRIG_EMITTER_CASE_OUTPUT,
            false,
            false,
            false,
        },
        run_text_bidi,
    },
    {
        {
            "text-emoji",
            "Text",
            "Emits emoji presentation, skin tone, flag, keycap, profession, "
            "and family clusters.",
            "each labeled emoji sequence stays in one two-column logical "
            "cluster",
            "character-and-c0",
            "UTF-8/emoji grapheme",
            BOXRIG_EMITTER_CASE_OUTPUT,
            false,
            false,
            false,
        },
        run_text_emoji,
    },
    {
        {
            "text-ambiguous-width",
            "Text",
            "Emits East Asian Width Ambiguous characters between rulers.",
            "middle dot and Greek omega use one or two columns according to "
            "the terminal width policy",
            "character-and-c0",
            "UTF-8/East Asian Ambiguous",
            BOXRIG_EMITTER_CASE_OUTPUT,
            false,
            false,
            false,
        },
        run_text_ambiguous_width,
    },
    {
        {
            "text-invalid-utf8",
            "Text",
            "Emits malformed and control-terminated UTF-8 followed by valid "
            "text.",
            "replacement glyphs appear and the RECOVERED line is complete",
            "character-and-c0",
            "invalid UTF-8 recovery",
            BOXRIG_EMITTER_CASE_OUTPUT,
            false,
            false,
            false,
        },
        run_text_invalid_utf8,
    },
    {
        {
            "c0-controls", "C0",
            "Emits every supported display-oriented C0 control and DEL.",
            "labeled BS, HT, line-feed, carriage-return, and DEL samples "
            "remain distinguishable; BEL requests host attention",
            "character-and-c0", "BEL/BS/HT/LF/VT/FF/CR/DEL",
            BOXRIG_EMITTER_CASE_OUTPUT, false, true, false,
        },
        run_c0_controls,
    },
    {
        {
            "cancel-recovery", "C0",
            "Cancels incomplete CSI and OSC input with CAN and SUB.",
            "RECOVERED-CAN and RECOVERED-SUB both appear unstyled",
            "character-and-c0", "CAN/SUB",
            BOXRIG_EMITTER_CASE_OUTPUT, false, false, false,
        },
        run_cancel_recovery,
    },
    {
        {
            "esc-indexing", "ESC",
            "Exercises index, next-line, tab-set, and reverse-index ESC "
            "functions on a numbered canvas.",
            "D, E, T, and R markers land at their labeled destinations",
            "esc-sequence", "IND/NEL/HTS/RI",
            BOXRIG_EMITTER_CASE_OUTPUT, false, false, false,
        },
        run_esc_indexing,
    },
    {
        {
            "character-sets", "ESC",
            "Selects DEC Special Graphics through G0 and G1, then restores "
            "ASCII.",
            "DEC q becomes a horizontal line while restored q stays ASCII",
            "esc-sequence", "SCS/SO/SI",
            BOXRIG_EMITTER_CASE_OUTPUT, false, false, false,
        },
        run_character_sets,
    },
    {
        {
            "cursor-movement", "CSI",
            "Exercises every supported relative and absolute cursor movement.",
            "letter markers appear on the ruler canvas and completion remains",
            "csi-cursor-editing", "CUU/CUD/CUF/CUB/CNL/CPL/CHA/VPA/CUP/HVP",
            BOXRIG_EMITTER_CASE_OUTPUT, true, false, false,
        },
        run_cursor_movement,
    },
    {
        {
            "character-editing", "CSI",
            "Exercises character insertion, deletion, erasure, and repeat.",
            "four labeled rows show shifted, removed, blanked, and repeated "
            "cells",
            "csi-cursor-editing", "ICH/DCH/ECH/REP",
            BOXRIG_EMITTER_CASE_OUTPUT, false, false, false,
        },
        run_character_editing,
    },
    {
        {
            "line-scrolling", "CSI",
            "Exercises line insertion/deletion and both scroll directions.",
            "numbered rows reveal IL, DL, SU, SD, and xterm SD movement",
            "csi-cursor-editing", "IL/DL/SU/SD",
            BOXRIG_EMITTER_CASE_OUTPUT, true, false, false,
        },
        run_line_scrolling,
    },
    {
        {
            "tab-stops", "CSI",
            "Sets and clears individual and all horizontal tab stops.",
            "tab markers land at the custom stop before RIS restores defaults",
            "csi-cursor-editing", "HTS/TBC/HT",
            BOXRIG_EMITTER_CASE_OUTPUT, true, false, false,
        },
        run_tab_stops,
    },
    {
        {
            "erase-display", "CSI",
            "Exercises ED 0, 1, 2, and scrollback-only ED 3 on a canvas.",
            "prefix, suffix, cleared-screen, and retained-screen markers show "
            "each erase range",
            "csi-erase-margin-save", "ED",
            BOXRIG_EMITTER_CASE_OUTPUT, true, false, false,
        },
        run_erase_display,
    },
    {
        {
            "erase-line", "CSI",
            "Exercises EL 0, 1, and 2 on separate ruler rows.",
            "the three rows retain only the cells outside each erase range",
            "csi-erase-margin-save", "EL",
            BOXRIG_EMITTER_CASE_OUTPUT, false, false, false,
        },
        run_erase_line,
    },
    {
        {
            "selective-erase", "CSI",
            "Exercises DECSED and DECSEL while mixing protected and normal "
            "cells.",
            "protected P markers survive while adjacent U markers disappear",
            "csi-erase-margin-save", "DECSED/DECSEL/DECSCA",
            BOXRIG_EMITTER_CASE_OUTPUT, true, false, false,
        },
        run_selective_erase,
    },
    {
        {
            "margins", "CSI",
            "Combines top/bottom and left/right margins with origin mode.",
            "scrolling changes only the marked rectangular canvas",
            "csi-erase-margin-save", "DECSTBM/DECSLRM/DECOM",
            BOXRIG_EMITTER_CASE_OUTPUT, true, false, false,
        },
        run_margins,
    },
    {
        {
            "save-restore-soft-reset", "CSI",
            "Exercises DEC and CSI cursor save/restore followed by DECSTR.",
            "both restored markers remain and soft reset preserves cell text",
            "csi-erase-margin-save", "DECSC/DECRC/SCOSC/SCORC/DECSTR",
            BOXRIG_EMITTER_CASE_OUTPUT, true, false, false,
        },
        run_save_restore_soft_reset,
    },
    {
        {
            "terminal-reset", "ESC",
            "Performs RIS after displaying a disposable setup canvas.",
            "the setup disappears and RESET COMPLETE begins on a clean screen",
            "esc-sequence", "RIS",
            BOXRIG_EMITTER_CASE_OUTPUT, true, false, false,
        },
        run_terminal_reset,
    },
    {
        {
            "standard-modes", "Mode",
            "Exercises insert/replace and newline/line-feed standard modes.",
            "inserted text and line positions differ before cleanup",
            "mode", "IRM/LNM",
            BOXRIG_EMITTER_CASE_OUTPUT, false, false, false,
        },
        run_standard_modes,
    },
    {
        {
            "private-modes", "Mode",
            "Briefly enables and disables every supported non-alternate DEC "
            "private mode.",
            "PRIVATE MODES COMPLETE appears with normal cursor and screen state",
            "mode", "DECSET/DECRST",
            BOXRIG_EMITTER_CASE_OUTPUT, true, false, false,
        },
        run_private_modes,
    },
    {
        {
            "alternate-screen-modes", "Mode",
            "Exercises modes 47, 1047, 1048, and 1049 with primary markers.",
            "the primary canvas returns after every alternate/save transition",
            "mode", "alternate screen",
            BOXRIG_EMITTER_CASE_OUTPUT, true, false, false,
        },
        run_alternate_screen_modes,
    },
    {
        {
            "cursor-style-protection", "CSI",
            "Selects cursor styles 0 through 6 and demonstrates protected "
            "cells.",
            "P survives selective erase, U and V disappear, and cleanup "
            "restores the default blinking block",
            "csi-erase-margin-save", "DECSCUSR/DECSCA",
            BOXRIG_EMITTER_CASE_OUTPUT, false, false, false,
        },
        run_cursor_style_protection,
    },
    {
        {
            "title-stack", "CSI",
            "Pushes and pops combined, icon-only, and window-only title stacks.",
            "the host restores each selected title target in order",
            "csi-title-stack", "XTITLESTACK",
            BOXRIG_EMITTER_CASE_OUTPUT, false, true, false,
        },
        run_title_stack,
    },
    {
        {
            "sgr-attributes", "SGR",
            "Emits every supported rendition attribute and matching reset.",
            "each labeled sample has only its named attribute",
            "sgr", "SGR attributes",
            BOXRIG_EMITTER_CASE_OUTPUT, false, false, false,
        },
        run_sgr_attributes,
    },
    {
        {
            "sgr-ansi-colors", "SGR",
            "Emits all normal and bright ANSI foreground/background colors.",
            "four eight-cell rows show the complete 16-color palette",
            "sgr", "SGR ANSI colors",
            BOXRIG_EMITTER_CASE_OUTPUT, false, false, false,
        },
        run_sgr_ansi_colors,
    },
    {
        {
            "sgr-extended-colors", "SGR",
            "Emits indexed and RGB colors using semicolon and colon forms.",
            "foreground, background, and underline samples show each color "
            "form and default reset",
            "sgr", "SGR extended colors",
            BOXRIG_EMITTER_CASE_OUTPUT, false, false, false,
        },
        run_sgr_extended_colors,
    },
    {
        {
            "sgr-underline-styles", "SGR",
            "Emits single, double, curly, dotted, and dashed underlines.",
            "nine rows show all underline selections and both reset forms",
            "sgr", "SGR underline styles",
            BOXRIG_EMITTER_CASE_OUTPUT, false, false, false,
        },
        run_sgr_underline_styles,
    },
    {
        {
            "osc-titles", "OSC",
            "Sets combined, icon-only, and window-only titles with BEL and ST.",
            "the host receives all three title targets and then restores the "
            "previous titles",
            "osc", "OSC 0/1/2",
            BOXRIG_EMITTER_CASE_OUTPUT, false, true, false,
        },
        run_osc_titles,
    },
    {
        {
            "osc-working-directory", "OSC",
            "Reports a fixed file URI as the terminal working directory.",
            "the host receives file://localhost/tmp/boxrig-emitter without "
            "changing the process directory",
            "osc", "OSC 7",
            BOXRIG_EMITTER_CASE_OUTPUT, false, true, false,
        },
        run_osc_working_directory,
    },
    {
        {
            "osc-hyperlink", "OSC",
            "Attaches an id and URI to one labeled cell, then closes the link.",
            "only the L sample carries hyperlink metadata; no URI is opened",
            "osc", "OSC 8",
            BOXRIG_EMITTER_CASE_OUTPUT, false, false, false,
        },
        run_osc_hyperlink,
    },
    {
        {
            "osc-palette", "OSC",
            "Sets two indexed palette colors, prints samples, and resets them.",
            "indices 1 and 2 use the requested colors during the framed sample",
            "osc", "OSC 4/104",
            BOXRIG_EMITTER_CASE_OUTPUT, true, false, false,
        },
        run_osc_palette,
    },
    {
        {
            "osc-dynamic-colors", "OSC",
            "Sets foreground, background, and cursor defaults, then resets them.",
            "all three dynamic colors change during the framed sample",
            "osc", "OSC 10/11/12/110/111/112",
            BOXRIG_EMITTER_CASE_OUTPUT, true, false, false,
        },
        run_osc_dynamic_colors,
    },
    {
        {
            "clipboard-write", "OSC",
            "Requests writing a fixed UTF-8 fixture to the clipboard.",
            "the host clipboard becomes 'boxrig clipboard fixture' when policy "
            "permits writes",
            "osc", "OSC 52 write",
            BOXRIG_EMITTER_CASE_OUTPUT, false, true, false,
        },
        run_clipboard_write,
    },
    {
        {
            "desktop-notification", "OSC",
            "Requests one identified normal-priority desktop notification.",
            "the request reaches the OS only when notifications are explicitly "
            "enabled for boxrig_term",
            "osc-99", "OSC 99 notification",
            BOXRIG_EMITTER_CASE_OUTPUT, false, true, false,
        },
        run_desktop_notification,
    },
    {
        {
            "desktop-notification-options", "OSC",
            "Exercises fragmented/Base64 notification fields and low/critical "
            "delivery options.",
            "two requests expose body, application, type, occasion, urgency, "
            "and identifier variants when notification policy permits",
            "osc-99", "OSC 99 fields and options",
            BOXRIG_EMITTER_CASE_OUTPUT, false, true, false,
        },
        run_desktop_notification_options,
    },
    {
        {
            "kitty-image-static", "Kitty graphics",
            "Builds one deterministic image canvas from raw RGB, chunked "
            "RGBA, zlib, PNG, crop/scale, scrollback, deletion, alternate "
            "screen, and a Unicode placeholder.",
            "fixed rows show multicolor pixel-art landscapes, a cropped "
            "right half, alpha, zlib, PNG, a deletion gap, an orange "
            "primary, a cyan placeholder grid, and a scrolled sailboat",
            "kitty-graphics", "static image transport and lifecycle",
            BOXRIG_EMITTER_CASE_OUTPUT, false, false, false,
        },
        run_kitty_image_static,
    },
    {
        {
            "kitty-image-external", "Kitty graphics",
            "Reads one platform-created regular file, safe temporary file, "
            "and shared-memory resource.",
            "the default policy reports EACCES; with external graphics "
            "enabled, red, green, and blue 4-by-2 pixel-art pictures appear",
            "kitty-graphics", "external file and shared-memory transport",
            BOXRIG_EMITTER_CASE_ROUND_TRIP, true, true, true,
        },
        run_kitty_image_external,
    },
    {
        {
            "kitty-animation-direct", "Kitty graphics",
            "Edits the visible root frame with a direct animation-frame "
            "transfer.",
            "one 4-by-2 landscape changes from red to green on its right half",
            "kitty-graphics", "direct animation frame transfer",
            BOXRIG_EMITTER_CASE_OUTPUT, true, false, false,
        },
        run_kitty_animation_direct,
    },
    {
        {
            "kitty-animation-client", "Kitty graphics",
            "Creates three frames and selects the blue frame from the "
            "client.",
            "one blue 4-by-2 landscape remains selected without automatic "
            "playback",
            "kitty-graphics", "client-driven animation frame selection",
            BOXRIG_EMITTER_CASE_OUTPUT, true, false, false,
        },
        run_kitty_animation_client,
    },
    {
        {
            "kitty-animation-loop", "Kitty graphics",
            "Starts an infinite terminal-driven animation using fixed frame "
            "gaps.",
            "one 4-by-2 landscape alternates between red and green about "
            "every 120 ms",
            "kitty-graphics", "terminal-driven animation loop",
            BOXRIG_EMITTER_CASE_OUTPUT, true, false, false,
        },
        run_kitty_animation_loop,
    },
    {
        {
            "kitty-animation-compose", "Kitty graphics",
            "Copies a rectangle between animation frames and selects the "
            "result.",
            "one 4-by-2 landscape shows a copied blue center inside a black "
            "border",
            "kitty-graphics", "animation frame composition",
            BOXRIG_EMITTER_CASE_OUTPUT, true, false, false,
        },
        run_kitty_animation_compose,
    },
    {
        {
            "query-cursor-position", "Query",
            "Queries the current cursor position as one focused round trip.",
            "one bounded CSI response displays the current row and column",
            "query-response", "DSR 6/DECXCPR",
            BOXRIG_EMITTER_CASE_ROUND_TRIP, false, false, true,
        },
        run_query_cursor_position,
    },
    {
        {
            "query-device-status", "Query",
            "Queries all supported standard and DEC device status reports.",
            "eight bounded CSI responses are displayed as visible tokens",
            "query-response", "DSR",
            BOXRIG_EMITTER_CASE_ROUND_TRIP, false, false, true,
        },
        run_query_device_status,
    },
    {
        {
            "query-device-attributes", "Query",
            "Queries primary/secondary device attributes and terminal version.",
            "two CSI and one DCS response are displayed as visible tokens",
            "query-response", "DA1/DA2/XTVERSION",
            BOXRIG_EMITTER_CASE_ROUND_TRIP, false, false, true,
        },
        run_query_device_attributes,
    },
    {
        {
            "query-modes", "Query",
            "Queries every supported standard and DEC private mode.",
            "22 DECRQM responses report set, reset, or unrecognized state",
            "query-response", "DECRQM",
            BOXRIG_EMITTER_CASE_ROUND_TRIP, false, false, true,
        },
        run_query_modes,
    },
    {
        {
            "query-decrqss", "Query",
            "Queries all five supported DECRQSS state strings and one unknown.",
            "five success responses and one failure response are visible",
            "query-response", "DECRQSS",
            BOXRIG_EMITTER_CASE_ROUND_TRIP, false, false, true,
        },
        run_query_decrqss,
    },
    {
        {
            "query-colors", "Query",
            "Queries one palette entry and all three dynamic colors.",
            "four OSC responses retain the query's BEL or ST terminator",
            "osc", "OSC 4/10/11/12 query",
            BOXRIG_EMITTER_CASE_ROUND_TRIP, false, false, true,
        },
        run_query_colors,
    },
    {
        {
            "clipboard-read", "Query",
            "Requests clipboard selection c and displays the bounded response.",
            "the sample policy returns an empty OSC 52 response; permissive "
            "hosts may expose clipboard data",
            "osc", "OSC 52 query",
            BOXRIG_EMITTER_CASE_ROUND_TRIP, false, true, true,
        },
        run_clipboard_read,
    },
    {
        {
            "notification-query", "Query",
            "Queries the implemented desktop-notification capability profile.",
            "the OSC 99 profile is displayed without creating a notification",
            "osc-99", "OSC 99 query",
            BOXRIG_EMITTER_CASE_ROUND_TRIP, false, false, true,
        },
        run_notification_query,
    },
    {
        {
            "query-kitty-graphics", "Query",
            "Queries one valid direct RGBA upload without committing it.",
            "one bounded Kitty graphics APC response reports image 9300 OK",
            "kitty-graphics", "a=q query response",
            BOXRIG_EMITTER_CASE_ROUND_TRIP, false, false, true,
        },
        run_query_kitty_graphics,
    },
    {
        {
            "query-kitty-keyboard", "Query",
            "Queries the active Kitty keyboard protocol flags.",
            "one CSI response displays the current flag bitmask",
            "kitty-keyboard", "CSI ? u",
            BOXRIG_EMITTER_CASE_ROUND_TRIP, false, false, true,
        },
        run_query_kitty_keyboard,
    },
    {
        {
            "input-key", "Input",
            "Captures one modified legacy key sequence without enabling a "
            "private input mode.",
            "pressing Ctrl+Alt+Up displays one CSI sequence as visible tokens",
            "key-and-keypad", "legacy modified key",
            BOXRIG_EMITTER_CASE_INTERACTIVE, false, false, true,
        },
        run_input_key,
    },
    {
        {
            "input-application-cursor", "Input",
            "Enables application cursor keys and captures one cursor key.",
            "pressing Up displays an SS3 sequence before normal mode returns",
            "key-and-keypad", "DECCKM/application cursor",
            BOXRIG_EMITTER_CASE_INTERACTIVE, false, false, true,
        },
        run_input_application_cursor,
    },
    {
        {
            "input-application-keypad", "Input",
            "Enables application keypad mode and captures one keypad key.",
            "pressing keypad 1 displays an SS3 sequence before numeric mode "
            "returns",
            "key-and-keypad", "DECNKM/application keypad",
            BOXRIG_EMITTER_CASE_INTERACTIVE, false, false, true,
        },
        run_input_application_keypad,
    },
    {
        {
            "input-kitty-keyboard", "Input",
            "Pushes all Kitty keyboard flags and captures press, repeat, and "
            "release reports.",
            "holding Shift+Ctrl+A displays three bounded CSI u events with "
            "alternate and associated text fields",
            "kitty-keyboard", "Kitty keyboard input",
            BOXRIG_EMITTER_CASE_INTERACTIVE, false, false, true,
        },
        run_input_kitty_keyboard,
    },
    {
        {
            "input-paste", "Input",
            "Enables bracketed paste and captures one complete transaction.",
            "one paste displays its begin marker, payload bytes, and end "
            "marker as visible tokens",
            "paste-focus-mouse", "bracketed paste input",
            BOXRIG_EMITTER_CASE_INTERACTIVE, false, false, true,
        },
        run_input_paste,
    },
    {
        {
            "input-focus", "Input",
            "Enables focus reporting and captures one focus-out/focus-in pair.",
            "leaving and returning to the window displays CSI O followed by "
            "CSI I",
            "paste-focus-mouse", "focus input",
            BOXRIG_EMITTER_CASE_INTERACTIVE, false, false, true,
        },
        run_input_focus,
    },
    {
        {
            "input-mouse", "Input",
            "Enables any-event tracking with SGR encoding and captures three "
            "mouse reports.",
            "motion, wheel, and button reports display bounded decimal CSI "
            "sequences",
            "paste-focus-mouse", "SGR mouse input",
            BOXRIG_EMITTER_CASE_INTERACTIVE, false, false, true,
        },
        run_input_mouse,
    },
    {
        {
            "input-ime-commit", "Input",
            "Captures committed IME text up to the following Enter key.",
            "committing Japanese text and pressing Enter displays UTF-8 bytes "
            "and CR as visible tokens",
            "ime-composition", "IME commit/literal text",
            BOXRIG_EMITTER_CASE_INTERACTIVE, false, false, true,
        },
        run_input_ime_commit,
    },
    {
        {
            "frontend-resize", "Frontend",
            "Explains how to observe grid resize and content reflow.",
            "resizing preserves logical content while the visible row and "
            "column count changes",
            "scrollback-resize", "frontend resize operation",
            BOXRIG_EMITTER_CASE_FRONTEND, true, false, false,
        },
        run_frontend_resize,
    },
    {
        {
            "frontend-scrollback", "Frontend",
            "Explains how to observe the primary scrollback viewport.",
            "scrolling visits retained history and returns to live output",
            "scrollback-resize", "frontend viewport operation",
            BOXRIG_EMITTER_CASE_FRONTEND, true, false, false,
        },
        run_frontend_scrollback,
    },
    {
        {
            "frontend-font-zoom", "Frontend",
            "Explains how to observe frontend font zoom and grid recomputation.",
            "zoom changes cell metrics without corrupting text or shaping",
            "font-metrics", "frontend font zoom operation",
            BOXRIG_EMITTER_CASE_FRONTEND, true, false, false,
        },
        run_frontend_font_zoom,
    },
    {
        {
            "frontend-ime-preedit", "Frontend",
            "Explains how to observe transient IME preedit rendering.",
            "preedit, segments, selection, and caret remain transient until "
            "commit or cancel",
            "ime-composition", "frontend IME preedit operation",
            BOXRIG_EMITTER_CASE_FRONTEND, false, false, false,
        },
        run_frontend_ime_preedit,
    },
    {
        {
            "safe-no-op", "Recovery",
            "Emits printer and memory-lock terminfo controls as safe no-ops.",
            "all separator letters and SAFE NO-OP COMPLETE remain visible",
            "terminfo", "mc0/mc4/mc5/meml/memu",
            BOXRIG_EMITTER_CASE_OUTPUT, false, false, false,
        },
        run_safe_no_op,
    },
    {
        {
            "unsupported-recovery", "Recovery",
            "Emits representative unsupported DCS, APC, PM, and SOS strings.",
            "separator letters and RECOVERED remain visible with no response "
            "or host action",
            "unsupported", "DCS/APC/PM/SOS recovery",
            BOXRIG_EMITTER_CASE_OUTPUT, false, false, false,
        },
        run_unsupported_recovery,
    },
};

static const emitter_cleanup cleanup_cases[] = {
    {run_osc_titles, cleanup_osc_titles},
    {run_osc_palette, cleanup_osc_palette},
    {run_osc_dynamic_colors, cleanup_osc_dynamic_colors},
};

static int write_text(
    const boxrig_emitter_io *io,
    boxrig_emitter_stream stream,
    const char *text)
{
    return boxrig_emitter_write(
        io,
        stream,
        (const uint8_t *)text,
        strlen(text));
}

static int encode_base64(
    const uint8_t *input,
    size_t input_length,
    char *output,
    size_t output_capacity)
{
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t encoded_length;
    size_t input_offset = 0;
    size_t output_offset = 0;

    if (input == NULL || input_length == 0 ||
        input_length > (SIZE_MAX - 2) / 4 * 3) {
        return -1;
    }
    encoded_length = ((input_length + 2) / 3) * 4;
    if (encoded_length >= output_capacity) {
        return -1;
    }
    while (input_offset + 3 <= input_length) {
        uint32_t value = ((uint32_t)input[input_offset] << 16) |
                         ((uint32_t)input[input_offset + 1] << 8) |
                         (uint32_t)input[input_offset + 2];

        output[output_offset++] = alphabet[(value >> 18) & 0x3f];
        output[output_offset++] = alphabet[(value >> 12) & 0x3f];
        output[output_offset++] = alphabet[(value >> 6) & 0x3f];
        output[output_offset++] = alphabet[value & 0x3f];
        input_offset += 3;
    }
    if (input_offset < input_length) {
        uint32_t value = (uint32_t)input[input_offset] << 16;

        if (input_offset + 1 < input_length) {
            value |= (uint32_t)input[input_offset + 1] << 8;
        }
        output[output_offset++] = alphabet[(value >> 18) & 0x3f];
        output[output_offset++] = alphabet[(value >> 12) & 0x3f];
        output[output_offset++] = input_offset + 1 < input_length
                                      ? alphabet[(value >> 6) & 0x3f]
                                      : '=';
        output[output_offset++] = '=';
    }
    output[output_offset] = '\0';
    return output_offset == encoded_length ? 0 : -1;
}

static int encode_fixture_name(
    const uint8_t *input,
    size_t input_length,
    char *output,
    size_t output_capacity)
{
    if (input_length > BOXRIG_EMITTER_GRAPHICS_NAME_CAPACITY) {
        return -1;
    }
    return encode_base64(
        input,
        input_length,
        output,
        output_capacity);
}

static int make_graphics_scroll_pixels(
    uint8_t *rgba,
    size_t rgba_capacity)
{
    static const char *const pattern[GRAPHICS_SCROLL_HEIGHT] = {
        ".......W........",
        "......WWW.......",
        ".....WWWWW......",
        "....WWWWWWW.....",
        ".......O........",
        "...OOOOOOOOO....",
        "BBBBBBBBBBBBBBBB",
        ".BB.BB.BB.BB.BB.",
    };
    static const uint8_t sky[] = {12, 24, 48, 255};
    static const uint8_t sail[] = {240, 248, 255, 255};
    static const uint8_t hull[] = {240, 112, 32, 255};
    static const uint8_t water[] = {40, 120, 240, 255};
    size_t y;

    if (rgba == NULL || rgba_capacity < GRAPHICS_SCROLL_RGBA_LENGTH) {
        return -1;
    }
    for (y = 0; y < GRAPHICS_SCROLL_HEIGHT; y++) {
        size_t x;

        for (x = 0; x < GRAPHICS_SCROLL_WIDTH; x++) {
            const uint8_t *color = sky;
            size_t offset = (y * GRAPHICS_SCROLL_WIDTH + x) * 4;

            if (pattern[y][x] == 'W') {
                color = sail;
            } else if (pattern[y][x] == 'O') {
                color = hull;
            } else if (pattern[y][x] == 'B') {
                color = water;
            }
            memcpy(rgba + offset, color, 4);
        }
    }
    return 0;
}

static void graphics_rgba_to_rgb(
    const uint8_t rgba[BOXRIG_EMITTER_GRAPHICS_FIXTURE_RGBA_LENGTH],
    uint8_t rgb[BOXRIG_EMITTER_GRAPHICS_FIXTURE_WIDTH *
                BOXRIG_EMITTER_GRAPHICS_FIXTURE_HEIGHT * 3])
{
    size_t pixel;

    for (pixel = 0;
         pixel < BOXRIG_EMITTER_GRAPHICS_FIXTURE_WIDTH *
                     BOXRIG_EMITTER_GRAPHICS_FIXTURE_HEIGHT;
         pixel++) {
        memcpy(rgb + pixel * 3, rgba + pixel * 4, 3);
    }
}

/* A single stored DEFLATE block keeps the zlib fixture deterministic without
 * adding a compression-library dependency to the standalone emitter. */
static int make_stored_zlib(
    const uint8_t *input,
    size_t input_length,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_length)
{
    uint32_t first = 1;
    uint32_t second = 0;
    uint16_t length;
    uint16_t inverse_length;
    uint32_t checksum;
    size_t index;

    if (input == NULL || input_length == 0 || input_length > UINT16_MAX ||
        output == NULL || output_length == NULL ||
        output_capacity < input_length + 11) {
        return -1;
    }
    length = (uint16_t)input_length;
    inverse_length = (uint16_t)~length;
    output[0] = 0x78;
    output[1] = 0x01;
    output[2] = 0x01;
    output[3] = (uint8_t)(length & 0xffu);
    output[4] = (uint8_t)(length >> 8);
    output[5] = (uint8_t)(inverse_length & 0xffu);
    output[6] = (uint8_t)(inverse_length >> 8);
    memcpy(output + 7, input, input_length);
    for (index = 0; index < input_length; index++) {
        first = (first + input[index]) % 65521u;
        second = (second + first) % 65521u;
    }
    checksum = (second << 16) | first;
    output[7 + input_length] = (uint8_t)(checksum >> 24);
    output[8 + input_length] = (uint8_t)(checksum >> 16);
    output[9 + input_length] = (uint8_t)(checksum >> 8);
    output[10 + input_length] = (uint8_t)checksum;
    *output_length = input_length + 11;
    return 0;
}

static int encode_graphics_landscape(
    graphics_palette palette,
    char *encoded,
    size_t encoded_capacity)
{
    uint8_t rgba[BOXRIG_EMITTER_GRAPHICS_FIXTURE_RGBA_LENGTH];

    if (make_graphics_landscape(palette, rgba, sizeof(rgba)) == -1) {
        return -1;
    }
    return encode_base64(rgba, sizeof(rgba), encoded, encoded_capacity);
}

static int encode_graphics_right_half(
    graphics_palette palette,
    char *encoded,
    size_t encoded_capacity)
{
    enum {
        HALF_WIDTH = BOXRIG_EMITTER_GRAPHICS_FIXTURE_WIDTH / 2,
        HALF_RGBA_LENGTH =
            HALF_WIDTH * BOXRIG_EMITTER_GRAPHICS_FIXTURE_HEIGHT * 4
    };
    uint8_t rgba[BOXRIG_EMITTER_GRAPHICS_FIXTURE_RGBA_LENGTH];
    uint8_t half[HALF_RGBA_LENGTH];
    size_t row;

    if (make_graphics_landscape(palette, rgba, sizeof(rgba)) == -1) {
        return -1;
    }
    for (row = 0; row < BOXRIG_EMITTER_GRAPHICS_FIXTURE_HEIGHT; row++) {
        memcpy(
            half + row * HALF_WIDTH * 4,
            rgba +
                (row * BOXRIG_EMITTER_GRAPHICS_FIXTURE_WIDTH + HALF_WIDTH) *
                    4,
            HALF_WIDTH * 4);
    }
    return encode_base64(half, sizeof(half), encoded, encoded_capacity);
}

static const char *class_name(boxrig_emitter_case_class case_class)
{
    switch (case_class) {
    case BOXRIG_EMITTER_CASE_OUTPUT:
        return "output";
    case BOXRIG_EMITTER_CASE_ROUND_TRIP:
        return "round-trip";
    case BOXRIG_EMITTER_CASE_INTERACTIVE:
        return "interactive";
    case BOXRIG_EMITTER_CASE_FRONTEND:
        return "frontend";
    }
    return "unknown";
}

static const emitter_case *find_case(const char *name)
{
    size_t index;

    for (index = 0; index < sizeof(cases) / sizeof(cases[0]); index++) {
        if (strcmp(cases[index].info.name, name) == 0) {
            return &cases[index];
        }
    }
    return NULL;
}

static case_runner find_cleanup(const emitter_case *test_case)
{
    size_t index;

    for (index = 0;
         index < sizeof(cleanup_cases) / sizeof(cleanup_cases[0]);
         index++) {
        if (cleanup_cases[index].run == test_case->run) {
            return cleanup_cases[index].cleanup;
        }
    }
    return NULL;
}

static int write_case_frame_start(
    const boxrig_emitter_io *io,
    const boxrig_emitter_case_info *info)
{
    static const uint8_t banner[] = "boxrig CLI test emitter.\r\nTEST ";
    static const uint8_t expectation[] = "\r\nEXPECT: ";
    static const uint8_t begin[] = "\r\nBEGIN ----------\r\n";

    if (boxrig_emitter_write(
            io,
            BOXRIG_EMITTER_STANDARD,
            banner,
            sizeof(banner) - 1) == -1 ||
        write_text(io, BOXRIG_EMITTER_STANDARD, info->name) == -1 ||
        boxrig_emitter_write(
            io,
            BOXRIG_EMITTER_STANDARD,
            expectation,
            sizeof(expectation) - 1) == -1 ||
        write_text(io, BOXRIG_EMITTER_STANDARD, info->expectation) == -1 ||
        boxrig_emitter_write(
            io,
            BOXRIG_EMITTER_STANDARD,
            begin,
            sizeof(begin) - 1) == -1) {
        return -1;
    }
    return 0;
}

static int write_case_frame_end(const boxrig_emitter_io *io)
{
    static const uint8_t end[] = "END ------------\r\n";

    return boxrig_emitter_write(
        io,
        BOXRIG_EMITTER_STANDARD,
        end,
        sizeof(end) - 1);
}

static int run_framed_payload(
    const boxrig_emitter_io *io,
    const char *name,
    const uint8_t *payload,
    size_t payload_length)
{
    const emitter_case *test_case = find_case(name);

    if (test_case == NULL ||
        write_case_frame_start(io, &test_case->info) == -1 ||
        boxrig_emitter_write(
            io,
            BOXRIG_EMITTER_STANDARD,
            payload,
            payload_length) == -1 ||
        write_case_frame_end(io) == -1) {
        return -1;
    }
    return 0;
}

static int run_screen_payload(
    const boxrig_emitter_io *io,
    const char *name,
    const uint8_t *payload,
    size_t payload_length)
{
    static const uint8_t banner[] = "boxrig CLI test emitter.\r\nTEST ";
    static const uint8_t expectation[] = "\r\nEXPECT: ";
    static const uint8_t action[] =
        "\r\nACTION: applying a dedicated full-screen canvas.\r\n";
    const emitter_case *test_case = find_case(name);

    if (test_case == NULL ||
        boxrig_emitter_write(
            io,
            BOXRIG_EMITTER_STANDARD,
            banner,
            sizeof(banner) - 1) == -1 ||
        write_text(io, BOXRIG_EMITTER_STANDARD, test_case->info.name) == -1 ||
        boxrig_emitter_write(
            io,
            BOXRIG_EMITTER_STANDARD,
            expectation,
            sizeof(expectation) - 1) == -1 ||
        write_text(
            io,
            BOXRIG_EMITTER_STANDARD,
            test_case->info.expectation) == -1 ||
        boxrig_emitter_write(
            io,
            BOXRIG_EMITTER_STANDARD,
            action,
            sizeof(action) - 1) == -1 ||
        boxrig_emitter_write(
            io,
            BOXRIG_EMITTER_STANDARD,
            payload,
            payload_length) == -1) {
        return -1;
    }
    return 0;
}

static int run_prefixed_framed_payload(
    const boxrig_emitter_io *io,
    const char *name,
    const uint8_t *prefix,
    size_t prefix_length,
    const uint8_t *payload,
    size_t payload_length)
{
    const emitter_case *test_case = find_case(name);

    if (test_case == NULL ||
        boxrig_emitter_write(
            io,
            BOXRIG_EMITTER_STANDARD,
            prefix,
            prefix_length) == -1 ||
        write_case_frame_start(io, &test_case->info) == -1 ||
        boxrig_emitter_write(
            io,
            BOXRIG_EMITTER_STANDARD,
            payload,
            payload_length) == -1 ||
        write_case_frame_end(io) == -1) {
        return -1;
    }
    return 0;
}

static int write_visible_response_byte(
    const boxrig_emitter_io *io,
    uint8_t byte)
{
    const char *token = NULL;
    uint8_t hexadecimal[6];
    static const char digits[] = "0123456789ABCDEF";

    switch (byte) {
    case 0x00:
        token = "<NUL>";
        break;
    case 0x07:
        token = "<BEL>";
        break;
    case 0x08:
        token = "<BS>";
        break;
    case 0x09:
        token = "<HT>";
        break;
    case 0x0a:
        token = "<LF>";
        break;
    case 0x0b:
        token = "<VT>";
        break;
    case 0x0c:
        token = "<FF>";
        break;
    case 0x0d:
        token = "<CR>";
        break;
    case 0x1b:
        token = "<ESC>";
        break;
    case 0x7f:
        token = "<DEL>";
        break;
    default:
        break;
    }
    if (token != NULL) {
        return write_text(io, BOXRIG_EMITTER_STANDARD, token);
    }
    if (byte >= 0x20 && byte <= 0x7e) {
        return boxrig_emitter_write(
            io,
            BOXRIG_EMITTER_STANDARD,
            &byte,
            1);
    }
    hexadecimal[0] = (uint8_t)'<';
    hexadecimal[1] = (uint8_t)'0';
    hexadecimal[2] = (uint8_t)'x';
    hexadecimal[3] = (uint8_t)digits[byte >> 4];
    hexadecimal[4] = (uint8_t)digits[byte & 0x0f];
    hexadecimal[5] = (uint8_t)'>';
    return boxrig_emitter_write(
        io,
        BOXRIG_EMITTER_STANDARD,
        hexadecimal,
        sizeof(hexadecimal));
}

static bool response_is_complete(
    response_terminator terminator,
    uint8_t byte,
    int *state)
{
    if (terminator == RESPONSE_CSI) {
        if (*state == 0) {
            *state = byte == 0x1b ? 1 : 0;
        } else if (*state == 1) {
            *state = byte == (uint8_t)'[' ? 2 : (byte == 0x1b ? 1 : 0);
        } else if (byte >= 0x40 && byte <= 0x7e) {
            return true;
        }
        return false;
    }
    if (terminator == RESPONSE_OSC && byte == 0x07) {
        return true;
    }
    if (*state == 1 && byte == (uint8_t)'\\') {
        return true;
    }
    *state = byte == 0x1b ? 1 : 0;
    return false;
}

static int read_visible_response(
    const boxrig_emitter_io *io,
    response_terminator terminator,
    size_t response_limit)
{
    size_t count;
    int state = 0;

    for (count = 0; count < response_limit; count++) {
        uint8_t byte;
        int read_status = io->read_byte(io->context, &byte);

        if (read_status != 1 ||
            write_visible_response_byte(io, byte) == -1) {
            return -1;
        }
        if (response_is_complete(terminator, byte, &state)) {
            return 0;
        }
    }
    return -1;
}

static int run_round_trip_case(
    const boxrig_emitter_io *io,
    const char *name,
    const query_step *steps,
    size_t step_count)
{
    static const uint8_t query_label[] = "QUERY ";
    static const uint8_t response_label[] = "\r\nRESPONSE: ";
    static const uint8_t newline[] = "\r\n";
    const emitter_case *test_case = find_case(name);
    size_t index;
    int status = -1;

    if (test_case == NULL || io->read_byte == NULL ||
        io->capture_begin == NULL || io->capture_end == NULL ||
        write_case_frame_start(io, &test_case->info) == -1 ||
        io->capture_begin(io->context) == -1) {
        return -1;
    }
    for (index = 0; index < step_count; index++) {
        if (boxrig_emitter_write(
                io,
                BOXRIG_EMITTER_STANDARD,
                query_label,
                sizeof(query_label) - 1) == -1 ||
            write_text(io, BOXRIG_EMITTER_STANDARD, steps[index].label) == -1 ||
            boxrig_emitter_write(
                io,
                BOXRIG_EMITTER_STANDARD,
                newline,
                sizeof(newline) - 1) == -1 ||
            boxrig_emitter_write(
                io,
                BOXRIG_EMITTER_STANDARD,
                steps[index].request,
                steps[index].request_length) == -1 ||
            boxrig_emitter_write(
                io,
                BOXRIG_EMITTER_STANDARD,
                response_label,
                sizeof(response_label) - 1) == -1 ||
            read_visible_response(
                io,
                steps[index].terminator,
                steps[index].response_limit) == -1 ||
            boxrig_emitter_write(
                io,
                BOXRIG_EMITTER_STANDARD,
                newline,
                sizeof(newline) - 1) == -1) {
            goto cleanup;
        }
    }
    status = 0;

cleanup:
    if (io->capture_end(io->context) == -1) {
        status = -1;
    }
    if (status == 0 && write_case_frame_end(io) == -1) {
        status = -1;
    }
    return status;
}

/* Interactive fixtures detect only CSI/SS3 event boundaries. The terminal
 * core remains responsible for validating and producing protocol semantics. */
static bool interactive_sequence_is_complete(uint8_t byte, int *state)
{
    if (*state == 0) {
        *state = byte == 0x1b ? 1 : 0;
    } else if (*state == 1) {
        if (byte == (uint8_t)'[') {
            *state = 2;
        } else if (byte == (uint8_t)'O') {
            *state = 3;
        } else {
            *state = byte == 0x1b ? 1 : 0;
        }
    } else if (byte >= 0x40 && byte <= 0x7e) {
        *state = 0;
        return true;
    }
    return false;
}

static int read_interactive_input(
    const boxrig_emitter_io *io,
    interactive_terminator terminator,
    size_t sequence_count,
    size_t input_limit,
    uint8_t captured[INTERACTIVE_CAPTURE_CAPACITY],
    size_t *captured_length_out)
{
    static const uint8_t paste_end[] = "\x1b[201~";
    size_t completed_sequences = 0;
    size_t count;
    int sequence_state = 0;

    if (input_limit > INTERACTIVE_CAPTURE_CAPACITY ||
        (terminator == INTERACTIVE_SEQUENCE && sequence_count == 0)) {
        return -1;
    }
    for (count = 0; count < input_limit; count++) {
        int read_status = io->read_byte(io->context, &captured[count]);

        if (read_status != 1) {
            return -1;
        }
        *captured_length_out = count + 1;
        if (terminator == INTERACTIVE_SEQUENCE &&
            interactive_sequence_is_complete(
                captured[count],
                &sequence_state)) {
            completed_sequences++;
            if (completed_sequences == sequence_count) {
                return 0;
            }
        } else if (terminator == INTERACTIVE_PASTE &&
                   count + 1 >= sizeof(paste_end) - 1 &&
                   memcmp(
                       captured + count + 1 - (sizeof(paste_end) - 1),
                       paste_end,
                       sizeof(paste_end) - 1) == 0) {
            return 0;
        } else if (terminator == INTERACTIVE_LINE &&
                   (captured[count] == (uint8_t)'\r' ||
                    captured[count] == (uint8_t)'\n')) {
            return 0;
        }
    }
    return -1;
}

static int run_interactive_case(
    const boxrig_emitter_io *io,
    const char *name,
    const char *instruction,
    const uint8_t *enable,
    size_t enable_length,
    const uint8_t *disable,
    size_t disable_length,
    interactive_terminator terminator,
    size_t sequence_count,
    size_t input_limit)
{
    static const uint8_t instruction_label[] = "ACTION: ";
    static const uint8_t input_label[] = "INPUT: ";
    static const uint8_t newline[] = "\r\n";
    uint8_t captured[INTERACTIVE_CAPTURE_CAPACITY];
    const emitter_case *test_case = find_case(name);
    size_t captured_length = 0;
    size_t index;
    bool disable_required = false;
    int status = -1;

    if (test_case == NULL || io->read_byte == NULL ||
        io->capture_begin == NULL || io->capture_end == NULL ||
        write_case_frame_start(io, &test_case->info) == -1 ||
        boxrig_emitter_write(
            io,
            BOXRIG_EMITTER_STANDARD,
            instruction_label,
            sizeof(instruction_label) - 1) == -1 ||
        write_text(io, BOXRIG_EMITTER_STANDARD, instruction) == -1 ||
        boxrig_emitter_write(
            io,
            BOXRIG_EMITTER_STANDARD,
            newline,
            sizeof(newline) - 1) == -1 ||
        io->capture_begin(io->context) == -1) {
        return -1;
    }
    if (enable_length != 0) {
        disable_required = true;
        if (boxrig_emitter_write(
                io,
                BOXRIG_EMITTER_STANDARD,
                enable,
                enable_length) == -1) {
            goto cleanup;
        }
    }
    if (read_interactive_input(
            io,
            terminator,
            sequence_count,
            input_limit,
            captured,
            &captured_length) == 0) {
        status = 0;
    }

cleanup:
    if (disable_required &&
        boxrig_emitter_write(
            io,
            BOXRIG_EMITTER_STANDARD,
            disable,
            disable_length) == -1) {
        status = -1;
    }
    if (io->capture_end(io->context) == -1) {
        status = -1;
    }
    if (status != 0 ||
        boxrig_emitter_write(
            io,
            BOXRIG_EMITTER_STANDARD,
            input_label,
            sizeof(input_label) - 1) == -1) {
        return -1;
    }
    for (index = 0; index < captured_length; index++) {
        if (write_visible_response_byte(io, captured[index]) == -1) {
            return -1;
        }
    }
    if (boxrig_emitter_write(
            io,
            BOXRIG_EMITTER_STANDARD,
            newline,
            sizeof(newline) - 1) == -1 ||
        write_case_frame_end(io) == -1) {
        return -1;
    }
    return 0;
}

static int run_backspace(const boxrig_emitter_io *io)
{
    static const uint8_t payload[] = {'A', 'B', 'C', 0x08, ' ', 0x08, '\r', '\n'};

    if (write_case_frame_start(io, &cases[0].info) == -1 ||
        boxrig_emitter_write(
            io,
            BOXRIG_EMITTER_STANDARD,
            payload,
            sizeof(payload)) == -1 ||
        write_case_frame_end(io) == -1) {
        return -1;
    }
    return 0;
}

static int run_transport_bytes(const boxrig_emitter_io *io)
{
    static const uint8_t payload[] =
        "C0:A\aB\bC\x18" "D\r\n"
        "\033[31mCSI\033[0m\r\n"
        "\033]2;BOXRIG\a\r\n"
        "\033P$qm\033\\\r\n"
        "\033_Gi=1;APC\033\\\r\n"
        "UTF8:\xe7\x95\x8c" "\xf0\x9f\x91\xa9\r\n"
        "INVALID:\xf0\x28\x8c\x28\r\n";

    if (write_case_frame_start(io, &cases[1].info) == -1 ||
        boxrig_emitter_write(
            io,
            BOXRIG_EMITTER_STANDARD,
            payload,
            sizeof(payload) - 1) == -1 ||
        write_case_frame_end(io) == -1) {
        return -1;
    }
    return 0;
}

static int run_text_ascii(const boxrig_emitter_io *io)
{
    static const uint8_t payload[] =
        "ASCII: The quick brown fox 0123456789.\r\n";

    return run_framed_payload(
        io,
        "text-ascii",
        payload,
        sizeof(payload) - 1);
}

static int run_text_cjk(const boxrig_emitter_io *io)
{
    static const uint8_t payload[] =
        "Japanese: \xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\r\n"
        "Simplified Chinese: "
        "\xe7\xae\x80\xe4\xbd\x93\xe4\xb8\xad\xe6\x96\x87\r\n"
        "Traditional Chinese: "
        "\xe7\xb9\x81\xe9\xab\x94\xe4\xb8\xad\xe6\x96\x87\r\n"
        "Korean: \xed\x95\x9c\xea\xb5\xad\xec\x96\xb4\r\n"
        "Wide boundary: 123456789|\xe7\x95\x8c|\r\n";

    return run_framed_payload(
        io,
        "text-cjk",
        payload,
        sizeof(payload) - 1);
}

static int run_text_combining(const boxrig_emitter_io *io)
{
    static const uint8_t payload[] =
        "One mark: e\xcc\x81\r\n"
        "Multiple marks: a\xcc\x81\xcc\xa7\r\n"
        "Text variation selector: \xe2\x9d\xa4\xef\xb8\x8e\r\n";

    return run_framed_payload(
        io,
        "text-combining",
        payload,
        sizeof(payload) - 1);
}

static int run_text_long_grapheme(const boxrig_emitter_io *io)
{
    uint8_t payload[5003];
    size_t offset;

    payload[0] = (uint8_t)'e';
    for (offset = 1; offset < 5001; offset += 2) {
        payload[offset] = 0xcc;
        payload[offset + 1] = 0x81;
    }
    payload[5001] = (uint8_t)'\r';
    payload[5002] = (uint8_t)'\n';
    return run_framed_payload(
        io,
        "text-long-grapheme",
        payload,
        sizeof(payload));
}

static int run_text_ligatures(const boxrig_emitter_io *io)
{
    static const uint8_t payload[] =
        "ffi fi fl -> => != === | \xef\xac\x81 \xef\xac\x83\r\n";

    return run_framed_payload(
        io,
        "text-ligatures",
        payload,
        sizeof(payload) - 1);
}

static int run_text_complex_scripts(const boxrig_emitter_io *io)
{
    static const uint8_t payload[] =
        "Arabic: \xd8\xb3\xd9\x84\xd8\xa7\xd9\x85\r\n"
        "Hebrew: \xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d\r\n"
        "Devanagari: "
        "\xe0\xa4\xa8\xe0\xa4\xae\xe0\xa4\xb8\xe0\xa5\x8d"
        "\xe0\xa4\xa4\xe0\xa5\x87\r\n"
        "Thai: \xe0\xb9\x84\xe0\xb8\x97\xe0\xb8\xa2\r\n";

    return run_framed_payload(
        io,
        "text-complex-scripts",
        payload,
        sizeof(payload) - 1);
}

static int run_text_bidi(const boxrig_emitter_io *io)
{
    static const uint8_t payload[] =
        "Hebrew mixed: A \xd7\x90\xd7\x91\xd7\x92 B\r\n"
        "Arabic mixed: C \xd8\xb3\xd9\x84\xd8\xa7\xd9\x85 D\r\n";

    return run_framed_payload(
        io,
        "text-bidi",
        payload,
        sizeof(payload) - 1);
}

static int run_text_emoji(const boxrig_emitter_io *io)
{
    static const uint8_t payload[] =
        "Emoji: \xf0\x9f\x98\x80\r\n"
        "Emoji presentation: \xe2\x9d\xa4\xef\xb8\x8f\r\n"
        "Skin tone: \xf0\x9f\x91\x8d\xf0\x9f\x8f\xbd\r\n"
        "Regional flag: \xf0\x9f\x87\xaf\xf0\x9f\x87\xb5\r\n"
        "Keycap: 1\xef\xb8\x8f\xe2\x83\xa3\r\n"
        "ZWJ profession: "
        "\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x92\xbb\r\n"
        "ZWJ family: "
        "\xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x91\xa9"
        "\xe2\x80\x8d\xf0\x9f\x91\xa7\xe2\x80\x8d"
        "\xf0\x9f\x91\xa6\r\n";

    return run_framed_payload(
        io,
        "text-emoji",
        payload,
        sizeof(payload) - 1);
}

static int run_text_ambiguous_width(const boxrig_emitter_io *io)
{
    static const uint8_t payload[] =
        "Ruler:     12345678901234567890\r\n"
        "Ambiguous: |\xc2\xb7|\xce\xa9|\r\n";

    return run_framed_payload(
        io,
        "text-ambiguous-width",
        payload,
        sizeof(payload) - 1);
}

static int run_text_invalid_utf8(const boxrig_emitter_io *io)
{
    static const uint8_t payload[] =
        "Malformed: A\xf0(\x8c(B\r\n"
        "Truncated before control: X\xe3\r\n"
        "RECOVERED after malformed and truncated UTF-8\r\n";

    return run_framed_payload(
        io,
        "text-invalid-utf8",
        payload,
        sizeof(payload) - 1);
}

static int run_c0_controls(const boxrig_emitter_io *io)
{
    static const uint8_t payload[] =
        "BEL: host event\a queued\r\n"
        "BS: ABC\b \b\r\n"
        "HT: A\tB\r\n"
        "LF: A\nB\r\n"
        "VT: A\vB\r\n"
        "FF: A\fB\r\n"
        "CR: ABC\rXY\r\n"
        "DEL: A\x7f" "B\r\n"
        "C0 COMPLETE\r\n";

    return run_framed_payload(
        io,
        "c0-controls",
        payload,
        sizeof(payload) - 1);
}

static int run_cancel_recovery(const boxrig_emitter_io *io)
{
    static const uint8_t payload[] =
        "CSI cancelled: \x1b[31\x18RECOVERED-CAN\r\n"
        "OSC cancelled: \x1b]2;discarded\x1aRECOVERED-SUB\r\n";

    return run_framed_payload(
        io,
        "cancel-recovery",
        payload,
        sizeof(payload) - 1);
}

static int run_esc_indexing(const boxrig_emitter_io *io)
{
    static const uint8_t payload[] =
        "ESC indexing ruler: rows 8 through 16\r\n"
        "\x1b[8;5HD\x1b" "D" "I"
        "\x1b" "E" "E"
        "\x1b[12;10H\x1b" "H\r\tT"
        "\x1b[16;5H\x1b" "M" "R"
        "\x1b[12;10H\x1b[g"
        "\x1b[18;1HESC INDEX COMPLETE\r\n";

    return run_framed_payload(
        io,
        "esc-indexing",
        payload,
        sizeof(payload) - 1);
}

static int run_character_sets(const boxrig_emitter_io *io)
{
    static const uint8_t payload[] =
        "G0 DEC then ASCII: \x1b(0q\x1b(Bq\r\n"
        "G1 DEC through SO/SI: \x1b)0\x0e" "q\x0f" "q\x1b)B\r\n"
        "CHARACTER SETS COMPLETE\r\n";

    return run_framed_payload(
        io,
        "character-sets",
        payload,
        sizeof(payload) - 1);
}

static int run_cursor_movement(const boxrig_emitter_io *io)
{
    static const uint8_t payload[] =
        "Cursor canvas uses letter markers.\r\n"
        "\x1b[10;10HO"
        "\x1b[2AA"
        "\x1b[2BB"
        "\x1b[2CC"
        "\x1b[2DD"
        "\x1b[2EE"
        "\x1b[2FF"
        "\x1b[5GG"
        "\x1b[5dV"
        "\x1b[4;7HH"
        "\x1b[5;8ff"
        "\x1b[20;1HCURSOR MOVEMENT COMPLETE\r\n";

    return run_framed_payload(
        io,
        "cursor-movement",
        payload,
        sizeof(payload) - 1);
}

static int run_character_editing(const boxrig_emitter_io *io)
{
    static const uint8_t payload[] =
        "\x1b[8;1HABCDEFGH\x1b[8;3H\x1b[2@"
        "\x1b[10;1H12345678\x1b[10;3H\x1b[2P"
        "\x1b[12;1Habcdefgh\x1b[12;3H\x1b[3X"
        "\x1b[14;1HR\x1b[4b"
        "\x1b[16;1HCHARACTER EDITING COMPLETE\r\n";

    return run_framed_payload(
        io,
        "character-editing",
        payload,
        sizeof(payload) - 1);
}

static int run_line_scrolling(const boxrig_emitter_io *io)
{
    static const uint8_t payload[] =
        "\x1b[8;1HROW-08\x1b[9;1HROW-09\x1b[10;1HROW-10"
        "\x1b[11;1HROW-11\x1b[12;1HROW-12\x1b[13;1HROW-13"
        "\x1b[8;16r"
        "\x1b[10;1H\x1b[2L"
        "\x1b[12;1H\x1b[2M"
        "\x1b[2S\x1b[2T\x1b[2^"
        "\x1b[r\x1b[18;1HLINE SCROLLING COMPLETE\r\n";

    return run_framed_payload(
        io,
        "line-scrolling",
        payload,
        sizeof(payload) - 1);
}

static int run_tab_stops(const boxrig_emitter_io *io)
{
    static const uint8_t payload[] =
        "\x1b[3g"
        "\x1b[8;6H\x1b" "H"
        "\x1b[8;1H\tCUSTOM"
        "\x1b[8;6H\x1b[g"
        "\x1b[9;1H\tRIGHT-BOUNDARY"
        "\x1b" "c"
        "TAB STOPS COMPLETE; RIS RESTORED DEFAULT STOPS.\r\n";

    return run_screen_payload(
        io,
        "tab-stops",
        payload,
        sizeof(payload) - 1);
}

static int run_erase_display(const boxrig_emitter_io *io)
{
    static const uint8_t payload[] =
        "\x1b[2J"
        "\x1b[6;1HED2 cleared the disposable canvas"
        "\x1b[1;1H1111111111\x1b[1;6H\x1b[1J"
        "\x1b[3;1HED1 leaves the row-1 suffix"
        "\x1b[14;1H0000000000\x1b[14;6H\x1b[J"
        "\x1b[12;1HED0 leaves the row-14 prefix"
        "\x1b[3J"
        "\x1b[16;1HED3 retained the visible markers\r\n"
        "ERASE DISPLAY COMPLETE\r\n";

    return run_screen_payload(
        io,
        "erase-display",
        payload,
        sizeof(payload) - 1);
}

static int run_erase_line(const boxrig_emitter_io *io)
{
    static const uint8_t payload[] =
        "\x1b[8;1HABCDEFGHIJ\x1b[8;6H\x1b[K"
        "\x1b[10;1HABCDEFGHIJ\x1b[10;6H\x1b[1K"
        "\x1b[12;1HABCDEFGHIJ\x1b[12;6H\x1b[2K"
        "\x1b[14;1HERASE LINE COMPLETE\r\n";

    return run_framed_payload(
        io,
        "erase-line",
        payload,
        sizeof(payload) - 1);
}

static int run_selective_erase(const boxrig_emitter_io *io)
{
    static const uint8_t payload[] =
        "\x1b[2J\x1b[H"
        "\x1b[1\"qP\x1b[0\"qU\x1b[2\"qV"
        "\x1b[H\x1b[?K"
        "\x1b[3;1H\x1b[1\"qP\x1b[0\"qU\x1b[2\"qV"
        "\x1b[3;2H\x1b[?1K"
        "\x1b[5;1H\x1b[1\"qP\x1b[0\"qU\x1b[2\"qV"
        "\x1b[5;1H\x1b[?2K"
        "\x1b[7;1H\x1b[1\"qP\x1b[0\"qU\x1b[2\"qV"
        "\x1b[7;2H\x1b[?J"
        "\x1b[9;1H\x1b[1\"qP\x1b[0\"qU\x1b[2\"qV"
        "\x1b[9;2H\x1b[?1J"
        "\x1b[11;1H\x1b[1\"qP\x1b[0\"qU\x1b[2\"qV"
        "\x1b[H\x1b[?2J"
        "\x1b[14;1HSELECTIVE ERASE COMPLETE\r\n";

    return run_screen_payload(
        io,
        "selective-erase",
        payload,
        sizeof(payload) - 1);
}

static int run_margins(const boxrig_emitter_io *io)
{
    static const uint8_t payload[] =
        "\x1b[2J"
        "\x1b[1;1HTOP OUTSIDE"
        "\x1b[3;1H03|abcdefghijklmnopqrstuvwxyz|"
        "\x1b[4;1H04|ABCDEFGHIJKLMNOPQRSTUVWXYZ|"
        "\x1b[5;1H05|01234567890123456789012345|"
        "\x1b[10;1H10|bottom margin-------------|"
        "\x1b[12;1HBOTTOM OUTSIDE"
        "\x1b[3;10r\x1b[?69h\x1b[5;30s\x1b[?6h"
        "\x1b[8;1Hinside\nscroll"
        "\x1b[?6l\x1b[?69l\x1b[r"
        "\x1b[15;1HMARGINS COMPLETE\r\n";

    return run_screen_payload(
        io,
        "margins",
        payload,
        sizeof(payload) - 1);
}

static int run_save_restore_soft_reset(const boxrig_emitter_io *io)
{
    static const uint8_t payload[] =
        "\x1b[8;5H\x1b" "7\x1b[10;20HX\x1b" "8R"
        "\x1b[12;5H\x1b[s\x1b[14;20HY\x1b[uS"
        "\x1b[3;10r\x1b[?6h\x1b[4h\x1b[?1h\x1b=\x1b[31;1m"
        "\x1b[!p"
        "\x1b[16;1HSAVE RESTORE SOFT RESET COMPLETE\r\n";

    return run_framed_payload(
        io,
        "save-restore-soft-reset",
        payload,
        sizeof(payload) - 1);
}

static int run_terminal_reset(const boxrig_emitter_io *io)
{
    static const uint8_t payload[] =
        "\x1b[2J\x1b[H"
        "DISPOSABLE RIS SETUP\r\n"
        "\x1b[31;1mred bold\x1b[?25l\x1b[?1049hALTERNATE"
        "\x1b" "c"
        "RESET COMPLETE\r\n"
        "Cursor, colors, modes, margins, and both screens are reset.\r\n";

    return run_screen_payload(
        io,
        "terminal-reset",
        payload,
        sizeof(payload) - 1);
}

static int run_standard_modes(const boxrig_emitter_io *io)
{
    static const uint8_t payload[] =
        "Insert mode: abcd\r\x1b[4hXY\x1b[4l\r\n"
        "Newline mode: A\x1b[20h\nB\x1b[20l\r\n"
        "STANDARD MODES COMPLETE\r\n";

    return run_framed_payload(
        io,
        "standard-modes",
        payload,
        sizeof(payload) - 1);
}

static int run_private_modes(const boxrig_emitter_io *io)
{
    static const uint8_t payload[] =
        "\x1b[?1h\x1b[?1l"
        "\x1b[?5h\x1b[?5l"
        "\x1b[?6h\x1b[?6l"
        "\x1b[?7h\x1b[?7l"
        "\x1b[?9h\x1b[?9l"
        "\x1b[?12l\x1b[?12h"
        "\x1b[?25l\x1b[?25h"
        "\x1b[?66h\x1b[?66l\x1b=\x1b>"
        "\x1b[?69h\x1b[?69l"
        "\x1b[?1000h\x1b[?1000l"
        "\x1b[?1002h\x1b[?1002l"
        "\x1b[?1003h\x1b[?1003l"
        "\x1b[?1004h\x1b[?1004l"
        "\x1b[?1006h\x1b[?1006l"
        "\x1b[?1034h\x1b[?1034l"
        "\x1b[?2004h\x1b[?2004l"
        "PRIVATE MODES COMPLETE\r\n";

    return run_framed_payload(
        io,
        "private-modes",
        payload,
        sizeof(payload) - 1);
}

static int run_alternate_screen_modes(const boxrig_emitter_io *io)
{
    static const uint8_t payload[] =
        "PRIMARY START\r\n"
        "\x1b[?47hALT-47\x1b[?47lPRIMARY AFTER 47\r\n"
        "\x1b[?1047hALT-1047\x1b[?1047lPRIMARY AFTER 1047\r\n"
        "\x1b[12;20H\x1b[?1048h\x1b[2;2H\x1b[?1048lS"
        "\x1b[?1049hALT-1049\x1b[?1049l"
        "\x1b[16;1HALTERNATE SCREEN MODES COMPLETE\r\n";

    return run_framed_payload(
        io,
        "alternate-screen-modes",
        payload,
        sizeof(payload) - 1);
}

static int run_cursor_style_protection(const boxrig_emitter_io *io)
{
    static const uint8_t payload[] =
        "\x1b[0 q0 default block\r\n"
        "\x1b[1 q1 blinking block\r\n"
        "\x1b[2 q2 steady block\r\n"
        "\x1b[3 q3 blinking underline\r\n"
        "\x1b[4 q4 steady underline\r\n"
        "\x1b[5 q5 blinking bar\r\n"
        "\x1b[6 q6 steady bar\r\n"
        "\x1b[16;1H\x1b[1\"qP\x1b[0\"qU\x1b[2\"qV"
        "\x1b[16;1H\x1b[?2K\x1b[0\"q\x1b[0 q"
        "\x1b[18;1HCURSOR STYLE PROTECTION COMPLETE\r\n";

    return run_framed_payload(
        io,
        "cursor-style-protection",
        payload,
        sizeof(payload) - 1);
}

static int run_title_stack(const boxrig_emitter_io *io)
{
    static const uint8_t payload[] =
        "combined push\x1b[22;0;0t / pop\x1b[23;0;0t\r\n"
        "icon push\x1b[22;1;0t / pop\x1b[23;1;0t\r\n"
        "window push\x1b[22;2;0t / pop\x1b[23;2;0t\r\n"
        "TITLE STACK COMPLETE\r\n";

    return run_framed_payload(
        io,
        "title-stack",
        payload,
        sizeof(payload) - 1);
}

static int run_sgr_attributes(const boxrig_emitter_io *io)
{
    static const uint8_t payload[] =
        "\x1b[1mB:bold\x1b[22m\r\n"
        "\x1b[2mF:faint\x1b[22m\r\n"
        "\x1b[3mI:italic\x1b[23m\r\n"
        "\x1b[4mU:underline\x1b[24m\r\n"
        "\x1b[21mD:double underline\x1b[24m\r\n"
        "\x1b[5mL:blink\x1b[25m\r\n"
        "\x1b[6mR:rapid blink\x1b[25m\r\n"
        "\x1b[7mV:inverse\x1b[27m\r\n"
        "\x1b[8mC:concealed\x1b[28m\r\n"
        "\x1b[9mS:strikethrough\x1b[29m\r\n"
        "\x1b[0mN:reset\r\n"
        "\x1b[4:1m1:single colon\x1b[4:0m\r\n"
        "\x1b[4:2m2:double colon\x1b[4:0m\r\n"
        "\x1b[4:3m3:curly\x1b[4:0m\r\n"
        "\x1b[4:4m4:dotted\x1b[4:0m\r\n"
        "\x1b[4:5m5:dashed\x1b[4:0m\r\n";

    return run_framed_payload(
        io,
        "sgr-attributes",
        payload,
        sizeof(payload) - 1);
}

static int run_sgr_ansi_colors(const boxrig_emitter_io *io)
{
    static const uint8_t payload[] =
        "\x1b[30m0\x1b[0m\x1b[31m1\x1b[0m\x1b[32m2\x1b[0m"
        "\x1b[33m3\x1b[0m\x1b[34m4\x1b[0m\x1b[35m5\x1b[0m"
        "\x1b[36m6\x1b[0m\x1b[37m7\x1b[0m\r\n"
        "\x1b[90m0\x1b[0m\x1b[91m1\x1b[0m\x1b[92m2\x1b[0m"
        "\x1b[93m3\x1b[0m\x1b[94m4\x1b[0m\x1b[95m5\x1b[0m"
        "\x1b[96m6\x1b[0m\x1b[97m7\x1b[0m\r\n"
        "\x1b[40m0\x1b[0m\x1b[41m1\x1b[0m\x1b[42m2\x1b[0m"
        "\x1b[43m3\x1b[0m\x1b[44m4\x1b[0m\x1b[45m5\x1b[0m"
        "\x1b[46m6\x1b[0m\x1b[47m7\x1b[0m\r\n"
        "\x1b[100m0\x1b[0m\x1b[101m1\x1b[0m\x1b[102m2\x1b[0m"
        "\x1b[103m3\x1b[0m\x1b[104m4\x1b[0m\x1b[105m5\x1b[0m"
        "\x1b[106m6\x1b[0m\x1b[107m7\x1b[0m\r\n";

    return run_framed_payload(
        io,
        "sgr-ansi-colors",
        payload,
        sizeof(payload) - 1);
}

static int run_sgr_extended_colors(const boxrig_emitter_io *io)
{
    static const uint8_t payload[] =
        "\x1b[38;5;123mF:indexed foreground\x1b[0m\r\n"
        "\x1b[48;5;124mB:indexed background\x1b[0m\r\n"
        "\x1b[4;58;5;125mU:indexed underline\x1b[0m\r\n"
        "\x1b[38;2;1;2;3mF:RGB foreground\x1b[0m\r\n"
        "\x1b[48;2;4;5;6mB:RGB background\x1b[0m\r\n"
        "\x1b[4;58;2;7;8;9mU:RGB underline\x1b[0m\r\n"
        "\x1b[38:5:130mF:colon indexed foreground\x1b[0m\r\n"
        "\x1b[48:5:131mB:colon indexed background\x1b[0m\r\n"
        "\x1b[4;58:5:132mU:colon indexed underline\x1b[0m\r\n"
        "\x1b[38:2::10:20:30mF:colon RGB foreground\x1b[0m\r\n"
        "\x1b[48:2::40:50:60mB:colon RGB background\x1b[0m\r\n"
        "\x1b[4;58:2::70:80:90mU:colon RGB underline\x1b[0m\r\n"
        "\x1b[38;5;1;48;5;2;58;5;3;4m"
        "\x1b[39m\x1b[49m\x1b[59mN:defaults\x1b[0m\r\n";

    return run_framed_payload(
        io,
        "sgr-extended-colors",
        payload,
        sizeof(payload) - 1);
}

static int run_sgr_underline_styles(const boxrig_emitter_io *io)
{
    static const uint8_t payload[] =
        "\x1b[4mS:single\x1b[0m\r\n"
        "\x1b[4:1mS:single colon\x1b[0m\r\n"
        "\x1b[21mD:double\x1b[0m\r\n"
        "\x1b[4:2mD:double colon\x1b[0m\r\n"
        "\x1b[4:3mC:curly\x1b[0m\r\n"
        "\x1b[4:4mO:dotted\x1b[0m\r\n"
        "\x1b[4:5mA:dashed\x1b[0m\r\n"
        "\x1b[4m\x1b[4:0mN:off colon\x1b[0m\r\n"
        "\x1b[4m\x1b[24mN:off\x1b[0m\r\n";

    return run_framed_payload(
        io,
        "sgr-underline-styles",
        payload,
        sizeof(payload) - 1);
}

static int run_osc_titles(const boxrig_emitter_io *io)
{
    static const uint8_t prefix[] = "\x1b[22;0;0t";
    static const uint8_t payload[] =
        "Setting combined title.\x1b]0;boxrig emitter icon and window\x1b\\\r\n"
        "Setting icon title.\x1b]1;boxrig emitter icon\x07\r\n"
        "Setting window title.\x1b]2;boxrig emitter window\x1b\\\r\n"
        "The previous titles are restored after END.\r\n";
    return run_prefixed_framed_payload(
        io,
        "osc-titles",
        prefix,
        sizeof(prefix) - 1,
        payload,
        sizeof(payload) - 1);
}

static int cleanup_osc_titles(const boxrig_emitter_io *io)
{
    static const uint8_t cleanup[] = "\x1b[23;0;0t";

    return boxrig_emitter_write(
        io,
        BOXRIG_EMITTER_STANDARD,
        cleanup,
        sizeof(cleanup) - 1);
}

static int run_osc_working_directory(const boxrig_emitter_io *io)
{
    static const uint8_t payload[] =
        "Reporting a fixture URI without changing the process directory.\r\n"
        "\x1b]7;file://localhost/tmp/boxrig-emitter\x1b\\"
        "OSC WORKING DIRECTORY COMPLETE\r\n";

    return run_framed_payload(
        io,
        "osc-working-directory",
        payload,
        sizeof(payload) - 1);
}

static int run_osc_hyperlink(const boxrig_emitter_io *io)
{
    static const uint8_t payload[] =
        "Linked cell: "
        "\x1b]8;id=boxrig-emitter;https://example.invalid/boxrig\x1b\\"
        "L"
        "\x1b]8;;\x1b\\"
        " | unlinked text\r\n";

    return run_framed_payload(
        io,
        "osc-hyperlink",
        payload,
        sizeof(payload) - 1);
}

static int run_osc_palette(const boxrig_emitter_io *io)
{
    static const uint8_t empty[] = "";
    static const uint8_t payload[] =
        "\x1b]4;1;#123456;2;rgb:00/80/ff\x1b\\"
        "\x1b[31mINDEX 1\x1b[0m | \x1b[32mINDEX 2\x1b[0m\r\n"
        "Palette indices are reset after END.\r\n";
    return run_prefixed_framed_payload(
        io,
        "osc-palette",
        empty,
        0,
        payload,
        sizeof(payload) - 1);
}

static int cleanup_osc_palette(const boxrig_emitter_io *io)
{
    static const uint8_t cleanup[] =
        "\x1b]104;1;2\x1b\\"
        "\x1b]104\x07";

    return boxrig_emitter_write(
        io,
        BOXRIG_EMITTER_STANDARD,
        cleanup,
        sizeof(cleanup) - 1);
}

static int run_osc_dynamic_colors(const boxrig_emitter_io *io)
{
    static const uint8_t empty[] = "";
    static const uint8_t payload[] =
        "\x1b]10;#112233\x1b\\"
        "\x1b]11;rgb:44/55/66\x07"
        "\x1b]12;#778899\x1b\\"
        "Default foreground/background and cursor colors are active.\r\n"
        "All three defaults are reset after END.\r\n";
    return run_prefixed_framed_payload(
        io,
        "osc-dynamic-colors",
        empty,
        0,
        payload,
        sizeof(payload) - 1);
}

static int cleanup_osc_dynamic_colors(const boxrig_emitter_io *io)
{
    static const uint8_t cleanup[] =
        "\x1b]110\x1b\\"
        "\x1b]111\x07"
        "\x1b]112\x1b\\";

    return boxrig_emitter_write(
        io,
        BOXRIG_EMITTER_STANDARD,
        cleanup,
        sizeof(cleanup) - 1);
}

static int run_clipboard_write(const boxrig_emitter_io *io)
{
    static const uint8_t payload[] =
        "WARNING: this case requests replacing every supported clipboard "
        "selection.\r\n"
        "\x1b]52;cpqs01234567;Ym94cmlnIGNsaXBib2FyZCBmaXh0dXJl\x1b\\"
        "CLIPBOARD WRITE REQUESTED\r\n";

    return run_framed_payload(
        io,
        "clipboard-write",
        payload,
        sizeof(payload) - 1);
}

static int run_desktop_notification(const boxrig_emitter_io *io)
{
    static const uint8_t payload[] =
        "Notification policy is deny unless BOXRIG_ALLOW_NOTIFICATIONS=1.\r\n"
        "\x1b]99;i=boxrig-emitter:p=title:o=always:u=1;"
        "boxrig emitter notification\x1b\\"
        "DESKTOP NOTIFICATION REQUESTED\r\n";

    return run_framed_payload(
        io,
        "desktop-notification",
        payload,
        sizeof(payload) - 1);
}

static int run_desktop_notification_options(const boxrig_emitter_io *io)
{
    static const uint8_t payload[] =
        "Notification policy is deny unless BOXRIG_ALLOW_NOTIFICATIONS=1.\r\n"
        "The first request assembles Base64 fields across two OSC 99 chunks.\r\n"
        "\x1b]99;i=boxrig-fields:p=title:d=0:e=0;Options\x1b\\"
        "\x1b]99;i=boxrig-fields:p=body:d=1:e=1:"
        "f=Ym94cmlnIGVtaXR0ZXI:t=c3lzdGVtLnRlc3Q:t=dGVybWluYWw:"
        "o=invisible:u=2;Qm9keQ0KbGluZQ==\x1b\\"
        "The second request has no identifier and uses BEL termination.\r\n"
        "\x1b]99;i=0:p=title:o=unfocused:u=0;Low option\x07"
        "DESKTOP NOTIFICATION OPTION REQUESTS SENT\r\n";

    return run_framed_payload(
        io,
        "desktop-notification-options",
        payload,
        sizeof(payload) - 1);
}

static int run_kitty_image_static(const boxrig_emitter_io *io)
{
    static const char png[] =
        "iVBORw0KGgoAAAANSUhEUgAAABAAAAAQCAYAAAAf8/9hAAAAbElEQVR4nGPg"
        "kTD4j4z/33EAY3RxXJgBm2ZSDKGuARR7gRxMlgEffvwn3wCQZhgm2QBkzTBM"
        "tAEgxQ82aIAxMpsoA5A1oGOCBuDTTNAAQprxGkCMZrABBgsC/lOCcRqgUfEB"
        "BZNkALpmfIYAAJbr6Om3F0qNAAAAAElFTkSuQmCC";
    uint8_t rgba[BOXRIG_EMITTER_GRAPHICS_FIXTURE_RGBA_LENGTH];
    uint8_t rgb[BOXRIG_EMITTER_GRAPHICS_FIXTURE_WIDTH *
                BOXRIG_EMITTER_GRAPHICS_FIXTURE_HEIGHT * 3];
    uint8_t compressed[GRAPHICS_ZLIB_LENGTH];
    uint8_t scroll[GRAPHICS_SCROLL_RGBA_LENGTH];
    char raw_rgb[GRAPHICS_DATA_ENCODED_CAPACITY];
    char alpha[GRAPHICS_DATA_ENCODED_CAPACITY];
    char zlib[GRAPHICS_DATA_ENCODED_CAPACITY];
    char deleted[GRAPHICS_DATA_ENCODED_CAPACITY];
    char primary[GRAPHICS_DATA_ENCODED_CAPACITY];
    char alternate[GRAPHICS_DATA_ENCODED_CAPACITY];
    char placeholder[GRAPHICS_DATA_ENCODED_CAPACITY];
    char scrolled[GRAPHICS_DATA_ENCODED_CAPACITY];
    char payload[GRAPHICS_SCREEN_PAYLOAD_CAPACITY];
    size_t compressed_length = 0;
    size_t alpha_split;
    size_t png_split = (sizeof(png) - 1) / 2 / 4 * 4;
    int payload_length;

    if (make_graphics_landscape(
            GRAPHICS_PALETTE_RED,
            rgba,
            sizeof(rgba)) == -1) {
        return -1;
    }
    graphics_rgba_to_rgb(rgba, rgb);
    if (encode_base64(rgb, sizeof(rgb), raw_rgb, sizeof(raw_rgb)) == -1 ||
        encode_graphics_landscape(
            GRAPHICS_PALETTE_ALPHA,
            alpha,
            sizeof(alpha)) == -1 ||
        make_graphics_landscape(
            GRAPHICS_PALETTE_PURPLE,
            rgba,
            sizeof(rgba)) == -1 ||
        make_stored_zlib(
            rgba,
            sizeof(rgba),
            compressed,
            sizeof(compressed),
            &compressed_length) == -1 ||
        encode_base64(
            compressed,
            compressed_length,
            zlib,
            sizeof(zlib)) == -1 ||
        encode_graphics_landscape(
            GRAPHICS_PALETTE_MAGENTA,
            deleted,
            sizeof(deleted)) == -1 ||
        encode_graphics_landscape(
            GRAPHICS_PALETTE_ORANGE,
            primary,
            sizeof(primary)) == -1 ||
        encode_graphics_landscape(
            GRAPHICS_PALETTE_MAGENTA,
            alternate,
            sizeof(alternate)) == -1 ||
        encode_graphics_landscape(
            GRAPHICS_PALETTE_CYAN,
            placeholder,
            sizeof(placeholder)) == -1 ||
        make_graphics_scroll_pixels(scroll, sizeof(scroll)) == -1 ||
        encode_base64(
            scroll,
            sizeof(scroll),
            scrolled,
            sizeof(scrolled)) == -1) {
        return -1;
    }
    alpha_split = strlen(alpha) / 2 / 4 * 4;
    payload_length = snprintf(
        payload,
        sizeof(payload),
        "\x1b[?25l\x1b]11;#000000\x1b\\\x1b[2J\x1b[H"
        "\x1b_Ga=T,i=9200,p=21,q=2,f=32,s=16,v=8,c=4,r=1,C=1;"
        "%s\x1b\\"
        "\x1b[23;2H\x1b_Ga=p,i=9200,p=22,q=2,c=4,r=1,C=1\x1b\\"
        "\x1b[24;1H\n\x1b[1;1H"
        "The pixel-art pictures below demonstrate raw, compressed, PNG, "
        "cropped,\r\n"
        "deleted, alternate-screen, placeholder, and scrolled Kitty "
        "graphics."
        "\x1b[3;1Hraw RGB landscape       cropped right half"
        "\x1b[4;1H\x1b_Ga=T,i=9101,p=11,q=2,f=24,s=16,v=16,"
        "c=4,r=2,C=1;%s\x1b\\"
        "\x1b[4;7H\x1b_Ga=p,i=9101,p=15,q=2,x=8,w=8,"
        "c=2,r=2,C=1\x1b\\"
        "\x1b[6;1Hchunked RGBA alpha      zlib purple landscape"
        "\x1b[7;1H\x1b_Ga=T,i=9102,p=12,q=2,f=32,s=16,v=16,"
        "c=4,r=2,C=1,m=1;%.*s\x1b\\"
        "\x1b_Gm=0;%s\x1b\\"
        "\x1b[7;7H\x1b_Ga=T,i=9103,p=13,q=2,f=32,s=16,v=16,o=z,"
        "c=4,r=2,C=1;%s\x1b\\"
        "\x1b[9;1HPNG landscape           deleted image gap"
        "\x1b[10;1H\x1b_Ga=T,i=9104,p=14,q=2,f=100,c=4,r=2,C=1,"
        "m=1;%.*s\x1b\\"
        "\x1b_Gm=0;%s\x1b\\"
        "\x1b[10;7H\x1b_Ga=T,i=9201,p=23,q=2,f=32,s=16,v=16,"
        "c=4,r=2,C=1;%s\x1b\\"
        "\x1b_Ga=d,d=I,i=9201,q=2\x1b\\"
        "\x1b[12;1Hprimary landscape       alternate hidden"
        "\x1b[13;1H\x1b_Ga=T,i=9202,p=24,q=2,f=32,s=16,v=16,"
        "c=4,r=2,C=1;%s\x1b\\"
        "\x1b[?1049h\x1b[13;1H"
        "\x1b_Ga=T,i=9203,p=25,q=2,f=32,s=16,v=16,c=4,r=2,C=1;"
        "%s\x1b\\\x1b[?1049l"
        "\x1b[15;1HUnicode placeholder grid"
        "\x1b_Gi=42,q=2,f=32,s=16,v=16;%s\x1b\\"
        "\x1b_Ga=p,i=42,p=26,q=2,c=4,r=2,U=1\x1b\\"
        "\x1b[16;1H\x1b[38;5;42;58;5;26m"
        "\xf4\x8e\xbb\xae\xcc\x85\xcc\x85"
        "\xf4\x8e\xbb\xae\xcc\x85\xcc\x8d"
        "\xf4\x8e\xbb\xae\xcc\x85\xcc\x8e"
        "\xf4\x8e\xbb\xae\xcc\x85\xcc\x90"
        "\x1b[17;1H"
        "\xf4\x8e\xbb\xae\xcc\x8d\xcc\x85"
        "\xf4\x8e\xbb\xae\xcc\x8d\xcc\x8d"
        "\xf4\x8e\xbb\xae\xcc\x8d\xcc\x8e"
        "\xf4\x8e\xbb\xae\xcc\x8d\xcc\x90"
        "\x1b[0m"
        "\x1b[21;1Hscrolled sailboat\x1b[23;1H",
        scrolled,
        raw_rgb,
        (int)alpha_split,
        alpha,
        alpha + alpha_split,
        zlib,
        (int)png_split,
        png,
        png + png_split,
        deleted,
        primary,
        alternate,
        placeholder);
    if (payload_length <= 0 ||
        (size_t)payload_length >= sizeof(payload)) {
        return -1;
    }
    return run_screen_payload(
        io,
        "kitty-image-static",
        (const uint8_t *)payload,
        (size_t)payload_length);
}

static int run_kitty_image_external(const boxrig_emitter_io *io)
{
    static const char *const labels[] = {
        "regular file (red)",
        "safe temporary file (green)",
        "shared memory (blue)",
    };
    boxrig_emitter_graphics_fixture fixture;
    char encoded[3][GRAPHICS_FIXTURE_ENCODED_CAPACITY];
    uint8_t requests[3][GRAPHICS_FIXTURE_REQUEST_CAPACITY];
    query_step steps[3];
    const uint8_t *names[3];
    size_t name_lengths[3];
    static const char media[] = {'f', 't', 's'};
    static const char *const prefixes[] = {
        "\x1b[?25l\x1b]11;#000000\x1b\\\x1b[2J\x1b[H"
        "With external graphics enabled, three pixel-art pictures appear "
        "below.\r\n"
        "Red uses a regular file, green a temporary file, and blue shared "
        "memory.\r\n"
        "Without opt-in, every request reports EACCES and no picture "
        "appears.\x1b[4;1H",
        "\x1b[4;7H",
        "\x1b[4;13H",
    };
    static const uint8_t placements[] =
        "\x1b[4;1H\x1b_Ga=p,i=9401,q=2,c=4,r=2,C=1\x1b\\"
        "\x1b[4;7H\x1b_Ga=p,i=9402,q=2,c=4,r=2,C=1\x1b\\"
        "\x1b[4;13H\x1b_Ga=p,i=9403,q=2,c=4,r=2,C=1\x1b\\"
        "\x1b[23;1H";
    size_t index;
    int status = -1;

    if (io->graphics_fixture_begin == NULL ||
        io->graphics_fixture_end == NULL) {
        return -1;
    }
    memset(&fixture, 0, sizeof(fixture));
    if (io->graphics_fixture_begin(io->context, &fixture) == -1) {
        return -1;
    }
    names[0] = fixture.regular_name;
    names[1] = fixture.temporary_name;
    names[2] = fixture.shared_name;
    name_lengths[0] = fixture.regular_name_length;
    name_lengths[1] = fixture.temporary_name_length;
    name_lengths[2] = fixture.shared_name_length;
    for (index = 0; index < 3; index++) {
        int request_length;

        if (encode_fixture_name(
                names[index],
                name_lengths[index],
                encoded[index],
                sizeof(encoded[index])) == -1) {
            goto cleanup;
        }
        request_length = snprintf(
            (char *)requests[index],
            sizeof(requests[index]),
            "%s\x1b_Ga=t,i=%lu,t=%c,q=0,f=32,s=16,v=16,O=0,S=1024;"
            "%s\x1b\\\x1b[20;1H",
            prefixes[index],
            (unsigned long)(9401 + index),
            media[index],
            encoded[index]);
        if (request_length <= 0 ||
            (size_t)request_length >= sizeof(requests[index])) {
            goto cleanup;
        }
        steps[index].label = labels[index];
        steps[index].request = requests[index];
        steps[index].request_length = (size_t)request_length;
        steps[index].terminator = RESPONSE_APC;
        steps[index].response_limit = 256;
    }
    status = run_round_trip_case(
        io,
        "kitty-image-external",
        steps,
        sizeof(steps) / sizeof(steps[0]));
    if (status == 0 &&
        boxrig_emitter_write(
            io,
            BOXRIG_EMITTER_STANDARD,
            placements,
            sizeof(placements) - 1) == -1) {
        status = -1;
    }

cleanup:
    if (io->graphics_fixture_end(io->context) == -1) {
        status = -1;
    }
    return status;
}

static int run_kitty_animation_direct(const boxrig_emitter_io *io)
{
    char red[GRAPHICS_DATA_ENCODED_CAPACITY];
    char green_half[GRAPHICS_DATA_ENCODED_CAPACITY];
    char payload[GRAPHICS_SCREEN_PAYLOAD_CAPACITY];
    int payload_length;

    if (encode_graphics_landscape(
            GRAPHICS_PALETTE_RED,
            red,
            sizeof(red)) == -1 ||
        encode_graphics_right_half(
            GRAPHICS_PALETTE_GREEN,
            green_half,
            sizeof(green_half)) == -1) {
        return -1;
    }
    payload_length = snprintf(
        payload,
        sizeof(payload),
        "\x1b[?25l\x1b]11;#000000\x1b\\\x1b[2J\x1b[H"
        "A 4-by-2 pixel-art landscape appears below. Direct frame transfer "
        "changes\r\n"
        "its right half from red to green; it remains after the emitter "
        "exits.\x1b[3;1H"
        "\x1b_Ga=T,i=9301,p=301,q=2,f=32,s=16,v=16,c=4,r=2,C=1;"
        "%s\x1b\\"
        "\x1b_Ga=f,i=9301,q=2,f=32,s=8,v=16,r=1,x=8,X=1;"
        "%s\x1b\\\x1b[5;1H",
        red,
        green_half);
    if (payload_length <= 0 ||
        (size_t)payload_length >= sizeof(payload)) {
        return -1;
    }

    return run_screen_payload(
        io,
        "kitty-animation-direct",
        (const uint8_t *)payload,
        (size_t)payload_length);
}

static int run_kitty_animation_client(const boxrig_emitter_io *io)
{
    char red[GRAPHICS_DATA_ENCODED_CAPACITY];
    char green[GRAPHICS_DATA_ENCODED_CAPACITY];
    char blue[GRAPHICS_DATA_ENCODED_CAPACITY];
    char payload[GRAPHICS_SCREEN_PAYLOAD_CAPACITY];
    int payload_length;

    if (encode_graphics_landscape(
            GRAPHICS_PALETTE_RED,
            red,
            sizeof(red)) == -1 ||
        encode_graphics_landscape(
            GRAPHICS_PALETTE_GREEN,
            green,
            sizeof(green)) == -1 ||
        encode_graphics_landscape(
            GRAPHICS_PALETTE_BLUE,
            blue,
            sizeof(blue)) == -1) {
        return -1;
    }
    payload_length = snprintf(
        payload,
        sizeof(payload),
        "\x1b[?25l\x1b]11;#000000\x1b\\\x1b[2J\x1b[H"
        "A blue 4-by-2 pixel-art landscape appears below. The emitter "
        "selects it\r\n"
        "without automatic playback; it remains after the emitter exits."
        "\x1b[3;1H"
        "\x1b_Ga=T,i=9302,p=302,q=2,f=32,s=16,v=16,c=4,r=2,C=1;"
        "%s\x1b\\"
        "\x1b_Ga=f,i=9302,q=2,f=32,s=16,v=16,X=1,z=120;"
        "%s\x1b\\"
        "\x1b_Ga=f,i=9302,q=2,f=32,s=16,v=16,X=1,z=120;"
        "%s\x1b\\"
        "\x1b_Ga=a,i=9302,q=2,s=1,c=3\x1b\\\x1b[5;1H",
        red,
        green,
        blue);
    if (payload_length <= 0 ||
        (size_t)payload_length >= sizeof(payload)) {
        return -1;
    }

    return run_screen_payload(
        io,
        "kitty-animation-client",
        (const uint8_t *)payload,
        (size_t)payload_length);
}

static int run_kitty_animation_loop(const boxrig_emitter_io *io)
{
    char red[GRAPHICS_DATA_ENCODED_CAPACITY];
    char green[GRAPHICS_DATA_ENCODED_CAPACITY];
    char payload[GRAPHICS_SCREEN_PAYLOAD_CAPACITY];
    int payload_length;

    if (encode_graphics_landscape(
            GRAPHICS_PALETTE_RED,
            red,
            sizeof(red)) == -1 ||
        encode_graphics_landscape(
            GRAPHICS_PALETTE_GREEN,
            green,
            sizeof(green)) == -1) {
        return -1;
    }
    payload_length = snprintf(
        payload,
        sizeof(payload),
        "\x1b[?25l\x1b]11;#000000\x1b\\\x1b[2J\x1b[H"
        "A 4-by-2 pixel-art landscape that flashes red and green appears "
        "below.\r\n"
        "It continues to flash even after the emitter is closed.\x1b[3;1H"
        "\x1b_Ga=T,i=9303,p=303,q=2,f=32,s=16,v=16,c=4,r=2,C=1;"
        "%s\x1b\\"
        "\x1b_Ga=f,i=9303,q=2,f=32,s=16,v=16,X=1,z=120;"
        "%s\x1b\\"
        "\x1b_Ga=a,i=9303,q=2,r=1,z=120,s=3,v=1,c=1\x1b\\"
        "\x1b[5;1H",
        red,
        green);
    if (payload_length <= 0 ||
        (size_t)payload_length >= sizeof(payload)) {
        return -1;
    }

    return run_screen_payload(
        io,
        "kitty-animation-loop",
        (const uint8_t *)payload,
        (size_t)payload_length);
}

static int run_kitty_animation_compose(const boxrig_emitter_io *io)
{
    char blue[GRAPHICS_DATA_ENCODED_CAPACITY];
    char payload[GRAPHICS_SCREEN_PAYLOAD_CAPACITY];
    int payload_length;

    if (encode_graphics_landscape(
            GRAPHICS_PALETTE_BLUE,
            blue,
            sizeof(blue)) == -1) {
        return -1;
    }
    payload_length = snprintf(
        payload,
        sizeof(payload),
        "\x1b[?25l\x1b]11;#000000\x1b\\\x1b[2J\x1b[H"
        "A 4-by-2 pixel-art landscape with a black border appears below. "
        "Frame\r\n"
        "composition copies its blue center; it remains after emitter exits."
        "\x1b[3;1H"
        "\x1b_Ga=T,i=9304,p=304,q=2,f=32,s=16,v=16,c=4,r=2,C=1;"
        "%s\x1b\\"
        "\x1b_Ga=f,i=9304,q=2,f=32,s=1,v=1,Y=255,X=1;"
        "AAAA/w==\x1b\\"
        "\x1b_Ga=c,i=9304,q=2,r=1,c=2,x=2,y=2,X=2,Y=2,w=12,h=12,"
        "C=1\x1b\\"
        "\x1b_Ga=a,i=9304,q=2,s=1,c=2\x1b\\\x1b[5;1H",
        blue);
    if (payload_length <= 0 ||
        (size_t)payload_length >= sizeof(payload)) {
        return -1;
    }

    return run_screen_payload(
        io,
        "kitty-animation-compose",
        (const uint8_t *)payload,
        (size_t)payload_length);
}

static int run_query_device_status(const boxrig_emitter_io *io)
{
    static const uint8_t q0[] = "\x1b[5n";
    static const uint8_t q2[] = "\x1b[?6n";
    static const uint8_t q3[] = "\x1b[?15n";
    static const uint8_t q4[] = "\x1b[?25n";
    static const uint8_t q5[] = "\x1b[?26n";
    static const uint8_t q6[] = "\x1b[?75n";
    static const uint8_t q7[] = "\x1b[?85n";
    static const query_step steps[] = {
        {"operating status", q0, sizeof(q0) - 1, RESPONSE_CSI, 256},
        {"cursor position", cursor_position_query,
         sizeof(cursor_position_query) - 1, RESPONSE_CSI, 256},
        {"DEC cursor position", q2, sizeof(q2) - 1, RESPONSE_CSI, 256},
        {"printer status", q3, sizeof(q3) - 1, RESPONSE_CSI, 256},
        {"UDK status", q4, sizeof(q4) - 1, RESPONSE_CSI, 256},
        {"keyboard status", q5, sizeof(q5) - 1, RESPONSE_CSI, 256},
        {"data integrity", q6, sizeof(q6) - 1, RESPONSE_CSI, 256},
        {"multi-session status", q7, sizeof(q7) - 1, RESPONSE_CSI, 256},
    };

    return run_round_trip_case(
        io,
        "query-device-status",
        steps,
        sizeof(steps) / sizeof(steps[0]));
}

static int run_query_cursor_position(const boxrig_emitter_io *io)
{
    static const query_step steps[] = {
        {"cursor position", cursor_position_query,
         sizeof(cursor_position_query) - 1, RESPONSE_CSI, 256},
    };

    return run_round_trip_case(
        io,
        "query-cursor-position",
        steps,
        sizeof(steps) / sizeof(steps[0]));
}

static int run_query_device_attributes(const boxrig_emitter_io *io)
{
    static const uint8_t da1[] = "\x1b[c";
    static const uint8_t da2[] = "\x1b[>c";
    static const uint8_t version[] = "\x1b[>0q";
    static const query_step steps[] = {
        {"primary attributes", da1, sizeof(da1) - 1, RESPONSE_CSI, 256},
        {"secondary attributes", da2, sizeof(da2) - 1, RESPONSE_CSI, 256},
        {"terminal version", version, sizeof(version) - 1, RESPONSE_DCS, 256},
    };

    return run_round_trip_case(
        io,
        "query-device-attributes",
        steps,
        sizeof(steps) / sizeof(steps[0]));
}

static int run_query_modes(const boxrig_emitter_io *io)
{
    static const unsigned int standard_modes[] = {4, 20};
    static const unsigned int private_modes[] = {
        1, 5, 6, 7, 9, 12, 25, 47, 66, 69,
        1000, 1002, 1003, 1004, 1006, 1034, 1047, 1048, 1049, 2004,
    };
    uint8_t requests[22][24];
    char labels[22][32];
    query_step steps[22];
    size_t step = 0;
    size_t index;

    for (index = 0;
         index < sizeof(standard_modes) / sizeof(standard_modes[0]);
         index++) {
        int request_length = snprintf(
            (char *)requests[step],
            sizeof(requests[step]),
            "\x1b[%u$p",
            standard_modes[index]);
        int label_length = snprintf(
            labels[step],
            sizeof(labels[step]),
            "standard mode %u",
            standard_modes[index]);

        if (request_length <= 0 ||
            (size_t)request_length >= sizeof(requests[step]) ||
            label_length <= 0 ||
            (size_t)label_length >= sizeof(labels[step])) {
            return -1;
        }
        steps[step].label = labels[step];
        steps[step].request = requests[step];
        steps[step].request_length = (size_t)request_length;
        steps[step].terminator = RESPONSE_CSI;
        steps[step].response_limit = 256;
        step++;
    }
    for (index = 0;
         index < sizeof(private_modes) / sizeof(private_modes[0]);
         index++) {
        int request_length = snprintf(
            (char *)requests[step],
            sizeof(requests[step]),
            "\x1b[?%u$p",
            private_modes[index]);
        int label_length = snprintf(
            labels[step],
            sizeof(labels[step]),
            "private mode %u",
            private_modes[index]);

        if (request_length <= 0 ||
            (size_t)request_length >= sizeof(requests[step]) ||
            label_length <= 0 ||
            (size_t)label_length >= sizeof(labels[step])) {
            return -1;
        }
        steps[step].label = labels[step];
        steps[step].request = requests[step];
        steps[step].request_length = (size_t)request_length;
        steps[step].terminator = RESPONSE_CSI;
        steps[step].response_limit = 256;
        step++;
    }
    return run_round_trip_case(io, "query-modes", steps, step);
}

static int run_query_decrqss(const boxrig_emitter_io *io)
{
    static const uint8_t sgr[] = "\x1bP$qm\x1b\\";
    static const uint8_t vertical_margin[] = "\x1bP$qr\x1b\\";
    static const uint8_t horizontal_margin[] = "\x1bP$qs\x1b\\";
    static const uint8_t cursor_style[] = "\x1bP$q q\x1b\\";
    static const uint8_t protection[] = "\x1bP$q\"q\x1b\\";
    static const uint8_t unknown[] = "\x1bP$qx\x1b\\";
    static const query_step steps[] = {
        {"SGR", sgr, sizeof(sgr) - 1, RESPONSE_DCS, 4096},
        {"vertical margins", vertical_margin, sizeof(vertical_margin) - 1,
         RESPONSE_DCS, 4096},
        {"horizontal margins", horizontal_margin,
         sizeof(horizontal_margin) - 1, RESPONSE_DCS, 4096},
        {"cursor style", cursor_style, sizeof(cursor_style) - 1,
         RESPONSE_DCS, 4096},
        {"protected state", protection, sizeof(protection) - 1,
         RESPONSE_DCS, 4096},
        {"unknown request", unknown, sizeof(unknown) - 1,
         RESPONSE_DCS, 4096},
    };

    return run_round_trip_case(
        io,
        "query-decrqss",
        steps,
        sizeof(steps) / sizeof(steps[0]));
}

static int run_query_colors(const boxrig_emitter_io *io)
{
    static const uint8_t palette[] = "\x1b]4;1;?\x07";
    static const uint8_t foreground[] = "\x1b]10;?\x1b\\";
    static const uint8_t background[] = "\x1b]11;?\x07";
    static const uint8_t cursor[] = "\x1b]12;?\x1b\\";
    static const query_step steps[] = {
        {"palette index 1", palette, sizeof(palette) - 1, RESPONSE_OSC, 1024},
        {"default foreground", foreground, sizeof(foreground) - 1,
         RESPONSE_OSC, 1024},
        {"default background", background, sizeof(background) - 1,
         RESPONSE_OSC, 1024},
        {"cursor color", cursor, sizeof(cursor) - 1, RESPONSE_OSC, 1024},
    };

    return run_round_trip_case(
        io,
        "query-colors",
        steps,
        sizeof(steps) / sizeof(steps[0]));
}

static int run_clipboard_read(const boxrig_emitter_io *io)
{
    static const uint8_t query[] = "\x1b]52;c;?\x1b\\";
    static const query_step steps[] = {
        {"clipboard selection c", query, sizeof(query) - 1,
         RESPONSE_OSC, 43728},
    };

    return run_round_trip_case(
        io,
        "clipboard-read",
        steps,
        sizeof(steps) / sizeof(steps[0]));
}

static int run_notification_query(const boxrig_emitter_io *io)
{
    static const uint8_t query[] =
        "\x1b]99;i=boxrig-emitter:p=?;\x1b\\";
    static const query_step steps[] = {
        {"notification profile", query, sizeof(query) - 1,
         RESPONSE_OSC, 2048},
    };

    return run_round_trip_case(
        io,
        "notification-query",
        steps,
        sizeof(steps) / sizeof(steps[0]));
}

static int run_query_kitty_graphics(const boxrig_emitter_io *io)
{
    static const uint8_t query[] =
        "\r\x1b_Ga=q,i=9300,f=32,s=1,v=1;/wAA/w==\x1b\\";
    static const query_step steps[] = {
        {"direct RGBA support", query, sizeof(query) - 1,
         RESPONSE_APC, 256},
    };

    return run_round_trip_case(
        io,
        "query-kitty-graphics",
        steps,
        sizeof(steps) / sizeof(steps[0]));
}

static int run_query_kitty_keyboard(const boxrig_emitter_io *io)
{
    static const uint8_t query[] = "\x1b[?u";
    static const query_step steps[] = {
        {"Kitty keyboard flags", query, sizeof(query) - 1,
         RESPONSE_CSI, 256},
    };

    return run_round_trip_case(
        io,
        "query-kitty-keyboard",
        steps,
        sizeof(steps) / sizeof(steps[0]));
}

static int run_input_key(const boxrig_emitter_io *io)
{
    static const uint8_t empty[] = "";

    return run_interactive_case(
        io,
        "input-key",
        "Press Ctrl+Alt+Up once.",
        empty,
        0,
        empty,
        0,
        INTERACTIVE_SEQUENCE,
        1,
        256);
}

static int run_input_application_cursor(const boxrig_emitter_io *io)
{
    static const uint8_t enable[] = "\x1b[?1h";
    static const uint8_t disable[] = "\x1b[?1l";

    return run_interactive_case(
        io,
        "input-application-cursor",
        "Press the Up cursor key once.",
        enable,
        sizeof(enable) - 1,
        disable,
        sizeof(disable) - 1,
        INTERACTIVE_SEQUENCE,
        1,
        256);
}

static int run_input_application_keypad(const boxrig_emitter_io *io)
{
    static const uint8_t enable[] = "\x1b[?66h";
    static const uint8_t disable[] = "\x1b[?66l";

    return run_interactive_case(
        io,
        "input-application-keypad",
        "Press keypad 1 once with Num Lock enabled.",
        enable,
        sizeof(enable) - 1,
        disable,
        sizeof(disable) - 1,
        INTERACTIVE_SEQUENCE,
        1,
        256);
}

static int run_input_kitty_keyboard(const boxrig_emitter_io *io)
{
    static const uint8_t enable[] = "\x1b[>31u";
    static const uint8_t disable[] = "\x1b[<u";

    return run_interactive_case(
        io,
        "input-kitty-keyboard",
        "Hold Shift+Ctrl+A until one repeat, then release it.",
        enable,
        sizeof(enable) - 1,
        disable,
        sizeof(disable) - 1,
        INTERACTIVE_SEQUENCE,
        3,
        1024);
}

static int run_input_paste(const boxrig_emitter_io *io)
{
    static const uint8_t enable[] = "\x1b[?2004h";
    static const uint8_t disable[] = "\x1b[?2004l";

    return run_interactive_case(
        io,
        "input-paste",
        "Paste one short UTF-8 fixture from the clipboard.",
        enable,
        sizeof(enable) - 1,
        disable,
        sizeof(disable) - 1,
        INTERACTIVE_PASTE,
        0,
        INTERACTIVE_CAPTURE_CAPACITY);
}

static int run_input_focus(const boxrig_emitter_io *io)
{
    static const uint8_t enable[] = "\x1b[?1004h";
    static const uint8_t disable[] = "\x1b[?1004l";

    return run_interactive_case(
        io,
        "input-focus",
        "Move focus to another window, then return focus here.",
        enable,
        sizeof(enable) - 1,
        disable,
        sizeof(disable) - 1,
        INTERACTIVE_SEQUENCE,
        2,
        256);
}

static int run_input_mouse(const boxrig_emitter_io *io)
{
    static const uint8_t enable[] = "\x1b[?1003h\x1b[?1006h";
    static const uint8_t disable[] = "\x1b[?1006l\x1b[?1003l";

    return run_interactive_case(
        io,
        "input-mouse",
        "Generate three mouse events such as motion, wheel, and a button.",
        enable,
        sizeof(enable) - 1,
        disable,
        sizeof(disable) - 1,
        INTERACTIVE_SEQUENCE,
        3,
        1024);
}

static int run_input_ime_commit(const boxrig_emitter_io *io)
{
    static const uint8_t empty[] = "";

    return run_interactive_case(
        io,
        "input-ime-commit",
        "Commit the Japanese text \"\xe6\x97\xa5\xe6\x9c\xac\", then press Enter.",
        empty,
        0,
        empty,
        0,
        INTERACTIVE_LINE,
        0,
        4097);
}

static int run_frontend_resize(const boxrig_emitter_io *io)
{
    static const uint8_t payload[] =
        "Resize the terminal window narrower and wider.\r\n"
        "Observe logical-line reflow, wide-cell integrity, and cursor position.\r\n";

    return run_framed_payload(
        io,
        "frontend-resize",
        payload,
        sizeof(payload) - 1);
}

static int run_frontend_scrollback(const boxrig_emitter_io *io)
{
    static const uint8_t payload[] =
        "Scroll the viewport into primary-screen history and back to live output.\r\n"
        "Observe retained rows and the hidden cursor while history is visible.\r\n";

    return run_framed_payload(
        io,
        "frontend-scrollback",
        payload,
        sizeof(payload) - 1);
}

static int run_frontend_font_zoom(const boxrig_emitter_io *io)
{
    static const uint8_t payload[] =
        "Use the frontend font zoom shortcuts to zoom in, out, and reset.\r\n"
        "Observe new cell metrics, grid dimensions, shaping, and redraw.\r\n";

    return run_framed_payload(
        io,
        "frontend-font-zoom",
        payload,
        sizeof(payload) - 1);
}

static int run_frontend_ime_preedit(const boxrig_emitter_io *io)
{
    static const uint8_t payload[] =
        "Start an IME preedit for \"A"
        "\xe7\x95\x8c"
        "e\xcc\x81\" without committing it.\r\n"
        "Observe transient segments, selection, caret, candidate placement, "
        "commit, and cancel.\r\n";

    return run_framed_payload(
        io,
        "frontend-ime-preedit",
        payload,
        sizeof(payload) - 1);
}

static int run_safe_no_op(const boxrig_emitter_io *io)
{
    static const uint8_t payload[] =
        "A\x1b[iB\x1b[4iC\x1b[5iD\x1b" "lE\x1b" "mF\r\n"
        "SAFE NO-OP COMPLETE\r\n";

    return run_framed_payload(
        io,
        "safe-no-op",
        payload,
        sizeof(payload) - 1);
}

static int run_unsupported_recovery(const boxrig_emitter_io *io)
{
    static const uint8_t payload[] =
        "A\x1bPqSIXEL\x1b\\B"
        "\x1bP1;2pREGIS\x1b\\C"
        "\x1bP1;2|DECUDK\x1b\\D"
        "\x1bP1$uDECRSPS\x1b\\E"
        "\x1bP+q544e\x1b\\F"
        "\x1bP+Q544e\x1b\\G"
        "\x1bP@kitty-cmd{}\x1b\\H"
        "\x1b_XAPC\x1b\\I"
        "\x1b_Gaa=1;QUJD\x1b\\J"
        "\x1b^PM\x1b\\K"
        "\x1bXSOS\x1b\\L\r\n"
        "RECOVERED after unsupported control strings\r\n";

    return run_framed_payload(
        io,
        "unsupported-recovery",
        payload,
        sizeof(payload) - 1);
}

static int write_help(const boxrig_emitter_io *io)
{
    static const uint8_t help[] =
        "Usage:\r\n"
        "  boxrig_emitter --help\r\n"
        "  boxrig_emitter --list\r\n"
        "  boxrig_emitter --describe NAME\r\n"
        "  boxrig_emitter -t NAME [--no-hold]\r\n";

    return boxrig_emitter_write(
        io,
        BOXRIG_EMITTER_STANDARD,
        help,
        sizeof(help) - 1);
}

static int write_list(const boxrig_emitter_io *io)
{
    size_t index;

    for (index = 0; index < sizeof(cases) / sizeof(cases[0]); index++) {
        const boxrig_emitter_case_info *info = &cases[index].info;

        if (write_text(io, BOXRIG_EMITTER_STANDARD, info->name) == -1 ||
            write_text(io, BOXRIG_EMITTER_STANDARD, "\t") == -1 ||
            write_text(io, BOXRIG_EMITTER_STANDARD, info->category) == -1 ||
            write_text(io, BOXRIG_EMITTER_STANDARD, "\t") == -1 ||
            write_text(
                io,
                BOXRIG_EMITTER_STANDARD,
                class_name(info->case_class)) == -1 ||
            (info->input_required &&
             write_text(io, BOXRIG_EMITTER_STANDARD, ",input") == -1) ||
            (info->host_effect &&
             write_text(io, BOXRIG_EMITTER_STANDARD, ",host-effect") == -1) ||
            write_text(io, BOXRIG_EMITTER_STANDARD, "\r\n") == -1) {
            return -1;
        }
    }
    return 0;
}

static int write_boolean(
    const boxrig_emitter_io *io,
    const char *label,
    bool value)
{
    if (write_text(io, BOXRIG_EMITTER_STANDARD, label) == -1 ||
        write_text(
            io,
            BOXRIG_EMITTER_STANDARD,
            value ? "yes\r\n" : "no\r\n") == -1) {
        return -1;
    }
    return 0;
}

static int write_description(
    const boxrig_emitter_io *io,
    const boxrig_emitter_case_info *info)
{
    if (write_text(io, BOXRIG_EMITTER_STANDARD, "NAME: ") == -1 ||
        write_text(io, BOXRIG_EMITTER_STANDARD, info->name) == -1 ||
        write_text(io, BOXRIG_EMITTER_STANDARD, "\r\nCATEGORY: ") == -1 ||
        write_text(io, BOXRIG_EMITTER_STANDARD, info->category) == -1 ||
        write_text(io, BOXRIG_EMITTER_STANDARD, "\r\nCLASS: ") == -1 ||
        write_text(
            io,
            BOXRIG_EMITTER_STANDARD,
            class_name(info->case_class)) == -1 ||
        write_text(io, BOXRIG_EMITTER_STANDARD, "\r\nDESCRIPTION: ") == -1 ||
        write_text(io, BOXRIG_EMITTER_STANDARD, info->description) == -1 ||
        write_text(io, BOXRIG_EMITTER_STANDARD, "\r\nEXPECT: ") == -1 ||
        write_text(io, BOXRIG_EMITTER_STANDARD, info->expectation) == -1 ||
        write_text(io, BOXRIG_EMITTER_STANDARD, "\r\nCOMPATIBILITY: ") == -1 ||
        write_text(
            io,
            BOXRIG_EMITTER_STANDARD,
            info->compatibility_section) == -1 ||
        write_text(io, BOXRIG_EMITTER_STANDARD, "\r\nSEQUENCE: ") == -1 ||
        write_text(io, BOXRIG_EMITTER_STANDARD, info->sequence_family) == -1 ||
        write_text(io, BOXRIG_EMITTER_STANDARD, "\r\n") == -1 ||
        write_boolean(io, "SCREEN-WIDE: ", info->screen_wide) == -1 ||
        write_boolean(io, "HOST-EFFECT: ", info->host_effect) == -1 ||
        write_boolean(io, "INPUT: ", info->input_required) == -1) {
        return -1;
    }
    return 0;
}

static int write_cli_error(
    const boxrig_emitter_io *io,
    const char *message,
    const char *value)
{
    if (write_text(io, BOXRIG_EMITTER_DIAGNOSTIC, "boxrig_emitter: ") == -1 ||
        write_text(io, BOXRIG_EMITTER_DIAGNOSTIC, message) == -1 ||
        (value != NULL &&
         write_text(io, BOXRIG_EMITTER_DIAGNOSTIC, value) == -1) ||
        write_text(io, BOXRIG_EMITTER_DIAGNOSTIC, "\r\n") == -1) {
        return 1;
    }
    return 2;
}

static int hold_for_enter(const boxrig_emitter_io *io)
{
    static const uint8_t prompt[] = "HOLD: Press Enter to exit.\r\n";

    if (boxrig_emitter_write(
            io,
            BOXRIG_EMITTER_STANDARD,
            prompt,
            sizeof(prompt) - 1) == -1 ||
        io->read_byte == NULL) {
        return -1;
    }
    for (;;) {
        uint8_t byte;
        int status = io->read_byte(io->context, &byte);

        if (status != 1) {
            return -1;
        }
        if (byte == '\r' || byte == '\n') {
            return 0;
        }
    }
}

int boxrig_emitter_write(
    const boxrig_emitter_io *io,
    boxrig_emitter_stream stream,
    const uint8_t *bytes,
    size_t length)
{
    if (io == NULL || io->write == NULL ||
        (length != 0 && bytes == NULL) ||
        (stream != BOXRIG_EMITTER_STANDARD &&
         stream != BOXRIG_EMITTER_DIAGNOSTIC)) {
        return -1;
    }
    return io->write(io->context, stream, bytes, length) == 0 ? 0 : -1;
}

size_t boxrig_emitter_case_count(void)
{
    return sizeof(cases) / sizeof(cases[0]);
}

const boxrig_emitter_case_info *boxrig_emitter_case_at(size_t index)
{
    if (index >= sizeof(cases) / sizeof(cases[0])) {
        return NULL;
    }
    return &cases[index].info;
}

int boxrig_emitter_run(
    int argument_count,
    const char *const *arguments,
    const boxrig_emitter_io *io)
{
    const emitter_case *test_case;
    case_runner cleanup;
    bool hold = true;
    int case_status;

    if (argument_count < 1 || arguments == NULL || io == NULL ||
        io->write == NULL) {
        return 1;
    }
    if (argument_count == 2 && strcmp(arguments[1], "--help") == 0) {
        return write_help(io) == 0 ? 0 : 1;
    }
    if (argument_count == 2 && strcmp(arguments[1], "--list") == 0) {
        return write_list(io) == 0 ? 0 : 1;
    }
    if (argument_count >= 2 && strcmp(arguments[1], "--describe") == 0) {
        if (argument_count == 2) {
            return write_cli_error(io, "missing case name", NULL);
        }
        if (argument_count != 3) {
            return write_cli_error(
                io,
                "unexpected argument: ",
                arguments[3]);
        }
        test_case = find_case(arguments[2]);
        if (test_case == NULL) {
            return write_cli_error(io, "unknown case: ", arguments[2]);
        }
        return write_description(io, &test_case->info) == 0 ? 0 : 1;
    }
    if (argument_count >= 2 && strcmp(arguments[1], "-t") == 0) {
        if (argument_count == 2) {
            return write_cli_error(io, "missing case name", NULL);
        }
        if (argument_count == 4 &&
            strcmp(arguments[3], "--no-hold") == 0) {
            hold = false;
        } else if (argument_count != 3) {
            return write_cli_error(
                io,
                "unexpected argument: ",
                arguments[3]);
        }
        test_case = find_case(arguments[2]);
        if (test_case == NULL) {
            return write_cli_error(io, "unknown case: ", arguments[2]);
        }
        cleanup = find_cleanup(test_case);
        case_status = test_case->run(io);
        if (case_status == 0 && hold && hold_for_enter(io) == -1) {
            case_status = -1;
        }
        if (cleanup != NULL && cleanup(io) == -1) {
            case_status = -1;
        }
        return case_status == 0 ? 0 : 1;
    }
    if (argument_count == 1) {
        return write_cli_error(io, "missing command", NULL);
    }
    return write_cli_error(io, "unexpected argument: ", arguments[1]);
}
