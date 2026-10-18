PHASE_PROBE_BINARY := $(BUILD_DIR)/tests/performance/boxrig_term_phase
PHASE_STATE_TEST := $(BUILD_DIR)/tests/performance/test_phase_state

$(BUILD_DIR)/tests/performance:
	mkdir -p $@

$(PHASE_STATE_TEST): tests/performance/test_phase_state.c \
	tests/performance/phase_state.c tests/performance/phase_state.h \
	| $(BUILD_DIR)/tests/performance
	$(CC) -std=c99 -O2 -Wall -Wextra -Wpedantic -Werror \
		tests/performance/test_phase_state.c \
		tests/performance/phase_state.c -o $@

$(PHASE_PROBE_BINARY): $(GTK3_EXAMPLE_SOURCES) tests/performance/phase_probe.c \
	tests/performance/phase_state.c tests/performance/phase_state.h \
	$(GTK3_LIBRARY) $(NOTIFICATION_LIBRARY) $(POSIX_LIBRARY) \
	$(SESSION_LIBRARY) $(UI_LIBRARY) $(LIBRARY) \
	| $(BUILD_DIR)/tests/performance
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(POSIX_URI_CPPFLAGS) \
		$(POSIX_CPPFLAGS) $(GTK3_CPPFLAGS) $(CFLAGS) $(GTK3_PROJECT_CFLAGS) \
		$(GTK3_EXAMPLE_SOURCES) tests/performance/phase_probe.c \
		tests/performance/phase_state.c \
		$(GTK3_LIBRARY) $(NOTIFICATION_LIBRARY) $(POSIX_LIBRARY) \
		$(SESSION_LIBRARY) $(UI_LIBRARY) $(LIBRARY) \
		$(GTK3_LIBS) -lutil $(POSIX_URI_LIBS) \
		-Wl,--wrap=boxrig_term_text_policy_create \
		-Wl,--wrap=boxrig_term_gtk3_set_window_title \
		-Wl,--wrap=boxrig_term_text_policy_resume \
		-Wl,--wrap=boxrig_pango_cairo_renderer_draw_view_composition \
		-Wl,--wrap=boxrig_gtk3_terminal_widget_frame -o $@
