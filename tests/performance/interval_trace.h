/* Test-only buffered interval recording. No file I/O occurs in measured work. */
#ifndef BOXRIG_PERFORMANCE_INTERVAL_TRACE_H
#define BOXRIG_PERFORMANCE_INTERVAL_TRACE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct boxrig_interval_start {
    uint64_t wall_ns;
    uint64_t cpu_ns;
} boxrig_interval_start;

/* Enable recording when environment_variable names an output file. */
void boxrig_interval_initialize(const char *environment_variable);
boxrig_interval_start boxrig_interval_begin(void);
/* CPU time belongs to the calling thread. Nested intervals overlap; they
 * must not be added to their enclosing interval when computing totals. */
void boxrig_interval_end(const char *name, boxrig_interval_start start,
    size_t units, uint64_t revision, int status, bool more);

#endif
