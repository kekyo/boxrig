PHASE_PROBE_BINARY := $(BUILD_DIR)/tests/performance/boxrig_term_phase
PHASE_TRACE_BINARY := $(BUILD_DIR)/tests/performance/boxrig_term_trace
PHASE_STATE_TEST := $(BUILD_DIR)/tests/performance/test_phase_state

.PHONY: test-phase-trace
test: test-phase-trace
test-phase-trace: $(PHASE_TRACE_BINARY) $(PHASE_PROBE_BINARY)
	bash tests/performance/test_phase_trace.sh $(PHASE_TRACE_BINARY) $(PHASE_PROBE_BINARY) $(BUILD_DIR)/tests/performance

$(BUILD_DIR)/tests/performance:
	mkdir -p $@

$(PHASE_STATE_TEST): tests/performance/test_phase_state.c \
	tests/performance/phase_state.c tests/performance/phase_state.h \
	| $(BUILD_DIR)/tests/performance
	$(CC) -std=c99 -O2 -Wall -Wextra -Wpedantic -Werror \
		tests/performance/test_phase_state.c \
		tests/performance/phase_state.c -o $@

$(PHASE_TRACE_BINARY): PHASE_TRACE_SOURCES = tests/performance/phase_trace.c tests/performance/interval_trace.c
$(PHASE_TRACE_BINARY): PHASE_TRACE_FLAGS = -DBOXRIG_PHASE_TRACE \
	-Wl,--wrap=boxrig_session_dispatch -Wl,--wrap=boxrig_terminal_feed \
	-Wl,--wrap=boxrig_terminal_resume_text_recognizers \
	-Wl,--wrap=boxrig_text_line_projection_cursor_resume \
	-Wl,--wrap=boxrig_regex_cursor_resume -Wl,--wrap=boxrig_regex_cursor_create_result \
	-Wl,--wrap=read
$(PHASE_TRACE_BINARY): tests/performance/phase_trace.c tests/performance/interval_trace.c \
	tests/performance/interval_trace.h

$(PHASE_PROBE_BINARY) $(PHASE_TRACE_BINARY): $(GTK3_EXAMPLE_SOURCES) tests/performance/phase_probe.c \
	tests/performance/phase_state.c tests/performance/phase_state.h \
	$(GTK3_LIBRARY) $(NOTIFICATION_LIBRARY) $(POSIX_LIBRARY) \
	$(SESSION_LIBRARY) $(UI_LIBRARY) $(LIBRARY) \
	| $(BUILD_DIR)/tests/performance
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(POSIX_URI_CPPFLAGS) \
		$(POSIX_CPPFLAGS) $(GTK3_CPPFLAGS) $(CFLAGS) $(GTK3_PROJECT_CFLAGS) \
		$(GTK3_EXAMPLE_SOURCES) tests/performance/phase_probe.c \
		tests/performance/phase_state.c $(PHASE_TRACE_SOURCES) $(PHASE_TRACE_FLAGS) \
		$(GTK3_LIBRARY) $(NOTIFICATION_LIBRARY) $(POSIX_LIBRARY) \
		$(SESSION_LIBRARY) $(UI_LIBRARY) $(LIBRARY) \
		$(GTK3_LIBS) -lutil $(POSIX_URI_LIBS) \
		-Wl,--wrap=boxrig_term_text_policy_create \
		-Wl,--wrap=boxrig_term_gtk3_set_window_title \
		-Wl,--wrap=boxrig_term_text_policy_resume \
		-Wl,--wrap=boxrig_pango_cairo_renderer_draw_view_composition \
		-Wl,--wrap=boxrig_gtk3_terminal_widget_frame -o $@
