/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "frontend_component_fixture.h"
#include "frontend_conformance_cases.h"
#include "frontend_sample_scenario.h"

#include <stdbool.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef BOXRIG_TEST_FRONTEND_COMPONENT_FIXTURE_VERSION
#error "frontend component fixture version is not declared"
#endif

#if BOXRIG_TEST_FRONTEND_COMPONENT_FIXTURE_VERSION != 7
#error "frontend component fixture lacks synchronized scrollbar state"
#endif

typedef enum conformance_scope {
    SCOPE_BUILD_ARTIFACT = 0,
    SCOPE_CORE_UI_VALUE,
    SCOPE_PORTABLE_COMPONENT,
    SCOPE_RENDERER_MEANING,
    SCOPE_NATIVE_FRONTEND,
    SCOPE_SAMPLE_POLICY,
    SCOPE_EXTERNAL_P2,
    SCOPE_COUNT
} conformance_scope;

typedef enum conformance_verdict {
    VERDICT_COMMON = 0,
    VERDICT_NATIVE,
    VERDICT_UNSUPPORTED_BY_CONTRACT,
    VERDICT_P2,
    VERDICT_COUNT
} conformance_verdict;

typedef struct conformance_cell {
    conformance_verdict verdict;
    const char *target;
} conformance_cell;

typedef struct conformance_case {
    const char *token;
    const char *id;
    conformance_scope scope;
    const char *description;
    conformance_cell cells[5];
} conformance_case;

#define BOXRIG_FRONTEND_CONFORMANCE_CASE(                                  \
    token, id, scope, description,                                          \
    x11_verdict, x11_target,                                                \
    win32_verdict, win32_target,                                            \
    gtk3_verdict, gtk3_target,                                              \
    gtk4_verdict, gtk4_target,                                              \
    wayland_verdict, wayland_target)                                        \
    {                                                                        \
        #token, id, SCOPE_##scope, description,                              \
        {                                                                    \
            {VERDICT_##x11_verdict, x11_target},                             \
            {VERDICT_##win32_verdict, win32_target},                         \
            {VERDICT_##gtk3_verdict, gtk3_target},                           \
            {VERDICT_##gtk4_verdict, gtk4_target},                           \
            {VERDICT_##wayland_verdict, wayland_target}                      \
        }                                                                    \
    },

static const conformance_case cases[] = {
#include "frontend_conformance_matrix.def"
};

#undef BOXRIG_FRONTEND_CONFORMANCE_CASE

typedef enum public_contract_kind {
    CONTRACT_OPERATION,
    CONTRACT_CAPABILITY,
    CONTRACT_OVERLAY,
    CONTRACT_FAILURE
} public_contract_kind;

typedef struct public_contract_source {
    const char *path;
    const char *text;
} public_contract_source;

typedef struct public_contract_binding {
    const char *path;
    public_contract_kind kind;
    const char *case_token;
    unsigned int frontends;
    const char *names;
} public_contract_binding;

#define BOXRIG_PUBLIC_CONTRACT(kind, path, test_case, frontends, names) \
    {path, CONTRACT_##kind, #test_case, frontends, names},
static const public_contract_binding public_contract_bindings[] = {
#include "frontend_public_contracts.def"
};
#undef BOXRIG_PUBLIC_CONTRACT

typedef enum public_contract_audit_result {
    CONTRACT_AUDIT_OK,
    CONTRACT_AUDIT_MISSING,
    CONTRACT_AUDIT_ORPHAN,
    CONTRACT_AUDIT_DUPLICATE,
    CONTRACT_AUDIT_CLASSIFICATION,
    CONTRACT_AUDIT_INPUT
} public_contract_audit_result;

typedef struct contract_token {
    const char *text;
    size_t length;
} contract_token;

typedef struct contract_cursor {
    const char *text;
    bool valid;
} contract_cursor;

typedef struct contract_declaration {
    const char *path;
    contract_token name;
    public_contract_kind kind;
    bool bound;
} contract_declaration;

/* Exceeding either audit bound is an explicit failure, never truncated coverage. */
#define CONTRACT_MAX_DECLARATIONS 4096u
#define CONTRACT_MAX_ENUM_MEMBERS 512u

static bool starts_with(const char *value, const char *prefix);

static bool token_equals(contract_token token, const char *text)
{
    return token.length == strlen(text) && memcmp(token.text, text, token.length) == 0;
}

static bool token_prefix(contract_token token, const char *text)
{
    size_t length = strlen(text);
    return token.length >= length && memcmp(token.text, text, length) == 0;
}

static bool token_contains(contract_token token, const char *text)
{
    size_t length = strlen(text);
    size_t index;
    for (index = 0; index + length <= token.length; index++) {
        if (memcmp(token.text + index, text, length) == 0) {
            return true;
        }
    }
    return false;
}

static bool token_identifier(contract_token token)
{
    return token.length != 0u &&
        (isalpha((unsigned char)token.text[0]) || token.text[0] == '_');
}

static bool next_contract_token(contract_cursor *cursor, contract_token *token)
{
    const char *text = cursor->text;

    for (;;) {
        if (isspace((unsigned char)*text)) {
            text++;
        } else if (text[0] == '\\' && text[1] == '\n') {
            text += 2;
        } else if (text[0] == '/' && text[1] == '/') {
            while (*text != '\0' && *text != '\n') {
                text++;
            }
        } else if (text[0] == '/' && text[1] == '*') {
            text += 2;
            while (*text != '\0' && !(text[0] == '*' && text[1] == '/')) {
                text++;
            }
            if (*text == '\0') {
                cursor->valid = false;
                return false;
            }
            text += 2;
        } else {
            break;
        }
    }
    if (*text == '\0') {
        cursor->text = text;
        return false;
    }
    token->text = text;
    if (*text == '"' || *text == '\'') {
        char quote = *text++;
        while (*text != '\0' && *text != quote) {
            if (*text == '\\' && text[1] != '\0') {
                text++;
            }
            text++;
        }
        if (*text == '\0') {
            cursor->valid = false;
            return false;
        }
        text++;
    } else if (isalnum((unsigned char)*text) || *text == '_') {
        do {
            text++;
        } while (isalnum((unsigned char)*text) || *text == '_');
    } else {
        text++;
    }
    token->length = (size_t)(text - token->text);
    cursor->text = text;
    return true;
}

static int enum_contract_kind(contract_token name)
{
    if (token_contains(name, "capability") || token_contains(name, "CAPABILITY")) {
        return CONTRACT_CAPABILITY;
    }
    if (token_contains(name, "overlay_role") || token_contains(name, "_OVERLAY_")) {
        return CONTRACT_OVERLAY;
    }
    if (token_contains(name, "failure") || token_contains(name, "FAILURE")) {
        return CONTRACT_FAILURE;
    }
    return -1;
}

static bool add_contract_declaration(
    contract_declaration *declarations, size_t *count, const char *path,
    contract_token name, public_contract_kind kind)
{
    size_t index;
    for (index = 0; index < *count; index++) {
        if (strcmp(declarations[index].path, path) == 0 &&
            declarations[index].name.length == name.length &&
            memcmp(declarations[index].name.text, name.text, name.length) == 0) {
            return declarations[index].kind == kind;
        }
    }
    if (*count == CONTRACT_MAX_DECLARATIONS) {
        return false;
    }
    declarations[*count].path = path;
    declarations[*count].name = name;
    declarations[*count].kind = kind;
    declarations[*count].bound = false;
    (*count)++;
    return true;
}

static bool read_contract_enum(
    contract_cursor *cursor, contract_declaration *declarations,
    size_t *count, const char *path)
{
    contract_cursor next = *cursor;
    contract_token token;
    contract_token name = {"", 0u};
    contract_token members[CONTRACT_MAX_ENUM_MEMBERS];
    size_t member_count = 0u;
    size_t depth = 0u;
    size_t index;
    bool member_start = true;
    bool closed = false;
    int kind;

    if (!next_contract_token(&next, &token)) {
        return next.valid;
    }
    if (token_identifier(token)) {
        name = token;
        if (!next_contract_token(&next, &token)) {
            return next.valid;
        }
    }
    if (!token_equals(token, "{")) {
        return true;
    }
    while (next_contract_token(&next, &token)) {
        if (depth == 0u && token_equals(token, "}")) {
            closed = true;
            break;
        }
        if (member_start) {
            if (!token_identifier(token) || member_count == CONTRACT_MAX_ENUM_MEMBERS) {
                return false;
            }
            members[member_count++] = token;
            member_start = false;
        }
        if (token_equals(token, "(") || token_equals(token, "[") || token_equals(token, "{")) {
            depth++;
        } else if (token_equals(token, ")") || token_equals(token, "]") || token_equals(token, "}")) {
            if (depth == 0u) {
                return false;
            }
            depth--;
        } else if (depth == 0u && token_equals(token, ",")) {
            member_start = true;
        }
    }
    if (!closed || !next.valid) {
        return false;
    }
    *cursor = next;
    if (name.length == 0u && next_contract_token(&next, &token) && token_identifier(token)) {
        name = token;
    }
    kind = enum_contract_kind(name);
    for (index = 0; index < member_count; index++) {
        if (kind >= 0 && token_prefix(members[index], "BOXRIG_") &&
            !add_contract_declaration(declarations, count, path,
                members[index], (public_contract_kind)kind)) {
            return false;
        }
    }
    return true;
}

static bool read_contract_declarations(
    const public_contract_source *source, contract_declaration *declarations, size_t *count)
{
    contract_cursor cursor = {source->text, true};
    contract_token token;

    while (next_contract_token(&cursor, &token)) {
        contract_cursor lookahead = cursor;
        contract_token next;
        contract_token argument;
        if (token_equals(token, "enum")) {
            if (!read_contract_enum(&cursor, declarations, count, source->path)) {
                return false;
            }
        } else if (token_prefix(token, "boxrig_") &&
                   next_contract_token(&lookahead, &next) && token_equals(next, "(") &&
                   next_contract_token(&lookahead, &argument) && !token_equals(argument, "*")) {
            if (!add_contract_declaration(declarations, count, source->path,
                    token, CONTRACT_OPERATION)) {
                return false;
            }
        } else if (token_equals(token, "#") &&
                   next_contract_token(&lookahead, &next) && token_equals(next, "define") &&
                   next_contract_token(&lookahead, &argument) && token_prefix(argument, "BOXRIG_")) {
            int kind = enum_contract_kind(argument);
            if (kind >= 0 && !add_contract_declaration(declarations, count,
                    source->path, argument, (public_contract_kind)kind)) {
                return false;
            }
        }
    }
    return cursor.valid;
}

static unsigned int contract_frontends(const char *path)
{
    static const char *const platforms[] = {"x11", "win32", "gtk3", "gtk4", "wayland"};
    size_t index;
    if (strcmp(path, "include/boxrig/posix.h") == 0) {
        return 29u;
    }
    if (strcmp(path, "include/boxrig/render/d3d11.h") == 0) {
        return 2u;
    }
    for (index = 0; index < 5u; index++) {
        const char *relative = path + strlen("include/boxrig/");
        size_t length = strlen(platforms[index]);
        if (strncmp(relative, platforms[index], length) == 0 &&
            (relative[length] == '/' || strcmp(relative + length, ".h") == 0)) {
            return 1u << index;
        }
    }
    return 31u;
}

static bool valid_contract_cell(const conformance_cell *cell)
{
    if (cell->verdict < 0 || cell->verdict >= VERDICT_COUNT ||
        cell->target == NULL || cell->target[0] == '\0') {
        return false;
    }
    if (cell->verdict == VERDICT_UNSUPPORTED_BY_CONTRACT) {
        return starts_with(cell->target, "reason:") && cell->target[7] != '\0';
    }
    if (cell->verdict == VERDICT_P2) {
        return starts_with(cell->target, "test-p2") && cell->target[7] != '\0';
    }
    return !starts_with(cell->target, "reason:");
}

static public_contract_audit_result audit_public_contracts(
    const public_contract_source *sources, size_t source_count,
    const public_contract_binding *bindings, size_t binding_count,
    const conformance_case *matrix, size_t matrix_count,
    size_t *declaration_count, bool diagnostics)
{
    contract_declaration *declarations = calloc(CONTRACT_MAX_DECLARATIONS, sizeof(*declarations));
    public_contract_audit_result result = CONTRACT_AUDIT_INPUT;
    size_t source_index;
    size_t binding_index;
    const char *problem_path = "";
    contract_token problem_name = {"", 0u};
    *declaration_count = 0u;
    if (declarations == NULL) {
        return result;
    }
    for (source_index = 0; source_index < source_count; source_index++) {
        problem_path = sources[source_index].path;
        if (problem_path == NULL || !starts_with(problem_path, "include/boxrig/") ||
            sources[source_index].text == NULL ||
            !read_contract_declarations(&sources[source_index], declarations, declaration_count)) {
            goto done;
        }
    }
    for (binding_index = 0; binding_index < binding_count; binding_index++) {
        const public_contract_binding *binding = &bindings[binding_index];
        const conformance_case *test_case = NULL;
        contract_cursor names;
        size_t index;
        size_t bound_count = 0u;
        result = CONTRACT_AUDIT_CLASSIFICATION;
        problem_path = binding->path;
        if (binding->path == NULL || !starts_with(binding->path, "include/boxrig/") ||
            binding->names == NULL || binding->case_token == NULL ||
            binding->frontends != contract_frontends(binding->path)) {
            goto done;
        }
        for (index = 0; index < matrix_count; index++) {
            if (strcmp(matrix[index].token, binding->case_token) == 0) {
                test_case = &matrix[index];
                break;
            }
        }
        if (test_case == NULL) {
            goto done;
        }
        for (index = 0; index < 5u; index++) {
            if ((binding->frontends & (1u << index)) != 0u &&
                !valid_contract_cell(&test_case->cells[index])) {
                goto done;
            }
        }
        names.text = binding->names;
        names.valid = true;
        while (next_contract_token(&names, &problem_name)) {
            contract_declaration *declaration = NULL;
            if (!token_identifier(problem_name)) {
                goto done;
            }
            for (index = 0; index < *declaration_count; index++) {
                if (strcmp(declarations[index].path, binding->path) == 0 &&
                    declarations[index].name.length == problem_name.length &&
                    memcmp(declarations[index].name.text, problem_name.text, problem_name.length) == 0) {
                    declaration = &declarations[index];
                    break;
                }
            }
            if (declaration == NULL) {
                result = CONTRACT_AUDIT_ORPHAN;
                goto done;
            }
            if (declaration->kind != binding->kind) {
                goto done;
            }
            if (declaration->bound) {
                result = CONTRACT_AUDIT_DUPLICATE;
                goto done;
            }
            declaration->bound = true;
            bound_count++;
        }
        if (!names.valid || bound_count == 0u) {
            goto done;
        }
    }
    for (source_index = 0; source_index < *declaration_count; source_index++) {
        if (!declarations[source_index].bound) {
            result = CONTRACT_AUDIT_MISSING;
            problem_path = declarations[source_index].path;
            problem_name = declarations[source_index].name;
            goto done;
        }
    }
    result = CONTRACT_AUDIT_OK;
done:
    if (diagnostics && result != CONTRACT_AUDIT_OK) {
        fprintf(stderr, "FAIL public contract audit result %d: %s %.*s\n", result,
                problem_path != NULL ? problem_path : "(missing path)",
                (int)problem_name.length, problem_name.text);
    }
    free(declarations);
    return result;
}

static bool expect_audit_result(
    const char *name, public_contract_audit_result actual,
    public_contract_audit_result expected)
{
    if (actual != expected) {
        fprintf(stderr, "FAIL contract auditor %s: result %d, expected %d\n",
                name, actual, expected);
        return false;
    }
    return true;
}

static bool test_public_contract_auditor(void)
{
    static const public_contract_source additions[] = {
        {"include/boxrig/ui/probe.h", "int boxrig_probe(void);"},
        {"include/boxrig/ui/probe.h", "typedef enum boxrig_probe_capability { BOXRIG_PROBE_CAPABILITY_READ = 1u << 0 } boxrig_probe_capability;"},
        {"include/boxrig/ui/probe.h", "typedef enum boxrig_probe_overlay_role { BOXRIG_PROBE_OVERLAY_MARK = 1u << 1 } boxrig_probe_overlay_role;"},
        {"include/boxrig/ui/probe.h", "typedef enum { BOXRIG_PROBE_FAILURE_NATIVE = 0 } boxrig_probe_failure_reason;"}
    };
    static const char *const kinds[] = {"operation", "capability", "overlay", "failure"};
    static const public_contract_binding classified[] = {
        {"include/boxrig/ui/probe.h", CONTRACT_OPERATION, "COMPONENT_DIAGNOSTIC", 31u, "boxrig_probe"},
        {"include/boxrig/ui/probe.h", CONTRACT_CAPABILITY, "COMPONENT_DIAGNOSTIC", 31u, "BOXRIG_PROBE_CAPABILITY_READ"},
        {"include/boxrig/ui/probe.h", CONTRACT_OVERLAY, "COMPONENT_DIAGNOSTIC", 31u, "BOXRIG_PROBE_OVERLAY_MARK"},
        {"include/boxrig/ui/probe.h", CONTRACT_FAILURE, "COMPONENT_DIAGNOSTIC", 31u, "BOXRIG_PROBE_FAILURE_NATIVE"}
    };
    const public_contract_source ignored = {"include/boxrig/ui/probe.h",
        "/* int boxrig_comment(void); */ // int boxrig_line_comment(void);\n"
        "const char *example = \"boxrig_string()\"; char quote = '\\'';\n"
        "typedef boxrig_ui_status (*probe_callback)(void);"};
    const public_contract_source repeated = {"include/boxrig/ui/probe.h",
        "#define BOXRIG_TYPE_PROBE (boxrig_probe())\nint boxrig_probe(void);"};
    const public_contract_source native = {"include/boxrig/gtk3/probe.h",
        "int boxrig_probe(void);"};
    const public_contract_source macro = {"include/boxrig/ui/probe.h",
        "#define BOXRIG_PROBE_CAPABILITY_READ (1u << 0)\n"};
    public_contract_binding changed[5];
    conformance_case changed_case = cases[0];
    size_t declaration_count = 0u;
    size_t index;
    bool passed = true;

    for (index = 0; index < 4u; index++) {
        passed &= expect_audit_result(kinds[index], audit_public_contracts(
            &additions[index], 1u, NULL, 0u, cases, sizeof(cases) / sizeof(cases[0]),
            &declaration_count, false), CONTRACT_AUDIT_MISSING);
        passed &= expect_audit_result("classified declaration", audit_public_contracts(
            &additions[index], 1u, &classified[index], 1u,
            cases, sizeof(cases) / sizeof(cases[0]), &declaration_count, false), CONTRACT_AUDIT_OK);
        if (declaration_count != 1u) {
            fprintf(stderr, "FAIL contract auditor did not count the %s declaration\n", kinds[index]);
            passed = false;
        }
    }
    passed &= expect_audit_result("ignored tokens", audit_public_contracts(
        &ignored, 1u, NULL, 0u, cases, sizeof(cases) / sizeof(cases[0]),
        &declaration_count, false), CONTRACT_AUDIT_OK);
    if (declaration_count != 0u) {
        fprintf(stderr, "FAIL contract auditor counted a comment, literal, or callback return type\n");
        passed = false;
    }
    passed &= expect_audit_result("repeated public type macro and declaration", audit_public_contracts(
        &repeated, 1u, classified, 1u, cases, sizeof(cases) / sizeof(cases[0]),
        &declaration_count, false), CONTRACT_AUDIT_OK);
    if (declaration_count != 1u) {
        fprintf(stderr, "FAIL repeated declaration must retain one public operation\n");
        passed = false;
    }
    passed &= expect_audit_result("removed declaration", audit_public_contracts(
        &ignored, 1u, classified, 1u, cases, sizeof(cases) / sizeof(cases[0]),
        &declaration_count, false), CONTRACT_AUDIT_ORPHAN);
    passed &= expect_audit_result("capability macro addition", audit_public_contracts(
        &macro, 1u, NULL, 0u, cases, sizeof(cases) / sizeof(cases[0]),
        &declaration_count, false), CONTRACT_AUDIT_MISSING);
    passed &= expect_audit_result("classified capability macro", audit_public_contracts(
        &macro, 1u, &classified[1], 1u, cases, sizeof(cases) / sizeof(cases[0]),
        &declaration_count, false), CONTRACT_AUDIT_OK);
    memcpy(changed, classified, sizeof(classified));
    changed[4] = classified[0];
    passed &= expect_audit_result("duplicate binding", audit_public_contracts(
        additions, 4u, changed, 5u, cases, sizeof(cases) / sizeof(cases[0]),
        &declaration_count, false), CONTRACT_AUDIT_DUPLICATE);
    changed[0].frontends = 1u;
    passed &= expect_audit_result("missing applicable frontend", audit_public_contracts(
        additions, 4u, changed, 4u, cases, sizeof(cases) / sizeof(cases[0]),
        &declaration_count, false), CONTRACT_AUDIT_CLASSIFICATION);
    changed[0] = classified[0];
    changed[0].kind = CONTRACT_FAILURE;
    passed &= expect_audit_result("wrong contract kind", audit_public_contracts(
        additions, 4u, changed, 4u, cases, sizeof(cases) / sizeof(cases[0]),
        &declaration_count, false), CONTRACT_AUDIT_CLASSIFICATION);
    changed[0] = classified[0];
    changed[0].case_token = "MISSING_CASE";
    passed &= expect_audit_result("missing case", audit_public_contracts(
        additions, 4u, changed, 4u, cases, sizeof(cases) / sizeof(cases[0]),
        &declaration_count, false), CONTRACT_AUDIT_CLASSIFICATION);
    changed[0] = classified[0];
    changed[0].path = native.path;
    changed[0].frontends = 4u;
    passed &= expect_audit_result("native header applicability", audit_public_contracts(
        &native, 1u, changed, 1u, cases, sizeof(cases) / sizeof(cases[0]),
        &declaration_count, false), CONTRACT_AUDIT_OK);
    changed[0].frontends = 1u;
    passed &= expect_audit_result("wrong native frontend", audit_public_contracts(
        &native, 1u, changed, 1u, cases, sizeof(cases) / sizeof(cases[0]),
        &declaration_count, false), CONTRACT_AUDIT_CLASSIFICATION);

    changed_case.token = "COMPONENT_DIAGNOSTIC";
    changed_case.cells[2].target = "";
    passed &= expect_audit_result("unexplained frontend cell", audit_public_contracts(
        additions, 1u, classified, 1u, &changed_case, 1u,
        &declaration_count, false), CONTRACT_AUDIT_CLASSIFICATION);
    changed_case.cells[2].verdict = VERDICT_UNSUPPORTED_BY_CONTRACT;
    changed_case.cells[2].target = "reason:";
    passed &= expect_audit_result("empty unsupported reason", audit_public_contracts(
        additions, 1u, classified, 1u, &changed_case, 1u,
        &declaration_count, false), CONTRACT_AUDIT_CLASSIFICATION);
    changed_case.cells[2].target = "reason:no-native-contract";
    passed &= expect_audit_result("explicit unsupported contract", audit_public_contracts(
        additions, 1u, classified, 1u, &changed_case, 1u,
        &declaration_count, false), CONTRACT_AUDIT_OK);
    changed_case.cells[2].verdict = VERDICT_P2;
    changed_case.cells[2].target = "test-p2";
    passed &= expect_audit_result("missing concrete P2 target", audit_public_contracts(
        additions, 1u, classified, 1u, &changed_case, 1u,
        &declaration_count, false), CONTRACT_AUDIT_CLASSIFICATION);
    changed_case.cells[2].target = "test-p2-probe";
    passed &= expect_audit_result("explicit P2 gate", audit_public_contracts(
        additions, 1u, classified, 1u, &changed_case, 1u,
        &declaration_count, false), CONTRACT_AUDIT_OK);
    if (passed) {
        puts("PASS public contract auditor detects unclassified additions and invalid coverage");
    }
    return passed;
}

static bool test_frontend_specific_common_coverage(void)
{
    static const char *const names[] = {"X11", "Win32", "GTK 3", "GTK 4", "native Wayland"};
    size_t index;
    bool passed = true;

    for (index = 0; index < 5u; index++) {
        unsigned int required = 1u << index;
        if (boxrig_test_frontend_case_satisfied(names[index], required, false) ||
            !boxrig_test_frontend_case_satisfied(names[index], required, true) ||
            !boxrig_test_frontend_case_satisfied(names[index], 0u, false)) {
            fprintf(stderr, "FAIL %s must satisfy its own COMMON case\n", names[index]);
            passed = false;
        }
    }
    if (boxrig_test_frontend_case_satisfied("unknown", 0u, true)) {
        fprintf(stderr, "FAIL unknown frontend cannot satisfy coverage\n");
        passed = false;
    }
    if (passed) {
        puts("PASS shared coverage requires each frontend's own COMMON cases");
    }
    return passed;
}

static bool test_shutdown_contract_classification(void)
{
    const conformance_case *ownership = NULL;
    const conformance_case *pending = NULL;
    size_t index;

    for (index = 0; index < sizeof(cases) / sizeof(cases[0]); index++) {
        if (strcmp(cases[index].token, "SAMPLE_SHUTDOWN_OWNERSHIP") == 0) {
            ownership = &cases[index];
        }
        if (strcmp(cases[index].token, "SAMPLE_PENDING_CLIPBOARD") == 0) {
            pending = &cases[index];
        }
    }
    if (ownership == NULL || pending == NULL ||
        BOXRIG_TEST_FRONTEND_SAMPLE_SCENARIO_VERSION != 7) {
        fprintf(stderr, "FAIL shutdown ownership and pending read require versioned matrix cases\n");
        return false;
    }
    for (index = 0; index < 5u; index++) {
        if (ownership->cells[index].verdict != VERDICT_NATIVE ||
            pending->cells[index].verdict != (index == 1u
                ? VERDICT_UNSUPPORTED_BY_CONTRACT : VERDICT_NATIVE)) {
            fprintf(stderr, "FAIL shutdown classification for frontend %zu\n", index);
            return false;
        }
    }
    if (strcmp(pending->cells[1].target, "reason:Win32-clipboard-read-is-synchronous") != 0) {
        fprintf(stderr, "FAIL Win32 must retain its synchronous clipboard contract\n");
        return false;
    }
    puts("PASS shutdown coverage distinguishes synchronous and pending clipboard contracts");
    return true;
}

static bool starts_with(const char *value, const char *prefix)
{
    return value != NULL && prefix != NULL &&
           strncmp(value, prefix, strlen(prefix)) == 0;
}

static bool audit_matrix(void)
{
    size_t scope_counts[SCOPE_COUNT] = {0};
    size_t case_count = sizeof(cases) / sizeof(cases[0]);
    size_t case_index;

    if (case_count == 0) {
        fprintf(stderr, "FAIL conformance matrix is empty\n");
        return false;
    }
    for (case_index = 0; case_index < case_count; case_index++) {
        const conformance_case *current = &cases[case_index];
        size_t previous_index;
        size_t frontend_index;

        if (current->token == NULL || current->token[0] == '\0' ||
            current->id == NULL || current->id[0] == '\0' ||
            current->description == NULL || current->description[0] == '\0' ||
            current->scope < 0 || current->scope >= SCOPE_COUNT) {
            fprintf(stderr, "FAIL invalid conformance case at %zu\n", case_index);
            return false;
        }
        for (previous_index = 0; previous_index < case_index;
             previous_index++) {
            if (strcmp(current->token, cases[previous_index].token) == 0 ||
                strcmp(current->id, cases[previous_index].id) == 0) {
                fprintf(stderr, "FAIL duplicate conformance case %s\n", current->id);
                return false;
            }
        }
        scope_counts[current->scope]++;
        for (frontend_index = 0; frontend_index < 5; frontend_index++) {
            const conformance_cell *cell = &current->cells[frontend_index];

            if (!valid_contract_cell(cell)) {
                fprintf(
                    stderr,
                    "FAIL invalid or unexplained cell %s frontend %zu\n",
                    current->id,
                    frontend_index);
                return false;
            }
        }
    }
    for (case_index = 0; case_index < SCOPE_COUNT; case_index++) {
        if (scope_counts[case_index] == 0) {
            fprintf(stderr, "FAIL empty conformance scope %zu\n", case_index);
            return false;
        }
    }
    printf(
        "PASS %zu frontend conformance cases cover all %d scopes\n",
        case_count,
        SCOPE_COUNT);
    return true;
}

static bool audit_public_headers(int argument_count, char **arguments)
{
    public_contract_source *sources;
    size_t source_count;
    size_t index;
    size_t declaration_count = 0u;
    bool passed = false;

    if (argument_count < 2) {
        fprintf(stderr, "FAIL public contract audit requires all public header paths\n");
        return false;
    }
    source_count = (size_t)argument_count - 1u;
    sources = calloc(source_count, sizeof(*sources));
    if (sources == NULL) {
        return false;
    }
    for (index = 0; index < source_count; index++) {
        FILE *file;
        long length;
        char *text;
        sources[index].path = arguments[index + 1u];
        file = fopen(sources[index].path, "rb");
        if (file == NULL) {
            fprintf(stderr, "FAIL cannot read public header %s\n", sources[index].path);
            goto done;
        }
        if (fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) < 0 ||
            length > 16L * 1024L * 1024L || fseek(file, 0, SEEK_SET) != 0) {
            fclose(file);
            fprintf(stderr, "FAIL cannot size public header %s\n", sources[index].path);
            goto done;
        }
        text = malloc((size_t)length + 1u);
        sources[index].text = text;
        if (text == NULL || fread(text, 1u, (size_t)length, file) != (size_t)length) {
            fclose(file);
            fprintf(stderr, "FAIL incomplete public header %s\n", sources[index].path);
            goto done;
        }
        text[length] = '\0';
        if (fclose(file) != 0) {
            goto done;
        }
    }
    passed = audit_public_contracts(sources, source_count, public_contract_bindings,
        sizeof(public_contract_bindings) / sizeof(public_contract_bindings[0]),
        cases, sizeof(cases) / sizeof(cases[0]), &declaration_count, true) == CONTRACT_AUDIT_OK;
    if (passed) {
        printf("PASS %zu public operations and contract values classified across %zu headers\n",
                declaration_count, source_count);
    }
done:
    for (index = 0; index < source_count; index++) {
        free((void *)sources[index].text);
    }
    free(sources);
    return passed;
}

int main(int argument_count, char **arguments)
{
    bool passed = audit_matrix();
    passed &= test_public_contract_auditor();
    passed &= test_frontend_specific_common_coverage();
    passed &= test_shutdown_contract_classification();
    passed &= audit_public_headers(argument_count, arguments);
    return passed ? 0 : 1;
}
