/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#ifndef BOXRIG_TEST_FRONTEND_CONFORMANCE_CASES_H
#define BOXRIG_TEST_FRONTEND_CONFORMANCE_CASES_H

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/** Checks whether a frontend's required shared case actually passed.
 * @param frontend_name Name of the adapter executing the fixture.
 * @param required_frontends Bit set for each COMMON frontend in matrix order.
 * @param passed True only after the shared case assertions finish.
 * @return True if the known frontend satisfies its own cell.
 */
static inline bool boxrig_test_frontend_case_satisfied(
    const char *frontend_name, unsigned int required_frontends, bool passed)
{
    static const char *const names[] = {"X11", "Win32", "GTK 3", "GTK 4", "native Wayland"};
    size_t index;
    if (frontend_name != NULL) {
        for (index = 0; index < 5u; index++) {
            if (strcmp(frontend_name, names[index]) == 0) {
                return (required_frontends & (1u << index)) == 0u || passed;
            }
        }
    }
    return false;
}

#define BOXRIG_TEST_CASE_EDITABLE_PROVIDER_ABSENT                         \
    "view-action.reconversion.provider-absent"

#endif
