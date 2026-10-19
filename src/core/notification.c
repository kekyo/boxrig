/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "terminal_internal.h"

#include "utf8proc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    BOXRIG_NOTIFICATION_IDENTIFIER_CAPACITY = 128,
    BOXRIG_NOTIFICATION_TYPE_CAPACITY = 16,
    BOXRIG_NOTIFICATION_PLAIN_FRAGMENT_CAPACITY = 2048,
    BOXRIG_NOTIFICATION_ENCODED_FRAGMENT_CAPACITY = 4096
};

typedef struct boxrig_notification_bytes {
    uint8_t *bytes;
    size_t length;
    size_t capacity;
    uint8_t base64_quartet[4];
    size_t base64_quartet_length;
} boxrig_notification_bytes;

typedef struct boxrig_notification_transaction {
    bool active;
    uint8_t identifier[BOXRIG_NOTIFICATION_IDENTIFIER_CAPACITY];
    size_t identifier_length;
    boxrig_notification_bytes title;
    boxrig_notification_bytes body;
    boxrig_notification_bytes application_name;
    boxrig_notification_bytes types[BOXRIG_NOTIFICATION_TYPE_CAPACITY];
    size_t type_count;
    boxrig_notification_occasion occasion;
    boxrig_notification_urgency urgency;
} boxrig_notification_transaction;

typedef struct boxrig_notification_discard {
    bool active;
    uint8_t identifier[BOXRIG_NOTIFICATION_IDENTIFIER_CAPACITY];
    size_t identifier_length;
} boxrig_notification_discard;

struct boxrig_notification_state {
    boxrig_notification_transaction *transactions;
    boxrig_notification_discard *discards;
    bool reject_unknown_identifiers;
};

typedef struct boxrig_notification_slice {
    const uint8_t *bytes;
    size_t length;
} boxrig_notification_slice;

typedef enum boxrig_notification_payload_kind {
    BOXRIG_NOTIFICATION_PAYLOAD_TITLE = 0,
    BOXRIG_NOTIFICATION_PAYLOAD_BODY,
    BOXRIG_NOTIFICATION_PAYLOAD_QUERY,
    BOXRIG_NOTIFICATION_PAYLOAD_IGNORE
} boxrig_notification_payload_kind;

typedef struct boxrig_notification_metadata {
    bool valid;
    bool done;
    bool encoded;
    bool identifier_was_set;
    bool has_identifier;
    boxrig_notification_slice identifier;
    boxrig_notification_payload_kind payload_kind;
    bool application_name_was_set;
    boxrig_notification_slice application_name;
    boxrig_notification_slice types[BOXRIG_NOTIFICATION_TYPE_CAPACITY];
    size_t type_count;
    bool occasion_was_set;
    boxrig_notification_occasion occasion;
    bool urgency_was_set;
    boxrig_notification_urgency urgency;
} boxrig_notification_metadata;

static bool slice_equal(
    boxrig_notification_slice value,
    const char *expected)
{
    size_t expected_length = strlen(expected);

    return value.length == expected_length &&
           memcmp(value.bytes, expected, expected_length) == 0;
}

static bool metadata_value_character(uint8_t byte)
{
    return (byte >= (uint8_t)'a' && byte <= (uint8_t)'z') ||
           (byte >= (uint8_t)'A' && byte <= (uint8_t)'Z') ||
           (byte >= (uint8_t)'0' && byte <= (uint8_t)'9') ||
           byte == (uint8_t)'-' || byte == (uint8_t)'_' ||
           byte == (uint8_t)'/' || byte == (uint8_t)'+' ||
           byte == (uint8_t)'.' || byte == (uint8_t)',' ||
           byte == (uint8_t)'(' || byte == (uint8_t)')' ||
           byte == (uint8_t)'{' || byte == (uint8_t)'}' ||
           byte == (uint8_t)'[' || byte == (uint8_t)']' ||
           byte == (uint8_t)'*' || byte == (uint8_t)'&' ||
           byte == (uint8_t)'^' || byte == (uint8_t)'%' ||
           byte == (uint8_t)'$' || byte == (uint8_t)'#' ||
           byte == (uint8_t)'@' || byte == (uint8_t)'!' ||
           byte == (uint8_t)'`' || byte == (uint8_t)'~';
}

static bool identifier_is_valid(boxrig_notification_slice identifier)
{
    size_t index;

    if (identifier.length == 0 ||
        identifier.length > BOXRIG_NOTIFICATION_IDENTIFIER_CAPACITY) {
        return false;
    }
    for (index = 0; index < identifier.length; index++) {
        uint8_t byte = identifier.bytes[index];

        if (!((byte >= (uint8_t)'a' && byte <= (uint8_t)'z') ||
              (byte >= (uint8_t)'A' && byte <= (uint8_t)'Z') ||
              (byte >= (uint8_t)'0' && byte <= (uint8_t)'9') ||
              byte == (uint8_t)'_' || byte == (uint8_t)'-' ||
              byte == (uint8_t)'+' || byte == (uint8_t)'.')) {
            return false;
        }
    }
    return true;
}

static boxrig_notification_metadata parse_metadata(
    const uint8_t *metadata,
    size_t metadata_length)
{
    boxrig_notification_metadata parsed;
    size_t offset = 0;

    memset(&parsed, 0, sizeof(parsed));
    parsed.valid = true;
    parsed.done = true;
    parsed.payload_kind = BOXRIG_NOTIFICATION_PAYLOAD_TITLE;
    parsed.occasion = BOXRIG_NOTIFICATION_ALWAYS;
    parsed.urgency = BOXRIG_NOTIFICATION_URGENCY_NORMAL;
    while (offset < metadata_length) {
        size_t end = offset;
        boxrig_notification_slice value;
        uint8_t key;
        size_t index;

        while (end < metadata_length && metadata[end] != (uint8_t)':') {
            end++;
        }
        if (end - offset < 3 || metadata[offset + 1] != (uint8_t)'=' ||
            !((metadata[offset] >= (uint8_t)'a' &&
               metadata[offset] <= (uint8_t)'z') ||
              (metadata[offset] >= (uint8_t)'A' &&
               metadata[offset] <= (uint8_t)'Z'))) {
            parsed.valid = false;
            return parsed;
        }
        key = metadata[offset];
        value.bytes = metadata + offset + 2;
        value.length = end - offset - 2;
        for (index = 0; index < value.length; index++) {
            if (!metadata_value_character(value.bytes[index]) &&
                !(key == (uint8_t)'p' && value.length == 1 &&
                  value.bytes[index] == (uint8_t)'?')) {
                parsed.valid = false;
                return parsed;
            }
        }
        if (key == (uint8_t)'i') {
            parsed.identifier_was_set = true;
            parsed.identifier = value;
            if (!identifier_is_valid(value)) {
                parsed.valid = false;
                return parsed;
            }
            parsed.has_identifier = !slice_equal(value, "0");
        } else if (key == (uint8_t)'d') {
            if (slice_equal(value, "0")) {
                parsed.done = false;
            } else if (slice_equal(value, "1")) {
                parsed.done = true;
            } else {
                parsed.valid = false;
                return parsed;
            }
        } else if (key == (uint8_t)'e') {
            if (slice_equal(value, "0")) {
                parsed.encoded = false;
            } else if (slice_equal(value, "1")) {
                parsed.encoded = true;
            } else {
                parsed.valid = false;
                return parsed;
            }
        } else if (key == (uint8_t)'p') {
            if (slice_equal(value, "title")) {
                parsed.payload_kind = BOXRIG_NOTIFICATION_PAYLOAD_TITLE;
            } else if (slice_equal(value, "body")) {
                parsed.payload_kind = BOXRIG_NOTIFICATION_PAYLOAD_BODY;
            } else if (slice_equal(value, "?")) {
                parsed.payload_kind = BOXRIG_NOTIFICATION_PAYLOAD_QUERY;
            } else {
                parsed.payload_kind = BOXRIG_NOTIFICATION_PAYLOAD_IGNORE;
            }
        } else if (key == (uint8_t)'f') {
            parsed.application_name_was_set = true;
            parsed.application_name = value;
        } else if (key == (uint8_t)'t') {
            if (parsed.type_count >= BOXRIG_NOTIFICATION_TYPE_CAPACITY) {
                parsed.valid = false;
                return parsed;
            }
            parsed.types[parsed.type_count] = value;
            parsed.type_count++;
        } else if (key == (uint8_t)'o') {
            parsed.occasion_was_set = true;
            if (slice_equal(value, "always")) {
                parsed.occasion = BOXRIG_NOTIFICATION_ALWAYS;
            } else if (slice_equal(value, "unfocused")) {
                parsed.occasion = BOXRIG_NOTIFICATION_UNFOCUSED;
            } else if (slice_equal(value, "invisible")) {
                parsed.occasion = BOXRIG_NOTIFICATION_INVISIBLE;
            } else {
                parsed.valid = false;
                return parsed;
            }
        } else if (key == (uint8_t)'u') {
            parsed.urgency_was_set = true;
            if (slice_equal(value, "0")) {
                parsed.urgency = BOXRIG_NOTIFICATION_URGENCY_LOW;
            } else if (slice_equal(value, "1")) {
                parsed.urgency = BOXRIG_NOTIFICATION_URGENCY_NORMAL;
            } else if (slice_equal(value, "2")) {
                parsed.urgency = BOXRIG_NOTIFICATION_URGENCY_CRITICAL;
            } else {
                parsed.valid = false;
                return parsed;
            }
        }
        if (end == metadata_length) {
            offset = end;
        } else {
            offset = end + 1;
            if (offset == metadata_length) {
                parsed.valid = false;
                return parsed;
            }
        }
    }
    if (!parsed.done && !parsed.has_identifier) {
        parsed.valid = false;
    }
    return parsed;
}

static void bytes_destroy(boxrig_notification_bytes *bytes)
{
    free(bytes->bytes);
    memset(bytes, 0, sizeof(*bytes));
}

static boxrig_status bytes_clone(
    const boxrig_notification_bytes *source,
    boxrig_notification_bytes *destination)
{
    memset(destination, 0, sizeof(*destination));
    if (source->length != 0) {
        destination->bytes = malloc(source->length);
        if (destination->bytes == NULL) {
            return BOXRIG_STATUS_OUT_OF_MEMORY;
        }
        memcpy(destination->bytes, source->bytes, source->length);
        destination->length = source->length;
        destination->capacity = source->length;
    }
    memcpy(
        destination->base64_quartet,
        source->base64_quartet,
        sizeof(destination->base64_quartet));
    destination->base64_quartet_length = source->base64_quartet_length;
    return BOXRIG_STATUS_OK;
}

static boxrig_status bytes_reserve(
    boxrig_notification_bytes *bytes,
    size_t required,
    size_t maximum)
{
    uint8_t *replacement;

    if (required > maximum) {
        return BOXRIG_STATUS_LIMIT_EXCEEDED;
    }
    if (required <= bytes->capacity) {
        return BOXRIG_STATUS_OK;
    }
    /* Exact growth keeps aggregate transaction storage within the configured
     * decoded-byte bound even when a notification uses many small fields. */
    replacement = realloc(bytes->bytes, required);
    if (replacement == NULL) {
        return BOXRIG_STATUS_OUT_OF_MEMORY;
    }
    bytes->bytes = replacement;
    bytes->capacity = required;
    return BOXRIG_STATUS_OK;
}

static boxrig_status bytes_append(
    boxrig_notification_bytes *bytes,
    const uint8_t *input,
    size_t input_length,
    size_t maximum)
{
    boxrig_status status;

    if (input_length > maximum - bytes->length) {
        return BOXRIG_STATUS_LIMIT_EXCEEDED;
    }
    status = bytes_reserve(bytes, bytes->length + input_length, maximum);
    if (status != BOXRIG_STATUS_OK) {
        return status;
    }
    if (input_length != 0) {
        memcpy(bytes->bytes + bytes->length, input, input_length);
        bytes->length += input_length;
    }
    return BOXRIG_STATUS_OK;
}

static int base64_value(uint8_t byte)
{
    if (byte >= (uint8_t)'A' && byte <= (uint8_t)'Z') {
        return byte - (uint8_t)'A';
    }
    if (byte >= (uint8_t)'a' && byte <= (uint8_t)'z') {
        return byte - (uint8_t)'a' + 26;
    }
    if (byte >= (uint8_t)'0' && byte <= (uint8_t)'9') {
        return byte - (uint8_t)'0' + 52;
    }
    if (byte == (uint8_t)'+') {
        return 62;
    }
    if (byte == (uint8_t)'/') {
        return 63;
    }
    if (byte == (uint8_t)'=') {
        return 64;
    }
    return -1;
}

static boxrig_status decode_quartet(
    boxrig_notification_bytes *bytes,
    size_t maximum,
    bool *padded_out)
{
    const uint8_t *quartet = bytes->base64_quartet;
    uint8_t decoded[3];
    size_t decoded_length;

    *padded_out = false;
    if (quartet[0] >= 64 || quartet[1] >= 64) {
        return BOXRIG_STATUS_LIMIT_EXCEEDED;
    }
    decoded[0] = (uint8_t)((quartet[0] << 2) | (quartet[1] >> 4));
    if (quartet[2] == 64) {
        if (quartet[3] != 64 || (quartet[1] & 0x0f) != 0) {
            return BOXRIG_STATUS_LIMIT_EXCEEDED;
        }
        decoded_length = 1;
        *padded_out = true;
    } else {
        decoded[1] = (uint8_t)((quartet[1] << 4) | (quartet[2] >> 2));
        if (quartet[3] == 64) {
            if ((quartet[2] & 0x03) != 0) {
                return BOXRIG_STATUS_LIMIT_EXCEEDED;
            }
            decoded_length = 2;
            *padded_out = true;
        } else {
            decoded[2] = (uint8_t)((quartet[2] << 6) | quartet[3]);
            decoded_length = 3;
        }
    }
    bytes->base64_quartet_length = 0;
    return bytes_append(bytes, decoded, decoded_length, maximum);
}

static boxrig_status append_base64_fragment(
    boxrig_notification_bytes *bytes,
    const uint8_t *input,
    size_t input_length,
    size_t maximum)
{
    bool padded = false;
    size_t index;

    if (input_length > BOXRIG_NOTIFICATION_ENCODED_FRAGMENT_CAPACITY) {
        return BOXRIG_STATUS_LIMIT_EXCEEDED;
    }
    for (index = 0; index < input_length; index++) {
        int value;
        boxrig_status status;

        if (padded) {
            return BOXRIG_STATUS_LIMIT_EXCEEDED;
        }
        value = base64_value(input[index]);
        if (value < 0) {
            return BOXRIG_STATUS_LIMIT_EXCEEDED;
        }
        bytes->base64_quartet[bytes->base64_quartet_length] =
            (uint8_t)value;
        bytes->base64_quartet_length++;
        if (bytes->base64_quartet_length == 4) {
            status = decode_quartet(bytes, maximum, &padded);
            if (status != BOXRIG_STATUS_OK) {
                return status;
            }
        }
    }
    return BOXRIG_STATUS_OK;
}

static boxrig_status finalize_base64(
    boxrig_notification_bytes *bytes,
    size_t maximum)
{
    uint8_t decoded[2];
    size_t decoded_length;

    if (bytes->base64_quartet_length == 0) {
        return BOXRIG_STATUS_OK;
    }
    if (bytes->base64_quartet_length == 1 ||
        bytes->base64_quartet[0] >= 64 ||
        bytes->base64_quartet[1] >= 64) {
        return BOXRIG_STATUS_LIMIT_EXCEEDED;
    }
    decoded[0] = (uint8_t)((bytes->base64_quartet[0] << 2) |
                           (bytes->base64_quartet[1] >> 4));
    if (bytes->base64_quartet_length == 2) {
        if ((bytes->base64_quartet[1] & 0x0f) != 0) {
            return BOXRIG_STATUS_LIMIT_EXCEEDED;
        }
        decoded_length = 1;
    } else {
        if (bytes->base64_quartet[2] >= 64 ||
            (bytes->base64_quartet[2] & 0x03) != 0) {
            return BOXRIG_STATUS_LIMIT_EXCEEDED;
        }
        decoded[1] = (uint8_t)((bytes->base64_quartet[1] << 4) |
                               (bytes->base64_quartet[2] >> 2));
        decoded_length = 2;
    }
    bytes->base64_quartet_length = 0;
    return bytes_append(bytes, decoded, decoded_length, maximum);
}

static bool is_control(utf8proc_int32_t codepoint)
{
    return (codepoint >= 0 && codepoint <= 0x1f) ||
           (codepoint >= 0x7f && codepoint <= 0x9f);
}

static bool valid_utf8_text(
    const uint8_t *bytes,
    size_t length,
    bool body_controls)
{
    size_t offset = 0;

    if (length > (size_t)PTRDIFF_MAX) {
        return false;
    }
    while (offset < length) {
        utf8proc_int32_t codepoint = -1;
        utf8proc_ssize_t decoded = utf8proc_iterate(
            bytes + offset,
            (utf8proc_ssize_t)(length - offset),
            &codepoint);

        if (decoded <= 0 ||
            (is_control(codepoint) &&
             !(body_controls &&
               (codepoint == 0x09 || codepoint == 0x0a ||
                codepoint == 0x0d)))) {
            return false;
        }
        offset += (size_t)decoded;
    }
    return true;
}

static boxrig_status normalize_body(boxrig_notification_bytes *body)
{
    uint8_t *normalized;
    size_t input_offset = 0;
    size_t output_length = 0;

    if (!valid_utf8_text(body->bytes, body->length, true)) {
        return BOXRIG_STATUS_LIMIT_EXCEEDED;
    }
    if (body->length == 0 ||
        memchr(body->bytes, '\r', body->length) == NULL) {
        return BOXRIG_STATUS_OK;
    }
    normalized = malloc(body->length);
    if (normalized == NULL) {
        return BOXRIG_STATUS_OUT_OF_MEMORY;
    }
    while (input_offset < body->length) {
        if (body->bytes[input_offset] == (uint8_t)'\r') {
            normalized[output_length] = (uint8_t)'\n';
            output_length++;
            input_offset++;
            if (input_offset < body->length &&
                body->bytes[input_offset] == (uint8_t)'\n') {
                input_offset++;
            }
        } else {
            normalized[output_length] = body->bytes[input_offset];
            output_length++;
            input_offset++;
        }
    }
    free(body->bytes);
    body->bytes = normalized;
    body->length = output_length;
    body->capacity = output_length;
    return BOXRIG_STATUS_OK;
}

static void transaction_destroy(boxrig_notification_transaction *transaction)
{
    size_t index;

    bytes_destroy(&transaction->title);
    bytes_destroy(&transaction->body);
    bytes_destroy(&transaction->application_name);
    for (index = 0; index < transaction->type_count; index++) {
        bytes_destroy(&transaction->types[index]);
    }
    memset(transaction, 0, sizeof(*transaction));
}

static void transaction_initialize(
    boxrig_notification_transaction *transaction,
    boxrig_notification_slice identifier)
{
    memset(transaction, 0, sizeof(*transaction));
    transaction->active = true;
    transaction->identifier_length = identifier.length;
    if (identifier.length != 0) {
        memcpy(
            transaction->identifier,
            identifier.bytes,
            identifier.length);
    }
    transaction->occasion = BOXRIG_NOTIFICATION_ALWAYS;
    transaction->urgency = BOXRIG_NOTIFICATION_URGENCY_NORMAL;
}

static boxrig_status transaction_clone(
    const boxrig_notification_transaction *source,
    boxrig_notification_transaction *destination)
{
    boxrig_status status;
    size_t index;

    memset(destination, 0, sizeof(*destination));
    destination->active = source->active;
    memcpy(
        destination->identifier,
        source->identifier,
        source->identifier_length);
    destination->identifier_length = source->identifier_length;
    destination->occasion = source->occasion;
    destination->urgency = source->urgency;
    status = bytes_clone(&source->title, &destination->title);
    if (status == BOXRIG_STATUS_OK) {
        status = bytes_clone(&source->body, &destination->body);
    }
    if (status == BOXRIG_STATUS_OK) {
        status = bytes_clone(
            &source->application_name,
            &destination->application_name);
    }
    for (index = 0; status == BOXRIG_STATUS_OK &&
                    index < source->type_count;
         index++) {
        status = bytes_clone(&source->types[index], &destination->types[index]);
        if (status == BOXRIG_STATUS_OK) {
            destination->type_count++;
        }
    }
    if (status != BOXRIG_STATUS_OK) {
        transaction_destroy(destination);
    }
    return status;
}

static size_t transaction_length(
    const boxrig_notification_transaction *transaction)
{
    size_t length = transaction->title.length + transaction->body.length +
                    transaction->application_name.length;
    size_t index;

    for (index = 0; index < transaction->type_count; index++) {
        length += transaction->types[index].length;
    }
    return length;
}

static boxrig_status decode_metadata_text(
    boxrig_notification_slice encoded,
    size_t maximum,
    boxrig_notification_bytes *decoded)
{
    boxrig_status status;

    memset(decoded, 0, sizeof(*decoded));
    status = append_base64_fragment(
        decoded,
        encoded.bytes,
        encoded.length,
        maximum);
    if (status == BOXRIG_STATUS_OK) {
        status = finalize_base64(decoded, maximum);
    }
    if (status == BOXRIG_STATUS_OK &&
        !valid_utf8_text(decoded->bytes, decoded->length, false)) {
        status = BOXRIG_STATUS_LIMIT_EXCEEDED;
    }
    if (status != BOXRIG_STATUS_OK) {
        bytes_destroy(decoded);
    }
    return status;
}

static boxrig_status apply_metadata(
    boxrig_notification_transaction *transaction,
    const boxrig_notification_metadata *metadata,
    size_t maximum)
{
    boxrig_status status = BOXRIG_STATUS_OK;
    size_t index;

    if (metadata->occasion_was_set) {
        transaction->occasion = metadata->occasion;
    }
    if (metadata->urgency_was_set) {
        transaction->urgency = metadata->urgency;
    }
    if (metadata->application_name_was_set) {
        boxrig_notification_bytes decoded;
        size_t other_length = transaction_length(transaction) -
                              transaction->application_name.length;

        status = decode_metadata_text(
            metadata->application_name,
            maximum - other_length,
            &decoded);
        if (status != BOXRIG_STATUS_OK) {
            return status;
        }
        bytes_destroy(&transaction->application_name);
        transaction->application_name = decoded;
    }
    for (index = 0; index < metadata->type_count; index++) {
        boxrig_notification_bytes decoded;
        size_t current_length;

        if (transaction->type_count >= BOXRIG_NOTIFICATION_TYPE_CAPACITY) {
            return BOXRIG_STATUS_LIMIT_EXCEEDED;
        }
        current_length = transaction_length(transaction);
        status = decode_metadata_text(
            metadata->types[index],
            maximum - current_length,
            &decoded);
        if (status != BOXRIG_STATUS_OK) {
            return status;
        }
        transaction->types[transaction->type_count] = decoded;
        transaction->type_count++;
    }
    return BOXRIG_STATUS_OK;
}

static boxrig_status apply_payload(
    boxrig_notification_transaction *transaction,
    const boxrig_notification_metadata *metadata,
    const uint8_t *payload,
    size_t payload_length,
    size_t maximum)
{
    boxrig_notification_bytes *target =
        metadata->payload_kind == BOXRIG_NOTIFICATION_PAYLOAD_BODY
            ? &transaction->body
            : &transaction->title;
    size_t other_length = transaction_length(transaction) - target->length;

    if (metadata->encoded) {
        return append_base64_fragment(
            target,
            payload,
            payload_length,
            maximum - other_length);
    }
    if (payload_length > BOXRIG_NOTIFICATION_PLAIN_FRAGMENT_CAPACITY ||
        target->base64_quartet_length != 0 ||
        !valid_utf8_text(payload, payload_length, false)) {
        return BOXRIG_STATUS_LIMIT_EXCEEDED;
    }
    return bytes_append(
        target,
        payload,
        payload_length,
        maximum - other_length);
}

static boxrig_status finalize_transaction(
    boxrig_notification_transaction *transaction,
    size_t maximum)
{
    boxrig_status status;
    size_t other_length = transaction_length(transaction) -
                          transaction->title.length;

    status = finalize_base64(
        &transaction->title,
        maximum - other_length);
    if (status != BOXRIG_STATUS_OK) {
        return status;
    }
    other_length = transaction_length(transaction) - transaction->body.length;
    status = finalize_base64(
        &transaction->body,
        maximum - other_length);
    if (status != BOXRIG_STATUS_OK ||
        !valid_utf8_text(
            transaction->title.bytes,
            transaction->title.length,
            false)) {
        return status == BOXRIG_STATUS_OK
                   ? BOXRIG_STATUS_LIMIT_EXCEEDED
                   : status;
    }
    return normalize_body(&transaction->body);
}

static bool identifier_matches(
    const uint8_t *stored,
    size_t stored_length,
    boxrig_notification_slice identifier)
{
    return stored_length == identifier.length &&
           memcmp(stored, identifier.bytes, stored_length) == 0;
}

static boxrig_notification_transaction *find_transaction(
    boxrig_terminal *terminal,
    boxrig_notification_slice identifier)
{
    size_t index;

    for (index = 0; index < terminal->notification_pending_capacity; index++) {
        boxrig_notification_transaction *transaction =
            &terminal->notification_state->transactions[index];

        if (transaction->active &&
            identifier_matches(
                transaction->identifier,
                transaction->identifier_length,
                identifier)) {
            return transaction;
        }
    }
    return NULL;
}

static boxrig_notification_transaction *find_free_transaction(
    boxrig_terminal *terminal)
{
    size_t index;

    for (index = 0; index < terminal->notification_pending_capacity; index++) {
        if (!terminal->notification_state->transactions[index].active) {
            return &terminal->notification_state->transactions[index];
        }
    }
    return NULL;
}

static boxrig_notification_discard *find_discard(
    boxrig_terminal *terminal,
    boxrig_notification_slice identifier)
{
    size_t index;

    for (index = 0; index < terminal->notification_pending_capacity; index++) {
        boxrig_notification_discard *discard =
            &terminal->notification_state->discards[index];

        if (discard->active &&
            identifier_matches(
                discard->identifier,
                discard->identifier_length,
                identifier)) {
            return discard;
        }
    }
    return NULL;
}

static void add_discard(
    boxrig_terminal *terminal,
    boxrig_notification_slice identifier)
{
    size_t index;

    if (find_discard(terminal, identifier) != NULL) {
        return;
    }
    for (index = 0; index < terminal->notification_pending_capacity; index++) {
        boxrig_notification_discard *discard =
            &terminal->notification_state->discards[index];

        if (!discard->active) {
            discard->active = true;
            discard->identifier_length = identifier.length;
            memcpy(
                discard->identifier,
                identifier.bytes,
                identifier.length);
            return;
        }
    }
    terminal->notification_state->reject_unknown_identifiers = true;
}

static boxrig_status queue_transaction(
    boxrig_terminal *terminal,
    const boxrig_notification_transaction *transaction)
{
    boxrig_notification_view view;
    boxrig_notification_text_view types[BOXRIG_NOTIFICATION_TYPE_CAPACITY];
    size_t index;

    memset(&view, 0, sizeof(view));
    view.has_identifier = transaction->identifier_length != 0;
    view.identifier.bytes = transaction->identifier;
    view.identifier.length = transaction->identifier_length;
    if (transaction->title.length == 0 && transaction->body.length != 0) {
        view.title.bytes = transaction->body.bytes;
        view.title.length = transaction->body.length;
    } else {
        view.title.bytes = transaction->title.bytes;
        view.title.length = transaction->title.length;
        view.body.bytes = transaction->body.bytes;
        view.body.length = transaction->body.length;
    }
    if (view.title.length == 0 && view.body.length == 0) {
        return BOXRIG_STATUS_OK;
    }
    view.application_name.bytes = transaction->application_name.bytes;
    view.application_name.length = transaction->application_name.length;
    for (index = 0; index < transaction->type_count; index++) {
        types[index].bytes = transaction->types[index].bytes;
        types[index].length = transaction->types[index].length;
    }
    view.types = transaction->type_count == 0 ? NULL : types;
    view.type_count = transaction->type_count;
    view.occasion = transaction->occasion;
    view.urgency = transaction->urgency;
    return boxrig_terminal_queue_notification(terminal, &view);
}

static boxrig_status append_query_response(
    boxrig_terminal *terminal,
    const boxrig_notification_metadata *metadata,
    boxrig_parser_string_terminator terminator)
{
    static const char capabilities[] =
        "p=title,body,?:o=always,unfocused,invisible:u=0,1,2";
    char response[320];
    const uint8_t *identifier = (const uint8_t *)"0";
    size_t identifier_length = 1;
    int prefix_length;
    size_t response_length;

    if (metadata->identifier_was_set) {
        identifier = metadata->identifier.bytes;
        identifier_length = metadata->identifier.length;
    }
    prefix_length = snprintf(
        response,
        sizeof(response),
        "\x1b]99;i=%.*s:p=?;%s",
        (int)identifier_length,
        (const char *)identifier,
        capabilities);
    if (prefix_length < 0 || (size_t)prefix_length + 2 > sizeof(response)) {
        return BOXRIG_STATUS_LIMIT_EXCEEDED;
    }
    response_length = (size_t)prefix_length;
    if (terminator == BOXRIG_PARSER_STRING_TERMINATOR_BEL) {
        response[response_length] = '\x07';
        response_length++;
    } else {
        response[response_length] = '\x1b';
        response[response_length + 1] = '\\';
        response_length += 2;
    }
    return boxrig_terminal_append_output(
        terminal,
        (const uint8_t *)response,
        response_length);
}

static boxrig_status apply_fragment(
    boxrig_terminal *terminal,
    boxrig_notification_transaction *transaction,
    const boxrig_notification_metadata *metadata,
    const uint8_t *payload,
    size_t payload_length)
{
    boxrig_status status = apply_metadata(
        transaction,
        metadata,
        terminal->notification_capacity);

    if (status == BOXRIG_STATUS_OK) {
        status = apply_payload(
            transaction,
            metadata,
            payload,
            payload_length,
            terminal->notification_capacity);
    }
    if (status == BOXRIG_STATUS_OK && metadata->done) {
        status = finalize_transaction(
            transaction,
            terminal->notification_capacity);
    }
    return status;
}

boxrig_status boxrig_notification_initialize(boxrig_terminal *terminal)
{
    boxrig_notification_state *state;

    if (terminal->notification_capacity == 0) {
        return BOXRIG_STATUS_OK;
    }
    state = calloc(1, sizeof(*state));
    if (state == NULL) {
        return BOXRIG_STATUS_OUT_OF_MEMORY;
    }
    state->transactions = calloc(
        terminal->notification_pending_capacity,
        sizeof(*state->transactions));
    state->discards = calloc(
        terminal->notification_pending_capacity,
        sizeof(*state->discards));
    if (state->transactions == NULL || state->discards == NULL) {
        free(state->discards);
        free(state->transactions);
        free(state);
        return BOXRIG_STATUS_OUT_OF_MEMORY;
    }
    terminal->notification_state = state;
    return BOXRIG_STATUS_OK;
}

void boxrig_notification_reset(boxrig_terminal *terminal)
{
    size_t index;

    if (terminal == NULL || terminal->notification_state == NULL) {
        return;
    }
    for (index = 0; index < terminal->notification_pending_capacity; index++) {
        transaction_destroy(
            &terminal->notification_state->transactions[index]);
    }
    memset(
        terminal->notification_state->discards,
        0,
        terminal->notification_pending_capacity *
            sizeof(*terminal->notification_state->discards));
    terminal->notification_state->reject_unknown_identifiers = false;
}

void boxrig_notification_destroy(boxrig_terminal *terminal)
{
    if (terminal == NULL || terminal->notification_state == NULL) {
        return;
    }
    boxrig_notification_reset(terminal);
    free(terminal->notification_state->discards);
    free(terminal->notification_state->transactions);
    free(terminal->notification_state);
    terminal->notification_state = NULL;
}

boxrig_status boxrig_notification_dispatch(
    boxrig_terminal *terminal,
    const uint8_t *payload,
    size_t payload_length,
    boxrig_parser_string_terminator terminator)
{
    boxrig_notification_metadata metadata;
    boxrig_notification_transaction *stored = NULL;
    boxrig_notification_transaction candidate;
    boxrig_notification_discard *discard;
    const uint8_t *fragment;
    size_t fragment_length;
    size_t separator = 0;
    boxrig_status status;

    if (terminal->notification_capacity == 0) {
        return BOXRIG_STATUS_OK;
    }
    while (separator < payload_length && payload[separator] != (uint8_t)';') {
        separator++;
    }
    if (separator == payload_length) {
        return BOXRIG_STATUS_OK;
    }
    metadata = parse_metadata(payload, separator);
    fragment = payload + separator + 1;
    fragment_length = payload_length - separator - 1;
    if (!metadata.valid) {
        if (metadata.has_identifier) {
            stored = find_transaction(terminal, metadata.identifier);
            if (stored != NULL) {
                transaction_destroy(stored);
            }
        }
        return BOXRIG_STATUS_OK;
    }
    if (metadata.payload_kind == BOXRIG_NOTIFICATION_PAYLOAD_IGNORE) {
        return BOXRIG_STATUS_OK;
    }
    if (metadata.payload_kind == BOXRIG_NOTIFICATION_PAYLOAD_QUERY) {
        return append_query_response(terminal, &metadata, terminator);
    }
    if (metadata.has_identifier) {
        stored = find_transaction(terminal, metadata.identifier);
        discard = find_discard(terminal, metadata.identifier);
        if (discard != NULL) {
            if (metadata.done) {
                memset(discard, 0, sizeof(*discard));
            }
            return BOXRIG_STATUS_OK;
        }
        if (stored == NULL &&
            terminal->notification_state->reject_unknown_identifiers) {
            return BOXRIG_STATUS_OK;
        }
    }
    if (stored != NULL) {
        status = transaction_clone(stored, &candidate);
    } else {
        transaction_initialize(
            &candidate,
            metadata.has_identifier
                ? metadata.identifier
                : (boxrig_notification_slice){NULL, 0});
        status = BOXRIG_STATUS_OK;
    }
    if (status != BOXRIG_STATUS_OK) {
        return status;
    }
    status = apply_fragment(
        terminal,
        &candidate,
        &metadata,
        fragment,
        fragment_length);
    if (status == BOXRIG_STATUS_LIMIT_EXCEEDED) {
        transaction_destroy(&candidate);
        if (stored != NULL) {
            transaction_destroy(stored);
        }
        return BOXRIG_STATUS_OK;
    }
    if (status != BOXRIG_STATUS_OK) {
        transaction_destroy(&candidate);
        return status;
    }
    if (!metadata.done) {
        if (stored == NULL) {
            stored = find_free_transaction(terminal);
            if (stored == NULL) {
                add_discard(terminal, metadata.identifier);
                transaction_destroy(&candidate);
                return BOXRIG_STATUS_OK;
            }
        } else {
            transaction_destroy(stored);
        }
        *stored = candidate;
        return BOXRIG_STATUS_OK;
    }
    status = queue_transaction(terminal, &candidate);
    transaction_destroy(&candidate);
    if (status == BOXRIG_STATUS_OK ||
        status == BOXRIG_STATUS_LIMIT_EXCEEDED) {
        if (stored != NULL) {
            transaction_destroy(stored);
        }
        return BOXRIG_STATUS_OK;
    }
    return status;
}
