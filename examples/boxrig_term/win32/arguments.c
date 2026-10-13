/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */
#include "arguments.h"
#include <stddef.h>
#include <string.h>

const char *boxrig_term_win32_parse_transparency_mode(const char *value,
    boxrig_term_win32_transparency_mode *result)
{
    if (result == NULL) return "invalid transparency mode output";
    if (value == NULL || strcmp(value, "background") == 0)
        *result = BOXRIG_TERM_WIN32_TRANSPARENCY_BACKGROUND;
    else if (strcmp(value, "window") == 0)
        *result = BOXRIG_TERM_WIN32_TRANSPARENCY_WINDOW;
    else if (strcmp(value, "opaque") == 0)
        *result = BOXRIG_TERM_WIN32_TRANSPARENCY_OPAQUE;
    else return "BOXRIG_TRANSPARENCY_MODE must be background, window or opaque";
    return NULL;
}


const char *boxrig_term_win32_parse_transparency_fallback(const char *value,
    bool *result)
{
    if (result == NULL) return "invalid transparency fallback output";
    if (value == NULL || strcmp(value, "window") == 0) *result = true;
    else if (strcmp(value, "opaque") == 0) *result = false;
    else return "BOXRIG_TRANSPARENCY_FALLBACK must be opaque or window";
    return NULL;
}

const char *boxrig_term_win32_parse_window_opacity(const char *value,
    uint32_t *result)
{
    const char *error = "BOXRIG_WINDOW_OPACITY must be an integer from 1 through 100";
    uint32_t percent = 0;
    size_t index;
    if (result == NULL) return "invalid window opacity output";
    if (value == NULL) { *result = 100; return NULL; }
    if (value[0] == '\0') return error;
    for (index = 0; value[index] != '\0'; index++) {
        unsigned int digit;
        if (value[index] < '0' || value[index] > '9') return error;
        digit = (unsigned int)(value[index] - '0');
        if (percent > 10 || (percent == 10 && digit > 0)) return error;
        percent = percent * 10 + digit;
    }
    if (percent == 0) return error;
    *result = percent;
    return NULL;
}

static bool separator(wchar_t value)
{
    return value == L' ' || value == L'\t';
}

const char *boxrig_term_win32_renderer_name(boxrig_term_win32_renderer renderer)
{
    switch (renderer) {
    case BOXRIG_TERM_WIN32_RENDERER_AUTO: return "auto";
    case BOXRIG_TERM_WIN32_RENDERER_SOFTWARE: return "win32";
    case BOXRIG_TERM_WIN32_RENDERER_OPENGL: return "opengl";
    case BOXRIG_TERM_WIN32_RENDERER_D3D11: return "d3d11";
    default: return "invalid";
    }
}

const char *boxrig_term_win32_parse_arguments(
    const wchar_t *command_line, boxrig_term_win32_arguments *result)
{
    const wchar_t *cursor = command_line;
    bool selected = false;
    if (command_line == NULL || result == NULL) return "invalid command line";
    *result = (boxrig_term_win32_arguments){BOXRIG_TERM_WIN32_RENDERER_AUTO,
        command_line, false};
    for (;;) {
        const wchar_t *start;
        wchar_t token[128];
        size_t length = 0;
        bool quoted = false;
        while (separator(*cursor)) cursor++;
        start = cursor;
        result->command_line = start;
        if (*cursor == L'\0') return NULL;
        /* Decode only the leading option token. Child text is always borrowed
         * from start, including its original quoting and backslashes. */
        while (*cursor != L'\0' && (quoted || !separator(*cursor))) {
            size_t slashes = 0;
            while (*cursor == L'\\') { slashes++; cursor++; }
            if (*cursor == L'"') {
                size_t index;
                for (index = 0; index < slashes / 2; index++)
                    if (length < 127) token[length++] = L'\\';
                if (slashes % 2 != 0) {
                    if (length < 127) token[length++] = L'"';
                } else if (quoted && cursor[1] == L'"') {
                    if (length < 127) token[length++] = L'"';
                    cursor++;
                } else quoted = !quoted;
                cursor++;
            } else {
                size_t index;
                for (index = 0; index < slashes; index++)
                    if (length < 127) token[length++] = L'\\';
                if (*cursor != L'\0' && (quoted || !separator(*cursor))) {
                    if (length < 127) token[length++] = *cursor;
                    cursor++;
                }
            }
        }
        token[length] = L'\0';
        if (wcscmp(token, L"--") == 0) {
            while (separator(*cursor)) cursor++;
            if (*cursor == L'\0') return "-- requires a child command";
            result->command_line = cursor;
            return NULL;
        }
        if (wcscmp(token, L"--help") == 0) {
            result->help = true;
            return NULL;
        }
        if (wcsncmp(token, L"--renderer=", 11) != 0) return NULL;
        if (selected) return "--renderer may only be specified once";
        selected = true;
        if (quoted) return "unterminated renderer option quote";
        if (wcscmp(token + 11, L"auto") == 0)
            result->renderer = BOXRIG_TERM_WIN32_RENDERER_AUTO;
        else if (wcscmp(token + 11, L"win32") == 0)
            result->renderer = BOXRIG_TERM_WIN32_RENDERER_SOFTWARE;
        else if (wcscmp(token + 11, L"opengl") == 0)
            result->renderer = BOXRIG_TERM_WIN32_RENDERER_OPENGL;
        else if (wcscmp(token + 11, L"d3d11") == 0)
            result->renderer = BOXRIG_TERM_WIN32_RENDERER_D3D11;
        else return "invalid --renderer value";
    }
}
