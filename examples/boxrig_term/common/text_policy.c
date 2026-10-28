/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "text_policy.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

#define BOXRIG_TERM_TEXT_POLICY_EVENT_BATCH_CAPACITY ((size_t)64u)

static const uint8_t url_pattern[] =
    "[Hh][Tt][Tt][Pp][Ss]?://"
    "([A-Za-z0-9]|"
    "[A-Za-z0-9][A-Za-z0-9._~:/?#@!$&+,;=%()-]*"
    "[A-Za-z0-9_~/#@%=/-])";

struct boxrig_term_text_policy {
    boxrig_terminal *terminal;
    boxrig_ui_view *view;
    boxrig_text_recognition_result *results;
    boxrig_ui_text_annotation *annotations;
    size_t result_capacity;
    size_t annotation_capacity;
    uint64_t recognizer_id;
    /* Cached identities are compared only; borrowed hit URI pointers are never reused. */
    boxrig_ui_hit_test_result pointer_hit;
    bool pointer_cached;
    bool pointer_hand;
};

static int fail_core_status(boxrig_status status)
{
    switch (status) {
    case BOXRIG_STATUS_OUT_OF_MEMORY:
        errno = ENOMEM;
        break;
    case BOXRIG_STATUS_LIMIT_EXCEEDED:
        errno = EOVERFLOW;
        break;
    case BOXRIG_STATUS_INCOMPLETE:
    case BOXRIG_STATUS_STALE:
        errno = EAGAIN;
        break;
    case BOXRIG_STATUS_INVALID_ARGUMENT:
    default:
        errno = EINVAL;
        break;
    }
    return -1;
}

static int fail_ui_status(boxrig_ui_status status)
{
    switch (status) {
    case BOXRIG_UI_OUT_OF_MEMORY:
        errno = ENOMEM;
        break;
    case BOXRIG_UI_LIMIT_EXCEEDED:
        errno = EOVERFLOW;
        break;
    case BOXRIG_UI_BUSY:
    case BOXRIG_UI_STALE:
        errno = EAGAIN;
        break;
    case BOXRIG_UI_UNAVAILABLE:
        errno = ENOTSUP;
        break;
    case BOXRIG_UI_INVALID_ARGUMENT:
    default:
        errno = EINVAL;
        break;
    }
    return -1;
}

static int register_url_recognizer(boxrig_term_text_policy *policy)
{
    boxrig_text_recognizer_options options;
    boxrig_text_regex_diagnostic diagnostic;
    boxrig_status status;

    memset(&options, 0, sizeof(options));
    options.scope = BOXRIG_TEXT_RECOGNIZER_SCOPE_BOTH;
    options.pattern = url_pattern;
    options.pattern_length = sizeof(url_pattern) - 1u;
    options.limits = boxrig_text_recognizer_limits_default();
    status = boxrig_terminal_register_text_recognizer(
        policy->terminal,
        &options,
        &diagnostic,
        &policy->recognizer_id);
    return status == BOXRIG_STATUS_OK ? 0 : fail_core_status(status);
}

static int ensure_result_capacity(
    boxrig_term_text_policy *policy,
    size_t required)
{
    boxrig_text_recognition_result *replacement;

    if (required <= policy->result_capacity) {
        return 0;
    }
    if (required > SIZE_MAX / sizeof(*replacement)) {
        errno = EOVERFLOW;
        return -1;
    }
    replacement = realloc(policy->results, required * sizeof(*replacement));
    if (replacement == NULL) {
        errno = ENOMEM;
        return -1;
    }
    policy->results = replacement;
    policy->result_capacity = required;
    return 0;
}

static int ensure_annotation_capacity(
    boxrig_term_text_policy *policy,
    size_t required)
{
    boxrig_ui_text_annotation *replacement;

    if (required <= policy->annotation_capacity) {
        return 0;
    }
    if (required > SIZE_MAX / sizeof(*replacement)) {
        errno = EOVERFLOW;
        return -1;
    }
    replacement = realloc(
        policy->annotations,
        required * sizeof(*replacement));
    if (replacement == NULL) {
        errno = ENOMEM;
        return -1;
    }
    policy->annotations = replacement;
    policy->annotation_capacity = required;
    return 0;
}

static int apply_line_replacement(
    boxrig_term_text_policy *policy,
    const boxrig_text_recognizer_event *event)
{
    boxrig_status core_status;
    boxrig_ui_status ui_status;
    size_t required = 0u;
    size_t index;

    if (ensure_result_capacity(policy, event->result_count) == -1 ||
        ensure_annotation_capacity(policy, event->result_count) == -1) {
        return -1;
    }
    core_status = boxrig_terminal_query_text_recognizer_line_results(
        policy->terminal,
        policy->recognizer_id,
        event->screen,
        event->line_id,
        event->line_revision,
        event->result_set_revision,
        policy->results,
        policy->result_capacity,
        &required);
    if (core_status != BOXRIG_STATUS_OK) {
        return fail_core_status(core_status);
    }
    if (required != event->result_count) {
        errno = EPROTO;
        return -1;
    }
    for (index = 0u; index < required; index++) {
        boxrig_ui_text_annotation *annotation = &policy->annotations[index];

        memset(annotation, 0, sizeof(*annotation));
        annotation->source_id = BOXRIG_TERM_URL_ANNOTATION_SOURCE_ID;
        annotation->item_id = policy->results[index].match_id;
        annotation->range = policy->results[index].range;
        annotation->style.field_mask =
            BOXRIG_UI_ANNOTATION_UNDERLINE_STYLE;
        annotation->style.underline_style = BOXRIG_UNDERLINE_SINGLE;
    }
    ui_status = boxrig_ui_view_replace_text_annotations(
        policy->view,
        BOXRIG_TERM_URL_ANNOTATION_SOURCE_ID,
        event->screen,
        event->line_id,
        event->line_revision,
        policy->annotations,
        required);
    return ui_status == BOXRIG_UI_OK ? 0 : fail_ui_status(ui_status);
}

static int process_events(
    boxrig_term_text_policy *policy,
    bool *more_work_out)
{
    size_t processed = 0u;

    while (processed < BOXRIG_TERM_TEXT_POLICY_EVENT_BATCH_CAPACITY) {
        boxrig_text_recognizer_event event;

        if (!boxrig_terminal_text_recognizer_event_peek(
                policy->terminal,
                &event)) {
            return 0;
        }
        if (event.recognizer_id != policy->recognizer_id) {
            errno = EPROTO;
            return -1;
        }
        if (event.kind == BOXRIG_TEXT_RECOGNIZER_EVENT_RESCAN_REQUIRED) {
            boxrig_ui_status ui_status =
                boxrig_ui_view_clear_text_annotations(
                    policy->view,
                    BOXRIG_TERM_URL_ANNOTATION_SOURCE_ID);
            boxrig_status core_status;

            if (ui_status != BOXRIG_UI_OK) {
                return fail_ui_status(ui_status);
            }
            core_status = boxrig_terminal_rescan_text_recognizer(
                policy->terminal,
                policy->recognizer_id);
            if (core_status != BOXRIG_STATUS_OK) {
                return fail_core_status(core_status);
            }
            *more_work_out = true;
            return 0;
        }
        if (event.kind == BOXRIG_TEXT_RECOGNIZER_EVENT_LINE_EVICTED) {
            boxrig_ui_status ui_status = boxrig_ui_view_clear_text_annotation_line(
                policy->view, BOXRIG_TERM_URL_ANNOTATION_SOURCE_ID,
                event.screen, event.line_id);
            if (ui_status != BOXRIG_UI_OK) {
                return fail_ui_status(ui_status);
            }
        } else if (event.kind == BOXRIG_TEXT_RECOGNIZER_EVENT_SCOPE_INCOMPLETE) {
            boxrig_ui_status ui_status =
                boxrig_ui_view_clear_text_annotations(
                    policy->view,
                    BOXRIG_TERM_URL_ANNOTATION_SOURCE_ID);

            if (ui_status != BOXRIG_UI_OK) {
                return fail_ui_status(ui_status);
            }
        } else if (event.kind ==
                   BOXRIG_TEXT_RECOGNIZER_EVENT_LINE_REPLACED) {
            if (apply_line_replacement(policy, &event) == -1) {
                return -1;
            }
        } else {
            errno = EPROTO;
            return -1;
        }
        if (boxrig_terminal_text_recognizer_event_consume(
                policy->terminal) != BOXRIG_STATUS_OK) {
            errno = EPROTO;
            return -1;
        }
        processed++;
    }
    *more_work_out = boxrig_terminal_text_recognizer_event_peek(
        policy->terminal,
        NULL);
    return 0;
}

/* Read the setting once per policy. Only a single ASCII digit is accepted;
 * Windows therefore needs no allocation for an arbitrarily long value. */
static int read_auto_urls(bool *enabled_out)
{
    const char *value;
    bool valid;
#ifdef _WIN32
    char buffer[2];
    DWORD length;

    SetLastError(ERROR_SUCCESS);
    length = GetEnvironmentVariableA(
        "BOXRIG_AUTO_URLS", buffer, (DWORD)sizeof(buffer));
    if (length == 0u && GetLastError() == ERROR_ENVVAR_NOT_FOUND) {
        value = NULL;
    } else if (length != 1u) {
        errno = EINVAL;
        return -1;
    } else {
        value = buffer;
    }
#else
    value = getenv("BOXRIG_AUTO_URLS");
#endif
    valid = value == NULL || strcmp(value, "0") == 0 ||
        strcmp(value, "1") == 0;
    *enabled_out = value == NULL || strcmp(value, "1") == 0;
    if (!valid) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

int boxrig_term_text_policy_create(
    boxrig_terminal *terminal,
    boxrig_ui_view *view,
    boxrig_term_text_policy **policy_out)
{
    boxrig_term_text_policy *policy;
    bool auto_urls;

    if (terminal == NULL || view == NULL || policy_out == NULL ||
        boxrig_ui_view_terminal(view) != terminal) {
        errno = EINVAL;
        return -1;
    }
    *policy_out = NULL;
    if (read_auto_urls(&auto_urls) == -1) {
        return -1;
    }
    policy = calloc(1u, sizeof(*policy));
    if (policy == NULL) {
        errno = ENOMEM;
        return -1;
    }
    policy->terminal = terminal;
    policy->view = view;
    if (auto_urls && register_url_recognizer(policy) == -1) {
        free(policy);
        return -1;
    }
    *policy_out = policy;
    return 0;
}

void boxrig_term_text_policy_destroy(boxrig_term_text_policy *policy)
{
    if (policy == NULL) {
        return;
    }
    if (policy->view != NULL) {
        (void)boxrig_ui_view_clear_text_annotations(
            policy->view,
            BOXRIG_TERM_URL_ANNOTATION_SOURCE_ID);
    }
    if (policy->terminal != NULL && policy->recognizer_id != 0u) {
        (void)boxrig_terminal_unregister_text_recognizer(
            policy->terminal,
            policy->recognizer_id);
    }
    free(policy->results);
    free(policy->annotations);
    free(policy);
}

int boxrig_term_text_policy_resume(
    boxrig_term_text_policy *policy,
    size_t work_budget,
    size_t *work_used_out,
    bool *more_work_out)
{
    boxrig_status status;
    bool core_more_work = false;
    bool event_more_work = false;

    if (policy == NULL || work_budget == 0u || work_used_out == NULL ||
        more_work_out == NULL) {
        errno = EINVAL;
        return -1;
    }
    *work_used_out = 0u;
    *more_work_out = false;
    if (policy->recognizer_id == 0u) {
        return 0;
    }
    status = boxrig_terminal_resume_text_recognizers(
        policy->terminal,
        work_budget,
        work_used_out,
        &core_more_work);
    if (status != BOXRIG_STATUS_OK) {
        return fail_core_status(status);
    }
    if (process_events(policy, &event_more_work) == -1) {
        return -1;
    }
    *more_work_out = core_more_work || event_more_work;
    return 0;
}

static int classify_pointer(
    boxrig_term_text_policy *policy,
    const boxrig_ui_hit_test_result *hit,
    boxrig_term_text_pointer_target *target_out, bool update_hover)
{
    boxrig_ui_status status;
    size_t required = 0u;
    size_t index;

    if (policy == NULL || hit == NULL || target_out == NULL) {
        errno = EINVAL;
        return -1;
    }
    *target_out = BOXRIG_TERM_TEXT_POINTER_NONE;
    if (!hit->inside_grid || !hit->text_position_valid) {
        status = update_hover ? boxrig_ui_view_clear_hyperlink_hover(policy->view) : BOXRIG_UI_OK;
        return status == BOXRIG_UI_OK ? 0 : fail_ui_status(status);
    }
    status = boxrig_ui_view_query_text_annotation_hits(
        policy->view,
        hit,
        BOXRIG_UI_ANNOTATION_HIT_ALL,
        NULL,
        0u,
        &required,
        NULL);
    /* PTY output may replace the hit's text before queued motion is routed.
     * Discard obsolete hover information without stopping terminal input. */
    if (status == BOXRIG_UI_STALE) {
        status = update_hover ? boxrig_ui_view_clear_hyperlink_hover(policy->view) : BOXRIG_UI_OK;
        return status == BOXRIG_UI_OK ? 0 : fail_ui_status(status);
    }
    if (status != BOXRIG_UI_OK && status != BOXRIG_UI_LIMIT_EXCEEDED) {
        return fail_ui_status(status);
    }
    if (hit->hyperlink_handle != 0u && hit->hyperlink_uri != NULL) {
        /* Unsupported OSC 8 metadata still overrides an inferred visible URL.
         * Reject it before changing hover, avoiding transient add/clear dirty
         * on every queued motion or presentation refresh. */
        if (!boxrig_term_text_policy_validate_uri(
                policy, hit->hyperlink_uri, hit->hyperlink_uri_length)) {
            status = update_hover ? boxrig_ui_view_clear_hyperlink_hover(policy->view) : BOXRIG_UI_OK;
            return status == BOXRIG_UI_OK ? 0 : fail_ui_status(status);
        }
        status = update_hover ? boxrig_ui_view_update_hyperlink_hover(policy->view, hit) : BOXRIG_UI_OK;
        if (status != BOXRIG_UI_OK) {
            return fail_ui_status(status);
        }
        *target_out = BOXRIG_TERM_TEXT_POINTER_OSC8;
        return 0;
    }
    status = update_hover ? boxrig_ui_view_clear_hyperlink_hover(policy->view) : BOXRIG_UI_OK;
    if (status != BOXRIG_UI_OK) {
        return fail_ui_status(status);
    }
    if (ensure_annotation_capacity(policy, required) == -1) {
        return -1;
    }
    if (required != 0u) {
        status = boxrig_ui_view_query_text_annotation_hits(
            policy->view,
            hit,
            BOXRIG_UI_ANNOTATION_HIT_ALL,
            policy->annotations,
            policy->annotation_capacity,
            &required,
            NULL);
        if (status != BOXRIG_UI_OK) {
            return fail_ui_status(status);
        }
    }
    for (index = required; index != 0u; index--) {
        if (policy->annotations[index - 1u].source_id ==
            BOXRIG_TERM_URL_ANNOTATION_SOURCE_ID) {
            *target_out = BOXRIG_TERM_TEXT_POINTER_URL;
            return 0;
        }
    }
    if (required != 0u) {
        *target_out = BOXRIG_TERM_TEXT_POINTER_ANNOTATION;
    }
    return 0;
}

int boxrig_term_text_policy_update_pointer_hover(
    boxrig_term_text_policy *policy, const boxrig_ui_hit_test_result *hit,
    boxrig_term_text_pointer_target *target_out)
{
    return classify_pointer(policy, hit, target_out, true);
}

uint64_t boxrig_term_text_policy_recognizer_id(
    const boxrig_term_text_policy *policy)
{
    return policy == NULL ? 0u : policy->recognizer_id;
}

static unsigned char ascii_lower(unsigned char value)
{
    return value >= 'A' && value <= 'Z' ? (unsigned char)(value + ('a' - 'A')) : value;
}

bool boxrig_term_text_policy_validate_uri(
    const boxrig_term_text_policy *policy, const char *uri, size_t length)
{
    if (policy == NULL || uri == NULL || length > BOXRIG_TERM_LINK_URI_CAPACITY)
        return false;
    size_t scheme_length = 0;
    static const char *schemes[] = {"http://", "https://"};
    for (size_t i = 0; i < sizeof(schemes) / sizeof(schemes[0]); i++) {
        size_t n = strlen(schemes[i]);
        if (length <= n) continue;
        size_t j = 0;
        while (j < n && ascii_lower((unsigned char)uri[j]) == (unsigned char)schemes[i][j]) j++;
        if (j == n) { scheme_length = n; break; }
    }
    if (scheme_length == 0 || uri[scheme_length] == '/' ||
        uri[scheme_length] == '?' || uri[scheme_length] == '#') return false;
    const unsigned char *bytes = (const unsigned char *)uri;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = bytes[i];
        if (c <= 0x20 || c == 0x7f ||
            (c == 0xc2 && i + 1 < length && bytes[i + 1] >= 0x80 && bytes[i + 1] <= 0xa0))
            return false;
        /* Other Unicode White_Space scalars are never part of a launch URI. */
        if (i + 2 < length &&
            ((c == 0xe1 && bytes[i + 1] == 0x9a && bytes[i + 2] == 0x80) ||
             (c == 0xe2 && bytes[i + 1] == 0x80 &&
              ((bytes[i + 2] >= 0x80 && bytes[i + 2] <= 0x8a) ||
               bytes[i + 2] == 0xa8 || bytes[i + 2] == 0xa9 || bytes[i + 2] == 0xaf)) ||
             (c == 0xe2 && bytes[i + 1] == 0x81 && bytes[i + 2] == 0x9f) ||
             (c == 0xe3 && bytes[i + 1] == 0x80 && bytes[i + 2] == 0x80))) return false;
    }
    size_t offset = 0;
    while (offset < length) {
        boxrig_text_cluster_measurement measurement;
        if (boxrig_terminal_measure_text_cluster(policy->terminal,
                bytes + offset, length - offset, &measurement) != BOXRIG_STATUS_OK)
            return false;
        offset += measurement.text_length;
    }
    return true;
}

int boxrig_term_text_policy_query_link(
    boxrig_term_text_policy *policy, const boxrig_ui_hit_test_result *hit,
    boxrig_term_link_target *target_out)
{
    if (target_out == NULL) { errno = EINVAL; return -1; }
    memset(target_out, 0, sizeof(*target_out));
    if (policy == NULL || hit == NULL) { errno = EINVAL; return -1; }
    boxrig_term_text_pointer_target kind;
    if (classify_pointer(policy, hit, &kind, false) == -1) return -1;
    if (kind != BOXRIG_TERM_TEXT_POINTER_OSC8 && kind != BOXRIG_TERM_TEXT_POINTER_URL) return 0;
    boxrig_term_link_target target = {0};
    target.kind = kind;
    const char *uri = hit->hyperlink_uri;
    size_t length = hit->hyperlink_uri_length;
    boxrig_text_snapshot *snapshot = NULL;
    if (kind == BOXRIG_TERM_TEXT_POINTER_OSC8) {
        target.hyperlink_handle = hit->hyperlink_handle;
        target.range.start = hit->leading_position;
        target.range.end = hit->trailing_position;
    } else {
        size_t count;
        boxrig_ui_status status = boxrig_ui_view_query_text_annotation_hits(
            policy->view, hit, BOXRIG_UI_ANNOTATION_HIT_ALL, policy->annotations,
            policy->annotation_capacity, &count, NULL);
        if (status == BOXRIG_UI_STALE) return 0;
        if (status != BOXRIG_UI_OK) return fail_ui_status(status);
        const boxrig_ui_text_annotation *annotation = NULL;
        for (size_t i = count; i > 0; i--) {
            if (policy->annotations[i - 1].source_id == BOXRIG_TERM_URL_ANNOTATION_SOURCE_ID) {
                annotation = &policy->annotations[i - 1]; break;
            }
        }
        if (annotation == NULL) return 0;
        target.source_id = annotation->source_id;
        target.item_id = annotation->item_id;
        target.range = annotation->range;
        boxrig_text_snapshot_options options = {
            .screen = target.range.start.screen,
            .range = &target.range,
            .byte_capacity = BOXRIG_TERM_LINK_URI_CAPACITY,
            .line_capacity = 1
        };
        boxrig_status core_status = boxrig_terminal_create_text_snapshot(policy->terminal, &options, &snapshot);
        if (core_status == BOXRIG_STATUS_STALE || core_status == BOXRIG_STATUS_LIMIT_EXCEEDED) return 0;
        if (core_status != BOXRIG_STATUS_OK) return fail_core_status(core_status);
        uri = (const char *)boxrig_text_snapshot_data(snapshot, &length);
    }
    if (!boxrig_term_text_policy_validate_uri(policy, uri, length)) {
        boxrig_text_snapshot_destroy(snapshot);
        return 0;
    }
    target.uri = malloc(length + 1);
    if (target.uri == NULL) {
        boxrig_text_snapshot_destroy(snapshot);
        errno = ENOMEM;
        return -1;
    }
    memcpy(target.uri, uri, length);
    target.uri[length] = '\0';
    target.uri_length = length;
    boxrig_text_snapshot_destroy(snapshot);
    *target_out = target;
    return 0;
}

void boxrig_term_link_target_clear(boxrig_term_link_target *target)
{
    if (target != NULL) { free(target->uri); memset(target, 0, sizeof(*target)); }
}

static bool same_pointer_cell(
    const boxrig_ui_hit_test_result *a, const boxrig_ui_hit_test_result *b)
{
    return a->inside_grid == b->inside_grid &&
        a->text_position_valid == b->text_position_valid &&
        a->layout_generation == b->layout_generation &&
        a->annotation_generation == b->annotation_generation &&
        a->row == b->row && a->logical_column == b->logical_column &&
        a->hyperlink_handle == b->hyperlink_handle &&
        a->leading_position.screen == b->leading_position.screen &&
        a->leading_position.line_id.epoch == b->leading_position.line_id.epoch &&
        a->leading_position.line_id.serial == b->leading_position.line_id.serial &&
        a->leading_position.line_revision == b->leading_position.line_revision &&
        a->leading_position.text_revision == b->leading_position.text_revision;
}

int boxrig_term_text_policy_refresh_pointer(
    boxrig_term_text_policy *policy, bool inside,
    boxrig_ui_fixed x, boxrig_ui_fixed y, bool *hand_out)
{
    if (policy == NULL || hand_out == NULL) { errno = EINVAL; return -1; }
    *hand_out = false;
    if (!inside) {
        bool changed = policy->pointer_cached;
        policy->pointer_cached = false;
        boxrig_ui_status status = boxrig_ui_view_clear_hyperlink_hover(policy->view);
        return status == BOXRIG_UI_OK ? (changed ? 1 : 0) : fail_ui_status(status);
    }
    boxrig_ui_hit_test_result hit;
    boxrig_ui_status status = boxrig_ui_view_hit_test(policy->view, x, y, &hit);
    if (status != BOXRIG_UI_OK) return fail_ui_status(status);
    if (policy->pointer_cached && same_pointer_cell(&policy->pointer_hit, &hit)) {
        *hand_out = policy->pointer_hand;
        if (*hand_out && hit.hyperlink_handle != 0) {
            status = boxrig_ui_view_update_hyperlink_hover(policy->view, &hit);
            if (status != BOXRIG_UI_OK) return fail_ui_status(status);
        }
        return 0;
    }
    boxrig_term_link_target target;
    if (boxrig_term_text_policy_query_link(policy, &hit, &target) == -1) return -1;
    *hand_out = target.kind != BOXRIG_TERM_TEXT_POINTER_NONE;
    status = target.kind == BOXRIG_TERM_TEXT_POINTER_OSC8
        ? boxrig_ui_view_update_hyperlink_hover(policy->view, &hit)
        : boxrig_ui_view_clear_hyperlink_hover(policy->view);
    boxrig_term_link_target_clear(&target);
    if (status != BOXRIG_UI_OK) return fail_ui_status(status);
    policy->pointer_hit = hit;
    policy->pointer_hand = *hand_out;
    policy->pointer_cached = true;
    return 1;
}
