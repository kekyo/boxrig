/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "regex.h"

#include "utf8proc.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

typedef struct boxrig_regex_range {
    int32_t first;
    int32_t last;
} boxrig_regex_range;

typedef struct boxrig_regex_class {
    size_t range_start;
    size_t range_count;
    bool negated;
} boxrig_regex_class;

typedef enum boxrig_regex_node_type {
    BOXRIG_REGEX_NODE_EMPTY = 0,
    BOXRIG_REGEX_NODE_LITERAL,
    BOXRIG_REGEX_NODE_DOT,
    BOXRIG_REGEX_NODE_CLASS,
    BOXRIG_REGEX_NODE_BEGIN,
    BOXRIG_REGEX_NODE_END,
    BOXRIG_REGEX_NODE_CONCAT,
    BOXRIG_REGEX_NODE_ALTERNATE,
    BOXRIG_REGEX_NODE_OPTIONAL,
    BOXRIG_REGEX_NODE_STAR,
    BOXRIG_REGEX_NODE_PLUS
} boxrig_regex_node_type;

typedef struct boxrig_regex_node {
    boxrig_regex_node_type type;
    size_t left;
    size_t right;
    size_t first;
    size_t pattern_offset;
    int32_t scalar;
    size_t class_index;
    bool nullable;
} boxrig_regex_node;

typedef enum boxrig_regex_token_type {
    BOXRIG_REGEX_TOKEN_LITERAL = 0,
    BOXRIG_REGEX_TOKEN_DOT,
    BOXRIG_REGEX_TOKEN_CLASS,
    BOXRIG_REGEX_TOKEN_BEGIN,
    BOXRIG_REGEX_TOKEN_END,
    BOXRIG_REGEX_TOKEN_CONCAT,
    BOXRIG_REGEX_TOKEN_ALTERNATE,
    BOXRIG_REGEX_TOKEN_OPTIONAL,
    BOXRIG_REGEX_TOKEN_STAR,
    BOXRIG_REGEX_TOKEN_PLUS,
    BOXRIG_REGEX_TOKEN_REPEAT
} boxrig_regex_token_type;

typedef struct boxrig_regex_token {
    boxrig_regex_token_type type;
    size_t pattern_offset;
    int32_t scalar;
    size_t class_index;
    size_t repeat_minimum;
    size_t repeat_maximum;
} boxrig_regex_token;

typedef enum boxrig_regex_operator {
    BOXRIG_REGEX_OPERATOR_GROUP = 0,
    BOXRIG_REGEX_OPERATOR_ALTERNATE,
    BOXRIG_REGEX_OPERATOR_CONCAT
} boxrig_regex_operator;

typedef struct boxrig_regex_operator_entry {
    boxrig_regex_operator operator_kind;
    size_t pattern_offset;
} boxrig_regex_operator_entry;

typedef struct boxrig_regex_parser {
    const uint8_t *pattern;
    size_t pattern_length;
    size_t offset;
    const boxrig_text_recognizer_limits *limits;
    boxrig_text_regex_diagnostic *diagnostic;
    boxrig_regex_token *postfix;
    size_t postfix_count;
    size_t postfix_capacity;
    boxrig_regex_operator_entry *operators;
    size_t operator_count;
    size_t operator_capacity;
    boxrig_regex_class *classes;
    size_t class_count;
    size_t class_capacity;
    boxrig_regex_range *ranges;
    size_t range_count;
    size_t range_capacity;
} boxrig_regex_parser;

typedef enum boxrig_regex_state_type {
    BOXRIG_REGEX_STATE_EPSILON = 0,
    BOXRIG_REGEX_STATE_SPLIT,
    BOXRIG_REGEX_STATE_LITERAL,
    BOXRIG_REGEX_STATE_DOT,
    BOXRIG_REGEX_STATE_CLASS,
    BOXRIG_REGEX_STATE_ASSERT_BEGIN,
    BOXRIG_REGEX_STATE_ASSERT_END,
    BOXRIG_REGEX_STATE_ACCEPT
} boxrig_regex_state_type;

typedef struct boxrig_regex_state {
    boxrig_regex_state_type type;
    size_t out;
    size_t alternate_out;
    int32_t scalar;
    size_t class_index;
} boxrig_regex_state;

typedef struct boxrig_regex_fragment {
    size_t start;
    size_t end;
} boxrig_regex_fragment;

struct boxrig_regex_program {
    boxrig_regex_state *states;
    size_t state_count;
    size_t start_state;
    boxrig_regex_class *classes;
    size_t class_count;
    boxrig_regex_range *ranges;
    size_t range_count;
};

typedef enum boxrig_regex_cursor_phase {
    BOXRIG_REGEX_CURSOR_CANDIDATE = 0,
    BOXRIG_REGEX_CURSOR_START_CLOSURE,
    BOXRIG_REGEX_CURSOR_TRANSITION,
    BOXRIG_REGEX_CURSOR_OUTPUT_CLOSURE,
    BOXRIG_REGEX_CURSOR_COMPLETE
} boxrig_regex_cursor_phase;

typedef enum boxrig_regex_closure_target {
    BOXRIG_REGEX_CLOSURE_CURRENT = 0,
    BOXRIG_REGEX_CLOSURE_NEXT
} boxrig_regex_closure_target;

struct boxrig_regex_cursor {
    const boxrig_regex_program *program;
    const int32_t *scalars;
    const uint8_t *boundaries;
    size_t scalar_count;
    size_t candidate;
    size_t position;
    size_t longest_end;
    size_t *current_states;
    size_t current_count;
    size_t *next_states;
    size_t next_count;
    size_t transition_index;
    size_t *closure_stack;
    size_t closure_count;
    uint64_t *seen;
    uint64_t generation;
    boxrig_regex_cursor_phase phase;
    boxrig_regex_closure_target closure_target;
    bool current_accept;
    bool next_accept;
    bool next_prepared;
};

static void set_diagnostic(
    boxrig_regex_parser *parser,
    boxrig_text_regex_diagnostic_code code,
    size_t offset)
{
    parser->diagnostic->code = code;
    parser->diagnostic->pattern_offset =
        offset < parser->pattern_length ? offset : parser->pattern_length;
}

static bool append_range(
    boxrig_regex_parser *parser,
    int32_t first,
    int32_t last)
{
    if (parser->range_count == parser->range_capacity) {
        set_diagnostic(
            parser,
            BOXRIG_TEXT_REGEX_STATE_LIMIT,
            parser->offset);
        return false;
    }
    parser->ranges[parser->range_count].first = first;
    parser->ranges[parser->range_count].last = last;
    parser->range_count++;
    return true;
}

static bool decode_scalar(
    boxrig_regex_parser *parser,
    int32_t *scalar_out,
    size_t *length_out)
{
    utf8proc_int32_t scalar = -1;
    utf8proc_ssize_t length = utf8proc_iterate(
        (const utf8proc_uint8_t *)&parser->pattern[parser->offset],
        (utf8proc_ssize_t)(parser->pattern_length - parser->offset),
        &scalar);

    if (length <= 0) {
        set_diagnostic(
            parser,
            BOXRIG_TEXT_REGEX_INVALID_UTF8,
            parser->offset);
        return false;
    }
    *scalar_out = (int32_t)scalar;
    *length_out = (size_t)length;
    return true;
}

static bool is_hex(uint8_t byte)
{
    return (byte >= (uint8_t)'0' && byte <= (uint8_t)'9') ||
           (byte >= (uint8_t)'a' && byte <= (uint8_t)'f') ||
           (byte >= (uint8_t)'A' && byte <= (uint8_t)'F');
}

static unsigned int hex_value(uint8_t byte)
{
    if (byte <= (uint8_t)'9') {
        return (unsigned int)(byte - (uint8_t)'0');
    }
    if (byte <= (uint8_t)'F') {
        return (unsigned int)(byte - (uint8_t)'A') + 10u;
    }
    return (unsigned int)(byte - (uint8_t)'a') + 10u;
}

static bool escaped_scalar(
    boxrig_regex_parser *parser,
    int32_t *scalar_out,
    size_t *escape_kind_out)
{
    size_t escape_offset = parser->offset;
    uint8_t byte;

    *escape_kind_out = 0u;
    parser->offset++;
    if (parser->offset == parser->pattern_length) {
        set_diagnostic(
            parser,
            BOXRIG_TEXT_REGEX_INVALID_ESCAPE,
            escape_offset);
        return false;
    }
    byte = parser->pattern[parser->offset++];
    if (byte == (uint8_t)'n') {
        *scalar_out = '\n';
    } else if (byte == (uint8_t)'r') {
        *scalar_out = '\r';
    } else if (byte == (uint8_t)'t') {
        *scalar_out = '\t';
    } else if (byte == (uint8_t)'d') {
        *escape_kind_out = 1u;
    } else if (byte == (uint8_t)'s') {
        *escape_kind_out = 2u;
    } else if (byte == (uint8_t)'w') {
        *escape_kind_out = 3u;
    } else if (byte == (uint8_t)'u') {
        uint32_t scalar = 0u;
        size_t digit_count = 0u;

        if (parser->offset == parser->pattern_length ||
            parser->pattern[parser->offset] != (uint8_t)'{') {
            set_diagnostic(
                parser,
                BOXRIG_TEXT_REGEX_INVALID_ESCAPE,
                escape_offset);
            return false;
        }
        parser->offset++;
        while (parser->offset < parser->pattern_length &&
               is_hex(parser->pattern[parser->offset]) &&
               digit_count < 7u) {
            unsigned int digit = hex_value(parser->pattern[parser->offset]);

            if (scalar > (UINT32_MAX - digit) / 16u) {
                digit_count = 7u;
                break;
            }
            scalar = scalar * 16u + digit;
            parser->offset++;
            digit_count++;
        }
        if (digit_count == 0u || digit_count > 6u ||
            parser->offset == parser->pattern_length ||
            parser->pattern[parser->offset] != (uint8_t)'}' ||
            scalar > UINT32_C(0x10ffff) ||
            (scalar >= UINT32_C(0xd800) && scalar <= UINT32_C(0xdfff))) {
            set_diagnostic(
                parser,
                BOXRIG_TEXT_REGEX_INVALID_ESCAPE,
                escape_offset);
            return false;
        }
        parser->offset++;
        *scalar_out = (int32_t)scalar;
    } else if (strchr("\\.[](){}*+?|^$-", (int)byte) != NULL) {
        *scalar_out = (int32_t)byte;
    } else {
        set_diagnostic(
            parser,
            BOXRIG_TEXT_REGEX_INVALID_ESCAPE,
            escape_offset);
        return false;
    }
    return true;
}

static bool append_ascii_class_ranges(
    boxrig_regex_parser *parser,
    size_t escape_kind)
{
    if (escape_kind == 1u) {
        return append_range(parser, '0', '9');
    }
    if (escape_kind == 2u) {
        return append_range(parser, '\t', '\r') &&
               append_range(parser, ' ', ' ');
    }
    return append_range(parser, '0', '9') &&
           append_range(parser, 'A', 'Z') &&
           append_range(parser, '_', '_') &&
           append_range(parser, 'a', 'z');
}

static bool parse_class_element(
    boxrig_regex_parser *parser,
    size_t *range_start_out,
    size_t *range_count_out,
    bool *single_out)
{
    size_t range_start = parser->range_count;
    int32_t scalar = 0;
    size_t escape_kind = 0u;

    if (parser->offset == parser->pattern_length) {
        set_diagnostic(
            parser,
            BOXRIG_TEXT_REGEX_UNBALANCED_DELIMITER,
            parser->offset == 0u ? 0u : parser->offset - 1u);
        return false;
    }
    if (parser->pattern[parser->offset] == (uint8_t)'\\') {
        if (!escaped_scalar(parser, &scalar, &escape_kind)) {
            return false;
        }
        if (escape_kind != 0u) {
            if (!append_ascii_class_ranges(parser, escape_kind)) {
                return false;
            }
        } else if (!append_range(parser, scalar, scalar)) {
            return false;
        }
    } else {
        size_t length;

        if (!decode_scalar(parser, &scalar, &length)) {
            return false;
        }
        parser->offset += length;
        if (!append_range(parser, scalar, scalar)) {
            return false;
        }
    }
    *range_start_out = range_start;
    *range_count_out = parser->range_count - range_start;
    *single_out = *range_count_out == 1u &&
                  parser->ranges[range_start].first ==
                      parser->ranges[range_start].last;
    return true;
}

static bool parse_class(
    boxrig_regex_parser *parser,
    size_t class_offset,
    size_t *class_index_out)
{
    boxrig_regex_class *class_value;
    size_t class_range_start;
    bool has_element = false;

    if (parser->class_count == parser->class_capacity) {
        set_diagnostic(
            parser,
            BOXRIG_TEXT_REGEX_STATE_LIMIT,
            class_offset);
        return false;
    }
    class_value = &parser->classes[parser->class_count];
    memset(class_value, 0, sizeof(*class_value));
    parser->offset++;
    if (parser->offset < parser->pattern_length &&
        parser->pattern[parser->offset] == (uint8_t)'^') {
        class_value->negated = true;
        parser->offset++;
    }
    class_range_start = parser->range_count;
    while (parser->offset < parser->pattern_length &&
           parser->pattern[parser->offset] != (uint8_t)']') {
        size_t first_start;
        size_t first_count;
        bool first_single;

        if (!parse_class_element(
                parser,
                &first_start,
                &first_count,
                &first_single)) {
            return false;
        }
        has_element = true;
        if (first_single && parser->offset + 1u < parser->pattern_length &&
            parser->pattern[parser->offset] == (uint8_t)'-' &&
            parser->pattern[parser->offset + 1u] != (uint8_t)']') {
            size_t second_start;
            size_t second_count;
            bool second_single;
            int32_t first = parser->ranges[first_start].first;

            parser->offset++;
            if (!parse_class_element(
                    parser,
                    &second_start,
                    &second_count,
                    &second_single)) {
                return false;
            }
            if (!second_single ||
                first > parser->ranges[second_start].first) {
                set_diagnostic(
                    parser,
                    BOXRIG_TEXT_REGEX_INVALID_RANGE,
                    parser->offset == 0u ? class_offset : parser->offset - 1u);
                return false;
            }
            parser->ranges[first_start].last =
                parser->ranges[second_start].first;
            parser->range_count = second_start;
        }
    }
    if (parser->offset == parser->pattern_length) {
        set_diagnostic(
            parser,
            BOXRIG_TEXT_REGEX_UNBALANCED_DELIMITER,
            class_offset);
        return false;
    }
    if (!has_element) {
        set_diagnostic(
            parser,
            BOXRIG_TEXT_REGEX_INVALID_RANGE,
            class_offset);
        return false;
    }
    parser->offset++;
    class_value->range_start = class_range_start;
    class_value->range_count = parser->range_count - class_range_start;
    *class_index_out = parser->class_count++;
    return true;
}

static bool append_postfix(
    boxrig_regex_parser *parser,
    boxrig_regex_token token)
{
    if (parser->postfix_count == parser->postfix_capacity) {
        set_diagnostic(
            parser,
            BOXRIG_TEXT_REGEX_STATE_LIMIT,
            token.pattern_offset);
        return false;
    }
    parser->postfix[parser->postfix_count++] = token;
    return true;
}

static unsigned int operator_precedence(boxrig_regex_operator operator_kind)
{
    return operator_kind == BOXRIG_REGEX_OPERATOR_CONCAT ? 2u : 1u;
}

static bool emit_operator(
    boxrig_regex_parser *parser,
    boxrig_regex_operator_entry entry)
{
    boxrig_regex_token token;

    memset(&token, 0, sizeof(token));
    token.pattern_offset = entry.pattern_offset;
    token.type = entry.operator_kind == BOXRIG_REGEX_OPERATOR_CONCAT
                     ? BOXRIG_REGEX_TOKEN_CONCAT
                     : BOXRIG_REGEX_TOKEN_ALTERNATE;
    return append_postfix(parser, token);
}

static bool push_operator(
    boxrig_regex_parser *parser,
    boxrig_regex_operator operator_kind,
    size_t pattern_offset)
{
    boxrig_regex_operator_entry entry;

    while (parser->operator_count != 0u &&
           parser->operators[parser->operator_count - 1u].operator_kind !=
               BOXRIG_REGEX_OPERATOR_GROUP &&
           operator_precedence(
               parser->operators[parser->operator_count - 1u].operator_kind) >=
               operator_precedence(operator_kind)) {
        if (!emit_operator(
                parser,
                parser->operators[--parser->operator_count])) {
            return false;
        }
    }
    if (parser->operator_count == parser->operator_capacity) {
        set_diagnostic(
            parser,
            BOXRIG_TEXT_REGEX_STATE_LIMIT,
            pattern_offset);
        return false;
    }
    entry.operator_kind = operator_kind;
    entry.pattern_offset = pattern_offset;
    parser->operators[parser->operator_count++] = entry;
    return true;
}

static bool parse_decimal(
    boxrig_regex_parser *parser,
    size_t *value_out,
    size_t quantifier_offset)
{
    size_t value = 0u;
    size_t digit_count = 0u;

    while (parser->offset < parser->pattern_length &&
           parser->pattern[parser->offset] >= (uint8_t)'0' &&
           parser->pattern[parser->offset] <= (uint8_t)'9') {
        size_t digit =
            (size_t)(parser->pattern[parser->offset] - (uint8_t)'0');

        if (value > (SIZE_MAX - digit) / 10u) {
            set_diagnostic(
                parser,
                BOXRIG_TEXT_REGEX_QUANTIFIER_LIMIT,
                quantifier_offset);
            return false;
        }
        value = value * 10u + digit;
        parser->offset++;
        digit_count++;
    }
    if (digit_count == 0u) {
        set_diagnostic(
            parser,
            BOXRIG_TEXT_REGEX_INVALID_QUANTIFIER,
            quantifier_offset);
        return false;
    }
    *value_out = value;
    return true;
}

static bool parse_repeat(
    boxrig_regex_parser *parser,
    boxrig_regex_token *token)
{
    size_t quantifier_offset = parser->offset;
    size_t minimum;
    size_t maximum;

    parser->offset++;
    if (!parse_decimal(parser, &minimum, quantifier_offset)) {
        return false;
    }
    maximum = minimum;
    if (parser->offset < parser->pattern_length &&
        parser->pattern[parser->offset] == (uint8_t)',') {
        parser->offset++;
        if (!parse_decimal(parser, &maximum, quantifier_offset)) {
            return false;
        }
    }
    if (parser->offset == parser->pattern_length ||
        parser->pattern[parser->offset] != (uint8_t)'}' ||
        minimum > maximum) {
        set_diagnostic(
            parser,
            BOXRIG_TEXT_REGEX_INVALID_QUANTIFIER,
            quantifier_offset);
        return false;
    }
    parser->offset++;
    if (maximum > parser->limits->quantifier_capacity ||
        maximum > BOXRIG_TEXT_RECOGNIZER_QUANTIFIER_CAPACITY_MAX) {
        set_diagnostic(
            parser,
            BOXRIG_TEXT_REGEX_QUANTIFIER_LIMIT,
            quantifier_offset);
        return false;
    }
    memset(token, 0, sizeof(*token));
    token->type = BOXRIG_REGEX_TOKEN_REPEAT;
    token->pattern_offset = quantifier_offset;
    token->repeat_minimum = minimum;
    token->repeat_maximum = maximum;
    return true;
}

static bool append_atom_token(
    boxrig_regex_parser *parser,
    boxrig_regex_token *token)
{
    size_t atom_offset = parser->offset;
    uint8_t byte = parser->pattern[parser->offset];

    memset(token, 0, sizeof(*token));
    token->pattern_offset = atom_offset;
    if (byte == (uint8_t)'.') {
        token->type = BOXRIG_REGEX_TOKEN_DOT;
        parser->offset++;
    } else if (byte == (uint8_t)'^') {
        token->type = BOXRIG_REGEX_TOKEN_BEGIN;
        parser->offset++;
    } else if (byte == (uint8_t)'$') {
        token->type = BOXRIG_REGEX_TOKEN_END;
        parser->offset++;
    } else if (byte == (uint8_t)'[') {
        token->type = BOXRIG_REGEX_TOKEN_CLASS;
        if (!parse_class(parser, atom_offset, &token->class_index)) {
            return false;
        }
    } else if (byte == (uint8_t)'\\') {
        size_t escape_kind;

        if (!escaped_scalar(parser, &token->scalar, &escape_kind)) {
            return false;
        }
        if (escape_kind == 0u) {
            token->type = BOXRIG_REGEX_TOKEN_LITERAL;
        } else {
            boxrig_regex_class *class_value;

            if (parser->class_count == parser->class_capacity) {
                set_diagnostic(
                    parser,
                    BOXRIG_TEXT_REGEX_STATE_LIMIT,
                    atom_offset);
                return false;
            }
            class_value = &parser->classes[parser->class_count];
            class_value->range_start = parser->range_count;
            class_value->negated = false;
            if (!append_ascii_class_ranges(parser, escape_kind)) {
                return false;
            }
            class_value->range_count =
                parser->range_count - class_value->range_start;
            token->type = BOXRIG_REGEX_TOKEN_CLASS;
            token->class_index = parser->class_count++;
        }
    } else {
        size_t length;

        if (byte < UINT8_C(0x80) &&
            strchr("|()*+?{}]", (int)byte) != NULL) {
            set_diagnostic(
                parser,
                BOXRIG_TEXT_REGEX_UNSUPPORTED_SYNTAX,
                atom_offset);
            return false;
        }
        if (!decode_scalar(parser, &token->scalar, &length)) {
            return false;
        }
        token->type = BOXRIG_REGEX_TOKEN_LITERAL;
        parser->offset += length;
    }
    return true;
}

static bool parse_to_postfix(boxrig_regex_parser *parser)
{
    bool previous_atom = false;

    while (parser->offset < parser->pattern_length) {
        size_t token_offset = parser->offset;
        uint8_t byte = parser->pattern[parser->offset];

        if (byte == (uint8_t)'(') {
            boxrig_regex_operator_entry entry;

            if (parser->offset + 1u < parser->pattern_length &&
                parser->pattern[parser->offset + 1u] == (uint8_t)'?') {
                set_diagnostic(
                    parser,
                    BOXRIG_TEXT_REGEX_UNSUPPORTED_SYNTAX,
                    parser->offset + 1u);
                return false;
            }
            if (previous_atom &&
                !push_operator(
                    parser,
                    BOXRIG_REGEX_OPERATOR_CONCAT,
                    token_offset)) {
                return false;
            }
            if (parser->operator_count == parser->operator_capacity) {
                set_diagnostic(
                    parser,
                    BOXRIG_TEXT_REGEX_STATE_LIMIT,
                    token_offset);
                return false;
            }
            entry.operator_kind = BOXRIG_REGEX_OPERATOR_GROUP;
            entry.pattern_offset = token_offset;
            parser->operators[parser->operator_count++] = entry;
            parser->offset++;
            previous_atom = false;
        } else if (byte == (uint8_t)')') {
            bool found_group = false;

            if (!previous_atom) {
                set_diagnostic(
                    parser,
                    BOXRIG_TEXT_REGEX_UNBALANCED_DELIMITER,
                    token_offset);
                return false;
            }
            while (parser->operator_count != 0u) {
                boxrig_regex_operator_entry entry =
                    parser->operators[--parser->operator_count];

                if (entry.operator_kind == BOXRIG_REGEX_OPERATOR_GROUP) {
                    found_group = true;
                    break;
                }
                if (!emit_operator(parser, entry)) {
                    return false;
                }
            }
            if (!found_group) {
                set_diagnostic(
                    parser,
                    BOXRIG_TEXT_REGEX_UNBALANCED_DELIMITER,
                    token_offset);
                return false;
            }
            parser->offset++;
            previous_atom = true;
        } else if (byte == (uint8_t)'|') {
            if (!previous_atom ||
                !push_operator(
                    parser,
                    BOXRIG_REGEX_OPERATOR_ALTERNATE,
                    token_offset)) {
                if (parser->diagnostic->code ==
                    BOXRIG_TEXT_REGEX_DIAGNOSTIC_NONE) {
                    set_diagnostic(
                        parser,
                        BOXRIG_TEXT_REGEX_UNSUPPORTED_SYNTAX,
                        token_offset);
                }
                return false;
            }
            parser->offset++;
            previous_atom = false;
        } else if (byte == (uint8_t)'?' || byte == (uint8_t)'*' ||
                   byte == (uint8_t)'+' || byte == (uint8_t)'{') {
            boxrig_regex_token token;

            if (!previous_atom) {
                set_diagnostic(
                    parser,
                    byte == (uint8_t)'{'
                        ? BOXRIG_TEXT_REGEX_INVALID_QUANTIFIER
                        : BOXRIG_TEXT_REGEX_UNSUPPORTED_SYNTAX,
                    token_offset);
                return false;
            }
            memset(&token, 0, sizeof(token));
            token.pattern_offset = token_offset;
            if (byte == (uint8_t)'?') {
                token.type = BOXRIG_REGEX_TOKEN_OPTIONAL;
                parser->offset++;
            } else if (byte == (uint8_t)'*') {
                token.type = BOXRIG_REGEX_TOKEN_STAR;
                parser->offset++;
            } else if (byte == (uint8_t)'+') {
                token.type = BOXRIG_REGEX_TOKEN_PLUS;
                parser->offset++;
            } else if (!parse_repeat(parser, &token)) {
                return false;
            }
            if (!append_postfix(parser, token)) {
                return false;
            }
        } else {
            boxrig_regex_token token;

            if (previous_atom &&
                !push_operator(
                    parser,
                    BOXRIG_REGEX_OPERATOR_CONCAT,
                    token_offset)) {
                return false;
            }
            if (!append_atom_token(parser, &token) ||
                !append_postfix(parser, token)) {
                return false;
            }
            previous_atom = true;
        }
    }
    if (!previous_atom) {
        set_diagnostic(
            parser,
            parser->operator_count == 0u
                ? BOXRIG_TEXT_REGEX_NULLABLE
                : BOXRIG_TEXT_REGEX_UNBALANCED_DELIMITER,
            parser->pattern_length == 0u ? 0u : parser->pattern_length - 1u);
        return false;
    }
    while (parser->operator_count != 0u) {
        boxrig_regex_operator_entry entry =
            parser->operators[--parser->operator_count];

        if (entry.operator_kind == BOXRIG_REGEX_OPERATOR_GROUP) {
            set_diagnostic(
                parser,
                BOXRIG_TEXT_REGEX_UNBALANCED_DELIMITER,
                entry.pattern_offset);
            return false;
        }
        if (!emit_operator(parser, entry)) {
            return false;
        }
    }
    return true;
}

static bool append_node(
    boxrig_regex_parser *parser,
    boxrig_regex_node *nodes,
    size_t *node_count,
    boxrig_regex_node node,
    size_t *index_out)
{
    if (*node_count == parser->limits->state_capacity) {
        set_diagnostic(
            parser,
            BOXRIG_TEXT_REGEX_STATE_LIMIT,
            node.pattern_offset);
        return false;
    }
    nodes[*node_count] = node;
    *index_out = (*node_count)++;
    return true;
}

static bool clone_subtree(
    boxrig_regex_parser *parser,
    boxrig_regex_node *nodes,
    size_t *node_count,
    size_t root,
    size_t pattern_offset,
    size_t *clone_out)
{
    size_t source_start = nodes[root].first;
    size_t source_count = root - source_start + 1u;
    size_t destination_start = *node_count;
    size_t offset;

    if (source_count > parser->limits->state_capacity - *node_count) {
        set_diagnostic(
            parser,
            BOXRIG_TEXT_REGEX_STATE_LIMIT,
            pattern_offset);
        return false;
    }
    for (offset = 0u; offset < source_count; offset++) {
        boxrig_regex_node copy = nodes[source_start + offset];

        if (copy.left != SIZE_MAX) {
            copy.left = destination_start + copy.left - source_start;
        }
        if (copy.right != SIZE_MAX) {
            copy.right = destination_start + copy.right - source_start;
        }
        copy.first = destination_start + copy.first - source_start;
        nodes[destination_start + offset] = copy;
    }
    *node_count += source_count;
    *clone_out = destination_start + source_count - 1u;
    return true;
}

static bool append_unary_node(
    boxrig_regex_parser *parser,
    boxrig_regex_node *nodes,
    size_t *node_count,
    boxrig_regex_node_type type,
    size_t child,
    size_t pattern_offset,
    size_t *root_out)
{
    boxrig_regex_node node;

    memset(&node, 0, sizeof(node));
    node.type = type;
    node.left = child;
    node.right = SIZE_MAX;
    node.first = nodes[child].first;
    node.pattern_offset = pattern_offset;
    node.nullable = type == BOXRIG_REGEX_NODE_OPTIONAL ||
                    type == BOXRIG_REGEX_NODE_STAR || nodes[child].nullable;
    return append_node(parser, nodes, node_count, node, root_out);
}

static bool append_binary_node(
    boxrig_regex_parser *parser,
    boxrig_regex_node *nodes,
    size_t *node_count,
    boxrig_regex_node_type type,
    size_t left,
    size_t right,
    size_t pattern_offset,
    size_t *root_out)
{
    boxrig_regex_node node;

    memset(&node, 0, sizeof(node));
    node.type = type;
    node.left = left;
    node.right = right;
    node.first = nodes[left].first;
    node.pattern_offset = pattern_offset;
    node.nullable = type == BOXRIG_REGEX_NODE_CONCAT
                        ? nodes[left].nullable && nodes[right].nullable
                        : nodes[left].nullable || nodes[right].nullable;
    return append_node(parser, nodes, node_count, node, root_out);
}

static bool expand_repeat(
    boxrig_regex_parser *parser,
    boxrig_regex_node *nodes,
    size_t *node_count,
    size_t source,
    const boxrig_regex_token *token,
    size_t *root_out)
{
    size_t result = SIZE_MAX;
    size_t copy_index;
    size_t repetition;

    if (token->repeat_maximum == 0u) {
        boxrig_regex_node empty;

        memset(&empty, 0, sizeof(empty));
        empty.type = BOXRIG_REGEX_NODE_EMPTY;
        empty.left = SIZE_MAX;
        empty.right = SIZE_MAX;
        empty.first = *node_count;
        empty.pattern_offset = token->pattern_offset;
        empty.nullable = true;
        return append_node(parser, nodes, node_count, empty, root_out);
    }
    for (repetition = 0u; repetition < token->repeat_maximum; repetition++) {
        size_t current;

        if (repetition == 0u) {
            current = source;
        } else if (!clone_subtree(
                       parser,
                       nodes,
                       node_count,
                       source,
                       token->pattern_offset,
                       &current)) {
            return false;
        }
        if (repetition >= token->repeat_minimum &&
            !append_unary_node(
                parser,
                nodes,
                node_count,
                BOXRIG_REGEX_NODE_OPTIONAL,
                current,
                token->pattern_offset,
                &current)) {
            return false;
        }
        if (result == SIZE_MAX) {
            result = current;
        } else {
            copy_index = current;
            if (!append_binary_node(
                    parser,
                    nodes,
                    node_count,
                    BOXRIG_REGEX_NODE_CONCAT,
                    result,
                    copy_index,
                    token->pattern_offset,
                    &result)) {
                return false;
            }
        }
    }
    *root_out = result;
    return true;
}

static bool build_ast(
    boxrig_regex_parser *parser,
    boxrig_regex_node *nodes,
    size_t *node_count_out,
    size_t *root_out)
{
    size_t *stack = NULL;
    size_t stack_count = 0u;
    size_t node_count = 0u;
    size_t index;
    bool success = false;

    stack = calloc(parser->postfix_count + 1u, sizeof(*stack));
    if (stack == NULL) {
        return false;
    }
    for (index = 0u; index < parser->postfix_count; index++) {
        const boxrig_regex_token *token = &parser->postfix[index];
        boxrig_regex_node node;
        size_t root;

        memset(&node, 0, sizeof(node));
        node.left = SIZE_MAX;
        node.right = SIZE_MAX;
        node.first = node_count;
        node.pattern_offset = token->pattern_offset;
        node.scalar = token->scalar;
        node.class_index = token->class_index;
        if (token->type <= BOXRIG_REGEX_TOKEN_END) {
            node.type = (boxrig_regex_node_type)(
                BOXRIG_REGEX_NODE_LITERAL + token->type);
            node.nullable = token->type == BOXRIG_REGEX_TOKEN_BEGIN ||
                            token->type == BOXRIG_REGEX_TOKEN_END;
            if (!append_node(
                    parser,
                    nodes,
                    &node_count,
                    node,
                    &root)) {
                goto cleanup;
            }
            stack[stack_count++] = root;
        } else if (token->type == BOXRIG_REGEX_TOKEN_CONCAT ||
                   token->type == BOXRIG_REGEX_TOKEN_ALTERNATE) {
            size_t left;
            size_t right;

            if (stack_count < 2u) {
                set_diagnostic(
                    parser,
                    BOXRIG_TEXT_REGEX_UNSUPPORTED_SYNTAX,
                    token->pattern_offset);
                goto cleanup;
            }
            right = stack[--stack_count];
            left = stack[--stack_count];
            if (!append_binary_node(
                    parser,
                    nodes,
                    &node_count,
                    token->type == BOXRIG_REGEX_TOKEN_CONCAT
                        ? BOXRIG_REGEX_NODE_CONCAT
                        : BOXRIG_REGEX_NODE_ALTERNATE,
                    left,
                    right,
                    token->pattern_offset,
                    &root)) {
                goto cleanup;
            }
            stack[stack_count++] = root;
        } else {
            size_t child;

            if (stack_count == 0u) {
                set_diagnostic(
                    parser,
                    BOXRIG_TEXT_REGEX_UNSUPPORTED_SYNTAX,
                    token->pattern_offset);
                goto cleanup;
            }
            child = stack[--stack_count];
            if (token->type == BOXRIG_REGEX_TOKEN_REPEAT) {
                if (!expand_repeat(
                        parser,
                        nodes,
                        &node_count,
                        child,
                        token,
                        &root)) {
                    goto cleanup;
                }
            } else if (!append_unary_node(
                           parser,
                           nodes,
                           &node_count,
                           token->type == BOXRIG_REGEX_TOKEN_OPTIONAL
                               ? BOXRIG_REGEX_NODE_OPTIONAL
                               : token->type == BOXRIG_REGEX_TOKEN_STAR
                                     ? BOXRIG_REGEX_NODE_STAR
                                     : BOXRIG_REGEX_NODE_PLUS,
                           child,
                           token->pattern_offset,
                           &root)) {
                goto cleanup;
            }
            stack[stack_count++] = root;
        }
    }
    if (stack_count != 1u) {
        set_diagnostic(
            parser,
            BOXRIG_TEXT_REGEX_UNSUPPORTED_SYNTAX,
            parser->pattern_length == 0u ? 0u : parser->pattern_length - 1u);
        goto cleanup;
    }
    *root_out = stack[0];
    *node_count_out = node_count;
    success = true;

cleanup:
    free(stack);
    return success;
}

static bool add_state(
    boxrig_regex_parser *parser,
    boxrig_regex_program *program,
    boxrig_regex_state_type type,
    size_t pattern_offset,
    size_t *index_out)
{
    boxrig_regex_state *state;

    if (program->state_count == parser->limits->state_capacity) {
        set_diagnostic(
            parser,
            BOXRIG_TEXT_REGEX_STATE_LIMIT,
            pattern_offset);
        return false;
    }
    state = &program->states[program->state_count];
    memset(state, 0, sizeof(*state));
    state->type = type;
    state->out = SIZE_MAX;
    state->alternate_out = SIZE_MAX;
    *index_out = program->state_count++;
    return true;
}

static bool compile_ast(
    boxrig_regex_parser *parser,
    const boxrig_regex_node *nodes,
    size_t node_count,
    size_t root,
    boxrig_regex_program *program)
{
    boxrig_regex_fragment *fragments = NULL;
    size_t index;
    size_t accept;
    bool success = false;

    fragments = calloc(node_count, sizeof(*fragments));
    if (fragments == NULL) {
        return false;
    }
    for (index = 0u; index < node_count; index++) {
        const boxrig_regex_node *node = &nodes[index];
        boxrig_regex_fragment *fragment = &fragments[index];

        if (!add_state(
                parser,
                program,
                BOXRIG_REGEX_STATE_EPSILON,
                node->pattern_offset,
                &fragment->start) ||
            !add_state(
                parser,
                program,
                BOXRIG_REGEX_STATE_EPSILON,
                node->pattern_offset,
                &fragment->end)) {
            goto cleanup;
        }
        if (node->type == BOXRIG_REGEX_NODE_EMPTY) {
            program->states[fragment->start].out = fragment->end;
        } else if (node->type <= BOXRIG_REGEX_NODE_END) {
            size_t atom;
            boxrig_regex_state_type type = BOXRIG_REGEX_STATE_LITERAL;

            if (node->type == BOXRIG_REGEX_NODE_DOT) {
                type = BOXRIG_REGEX_STATE_DOT;
            } else if (node->type == BOXRIG_REGEX_NODE_CLASS) {
                type = BOXRIG_REGEX_STATE_CLASS;
            } else if (node->type == BOXRIG_REGEX_NODE_BEGIN) {
                type = BOXRIG_REGEX_STATE_ASSERT_BEGIN;
            } else if (node->type == BOXRIG_REGEX_NODE_END) {
                type = BOXRIG_REGEX_STATE_ASSERT_END;
            }
            if (!add_state(
                    parser,
                    program,
                    type,
                    node->pattern_offset,
                    &atom)) {
                goto cleanup;
            }
            program->states[fragment->start].out = atom;
            program->states[atom].out = fragment->end;
            program->states[atom].scalar = node->scalar;
            program->states[atom].class_index = node->class_index;
        } else if (node->type == BOXRIG_REGEX_NODE_CONCAT) {
            boxrig_regex_fragment left = fragments[node->left];
            boxrig_regex_fragment right = fragments[node->right];

            program->states[fragment->start].out = left.start;
            program->states[left.end].out = right.start;
            program->states[right.end].out = fragment->end;
        } else if (node->type == BOXRIG_REGEX_NODE_ALTERNATE) {
            boxrig_regex_fragment left = fragments[node->left];
            boxrig_regex_fragment right = fragments[node->right];

            program->states[fragment->start].type = BOXRIG_REGEX_STATE_SPLIT;
            program->states[fragment->start].out = left.start;
            program->states[fragment->start].alternate_out = right.start;
            program->states[left.end].out = fragment->end;
            program->states[right.end].out = fragment->end;
        } else {
            boxrig_regex_fragment child = fragments[node->left];

            if (node->type == BOXRIG_REGEX_NODE_OPTIONAL) {
                program->states[fragment->start].type =
                    BOXRIG_REGEX_STATE_SPLIT;
                program->states[fragment->start].out = child.start;
                program->states[fragment->start].alternate_out = fragment->end;
                program->states[child.end].out = fragment->end;
            } else if (node->type == BOXRIG_REGEX_NODE_STAR) {
                program->states[fragment->start].type =
                    BOXRIG_REGEX_STATE_SPLIT;
                program->states[fragment->start].out = child.start;
                program->states[fragment->start].alternate_out = fragment->end;
                program->states[child.end].type = BOXRIG_REGEX_STATE_SPLIT;
                program->states[child.end].out = child.start;
                program->states[child.end].alternate_out = fragment->end;
            } else {
                program->states[fragment->start].out = child.start;
                program->states[child.end].type = BOXRIG_REGEX_STATE_SPLIT;
                program->states[child.end].out = child.start;
                program->states[child.end].alternate_out = fragment->end;
            }
        }
    }
    if (!add_state(
            parser,
            program,
            BOXRIG_REGEX_STATE_ACCEPT,
            nodes[root].pattern_offset,
            &accept)) {
        goto cleanup;
    }
    program->states[fragments[root].end].out = accept;
    program->start_state = fragments[root].start;
    success = true;

cleanup:
    free(fragments);
    return success;
}

static bool limits_valid(const boxrig_text_recognizer_limits *limits)
{
    return limits != NULL && limits->state_capacity != 0u &&
           limits->state_capacity <= BOXRIG_TEXT_RECOGNIZER_STATE_CAPACITY_MAX &&
           limits->quantifier_capacity != 0u &&
           limits->quantifier_capacity <=
               BOXRIG_TEXT_RECOGNIZER_QUANTIFIER_CAPACITY_MAX &&
           limits->line_byte_capacity != 0u &&
           limits->line_column_capacity != 0u &&
           limits->line_match_capacity != 0u &&
           limits->result_capacity != 0u &&
           limits->dirty_line_capacity != 0u &&
           limits->snapshot_byte_capacity != 0u &&
           limits->snapshot_line_capacity != 0u;
}

boxrig_status boxrig_regex_compile(
    const uint8_t *pattern,
    size_t pattern_length,
    const boxrig_text_recognizer_limits *limits,
    boxrig_text_regex_diagnostic *diagnostic_out,
    boxrig_regex_program **program_out)
{
    boxrig_regex_parser parser;
    boxrig_regex_node *nodes = NULL;
    boxrig_regex_program *program = NULL;
    size_t root = 0u;
    size_t node_count = 0u;
    size_t token_capacity;
    size_t range_capacity;
    boxrig_status status = BOXRIG_STATUS_OUT_OF_MEMORY;

    if (diagnostic_out == NULL || program_out == NULL) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    diagnostic_out->code = BOXRIG_TEXT_REGEX_DIAGNOSTIC_NONE;
    diagnostic_out->pattern_offset = 0u;
    *program_out = NULL;
    if ((pattern == NULL && pattern_length != 0u) || !limits_valid(limits)) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    if (pattern_length > BOXRIG_TEXT_RECOGNIZER_PATTERN_CAPACITY_MAX) {
        diagnostic_out->code = BOXRIG_TEXT_REGEX_PATTERN_LIMIT;
        diagnostic_out->pattern_offset =
            BOXRIG_TEXT_RECOGNIZER_PATTERN_CAPACITY_MAX;
        return BOXRIG_STATUS_LIMIT_EXCEEDED;
    }
    if (pattern_length > (SIZE_MAX - 1u) / 2u ||
        pattern_length > (SIZE_MAX - 8u) / 4u) {
        diagnostic_out->code = BOXRIG_TEXT_REGEX_PATTERN_LIMIT;
        return BOXRIG_STATUS_LIMIT_EXCEEDED;
    }
    token_capacity = pattern_length * 2u + 1u;
    range_capacity = pattern_length * 4u + 8u;
    memset(&parser, 0, sizeof(parser));
    parser.pattern = pattern;
    parser.pattern_length = pattern_length;
    parser.limits = limits;
    parser.diagnostic = diagnostic_out;
    parser.postfix_capacity = token_capacity;
    parser.operator_capacity = pattern_length + 1u;
    parser.class_capacity = pattern_length + 1u;
    parser.range_capacity = range_capacity;
    parser.postfix = calloc(token_capacity, sizeof(*parser.postfix));
    parser.operators = calloc(
        parser.operator_capacity,
        sizeof(*parser.operators));
    parser.classes = calloc(parser.class_capacity, sizeof(*parser.classes));
    parser.ranges = calloc(parser.range_capacity, sizeof(*parser.ranges));
    nodes = calloc(limits->state_capacity, sizeof(*nodes));
    if (parser.postfix == NULL || parser.operators == NULL ||
        parser.classes == NULL || parser.ranges == NULL || nodes == NULL) {
        goto cleanup;
    }
    if (!parse_to_postfix(&parser) ||
        !build_ast(&parser, nodes, &node_count, &root)) {
        status = diagnostic_out->code == BOXRIG_TEXT_REGEX_STATE_LIMIT ||
                         diagnostic_out->code ==
                             BOXRIG_TEXT_REGEX_QUANTIFIER_LIMIT
                     ? BOXRIG_STATUS_LIMIT_EXCEEDED
                     : BOXRIG_STATUS_INVALID_ARGUMENT;
        goto cleanup;
    }
    if (nodes[root].nullable) {
        set_diagnostic(
            &parser,
            BOXRIG_TEXT_REGEX_NULLABLE,
            nodes[root].pattern_offset);
        status = BOXRIG_STATUS_INVALID_ARGUMENT;
        goto cleanup;
    }
    program = calloc(1u, sizeof(*program));
    if (program == NULL) {
        goto cleanup;
    }
    program->states = calloc(limits->state_capacity, sizeof(*program->states));
    if (program->states == NULL) {
        goto cleanup;
    }
    if (!compile_ast(&parser, nodes, node_count, root, program)) {
        status = diagnostic_out->code == BOXRIG_TEXT_REGEX_STATE_LIMIT
                     ? BOXRIG_STATUS_LIMIT_EXCEEDED
                     : BOXRIG_STATUS_OUT_OF_MEMORY;
        goto cleanup;
    }
    program->classes = parser.classes;
    program->class_count = parser.class_count;
    program->ranges = parser.ranges;
    program->range_count = parser.range_count;
    parser.classes = NULL;
    parser.ranges = NULL;
    diagnostic_out->code = BOXRIG_TEXT_REGEX_DIAGNOSTIC_NONE;
    diagnostic_out->pattern_offset = pattern_length;
    *program_out = program;
    program = NULL;
    status = BOXRIG_STATUS_OK;

cleanup:
    boxrig_regex_program_destroy(program);
    free(nodes);
    free(parser.postfix);
    free(parser.operators);
    free(parser.classes);
    free(parser.ranges);
    return status;
}

void boxrig_regex_program_destroy(boxrig_regex_program *program)
{
    if (program == NULL) {
        return;
    }
    free(program->states);
    free(program->classes);
    free(program->ranges);
    free(program);
}

size_t boxrig_regex_program_state_count(const boxrig_regex_program *program)
{
    return program == NULL ? 0u : program->state_count;
}

static void begin_generation(boxrig_regex_cursor *cursor)
{
    if (cursor->generation == UINT64_MAX) {
        memset(
            cursor->seen,
            0,
            cursor->program->state_count * sizeof(*cursor->seen));
        cursor->generation = 1u;
    } else {
        cursor->generation++;
    }
}

static void schedule_closure(
    boxrig_regex_cursor *cursor,
    size_t state_index)
{
    if (state_index == SIZE_MAX ||
        cursor->seen[state_index] == cursor->generation) {
        return;
    }
    cursor->seen[state_index] = cursor->generation;
    cursor->closure_stack[cursor->closure_count++] = state_index;
}

static void process_closure_state(boxrig_regex_cursor *cursor)
{
    size_t state_index = cursor->closure_stack[--cursor->closure_count];
    const boxrig_regex_state *state = &cursor->program->states[state_index];
    size_t closure_position = cursor->closure_target ==
                                      BOXRIG_REGEX_CLOSURE_CURRENT
                                  ? cursor->position
                                  : cursor->position + 1u;

    if (state->type == BOXRIG_REGEX_STATE_EPSILON) {
        schedule_closure(cursor, state->out);
    } else if (state->type == BOXRIG_REGEX_STATE_SPLIT) {
        schedule_closure(cursor, state->alternate_out);
        schedule_closure(cursor, state->out);
    } else if (state->type == BOXRIG_REGEX_STATE_ASSERT_BEGIN) {
        if (closure_position == 0u) {
            schedule_closure(cursor, state->out);
        }
    } else if (state->type == BOXRIG_REGEX_STATE_ASSERT_END) {
        if (closure_position == cursor->scalar_count) {
            schedule_closure(cursor, state->out);
        }
    } else if (state->type == BOXRIG_REGEX_STATE_ACCEPT) {
        if (cursor->closure_target == BOXRIG_REGEX_CLOSURE_CURRENT) {
            cursor->current_accept = true;
        } else {
            cursor->next_accept = true;
        }
    } else if (cursor->closure_target == BOXRIG_REGEX_CLOSURE_CURRENT) {
        cursor->current_states[cursor->current_count++] = state_index;
    } else {
        cursor->next_states[cursor->next_count++] = state_index;
    }
}

static bool state_matches(
    const boxrig_regex_program *program,
    const boxrig_regex_state *state,
    int32_t scalar)
{
    if (state->type == BOXRIG_REGEX_STATE_DOT) {
        return true;
    }
    if (state->type == BOXRIG_REGEX_STATE_LITERAL) {
        return state->scalar == scalar;
    }
    if (state->type == BOXRIG_REGEX_STATE_CLASS) {
        const boxrig_regex_class *class_value =
            &program->classes[state->class_index];
        bool contained = false;
        size_t index;

        for (index = 0u; index < class_value->range_count; index++) {
            const boxrig_regex_range *range =
                &program->ranges[class_value->range_start + index];

            if (scalar >= range->first && scalar <= range->last) {
                contained = true;
                break;
            }
        }
        return class_value->negated ? !contained : contained;
    }
    return false;
}

boxrig_status boxrig_regex_cursor_create(
    const boxrig_regex_program *program,
    const int32_t *scalars,
    const uint8_t *grapheme_boundaries,
    size_t scalar_count,
    size_t start,
    boxrig_regex_cursor **cursor_out)
{
    boxrig_regex_cursor *cursor;

    if (program == NULL || cursor_out == NULL || start > scalar_count ||
        (scalar_count != 0u &&
         (scalars == NULL || grapheme_boundaries == NULL))) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    *cursor_out = NULL;
    cursor = calloc(1u, sizeof(*cursor));
    if (cursor == NULL) {
        return BOXRIG_STATUS_OUT_OF_MEMORY;
    }
    cursor->current_states = calloc(
        program->state_count,
        sizeof(*cursor->current_states));
    cursor->next_states = calloc(
        program->state_count,
        sizeof(*cursor->next_states));
    cursor->closure_stack = calloc(
        program->state_count,
        sizeof(*cursor->closure_stack));
    cursor->seen = calloc(program->state_count, sizeof(*cursor->seen));
    if (cursor->current_states == NULL || cursor->next_states == NULL ||
        cursor->closure_stack == NULL || cursor->seen == NULL) {
        boxrig_regex_cursor_destroy(cursor);
        return BOXRIG_STATUS_OUT_OF_MEMORY;
    }
    cursor->program = program;
    (void)boxrig_regex_cursor_reset(
        cursor,
        scalars,
        grapheme_boundaries,
        scalar_count,
        start);
    *cursor_out = cursor;
    return BOXRIG_STATUS_OK;
}

boxrig_status boxrig_regex_cursor_reset(
    boxrig_regex_cursor *cursor,
    const int32_t *scalars,
    const uint8_t *grapheme_boundaries,
    size_t scalar_count,
    size_t start)
{
    if (cursor == NULL || start > scalar_count ||
        (scalar_count != 0u &&
         (scalars == NULL || grapheme_boundaries == NULL))) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    cursor->scalars = scalars;
    cursor->boundaries = grapheme_boundaries;
    cursor->scalar_count = scalar_count;
    cursor->candidate = start;
    cursor->position = 0u;
    cursor->longest_end = SIZE_MAX;
    cursor->current_count = 0u;
    cursor->next_count = 0u;
    cursor->transition_index = 0u;
    cursor->closure_count = 0u;
    cursor->phase = BOXRIG_REGEX_CURSOR_CANDIDATE;
    cursor->closure_target = BOXRIG_REGEX_CLOSURE_CURRENT;
    cursor->current_accept = false;
    cursor->next_accept = false;
    cursor->next_prepared = false;
    return BOXRIG_STATUS_OK;
}

void boxrig_regex_cursor_destroy(boxrig_regex_cursor *cursor)
{
    if (cursor == NULL) {
        return;
    }
    free(cursor->current_states);
    free(cursor->next_states);
    free(cursor->closure_stack);
    free(cursor->seen);
    free(cursor);
}

static void clear_match(boxrig_regex_match *match)
{
    match->start = 0u;
    match->end = 0u;
}

boxrig_status boxrig_regex_cursor_resume(
    boxrig_regex_cursor *cursor,
    size_t work_budget,
    size_t *work_used_out,
    boxrig_regex_progress *progress_out,
    boxrig_regex_match *match_out)
{
    size_t work_used = 0u;

    if (cursor == NULL || work_budget == 0u || work_used_out == NULL ||
        progress_out == NULL || match_out == NULL ||
        cursor->phase == BOXRIG_REGEX_CURSOR_COMPLETE) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    *work_used_out = 0u;
    *progress_out = BOXRIG_REGEX_IN_PROGRESS;
    clear_match(match_out);
    while (work_used < work_budget) {
        if (cursor->phase == BOXRIG_REGEX_CURSOR_CANDIDATE) {
            work_used++;
            if (cursor->candidate > cursor->scalar_count) {
                cursor->phase = BOXRIG_REGEX_CURSOR_COMPLETE;
                *progress_out = BOXRIG_REGEX_NOT_FOUND;
                break;
            }
            if (!cursor->boundaries[cursor->candidate]) {
                cursor->candidate++;
                continue;
            }
            cursor->position = cursor->candidate;
            cursor->longest_end = SIZE_MAX;
            cursor->current_count = 0u;
            cursor->next_count = 0u;
            cursor->closure_count = 0u;
            cursor->current_accept = false;
            cursor->next_accept = false;
            cursor->next_prepared = false;
            cursor->closure_target = BOXRIG_REGEX_CLOSURE_CURRENT;
            begin_generation(cursor);
            schedule_closure(cursor, cursor->program->start_state);
            cursor->phase = BOXRIG_REGEX_CURSOR_START_CLOSURE;
        } else if (cursor->phase == BOXRIG_REGEX_CURSOR_START_CLOSURE ||
                   cursor->phase == BOXRIG_REGEX_CURSOR_OUTPUT_CLOSURE) {
            if (cursor->closure_count != 0u) {
                process_closure_state(cursor);
                work_used++;
            } else if (cursor->phase ==
                       BOXRIG_REGEX_CURSOR_START_CLOSURE) {
                if (cursor->current_accept &&
                    cursor->boundaries[cursor->position]) {
                    cursor->longest_end = cursor->position;
                }
                cursor->transition_index = 0u;
                cursor->phase = BOXRIG_REGEX_CURSOR_TRANSITION;
            } else {
                cursor->phase = BOXRIG_REGEX_CURSOR_TRANSITION;
            }
        } else if (cursor->phase == BOXRIG_REGEX_CURSOR_TRANSITION) {
            if (cursor->position == cursor->scalar_count ||
                cursor->current_count == 0u) {
                if (cursor->longest_end != SIZE_MAX) {
                    match_out->start = cursor->candidate;
                    match_out->end = cursor->longest_end;
                    cursor->phase = BOXRIG_REGEX_CURSOR_COMPLETE;
                    *progress_out = BOXRIG_REGEX_MATCH;
                    break;
                }
                cursor->candidate++;
                cursor->phase = BOXRIG_REGEX_CURSOR_CANDIDATE;
                continue;
            }
            if (!cursor->next_prepared) {
                cursor->next_count = 0u;
                cursor->next_accept = false;
                cursor->closure_count = 0u;
                begin_generation(cursor);
                cursor->next_prepared = true;
            }
            if (cursor->transition_index < cursor->current_count) {
                const boxrig_regex_state *state =
                    &cursor->program->states[
                        cursor->current_states[cursor->transition_index++]];

                if (state_matches(
                        cursor->program,
                        state,
                        cursor->scalars[cursor->position])) {
                    cursor->closure_target = BOXRIG_REGEX_CLOSURE_NEXT;
                    schedule_closure(cursor, state->out);
                    cursor->phase = BOXRIG_REGEX_CURSOR_OUTPUT_CLOSURE;
                }
                work_used++;
            } else {
                size_t *state_swap = cursor->current_states;

                cursor->current_states = cursor->next_states;
                cursor->next_states = state_swap;
                cursor->current_count = cursor->next_count;
                cursor->current_accept = cursor->next_accept;
                cursor->position++;
                if (cursor->current_accept &&
                    cursor->boundaries[cursor->position]) {
                    cursor->longest_end = cursor->position;
                }
                cursor->transition_index = 0u;
                cursor->next_prepared = false;
            }
        }
    }
    *work_used_out = work_used;
    return BOXRIG_STATUS_OK;
}
