/* boxrig - Under MIT. Host-side measurement of live requested heap bytes.
 * Link the same driver and allocator wrappers for every configuration. */
#include <boxrig/terminal.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *__real_malloc(size_t size);
void *__real_realloc(void *ptr, size_t size);
void __real_free(void *ptr);

typedef union allocation_header {
    long double alignment;
    void *pointer_alignment;
    size_t size;
} allocation_header;

static size_t live_bytes;

void *__wrap_malloc(size_t size)
{
    if (size > SIZE_MAX - sizeof(allocation_header)) return NULL;
    allocation_header *block = __real_malloc(sizeof(*block) + size);
    if (!block) return NULL;
    block->size = size;
    live_bytes += size;
    return block + 1;
}

void *__wrap_calloc(size_t count, size_t size)
{
    if (size && count > SIZE_MAX / size) return NULL;
    size_t bytes = count * size;
    void *ptr = __wrap_malloc(bytes);
    if (ptr) memset(ptr, 0, bytes);
    return ptr;
}

void __wrap_free(void *ptr)
{
    if (!ptr) return;
    allocation_header *block = (allocation_header *)ptr - 1;
    live_bytes -= block->size;
    __real_free(block);
}

void *__wrap_realloc(void *ptr, size_t size)
{
    if (!ptr) return __wrap_malloc(size);
    if (!size) { __wrap_free(ptr); return NULL; }
    if (size > SIZE_MAX - sizeof(allocation_header)) return NULL;
    allocation_header *block = (allocation_header *)ptr - 1;
    size_t old = block->size;
    block = __real_realloc(block, sizeof(*block) + size);
    if (!block) return NULL;
    block->size = size;
    live_bytes = live_bytes - old + size;
    return block + 1;
}

int main(int argc, char **argv)
{
    assert(argc == 3);
    boxrig_terminal_config config = boxrig_terminal_config_default();
    config.columns = (size_t)strtoul(argv[1], NULL, 10);
    config.rows = (size_t)strtoul(argv[2], NULL, 10);
    config.scrollback_capacity = 0;
    boxrig_terminal *terminal;
    assert(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    size_t initial_heap = live_bytes;
    static const uint8_t record[] = "\033[32mserial input\r\nprogress 100%\rOK\033[K\r\n";
    for (size_t n = 0; n < 100; ++n) {
        size_t used;
        assert(boxrig_terminal_feed(terminal, record, sizeof(record)-1, &used) == BOXRIG_STATUS_OK);
        assert(used == sizeof(record)-1);
    }
    boxrig_dirty range;
    size_t drawn = 0;
    while (boxrig_terminal_take_dirty(terminal, &range)) {
        for (size_t n = 0; n < range.column_count; ++n) {
            boxrig_cell_view cell;
            assert(boxrig_terminal_get_cell(terminal, range.row, range.column + n, &cell) == BOXRIG_STATUS_OK);
            drawn += cell.text_length;
        }
    }
    assert(drawn == config.rows * config.columns);
    boxrig_terminal_destroy(terminal);
    assert(live_bytes == 0);
    printf("%zux%zu heap=%zu drawn=%zu\n", config.columns, config.rows, initial_heap, drawn);
}
