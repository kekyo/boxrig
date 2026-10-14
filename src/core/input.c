/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "terminal_internal.h"

#include "utf8proc.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint32_t all_modifiers =
    BOXRIG_MODIFIER_SHIFT | BOXRIG_MODIFIER_ALT |
    BOXRIG_MODIFIER_CONTROL | BOXRIG_MODIFIER_SUPER |
    BOXRIG_MODIFIER_HYPER | BOXRIG_MODIFIER_META |
    BOXRIG_MODIFIER_CAPS_LOCK | BOXRIG_MODIFIER_NUM_LOCK;

boxrig_input_config boxrig_input_config_default(void)
{
    boxrig_input_config config;
    config.enter_newline = BOXRIG_NEWLINE_TERMINAL;
    config.backspace_binding = BOXRIG_ERASE_AUTO;
    config.delete_binding = BOXRIG_ERASE_AUTO;
    config.key_mappings = NULL;
    config.key_mapping_count = 0u;
    return config;
}

static boxrig_status copy_key_mappings(
    const boxrig_input_config *config, boxrig_key_mapping **mappings_out)
{
    *mappings_out = NULL;
    if (config->key_mapping_count > BOXRIG_KEY_MAPPING_COUNT_MAX)
        return BOXRIG_STATUS_LIMIT_EXCEEDED;
    if (config->key_mapping_count == 0) return BOXRIG_STATUS_OK;
    if (config->key_mappings == NULL) return BOXRIG_STATUS_INVALID_ARGUMENT;
    const uint32_t modifiers = all_modifiers &
        ~(BOXRIG_MODIFIER_CAPS_LOCK | BOXRIG_MODIFIER_NUM_LOCK);
    size_t byte_count = 0;
    for (size_t index = 0; index < config->key_mapping_count; index++) {
        const boxrig_key_mapping *mapping = &config->key_mappings[index];
        if (mapping->key < BOXRIG_KEY_UP || mapping->key > BOXRIG_KEY_LEFT ||
            (mapping->modifiers & ~modifiers) != 0 ||
            (mapping->bytes == NULL && mapping->length != 0))
            return BOXRIG_STATUS_INVALID_ARGUMENT;
        if (mapping->length > BOXRIG_KEY_MAPPING_BYTES_MAX ||
            mapping->length > BOXRIG_KEY_MAPPING_TOTAL_BYTES_MAX - byte_count)
            return BOXRIG_STATUS_LIMIT_EXCEEDED;
        byte_count += mapping->length;
        for (size_t previous = 0; previous < index; previous++) {
            const boxrig_key_mapping *other = &config->key_mappings[previous];
            if (mapping->key == other->key && mapping->modifiers == other->modifiers)
                return BOXRIG_STATUS_INVALID_ARGUMENT;
        }
    }
    /* The public hard bounds keep this sum below 68 KiB on both 32- and
     * 64-bit targets. Byte payloads follow the naturally aligned entries. */
    size_t table_size = config->key_mapping_count * sizeof(boxrig_key_mapping);
    boxrig_key_mapping *owned = malloc(table_size + byte_count);
    if (owned == NULL) return BOXRIG_STATUS_OUT_OF_MEMORY;
    uint8_t *bytes = (uint8_t *)owned + table_size;
    for (size_t index = 0; index < config->key_mapping_count; index++) {
        owned[index] = config->key_mappings[index];
        owned[index].bytes = owned[index].length == 0 ? NULL : bytes;
        if (owned[index].length != 0) {
            memcpy(bytes, config->key_mappings[index].bytes, owned[index].length);
            bytes += owned[index].length;
        }
    }
    *mappings_out = owned;
    return BOXRIG_STATUS_OK;
}

boxrig_status boxrig_terminal_set_input_config(
    boxrig_terminal *terminal, const boxrig_input_config *config)
{
    if (terminal == NULL || config == NULL ||
        (unsigned int)config->enter_newline > BOXRIG_NEWLINE_TERMINAL ||
        (unsigned int)config->backspace_binding > BOXRIG_ERASE_DELETE_SEQUENCE ||
        (unsigned int)config->delete_binding > BOXRIG_ERASE_DELETE_SEQUENCE) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    boxrig_input_config candidate = *config;
    boxrig_key_mapping *mappings;
    boxrig_status status = copy_key_mappings(config, &mappings);
    if (status != BOXRIG_STATUS_OK) {
        return status;
    }
    candidate.key_mappings = mappings;
    free((void *)terminal->input_config.key_mappings);
    terminal->input_config = candidate;
    return BOXRIG_STATUS_OK;
}

boxrig_status boxrig_terminal_get_input_config(
    const boxrig_terminal *terminal, boxrig_input_config *config_out)
{
    if (terminal == NULL || config_out == NULL) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    *config_out = terminal->input_config;
    return BOXRIG_STATUS_OK;
}

static const uint32_t all_mouse_button_masks =
    BOXRIG_MOUSE_BUTTON_MASK_LEFT | BOXRIG_MOUSE_BUTTON_MASK_MIDDLE |
    BOXRIG_MOUSE_BUTTON_MASK_RIGHT | BOXRIG_MOUSE_BUTTON_MASK_8 |
    BOXRIG_MOUSE_BUTTON_MASK_9 | BOXRIG_MOUSE_BUTTON_MASK_10 |
    BOXRIG_MOUSE_BUTTON_MASK_11;

typedef struct boxrig_key_sequence_builder {
    uint8_t *bytes;
    size_t length;
    size_t capacity;
    size_t limit;
} boxrig_key_sequence_builder;

static boxrig_status key_sequence_initialize(
    boxrig_key_sequence_builder *builder,
    size_t limit)
{
    size_t initial_capacity = limit < 64 ? limit : 64;

    builder->bytes = malloc(initial_capacity);
    if (builder->bytes == NULL) {
        return BOXRIG_STATUS_OUT_OF_MEMORY;
    }
    builder->length = 0;
    builder->capacity = initial_capacity;
    builder->limit = limit;
    return BOXRIG_STATUS_OK;
}

static boxrig_status key_sequence_reserve(
    boxrig_key_sequence_builder *builder,
    size_t additional)
{
    size_t required;
    size_t capacity;
    uint8_t *replacement;

    if (additional > builder->limit - builder->length) {
        return BOXRIG_STATUS_LIMIT_EXCEEDED;
    }
    required = builder->length + additional;
    if (required <= builder->capacity) {
        return BOXRIG_STATUS_OK;
    }
    capacity = builder->capacity;
    while (capacity < required) {
        if (capacity > builder->limit / 2) {
            capacity = builder->limit;
        } else {
            capacity *= 2;
        }
    }
    replacement = realloc(builder->bytes, capacity);
    if (replacement == NULL) {
        return BOXRIG_STATUS_OUT_OF_MEMORY;
    }
    builder->bytes = replacement;
    builder->capacity = capacity;
    return BOXRIG_STATUS_OK;
}

static boxrig_status key_sequence_append(
    boxrig_key_sequence_builder *builder,
    const void *bytes,
    size_t length)
{
    boxrig_status status = key_sequence_reserve(builder, length);

    if (status != BOXRIG_STATUS_OK) {
        return status;
    }
    memcpy(builder->bytes + builder->length, bytes, length);
    builder->length += length;
    return BOXRIG_STATUS_OK;
}

static boxrig_status key_sequence_append_byte(
    boxrig_key_sequence_builder *builder,
    uint8_t byte)
{
    return key_sequence_append(builder, &byte, 1);
}

static boxrig_status key_sequence_append_number(
    boxrig_key_sequence_builder *builder,
    uint32_t number)
{
    char digits[16];
    int length = snprintf(digits, sizeof(digits), "%" PRIu32, number);

    if (length < 0 || (size_t)length >= sizeof(digits)) {
        return BOXRIG_STATUS_LIMIT_EXCEEDED;
    }
    return key_sequence_append(builder, digits, (size_t)length);
}

static boxrig_kitty_keyboard_mode *kitty_keyboard_mode(
    boxrig_input_state *input,
    bool alternate_screen)
{
    return alternate_screen ? &input->alternate_keyboard
                            : &input->primary_keyboard;
}

static const boxrig_kitty_keyboard_mode *const_kitty_keyboard_mode(
    const boxrig_input_state *input,
    bool alternate_screen)
{
    return alternate_screen ? &input->alternate_keyboard
                            : &input->primary_keyboard;
}

void boxrig_input_reset(boxrig_input_state *input)
{
    bool paste_active = input->paste_active;
    bool paste_bracketed = input->paste_bracketed;

    memset(input, 0, sizeof(*input));
    input->paste_active = paste_active;
    input->paste_bracketed = paste_bracketed;
}

void boxrig_input_soft_reset(boxrig_input_state *input)
{
    input->application_cursor = false;
    input->application_keypad = false;
}

void boxrig_input_dispatch_escape(
    boxrig_input_state *input,
    const uint8_t *intermediates,
    size_t intermediate_count,
    uint8_t final_byte)
{
    if (intermediate_count != 0) {
        (void)intermediates;
        return;
    }
    if (final_byte == (uint8_t)'=') {
        input->application_keypad = true;
    } else if (final_byte == (uint8_t)'>') {
        input->application_keypad = false;
    } else if (final_byte == (uint8_t)'c') {
        boxrig_input_reset(input);
    }
}

void boxrig_input_set_private_modes(
    boxrig_input_state *input,
    const boxrig_parser_parameter *parameters,
    size_t parameter_count,
    bool enabled)
{
    size_t index;

    for (index = 0; index < parameter_count; index++) {
        if (parameters[index].subparameter || !parameters[index].has_value) {
            continue;
        }
        if (parameters[index].value == 1) {
            input->application_cursor = enabled;
        } else if (parameters[index].value == 66) {
            input->application_keypad = enabled;
        } else if (parameters[index].value == 9) {
            if (enabled) {
                input->mouse_tracking = BOXRIG_MOUSE_TRACKING_X10;
            } else if (input->mouse_tracking == BOXRIG_MOUSE_TRACKING_X10) {
                input->mouse_tracking = BOXRIG_MOUSE_TRACKING_NONE;
            }
        } else if (parameters[index].value == 1000) {
            if (enabled) {
                input->mouse_tracking = BOXRIG_MOUSE_TRACKING_VT200;
            } else if (input->mouse_tracking == BOXRIG_MOUSE_TRACKING_VT200) {
                input->mouse_tracking = BOXRIG_MOUSE_TRACKING_NONE;
            }
        } else if (parameters[index].value == 1002) {
            if (enabled) {
                input->mouse_tracking = BOXRIG_MOUSE_TRACKING_BUTTON_EVENT;
            } else if (input->mouse_tracking ==
                       BOXRIG_MOUSE_TRACKING_BUTTON_EVENT) {
                input->mouse_tracking = BOXRIG_MOUSE_TRACKING_NONE;
            }
        } else if (parameters[index].value == 1003) {
            if (enabled) {
                input->mouse_tracking = BOXRIG_MOUSE_TRACKING_ANY_EVENT;
            } else if (input->mouse_tracking ==
                       BOXRIG_MOUSE_TRACKING_ANY_EVENT) {
                input->mouse_tracking = BOXRIG_MOUSE_TRACKING_NONE;
            }
        } else if (parameters[index].value == 1004) {
            input->focus_reporting = enabled;
        } else if (parameters[index].value == 1006) {
            input->sgr_mouse = enabled;
            if (enabled) input->pixel_mouse = false;
        } else if (parameters[index].value == 1007) {
            input->alternate_scroll = enabled;
        } else if (parameters[index].value == 1016) {
            input->pixel_mouse = enabled;
            if (enabled) input->sgr_mouse = false;
        } else if (parameters[index].value == 1034) {
            input->eight_bit_meta = enabled;
        } else if (parameters[index].value == 2004) {
            input->bracketed_paste = enabled;
        }
    }
}

bool boxrig_input_query_mode(
    const boxrig_input_state *input,
    bool private_mode,
    uint32_t mode,
    bool *enabled_out)
{
    if (!private_mode || enabled_out == NULL) {
        return false;
    }
    if (mode == 1) {
        *enabled_out = input->application_cursor;
        return true;
    }
    if (mode == 1004) {
        *enabled_out = input->focus_reporting;
        return true;
    }
    if (mode == 66) {
        *enabled_out = input->application_keypad;
        return true;
    }
    if (mode == 9) {
        *enabled_out = input->mouse_tracking == BOXRIG_MOUSE_TRACKING_X10;
        return true;
    }
    if (mode == 1000) {
        *enabled_out = input->mouse_tracking == BOXRIG_MOUSE_TRACKING_VT200;
        return true;
    }
    if (mode == 1002) {
        *enabled_out = input->mouse_tracking ==
                       BOXRIG_MOUSE_TRACKING_BUTTON_EVENT;
        return true;
    }
    if (mode == 1003) {
        *enabled_out = input->mouse_tracking ==
                       BOXRIG_MOUSE_TRACKING_ANY_EVENT;
        return true;
    }
    if (mode == 1006) {
        *enabled_out = input->sgr_mouse;
        return true;
    }
    if (mode == 1007) {
        *enabled_out = input->alternate_scroll;
        return true;
    }
    if (mode == 1016) {
        *enabled_out = input->pixel_mouse;
        return true;
    }
    if (mode == 1034) {
        *enabled_out = input->eight_bit_meta;
        return true;
    }
    if (mode == 2004) {
        *enabled_out = input->bracketed_paste;
        return true;
    }
    return false;
}

uint32_t boxrig_input_kitty_flags(
    const boxrig_input_state *input,
    bool alternate_screen)
{
    return const_kitty_keyboard_mode(input, alternate_screen)->flags;
}

void boxrig_input_set_kitty_flags(
    boxrig_input_state *input,
    bool alternate_screen,
    uint32_t flags,
    uint32_t mode)
{
    boxrig_kitty_keyboard_mode *keyboard = kitty_keyboard_mode(
        input,
        alternate_screen);

    flags &= BOXRIG_KITTY_ALL_FLAGS;
    if (mode == 1) {
        keyboard->flags = flags;
    } else if (mode == 2) {
        keyboard->flags |= flags;
    } else if (mode == 3) {
        keyboard->flags &= ~flags;
    }
}

void boxrig_input_push_kitty_flags(
    boxrig_input_state *input,
    bool alternate_screen,
    uint32_t flags)
{
    boxrig_kitty_keyboard_mode *keyboard = kitty_keyboard_mode(
        input,
        alternate_screen);

    if (keyboard->stack_count == BOXRIG_KITTY_KEYBOARD_STACK_CAPACITY) {
        memmove(
            keyboard->stack,
            keyboard->stack + 1,
            (BOXRIG_KITTY_KEYBOARD_STACK_CAPACITY - 1) *
                sizeof(keyboard->stack[0]));
        keyboard->stack_count--;
    }
    keyboard->stack[keyboard->stack_count] = keyboard->flags;
    keyboard->stack_count++;
    keyboard->flags = flags & BOXRIG_KITTY_ALL_FLAGS;
}

void boxrig_input_pop_kitty_flags(
    boxrig_input_state *input,
    bool alternate_screen,
    uint32_t count)
{
    boxrig_kitty_keyboard_mode *keyboard = kitty_keyboard_mode(
        input,
        alternate_screen);

    if (count == 0) {
        return;
    }
    /* Kitty resets flags when a pop consumes every remaining stack entry. */
    if (count >= keyboard->stack_count) {
        keyboard->flags = 0;
        keyboard->stack_count = 0;
        return;
    }
    keyboard->flags = keyboard->stack[keyboard->stack_count - count];
    keyboard->stack_count -= count;
}

static unsigned int modifier_parameter(uint32_t modifiers)
{
    unsigned int parameter = 1;

    if ((modifiers & BOXRIG_MODIFIER_SHIFT) != 0) {
        parameter += 1;
    }
    if ((modifiers & BOXRIG_MODIFIER_ALT) != 0) {
        parameter += 2;
    }
    if ((modifiers & BOXRIG_MODIFIER_CONTROL) != 0) {
        parameter += 4;
    }
    if ((modifiers & BOXRIG_MODIFIER_SUPER) != 0) {
        parameter += 8;
    }
    if ((modifiers & BOXRIG_MODIFIER_HYPER) != 0) {
        parameter += 16;
    }
    if ((modifiers & BOXRIG_MODIFIER_META) != 0) {
        parameter += 32;
    }
    return parameter;
}

static boxrig_status append_text_key(
    boxrig_terminal *terminal,
    const boxrig_key_event *event)
{
    bool alt = (event->modifiers & BOXRIG_MODIFIER_ALT) != 0;
    bool eight_bit_meta = terminal->input.eight_bit_meta &&
                          (event->modifiers & BOXRIG_MODIFIER_META) != 0 &&
                          event->text != NULL &&
                          event->text_length == 1 &&
                          (uint8_t)event->text[0] < 0x80;
    return boxrig_terminal_append_encoded_text(terminal,
        (const uint8_t *)event->text, event->text_length,
        BOXRIG_NEWLINE_LF, alt, eight_bit_meta, true);
}

static bool legacy_control_codepoint(uint32_t codepoint, uint8_t *byte_out)
{
    if (codepoint >= 'A' && codepoint <= 'Z') {
        *byte_out = (uint8_t)(codepoint - 'A' + 1);
        return true;
    }
    if (codepoint >= 'a' && codepoint <= 'z') {
        *byte_out = (uint8_t)(codepoint - 'a' + 1);
        return true;
    }
    switch (codepoint) {
    case ' ':
    case '@':
    case '`':
    case '2':
        *byte_out = 0x00;
        return true;
    case '[':
    case '{':
    case '3':
        *byte_out = 0x1b;
        return true;
    case '\\':
    case '|':
    case '4':
        *byte_out = 0x1c;
        return true;
    case ']':
    case '}':
    case '5':
        *byte_out = 0x1d;
        return true;
    case '^':
    case '~':
    case '6':
        *byte_out = 0x1e;
        return true;
    case '_':
    case '/':
    case '-':
    case '7':
        *byte_out = 0x1f;
        return true;
    case '?':
    case '8':
        *byte_out = 0x7f;
        return true;
    default:
        return false;
    }
}

static bool has_at_most_one_key_text_codepoint(
    const boxrig_key_event *event)
{
    utf8proc_int32_t codepoint = -1;
    utf8proc_ssize_t decoded;

    if (event->text_length == 0) {
        return true;
    }
    decoded = utf8proc_iterate(
        (const utf8proc_uint8_t *)event->text,
        (utf8proc_ssize_t)event->text_length,
        &codepoint);
    return decoded > 0 && (size_t)decoded == event->text_length;
}

static bool legacy_control_byte(
    const boxrig_key_event *event,
    uint8_t *byte_out)
{
    uint32_t effective_codepoint;
    bool shifted;

    if (event->key != BOXRIG_KEY_NONE ||
        (event->action != BOXRIG_KEY_PRESS &&
         event->action != BOXRIG_KEY_REPEAT) ||
        (event->modifiers & BOXRIG_MODIFIER_CONTROL) == 0 ||
        !has_at_most_one_key_text_codepoint(event)) {
        return false;
    }
    shifted = (event->modifiers & BOXRIG_MODIFIER_SHIFT) != 0;
    effective_codepoint = shifted && event->shifted_codepoint != 0
                              ? event->shifted_codepoint
                              : event->codepoint;
    if (legacy_control_codepoint(effective_codepoint, byte_out)) {
        return true;
    }
    /* The base-layout field is unshifted. Applying it with Shift would turn
     * symbols such as # into the control assigned to their digit key. */
    return !shifted && event->base_layout_codepoint != 0 &&
           legacy_control_codepoint(
               event->base_layout_codepoint,
               byte_out);
}

static bool key_final(boxrig_key key, char *final_out)
{
    switch (key) {
    case BOXRIG_KEY_UP:
        *final_out = 'A';
        return true;
    case BOXRIG_KEY_DOWN:
        *final_out = 'B';
        return true;
    case BOXRIG_KEY_RIGHT:
        *final_out = 'C';
        return true;
    case BOXRIG_KEY_LEFT:
        *final_out = 'D';
        return true;
    case BOXRIG_KEY_HOME:
        *final_out = 'H';
        return true;
    case BOXRIG_KEY_END:
        *final_out = 'F';
        return true;
    case BOXRIG_KEY_F1:
        *final_out = 'P';
        return true;
    case BOXRIG_KEY_F2:
        *final_out = 'Q';
        return true;
    case BOXRIG_KEY_F3:
        *final_out = 'R';
        return true;
    case BOXRIG_KEY_F4:
        *final_out = 'S';
        return true;
    default:
        return false;
    }
}

static bool tilde_key_number(boxrig_key key, unsigned int *number_out)
{
    switch (key) {
    case BOXRIG_KEY_INSERT:
        *number_out = 2;
        return true;
    case BOXRIG_KEY_DELETE:
        *number_out = 3;
        return true;
    case BOXRIG_KEY_PAGE_UP:
        *number_out = 5;
        return true;
    case BOXRIG_KEY_PAGE_DOWN:
        *number_out = 6;
        return true;
    case BOXRIG_KEY_F5:
        *number_out = 15;
        return true;
    case BOXRIG_KEY_F6:
        *number_out = 17;
        return true;
    case BOXRIG_KEY_F7:
        *number_out = 18;
        return true;
    case BOXRIG_KEY_F8:
        *number_out = 19;
        return true;
    case BOXRIG_KEY_F9:
        *number_out = 20;
        return true;
    case BOXRIG_KEY_F10:
        *number_out = 21;
        return true;
    case BOXRIG_KEY_F11:
        *number_out = 23;
        return true;
    case BOXRIG_KEY_F12:
        *number_out = 24;
        return true;
    case BOXRIG_KEY_F13:
        *number_out = 25;
        return true;
    case BOXRIG_KEY_F14:
        *number_out = 26;
        return true;
    case BOXRIG_KEY_F15:
        *number_out = 28;
        return true;
    case BOXRIG_KEY_F16:
        *number_out = 29;
        return true;
    case BOXRIG_KEY_F17:
        *number_out = 31;
        return true;
    case BOXRIG_KEY_F18:
        *number_out = 32;
        return true;
    case BOXRIG_KEY_F19:
        *number_out = 33;
        return true;
    case BOXRIG_KEY_F20:
        *number_out = 34;
        return true;
    default:
        return false;
    }
}

static size_t enter_newline_bytes(
    const boxrig_terminal *terminal,
    uint8_t *bytes)
{
    boxrig_newline policy = terminal->input_config.enter_newline;

    if (policy == BOXRIG_NEWLINE_TERMINAL) {
        policy = terminal->screen.newline_mode
                     ? BOXRIG_NEWLINE_CRLF : BOXRIG_NEWLINE_CR;
    }
    bytes[0] = policy == BOXRIG_NEWLINE_LF ? '\n' : '\r';
    if (policy == BOXRIG_NEWLINE_CRLF) {
        bytes[1] = '\n';
        return 2;
    }
    return 1;
}

static boxrig_status append_simple_key(
    boxrig_terminal *terminal,
    const boxrig_key_event *event)
{
    uint8_t bytes[4];
    size_t base = 0;
    size_t length;
    bool alt = (event->modifiers & BOXRIG_MODIFIER_ALT) != 0;

    switch (event->key) {
    case BOXRIG_KEY_ENTER:
        length = enter_newline_bytes(terminal, bytes);
        break;
    case BOXRIG_KEY_TAB:
        if ((event->modifiers & BOXRIG_MODIFIER_SHIFT) != 0) {
            bytes[0] = 0x1b;
            bytes[1] = '[';
            bytes[2] = 'Z';
            length = 3;
        } else {
            bytes[0] = '\t';
            length = 1;
        }
        break;
    case BOXRIG_KEY_ESCAPE:
        bytes[0] = 0x1b;
        length = 1;
        break;
    default:
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    if (alt) {
        memmove(bytes + 1, bytes, length);
        bytes[0] = 0x1b;
        base = 1;
    }
    return boxrig_terminal_append_output(terminal, bytes, length + base);
}

static boxrig_status append_erase_key(
    boxrig_terminal *terminal, const boxrig_key_event *event)
{
    boxrig_erase_binding binding = event->key == BOXRIG_KEY_BACKSPACE
        ? terminal->input_config.backspace_binding
        : terminal->input_config.delete_binding;
    if (binding == BOXRIG_ERASE_AUTO) {
        binding = event->key == BOXRIG_KEY_BACKSPACE
            ? BOXRIG_ERASE_ASCII_DELETE : BOXRIG_ERASE_DELETE_SEQUENCE;
    }
    if (binding == BOXRIG_ERASE_DELETE_SEQUENCE) {
        unsigned int modifier = modifier_parameter(event->modifiers);
        char sequence[32];
        int length = modifier == 1u
            ? snprintf(sequence, sizeof(sequence), "\x1b[3~")
            : snprintf(sequence, sizeof(sequence), "\x1b[3;%u~", modifier);
        if (length < 0 || (size_t)length >= sizeof(sequence)) {
            return BOXRIG_STATUS_LIMIT_EXCEEDED;
        }
        return boxrig_terminal_append_output(terminal,
            (const uint8_t *)sequence, (size_t)length);
    }
    uint8_t bytes[2];
    size_t length = 0u;
    uint8_t byte = binding == BOXRIG_ERASE_ASCII_BACKSPACE ? 0x08u : 0x7fu;
    if ((event->modifiers & BOXRIG_MODIFIER_CONTROL) != 0u) {
        byte = byte == 0x08u ? 0x7fu : 0x08u;
    }
    if ((event->modifiers & BOXRIG_MODIFIER_ALT) != 0u) {
        bytes[length++] = 0x1bu;
    }
    bytes[length++] = byte;
    return boxrig_terminal_append_output(terminal, bytes, length);
}

static bool keypad_value(
    boxrig_key key,
    uint8_t *normal_out,
    char *application_final_out)
{
    if (key >= BOXRIG_KEY_KP_0 && key <= BOXRIG_KEY_KP_9) {
        unsigned int offset = (unsigned int)(key - BOXRIG_KEY_KP_0);

        *normal_out = (uint8_t)('0' + offset);
        *application_final_out = (char)('p' + offset);
        return true;
    }
    switch (key) {
    case BOXRIG_KEY_KP_DECIMAL:
        *normal_out = '.';
        *application_final_out = 'n';
        return true;
    case BOXRIG_KEY_KP_DIVIDE:
        *normal_out = '/';
        *application_final_out = 'o';
        return true;
    case BOXRIG_KEY_KP_MULTIPLY:
        *normal_out = '*';
        *application_final_out = 'j';
        return true;
    case BOXRIG_KEY_KP_SUBTRACT:
        *normal_out = '-';
        *application_final_out = 'm';
        return true;
    case BOXRIG_KEY_KP_ADD:
        *normal_out = '+';
        *application_final_out = 'k';
        return true;
    case BOXRIG_KEY_KP_ENTER:
        *normal_out = '\r';
        *application_final_out = 'M';
        return true;
    case BOXRIG_KEY_KP_EQUAL:
        *normal_out = '=';
        *application_final_out = 'X';
        return true;
    case BOXRIG_KEY_KP_SEPARATOR:
        *normal_out = ',';
        *application_final_out = 'l';
        return true;
    default:
        return false;
    }
}

static bool keypad_navigation_key(boxrig_key key, boxrig_key *mapped_out)
{
    switch (key) {
    case BOXRIG_KEY_KP_LEFT:
        *mapped_out = BOXRIG_KEY_LEFT;
        return true;
    case BOXRIG_KEY_KP_RIGHT:
        *mapped_out = BOXRIG_KEY_RIGHT;
        return true;
    case BOXRIG_KEY_KP_UP:
        *mapped_out = BOXRIG_KEY_UP;
        return true;
    case BOXRIG_KEY_KP_DOWN:
        *mapped_out = BOXRIG_KEY_DOWN;
        return true;
    case BOXRIG_KEY_KP_PAGE_UP:
        *mapped_out = BOXRIG_KEY_PAGE_UP;
        return true;
    case BOXRIG_KEY_KP_PAGE_DOWN:
        *mapped_out = BOXRIG_KEY_PAGE_DOWN;
        return true;
    case BOXRIG_KEY_KP_HOME:
        *mapped_out = BOXRIG_KEY_HOME;
        return true;
    case BOXRIG_KEY_KP_END:
        *mapped_out = BOXRIG_KEY_END;
        return true;
    case BOXRIG_KEY_KP_INSERT:
        *mapped_out = BOXRIG_KEY_INSERT;
        return true;
    case BOXRIG_KEY_KP_DELETE:
        *mapped_out = BOXRIG_KEY_DELETE;
        return true;
    default:
        return false;
    }
}

static boxrig_status append_keypad_key(
    boxrig_terminal *terminal,
    const boxrig_key_event *event,
    uint8_t normal,
    char application_final)
{
    unsigned int modifier = modifier_parameter(event->modifiers);
    char sequence[32];
    int length;

    if (event->key == BOXRIG_KEY_KP_ENTER &&
        !terminal->input.application_keypad) {
        boxrig_key_event normalized = *event;

        normalized.key = BOXRIG_KEY_ENTER;
        return append_simple_key(terminal, &normalized);
    }
    if (terminal->input.application_keypad) {
        if (modifier == 1) {
            sequence[0] = 0x1b;
            sequence[1] = 'O';
            sequence[2] = application_final;
            length = 3;
        } else {
            length = snprintf(
                sequence,
                sizeof(sequence),
                "\x1b[1;%u%c",
                modifier,
                application_final);
        }
    } else if ((event->modifiers & BOXRIG_MODIFIER_ALT) != 0) {
        sequence[0] = 0x1b;
        sequence[1] = (char)normal;
        length = 2;
    } else {
        sequence[0] = (char)normal;
        length = 1;
    }
    if (length < 0 || (size_t)length >= sizeof(sequence)) {
        return BOXRIG_STATUS_LIMIT_EXCEEDED;
    }
    return boxrig_terminal_append_output(
        terminal,
        (const uint8_t *)sequence,
        (size_t)length);
}

static bool extended_function_code(boxrig_key key, unsigned int *code_out)
{
    if (key < BOXRIG_KEY_F21 || key > BOXRIG_KEY_F35) {
        return false;
    }
    *code_out = 57384u + (unsigned int)(key - BOXRIG_KEY_F21);
    return true;
}

static bool valid_key_codepoint(uint32_t codepoint)
{
    return codepoint == 0 ||
           (codepoint <= 0x10ffff &&
            (codepoint < 0xd800 || codepoint > 0xdfff));
}

static boxrig_status validate_key_text(const boxrig_key_event *event)
{
    size_t offset = 0;

    if (event->text == NULL && event->text_length != 0) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    if (event->text_length > (size_t)PTRDIFF_MAX) {
        return BOXRIG_STATUS_LIMIT_EXCEEDED;
    }
    while (offset < event->text_length) {
        utf8proc_int32_t codepoint = -1;
        utf8proc_ssize_t decoded = utf8proc_iterate(
            (const utf8proc_uint8_t *)event->text + offset,
            (utf8proc_ssize_t)(event->text_length - offset),
            &codepoint);

        if (decoded <= 0) {
            return BOXRIG_STATUS_INVALID_ARGUMENT;
        }
        offset += (size_t)decoded;
    }
    return BOXRIG_STATUS_OK;
}

static bool associated_text_codepoint(int32_t codepoint)
{
    return codepoint >= 0x20 &&
           (codepoint < 0x7f || codepoint > 0x9f);
}

static bool has_associated_text(const boxrig_key_event *event)
{
    size_t offset = 0;

    while (offset < event->text_length) {
        utf8proc_int32_t codepoint = -1;
        utf8proc_ssize_t decoded = utf8proc_iterate(
            (const utf8proc_uint8_t *)event->text + offset,
            (utf8proc_ssize_t)(event->text_length - offset),
            &codepoint);

        if (associated_text_codepoint(codepoint)) {
            return true;
        }
        offset += (size_t)decoded;
    }
    return false;
}

static boxrig_status append_associated_text(
    boxrig_key_sequence_builder *builder,
    const boxrig_key_event *event)
{
    size_t offset = 0;
    bool first = true;

    while (offset < event->text_length) {
        utf8proc_int32_t codepoint = -1;
        utf8proc_ssize_t decoded = utf8proc_iterate(
            (const utf8proc_uint8_t *)event->text + offset,
            (utf8proc_ssize_t)(event->text_length - offset),
            &codepoint);
        boxrig_status status;

        offset += (size_t)decoded;
        if (!associated_text_codepoint(codepoint)) {
            continue;
        }
        if (!first) {
            status = key_sequence_append_byte(builder, (uint8_t)':');
            if (status != BOXRIG_STATUS_OK) {
                return status;
            }
        }
        status = key_sequence_append_number(builder, (uint32_t)codepoint);
        if (status != BOXRIG_STATUS_OK) {
            return status;
        }
        first = false;
    }
    return BOXRIG_STATUS_OK;
}

static bool kitty_modifier_key(boxrig_key key)
{
    return (key >= BOXRIG_KEY_LEFT_SHIFT &&
            key <= BOXRIG_KEY_ISO_LEVEL5_SHIFT) ||
           key == BOXRIG_KEY_CAPS_LOCK || key == BOXRIG_KEY_SCROLL_LOCK ||
           key == BOXRIG_KEY_NUM_LOCK;
}

static bool kitty_functional_code(boxrig_key key, uint32_t *code_out)
{
    if (key == BOXRIG_KEY_ESCAPE) {
        *code_out = 27;
        return true;
    }
    if (key == BOXRIG_KEY_ENTER) {
        *code_out = 13;
        return true;
    }
    if (key == BOXRIG_KEY_TAB) {
        *code_out = 9;
        return true;
    }
    if (key == BOXRIG_KEY_BACKSPACE) {
        *code_out = 127;
        return true;
    }
    if (key >= BOXRIG_KEY_CAPS_LOCK && key <= BOXRIG_KEY_MENU) {
        *code_out = 57358u + (uint32_t)(key - BOXRIG_KEY_CAPS_LOCK);
        return true;
    }
    if (key >= BOXRIG_KEY_F13 && key <= BOXRIG_KEY_F35) {
        *code_out = 57376u + (uint32_t)(key - BOXRIG_KEY_F13);
        return true;
    }
    if (key >= BOXRIG_KEY_KP_0 && key <= BOXRIG_KEY_KP_BEGIN) {
        *code_out = 57399u + (uint32_t)(key - BOXRIG_KEY_KP_0);
        return true;
    }
    if (key >= BOXRIG_KEY_MEDIA_PLAY &&
        key <= BOXRIG_KEY_ISO_LEVEL5_SHIFT) {
        *code_out = 57428u + (uint32_t)(key - BOXRIG_KEY_MEDIA_PLAY);
        return true;
    }
    return false;
}

static boxrig_status append_kitty_csi_u(
    boxrig_terminal *terminal,
    const boxrig_key_event *event,
    uint32_t flags,
    uint32_t key_code,
    bool text_key)
{
    boxrig_key_sequence_builder builder;
    uint32_t modifiers = event->modifiers;
    uint32_t modifier;
    uint32_t event_type = (uint32_t)event->action + 1;
    bool shifted = false;
    bool base_layout = false;
    bool modifier_field;
    bool associated =
        (flags & BOXRIG_KITTY_REPORT_ALL_KEYS) != 0 &&
        (flags & BOXRIG_KITTY_REPORT_ASSOCIATED_TEXT) != 0 &&
        event->action != BOXRIG_KEY_RELEASE && has_associated_text(event);
    boxrig_status status = key_sequence_initialize(
        &builder,
        terminal->output_capacity);

    if (status != BOXRIG_STATUS_OK) {
        return status;
    }
    if (text_key && (flags & BOXRIG_KITTY_REPORT_ALL_KEYS) == 0) {
        modifiers &= ~(BOXRIG_MODIFIER_CAPS_LOCK | BOXRIG_MODIFIER_NUM_LOCK);
    }
    modifier = modifiers + 1;
    if (text_key && (flags & BOXRIG_KITTY_REPORT_ALTERNATE_KEYS) != 0) {
        shifted = (event->modifiers & BOXRIG_MODIFIER_SHIFT) != 0 &&
                  event->shifted_codepoint != 0;
        base_layout = event->base_layout_codepoint != 0;
    }
    modifier_field = modifier != 1 ||
                     ((flags & BOXRIG_KITTY_REPORT_EVENTS) != 0 &&
                      event_type != 1);

    status = key_sequence_append(&builder, "\x1b[", 2);
    if (status == BOXRIG_STATUS_OK) {
        status = key_sequence_append_number(&builder, key_code);
    }
    if (status == BOXRIG_STATUS_OK && (shifted || base_layout)) {
        status = key_sequence_append_byte(&builder, (uint8_t)':');
        if (status == BOXRIG_STATUS_OK && shifted) {
            status = key_sequence_append_number(
                &builder,
                event->shifted_codepoint);
        }
        if (status == BOXRIG_STATUS_OK && base_layout) {
            status = key_sequence_append_byte(&builder, (uint8_t)':');
            if (status == BOXRIG_STATUS_OK) {
                status = key_sequence_append_number(
                    &builder,
                    event->base_layout_codepoint);
            }
        }
    }
    if (status == BOXRIG_STATUS_OK && modifier_field) {
        status = key_sequence_append_byte(&builder, (uint8_t)';');
        if (status == BOXRIG_STATUS_OK) {
            status = key_sequence_append_number(&builder, modifier);
        }
        if (status == BOXRIG_STATUS_OK &&
            (flags & BOXRIG_KITTY_REPORT_EVENTS) != 0 && event_type != 1) {
            status = key_sequence_append_byte(&builder, (uint8_t)':');
            if (status == BOXRIG_STATUS_OK) {
                status = key_sequence_append_number(&builder, event_type);
            }
        }
    }
    if (status == BOXRIG_STATUS_OK && associated) {
        if (!modifier_field) {
            status = key_sequence_append_byte(&builder, (uint8_t)';');
        }
        if (status == BOXRIG_STATUS_OK) {
            status = key_sequence_append_byte(&builder, (uint8_t)';');
        }
        if (status == BOXRIG_STATUS_OK) {
            status = append_associated_text(&builder, event);
        }
    }
    if (status == BOXRIG_STATUS_OK) {
        status = key_sequence_append_byte(&builder, (uint8_t)'u');
    }
    if (status == BOXRIG_STATUS_OK) {
        status = boxrig_terminal_append_output(
            terminal,
            builder.bytes,
            builder.length);
    }
    free(builder.bytes);
    return status;
}

static boxrig_status append_kitty_legacy_function(
    boxrig_terminal *terminal,
    const boxrig_key_event *event,
    uint32_t flags,
    uint32_t number,
    char final_byte)
{
    char sequence[64];
    uint32_t modifier = event->modifiers + 1;
    uint32_t event_type = (uint32_t)event->action + 1;
    bool report_event = (flags & BOXRIG_KITTY_REPORT_EVENTS) != 0 &&
                        event_type != 1;
    int length;

    if (modifier == 1 && !report_event) {
        if (final_byte == '~') {
            length = snprintf(
                sequence,
                sizeof(sequence),
                "\x1b[%" PRIu32 "~",
                number);
        } else {
            length = snprintf(
                sequence,
                sizeof(sequence),
                "\x1b[%c",
                final_byte);
        }
    } else if (report_event) {
        length = snprintf(
            sequence,
            sizeof(sequence),
            "\x1b[%" PRIu32 ";%" PRIu32 ":%" PRIu32 "%c",
            number,
            modifier,
            event_type,
            final_byte);
    } else {
        length = snprintf(
            sequence,
            sizeof(sequence),
            "\x1b[%" PRIu32 ";%" PRIu32 "%c",
            number,
            modifier,
            final_byte);
    }
    if (length < 0 || (size_t)length >= sizeof(sequence)) {
        return BOXRIG_STATUS_LIMIT_EXCEEDED;
    }
    return boxrig_terminal_append_output(
        terminal,
        (const uint8_t *)sequence,
        (size_t)length);
}

static boxrig_status append_kitty_key(
    boxrig_terminal *terminal,
    const boxrig_key_event *event,
    uint32_t flags,
    bool *handled_out)
{
    uint32_t code;
    unsigned int number;
    char final_byte;
    bool report_events = (flags & BOXRIG_KITTY_REPORT_EVENTS) != 0;
    bool report_all = (flags & BOXRIG_KITTY_REPORT_ALL_KEYS) != 0;
    bool disambiguate = (flags & BOXRIG_KITTY_DISAMBIGUATE) != 0;
    bool enhanced_event = report_events && event->action != BOXRIG_KEY_PRESS;

    *handled_out = true;
    if (event->key == BOXRIG_KEY_NONE) {
        if (event->action == BOXRIG_KEY_RELEASE) {
            if (!report_events || event->codepoint == 0) {
                return BOXRIG_STATUS_OK;
            }
            return append_kitty_csi_u(
                terminal,
                event,
                flags,
                event->codepoint,
                true);
        }
        if (report_all ||
            (disambiguate && event->codepoint != 0 &&
             (event->text_length == 0 ||
              (event->modifiers &
               (BOXRIG_MODIFIER_ALT | BOXRIG_MODIFIER_CONTROL |
                BOXRIG_MODIFIER_SUPER | BOXRIG_MODIFIER_HYPER |
                BOXRIG_MODIFIER_META)) != 0))) {
            return append_kitty_csi_u(
                terminal,
                event,
                flags,
                event->codepoint,
                true);
        }
        *handled_out = false;
        return BOXRIG_STATUS_OK;
    }
    /* Modifier keys are observable only in report-all-keys mode. */
    if (kitty_modifier_key(event->key)) {
        if (!report_all ||
            (event->action == BOXRIG_KEY_RELEASE && !report_events)) {
            return BOXRIG_STATUS_OK;
        }
        if (!kitty_functional_code(event->key, &code)) {
            return BOXRIG_STATUS_INVALID_ARGUMENT;
        }
        return append_kitty_csi_u(terminal, event, flags, code, false);
    }
    if (event->action == BOXRIG_KEY_RELEASE && !report_events) {
        return BOXRIG_STATUS_OK;
    }
    /* These three releases remain suppressed so a crashed client can reset. */
    if ((event->key == BOXRIG_KEY_ENTER || event->key == BOXRIG_KEY_TAB ||
         event->key == BOXRIG_KEY_BACKSPACE) &&
        event->action == BOXRIG_KEY_RELEASE && !report_all) {
        return BOXRIG_STATUS_OK;
    }
    if (kitty_functional_code(event->key, &code)) {
        bool keypad = event->key >= BOXRIG_KEY_KP_0 &&
                      event->key <= BOXRIG_KEY_KP_BEGIN;
        bool simple = event->key == BOXRIG_KEY_ESCAPE ||
                      event->key == BOXRIG_KEY_ENTER ||
                      event->key == BOXRIG_KEY_TAB ||
                      event->key == BOXRIG_KEY_BACKSPACE;
        bool extended_function = event->key >= BOXRIG_KEY_F13 &&
                                 event->key <= BOXRIG_KEY_F35;
        bool nonlegacy =
            (event->key >= BOXRIG_KEY_PRINT_SCREEN &&
             event->key <= BOXRIG_KEY_MENU) ||
            (event->key >= BOXRIG_KEY_MEDIA_PLAY &&
             event->key <= BOXRIG_KEY_MUTE_VOLUME);

        if (report_all ||
            (disambiguate && (event->key == BOXRIG_KEY_ESCAPE || keypad)) ||
            enhanced_event ||
            (extended_function &&
             (flags & (BOXRIG_KITTY_DISAMBIGUATE |
                       BOXRIG_KITTY_REPORT_EVENTS |
                       BOXRIG_KITTY_REPORT_ALL_KEYS)) != 0) ||
            nonlegacy) {
            return append_kitty_csi_u(terminal, event, flags, code, false);
        }
        if (simple || keypad || extended_function) {
            *handled_out = false;
            return BOXRIG_STATUS_OK;
        }
    }
    if (key_final(event->key, &final_byte)) {
        if (report_all || disambiguate || enhanced_event) {
            return append_kitty_legacy_function(
                terminal,
                event,
                flags,
                1,
                final_byte);
        }
        *handled_out = false;
        return BOXRIG_STATUS_OK;
    }
    if (tilde_key_number(event->key, &number)) {
        if (report_all || disambiguate || enhanced_event) {
            return append_kitty_legacy_function(
                terminal,
                event,
                flags,
                (uint32_t)number,
                '~');
        }
        *handled_out = false;
        return BOXRIG_STATUS_OK;
    }
    *handled_out = false;
    return BOXRIG_STATUS_OK;
}

boxrig_status boxrig_terminal_send_key(
    boxrig_terminal *terminal,
    const boxrig_key_event *event)
{
    char sequence[32];
    char final_byte;
    unsigned int number;
    unsigned int modifier;
    uint8_t keypad_normal;
    boxrig_key mapped_key;
    boxrig_status status;
    uint32_t kitty_flags;
    bool kitty_handled;
    int length;

    if (terminal == NULL || event == NULL) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    if ((event->modifiers & ~all_modifiers) != 0) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    if (event->action != BOXRIG_KEY_PRESS &&
        event->action != BOXRIG_KEY_REPEAT &&
        event->action != BOXRIG_KEY_RELEASE) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    if ((unsigned int)event->key >
        (unsigned int)BOXRIG_KEY_ISO_LEVEL5_SHIFT) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    if (!valid_key_codepoint(event->codepoint) ||
        !valid_key_codepoint(event->shifted_codepoint) ||
        !valid_key_codepoint(event->base_layout_codepoint)) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    if (event->key != BOXRIG_KEY_NONE &&
        (event->codepoint != 0 || event->shifted_codepoint != 0 ||
         event->base_layout_codepoint != 0)) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    status = validate_key_text(event);
    if (status != BOXRIG_STATUS_OK) {
        return status;
    }
    kitty_flags = boxrig_input_kitty_flags(
        &terminal->input,
        terminal->screen.alternate_active);
    status = append_kitty_key(
        terminal,
        event,
        kitty_flags,
        &kitty_handled);
    if (status != BOXRIG_STATUS_OK || kitty_handled) {
        return status;
    }
    if (terminal->input_config.key_mapping_count != 0 &&
        event->key >= BOXRIG_KEY_UP && event->key <= BOXRIG_KEY_LEFT) {
        uint32_t modifiers = event->modifiers &
            ~(BOXRIG_MODIFIER_CAPS_LOCK | BOXRIG_MODIFIER_NUM_LOCK);
        for (size_t index = 0; index < terminal->input_config.key_mapping_count; index++) {
            const boxrig_key_mapping *mapping = &terminal->input_config.key_mappings[index];
            if (mapping->key == event->key && mapping->modifiers == modifiers) {
                return boxrig_terminal_append_output(
                    terminal, mapping->bytes, mapping->length);
            }
        }
    }
    if (event->key == BOXRIG_KEY_NONE) {
        uint8_t control_byte;

        if (legacy_control_byte(event, &control_byte)) {
            boxrig_key_event normalized = *event;

            normalized.text = (const char *)&control_byte;
            normalized.text_length = 1;
            return append_text_key(terminal, &normalized);
        }
        return append_text_key(terminal, event);
    }
    if (keypad_value(
            event->key,
            &keypad_normal,
            &final_byte)) {
        return append_keypad_key(
            terminal,
            event,
            keypad_normal,
            final_byte);
    }
    if (keypad_navigation_key(event->key, &mapped_key)) {
        boxrig_key_event mapped = *event;

        mapped.key = mapped_key;
        return boxrig_terminal_send_key(terminal, &mapped);
    }
    if (event->key == BOXRIG_KEY_KP_BEGIN) {
        modifier = modifier_parameter(event->modifiers);
        if (modifier == 1) {
            sequence[0] = 0x1b;
            sequence[1] = terminal->input.application_cursor ? 'O' : '[';
            sequence[2] = 'E';
            length = 3;
        } else {
            length = snprintf(
                sequence,
                sizeof(sequence),
                "\x1b[1;%uE",
                modifier);
        }
        if (length < 0 || (size_t)length >= sizeof(sequence)) {
            return BOXRIG_STATUS_LIMIT_EXCEEDED;
        }
        return boxrig_terminal_append_output(
            terminal,
            (const uint8_t *)sequence,
            (size_t)length);
    }
    if (event->key == BOXRIG_KEY_BACKSPACE || event->key == BOXRIG_KEY_DELETE) {
        return append_erase_key(terminal, event);
    }
    if (event->key == BOXRIG_KEY_ENTER || event->key == BOXRIG_KEY_TAB ||
        event->key == BOXRIG_KEY_ESCAPE) {
        return append_simple_key(terminal, event);
    }

    modifier = modifier_parameter(event->modifiers);
    if (key_final(event->key, &final_byte)) {
        if (modifier != 1) {
            length = snprintf(
                sequence,
                sizeof(sequence),
                "\x1b[1;%u%c",
                modifier,
                final_byte);
        } else if (event->key >= BOXRIG_KEY_F1 &&
                   event->key <= BOXRIG_KEY_F4) {
            length = snprintf(
                sequence,
                sizeof(sequence),
                "\x1bO%c",
                final_byte);
        } else if (terminal->input.application_cursor) {
            length = snprintf(
                sequence,
                sizeof(sequence),
                "\x1bO%c",
                final_byte);
        } else {
            length = snprintf(
                sequence,
                sizeof(sequence),
                "\x1b[%c",
                final_byte);
        }
    } else if (tilde_key_number(event->key, &number)) {
        if (modifier == 1) {
            length = snprintf(sequence, sizeof(sequence), "\x1b[%u~", number);
        } else {
            length = snprintf(
                sequence,
                sizeof(sequence),
                "\x1b[%u;%u~",
                number,
                modifier);
        }
    } else if (extended_function_code(event->key, &number)) {
        if (modifier == 1) {
            length = snprintf(sequence, sizeof(sequence), "\x1b[%uu", number);
        } else {
            length = snprintf(
                sequence,
                sizeof(sequence),
                "\x1b[%u;%uu",
                number,
                modifier);
        }
    } else {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    if (length < 0 || (size_t)length >= sizeof(sequence)) {
        return BOXRIG_STATUS_LIMIT_EXCEEDED;
    }
    return boxrig_terminal_append_output(
        terminal,
        (const uint8_t *)sequence,
        (size_t)length);
}

boxrig_status boxrig_terminal_send_paste(
    boxrig_terminal *terminal,
    const boxrig_paste_event *event)
{
    static const uint8_t begin[] = "\x1b[200~";
    static const uint8_t end[] = "\x1b[201~";
    boxrig_status status;

    if (terminal == NULL || event == NULL ||
        (event->text == NULL && event->text_length != 0)) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    switch (event->phase) {
    case BOXRIG_PASTE_BEGIN:
        if (terminal->input.paste_active || event->text_length != 0) {
            return BOXRIG_STATUS_INVALID_ARGUMENT;
        }
        if (terminal->input.bracketed_paste) {
            status = boxrig_terminal_append_output(
                terminal,
                begin,
                sizeof(begin) - 1);
            if (status != BOXRIG_STATUS_OK) {
                return status;
            }
        }
        terminal->input.paste_active = true;
        terminal->input.paste_bracketed =
            terminal->input.bracketed_paste;
        return BOXRIG_STATUS_OK;
    case BOXRIG_PASTE_CHUNK:
        if (!terminal->input.paste_active) {
            return BOXRIG_STATUS_INVALID_ARGUMENT;
        }
        if (event->text_length != 0 &&
            memchr(event->text, 0x1b, event->text_length) != NULL) {
            return BOXRIG_STATUS_INVALID_ARGUMENT;
        }
        return boxrig_terminal_append_encoded_text(
            terminal,
            (const uint8_t *)event->text,
            event->text_length, terminal->paste_newline, false, false, false);
    case BOXRIG_PASTE_END:
        if (!terminal->input.paste_active || event->text_length != 0) {
            return BOXRIG_STATUS_INVALID_ARGUMENT;
        }
        if (terminal->input.paste_bracketed) {
            status = boxrig_terminal_append_output(
                terminal,
                end,
                sizeof(end) - 1);
            if (status != BOXRIG_STATUS_OK) {
                return status;
            }
        }
        terminal->input.paste_active = false;
        terminal->input.paste_bracketed = false;
        return BOXRIG_STATUS_OK;
    default:
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
}

boxrig_status boxrig_terminal_send_focus(
    boxrig_terminal *terminal,
    const boxrig_focus_event *event)
{
    static const uint8_t focus_in[] = "\x1b[I";
    static const uint8_t focus_out[] = "\x1b[O";

    if (terminal == NULL || event == NULL) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    if (!terminal->input.focus_reporting) {
        return BOXRIG_STATUS_OK;
    }
    return boxrig_terminal_append_output(
        terminal,
        event->focused ? focus_in : focus_out,
        sizeof(focus_in) - 1);
}

static bool mouse_button_code(
    boxrig_mouse_button button,
    unsigned int *code_out)
{
    switch (button) {
    case BOXRIG_MOUSE_BUTTON_LEFT:
        *code_out = 0;
        return true;
    case BOXRIG_MOUSE_BUTTON_MIDDLE:
        *code_out = 1;
        return true;
    case BOXRIG_MOUSE_BUTTON_RIGHT:
        *code_out = 2;
        return true;
    case BOXRIG_MOUSE_BUTTON_WHEEL_UP:
        *code_out = 64;
        return true;
    case BOXRIG_MOUSE_BUTTON_WHEEL_DOWN:
        *code_out = 65;
        return true;
    case BOXRIG_MOUSE_BUTTON_WHEEL_LEFT:
        *code_out = 67;
        return true;
    case BOXRIG_MOUSE_BUTTON_WHEEL_RIGHT:
        *code_out = 66;
        return true;
    case BOXRIG_MOUSE_BUTTON_8:
        *code_out = 128;
        return true;
    case BOXRIG_MOUSE_BUTTON_9:
        *code_out = 129;
        return true;
    case BOXRIG_MOUSE_BUTTON_10:
        *code_out = 130;
        return true;
    case BOXRIG_MOUSE_BUTTON_11:
        *code_out = 131;
        return true;
    default:
        return false;
    }
}

static unsigned int mouse_modifier_code(uint32_t modifiers)
{
    unsigned int code = 0;

    if ((modifiers & BOXRIG_MODIFIER_SHIFT) != 0) {
        code |= 4;
    }
    if ((modifiers & (BOXRIG_MODIFIER_ALT | BOXRIG_MODIFIER_META)) != 0) {
        code |= 8;
    }
    if ((modifiers & BOXRIG_MODIFIER_CONTROL) != 0) {
        code |= 16;
    }
    return code;
}

static unsigned int motion_button_code(uint32_t pressed_buttons)
{
    if ((pressed_buttons & BOXRIG_MOUSE_BUTTON_MASK_LEFT) != 0) {
        return 0;
    }
    if ((pressed_buttons & BOXRIG_MOUSE_BUTTON_MASK_MIDDLE) != 0) {
        return 1;
    }
    if ((pressed_buttons & BOXRIG_MOUSE_BUTTON_MASK_RIGHT) != 0) {
        return 2;
    }
    if ((pressed_buttons & BOXRIG_MOUSE_BUTTON_MASK_8) != 0) {
        return 128;
    }
    if ((pressed_buttons & BOXRIG_MOUSE_BUTTON_MASK_9) != 0) {
        return 129;
    }
    if ((pressed_buttons & BOXRIG_MOUSE_BUTTON_MASK_10) != 0) {
        return 130;
    }
    if ((pressed_buttons & BOXRIG_MOUSE_BUTTON_MASK_11) != 0) {
        return 131;
    }
    return 3;
}

static bool wheel_button(boxrig_mouse_button button)
{
    return button == BOXRIG_MOUSE_BUTTON_WHEEL_UP ||
           button == BOXRIG_MOUSE_BUTTON_WHEEL_DOWN ||
           button == BOXRIG_MOUSE_BUTTON_WHEEL_LEFT ||
           button == BOXRIG_MOUSE_BUTTON_WHEEL_RIGHT;
}

bool boxrig_terminal_mouse_reporting_enabled(
    const boxrig_terminal *terminal)
{
    return terminal != NULL &&
           terminal->input.mouse_tracking != BOXRIG_MOUSE_TRACKING_NONE;
}

bool boxrig_terminal_alternate_scroll_enabled(const boxrig_terminal *terminal)
{
    return terminal != NULL && terminal->input.alternate_scroll;
}

boxrig_status boxrig_terminal_send_mouse(
    boxrig_terminal *terminal,
    const boxrig_mouse_event *event)
{
    uint8_t legacy[6] = {0x1b, '[', 'M', 0, 0, 0};
    char sgr[128];
    unsigned int button_code;
    unsigned int modifiers;
    unsigned int encoded_code;
    bool motion;
    int length;

    if (terminal == NULL || event == NULL ||
        (event->modifiers & ~all_modifiers) != 0 ||
        (event->pressed_buttons & ~all_mouse_button_masks) != 0) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    if (event->action != BOXRIG_MOUSE_PRESS &&
        event->action != BOXRIG_MOUSE_RELEASE &&
        event->action != BOXRIG_MOUSE_MOTION) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    motion = event->action == BOXRIG_MOUSE_MOTION;
    if (motion) {
        if (event->button != BOXRIG_MOUSE_BUTTON_NONE) {
            return BOXRIG_STATUS_INVALID_ARGUMENT;
        }
        button_code = motion_button_code(event->pressed_buttons);
    } else if (!mouse_button_code(event->button, &button_code)) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    if (terminal->input.mouse_tracking == BOXRIG_MOUSE_TRACKING_NONE) {
        if (terminal->input.alternate_scroll && terminal->screen.alternate_active &&
            event->action == BOXRIG_MOUSE_PRESS &&
            (event->button == BOXRIG_MOUSE_BUTTON_WHEEL_UP ||
             event->button == BOXRIG_MOUSE_BUTTON_WHEEL_DOWN)) {
            if (event->row >= terminal->screen.rows ||
                event->column >= terminal->screen.columns)
                return BOXRIG_STATUS_INVALID_ARGUMENT;
            /* Alternate Scroll emits DECCKM cursor controls, not physical
             * keyboard events (and therefore no Kitty key/release reports). */
            const uint8_t cursor[] = {0x1b,
                terminal->input.application_cursor ? 'O' : '[',
                event->button == BOXRIG_MOUSE_BUTTON_WHEEL_UP ? 'A' : 'B'};
            return boxrig_terminal_append_output(terminal, cursor, sizeof(cursor));
        }
        return BOXRIG_STATUS_OK;
    }
    if (terminal->input.mouse_tracking == BOXRIG_MOUSE_TRACKING_X10) {
        if (event->action != BOXRIG_MOUSE_PRESS ||
            event->button < BOXRIG_MOUSE_BUTTON_LEFT ||
            event->button > BOXRIG_MOUSE_BUTTON_RIGHT) {
            return BOXRIG_STATUS_OK;
        }
    } else if (motion) {
        if (terminal->input.mouse_tracking == BOXRIG_MOUSE_TRACKING_VT200 ||
            (terminal->input.mouse_tracking ==
                 BOXRIG_MOUSE_TRACKING_BUTTON_EVENT &&
             event->pressed_buttons == 0)) {
            return BOXRIG_STATUS_OK;
        }
    } else if (event->action == BOXRIG_MOUSE_RELEASE &&
               wheel_button(event->button)) {
        return BOXRIG_STATUS_OK;
    }
    /* A queued, unreported transition can outlive the grid that produced its
     * coordinates. Only selected cell-based reports need current grid bounds. */
    if (!terminal->input.pixel_mouse &&
        (event->row >= terminal->screen.rows ||
         event->column >= terminal->screen.columns)) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }

    modifiers = terminal->input.mouse_tracking == BOXRIG_MOUSE_TRACKING_X10
                    ? 0
                    : mouse_modifier_code(event->modifiers);
    if (event->action == BOXRIG_MOUSE_RELEASE &&
        (terminal->input.sgr_mouse || terminal->input.pixel_mouse)) {
        encoded_code = button_code + modifiers;
    } else if (event->action == BOXRIG_MOUSE_RELEASE) {
        encoded_code = 3 + modifiers;
    } else {
        encoded_code = button_code + modifiers + (motion ? 32u : 0u);
    }

    if (terminal->input.sgr_mouse || terminal->input.pixel_mouse) {
        if (terminal->input.pixel_mouse) {
            if (event->x == INT64_MAX || event->y == INT64_MAX)
                return BOXRIG_STATUS_LIMIT_EXCEEDED;
            /* Preserve signed positions outside the content, as xterm does;
             * neither the current cell grid nor font metrics quantize them. */
            length = snprintf(sgr, sizeof(sgr), "\x1b[<%u;%" PRId64 ";%" PRId64 "%c",
                encoded_code, event->x + 1, event->y + 1,
                event->action == BOXRIG_MOUSE_RELEASE ? 'm' : 'M');
        } else {
            length = snprintf(
                sgr,
                sizeof(sgr),
                "\x1b[<%u;%zu;%zu%c",
                encoded_code,
                event->column + 1,
                event->row + 1,
                event->action == BOXRIG_MOUSE_RELEASE ? 'm' : 'M');
        }
        if (length < 0 || (size_t)length >= sizeof(sgr)) {
            return BOXRIG_STATUS_LIMIT_EXCEEDED;
        }
        return boxrig_terminal_append_output(
            terminal,
            (const uint8_t *)sgr,
            (size_t)length);
    }
    if (event->column >= 223 || event->row >= 223) {
        return BOXRIG_STATUS_LIMIT_EXCEEDED;
    }
    legacy[3] = (uint8_t)(encoded_code + 32u);
    legacy[4] = (uint8_t)(event->column + 33u);
    legacy[5] = (uint8_t)(event->row + 33u);
    return boxrig_terminal_append_output(terminal, legacy, sizeof(legacy));
}
