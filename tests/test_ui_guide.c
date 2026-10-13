/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

typedef struct guide_source {
    const char *name;
    bool found;
} guide_source;

static guide_source sources[] = {
    {"Makefile", false},
    {"x11.c", false},
    {"win32.c", false},
};
static size_t source_count = sizeof(sources) / sizeof(sources[0]);

static bool directory_is_empty(const char *path)
{
    DIR *directory = opendir(path);
    struct dirent *entry;

    if (directory == NULL) {
        return false;
    }
    while ((entry = readdir(directory)) != NULL) {
        if (strcmp(entry->d_name, ".") != 0 &&
            strcmp(entry->d_name, "..") != 0) {
            closedir(directory);
            return false;
        }
    }
    closedir(directory);
    return true;
}

static guide_source *find_source(const char *name)
{
    size_t index;

    for (index = 0; index < source_count; index++) {
        if (strcmp(sources[index].name, name) == 0) {
            return &sources[index];
        }
    }
    return NULL;
}

static bool parse_marker(const char *line, char *name_out, size_t capacity)
{
    static const char prefix[] = "<!-- ui-guide-source: ";
    const char *name;
    const char *suffix;
    size_t length;

    if (strncmp(line, prefix, sizeof(prefix) - 1) != 0) {
        return false;
    }
    name = line + sizeof(prefix) - 1;
    suffix = strstr(name, " -->");
    if (suffix == NULL || (suffix[4] != '\0' && suffix[4] != '\n')) {
        return false;
    }
    length = (size_t)(suffix - name);
    if (length == 0 || length >= capacity) {
        return false;
    }
    memcpy(name_out, name, length);
    name_out[length] = '\0';
    return true;
}

static bool extract_sources(const char *guide_path, const char *output_path)
{
    FILE *guide = fopen(guide_path, "rb");
    FILE *output = NULL;
    guide_source *current = NULL;
    char line[4096];
    char name[64];
    char path[4096];
    bool awaiting_fence = false;
    bool in_source = false;
    bool succeeded = true;

    if (guide == NULL) {
        fprintf(stderr, "cannot open UI guide: %s\n", guide_path);
        return false;
    }
    while (fgets(line, sizeof(line), guide) != NULL) {
        if (!awaiting_fence && !in_source &&
            parse_marker(line, name, sizeof(name))) {
            current = find_source(name);
            if (current == NULL || current->found) {
                fprintf(stderr, "unexpected UI guide source: %s\n", name);
                succeeded = false;
                break;
            }
            awaiting_fence = true;
            continue;
        }
        if (awaiting_fence) {
            if (strncmp(line, "```", 3) != 0) {
                fprintf(stderr, "missing source fence after %s\n", current->name);
                succeeded = false;
                break;
            }
            if (snprintf(path, sizeof(path), "%s/%s", output_path,
                         current->name) >= (int)sizeof(path)) {
                succeeded = false;
                break;
            }
            output = fopen(path, "wb");
            if (output == NULL) {
                fprintf(stderr, "cannot create extracted source: %s\n", path);
                succeeded = false;
                break;
            }
            awaiting_fence = false;
            in_source = true;
            continue;
        }
        if (in_source &&
            (strcmp(line, "```\n") == 0 || strcmp(line, "```") == 0)) {
            if (fclose(output) != 0) {
                succeeded = false;
                output = NULL;
                break;
            }
            output = NULL;
            current->found = true;
            current = NULL;
            in_source = false;
            continue;
        }
        if (in_source && fputs(line, output) == EOF) {
            succeeded = false;
            break;
        }
    }
    if (output != NULL && fclose(output) != 0) {
        succeeded = false;
    }
    if (fclose(guide) != 0) {
        succeeded = false;
    }
    if (awaiting_fence || in_source) {
        fprintf(stderr, "unterminated UI guide source\n");
        succeeded = false;
    }
    return succeeded;
}

int main(int argument_count, char **arguments)
{
    size_t index;

    if (argument_count < 3 || argument_count > 6) {
        fprintf(stderr, "usage: %s GUIDE EMPTY_OUTPUT_DIRECTORY [SOURCE ...]\n", arguments[0]);
        return 2;
    }
    if (argument_count > 3) {
        source_count = (size_t)(argument_count - 3);
        for (index = 0; index < source_count; index++) {
            sources[index].name = arguments[index + 3];
        }
    }
    if (!directory_is_empty(arguments[2])) {
        fprintf(stderr, "UI guide output directory is not empty: %s\n", arguments[2]);
        return 1;
    }
    if (!extract_sources(arguments[1], arguments[2])) {
        return 1;
    }
    for (index = 0; index < source_count; index++) {
        if (!sources[index].found) {
            fprintf(stderr, "missing UI guide source: %s\n", sources[index].name);
            return 1;
        }
    }
    puts("PASS UI guide sources extracted from an empty directory");
    return 0;
}
