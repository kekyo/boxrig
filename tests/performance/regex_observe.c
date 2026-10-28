/* boxrig - Under MIT. Count external calls only while resume is active. */
#include "regex_observe.h"
#include "utf8proc.h"

bool regex_observe_active;
size_t regex_observe_categories;
size_t regex_observe_copies;
size_t regex_observe_copy_bytes;

void regex_observe_reset(void)
{
    regex_observe_active = false;
    regex_observe_categories = 0;
    regex_observe_copies = 0;
    regex_observe_copy_bytes = 0;
}

utf8proc_category_t __real_utf8proc_category(utf8proc_int32_t codepoint);
utf8proc_category_t __wrap_utf8proc_category(utf8proc_int32_t codepoint)
{
    if (regex_observe_active) regex_observe_categories++;
    return __real_utf8proc_category(codepoint);
}

void *__real_memcpy(void *destination, const void *source, size_t bytes);
void *__wrap_memcpy(void *destination, const void *source, size_t bytes)
{
    if (regex_observe_active) {
        regex_observe_copies++;
        regex_observe_copy_bytes += bytes;
    }
    return __real_memcpy(destination, source, bytes);
}
