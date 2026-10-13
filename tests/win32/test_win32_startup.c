/* Exercise the GUI executable's actual startup output destinations. */
#include <windows.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#define CHECK(value) do { if (!(value)) { \
    fprintf(stderr, "FAIL startup line %d: %s (error %lu)\n", \
        __LINE__, #value, (unsigned long)GetLastError()); return 1; \
} } while (0)

static bool phase_has_work(const char *bytes, const char *name)
{
    char marker[80];
    const char *line;
    unsigned long long calls, ticks, cycles;

    if (snprintf(marker, sizeof(marker),
            "boxrig_term: phase=%s calls=", name) <= 0) return false;
    line = strstr(bytes, marker);
    if (line == NULL) return false;
    return sscanf(line + strlen(marker),
        "%llu qpc-ticks=%llu cycles=%llu", &calls, &ticks, &cycles) == 3 &&
        calls > 0 && ticks > 0 && cycles > 0;
}

static int run(const wchar_t *executable, unsigned int mode,
    bool phase_trace, bool renderer_trace, bool work_trace,
    bool snapshot_trace, bool raster_detail_trace, bool row_state_trace)
{
    SECURITY_ATTRIBUTES security = {sizeof(security), NULL, TRUE};
    STARTUPINFOW startup = {0};
    PROCESS_INFORMATION process = {0};
    wchar_t command[32768];
    HANDLE output = NULL, input = NULL;
    DWORD count = 0, exit_code = 1;
    char bytes[65536] = {0};
    const char expected[] = "boxrig_term: renderer=win32";
    unsigned long long work_values[6] = {0};
    startup.cb = sizeof(startup);
    CHECK(SetEnvironmentVariableW(L"BOXRIG_PHASE_TRACE",
        phase_trace ? L"1" : NULL));
    CHECK(SetEnvironmentVariableW(L"BOXRIG_RENDERER_TRACE",
        renderer_trace ? L"1" : NULL));
    CHECK(SetEnvironmentVariableW(L"BOXRIG_WORK_TRACE",
        work_trace ? L"1" : NULL));
    CHECK(SetEnvironmentVariableW(L"BOXRIG_WORK_SNAPSHOT_TRACE",
        snapshot_trace ? L"1" : NULL));
    CHECK(SetEnvironmentVariableW(L"BOXRIG_RASTER_DETAIL_TRACE",
        raster_detail_trace ? L"1" : NULL));
    CHECK(SetEnvironmentVariableW(L"BOXRIG_ROW_STATE_TRACE",
        row_state_trace ? L"1" : NULL));
    CHECK(swprintf(command, sizeof(command) / sizeof(command[0]),
        L"\"%ls\" --renderer=win32 -- cmd.exe /d /c \"echo child-only-marker & exit 0\"", executable) > 0);
    if (mode == 0) {
        (void)FreeConsole();
        CHECK(AllocConsole());
        output = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        CHECK(output != INVALID_HANDLE_VALUE);
        CHECK(SetConsoleScreenBufferSize(output, (COORD){160, 100}));
        CHECK(FillConsoleOutputCharacterA(output, ' ', 16000, (COORD){0, 0}, &count));
        CHECK(SetConsoleCursorPosition(output, (COORD){0, 0}));
    } else if (mode == 1) {
        output = CreateFileW(L"boxrig-startup-output.tmp", GENERIC_WRITE | GENERIC_READ,
            FILE_SHARE_READ, &security, CREATE_ALWAYS,
            FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
        CHECK(output != INVALID_HANDLE_VALUE);
        input = output;
    } else if (mode == 2) {
        CHECK(CreatePipe(&input, &output, &security, 0));
        CHECK(SetHandleInformation(input, HANDLE_FLAG_INHERIT, 0));
    }
    if (mode == 1 || mode == 2) {
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdOutput = output;
        startup.hStdError = output;
        startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    }
    CHECK(CreateProcessW(executable, command, NULL, NULL, mode == 1 || mode == 2,
        mode == 3 ? DETACHED_PROCESS : 0, NULL, NULL, &startup, &process));
    CloseHandle(process.hThread);
    if (WaitForSingleObject(process.hProcess, 60000) != WAIT_OBJECT_0) {
        (void)TerminateProcess(process.hProcess, 1);
        (void)WaitForSingleObject(process.hProcess, INFINITE);
        CloseHandle(process.hProcess);
        CHECK(0 && "GUI executable did not finish");
    }
    CHECK(GetExitCodeProcess(process.hProcess, &exit_code));
    CloseHandle(process.hProcess);
    CHECK(exit_code == 0);
    if (mode == 0) {
        CHECK(ReadConsoleOutputCharacterA(output, bytes, sizeof(bytes) - 1,
            (COORD){0, 0}, &count));
        CloseHandle(output);
        CHECK(FreeConsole());
    } else if (mode == 1 || mode == 2) {
        if (mode == 1) CHECK(SetFilePointer(input, 0, NULL, FILE_BEGIN) == 0);
        else CloseHandle(output);
        CHECK(ReadFile(input, bytes, sizeof(bytes) - 1, &count, NULL));
        CloseHandle(input);
    }
    if (mode != 3) {
        char *found = strstr(bytes, expected);
        CHECK(found != NULL);
        CHECK(strstr(found + sizeof(expected) - 1, expected) == NULL);
        CHECK(strstr(bytes, "boxrig_term: graphics-api=DirectWrite/GDI") != NULL);
        CHECK(strstr(bytes, "child-only-marker") == NULL);
        if (phase_trace) {
            CHECK(strstr(bytes, "boxrig_term: phase-trace-version=1") != NULL);
            if (strstr(bytes, "boxrig_term: phase-trace-status=ok") != NULL) {
                CHECK(phase_has_work(bytes, "session-dispatch"));
                CHECK(phase_has_work(bytes, "component-frame"));
            } else {
                CHECK(strstr(bytes,
                    "boxrig_term: phase-trace-status=missing reason=") != NULL);
                CHECK(strstr(bytes, "boxrig_term: phase=") == NULL);
            }
        } else {
            CHECK(strstr(bytes, "boxrig_term: phase-trace-") == NULL);
            CHECK(strstr(bytes, "boxrig_term: phase=") == NULL);
        }
        if (renderer_trace) {
            CHECK(strstr(bytes, "boxrig_term: renderer-phase-version=1") != NULL);
            if (strstr(bytes, "boxrig_term: renderer-phase-status=ok") != NULL) {
                CHECK(strstr(bytes, "boxrig_term: renderer-phase-total ") != NULL);
                CHECK(strstr(bytes,
                    "boxrig_term: renderer-phase-update index=0") != NULL);
            } else {
                CHECK(strstr(bytes,
                    "boxrig_term: renderer-phase-status=missing"
                    " reason=thread-cycle-unavailable") != NULL);
                CHECK(strstr(bytes, "boxrig_term: renderer-phase-total ") == NULL);
                CHECK(strstr(bytes,
                    "boxrig_term: renderer-phase-update index=") == NULL);
            }
        } else {
            CHECK(strstr(bytes, "boxrig_term: renderer-phase-") == NULL);
        }
        if (work_trace) {
            unsigned long long signatures, hits, misses, shapes;
            unsigned long long rows, cells, presents;
            const char *summary = strstr(bytes,
                "boxrig_term: renderer-work row-signatures=");
            CHECK(strstr(bytes,
                "boxrig_term: renderer-work-version=1") != NULL);
            CHECK(summary != NULL);
            CHECK(sscanf(summary,
                "boxrig_term: renderer-work row-signatures=%llu"
                " row-cache-hits=%llu row-cache-misses=%llu"
                " row-shapes=%llu row-rasters=%llu cell-rasters=%llu"
                " presents=%llu", &signatures, &hits, &misses,
                &shapes, &rows, &cells, &presents) == 7);
            CHECK(signatures == hits + misses);
            CHECK(shapes >= misses);
            work_values[0] = signatures;
            work_values[1] = hits;
            work_values[2] = misses;
            work_values[3] = shapes;
            work_values[4] = rows;
            work_values[5] = cells;
            CHECK(strstr(bytes,
                "boxrig_term: renderer-work-status=ok") != NULL);
        } else {
            CHECK(strstr(bytes, "boxrig_term: renderer-work-") == NULL);
        }
        if (snapshot_trace) {
            const char *last = NULL;
            const char *next = bytes;
            unsigned long long index, ended_qpc;
            unsigned long long values[6];
            unsigned long long reasons[4];
            unsigned long long updates, phase_index, phase_started_qpc;
            unsigned long long phase_ended_qpc;
            char marker[96];
            const char *phase_update;
            CHECK(strstr(bytes,
                "boxrig_term: renderer-tally-version=2") != NULL);
            if (strstr(bytes, "boxrig_term: renderer-phase-status=ok") == NULL) {
                CHECK(strstr(bytes,
                    "boxrig_term: renderer-tally-status=missing"
                    " reason=thread-cycle-unavailable") != NULL);
                CHECK(strstr(bytes, "renderer-tally-total ") == NULL);
                CHECK(strstr(bytes, "renderer-tally-update index=") == NULL);
            } else {
                const char *total = strstr(bytes,
                    "boxrig_term: renderer-tally-total updates=");
                CHECK(total != NULL);
                CHECK(sscanf(total,
                    "boxrig_term: renderer-tally-total updates=%llu",
                    &updates) == 1);
                CHECK(updates > 0);
                while ((next = strstr(next,
                    "boxrig_term: renderer-tally-update index=")) != NULL) {
                    last = next;
                    next++;
                }
                CHECK(last != NULL);
                CHECK(sscanf(last,
                    "boxrig_term: renderer-tally-update index=%llu"
                    " ended-qpc=%llu row-signatures=%llu"
                    " row-cache-hits=%llu row-cache-misses=%llu"
                    " miss-invalid=%llu miss-layout=%llu"
                    " miss-direction=%llu miss-content=%llu"
                    " row-shapes=%llu row-rasters=%llu cell-rasters=%llu",
                    &index, &ended_qpc, &values[0], &values[1],
                    &values[2], &reasons[0], &reasons[1],
                    &reasons[2], &reasons[3], &values[3],
                    &values[4], &values[5]) == 12);
                CHECK(index + 1 == updates);
                CHECK(values[2] == reasons[0] + reasons[1] +
                    reasons[2] + reasons[3]);
                for (unsigned int value = 0; value < 6; value++)
                    CHECK(values[value] == work_values[value]);
                CHECK(snprintf(marker, sizeof(marker),
                    "boxrig_term: renderer-phase-update index=%llu started-qpc=",
                    index) > 0);
                phase_update = strstr(bytes, marker);
                CHECK(phase_update != NULL);
                CHECK(sscanf(phase_update,
                    "boxrig_term: renderer-phase-update index=%llu"
                    " started-qpc=%llu ended-qpc=%llu",
                    &phase_index, &phase_started_qpc,
                    &phase_ended_qpc) == 3);
                CHECK(phase_index == index);
                CHECK(phase_ended_qpc == ended_qpc);
                CHECK(strstr(bytes,
                    "boxrig_term: renderer-tally-status=ok") != NULL);
            }
        } else {
            CHECK(strstr(bytes, "boxrig_term: renderer-tally-") == NULL);
        }
        if (raster_detail_trace) {
            CHECK(strstr(bytes, "boxrig_term: raster-detail-version=1") != NULL);
            if (strstr(bytes, "boxrig_term: renderer-phase-status=ok") != NULL) {
                CHECK(strstr(bytes, "boxrig_term: raster-detail-total updates=") != NULL);
                CHECK(strstr(bytes, "boxrig_term: raster-detail-update index=0") != NULL);
                CHECK(strstr(bytes, "boxrig_term: raster-detail-status=ok") != NULL);
            } else {
                CHECK(strstr(bytes,
                    "boxrig_term: raster-detail-status=missing"
                    " reason=thread-cycle-unavailable") != NULL);
                CHECK(strstr(bytes, "boxrig_term: raster-detail-total ") == NULL);
            }
        } else {
            CHECK(strstr(bytes, "boxrig_term: raster-detail-") == NULL);
        }
        if (row_state_trace) {
            const char *total = strstr(bytes,
                "boxrig_term: renderer-row-state-total updates=");
            unsigned long long updates, comparisons;
            CHECK(strstr(bytes,
                "boxrig_term: renderer-row-state-version=1") != NULL);
            if (strstr(bytes, "boxrig_term: renderer-phase-status=ok") != NULL) {
                CHECK(total != NULL);
                CHECK(sscanf(total,
                    "boxrig_term: renderer-row-state-total updates=%llu"
                    " comparisons=%llu", &updates, &comparisons) == 2);
                CHECK(updates > 0);
                CHECK(comparisons == work_values[0]);
                CHECK(strstr(bytes,
                    "boxrig_term: renderer-row-state-update index=0") != NULL);
                CHECK(strstr(bytes,
                    "boxrig_term: renderer-row-state-status=ok") != NULL);
            } else {
                CHECK(strstr(bytes,
                    "boxrig_term: renderer-row-state-status=missing"
                    " reason=thread-cycle-unavailable") != NULL);
                CHECK(total == NULL);
            }
        } else {
            CHECK(strstr(bytes, "boxrig_term: renderer-row-state-") == NULL);
        }
    }
    return 0;
}

int wmain(int count, wchar_t **arguments)
{
    unsigned int mode;
    CHECK(count == 2);
    CHECK(SetEnvironmentVariableW(L"BOXRIG_BACKGROUND_OPACITY", L"100"));
    for (mode = 0; mode < 4; mode++)
        CHECK(run(arguments[1], mode, false, false, false, false, false, false) == 0);
    CHECK(run(arguments[1], 1, true, false, false, false, false, false) == 0);
    CHECK(run(arguments[1], 1, false, true, false, false, false, false) == 0);
    CHECK(run(arguments[1], 1, false, false, true, false, false, false) == 0);
    CHECK(run(arguments[1], 1, false, true, true, true, false, false) == 0);
    CHECK(run(arguments[1], 1, false, true, true, true, true, false) == 0);
    CHECK(run(arguments[1], 1, false, true, true, true, false, true) == 0);
    CHECK(SetEnvironmentVariableW(L"BOXRIG_PHASE_TRACE", NULL));
    CHECK(SetEnvironmentVariableW(L"BOXRIG_RENDERER_TRACE", NULL));
    CHECK(SetEnvironmentVariableW(L"BOXRIG_WORK_TRACE", NULL));
    CHECK(SetEnvironmentVariableW(L"BOXRIG_WORK_SNAPSHOT_TRACE", NULL));
    CHECK(SetEnvironmentVariableW(L"BOXRIG_RASTER_DETAIL_TRACE", NULL));
    CHECK(SetEnvironmentVariableW(L"BOXRIG_ROW_STATE_TRACE", NULL));
    puts("PASS Win32 GUI startup console, file, pipe, and detached launch");
    return 0;
}
