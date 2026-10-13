/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "boxrig/win32.h"

#include <stdbool.h>
#include <stdlib.h>

static bool input_adapter_destroyed;

/** Calls the input adapter implementation wrapped by the linker. */
int __real_boxrig_win32_input_adapter_process_message(
    boxrig_win32_input_adapter *adapter,
    UINT message,
    WPARAM word_parameter,
    LPARAM long_parameter,
    LRESULT *result_out);

/** Calls the input adapter destructor wrapped by the linker. */
void __real_boxrig_win32_input_adapter_destroy(
    boxrig_win32_input_adapter *adapter);

/** Fails the instrumented sample if a native message reaches a dead adapter. */
int __wrap_boxrig_win32_input_adapter_process_message(
    boxrig_win32_input_adapter *adapter,
    UINT message,
    WPARAM word_parameter,
    LPARAM long_parameter,
    LRESULT *result_out)
{
    if (input_adapter_destroyed) {
        exit(97);
    }
    return __real_boxrig_win32_input_adapter_process_message(
        adapter,
        message,
        word_parameter,
        long_parameter,
        result_out);
}

/** Records when the instrumented sample has released its input adapter. */
void __wrap_boxrig_win32_input_adapter_destroy(
    boxrig_win32_input_adapter *adapter)
{
    __real_boxrig_win32_input_adapter_destroy(adapter);
    if (adapter != NULL) {
        input_adapter_destroyed = true;
    }
}
