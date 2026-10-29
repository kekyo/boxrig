.PHONY: plan18-baseline
plan18-baseline: $(GTK3_EXAMPLE_BINARY) $(BUILD_DIR)/tests/performance/annotation_gtk_peer $(BUILD_DIR)/tests/performance/annotation_gtk_probe $(BUILD_DIR)/tests/performance/annotation_gl_vendor
	node tests/performance/run_plan18_baseline.mjs artifacts/plan18/baseline-xvfb
