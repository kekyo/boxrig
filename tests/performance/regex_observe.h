/* Test-only link wrappers; no instrumentation in the production library. */
#ifndef BOXRIG_TEST_REGEX_OBSERVE_H
#define BOXRIG_TEST_REGEX_OBSERVE_H
#include <stdbool.h>
#include <stddef.h>
extern bool regex_observe_active;
extern size_t regex_observe_categories;
extern size_t regex_observe_copies;
extern size_t regex_observe_copy_bytes;
void regex_observe_reset(void);
#endif
