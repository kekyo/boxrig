#define _POSIX_C_SOURCE 200809L
#include "interval_trace.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

typedef struct interval {
    const char *name;
    uint64_t start_ns;
    uint64_t end_ns;
    uint64_t cpu_ns;
    size_t units;
    uint64_t revision;
    int status;
    bool more;
} interval;

static interval records[262144];
static size_t record_count;
static size_t dropped;
static const char *output_path;

static uint64_t nanoseconds(clockid_t clock)
{
    struct timespec value;
    if (clock_gettime(clock, &value) != 0) {
        perror("interval clock");
        exit(EXIT_FAILURE);
    }
    return (uint64_t)value.tv_sec * UINT64_C(1000000000) + (uint64_t)value.tv_nsec;
}

static void flush_trace(void)
{
    FILE *output = fopen(output_path, "w");
    if (output == NULL) {
        perror("interval trace");
        _Exit(EXIT_FAILURE);
    }
    fprintf(output, "# dropped=%zu\n", dropped);
    fputs("name\tstart_ns\tend_ns\tcpu_ns\tunits\trevision\tstatus\tmore\n", output);
    for (size_t index = 0u; index < record_count; index++) {
        const interval *record = &records[index];
        fprintf(output, "%s\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64
            "\t%zu\t%" PRIu64 "\t%d\t%d\n", record->name,
            record->start_ns, record->end_ns, record->cpu_ns, record->units,
            record->revision, record->status, record->more);
    }
    bool failed = ferror(output) != 0;
    if (fclose(output) != 0 || failed || dropped != 0u) {
        fputs("interval trace incomplete\n", stderr);
        _Exit(EXIT_FAILURE);
    }
}

void boxrig_interval_initialize(const char *environment_variable)
{
    const char *path = getenv(environment_variable);
    if (output_path == NULL && path != NULL && *path != '\0') {
        output_path = path;
        if (atexit(flush_trace) != 0) {
            exit(EXIT_FAILURE);
        }
    }
}

boxrig_interval_start boxrig_interval_begin(void)
{
    boxrig_interval_start start = {0};
    if (output_path != NULL) {
        start.wall_ns = nanoseconds(CLOCK_MONOTONIC);
        start.cpu_ns = nanoseconds(CLOCK_THREAD_CPUTIME_ID);
    }
    return start;
}

void boxrig_interval_end(const char *name, boxrig_interval_start start,
    size_t units, uint64_t revision, int status, bool more)
{
    if (start.wall_ns == 0u) return;
    uint64_t cpu_ns = nanoseconds(CLOCK_THREAD_CPUTIME_ID) - start.cpu_ns;
    uint64_t end_ns = nanoseconds(CLOCK_MONOTONIC);
    if (record_count == sizeof(records) / sizeof(records[0])) {
        dropped++;
        return;
    }
    records[record_count++] = (interval){name, start.wall_ns, end_ns,
        cpu_ns, units, revision, status, more};
}
