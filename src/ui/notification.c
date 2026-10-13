/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "boxrig/ui/notification.h"

#include <stdlib.h>
#include <string.h>

enum {
    NOTIFICATION_IDENTIFIER_CAPACITY = 128,
    NOTIFICATION_TYPE_CAPACITY = 16
};

typedef struct notification_record {
    bool active;
    uint8_t identifier[NOTIFICATION_IDENTIFIER_CAPACITY];
    size_t identifier_length;
    uint8_t *content;
    size_t title_offset;
    size_t title_length;
    size_t body_offset;
    size_t body_length;
    size_t application_name_offset;
    size_t application_name_length;
    size_t type_offsets[NOTIFICATION_TYPE_CAPACITY];
    size_t type_lengths[NOTIFICATION_TYPE_CAPACITY];
    size_t type_count;
    boxrig_notification_occasion occasion;
    boxrig_notification_urgency urgency;
    uint64_t success_milliseconds;
    uint64_t sequence;
} notification_record;

struct boxrig_notification_policy_state {
    boxrig_notification_policy_config config;
    notification_record *records;
    size_t tokens;
    uint64_t last_refill_milliseconds;
    uint64_t next_sequence;
};

static bool text_view_valid(boxrig_notification_text_view text)
{
    return text.length == 0 || text.bytes != NULL;
}

static bool notification_valid(const boxrig_notification_view *notification)
{
    size_t index;

    if (notification == NULL ||
        notification->identifier.length >
            NOTIFICATION_IDENTIFIER_CAPACITY ||
        notification->type_count > NOTIFICATION_TYPE_CAPACITY ||
        (notification->type_count != 0 && notification->types == NULL) ||
        !text_view_valid(notification->identifier) ||
        !text_view_valid(notification->title) ||
        !text_view_valid(notification->body) ||
        !text_view_valid(notification->application_name) ||
        notification->occasion < BOXRIG_NOTIFICATION_ALWAYS ||
        notification->occasion > BOXRIG_NOTIFICATION_INVISIBLE ||
        notification->urgency < BOXRIG_NOTIFICATION_URGENCY_LOW ||
        notification->urgency > BOXRIG_NOTIFICATION_URGENCY_CRITICAL) {
        return false;
    }
    for (index = 0; index < notification->type_count; index++) {
        if (!text_view_valid(notification->types[index])) {
            return false;
        }
    }
    return true;
}

static bool byte_string_equal(
    const uint8_t *left,
    size_t left_length,
    const uint8_t *right,
    size_t right_length)
{
    return left_length == right_length &&
           (left_length == 0 || memcmp(left, right, left_length) == 0);
}

static bool identifier_equal(
    const notification_record *record,
    const boxrig_notification_view *notification)
{
    return notification->has_identifier &&
           byte_string_equal(
               record->identifier,
               record->identifier_length,
               notification->identifier.bytes,
               notification->identifier.length);
}

static bool record_content_equal(
    const notification_record *record,
    const boxrig_notification_view *notification)
{
    size_t index;

    if (record->type_count != notification->type_count ||
        record->occasion != notification->occasion ||
        record->urgency != notification->urgency ||
        !byte_string_equal(
            record->title_length == 0
                ? NULL
                : record->content + record->title_offset,
            record->title_length,
            notification->title.bytes,
            notification->title.length) ||
        !byte_string_equal(
            record->body_length == 0
                ? NULL
                : record->content + record->body_offset,
            record->body_length,
            notification->body.bytes,
            notification->body.length) ||
        !byte_string_equal(
            record->application_name_length == 0
                ? NULL
                : record->content + record->application_name_offset,
            record->application_name_length,
            notification->application_name.bytes,
            notification->application_name.length)) {
        return false;
    }
    for (index = 0; index < record->type_count; index++) {
        if (!byte_string_equal(
                record->type_lengths[index] == 0
                    ? NULL
                    : record->content + record->type_offsets[index],
                record->type_lengths[index],
                notification->types[index].bytes,
                notification->types[index].length)) {
            return false;
        }
    }
    return true;
}

static notification_record *find_record(
    boxrig_notification_policy_state *state,
    const boxrig_notification_view *notification)
{
    size_t index;

    if (!notification->has_identifier) {
        return NULL;
    }
    for (index = 0; index < state->config.tracking_capacity; index++) {
        notification_record *record = &state->records[index];

        if (record->active && identifier_equal(record, notification)) {
            return record;
        }
    }
    return NULL;
}

static void refill_tokens(
    boxrig_notification_policy_state *state,
    uint64_t now)
{
    uint64_t elapsed;
    uint64_t intervals;
    size_t added;

    if (now < state->last_refill_milliseconds) {
        return;
    }
    if (state->tokens == state->config.token_capacity) {
        state->last_refill_milliseconds = now;
        return;
    }
    elapsed = now - state->last_refill_milliseconds;
    intervals = elapsed / state->config.token_refill_milliseconds;
    if (intervals == 0) {
        return;
    }
    added = intervals > SIZE_MAX ? SIZE_MAX : (size_t)intervals;
    if (added > state->config.token_capacity - state->tokens) {
        added = state->config.token_capacity - state->tokens;
    }
    state->tokens += added;
    state->last_refill_milliseconds +=
        intervals * state->config.token_refill_milliseconds;
}

static bool occasion_matches(
    boxrig_notification_occasion occasion,
    bool window_focused,
    bool window_visible)
{
    if (occasion == BOXRIG_NOTIFICATION_UNFOCUSED) {
        return !window_focused;
    }
    if (occasion == BOXRIG_NOTIFICATION_INVISIBLE) {
        return !window_focused && !window_visible;
    }
    return true;
}

boxrig_notification_policy_config boxrig_notification_policy_config_default(
    boxrig_notification_policy_clock clock,
    void *clock_context)
{
    boxrig_notification_policy_config config;

    config.allow_notifications = false;
    config.token_capacity = 5;
    config.token_refill_milliseconds = 2000;
    config.duplicate_milliseconds = 1000;
    config.tracking_capacity = 64;
    config.clock = clock;
    config.clock_context = clock_context;
    return config;
}

int boxrig_notification_policy_create(
    const boxrig_notification_policy_config *config,
    boxrig_notification_policy_state **state_out)
{
    boxrig_notification_policy_state *state;

    if (config == NULL || state_out == NULL || config->clock == NULL ||
        config->token_capacity == 0 ||
        config->token_refill_milliseconds == 0 ||
        config->tracking_capacity == 0 ||
        config->tracking_capacity >
            SIZE_MAX / sizeof(*state->records)) {
        return -1;
    }
    *state_out = NULL;
    state = calloc(1, sizeof(*state));
    if (state == NULL) {
        return -1;
    }
    state->records = calloc(
        config->tracking_capacity,
        sizeof(*state->records));
    if (state->records == NULL) {
        free(state);
        return -1;
    }
    state->config = *config;
    state->tokens = config->token_capacity;
    state->last_refill_milliseconds = config->clock(config->clock_context);
    state->next_sequence = 1;
    *state_out = state;
    return 0;
}

boxrig_notification_policy_evaluation boxrig_notification_policy_evaluate(
    boxrig_notification_policy_state *state,
    const boxrig_notification_view *notification,
    bool window_focused,
    bool window_visible,
    bool provider_available)
{
    boxrig_notification_policy_evaluation evaluation = {
        BOXRIG_NOTIFICATION_POLICY_DENIED,
        0,
    };
    notification_record *record;
    uint64_t elapsed;

    if (state == NULL || !notification_valid(notification) ||
        !state->config.allow_notifications ||
        !occasion_matches(
            notification->occasion,
            window_focused,
            window_visible)) {
        return evaluation;
    }
    evaluation.timestamp_milliseconds =
        state->config.clock(state->config.clock_context);
    refill_tokens(state, evaluation.timestamp_milliseconds);
    record = find_record(state, notification);
    if (record != NULL && record_content_equal(record, notification)) {
        elapsed = evaluation.timestamp_milliseconds >=
                          record->success_milliseconds
                      ? evaluation.timestamp_milliseconds -
                            record->success_milliseconds
                      : 0;
        if (elapsed <= state->config.duplicate_milliseconds) {
            evaluation.decision = BOXRIG_NOTIFICATION_POLICY_DUPLICATE;
            return evaluation;
        }
    }
    if (state->tokens == 0) {
        return evaluation;
    }
    if (!provider_available) {
        evaluation.decision = BOXRIG_NOTIFICATION_POLICY_UNAVAILABLE;
        return evaluation;
    }
    state->tokens--;
    evaluation.decision = BOXRIG_NOTIFICATION_POLICY_DISPLAY;
    return evaluation;
}

static bool add_length(size_t *total, size_t length)
{
    if (length > SIZE_MAX - *total) {
        return false;
    }
    *total += length;
    return true;
}

static void copy_text(
    uint8_t *content,
    size_t *offset,
    boxrig_notification_text_view text,
    size_t *field_offset,
    size_t *field_length)
{
    *field_offset = *offset;
    *field_length = text.length;
    if (text.length != 0) {
        memcpy(content + *offset, text.bytes, text.length);
        *offset += text.length;
    }
}

int boxrig_notification_policy_record_success(
    boxrig_notification_policy_state *state,
    const boxrig_notification_view *notification,
    uint64_t timestamp_milliseconds)
{
    notification_record replacement;
    notification_record *destination = NULL;
    size_t total = 0;
    size_t offset = 0;
    size_t oldest_index = 0;
    size_t index;

    if (state == NULL || !notification_valid(notification)) {
        return -1;
    }
    if (!notification->has_identifier) {
        return 0;
    }
    if (!add_length(&total, notification->title.length) ||
        !add_length(&total, notification->body.length) ||
        !add_length(&total, notification->application_name.length)) {
        return -1;
    }
    for (index = 0; index < notification->type_count; index++) {
        if (!add_length(&total, notification->types[index].length)) {
            return -1;
        }
    }
    memset(&replacement, 0, sizeof(replacement));
    replacement.content = total == 0 ? NULL : malloc(total);
    if (total != 0 && replacement.content == NULL) {
        return -1;
    }
    replacement.active = true;
    memcpy(
        replacement.identifier,
        notification->identifier.bytes,
        notification->identifier.length);
    replacement.identifier_length = notification->identifier.length;
    copy_text(
        replacement.content,
        &offset,
        notification->title,
        &replacement.title_offset,
        &replacement.title_length);
    copy_text(
        replacement.content,
        &offset,
        notification->body,
        &replacement.body_offset,
        &replacement.body_length);
    copy_text(
        replacement.content,
        &offset,
        notification->application_name,
        &replacement.application_name_offset,
        &replacement.application_name_length);
    for (index = 0; index < notification->type_count; index++) {
        copy_text(
            replacement.content,
            &offset,
            notification->types[index],
            &replacement.type_offsets[index],
            &replacement.type_lengths[index]);
    }
    replacement.type_count = notification->type_count;
    replacement.occasion = notification->occasion;
    replacement.urgency = notification->urgency;
    replacement.success_milliseconds = timestamp_milliseconds;
    replacement.sequence = state->next_sequence;
    state->next_sequence++;

    for (index = 0; index < state->config.tracking_capacity; index++) {
        notification_record *record = &state->records[index];

        if (record->active && identifier_equal(record, notification)) {
            destination = record;
            break;
        }
        if (!record->active && destination == NULL) {
            destination = record;
        }
        if (state->records[index].sequence <
            state->records[oldest_index].sequence) {
            oldest_index = index;
        }
    }
    if (destination == NULL) {
        destination = &state->records[oldest_index];
    }
    free(destination->content);
    *destination = replacement;
    return 0;
}

void boxrig_notification_policy_destroy(
    boxrig_notification_policy_state *state)
{
    size_t index;

    if (state == NULL) {
        return;
    }
    for (index = 0; index < state->config.tracking_capacity; index++) {
        free(state->records[index].content);
    }
    free(state->records);
    free(state);
}
