RECOGNITION_COST_BINARY := $(BUILD_DIR)/tests/performance/recognition_cost
RECOGNIZER_RESCAN_BINARY := $(BUILD_DIR)/tests/performance/recognizer_event_rescan
OSC8_COST_BINARY := $(BUILD_DIR)/tests/performance/osc8_cost
POLICY_COST_BINARY := $(BUILD_DIR)/tests/performance/policy_cost

$(POLICY_COST_BINARY): tests/performance/policy_cost.c examples/boxrig_term/common/text_policy.c examples/boxrig_term/common/text_policy.h $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests/performance
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(POSIX_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) -Iexamples/boxrig_term/common $< examples/boxrig_term/common/text_policy.c $(UI_LIBRARY) $(LIBRARY) -Wl,--wrap=boxrig_terminal_register_text_recognizer -o $@

$(RECOGNITION_COST_BINARY): tests/performance/recognition_cost.c $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests/performance
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(POSIX_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $< $(UI_LIBRARY) $(LIBRARY) -o $@

$(RECOGNIZER_RESCAN_BINARY): tests/performance/recognizer_event_rescan.c $(LIBRARY) | $(BUILD_DIR)/tests/performance
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $< $(LIBRARY) -o $@

$(OSC8_COST_BINARY): tests/performance/osc8_cost.c $(LIBRARY) | $(BUILD_DIR)/tests/performance
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(POSIX_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $< $(LIBRARY) -o $@

.PHONY: recognition-drivers test-recognition-drivers
recognition-drivers: $(RECOGNITION_COST_BINARY) $(RECOGNIZER_RESCAN_BINARY) $(OSC8_COST_BINARY) $(POLICY_COST_BINARY)

test: test-recognition-drivers
test-recognition-drivers: recognition-drivers
	$(RECOGNITION_COST_BINARY) 0 2 2 65536 64 2 80
	$(RECOGNITION_COST_BINARY) 10 3 1 0 4096 2 80
	$(RECOGNIZER_RESCAN_BINARY) 8
	$(OSC8_COST_BINARY) repeat live 10 10 40 4 16 20 1048576
	$(OSC8_COST_BINARY) unique dead 10 10 40 4 16 20 400
	$(POLICY_COST_BINARY) 10 2 5 4096

.PHONY: test-annotation-eviction
test: test-annotation-eviction
test-annotation-eviction: $(BUILD_DIR)/tests/test_annotation_eviction
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $<

$(BUILD_DIR)/tests/test_annotation_eviction: tests/test_annotation_eviction.c $(UI_HEADERS) $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) $< $(UI_LIBRARY) $(LIBRARY) $(SANITIZERS) -Wl,--wrap=malloc -o $@

.PHONY: test-recognizer-delivery
test: test-recognizer-delivery
test-recognizer-delivery: $(BUILD_DIR)/tests/test_recognizer_delivery
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $<

$(BUILD_DIR)/tests/test_recognizer_delivery: tests/test_recognizer_delivery.c $(CORE_HEADERS) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) $< $(LIBRARY) $(SANITIZERS) -Wl,--wrap=realloc -o $@
