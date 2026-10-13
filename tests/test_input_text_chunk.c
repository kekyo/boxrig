/* boxrig - Under MIT. */
#include "boxrig/ui/input_text.h"
#include <stdio.h>
#include <string.h>

int main(void)
{
    const char text[] = "A日本🙂B";
    const size_t expected[] = {0,1,1,1,4,4,4,7,7,7,7,11,12,12};
    for (size_t size = 0; size < sizeof(expected) / sizeof(expected[0]); size++) {
        size_t result = boxrig_input_text_chunk_length(text, sizeof(text) - 1, size);
        if (result != expected[size]) {
            fprintf(stderr, "FAIL preferred=%zu expected=%zu actual=%zu\n", size, expected[size], result);
            return 1;
        }
    }
    const char *scalars[] = {"A", "£", "日", "🙂"};
    for (size_t i = 0; i < sizeof(scalars) / sizeof(scalars[0]); i++) {
        size_t length = strlen(scalars[i]);
        if (boxrig_input_text_chunk_length(scalars[i], length, 1) != length) return 1;
    }
    if (boxrig_input_text_chunk_length(NULL, 0, 1) != 0 ||
        boxrig_input_text_chunk_length(text, 0, 1) != 0 ||
        boxrig_input_text_chunk_length(text, sizeof(text) - 1, 0) != 0) return 1;
    puts("PASS paste chunks preserve UTF-8 scalars, including a one-byte preference");
    return 0;
}
