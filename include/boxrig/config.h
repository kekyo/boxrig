/* boxrig - Under MIT. */
#ifndef BOXRIG_CONFIG_H
#define BOXRIG_CONFIG_H

/** Enable application-provided encoding hooks (0 or 1, default 1). */
#ifndef BOXRIG_FEATURE_ENCODING_HOOKS
#define BOXRIG_FEATURE_ENCODING_HOOKS 1
#endif

/** Kitty images and placements (requires libpng and zlib). Values: 0 or 1 (default 1). */
#ifndef BOXRIG_FEATURE_KITTY_GRAPHICS
#define BOXRIG_FEATURE_KITTY_GRAPHICS 1
#endif

/** Incremental text monitoring and its capture buffers. Values: 0 or 1 (default 1). */
#ifndef BOXRIG_FEATURE_TEXT_MONITOR
#define BOXRIG_FEATURE_TEXT_MONITOR 1
#endif

/** Regular-expression recognition of terminal text. Values: 0 or 1 (default 1). */
#ifndef BOXRIG_FEATURE_TEXT_RECOGNIZER
#define BOXRIG_FEATURE_TEXT_RECOGNIZER 1
#endif

/** DEFLATE compression of scrollback (requires zlib). Values: 0 or 1 (default 1). */
#ifndef BOXRIG_FEATURE_HISTORY_DEFLATE
#define BOXRIG_FEATURE_HISTORY_DEFLATE 1
#endif

/** Terminal checkpoint export and import (requires zlib). Values: 0 or 1 (default 1). */
#ifndef BOXRIG_FEATURE_CHECKPOINT
#define BOXRIG_FEATURE_CHECKPOINT 1
#endif

/** A separate alternate screen for full-screen applications. Values: 0 or 1 (default 1). */
#ifndef BOXRIG_FEATURE_ALT_SCREEN
#define BOXRIG_FEATURE_ALT_SCREEN 1
#endif

#endif
