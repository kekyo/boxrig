.PHONY: plan18-baseline
plan18-baseline: $(GTK3_EXAMPLE_BINARY) $(BUILD_DIR)/tests/performance/annotation_gtk_peer $(BUILD_DIR)/tests/performance/annotation_gtk_probe $(BUILD_DIR)/tests/performance/annotation_gl_vendor
	node tests/performance/run_plan18_baseline.mjs artifacts/plan18/baseline-xvfb

$(BUILD_DIR)/tests/performance/plan18_region_bench: tests/performance/plan18_region_bench.c tests/test_ui_platform_fixture.c tests/opengl_host.c $(OPENGL_RENDERER_LIBRARY) $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests/performance
	$(CC) $(CPPFLAGS) -Isrc -Itests $(PROJECT_CPPFLAGS) $(POSIX_CPPFLAGS) $(OPENGL_RENDERER_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $< tests/opengl_host.c $(OPENGL_RENDERER_LIBRARY) $(UI_LIBRARY) $(LIBRARY) $(OPENGL_RENDERER_LIBS) -Wl,--wrap=boxrig_gl_runtime_create -Wl,--gc-sections -o $@

$(BUILD_DIR)/tests/performance/plan18_cairo_region_bench: tests/performance/plan18_region_bench.c tests/test_ui_platform_fixture.c src/pangocairo/renderer.c $(PANGO_CANDIDATE_OBJECT) $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests/performance
	$(CC) $(CPPFLAGS) -DBOXRIG_PLAN18_CAIRO=1 -Isrc $(PROJECT_CPPFLAGS) $(POSIX_CPPFLAGS) $(PANGO_CAIRO_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $< src/pangocairo/renderer.c $(PANGO_CANDIDATE_OBJECT) $(UI_LIBRARY) $(LIBRARY) $(PANGO_CAIRO_LIBS) -o $@
