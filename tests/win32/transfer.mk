$(MINGW_AMD64_BUILD_DIR)/tests/reparent_component.o: src/win32/component.c $(UI_HEADERS) $(WIN32_HEADERS) $(wildcard src/win32/*private.h) | $(MINGW_AMD64_BUILD_DIR)/tests
	$(MINGW_AMD64_CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(MINGW_WIN32_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) -DBOXRIG_TEST_REPARENT_FAILURE -c $< -o $@

$(MINGW_AMD64_BUILD_DIR)/tests/test_win32_reparent_failure.exe: tests/win32/test_win32_component.c $(MINGW_AMD64_BUILD_DIR)/tests/reparent_component.o $(MINGW_AMD64_WIN32_LIBRARY) $(MINGW_AMD64_UI_LIBRARY) $(MINGW_AMD64_LIBRARY) | $(MINGW_AMD64_BUILD_DIR)/tests
	$(MINGW_AMD64_CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(MINGW_WIN32_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) -DBOXRIG_TEST_REPARENT_FAILURE $< $(MINGW_AMD64_BUILD_DIR)/tests/reparent_component.o $(MINGW_AMD64_WIN32_LIBRARY) $(MINGW_AMD64_UI_LIBRARY) $(MINGW_AMD64_LIBRARY) $(WIN32_RENDERER_LIBS) -DBOXRIG_TEST_OMIT_OPTIONAL_INTERFACES -Wl,--wrap=boxrig_win32_input_adapter_resynchronize_candidate -Wl,--wrap=boxrig_win32_renderer_create -o $@

.PHONY: test-win32-reparent-failure
test: test-win32-reparent-failure
test-p2-win32: $(MINGW_AMD64_BUILD_DIR)/examples/win32_transfer.exe $(MINGW_I686_BUILD_DIR)/examples/win32_transfer.exe
test-win32-reparent-failure: $(MINGW_AMD64_BUILD_DIR)/tests/test_win32_reparent_failure.exe
	$(WINE_RUN) $(abspath $(MINGW_AMD64_BUILD_DIR)/tests/test_win32_reparent_failure.exe) $(abspath tests/fixtures/width-ascii.otf)

$(MINGW_AMD64_BUILD_DIR)/examples/win32_transfer.exe: examples/win32/transfer.c $(MINGW_AMD64_WIN32_LIBRARY) $(MINGW_AMD64_SESSION_LIBRARY) $(MINGW_AMD64_UI_LIBRARY) $(MINGW_AMD64_LIBRARY)
	mkdir -p $(MINGW_AMD64_BUILD_DIR)/examples
	$(MINGW_AMD64_CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(MINGW_WIN32_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $< $(MINGW_AMD64_WIN32_LIBRARY) $(MINGW_AMD64_SESSION_LIBRARY) $(MINGW_AMD64_UI_LIBRARY) $(MINGW_AMD64_LIBRARY) $(WIN32_RENDERER_LIBS) -ladvapi32 -lcomctl32 -o $@

$(MINGW_I686_BUILD_DIR)/examples/win32_transfer.exe: examples/win32/transfer.c $(MINGW_I686_WIN32_LIBRARY) $(MINGW_I686_SESSION_LIBRARY) $(MINGW_I686_UI_LIBRARY) $(MINGW_I686_LIBRARY)
	mkdir -p $(MINGW_I686_BUILD_DIR)/examples
	$(MINGW_I686_CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(MINGW_WIN32_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $< $(MINGW_I686_WIN32_LIBRARY) $(MINGW_I686_SESSION_LIBRARY) $(MINGW_I686_UI_LIBRARY) $(MINGW_I686_LIBRARY) $(WIN32_RENDERER_LIBS) -ladvapi32 -lcomctl32 -o $@
