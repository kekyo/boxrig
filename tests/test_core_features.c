/* boxrig - Under MIT. */
#include <boxrig/terminal.h>
#include <assert.h>
#include <string.h>

static void feed(boxrig_terminal *t, const char *s)
{
    for (size_t i = 0; s[i]; ++i) {
        size_t consumed;
        assert(boxrig_terminal_feed(t, (const uint8_t *)s + i, 1, &consumed) == BOXRIG_STATUS_OK && consumed == 1);
    }
}

int main(void)
{
    boxrig_terminal_config c = boxrig_terminal_config_default();
    c.rows = 2;
    c.columns = 8;
    c.scrollback_capacity = 3;
    boxrig_terminal *t = NULL;
    assert(boxrig_terminal_create(&c, &t) == BOXRIG_STATUS_OK);
#if !BOXRIG_FEATURE_TEXT_MONITOR
    assert(c.text_monitor_capacity == 0);
#endif
#if !BOXRIG_FEATURE_TEXT_RECOGNIZER
    assert(c.text_recognizer_capacity == 0);
#endif
#if !BOXRIG_FEATURE_KITTY_GRAPHICS
    assert(c.graphics_image_capacity == 0);
    feed(t, "\033_Ga=q,i=3;AAAA\033\\");
#endif
#if !BOXRIG_FEATURE_HISTORY_DEFLATE
    assert(boxrig_terminal_set_scrollback_compression(t, BOXRIG_SCROLLBACK_COMPRESSION_DEFLATE_LEVEL_1) != BOXRIG_STATUS_OK);
#endif
    feed(t, "A\r\nB\r\nC");
    assert(boxrig_terminal_scrollback_rows(t) == 1);
#if !BOXRIG_FEATURE_ALT_SCREEN
    feed(t, "\033[?1049hX\033[?1049l");
    boxrig_cell_view cell;
    assert(boxrig_terminal_get_cell(t,1,1,&cell) == BOXRIG_STATUS_OK && !memcmp(cell.text,"X",1));
#endif
    assert(boxrig_terminal_resize(t, 3, 12) == BOXRIG_STATUS_OK);
    boxrig_terminal_destroy(t);
}
