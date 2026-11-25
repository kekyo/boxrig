/* boxrig - Under MIT. */
#ifndef BOXRIG_CONFIG_H
#define BOXRIG_CONFIG_H

/** Two-byte ASCII/16-color cells for a single serial-log screen. Values: 0 or 1
 * (default 0). Rich-screen services and extended VT default to 0 in this build.
 * Transport encoding hooks can be selected with either cell representation. */
#ifndef BOXRIG_FEATURE_COMPACT_CELLS
#define BOXRIG_FEATURE_COMPACT_CELLS 0
#endif

/** Extended VT controls and host OSC/DCS protocols. A value of 0 selects log
 * controls (CR/LF/BS/HT, wrap, scroll, SGR 16 colors and EL). Defaults to 1
 * for Unicode cells and 0 for compact cells. Kitty APC is selected separately. */
#ifndef BOXRIG_FEATURE_EXTENDED_VT
#if BOXRIG_FEATURE_COMPACT_CELLS
#define BOXRIG_FEATURE_EXTENDED_VT 0
#else
#define BOXRIG_FEATURE_EXTENDED_VT 1
#endif
#endif

/** Add cursor positioning, visibility and display erase to log controls (0 or 1,
 * default 0). Works with either cell representation. Extended VT includes these. */
#ifndef BOXRIG_FEATURE_LOG_CURSOR
#define BOXRIG_FEATURE_LOG_CURSOR 0
#endif

/** Enable application-provided encoding hooks (0 or 1, default 1). */
#ifndef BOXRIG_FEATURE_ENCODING_HOOKS
#define BOXRIG_FEATURE_ENCODING_HOOKS 1
#endif

/** Kitty images and placements (requires libpng and zlib). Values: 0 or 1 (default 1 with Unicode cells, 0 with compact cells). */
#ifndef BOXRIG_FEATURE_KITTY_GRAPHICS
#if BOXRIG_FEATURE_COMPACT_CELLS
#define BOXRIG_FEATURE_KITTY_GRAPHICS 0
#else
#define BOXRIG_FEATURE_KITTY_GRAPHICS 1
#endif
#endif

/** Incremental text monitoring and its capture buffers. Values: 0 or 1 (default 1 with Unicode cells, 0 with compact cells). */
#ifndef BOXRIG_FEATURE_TEXT_MONITOR
#if BOXRIG_FEATURE_COMPACT_CELLS
#define BOXRIG_FEATURE_TEXT_MONITOR 0
#else
#define BOXRIG_FEATURE_TEXT_MONITOR 1
#endif
#endif

/** Regular-expression recognition of terminal text. Values: 0 or 1 (default 1 with Unicode cells, 0 with compact cells). */
#ifndef BOXRIG_FEATURE_TEXT_RECOGNIZER
#if BOXRIG_FEATURE_COMPACT_CELLS
#define BOXRIG_FEATURE_TEXT_RECOGNIZER 0
#else
#define BOXRIG_FEATURE_TEXT_RECOGNIZER 1
#endif
#endif

/** DEFLATE compression of scrollback (requires zlib). Values: 0 or 1 (default 1 with Unicode cells, 0 with compact cells). */
#ifndef BOXRIG_FEATURE_HISTORY_DEFLATE
#if BOXRIG_FEATURE_COMPACT_CELLS
#define BOXRIG_FEATURE_HISTORY_DEFLATE 0
#else
#define BOXRIG_FEATURE_HISTORY_DEFLATE 1
#endif
#endif

/** Terminal checkpoint export and import (requires zlib). Values: 0 or 1 (default 1 with Unicode cells, 0 with compact cells). */
#ifndef BOXRIG_FEATURE_CHECKPOINT
#if BOXRIG_FEATURE_COMPACT_CELLS
#define BOXRIG_FEATURE_CHECKPOINT 0
#else
#define BOXRIG_FEATURE_CHECKPOINT 1
#endif
#endif

/** A separate alternate screen for full-screen applications. Values: 0 or 1 (default 1 with Unicode cells, 0 with compact cells). */
#ifndef BOXRIG_FEATURE_ALT_SCREEN
#if BOXRIG_FEATURE_COMPACT_CELLS
#define BOXRIG_FEATURE_ALT_SCREEN 0
#else
#define BOXRIG_FEATURE_ALT_SCREEN 1
#endif
#endif

#if BOXRIG_FEATURE_COMPACT_CELLS && (BOXRIG_FEATURE_EXTENDED_VT || BOXRIG_FEATURE_KITTY_GRAPHICS || BOXRIG_FEATURE_TEXT_MONITOR || BOXRIG_FEATURE_TEXT_RECOGNIZER || BOXRIG_FEATURE_HISTORY_DEFLATE || BOXRIG_FEATURE_CHECKPOINT || BOXRIG_FEATURE_ALT_SCREEN)
#error "Compact cells support a single log screen. Select Unicode cells for rich-screen services."
#endif

#endif
