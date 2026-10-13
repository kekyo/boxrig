/* Win32 options preserve the child's original UTF-16 command line verbatim. */
#include "../examples/boxrig_term/win32/arguments.h"
#include <assert.h>
#include <stdio.h>
#include <wchar.h>

int main(void)
{
    const wchar_t *commands[] = {
        L"cmd.exe /d /c exit 0",
        L"\"C:\\Program Files\\child.exe\" \"two words\" \"日本語\" --renderer=child",
        L"child.exe \"\\\\\"escaped\\\"\" --renderer=d3d11"
    };
    boxrig_term_win32_arguments parsed;
    boxrig_term_win32_transparency_mode transparency;
    wchar_t input[1024];
    bool fallback;
    uint32_t window_percent;
    size_t i;
    assert(boxrig_term_win32_parse_transparency_mode(NULL, &transparency) == NULL);
    assert(transparency == BOXRIG_TERM_WIN32_TRANSPARENCY_BACKGROUND);
    assert(boxrig_term_win32_parse_transparency_mode("background", &transparency) == NULL);
    assert(transparency == BOXRIG_TERM_WIN32_TRANSPARENCY_BACKGROUND);
    assert(boxrig_term_win32_parse_transparency_mode("opaque", &transparency) == NULL);
    assert(transparency == BOXRIG_TERM_WIN32_TRANSPARENCY_OPAQUE);
    assert(boxrig_term_win32_parse_transparency_mode("", &transparency) != NULL);
    assert(boxrig_term_win32_parse_transparency_mode("bogus", &transparency) != NULL);
    assert(boxrig_term_win32_parse_transparency_mode("opaque", NULL) != NULL);
    assert(boxrig_term_win32_parse_transparency_mode("window", &transparency) == NULL);
    assert(transparency == BOXRIG_TERM_WIN32_TRANSPARENCY_WINDOW);
    assert(boxrig_term_win32_parse_transparency_fallback(NULL, &fallback) == NULL && fallback);
    assert(boxrig_term_win32_parse_transparency_fallback("window", &fallback) == NULL && fallback);
    assert(boxrig_term_win32_parse_transparency_fallback("", &fallback) != NULL && fallback);
    assert(boxrig_term_win32_parse_transparency_fallback("background", &fallback) != NULL && fallback);
    assert(boxrig_term_win32_parse_transparency_fallback("opaque", &fallback) == NULL && !fallback);
    assert(boxrig_term_win32_parse_transparency_fallback("window", NULL) != NULL);
    assert(boxrig_term_win32_parse_window_opacity(NULL, &window_percent) == NULL && window_percent == 100);
    assert(boxrig_term_win32_parse_window_opacity("1", &window_percent) == NULL && window_percent == 1);
    assert(boxrig_term_win32_parse_window_opacity("50", &window_percent) == NULL && window_percent == 50);
    assert(boxrig_term_win32_parse_window_opacity("100", &window_percent) == NULL && window_percent == 100);
    assert(boxrig_term_win32_parse_window_opacity("0", &window_percent) != NULL && window_percent == 100);
    assert(boxrig_term_win32_parse_window_opacity("", &window_percent) != NULL && window_percent == 100);
    assert(boxrig_term_win32_parse_window_opacity("101", &window_percent) != NULL && window_percent == 100);
    assert(boxrig_term_win32_parse_window_opacity("99999999999999999999", &window_percent) != NULL);
    assert(boxrig_term_win32_parse_window_opacity("1.5", &window_percent) != NULL);
    assert(boxrig_term_win32_parse_window_opacity("-1", &window_percent) != NULL);
    assert(boxrig_term_win32_parse_window_opacity(" 1", &window_percent) != NULL);
    assert(boxrig_term_win32_parse_window_opacity("1", NULL) != NULL);
    assert(boxrig_term_win32_parse_arguments(L"", &parsed) == NULL);
    assert(parsed.renderer == BOXRIG_TERM_WIN32_RENDERER_AUTO);
    assert(wcscmp(parsed.command_line, L"") == 0);
    assert(boxrig_term_win32_parse_arguments(L"--renderer=auto", &parsed) == NULL);
    assert(parsed.renderer == BOXRIG_TERM_WIN32_RENDERER_AUTO);
    assert(boxrig_term_win32_parse_arguments(L"--renderer=win32", &parsed) == NULL);
    assert(parsed.renderer == BOXRIG_TERM_WIN32_RENDERER_SOFTWARE);
    assert(boxrig_term_win32_parse_arguments(L"\"--renderer=opengl\"", &parsed) == NULL);
    assert(parsed.renderer == BOXRIG_TERM_WIN32_RENDERER_OPENGL);
    for (i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
        assert(swprintf(input, 1024, L"  --renderer=opengl -- %ls", commands[i]) > 0);
        assert(boxrig_term_win32_parse_arguments(input, &parsed) == NULL);
        assert(wcscmp(parsed.command_line, commands[i]) == 0);
        assert(boxrig_term_win32_parse_arguments(commands[i], &parsed) == NULL);
        assert(wcscmp(parsed.command_line, commands[i]) == 0);
    }
    assert(boxrig_term_win32_parse_arguments(L"-- -- child", &parsed) == NULL);
    assert(wcscmp(parsed.command_line, L"-- child") == 0);
    assert(boxrig_term_win32_parse_arguments(L"--renderer=\"opengl\"\t--\tchild", &parsed) == NULL);
    assert(parsed.renderer == BOXRIG_TERM_WIN32_RENDERER_OPENGL);
    assert(wcscmp(parsed.command_line, L"child") == 0);
    assert(boxrig_term_win32_parse_arguments(L"--renderer=d3d11", &parsed) == NULL);
    assert(parsed.renderer == BOXRIG_TERM_WIN32_RENDERER_D3D11);
    assert(boxrig_term_win32_parse_arguments(L"--renderer=bogus", &parsed) != NULL);
    assert(boxrig_term_win32_parse_arguments(L"--renderer=", &parsed) != NULL);
    assert(boxrig_term_win32_parse_arguments(L"--renderer=auto --renderer=win32", &parsed) != NULL);
    assert(boxrig_term_win32_parse_arguments(L"--renderer=opengl --", &parsed) != NULL);
    assert(boxrig_term_win32_parse_arguments(L"--", &parsed) != NULL);
    assert(boxrig_term_win32_parse_arguments(L"--help", &parsed) == NULL && parsed.help);
    puts("PASS Win32 renderer options and raw child command boundary");
    return 0;
}
