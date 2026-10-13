/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#define COBJMACROS


#include "renderer_private.h"
#include "text_private.h"
#include "text_state.h"
#include "system_library.h"
#include "boxrig/ui/image.h"
#include "boxrig/ui/pixel.h"
#include "boxrig/ui/scrollbar.h"

#include <dwrite_2.h>

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

uint64_t boxrig_win32_text_hash_append(uint64_t hash, const void *data, size_t length)
{
    const unsigned char *bytes = data;
    size_t index;

    for (index = 0; index < length; index++) {
        hash ^= bytes[index];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

uint64_t boxrig_win32_text_hash_u64(uint64_t hash, uint64_t value)
{
    return boxrig_win32_text_hash_append(hash, &value, sizeof(value));
}

static uint64_t color_key(uint64_t hash, boxrig_color color)
{
    hash = boxrig_win32_text_hash_u64(hash, (uint64_t)color.kind);
    hash = boxrig_win32_text_hash_u64(hash, color.index);
    hash = boxrig_win32_text_hash_u64(hash, color.red);
    hash = boxrig_win32_text_hash_u64(hash, color.green);
    return boxrig_win32_text_hash_u64(hash, color.blue);
}

uint64_t boxrig_win32_text_cell_style_key(const boxrig_cell_view *cell)
{
    uint64_t hash = UINT64_C(1469598103934665603);

    hash = boxrig_win32_text_hash_u64(hash, cell->attributes);
    hash = boxrig_win32_text_hash_u64(hash, (uint64_t)cell->underline_style);
    hash = boxrig_win32_text_hash_u64(hash, cell->protected_cell ? 1u : 0u);
    hash = color_key(hash, cell->foreground);
    hash = color_key(hash, cell->background);
    return color_key(hash, cell->underline_color);
}

uint64_t boxrig_win32_text_cell_hyperlink_key(const boxrig_cell_view *cell)
{
    uint64_t hash = UINT64_C(1469598103934665603);

    if (cell->hyperlink_uri != NULL) {
        hash = boxrig_win32_text_hash_append(
            hash,
            cell->hyperlink_uri,
            cell->hyperlink_uri_length);
    }
    hash = boxrig_win32_text_hash_u64(hash, cell->hyperlink_uri_length);
    if (cell->hyperlink_id != NULL) {
        hash = boxrig_win32_text_hash_append(
            hash,
            cell->hyperlink_id,
            cell->hyperlink_id_length);
    }
    return boxrig_win32_text_hash_u64(hash, cell->hyperlink_id_length);
}

static boxrig_win32_memory_font_loader *memory_loader_from_interface(
    IDWriteFontFileLoader *loader_interface)
{
    return CONTAINING_RECORD(
        loader_interface,
        boxrig_win32_memory_font_loader,
        loader_interface);
}

static boxrig_win32_memory_font_stream *memory_stream_from_interface(
    IDWriteFontFileStream *stream_interface)
{
    return CONTAINING_RECORD(
        stream_interface,
        boxrig_win32_memory_font_stream,
        stream_interface);
}

static ULONG memory_loader_add_reference(
    boxrig_win32_memory_font_loader *loader)
{
    return (ULONG)InterlockedIncrement(&loader->reference_count);
}

ULONG boxrig_win32_text_memory_loader_release_reference(
    boxrig_win32_memory_font_loader *loader)
{
    LONG remaining = InterlockedDecrement(&loader->reference_count);

    if (remaining == 0) {
        free(loader);
    }
    return (ULONG)remaining;
}

static HRESULT STDMETHODCALLTYPE memory_loader_query_interface(
    IDWriteFontFileLoader *loader_interface,
    REFIID interface_id,
    void **object_out)
{
    boxrig_win32_memory_font_loader *loader =
        memory_loader_from_interface(loader_interface);

    if (object_out == NULL) {
        return E_POINTER;
    }
    *object_out = NULL;
    if (!IsEqualIID(interface_id, &IID_IUnknown) &&
        !IsEqualIID(interface_id, &IID_IDWriteFontFileLoader)) {
        return E_NOINTERFACE;
    }
    *object_out = loader_interface;
    memory_loader_add_reference(loader);
    return S_OK;
}

static ULONG STDMETHODCALLTYPE memory_loader_add_ref(
    IDWriteFontFileLoader *loader_interface)
{
    return memory_loader_add_reference(
        memory_loader_from_interface(loader_interface));
}

static ULONG STDMETHODCALLTYPE memory_loader_release(
    IDWriteFontFileLoader *loader_interface)
{
    return boxrig_win32_text_memory_loader_release_reference(
        memory_loader_from_interface(loader_interface));
}

static HRESULT STDMETHODCALLTYPE memory_stream_query_interface(
    IDWriteFontFileStream *stream_interface,
    REFIID interface_id,
    void **object_out)
{
    boxrig_win32_memory_font_stream *stream =
        memory_stream_from_interface(stream_interface);

    if (object_out == NULL) {
        return E_POINTER;
    }
    *object_out = NULL;
    if (!IsEqualIID(interface_id, &IID_IUnknown) &&
        !IsEqualIID(interface_id, &IID_IDWriteFontFileStream)) {
        return E_NOINTERFACE;
    }
    *object_out = stream_interface;
    (void)InterlockedIncrement(&stream->reference_count);
    return S_OK;
}

static ULONG STDMETHODCALLTYPE memory_stream_add_ref(
    IDWriteFontFileStream *stream_interface)
{
    boxrig_win32_memory_font_stream *stream =
        memory_stream_from_interface(stream_interface);

    return (ULONG)InterlockedIncrement(&stream->reference_count);
}

static ULONG STDMETHODCALLTYPE memory_stream_release(
    IDWriteFontFileStream *stream_interface)
{
    boxrig_win32_memory_font_stream *stream =
        memory_stream_from_interface(stream_interface);
    LONG remaining = InterlockedDecrement(&stream->reference_count);

    if (remaining == 0) {
        boxrig_win32_text_memory_loader_release_reference(stream->loader);
        free(stream);
    }
    return (ULONG)remaining;
}

static HRESULT STDMETHODCALLTYPE memory_stream_read_fragment(
    IDWriteFontFileStream *stream_interface,
    const void **fragment_out,
    UINT64 offset,
    UINT64 fragment_size,
    void **fragment_context_out)
{
    boxrig_win32_memory_font_stream *stream =
        memory_stream_from_interface(stream_interface);

    if (fragment_out == NULL || fragment_context_out == NULL ||
        offset > stream->data_length ||
        fragment_size > stream->data_length - (size_t)offset) {
        return E_INVALIDARG;
    }
    *fragment_out = stream->data + (size_t)offset;
    *fragment_context_out = NULL;
    return S_OK;
}

static void STDMETHODCALLTYPE memory_stream_release_fragment(
    IDWriteFontFileStream *stream_interface,
    void *fragment_context)
{
    (void)stream_interface;
    (void)fragment_context;
}

static HRESULT STDMETHODCALLTYPE memory_stream_get_size(
    IDWriteFontFileStream *stream_interface,
    UINT64 *size_out)
{
    boxrig_win32_memory_font_stream *stream =
        memory_stream_from_interface(stream_interface);

    if (size_out == NULL) {
        return E_POINTER;
    }
    *size_out = stream->data_length;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE memory_stream_get_last_write_time(
    IDWriteFontFileStream *stream_interface,
    UINT64 *last_write_time_out)
{
    (void)stream_interface;
    if (last_write_time_out == NULL) {
        return E_POINTER;
    }
    *last_write_time_out = 0;
    return E_NOTIMPL;
}

static IDWriteFontFileStreamVtbl memory_stream_vtable = {
    memory_stream_query_interface,
    memory_stream_add_ref,
    memory_stream_release,
    memory_stream_read_fragment,
    memory_stream_release_fragment,
    memory_stream_get_size,
    memory_stream_get_last_write_time,
};

static HRESULT STDMETHODCALLTYPE memory_loader_create_stream(
    IDWriteFontFileLoader *loader_interface,
    const void *key,
    UINT32 key_size,
    IDWriteFontFileStream **stream_out)
{
    boxrig_win32_memory_font_loader *loader =
        memory_loader_from_interface(loader_interface);
    boxrig_win32_memory_font_stream *stream;
    boxrig_font_resource_id resource_id;
    const uint8_t *data;
    size_t data_length;

    if (key == NULL || key_size != sizeof(resource_id) ||
        stream_out == NULL) {
        return E_INVALIDARG;
    }
    *stream_out = NULL;
    memcpy(&resource_id, key, sizeof(resource_id));
    if (resource_id == BOXRIG_FONT_RESOURCE_NONE) {
        data = loader->validation_data;
        data_length = loader->validation_length;
    } else {
        boxrig_font_resource resource;

        if (loader->renderer == NULL ||
            boxrig_font_catalog_get_resource(
                loader->renderer->font_catalog,
                resource_id,
                &resource) != BOXRIG_FONT_CATALOG_OK ||
            resource.kind != BOXRIG_FONT_RESOURCE_MEMORY) {
            return E_INVALIDARG;
        }
        data = resource.data;
        data_length = resource.data_length;
    }
    if (data == NULL || data_length == 0) {
        return E_INVALIDARG;
    }
    stream = calloc(1, sizeof(*stream));
    if (stream == NULL) {
        return E_OUTOFMEMORY;
    }
    stream->stream_interface.lpVtbl = &memory_stream_vtable;
    stream->reference_count = 1;
    stream->loader = loader;
    stream->data = data;
    stream->data_length = data_length;
    memory_loader_add_reference(loader);
    *stream_out = &stream->stream_interface;
    return S_OK;
}

static IDWriteFontFileLoaderVtbl memory_loader_vtable = {
    memory_loader_query_interface,
    memory_loader_add_ref,
    memory_loader_release,
    memory_loader_create_stream,
};

boxrig_win32_memory_font_loader *boxrig_win32_text_memory_font_loader_create(
    boxrig_win32_renderer *renderer)
{
    boxrig_win32_memory_font_loader *loader = calloc(1, sizeof(*loader));

    if (loader == NULL) {
        return NULL;
    }
    loader->loader_interface.lpVtbl = &memory_loader_vtable;
    loader->reference_count = 1;
    loader->renderer = renderer;
    return loader;
}

static boxrig_win32_text_analysis *analysis_from_source(
    IDWriteTextAnalysisSource *source)
{
    return CONTAINING_RECORD(
        source,
        boxrig_win32_text_analysis,
        source_interface);
}

static boxrig_win32_text_analysis *analysis_from_sink(
    IDWriteTextAnalysisSink *sink)
{
    return CONTAINING_RECORD(
        sink,
        boxrig_win32_text_analysis,
        sink_interface);
}

static ULONG analysis_add_reference(boxrig_win32_text_analysis *analysis)
{
    return (ULONG)InterlockedIncrement(&analysis->reference_count);
}

static ULONG analysis_release_reference(boxrig_win32_text_analysis *analysis)
{
    LONG remaining = InterlockedDecrement(&analysis->reference_count);

    if (remaining == 0) {
        free(analysis->scripts);
        free(analysis->bidi_levels);
        free(analysis);
    }
    return (ULONG)remaining;
}

static HRESULT analysis_query_interface(
    boxrig_win32_text_analysis *analysis,
    REFIID interface_id,
    void **object_out)
{
    if (object_out == NULL) {
        return E_POINTER;
    }
    *object_out = NULL;
    if (IsEqualIID(interface_id, &IID_IUnknown) ||
        IsEqualIID(interface_id, &IID_IDWriteTextAnalysisSource)) {
        *object_out = &analysis->source_interface;
    } else if (IsEqualIID(interface_id, &IID_IDWriteTextAnalysisSink)) {
        *object_out = &analysis->sink_interface;
    } else {
        return E_NOINTERFACE;
    }
    analysis_add_reference(analysis);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE analysis_source_query_interface(
    IDWriteTextAnalysisSource *source,
    REFIID interface_id,
    void **object_out)
{
    return analysis_query_interface(
        analysis_from_source(source),
        interface_id,
        object_out);
}

static ULONG STDMETHODCALLTYPE analysis_source_add_ref(
    IDWriteTextAnalysisSource *source)
{
    return analysis_add_reference(analysis_from_source(source));
}

static ULONG STDMETHODCALLTYPE analysis_source_release(
    IDWriteTextAnalysisSource *source)
{
    return analysis_release_reference(analysis_from_source(source));
}

static HRESULT STDMETHODCALLTYPE analysis_get_text_at_position(
    IDWriteTextAnalysisSource *source,
    UINT32 position,
    const WCHAR **text_out,
    UINT32 *text_length_out)
{
    boxrig_win32_text_analysis *analysis = analysis_from_source(source);

    if (text_out == NULL || text_length_out == NULL ||
        position > analysis->text_length) {
        return E_INVALIDARG;
    }
    if (position == analysis->text_length) {
        *text_out = NULL;
        *text_length_out = 0;
    } else {
        *text_out = analysis->text + position;
        *text_length_out = analysis->text_length - position;
    }
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE analysis_get_text_before_position(
    IDWriteTextAnalysisSource *source,
    UINT32 position,
    const WCHAR **text_out,
    UINT32 *text_length_out)
{
    boxrig_win32_text_analysis *analysis = analysis_from_source(source);

    if (text_out == NULL || text_length_out == NULL ||
        position > analysis->text_length) {
        return E_INVALIDARG;
    }
    if (position == 0) {
        *text_out = NULL;
        *text_length_out = 0;
    } else {
        *text_out = analysis->text;
        *text_length_out = position;
    }
    return S_OK;
}

static DWRITE_READING_DIRECTION STDMETHODCALLTYPE
analysis_get_paragraph_direction(IDWriteTextAnalysisSource *source)
{
    return analysis_from_source(source)->reading_direction;
}

static HRESULT STDMETHODCALLTYPE analysis_get_locale_name(
    IDWriteTextAnalysisSource *source,
    UINT32 position,
    UINT32 *text_length_out,
    const WCHAR **locale_out)
{
    static const WCHAR locale_name[] = L"en-US";
    boxrig_win32_text_analysis *analysis = analysis_from_source(source);

    if (text_length_out == NULL || locale_out == NULL ||
        position > analysis->text_length) {
        return E_INVALIDARG;
    }
    *text_length_out = analysis->text_length - position;
    *locale_out = locale_name;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE analysis_get_number_substitution(
    IDWriteTextAnalysisSource *source,
    UINT32 position,
    UINT32 *text_length_out,
    IDWriteNumberSubstitution **substitution_out)
{
    boxrig_win32_text_analysis *analysis = analysis_from_source(source);

    if (text_length_out == NULL || substitution_out == NULL ||
        position > analysis->text_length) {
        return E_INVALIDARG;
    }
    *text_length_out = analysis->text_length - position;
    *substitution_out = NULL;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE analysis_sink_query_interface(
    IDWriteTextAnalysisSink *sink,
    REFIID interface_id,
    void **object_out)
{
    return analysis_query_interface(
        analysis_from_sink(sink),
        interface_id,
        object_out);
}

static ULONG STDMETHODCALLTYPE analysis_sink_add_ref(
    IDWriteTextAnalysisSink *sink)
{
    return analysis_add_reference(analysis_from_sink(sink));
}

static ULONG STDMETHODCALLTYPE analysis_sink_release(
    IDWriteTextAnalysisSink *sink)
{
    return analysis_release_reference(analysis_from_sink(sink));
}

static HRESULT STDMETHODCALLTYPE analysis_set_script(
    IDWriteTextAnalysisSink *sink,
    UINT32 position,
    UINT32 length,
    const DWRITE_SCRIPT_ANALYSIS *script)
{
    boxrig_win32_text_analysis *analysis = analysis_from_sink(sink);
    UINT32 index;

    if (script == NULL || position > analysis->text_length ||
        length > analysis->text_length - position) {
        return E_INVALIDARG;
    }
    for (index = position; index < position + length; index++) {
        analysis->scripts[index] = *script;
    }
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE analysis_set_line_breakpoints(
    IDWriteTextAnalysisSink *sink,
    UINT32 position,
    UINT32 length,
    const DWRITE_LINE_BREAKPOINT *breakpoints)
{
    boxrig_win32_text_analysis *analysis = analysis_from_sink(sink);

    if (breakpoints == NULL || position > analysis->text_length ||
        length > analysis->text_length - position) {
        return E_INVALIDARG;
    }
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE analysis_set_bidi_level(
    IDWriteTextAnalysisSink *sink,
    UINT32 position,
    UINT32 length,
    UINT8 explicit_level,
    UINT8 resolved_level)
{
    boxrig_win32_text_analysis *analysis = analysis_from_sink(sink);
    UINT32 index;

    (void)explicit_level;
    if (position > analysis->text_length ||
        length > analysis->text_length - position) {
        return E_INVALIDARG;
    }
    for (index = position; index < position + length; index++) {
        analysis->bidi_levels[index] = resolved_level;
    }
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE analysis_set_number_substitution(
    IDWriteTextAnalysisSink *sink,
    UINT32 position,
    UINT32 length,
    IDWriteNumberSubstitution *substitution)
{
    boxrig_win32_text_analysis *analysis = analysis_from_sink(sink);

    (void)substitution;
    if (position > analysis->text_length ||
        length > analysis->text_length - position) {
        return E_INVALIDARG;
    }
    return S_OK;
}

static IDWriteTextAnalysisSourceVtbl analysis_source_vtable = {
    analysis_source_query_interface,
    analysis_source_add_ref,
    analysis_source_release,
    analysis_get_text_at_position,
    analysis_get_text_before_position,
    analysis_get_paragraph_direction,
    analysis_get_locale_name,
    analysis_get_number_substitution,
};

static IDWriteTextAnalysisSinkVtbl analysis_sink_vtable = {
    analysis_sink_query_interface,
    analysis_sink_add_ref,
    analysis_sink_release,
    analysis_set_script,
    analysis_set_line_breakpoints,
    analysis_set_bidi_level,
    analysis_set_number_substitution,
};

static boxrig_win32_text_analysis *text_analysis_create(
    const WCHAR *text,
    UINT32 text_length,
    boxrig_text_direction_policy policy)
{
    boxrig_win32_text_analysis *analysis;
    UINT8 initial_level;
    UINT32 index;

    analysis = calloc(1, sizeof(*analysis));
    if (analysis == NULL) {
        return NULL;
    }
    analysis->scripts = calloc(text_length, sizeof(*analysis->scripts));
    analysis->bidi_levels = malloc(text_length * sizeof(*analysis->bidi_levels));
    if (analysis->scripts == NULL || analysis->bidi_levels == NULL) {
        free(analysis->scripts);
        free(analysis->bidi_levels);
        free(analysis);
        return NULL;
    }
    analysis->source_interface.lpVtbl = &analysis_source_vtable;
    analysis->sink_interface.lpVtbl = &analysis_sink_vtable;
    analysis->reference_count = 1;
    analysis->text = text;
    analysis->text_length = text_length;
    analysis->reading_direction =
        policy == BOXRIG_TEXT_DIRECTION_FORCE_RTL
            ? DWRITE_READING_DIRECTION_RIGHT_TO_LEFT
            : DWRITE_READING_DIRECTION_LEFT_TO_RIGHT;
    initial_level = policy == BOXRIG_TEXT_DIRECTION_FORCE_RTL ? 1 : 0;
    for (index = 0; index < text_length; index++) {
        analysis->bidi_levels[index] = initial_level;
    }
    return analysis;
}

static unsigned int positive_float_ceil(FLOAT value)
{
    unsigned int truncated;

    if (value <= 1.0f) {
        return 1;
    }
    if (value >= (FLOAT)UINT_MAX) {
        return UINT_MAX;
    }
    truncated = (unsigned int)value;
    return value > (FLOAT)truncated ? truncated + 1 : truncated;
}

static WCHAR *utf8_to_wide(const char *text, size_t text_length)
{
    WCHAR *wide;
    int wide_length;

    if (text == NULL || text_length == 0 || text_length > INT_MAX) {
        return NULL;
    }
    wide_length = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        text,
        (int)text_length,
        NULL,
        0);
    if (wide_length <= 0 ||
        (size_t)wide_length >= SIZE_MAX / sizeof(*wide)) {
        return NULL;
    }
    wide = malloc(((size_t)wide_length + 1) * sizeof(*wide));
    if (wide == NULL ||
        MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            text,
            (int)text_length,
            wide,
            wide_length) != wide_length) {
        free(wide);
        return NULL;
    }
    wide[wide_length] = L'\0';
    return wide;
}

static WCHAR *utf8_path_to_absolute_wide(
    const char *path,
    size_t path_length)
{
    WCHAR *wide = utf8_to_wide(path, path_length);
    WCHAR *absolute;
    DWORD required;
    DWORD written;

    if (wide == NULL) {
        return NULL;
    }
    required = GetFullPathNameW(wide, 0, NULL, NULL);
    if (required == 0) {
        free(wide);
        return NULL;
    }
#if SIZE_MAX < UINT64_MAX
    if ((uint64_t)required + 1 > SIZE_MAX / sizeof(*absolute)) {
        free(wide);
        return NULL;
    }
#endif
    absolute = malloc(((size_t)required + 1) * sizeof(*absolute));
    if (absolute == NULL) {
        free(wide);
        return NULL;
    }
    written = GetFullPathNameW(wide, required + 1, absolute, NULL);
    free(wide);
    if (written == 0 || written > required) {
        free(absolute);
        return NULL;
    }
    return absolute;
}

static HRESULT create_candidate_font_file(
    boxrig_win32_renderer *renderer,
    const boxrig_font_resource_candidate *candidate,
    IDWriteFontFile **font_file_out,
    size_t *byte_count_out)
{
    if (candidate->kind == BOXRIG_FONT_RESOURCE_FILE) {
        WIN32_FILE_ATTRIBUTE_DATA attributes;
        ULARGE_INTEGER file_size;
        WCHAR *path = utf8_path_to_absolute_wide(
            candidate->path,
            candidate->path_length);
        HRESULT result;

        if (path == NULL) {
            return HRESULT_FROM_WIN32(ERROR_INVALID_NAME);
        }
        if (!GetFileAttributesExW(
                path,
                GetFileExInfoStandard,
                &attributes)) {
            result = HRESULT_FROM_WIN32(GetLastError());
            free(path);
            return result;
        }
        file_size.LowPart = attributes.nFileSizeLow;
        file_size.HighPart = attributes.nFileSizeHigh;
        if (file_size.QuadPart == 0) {
            free(path);
            return E_INVALIDARG;
        }
#if SIZE_MAX < UINT64_MAX
        if (file_size.QuadPart > SIZE_MAX) {
            free(path);
            return E_INVALIDARG;
        }
#endif
        result = IDWriteFactory_CreateFontFileReference(
            renderer->factory,
            path,
            NULL,
            font_file_out);
        free(path);
        if (SUCCEEDED(result)) {
            *byte_count_out = (size_t)file_size.QuadPart;
        }
        return result;
    }
    if (candidate->kind == BOXRIG_FONT_RESOURCE_MEMORY) {
        boxrig_font_resource_id validation_key = BOXRIG_FONT_RESOURCE_NONE;
        HRESULT result;

        renderer->memory_font_loader->validation_data = candidate->data;
        renderer->memory_font_loader->validation_length =
            candidate->data_length;
        result = IDWriteFactory_CreateCustomFontFileReference(
            renderer->factory,
            &validation_key,
            sizeof(validation_key),
            &renderer->memory_font_loader->loader_interface,
            font_file_out);
        if (SUCCEEDED(result)) {
            *byte_count_out = candidate->data_length;
        }
        return result;
    }
    return E_INVALIDARG;
}

boxrig_font_catalog_status boxrig_win32_text_validate_font_resource(
    const boxrig_font_resource_candidate *candidate,
    size_t *byte_count_out,
    size_t *face_count_out,
    void *context)
{
    boxrig_win32_renderer *renderer = context;
    IDWriteFontFile *font_file = NULL;
    WINBOOL supported = FALSE;
    DWRITE_FONT_FILE_TYPE file_type;
    DWRITE_FONT_FACE_TYPE face_type;
    UINT32 face_count = 0;
    HRESULT result;

    result = create_candidate_font_file(
        renderer,
        candidate,
        &font_file,
        byte_count_out);
    if (SUCCEEDED(result)) {
        result = IDWriteFontFile_Analyze(
            font_file,
            &supported,
            &file_type,
            &face_type,
            &face_count);
    }
    if (font_file != NULL) {
        IDWriteFontFile_Release(font_file);
    }
    renderer->memory_font_loader->validation_data = NULL;
    renderer->memory_font_loader->validation_length = 0;
    if (FAILED(result)) {
        return candidate->kind == BOXRIG_FONT_RESOURCE_FILE
                   ? BOXRIG_FONT_CATALOG_IO_ERROR
                   : BOXRIG_FONT_CATALOG_INVALID_FONT;
    }
    if (!supported || face_count == 0 ||
        candidate->face_index >= face_count) {
        return BOXRIG_FONT_CATALOG_INVALID_FONT;
    }
    *face_count_out = face_count;
    return BOXRIG_FONT_CATALOG_OK;
}

static HRESULT create_registered_font(
    boxrig_win32_renderer *renderer,
    const boxrig_font_resource *resource,
    boxrig_win32_registered_font *registered_font)
{
    IDWriteFontFile *font_file = NULL;
    WINBOOL supported = FALSE;
    DWRITE_FONT_FILE_TYPE file_type;
    DWRITE_FONT_FACE_TYPE face_type;
    UINT32 face_count = 0;
    HRESULT result;

    if (resource->face_index > UINT32_MAX) {
        return E_INVALIDARG;
    }
    if (resource->kind == BOXRIG_FONT_RESOURCE_FILE) {
        WCHAR *path = utf8_path_to_absolute_wide(
            resource->path,
            resource->path_length);

        if (path == NULL) {
            return E_INVALIDARG;
        }
        result = IDWriteFactory_CreateFontFileReference(
            renderer->factory,
            path,
            NULL,
            &font_file);
        free(path);
    } else {
        result = IDWriteFactory_CreateCustomFontFileReference(
            renderer->factory,
            &resource->id,
            sizeof(resource->id),
            &renderer->memory_font_loader->loader_interface,
            &font_file);
    }
    if (SUCCEEDED(result)) {
        result = IDWriteFontFile_Analyze(
            font_file,
            &supported,
            &file_type,
            &face_type,
            &face_count);
    }
    if (SUCCEEDED(result) &&
        (!supported || resource->face_index >= face_count)) {
        result = DWRITE_E_FILEFORMAT;
    }
    if (SUCCEEDED(result)) {
        result = IDWriteFactory_CreateFontFace(
            renderer->factory,
            face_type,
            1,
            &font_file,
            (UINT32)resource->face_index,
            DWRITE_FONT_SIMULATIONS_NONE,
            &registered_font->font_face);
    }
    if (FAILED(result)) {
        if (font_file != NULL) {
            IDWriteFontFile_Release(font_file);
        }
        return result;
    }
    registered_font->resource_id = resource->id;
    registered_font->font_file = font_file;
    return S_OK;
}

static boxrig_win32_registered_font *find_registered_font(
    boxrig_win32_renderer *renderer,
    boxrig_font_resource_id resource_id)
{
    size_t index;

    for (index = 0; index < renderer->registered_font_count; index++) {
        if (renderer->registered_fonts[index].resource_id == resource_id) {
            return &renderer->registered_fonts[index];
        }
    }
    return NULL;
}

static HRESULT copy_family_name(
    IDWriteFontFamily *family,
    WCHAR *name_out,
    UINT32 capacity)
{
    IDWriteLocalizedStrings *names = NULL;
    UINT32 selected = 0;
    UINT32 length = 0;
    WINBOOL exists = FALSE;
    HRESULT result;

    result = IDWriteFontFamily_GetFamilyNames(family, &names);
    if (FAILED(result)) {
        return result;
    }
    result = IDWriteLocalizedStrings_FindLocaleName(
        names,
        L"en-US",
        &selected,
        &exists);
    if (FAILED(result)) {
        IDWriteLocalizedStrings_Release(names);
        return result;
    }
    if (!exists) {
        selected = 0;
    }
    result = IDWriteLocalizedStrings_GetStringLength(
        names,
        selected,
        &length);
    if (SUCCEEDED(result) && length + 1 > capacity) {
        result = E_NOT_SUFFICIENT_BUFFER;
    }
    if (SUCCEEDED(result)) {
        result = IDWriteLocalizedStrings_GetString(
            names,
            selected,
            name_out,
            capacity);
    }
    IDWriteLocalizedStrings_Release(names);
    return result;
}

static DWRITE_FONT_STYLE directwrite_font_style(boxrig_font_style style)
{
    if (style == BOXRIG_FONT_STYLE_ITALIC) {
        return DWRITE_FONT_STYLE_ITALIC;
    }
    if (style == BOXRIG_FONT_STYLE_OBLIQUE) {
        return DWRITE_FONT_STYLE_OBLIQUE;
    }
    return DWRITE_FONT_STYLE_NORMAL;
}

static DWRITE_FONT_STRETCH directwrite_font_stretch(
    boxrig_font_stretch stretch)
{
    if (stretch == BOXRIG_FONT_STRETCH_CONDENSED) {
        return DWRITE_FONT_STRETCH_CONDENSED;
    }
    if (stretch == BOXRIG_FONT_STRETCH_EXPANDED) {
        return DWRITE_FONT_STRETCH_EXPANDED;
    }
    return DWRITE_FONT_STRETCH_NORMAL;
}

static HRESULT find_system_font_face(
    boxrig_win32_renderer *renderer,
    const boxrig_font_specification *specification,
    IDWriteFontFace **font_face_out,
    WCHAR *family_name_out,
    UINT32 family_name_capacity)
{
    static const WCHAR *default_families[] = {
        L"Consolas",
        L"Cascadia Mono",
        L"Courier New",
        L"Liberation Mono",
        L"DejaVu Sans Mono",
    };
    IDWriteFontFamily *family = NULL;
    IDWriteFont *font = NULL;
    UINT32 family_count;
    UINT32 family_index = 0;
    UINT32 selected_index = 0;
    WINBOOL exists = FALSE;
    size_t candidate_index;
    HRESULT result = E_FAIL;

    *font_face_out = NULL;
    family_name_out[0] = L'\0';
    family_count = IDWriteFontCollection_GetFontFamilyCount(
        renderer->font_collection);
    if (family_count == 0) {
        return E_FAIL;
    }
    if (specification->family_count != 0) {
        for (candidate_index = 0;
             candidate_index < specification->family_count;
             candidate_index++) {
            const boxrig_font_family *candidate =
                &specification->families[candidate_index];
            WCHAR *wide_name;

            if (candidate->resource_id != BOXRIG_FONT_RESOURCE_NONE) {
                continue;
            }
            wide_name = utf8_to_wide(candidate->name, candidate->name_length);
            if (wide_name == NULL) {
                return E_INVALIDARG;
            }
            result = IDWriteFontCollection_FindFamilyName(
                renderer->font_collection,
                wide_name,
                &family_index,
                &exists);
            free(wide_name);
            if (FAILED(result)) {
                return result;
            }
            if (exists) {
                selected_index = family_index;
                break;
            }
        }
        if (!exists) {
            return E_FAIL;
        }
    } else {
        for (candidate_index = 0;
             candidate_index <
                 sizeof(default_families) / sizeof(default_families[0]);
             candidate_index++) {
            result = IDWriteFontCollection_FindFamilyName(
                renderer->font_collection,
                default_families[candidate_index],
                &family_index,
                &exists);
            if (FAILED(result)) {
                return result;
            }
            if (exists) {
                selected_index = family_index;
                break;
            }
        }
    }
    result = IDWriteFontCollection_GetFontFamily(
        renderer->font_collection,
        selected_index,
        &family);
    if (FAILED(result)) {
        return result;
    }
    result = copy_family_name(
        family,
        family_name_out,
        family_name_capacity);
    if (SUCCEEDED(result)) {
        result = IDWriteFontFamily_GetFirstMatchingFont(
            family,
            (DWRITE_FONT_WEIGHT)specification->weight,
            directwrite_font_stretch(specification->stretch),
            directwrite_font_style(specification->style),
            &font);
    }
    IDWriteFontFamily_Release(family);
    if (FAILED(result)) {
        return result;
    }
    result = IDWriteFont_CreateFontFace(font, font_face_out);
    IDWriteFont_Release(font);
    return result;
}

static uint64_t resolved_directwrite_face_key(
    IDWriteFontFace *font_face,
    const WCHAR *family_name,
    boxrig_font_resource_id primary_resource)
{
    uint64_t hash = UINT64_C(1469598103934665603);
    int family_length = family_name == NULL ? 0 : lstrlenW(family_name);

    if (family_length > 0) {
        hash = boxrig_win32_text_hash_append(
            hash,
            family_name,
            (size_t)family_length * sizeof(*family_name));
    }
    hash = boxrig_win32_text_hash_u64(hash, primary_resource);
    hash = boxrig_win32_text_hash_u64(hash, (uint64_t)IDWriteFontFace_GetType(font_face));
    hash = boxrig_win32_text_hash_u64(hash, IDWriteFontFace_GetIndex(font_face));
    hash = boxrig_win32_text_hash_u64(
        hash,
        (uint64_t)IDWriteFontFace_GetSimulations(font_face));
    return hash == 0 ? UINT64_C(1) : hash;
}

HRESULT boxrig_win32_text_measure_font_face(
    IDWriteFontFace *font_face,
    FLOAT font_size,
    const boxrig_font_rendering_policy *policy,
    boxrig_font_resource_id primary_resource,
    uint64_t primary_face_key,
    uint64_t layout_revision,
    boxrig_font_effective_metrics *metrics_out)
{
    DWRITE_FONT_METRICS font_metrics;
    DWRITE_GLYPH_METRICS glyph_metrics[2];
    boxrig_font_effective_metrics metrics;
    UINT32 codepoints[2] = {'0', 'M'};
    UINT16 glyph_indices[2];
    UINT32 maximum_advance;
    INT32 height_units;
    FLOAT glyph_width;
    FLOAT ascent;
    FLOAT descent;
    FLOAT line_gap;
    FLOAT horizontal_pixels_per_dip;
    FLOAT vertical_pixels_per_dip;
    unsigned int cell_width;
    unsigned int cell_height;
    HRESULT result;

    horizontal_pixels_per_dip =
        policy->dpi_x / 96.0f * policy->output_scale;
    vertical_pixels_per_dip =
        policy->dpi_y / 96.0f * policy->output_scale;

    IDWriteFontFace_GetMetrics(font_face, &font_metrics);
    if (font_metrics.designUnitsPerEm == 0) {
        return E_FAIL;
    }
    result = IDWriteFontFace_GetGlyphIndices(
        font_face,
        codepoints,
        2,
        glyph_indices);
    if (FAILED(result)) {
        return result;
    }
    result = IDWriteFontFace_GetDesignGlyphMetrics(
        font_face,
        glyph_indices,
        2,
        glyph_metrics,
        FALSE);
    if (FAILED(result)) {
        return result;
    }
    maximum_advance = glyph_metrics[0].advanceWidth;
    if (glyph_metrics[1].advanceWidth > maximum_advance) {
        maximum_advance = glyph_metrics[1].advanceWidth;
    }
    glyph_width =
        maximum_advance * font_size * horizontal_pixels_per_dip /
        font_metrics.designUnitsPerEm;
    ascent =
        font_metrics.ascent * font_size * vertical_pixels_per_dip /
        font_metrics.designUnitsPerEm;
    descent =
        font_metrics.descent * font_size * vertical_pixels_per_dip /
        font_metrics.designUnitsPerEm;
    line_gap =
        font_metrics.lineGap * font_size * vertical_pixels_per_dip /
        font_metrics.designUnitsPerEm;
    height_units = (INT32)font_metrics.ascent + font_metrics.descent +
                   font_metrics.lineGap;
    if (height_units <= 0) {
        height_units = (INT32)font_metrics.ascent + font_metrics.descent;
        line_gap = 0.0f;
    }
    cell_width = positive_float_ceil(glyph_width);
    cell_height = positive_float_ceil(
        height_units * font_size * vertical_pixels_per_dip /
        font_metrics.designUnitsPerEm);
    if (!(glyph_width > 0.0f) || !(ascent > 0.0f) || descent < 0.0f ||
        cell_width == UINT_MAX ||
        cell_height == UINT_MAX) {
        return E_FAIL;
    }
    memset(&metrics, 0, sizeof(metrics));
    metrics.requested_size_dip = font_size;
    metrics.primary_resource_id = primary_resource;
    metrics.primary_face_key = primary_face_key;
    metrics.dpi_x = policy->dpi_x;
    metrics.dpi_y = policy->dpi_y;
    metrics.output_scale = policy->output_scale;
    metrics.glyph_width = glyph_width;
    metrics.glyph_height = ascent + descent;
    metrics.ascent = ascent;
    metrics.descent = descent;
    metrics.line_gap = line_gap;
    metrics.cell_width = cell_width;
    metrics.cell_height = cell_height;
    metrics.baseline = positive_float_ceil(ascent);
    metrics.layout_revision = layout_revision;
    *metrics_out = metrics;
    return S_OK;
}

static uint32_t directwrite_opentype_tag(uint32_t tag)
{
    return ((tag & UINT32_C(0xff000000)) >> 24) |
           ((tag & UINT32_C(0x00ff0000)) >> 8) |
           ((tag & UINT32_C(0x0000ff00)) << 8) |
           ((tag & UINT32_C(0x000000ff)) << 24);
}

static DWRITE_PIXEL_GEOMETRY directwrite_pixel_geometry(
    boxrig_font_subpixel_order order,
    DWRITE_PIXEL_GEOMETRY system_geometry)
{
    if (order == BOXRIG_FONT_SUBPIXEL_RGB) {
        return DWRITE_PIXEL_GEOMETRY_RGB;
    }
    if (order == BOXRIG_FONT_SUBPIXEL_BGR) {
        return DWRITE_PIXEL_GEOMETRY_BGR;
    }
    if (order == BOXRIG_FONT_SUBPIXEL_VRGB ||
        order == BOXRIG_FONT_SUBPIXEL_VBGR) {
        return DWRITE_PIXEL_GEOMETRY_FLAT;
    }
    return system_geometry;
}

static HRESULT create_directwrite_rendering_parameters(
    boxrig_win32_renderer *renderer,
    const boxrig_font_rendering_policy *policy,
    IDWriteRenderingParams **parameters_out)
{
    IDWriteRenderingParams *system_parameters = NULL;
    IDWriteRenderingParams *base_parameters = NULL;
    IDWriteRenderingParams2 *parameters2 = NULL;
    DWRITE_PIXEL_GEOMETRY geometry;
    DWRITE_RENDERING_MODE mode;
    DWRITE_GRID_FIT_MODE grid_fit = DWRITE_GRID_FIT_MODE_DEFAULT;
    FLOAT gamma;
    FLOAT contrast;
    FLOAT clear_type_level;
    HRESULT result;

    *parameters_out = NULL;
    result = IDWriteFactory_CreateRenderingParams(
        renderer->factory,
        &system_parameters);
    if (FAILED(result)) {
        return result;
    }
    gamma = IDWriteRenderingParams_GetGamma(system_parameters);
    contrast = IDWriteRenderingParams_GetEnhancedContrast(system_parameters);
    clear_type_level =
        IDWriteRenderingParams_GetClearTypeLevel(system_parameters);
    geometry = directwrite_pixel_geometry(
        policy->subpixel_order,
        IDWriteRenderingParams_GetPixelGeometry(system_parameters));
    mode = IDWriteRenderingParams_GetRenderingMode(system_parameters);
    if (policy->antialias == BOXRIG_FONT_ANTIALIAS_GRAYSCALE) {
        clear_type_level = 0.0f;
        geometry = DWRITE_PIXEL_GEOMETRY_FLAT;
    } else if (policy->antialias == BOXRIG_FONT_ANTIALIAS_SUBPIXEL) {
        clear_type_level = 1.0f;
        if (geometry == DWRITE_PIXEL_GEOMETRY_FLAT) {
            geometry = DWRITE_PIXEL_GEOMETRY_RGB;
        }
    } else if (policy->antialias == BOXRIG_FONT_ANTIALIAS_NONE) {
        clear_type_level = 0.0f;
        geometry = DWRITE_PIXEL_GEOMETRY_FLAT;
        mode = DWRITE_RENDERING_MODE_ALIASED;
    }
    if (policy->hinting == BOXRIG_FONT_HINTING_ENABLED) {
        grid_fit = DWRITE_GRID_FIT_MODE_ENABLED;
    } else if (policy->hinting == BOXRIG_FONT_HINTING_DISABLED) {
        grid_fit = DWRITE_GRID_FIT_MODE_DISABLED;
    }
    if (policy->antialias == BOXRIG_FONT_ANTIALIAS_SYSTEM &&
        policy->hinting == BOXRIG_FONT_HINTING_SYSTEM &&
        policy->subpixel_order == BOXRIG_FONT_SUBPIXEL_UNKNOWN) {
        *parameters_out = system_parameters;
        return S_OK;
    }
    if (renderer->factory2 != NULL) {
        result = IDWriteFactory2_CreateCustomRenderingParams(
            renderer->factory2, gamma, contrast, contrast, clear_type_level,
            geometry, mode, grid_fit, &parameters2);
        if (SUCCEEDED(result)) {
            result = IDWriteRenderingParams2_QueryInterface(
                parameters2, &IID_IDWriteRenderingParams,
                (void **)&base_parameters);
            IDWriteRenderingParams2_Release(parameters2);
        }
    } else {
        /* Base DirectWrite uses its rendering mode's native grid-fitting policy. */
        result = IDWriteFactory_CreateCustomRenderingParams(
            renderer->factory, gamma, contrast, clear_type_level,
            geometry, mode, &base_parameters);
    }
    IDWriteRenderingParams_Release(system_parameters);
    if (FAILED(result)) {
        return result;
    }
    *parameters_out = base_parameters;
    return S_OK;
}

void boxrig_win32_renderer_font_candidate_destroy(
    boxrig_win32_renderer_font_candidate *candidate)
{
    if (candidate == NULL) {
        return;
    }
    if (candidate->font_face != NULL) {
        IDWriteFontFace_Release(candidate->font_face);
    }
    if (candidate->rendering_parameters != NULL) {
        IDWriteRenderingParams_Release(candidate->rendering_parameters);
    }
    free(candidate->features);
    free(candidate->variations);
    free(candidate);
}

boxrig_font_catalog_status boxrig_win32_renderer_prepare_font(
    boxrig_win32_renderer *renderer,
    const boxrig_font_specification *specification,
    const boxrig_font_rendering_policy *policy,
    boxrig_win32_renderer_font_candidate **candidate_out)
{
    boxrig_win32_renderer_font_candidate *candidate = NULL;
    boxrig_win32_registered_font *registered_font;
    uint64_t primary_face_key;
    uint64_t layout_revision;
    size_t index;
    HRESULT result;
    boxrig_font_catalog_status status;

    if (candidate_out == NULL) {
        return BOXRIG_FONT_CATALOG_INVALID_ARGUMENT;
    }
    *candidate_out = NULL;
    if (renderer == NULL) {
        return BOXRIG_FONT_CATALOG_INVALID_ARGUMENT;
    }
    status = boxrig_font_specification_validate(
        renderer->font_catalog,
        specification);
    if (status != BOXRIG_FONT_CATALOG_OK) {
        return status;
    }
    status = boxrig_font_rendering_policy_validate(policy);
    if (status != BOXRIG_FONT_CATALOG_OK ||
        specification->feature_count > UINT32_MAX) {
        return status != BOXRIG_FONT_CATALOG_OK
                   ? status
                   : BOXRIG_FONT_CATALOG_LIMIT_EXCEEDED;
    }
    if (renderer->effective_metrics.layout_revision == UINT64_MAX) {
        return BOXRIG_FONT_CATALOG_LIMIT_EXCEEDED;
    }
    candidate = calloc(1, sizeof(*candidate));
    if (candidate == NULL) {
        return BOXRIG_FONT_CATALOG_OUT_OF_MEMORY;
    }
    candidate->renderer = renderer;
    layout_revision = renderer->effective_metrics.layout_revision + 1;
    candidate->vertical_pixels_per_dip =
        policy->dpi_y / 96.0f * policy->output_scale;
    if (!(candidate->vertical_pixels_per_dip > 0.0f)) {
        status = BOXRIG_FONT_CATALOG_INVALID_ARGUMENT;
        goto failure;
    }
    if (specification->family_count != 0 &&
        specification->families[0].resource_id !=
            BOXRIG_FONT_RESOURCE_NONE) {
        candidate->primary_resource =
            specification->families[0].resource_id;
        registered_font = find_registered_font(
            renderer,
            candidate->primary_resource);
        if (registered_font == NULL) {
            status = BOXRIG_FONT_CATALOG_NOT_FOUND;
            goto failure;
        }
        candidate->font_face = registered_font->font_face;
        IDWriteFontFace_AddRef(candidate->font_face);
    } else {
        result = find_system_font_face(
            renderer,
            specification,
            &candidate->font_face,
            candidate->family_name,
            (UINT32)(sizeof(candidate->family_name) /
                     sizeof(candidate->family_name[0])));
        if (FAILED(result)) {
            status = result == E_OUTOFMEMORY
                         ? BOXRIG_FONT_CATALOG_OUT_OF_MEMORY
                         : BOXRIG_FONT_CATALOG_INVALID_FONT;
            goto failure;
        }
    }
    primary_face_key = resolved_directwrite_face_key(
        candidate->font_face,
        candidate->family_name,
        candidate->primary_resource);
    result = boxrig_win32_text_measure_font_face(
        candidate->font_face,
        specification->size_dip,
        policy,
        candidate->primary_resource,
        primary_face_key,
        layout_revision,
        &candidate->natural_metrics);
    if (FAILED(result)) {
        status = BOXRIG_FONT_CATALOG_INVALID_FONT;
        goto failure;
    }
    status = boxrig_font_effective_metrics_apply_line_height(
        &candidate->natural_metrics,
        specification->line_height_adjustment_dip,
        &candidate->effective_metrics);
    if (status != BOXRIG_FONT_CATALOG_OK) {
        goto failure;
    }
    if (specification->feature_count != 0) {
        if (specification->feature_count >
            SIZE_MAX / sizeof(*candidate->features)) {
            status = BOXRIG_FONT_CATALOG_LIMIT_EXCEEDED;
            goto failure;
        }
        candidate->features = calloc(
            specification->feature_count,
            sizeof(*candidate->features));
        if (candidate->features == NULL) {
            status = BOXRIG_FONT_CATALOG_OUT_OF_MEMORY;
            goto failure;
        }
        for (index = 0; index < specification->feature_count; index++) {
            candidate->features[index].nameTag =
                directwrite_opentype_tag(specification->features[index].tag);
            candidate->features[index].parameter =
                specification->features[index].value;
        }
    }
    if (specification->variation_count != 0) {
        if (specification->variation_count >
            SIZE_MAX / sizeof(*candidate->variations)) {
            status = BOXRIG_FONT_CATALOG_LIMIT_EXCEEDED;
            goto failure;
        }
        candidate->variations = malloc(
            specification->variation_count *
            sizeof(*candidate->variations));
        if (candidate->variations == NULL) {
            status = BOXRIG_FONT_CATALOG_OUT_OF_MEMORY;
            goto failure;
        }
        memcpy(
            candidate->variations,
            specification->variations,
            specification->variation_count *
                sizeof(*candidate->variations));
    }
    result = create_directwrite_rendering_parameters(
        renderer,
        policy,
        &candidate->rendering_parameters);
    if (FAILED(result)) {
        status = result == E_OUTOFMEMORY
                     ? BOXRIG_FONT_CATALOG_OUT_OF_MEMORY
                     : BOXRIG_FONT_CATALOG_INVALID_ARGUMENT;
        goto failure;
    }
    candidate->font_size = specification->size_dip;
    candidate->line_height_adjustment_dip =
        specification->line_height_adjustment_dip;
    candidate->fallback_policy = specification->fallback;
    candidate->rendering_policy = *policy;
    candidate->measuring_mode = policy->fractional_positions
                                    ? DWRITE_MEASURING_MODE_NATURAL
                                    : DWRITE_MEASURING_MODE_GDI_NATURAL;
    candidate->feature_count = (UINT32)specification->feature_count;
    candidate->variation_count = specification->variation_count;
    *candidate_out = candidate;
    return BOXRIG_FONT_CATALOG_OK;

failure:
    boxrig_win32_renderer_font_candidate_destroy(candidate);
    return status;
}

boxrig_font_catalog_status boxrig_win32_renderer_font_candidate_metrics(
    const boxrig_win32_renderer_font_candidate *candidate,
    boxrig_font_effective_metrics *metrics_out)
{
    if (candidate == NULL || metrics_out == NULL) {
        return BOXRIG_FONT_CATALOG_INVALID_ARGUMENT;
    }
    *metrics_out = candidate->effective_metrics;
    return BOXRIG_FONT_CATALOG_OK;
}

boxrig_font_catalog_status boxrig_win32_renderer_commit_font(
    boxrig_win32_renderer *renderer,
    boxrig_win32_renderer_font_candidate *candidate)
{
    HRESULT result;

    if (renderer == NULL || candidate == NULL ||
        candidate->renderer != renderer || candidate->font_face == NULL ||
        candidate->rendering_parameters == NULL) {
        return BOXRIG_FONT_CATALOG_INVALID_ARGUMENT;
    }
    result = renderer->apply_raster_scale == NULL ? S_OK :
        renderer->apply_raster_scale(renderer, candidate->vertical_pixels_per_dip);
    if (FAILED(result)) {
        return BOXRIG_FONT_CATALOG_INVALID_ARGUMENT;
    }
    if (renderer->primary_font_face != NULL) {
        IDWriteFontFace_Release(renderer->primary_font_face);
    }
    renderer->primary_font_face = candidate->font_face;
    candidate->font_face = NULL;
    if (renderer->rendering_parameters != NULL) {
        IDWriteRenderingParams_Release(renderer->rendering_parameters);
    }
    renderer->rendering_parameters = candidate->rendering_parameters;
    candidate->rendering_parameters = NULL;
    memcpy(
        renderer->primary_family,
        candidate->family_name,
        sizeof(candidate->family_name));
    renderer->font_size = candidate->font_size;
    renderer->natural_metrics = candidate->natural_metrics;
    renderer->effective_metrics = candidate->effective_metrics;
    renderer->line_height_adjustment_dip =
        candidate->line_height_adjustment_dip;
    renderer->cell_width = candidate->effective_metrics.cell_width;
    renderer->cell_height = candidate->effective_metrics.cell_height;
    renderer->ascent = candidate->effective_metrics.baseline;
    renderer->primary_font_resource = candidate->primary_resource;
    renderer->fallback_policy = candidate->fallback_policy;
    renderer->rendering_policy = candidate->rendering_policy;
    renderer->measuring_mode = candidate->measuring_mode;
    free(renderer->font_features);
    renderer->font_features = candidate->features;
    candidate->features = NULL;
    renderer->font_feature_count = candidate->feature_count;
    free(renderer->font_variations);
    renderer->font_variations = candidate->variations;
    candidate->variations = NULL;
    renderer->font_variation_count = candidate->variation_count;
    boxrig_font_visual_step_sequence_destroy(renderer->visual_steps);
    renderer->visual_steps = NULL;
    renderer->visual_step_policy_valid = false;
    boxrig_win32_text_request_full_raster(renderer);
    boxrig_win32_renderer_font_candidate_destroy(candidate);
    return BOXRIG_FONT_CATALOG_OK;
}

boxrig_font_catalog_status boxrig_win32_renderer_set_font(
    boxrig_win32_renderer *renderer,
    const boxrig_font_specification *specification,
    const boxrig_font_rendering_policy *policy)
{
    boxrig_win32_renderer_font_candidate *candidate = NULL;
    boxrig_font_catalog_status status = boxrig_win32_renderer_prepare_font(
        renderer,
        specification,
        policy,
        &candidate);

    if (status != BOXRIG_FONT_CATALOG_OK) {
        return status;
    }
    status = boxrig_win32_renderer_commit_font(renderer, candidate);
    if (status != BOXRIG_FONT_CATALOG_OK) {
        boxrig_win32_renderer_font_candidate_destroy(candidate);
    }
    return status;
}

static boxrig_font_catalog_status register_win32_font_resource(
    boxrig_win32_renderer *renderer,
    boxrig_font_resource_id resource_id)
{
    boxrig_font_resource resource;
    boxrig_win32_registered_font *registered_font;

    if (renderer->registered_font_count >=
        BOXRIG_WIN32_REGISTERED_FONT_LIMIT) {
        return BOXRIG_FONT_CATALOG_LIMIT_EXCEEDED;
    }
    if (boxrig_font_catalog_get_resource(
            renderer->font_catalog,
            resource_id,
            &resource) != BOXRIG_FONT_CATALOG_OK) {
        return BOXRIG_FONT_CATALOG_NOT_FOUND;
    }
    registered_font =
        &renderer->registered_fonts[renderer->registered_font_count];
    memset(registered_font, 0, sizeof(*registered_font));
    if (FAILED(create_registered_font(
            renderer,
            &resource,
            registered_font))) {
        memset(registered_font, 0, sizeof(*registered_font));
        return BOXRIG_FONT_CATALOG_INVALID_FONT;
    }
    renderer->registered_font_count++;
    return BOXRIG_FONT_CATALOG_OK;
}

boxrig_font_catalog_status boxrig_win32_renderer_register_font_file(
    boxrig_win32_renderer *renderer,
    const char *path,
    size_t path_length,
    size_t face_index,
    boxrig_font_resource_id *resource_id_out)
{
    boxrig_font_catalog_status status;

    if (renderer == NULL) {
        return BOXRIG_FONT_CATALOG_INVALID_ARGUMENT;
    }
    status = boxrig_font_catalog_register_file(
        renderer->font_catalog,
        path,
        path_length,
        face_index,
        resource_id_out);
    if (status != BOXRIG_FONT_CATALOG_OK) {
        return status;
    }
    status = register_win32_font_resource(renderer, *resource_id_out);
    if (status != BOXRIG_FONT_CATALOG_OK) {
        (void)boxrig_font_catalog_remove(
            renderer->font_catalog,
            *resource_id_out);
        *resource_id_out = BOXRIG_FONT_RESOURCE_NONE;
    }
    return status;
}

boxrig_font_catalog_status boxrig_win32_renderer_register_font_memory(
    boxrig_win32_renderer *renderer,
    const uint8_t *data,
    size_t data_length,
    size_t face_index,
    boxrig_font_resource_id *resource_id_out)
{
    boxrig_font_catalog_status status;

    if (renderer == NULL) {
        return BOXRIG_FONT_CATALOG_INVALID_ARGUMENT;
    }
    status = boxrig_font_catalog_register_memory(
        renderer->font_catalog,
        data,
        data_length,
        face_index,
        resource_id_out);
    if (status != BOXRIG_FONT_CATALOG_OK) {
        return status;
    }
    status = register_win32_font_resource(renderer, *resource_id_out);
    if (status != BOXRIG_FONT_CATALOG_OK) {
        (void)boxrig_font_catalog_remove(
            renderer->font_catalog,
            *resource_id_out);
        *resource_id_out = BOXRIG_FONT_RESOURCE_NONE;
    }
    return status;
}

boxrig_font_resource_id boxrig_win32_renderer_primary_font_resource(
    const boxrig_win32_renderer *renderer)
{
    return renderer == NULL ? BOXRIG_FONT_RESOURCE_NONE
                            : renderer->primary_font_resource;
}

uint64_t boxrig_win32_renderer_font_catalog_revision(
    const boxrig_win32_renderer *renderer)
{
    return renderer == NULL
               ? 0
               : boxrig_font_catalog_revision(renderer->font_catalog);
}

void boxrig_win32_text_row_shape_destroy(boxrig_win32_row_shape *shape)
{
    size_t run_index;

    for (run_index = 0; run_index < shape->run_count; run_index++) {
        boxrig_win32_shaped_run *run = &shape->runs[run_index];

        if (run->font_face != NULL) {
            IDWriteFontFace_Release(run->font_face);
        }
        free(run->cluster_map);
        free(run->text_properties);
        free(run->glyph_indices);
        free(run->glyph_properties);
        free(run->glyph_advances);
        free(run->glyph_offsets);
    }
    free(shape->runs);
    free(shape->clusters);
    free(shape->text);
    boxrig_text_layout_destroy(shape->layout);
    memset(shape, 0, sizeof(*shape));
}

static int utf8_required_utf16_units(const char *text, size_t length)
{
    if (text == NULL || length == 0 || length > (size_t)INT_MAX) {
        return 0;
    }
    return MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        text,
        (int)length,
        NULL,
        0);
}

static int collect_logical_clusters(
    const boxrig_terminal *terminal,
    size_t row,
    size_t column_count,
    boxrig_win32_row_shape *shape)
{
    size_t text_capacity = 0;
    size_t column;

    if (column_count > SIZE_MAX / sizeof(*shape->clusters)) {
        return -1;
    }
    shape->clusters = calloc(column_count, sizeof(*shape->clusters));
    if (shape->clusters == NULL) {
        return -1;
    }
    for (column = 0; column < column_count;) {
        boxrig_cell_view cell;
        const char blank = ' ';
        const char *source;
        size_t source_length;
        size_t width;
        int required;

        if (boxrig_terminal_get_cell(terminal, row, column, &cell) !=
                BOXRIG_STATUS_OK ||
            cell.continuation) {
            return -1;
        }
        width = cell.column_width == 0 ? 1 : cell.column_width;
        source = cell.text_length == 0 ? &blank : cell.text;
        source_length = cell.text_length == 0 ? 1 : cell.text_length;
        required = utf8_required_utf16_units(source, source_length);
        if (width > column_count - column || required <= 0 ||
            (size_t)required > UINT32_MAX - text_capacity) {
            return -1;
        }
        text_capacity += (size_t)required;
        column += width;
        shape->cluster_count++;
    }
    if (text_capacity == 0 ||
        text_capacity > (SIZE_MAX / sizeof(*shape->text)) - 1) {
        return -1;
    }
    shape->text = calloc(text_capacity + 1, sizeof(*shape->text));
    if (shape->text == NULL) {
        return -1;
    }
    shape->cluster_count = 0;
    for (column = 0; column < column_count;) {
        boxrig_win32_logical_cluster *cluster =
            &shape->clusters[shape->cluster_count];
        const char blank = ' ';
        const char *source;
        size_t source_length;
        int written;

        if (boxrig_terminal_get_cell(terminal, row, column, &cluster->cell) !=
            BOXRIG_STATUS_OK) {
            return -1;
        }
        cluster->logical_column = column;
        cluster->column_count = cluster->cell.column_width == 0
                                    ? 1
                                    : cluster->cell.column_width;
        source = cluster->cell.text_length == 0 ? &blank : cluster->cell.text;
        source_length = cluster->cell.text_length == 0
                            ? 1
                            : cluster->cell.text_length;
        cluster->text_offset = shape->text_length;
        written = MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            source,
            (int)source_length,
            shape->text + shape->text_length,
            (int)(text_capacity - shape->text_length));
        if (written <= 0) {
            return -1;
        }
        cluster->text_length = (UINT32)written;
        shape->text_length += (UINT32)written;
        cluster->style_key = boxrig_win32_text_cell_style_key(&cluster->cell);
        cluster->hyperlink_key = boxrig_win32_text_cell_hyperlink_key(&cluster->cell);
        shape->cluster_count++;
        column += cluster->column_count;
    }
    return shape->text_length == text_capacity ? 0 : -1;
}

int boxrig_win32_text_collect_composition_clusters(
    const boxrig_composition *composition,
    size_t first_cluster,
    size_t cluster_count,
    boxrig_win32_row_shape *shape)
{
    size_t text_capacity = 0;
    size_t logical_column = 0;
    size_t index;

    memset(shape, 0, sizeof(*shape));
    if (cluster_count == 0 ||
        cluster_count > SIZE_MAX / sizeof(*shape->clusters)) {
        return -1;
    }
    shape->clusters = calloc(cluster_count, sizeof(*shape->clusters));
    if (shape->clusters == NULL) {
        return -1;
    }
    for (index = 0; index < cluster_count; index++) {
        boxrig_composition_cluster_view cluster;
        int required;

        if (boxrig_composition_get_cluster(
                composition,
                first_cluster + index,
                &cluster) != BOXRIG_COMPOSITION_OK) {
            return -1;
        }
        required = utf8_required_utf16_units(
            cluster.text,
            cluster.text_length);
        if (required <= 0 ||
            (size_t)required > UINT32_MAX - text_capacity) {
            return -1;
        }
        text_capacity += (size_t)required;
    }
    if (text_capacity == 0 ||
        text_capacity > (SIZE_MAX / sizeof(*shape->text)) - 1) {
        return -1;
    }
    shape->text = calloc(text_capacity + 1, sizeof(*shape->text));
    if (shape->text == NULL) {
        return -1;
    }
    for (index = 0; index < cluster_count; index++) {
        boxrig_composition_cluster_view source;
        boxrig_win32_logical_cluster *cluster = &shape->clusters[index];
        int written;

        if (boxrig_composition_get_cluster(
                composition,
                first_cluster + index,
                &source) != BOXRIG_COMPOSITION_OK ||
            source.column_count > SIZE_MAX - logical_column) {
            return -1;
        }
        cluster->logical_column = logical_column;
        cluster->column_count = source.column_count;
        cluster->text_offset = shape->text_length;
        written = MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            source.text,
            (int)source.text_length,
            shape->text + shape->text_length,
            (int)(text_capacity - shape->text_length));
        if (written <= 0) {
            return -1;
        }
        cluster->text_length = (UINT32)written;
        shape->text_length += (UINT32)written;
        logical_column += source.column_count;
    }
    shape->cluster_count = cluster_count;
    return shape->text_length == text_capacity ? 0 : -1;
}

static size_t cluster_index_at_text_position(
    const boxrig_win32_row_shape *shape,
    UINT32 position)
{
    size_t cluster_index;

    for (cluster_index = 0;
         cluster_index < shape->cluster_count;
         cluster_index++) {
        const boxrig_win32_logical_cluster *cluster =
            &shape->clusters[cluster_index];

        if (position >= cluster->text_offset &&
            position < cluster->text_offset + cluster->text_length) {
            return cluster_index;
        }
    }
    return SIZE_MAX;
}

static DWRITE_FONT_WEIGHT cluster_font_weight(
    const boxrig_win32_logical_cluster *cluster)
{
    if ((cluster->cell.attributes & BOXRIG_CELL_BOLD) != 0) {
        return DWRITE_FONT_WEIGHT_BOLD;
    }
    if ((cluster->cell.attributes & BOXRIG_CELL_FAINT) != 0) {
        return DWRITE_FONT_WEIGHT_LIGHT;
    }
    return DWRITE_FONT_WEIGHT_NORMAL;
}

static DWRITE_FONT_STYLE cluster_font_style(
    const boxrig_win32_logical_cluster *cluster)
{
    return (cluster->cell.attributes & BOXRIG_CELL_ITALIC) != 0
               ? DWRITE_FONT_STYLE_ITALIC
               : DWRITE_FONT_STYLE_NORMAL;
}

static UINT32 analysis_run_limit(
    const boxrig_win32_row_shape *shape,
    const boxrig_win32_text_analysis *analysis,
    UINT32 position)
{
    size_t initial_cluster_index = cluster_index_at_text_position(
        shape,
        position);
    const boxrig_win32_logical_cluster *initial_cluster;
    UINT32 limit;

    if (initial_cluster_index == SIZE_MAX) {
        return position;
    }
    initial_cluster = &shape->clusters[initial_cluster_index];
    for (limit = position + 1; limit < shape->text_length; limit++) {
        size_t cluster_index = cluster_index_at_text_position(shape, limit);
        const boxrig_win32_logical_cluster *cluster;

        if (cluster_index == SIZE_MAX) {
            break;
        }
        cluster = &shape->clusters[cluster_index];
        if (analysis->bidi_levels[limit] !=
                analysis->bidi_levels[position] ||
            analysis->scripts[limit].script !=
                analysis->scripts[position].script ||
            analysis->scripts[limit].shapes !=
                analysis->scripts[position].shapes ||
            cluster->style_key != initial_cluster->style_key ||
            cluster->hyperlink_key != initial_cluster->hyperlink_key) {
            break;
        }
    }
    return limit;
}

static HRESULT create_base_system_face(
    boxrig_win32_renderer *renderer,
    const boxrig_win32_logical_cluster *cluster,
    IDWriteFontFace **face_out)
{
    IDWriteFontFamily *family = NULL;
    IDWriteFont *font = NULL;
    UINT32 index = 0;
    BOOL exists = FALSE;
    HRESULT result = IDWriteFontCollection_FindFamilyName(
        renderer->font_collection, renderer->primary_family, &index, &exists);

    if (SUCCEEDED(result) && !exists) {
        return E_FAIL;
    }
    if (SUCCEEDED(result)) {
        result = IDWriteFontCollection_GetFontFamily(
            renderer->font_collection, index, &family);
    }
    if (SUCCEEDED(result)) {
        result = IDWriteFontFamily_GetFirstMatchingFont(
            family, cluster_font_weight(cluster), DWRITE_FONT_STRETCH_NORMAL,
            cluster_font_style(cluster), &font);
    }
    if (SUCCEEDED(result)) {
        result = IDWriteFont_CreateFontFace(font, face_out);
    }
    if (font != NULL) {
        IDWriteFont_Release(font);
    }
    if (family != NULL) {
        IDWriteFontFamily_Release(family);
    }
    return result;
}

static int map_base_font_run(
    boxrig_win32_renderer *renderer,
    boxrig_win32_row_shape *shape,
    const boxrig_win32_text_analysis *analysis,
    UINT32 position,
    UINT32 limit,
    IDWriteFontFace **face,
    UINT32 *length_out);

static int create_font_runs(
    boxrig_win32_renderer *renderer,
    boxrig_win32_row_shape *shape,
    boxrig_win32_text_analysis *analysis)
{
    UINT32 position = 0;

    shape->runs = calloc(shape->text_length, sizeof(*shape->runs));
    if (shape->runs == NULL) {
        return -1;
    }
    while (position < shape->text_length) {
        size_t cluster_index = cluster_index_at_text_position(shape, position);
        const boxrig_win32_logical_cluster *cluster;
        boxrig_win32_shaped_run *run = &shape->runs[shape->run_count];
        IDWriteFont *mapped_font = NULL;
        UINT32 limit;
        UINT32 mapped_length = 0;
        FLOAT scale = 1.0f;
        HRESULT result;

        if (cluster_index == SIZE_MAX) {
            return -1;
        }
        cluster = &shape->clusters[cluster_index];
        limit = analysis_run_limit(shape, analysis, position);
        if (limit <= position) {
            return -1;
        }
        if (renderer->primary_font_resource != BOXRIG_FONT_RESOURCE_NONE) {
            run->font_face = renderer->primary_font_face;
            IDWriteFontFace_AddRef(run->font_face);
            mapped_length = limit - position;
        } else if (renderer->font_fallback == NULL) {
            /* Retain complete script/style runs when one font can shape them;
             * otherwise map fallback at terminal grapheme boundaries. */
            if (FAILED(create_base_system_face(
                    renderer, cluster, &run->font_face))) {
                return -1;
            }
            if (map_base_font_run(renderer, shape, analysis, position, limit,
                    &run->font_face, &mapped_length) == -1) {
                IDWriteFontFace_Release(run->font_face);
                run->font_face = NULL;
                return -1;
            }
        } else {
            result = IDWriteFontFallback_MapCharacters(
                renderer->font_fallback,
                &analysis->source_interface,
                position,
                limit - position,
                renderer->font_collection,
                renderer->primary_family,
                cluster_font_weight(cluster),
                cluster_font_style(cluster),
                DWRITE_FONT_STRETCH_NORMAL,
                &mapped_length,
                &mapped_font,
                &scale);
            if (FAILED(result) || mapped_length == 0 ||
                mapped_length > limit - position) {
                if (mapped_font != NULL) {
                    IDWriteFont_Release(mapped_font);
                }
                return -1;
            }
            if (mapped_font != NULL) {
                result = IDWriteFont_CreateFontFace(
                    mapped_font,
                    &run->font_face);
                IDWriteFont_Release(mapped_font);
                if (FAILED(result)) {
                    return -1;
                }
            } else {
                run->font_face = renderer->primary_font_face;
                IDWriteFontFace_AddRef(run->font_face);
            }
        }
        run->text_offset = position;
        run->text_length = mapped_length;
        run->bidi_level = analysis->bidi_levels[position];
        run->script = analysis->scripts[position];
        run->font_em_size = renderer->font_size * scale;
        run->font_key = boxrig_win32_text_hash_u64(
            UINT64_C(1469598103934665603),
            shape->run_count + 1);
        shape->run_count++;
        position += mapped_length;
    }
    return 0;
}

static bool run_overlaps_cluster(
    const boxrig_win32_shaped_run *run,
    const boxrig_win32_logical_cluster *cluster)
{
    UINT32 run_end = run->text_offset + run->text_length;
    UINT32 cluster_end = cluster->text_offset + cluster->text_length;

    return run->text_offset < cluster_end && cluster->text_offset < run_end;
}

static boxrig_text_direction resolved_base_direction(
    const boxrig_win32_text_analysis *analysis,
    boxrig_text_direction_policy policy)
{
    UINT8 minimum_level = UINT8_MAX;
    UINT32 index;

    if (policy == BOXRIG_TEXT_DIRECTION_FORCE_RTL) {
        return BOXRIG_TEXT_DIRECTION_RTL;
    }
    if (policy == BOXRIG_TEXT_DIRECTION_FORCE_LTR) {
        return BOXRIG_TEXT_DIRECTION_LTR;
    }
    for (index = 0; index < analysis->text_length; index++) {
        if (analysis->bidi_levels[index] < minimum_level) {
            minimum_level = analysis->bidi_levels[index];
        }
    }
    return (minimum_level & 1u) == 0 ? BOXRIG_TEXT_DIRECTION_LTR
                                     : BOXRIG_TEXT_DIRECTION_RTL;
}

static int build_portable_layout(
    boxrig_win32_row_shape *shape,
    const boxrig_win32_text_analysis *analysis,
    boxrig_text_direction_policy policy)
{
    boxrig_text_cluster_input *inputs;
    size_t column_count = 0;
    size_t cluster_index;

    if (shape->cluster_count > SIZE_MAX / sizeof(*inputs)) {
        return -1;
    }
    inputs = calloc(shape->cluster_count, sizeof(*inputs));
    if (inputs == NULL) {
        return -1;
    }
    for (cluster_index = 0;
         cluster_index < shape->cluster_count;
         cluster_index++) {
        const boxrig_win32_logical_cluster *cluster =
            &shape->clusters[cluster_index];
        uint64_t font_key = UINT64_C(1469598103934665603);
        bool found = false;
        size_t run_index;

        inputs[cluster_index].logical_column = cluster->logical_column;
        inputs[cluster_index].column_count = cluster->column_count;
        inputs[cluster_index].text_offset = cluster->text_offset;
        inputs[cluster_index].text_length = cluster->text_length;
        inputs[cluster_index].style_key = cluster->style_key;
        inputs[cluster_index].hyperlink_key = cluster->hyperlink_key;
        for (run_index = 0; run_index < shape->run_count; run_index++) {
            const boxrig_win32_shaped_run *run = &shape->runs[run_index];

            if (!run_overlaps_cluster(run, cluster)) {
                continue;
            }
            if (!found) {
                inputs[cluster_index].bidi_level = run->bidi_level;
                inputs[cluster_index].script_key = run->script.script;
                inputs[cluster_index].language_key = boxrig_win32_text_hash_append(
                    UINT64_C(1469598103934665603),
                    "en-US",
                    5);
            }
            font_key = boxrig_win32_text_hash_u64(font_key, run->font_key);
            found = true;
        }
        if (!found) {
            free(inputs);
            return -1;
        }
        inputs[cluster_index].font_key = font_key;
        column_count += cluster->column_count;
    }
    if (boxrig_text_layout_create(
            inputs,
            shape->cluster_count,
            column_count,
            resolved_base_direction(analysis, policy),
            &shape->layout) != BOXRIG_TEXT_LAYOUT_OK) {
        free(inputs);
        return -1;
    }
    free(inputs);
    return 0;
}

static int allocate_run_shaping_buffers(boxrig_win32_shaped_run *run)
{
    UINT32 maximum_glyph_count;

    if (run->text_length > UINT32_MAX - run->text_length / 2 - 16) {
        return -1;
    }
    maximum_glyph_count = run->text_length + run->text_length / 2 + 16;
    run->cluster_map = calloc(run->text_length, sizeof(*run->cluster_map));
    run->text_properties = calloc(
        run->text_length,
        sizeof(*run->text_properties));
    run->glyph_indices = calloc(
        maximum_glyph_count,
        sizeof(*run->glyph_indices));
    run->glyph_properties = calloc(
        maximum_glyph_count,
        sizeof(*run->glyph_properties));
    run->glyph_advances = calloc(
        maximum_glyph_count,
        sizeof(*run->glyph_advances));
    run->glyph_offsets = calloc(
        maximum_glyph_count,
        sizeof(*run->glyph_offsets));
    if (run->cluster_map == NULL || run->text_properties == NULL ||
        run->glyph_indices == NULL || run->glyph_properties == NULL ||
        run->glyph_advances == NULL || run->glyph_offsets == NULL) {
        return -1;
    }
    return (int)maximum_glyph_count;
}

static HRESULT get_run_glyphs(
    boxrig_win32_renderer *renderer,
    const boxrig_win32_row_shape *shape,
    boxrig_win32_shaped_run *run,
    IDWriteFontFace *font_face,
    UINT32 maximum_glyph_count)
{
    static const WCHAR locale_name[] = L"en-US";
    DWRITE_TYPOGRAPHIC_FEATURES feature_set;
    const DWRITE_TYPOGRAPHIC_FEATURES *feature_sets[1];
    UINT32 feature_range_length = run->text_length;
    UINT32 feature_range_count = 0;

    memset(
        run->cluster_map,
        0,
        run->text_length * sizeof(*run->cluster_map));
    memset(
        run->text_properties,
        0,
        run->text_length * sizeof(*run->text_properties));
    memset(
        run->glyph_indices,
        0,
        maximum_glyph_count * sizeof(*run->glyph_indices));
    memset(
        run->glyph_properties,
        0,
        maximum_glyph_count * sizeof(*run->glyph_properties));
    run->glyph_count = 0;
    if (renderer->font_feature_count != 0) {
        feature_set.features = renderer->font_features;
        feature_set.featureCount = renderer->font_feature_count;
        feature_sets[0] = &feature_set;
        feature_range_count = 1;
    }
    return IDWriteTextAnalyzer_GetGlyphs(
        renderer->text_analyzer,
        shape->text + run->text_offset,
        run->text_length,
        font_face,
        FALSE,
        (run->bidi_level & 1u) != 0,
        &run->script,
        locale_name,
        NULL,
        feature_range_count == 0 ? NULL : feature_sets,
        feature_range_count == 0 ? NULL : &feature_range_length,
        feature_range_count,
        maximum_glyph_count,
        run->cluster_map,
        run->text_properties,
        run->glyph_indices,
        run->glyph_properties,
        &run->glyph_count);
}

static bool run_has_missing_glyph(
    const boxrig_win32_shaped_run *run)
{
    UINT32 glyph_index;

    for (glyph_index = 0; glyph_index < run->glyph_count; glyph_index++) {
        if (run->glyph_indices[glyph_index] == 0 &&
            !run->glyph_properties[glyph_index].isZeroWidthSpace) {
            return true;
        }
    }
    return false;
}

static int select_shapable_system_face(
    boxrig_win32_renderer *renderer,
    boxrig_win32_row_shape *shape,
    boxrig_win32_shaped_run *run,
    UINT32 maximum_glyph_count)
{
    size_t cluster_index = cluster_index_at_text_position(
        shape,
        run->text_offset);
    DWRITE_FONT_WEIGHT weight;
    DWRITE_FONT_STYLE style;
    UINT32 family_count;
    UINT32 family_index;
    size_t registered_index;

    if (cluster_index == SIZE_MAX) {
        return -1;
    }
    weight = cluster_font_weight(&shape->clusters[cluster_index]);
    style = cluster_font_style(&shape->clusters[cluster_index]);
    for (registered_index = 0;
         registered_index < renderer->registered_font_count;
         registered_index++) {
        IDWriteFontFace *font_face =
            renderer->registered_fonts[registered_index].font_face;
        HRESULT result = get_run_glyphs(
            renderer,
            shape,
            run,
            font_face,
            maximum_glyph_count);

        if (SUCCEEDED(result) && run->glyph_count > 0 &&
            !run_has_missing_glyph(run)) {
            IDWriteFontFace_AddRef(font_face);
            IDWriteFontFace_Release(run->font_face);
            run->font_face = font_face;
            run->font_em_size = renderer->font_size;
            return 0;
        }
    }
    if (renderer->fallback_policy != BOXRIG_FONT_FALLBACK_SYSTEM) {
        if (FAILED(get_run_glyphs(
                renderer,
                shape,
                run,
                run->font_face,
                maximum_glyph_count))) {
            return -1;
        }
        return 0;
    }
    family_count = IDWriteFontCollection_GetFontFamilyCount(
        renderer->font_collection);
    for (family_index = 0; family_index < family_count; family_index++) {
        IDWriteFontFamily *family = NULL;
        IDWriteFont *font = NULL;
        IDWriteFontFace *font_face = NULL;
        HRESULT result = IDWriteFontCollection_GetFontFamily(
            renderer->font_collection,
            family_index,
            &family);

        if (SUCCEEDED(result)) {
            result = IDWriteFontFamily_GetFirstMatchingFont(
                family,
                weight,
                DWRITE_FONT_STRETCH_NORMAL,
                style,
                &font);
        }
        if (SUCCEEDED(result)) {
            result = IDWriteFont_CreateFontFace(font, &font_face);
        }
        if (SUCCEEDED(result)) {
            result = get_run_glyphs(
                renderer,
                shape,
                run,
                font_face,
                maximum_glyph_count);
        }
        if (font != NULL) {
            IDWriteFont_Release(font);
        }
        if (family != NULL) {
            IDWriteFontFamily_Release(family);
        }
        if (SUCCEEDED(result) && run->glyph_count > 0 &&
            !run_has_missing_glyph(run)) {
            IDWriteFontFace_Release(run->font_face);
            run->font_face = font_face;
            run->font_em_size = renderer->font_size;
            return 0;
        }
        if (font_face != NULL) {
            IDWriteFontFace_Release(font_face);
        }
    }
    if (FAILED(get_run_glyphs(
            renderer,
            shape,
            run,
            run->font_face,
            maximum_glyph_count))) {
        return -1;
    }
    return 0;
}

/* The base analyzer can shape text but has no MapCharacters operation.
 * Probe an entire analysis run first to preserve ligatures and joining. Only
 * when no single font covers it do we select a font for its first grapheme
 * and extend that font through subsequent complete graphemes. */
static int map_base_font_run(
    boxrig_win32_renderer *renderer,
    boxrig_win32_row_shape *shape,
    const boxrig_win32_text_analysis *analysis,
    UINT32 position,
    UINT32 limit,
    IDWriteFontFace **face,
    UINT32 *length_out)
{
    boxrig_win32_shaped_run probe;
    UINT32 maximum_glyph_count;
    UINT32 end;
    size_t cluster_index;
    int buffer_result;
    int status = -1;

    memset(&probe, 0, sizeof(probe));
    probe.text_offset = position;
    probe.text_length = limit - position;
    probe.bidi_level = analysis->bidi_levels[position];
    probe.script = analysis->scripts[position];
    probe.font_face = *face;
    IDWriteFontFace_AddRef(probe.font_face);
    buffer_result = allocate_run_shaping_buffers(&probe);
    if (buffer_result <= 0) {
        goto cleanup;
    }
    maximum_glyph_count = (UINT32)buffer_result;
    if (FAILED(get_run_glyphs(renderer, shape, &probe, probe.font_face,
            maximum_glyph_count)) ||
        (run_has_missing_glyph(&probe) &&
         select_shapable_system_face(renderer, shape, &probe,
             maximum_glyph_count) == -1)) {
        goto cleanup;
    }
    if (run_has_missing_glyph(&probe)) {
        cluster_index = cluster_index_at_text_position(shape, position);
        if (cluster_index == SIZE_MAX) {
            goto cleanup;
        }
        end = shape->clusters[cluster_index].text_offset +
            shape->clusters[cluster_index].text_length;
        if (end > limit) {
            end = limit;
        }
        probe.text_length = end - position;
        if (FAILED(get_run_glyphs(renderer, shape, &probe, probe.font_face,
                maximum_glyph_count)) ||
            (run_has_missing_glyph(&probe) &&
             select_shapable_system_face(renderer, shape, &probe,
                 maximum_glyph_count) == -1)) {
            goto cleanup;
        }
        if (!run_has_missing_glyph(&probe)) {
            while (end < limit) {
                UINT32 previous_length = probe.text_length;

                cluster_index = cluster_index_at_text_position(shape, end);
                if (cluster_index == SIZE_MAX) {
                    goto cleanup;
                }
                end = shape->clusters[cluster_index].text_offset +
                    shape->clusters[cluster_index].text_length;
                if (end > limit) {
                    end = limit;
                }
                probe.text_length = end - position;
                if (FAILED(get_run_glyphs(renderer, shape, &probe,
                        probe.font_face, maximum_glyph_count))) {
                    goto cleanup;
                }
                if (run_has_missing_glyph(&probe)) {
                    probe.text_length = previous_length;
                    break;
                }
            }
        }
    }
    IDWriteFontFace_Release(*face);
    *face = probe.font_face;
    probe.font_face = NULL;
    *length_out = probe.text_length;
    status = 0;
cleanup:
    if (probe.font_face != NULL) {
        IDWriteFontFace_Release(probe.font_face);
    }
    free(probe.cluster_map);
    free(probe.text_properties);
    free(probe.glyph_indices);
    free(probe.glyph_properties);
    free(probe.glyph_advances);
    free(probe.glyph_offsets);
    return status;
}

static int shape_run(
    boxrig_win32_renderer *renderer,
    boxrig_win32_row_shape *shape,
    boxrig_win32_shaped_run *run)
{
    static const WCHAR locale_name[] = L"en-US";
    DWRITE_TYPOGRAPHIC_FEATURES feature_set;
    const DWRITE_TYPOGRAPHIC_FEATURES *feature_sets[1];
    UINT32 feature_range_length = run->text_length;
    UINT32 feature_range_count = 0;
    UINT32 maximum_glyph_count;
    size_t cluster_index;
    size_t logical_end = 0;
    size_t visual_begin = SIZE_MAX;
    size_t visual_end = 0;
    UINT32 glyph_index;
    int buffer_result;
    HRESULT result;

    buffer_result = allocate_run_shaping_buffers(run);
    if (buffer_result <= 0) {
        return -1;
    }
    maximum_glyph_count = (UINT32)buffer_result;
    result = get_run_glyphs(
        renderer,
        shape,
        run,
        run->font_face,
        maximum_glyph_count);
    if (FAILED(result) || run->glyph_count == 0) {
        return -1;
    }
    if (run_has_missing_glyph(run) &&
        select_shapable_system_face(
            renderer,
            shape,
            run,
            maximum_glyph_count) == -1) {
        return -1;
    }
    if (renderer->font_feature_count != 0) {
        feature_set.features = renderer->font_features;
        feature_set.featureCount = renderer->font_feature_count;
        feature_sets[0] = &feature_set;
        feature_range_count = 1;
    }
    if (renderer->measuring_mode == DWRITE_MEASURING_MODE_NATURAL) {
        result = IDWriteTextAnalyzer_GetGlyphPlacements(
            renderer->text_analyzer,
            shape->text + run->text_offset,
            run->cluster_map,
            run->text_properties,
            run->text_length,
            run->glyph_indices,
            run->glyph_properties,
            run->glyph_count,
            run->font_face,
            run->font_em_size,
            FALSE,
            (run->bidi_level & 1u) != 0,
            &run->script,
            locale_name,
            feature_range_count == 0 ? NULL : feature_sets,
            feature_range_count == 0 ? NULL : &feature_range_length,
            feature_range_count,
            run->glyph_advances,
            run->glyph_offsets);
    } else {
        result = IDWriteTextAnalyzer_GetGdiCompatibleGlyphPlacements(
            renderer->text_analyzer,
            shape->text + run->text_offset,
            run->cluster_map,
            run->text_properties,
            run->text_length,
            run->glyph_indices,
            run->glyph_properties,
            run->glyph_count,
            run->font_face,
            run->font_em_size,
            renderer->rendering_policy.dpi_y / 96.0f *
                renderer->rendering_policy.output_scale,
            NULL,
            TRUE,
            FALSE,
            (run->bidi_level & 1u) != 0,
            &run->script,
            locale_name,
            feature_range_count == 0 ? NULL : feature_sets,
            feature_range_count == 0 ? NULL : &feature_range_length,
            feature_range_count,
            run->glyph_advances,
            run->glyph_offsets);
    }
    if (FAILED(result)) {
        return -1;
    }
    for (cluster_index = 0;
         cluster_index < shape->cluster_count;
         cluster_index++) {
        const boxrig_win32_logical_cluster *cluster =
            &shape->clusters[cluster_index];
        size_t offset;

        if (!run_overlaps_cluster(run, cluster)) {
            continue;
        }
        if (run->logical_column_count == 0) {
            run->logical_column = cluster->logical_column;
        }
        logical_end = cluster->logical_column + cluster->column_count;
        for (offset = 0; offset < cluster->column_count; offset++) {
            size_t visual;

            if (boxrig_text_layout_logical_to_visual(
                    shape->layout,
                    cluster->logical_column + offset,
                    &visual) != BOXRIG_TEXT_LAYOUT_OK) {
                return -1;
            }
            if (visual < visual_begin) {
                visual_begin = visual;
            }
            if (visual + 1 > visual_end) {
                visual_end = visual + 1;
            }
        }
        run->logical_column_count = logical_end - run->logical_column;
    }
    if (run->logical_column_count == 0 || visual_begin == SIZE_MAX) {
        return -1;
    }
    run->visual_column = visual_begin;
    run->visual_column_count = visual_end - visual_begin;
    shape->statistics.glyph_count += run->glyph_count;
    for (glyph_index = 0; glyph_index < run->glyph_count; glyph_index++) {
        if (run->glyph_indices[glyph_index] == 0 &&
            !run->glyph_properties[glyph_index].isZeroWidthSpace) {
            shape->statistics.missing_glyph_count++;
        }
    }
    return 0;
}

int boxrig_win32_text_shape_collected_row(
    boxrig_win32_renderer *renderer,
    boxrig_win32_row_shape *shape,
    boxrig_text_direction_policy direction_policy)
{
    boxrig_win32_text_analysis *analysis = NULL;
    size_t run_index;
    HRESULT result;

    analysis = text_analysis_create(
        shape->text,
        shape->text_length,
        direction_policy);
    if (analysis == NULL) {
        goto failure;
    }
    result = IDWriteTextAnalyzer_AnalyzeScript(
        renderer->text_analyzer,
        &analysis->source_interface,
        0,
        shape->text_length,
        &analysis->sink_interface);
    if (SUCCEEDED(result)) {
        result = IDWriteTextAnalyzer_AnalyzeBidi(
            renderer->text_analyzer,
            &analysis->source_interface,
            0,
            shape->text_length,
            &analysis->sink_interface);
    }
    if (FAILED(result) ||
        create_font_runs(renderer, shape, analysis) == -1 ||
        build_portable_layout(
            shape,
            analysis,
            direction_policy) == -1) {
        goto failure;
    }
    for (run_index = 0; run_index < shape->run_count; run_index++) {
        if (shape_run(renderer, shape, &shape->runs[run_index]) == -1) {
            goto failure;
        }
    }
    shape->statistics.cluster_count = shape->cluster_count;
    shape->statistics.font_run_count = shape->run_count;
    analysis_release_reference(analysis);
    return 0;

failure:
    if (analysis != NULL) {
        analysis_release_reference(analysis);
    }
    boxrig_win32_text_row_shape_destroy(shape);
    return -1;
}

int boxrig_win32_text_shape_terminal_row(
    boxrig_win32_renderer *renderer,
    const boxrig_terminal *terminal,
    size_t row,
    boxrig_win32_row_shape *shape)
{
    size_t rows;
    size_t columns;
    int result;

    if (renderer->render_statistics.row_shape_count != SIZE_MAX) {
        renderer->render_statistics.row_shape_count++;
    }
    memset(shape, 0, sizeof(*shape));
    boxrig_terminal_size(terminal, &rows, &columns);
    if (row >= rows || columns == 0 ||
        collect_logical_clusters(terminal, row, columns, shape) == -1) {
        boxrig_win32_text_row_shape_destroy(shape);
        return -1;
    }
    result = boxrig_win32_text_shape_collected_row(
        renderer,
        shape,
        renderer->direction_policy);
    if (result == 0) {
        free(shape->clusters);
        shape->clusters = NULL;
    }
    return result;
}

/* The optional GPU provider borrows the same DirectWrite selection/shaping
 * stack. Only glyph masks cross this boundary; no software frame is uploaded. */
static int export_gpu_shape(boxrig_win32_renderer *renderer,
    boxrig_win32_row_shape *shape, boxrig_ogl_row_shape *out)
{
    size_t i;
    const float scale = renderer->rendering_policy.dpi_y / 96.0f *
        renderer->rendering_policy.output_scale;
    memset(out, 0, sizeof(*out));
    out->clusters = calloc(shape->cluster_count, sizeof(*out->clusters));
    out->items = calloc(shape->run_count, sizeof(*out->items));
    if (out->clusters == NULL || out->items == NULL) goto failure;
    out->cluster_count = shape->cluster_count;
    out->item_count = shape->run_count;
    out->glyph_count = shape->statistics.glyph_count;
    out->missing_glyph_count = shape->statistics.missing_glyph_count;
    for (i = 0; i < shape->cluster_count; i++) {
        const boxrig_win32_logical_cluster *source = &shape->clusters[i];
        boxrig_ogl_logical_cluster *cluster = &out->clusters[i];
        cluster->logical_column = source->logical_column;
        cluster->column_count = source->column_count;
        /* Text offsets are provider-private units (UTF-16 for DirectWrite). */
        cluster->text_offset = source->text_offset;
        cluster->text_length = source->text_length;
        cluster->attributes = source->cell.attributes;
        cluster->style_key = source->style_key;
        cluster->hyperlink_key = source->hyperlink_key;
    }
    for (i = 0; i < shape->run_count; i++) {
        boxrig_win32_shaped_run *run = &shape->runs[i];
        boxrig_ogl_shaped_item *item = &out->items[i];
        boxrig_win32_text_face *face = calloc(1, sizeof(*face));
        size_t g;
        if (face == NULL) goto failure;
        item->native_item = face;
        face->face = run->font_face;
        IDWriteFontFace_AddRef(face->face);
        /* The shared compositor and glyph masks use physical pixels. Convert
         * DirectWrite DIPs once, before cell-width fitting in the compositor. */
        face->em_size = run->font_em_size * scale;
        item->logical_column = run->logical_column;
        item->logical_column_count = run->logical_column_count;
        item->visual_column = run->visual_column;
        item->visual_column_count = run->visual_column_count;
        item->glyph_count = run->glyph_count;
        item->glyphs = calloc(run->glyph_count, sizeof(*item->glyphs));
        if (item->glyphs == NULL) goto failure;
        for (g = 0; g < run->glyph_count; g++) {
            /* DirectWrite RTL advances move left. Expose visual left-to-right
             * glyph order to the shared atlas compositor instead. */
            size_t source = (run->bidi_level & 1u) != 0 ? run->glyph_count - g - 1 : g;
            boxrig_ogl_glyph *glyph = &item->glyphs[g];
            size_t position, cluster_glyph = 0;
            bool found_cluster = false;
            glyph->glyph = run->glyph_indices[source];
            glyph->width = (int32_t)(run->glyph_advances[source] * scale * BOXRIG_OGL_GLYPH_SCALE);
            glyph->x_offset = (int32_t)(run->glyph_offsets[source].advanceOffset *
                ((run->bidi_level & 1u) != 0 ? -1 : 1) * scale * BOXRIG_OGL_GLYPH_SCALE);
            glyph->y_offset = (int32_t)(-run->glyph_offsets[source].ascenderOffset * scale * BOXRIG_OGL_GLYPH_SCALE);
            glyph->text_offset = run->text_offset;
            for (position = 0; position < run->text_length; position++) {
                if (run->cluster_map[position] <= source &&
                    (!found_cluster || run->cluster_map[position] > cluster_glyph)) {
                    /* Retain the first code unit of a shared glyph cluster. */
                    glyph->text_offset = run->text_offset + position;
                    cluster_glyph = run->cluster_map[position];
                    found_cluster = true;
                }
            }
            item->natural_width += glyph->width;
        }
    }
    out->layout = shape->layout;
    shape->layout = NULL;
    return 0;
failure:
    boxrig_win32_text_shape_destroy(out);
    return -1;
}

int boxrig_win32_text_shape(
    boxrig_win32_renderer *renderer, const boxrig_terminal *terminal,
    size_t row, boxrig_ogl_row_shape *out)
{
    boxrig_win32_row_shape shape = {0};
    size_t rows, columns;
    int result = -1;
    memset(out, 0, sizeof(*out));
    boxrig_terminal_size(terminal, &rows, &columns);
    if (row < rows && columns != 0 &&
        collect_logical_clusters(terminal, row, columns, &shape) == 0 &&
        boxrig_win32_text_shape_collected_row(renderer, &shape,
            renderer->direction_policy) == 0)
        result = export_gpu_shape(renderer, &shape, out);
    boxrig_win32_text_row_shape_destroy(&shape);
    return result;
}

int boxrig_win32_text_shape_composition(
    boxrig_win32_renderer *renderer, const boxrig_composition *composition,
    size_t first_cluster, size_t cluster_count, boxrig_text_direction direction,
    boxrig_ogl_row_shape *out)
{
    boxrig_win32_row_shape shape = {0};
    int result = -1;
    memset(out, 0, sizeof(*out));
    if (boxrig_win32_text_collect_composition_clusters(composition,
            first_cluster, cluster_count, &shape) == 0 &&
        boxrig_win32_text_shape_collected_row(renderer, &shape,
            direction == BOXRIG_TEXT_DIRECTION_RTL ? BOXRIG_TEXT_DIRECTION_FORCE_RTL
                                                  : BOXRIG_TEXT_DIRECTION_FORCE_LTR) == 0)
        result = export_gpu_shape(renderer, &shape, out);
    boxrig_win32_text_row_shape_destroy(&shape);
    return result;
}

void boxrig_win32_text_shape_destroy(boxrig_ogl_row_shape *shape)
{
    size_t i;
    if (shape == NULL) return;
    for (i = 0; i < shape->item_count; i++) {
        boxrig_win32_text_face *face = shape->items[i].native_item;
        if (face != NULL) {
            if (face->face != NULL) IDWriteFontFace_Release(face->face);
            free(face);
        }
        free(shape->items[i].glyphs);
    }
    free(shape->items);
    free(shape->clusters);
    free(shape->text);
    boxrig_text_layout_destroy(shape->layout);
    memset(shape, 0, sizeof(*shape));
}

int boxrig_win32_text_raster(
    boxrig_win32_renderer *renderer, const boxrig_ogl_shaped_item *item,
    size_t glyph_index, size_t limit, uint8_t **bytes, size_t *count,
    uint32_t *width, uint32_t *height, int32_t *bearing_x,
    int32_t *bearing_y, bool *color)
{
    const boxrig_win32_text_face *face = item->native_item;
    const boxrig_ogl_glyph *glyph = &item->glyphs[glyph_index];
    DWRITE_GLYPH_RUN run;
    DWRITE_GLYPH_OFFSET offset;
    IDWriteGlyphRunAnalysis *analysis = NULL;
    UINT16 index = (UINT16)glyph->glyph;
    FLOAT advance = glyph->width / (float)BOXRIG_OGL_GLYPH_SCALE;
    RECT bounds;
    uint8_t *mask = NULL;
    uint8_t *coverage = NULL;
    size_t pixels, i;
    int result = -1;
    DWRITE_TEXTURE_TYPE texture_type = renderer->rendering_policy.antialias ==
        BOXRIG_FONT_ANTIALIAS_NONE ? DWRITE_TEXTURE_ALIASED_1x1 : DWRITE_TEXTURE_CLEARTYPE_3x1;
    size_t channels = texture_type == DWRITE_TEXTURE_ALIASED_1x1 ? 1 : 3;
    memset(&run, 0, sizeof(run));
    offset.advanceOffset = glyph->x_offset / (float)BOXRIG_OGL_GLYPH_SCALE;
    offset.ascenderOffset = -glyph->y_offset / (float)BOXRIG_OGL_GLYPH_SCALE;
    run.fontFace = face->face;
    run.fontEmSize = face->em_size;
    run.glyphCount = 1;
    run.glyphIndices = &index;
    run.glyphAdvances = &advance;
    run.glyphOffsets = &offset;
    if (FAILED(IDWriteFactory_CreateGlyphRunAnalysis(renderer->factory, &run,
            1.0f,
            NULL, channels == 1 ? DWRITE_RENDERING_MODE_ALIASED : DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC,
            renderer->measuring_mode, 0, 0, &analysis)) ||
        FAILED(IDWriteGlyphRunAnalysis_GetAlphaTextureBounds(analysis, texture_type, &bounds)))
        goto cleanup;
    *width = (uint32_t)(bounds.right - bounds.left);
    *height = (uint32_t)(bounds.bottom - bounds.top);
    *bearing_x = bounds.left;
    *bearing_y = bounds.top;
    *color = false;
    *bytes = NULL;
    *count = 0;
    if (*width == 0 || *height == 0) { result = 0; goto cleanup; }
    if (*width > SIZE_MAX / *height) goto cleanup;
    pixels = (size_t)*width * *height;
    if (pixels > limit / (channels + 1) || pixels > UINT32_MAX / channels) goto cleanup;
    mask = malloc(pixels * channels);
    coverage = malloc(pixels);
    if (mask == NULL || coverage == NULL ||
        FAILED(IDWriteGlyphRunAnalysis_CreateAlphaTexture(analysis, texture_type,
            &bounds, mask, (UINT32)(pixels * channels)))) goto cleanup;
    /* The atlas is scalar coverage. Never blend RGB subpixel masks into an
     * arbitrary caller target, which may have alpha or a different geometry. */
    for (i = 0; i < pixels; i++) coverage[i] = channels == 1 ? mask[i] :
        (uint8_t)(((unsigned int)mask[i * 3] + mask[i * 3 + 1] + mask[i * 3 + 2] + 1) / 3);
    *bytes = coverage;
    coverage = NULL;
    *count = pixels;
    result = 0;
cleanup:
    free(mask);
    free(coverage);
    if (analysis != NULL) IDWriteGlyphRunAnalysis_Release(analysis);
    return result;
}
