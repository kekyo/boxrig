/* Exercise only installed public interfaces, in both C99 and C++20. */
#include <boxrig/terminal.h>
#include <boxrig/regex.h>
#include <boxrig/ui.h>
#include <boxrig/session.h>
#include <boxrig/posix.h>

#include <assert.h>
#include <stdio.h>
#include <sys/wait.h>
#if !defined(BOXRIG_TEST_STATIC)
#include <dlfcn.h>
#endif

int main(void)
{
    const uint8_t pattern[] = "(?<path>a)?b";
    boxrig_regex_limits limits = boxrig_regex_limits_default();
    boxrig_text_regex_diagnostic regex_diagnostic;
    boxrig_regex_program *program = NULL;
    assert(boxrig_regex_compile(pattern, sizeof(pattern) - 1, &limits,
               &regex_diagnostic, &program) == BOXRIG_STATUS_OK);
    bool found = false;
    size_t group = 0;
    assert(boxrig_regex_program_find_named_group(program, "path", &found, &group) ==
           BOXRIG_STATUS_OK);
    assert(found && group == 1);
    assert(boxrig_regex_program_find_named_group(program, "missing", &found, &group) ==
           BOXRIG_STATUS_OK);
    assert(!found && group == 0);
    boxrig_regex_program_destroy(program);

    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    assert(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *view = NULL;
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    assert(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    assert(boxrig_ui_view_create(model, &view_config, &view) == BOXRIG_UI_OK);
    assert(boxrig_ui_model_terminal(model) == terminal);
    boxrig_ui_rectangular_copy_policy copy_policy;
    assert(boxrig_ui_view_get_rectangular_copy_policy(view, &copy_policy) ==
           BOXRIG_UI_OK);
    assert(copy_policy == BOXRIG_UI_RECTANGULAR_COPY_TRIM_TRAILING_SPACES);
    assert(boxrig_ui_view_set_rectangular_copy_policy(
               view, BOXRIG_UI_RECTANGULAR_COPY_PRESERVE_SPACES) == BOXRIG_UI_OK);
    assert(boxrig_ui_view_get_rectangular_copy_policy(view, &copy_policy) ==
           BOXRIG_UI_OK);
    assert(copy_policy == BOXRIG_UI_RECTANGULAR_COPY_PRESERVE_SPACES);

    char shell[] = "/bin/sh", option[] = "-c", command[] = "exit 127";
    char *arguments[] = {shell, option, command, NULL};
    boxrig_posix_pty *pty = NULL;
    boxrig_posix_pty_spawn_result diagnostic;
    assert(boxrig_posix_pty_spawn_with_diagnostic(arguments, "xterm", 24, 80,
        &pty, &diagnostic) == 0);
    boxrig_session *session = NULL;
    boxrig_session_config session_config = boxrig_session_config_default();
    assert(boxrig_posix_pty_session_create(terminal, pty, &session_config,
        &session) == BOXRIG_SESSION_OK);
    assert(boxrig_session_resize(session, 12, 40) == BOXRIG_SESSION_OK);
    int status;
    assert(boxrig_posix_pty_wait(pty, &status) == 0);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 127);
    assert(boxrig_posix_pty_get_spawn_result(pty, &diagnostic) == 0);
    assert(diagnostic.state == BOXRIG_POSIX_PTY_SPAWN_SUCCEEDED);
    boxrig_session_destroy(session);
    boxrig_posix_pty_destroy(pty);
    boxrig_ui_view_destroy(view);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);

#if !defined(BOXRIG_TEST_STATIC)
    void *core = dlopen("libboxrig.so.0", RTLD_NOW | RTLD_LOCAL);
    assert(core != NULL);
    assert(dlsym(core, "boxrig_terminal_create") != NULL);
    assert(dlsym(core, "png_create_read_struct") == NULL);
    assert(dlsym(core, "inflate") == NULL);
    assert(dlsym(core, "utf8proc_map") == NULL);
    assert(dlclose(core) == 0);
#endif
    puts("PASS installed core, UI, session and POSIX APIs");
    return 0;
}
