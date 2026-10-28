RECOGNITION_COST_BINARY := $(BUILD_DIR)/tests/performance/recognition_cost
REGEX_COST_BINARY := $(BUILD_DIR)/tests/performance/regex_cost
REGEX_OBSERVED_BINARY := $(BUILD_DIR)/tests/performance/regex_cost_observed
RECOGNITION_PROFILE_BINARY := $(BUILD_DIR)/tests/performance/recognition_profile
RECOGNIZER_RESCAN_BINARY := $(BUILD_DIR)/tests/performance/recognizer_event_rescan
OSC8_COST_BINARY := $(BUILD_DIR)/tests/performance/osc8_cost
POLICY_COST_BINARY := $(BUILD_DIR)/tests/performance/policy_cost
RECOGNIZER_BUDGET_COST_BINARY := $(BUILD_DIR)/tests/performance/recognizer_budget_cost

$(RECOGNIZER_BUDGET_COST_BINARY): tests/performance/recognizer_budget_cost.c $(LIBRARY) | $(BUILD_DIR)/tests/performance
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(POSIX_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $< $(LIBRARY) -o $@

$(POLICY_COST_BINARY): tests/performance/policy_cost.c examples/boxrig_term/common/text_policy.c examples/boxrig_term/common/text_policy.h $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests/performance
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(POSIX_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) -Iexamples/boxrig_term/common $< examples/boxrig_term/common/text_policy.c $(UI_LIBRARY) $(LIBRARY) -Wl,--wrap=boxrig_terminal_register_text_recognizer -o $@

$(RECOGNITION_COST_BINARY): tests/performance/recognition_cost.c $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests/performance
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(POSIX_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $< $(UI_LIBRARY) $(LIBRARY) -o $@

$(RECOGNIZER_RESCAN_BINARY): tests/performance/recognizer_event_rescan.c $(LIBRARY) | $(BUILD_DIR)/tests/performance
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $< $(LIBRARY) -o $@

$(OSC8_COST_BINARY): tests/performance/osc8_cost.c $(LIBRARY) | $(BUILD_DIR)/tests/performance
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(POSIX_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $< $(LIBRARY) -o $@

.PHONY: recognition-drivers test-recognition-drivers
recognition-drivers: $(RECOGNITION_COST_BINARY) $(RECOGNIZER_RESCAN_BINARY) $(OSC8_COST_BINARY) $(POLICY_COST_BINARY) $(RECOGNIZER_BUDGET_COST_BINARY) $(REGEX_COST_BINARY) $(REGEX_OBSERVED_BINARY) $(RECOGNITION_PROFILE_BINARY)

test: test-recognition-drivers
test-recognition-drivers: recognition-drivers
	$(REGEX_COST_BINARY) http 64 match 2
	$(REGEX_COST_BINARY) unicode 64 miss 2
	$(REGEX_OBSERVED_BINARY) capture2 64 match 2
	$(RECOGNITION_COST_BINARY) 0 2 2 65536 64 2 80
	$(RECOGNITION_COST_BINARY) 10 3 1 0 4096 2 80
	$(RECOGNIZER_RESCAN_BINARY) 8
	$(OSC8_COST_BINARY) repeat live 10 10 40 4 16 20 1048576
	$(OSC8_COST_BINARY) unique dead 10 10 40 4 16 20 400
	$(POLICY_COST_BINARY) 10 2 5 4096
	$(RECOGNIZER_BUDGET_COST_BINARY) 1600 7

$(REGEX_COST_BINARY): tests/performance/regex_cost.c $(LIBRARY) | $(BUILD_DIR)/tests/performance
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $< $(LIBRARY) -o $@

$(REGEX_OBSERVED_BINARY): tests/performance/regex_cost.c tests/performance/regex_observe.c tests/performance/regex_observe.h $(LIBRARY) | $(BUILD_DIR)/tests/performance
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) -DBOXRIG_OBSERVE_REGEX $< tests/performance/regex_observe.c $(LIBRARY) -Wl,--wrap=utf8proc_category -Wl,--wrap=memcpy -o $@

$(RECOGNITION_PROFILE_BINARY): tests/performance/recognition_cost.c tests/performance/recognition_profile.c $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests/performance
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(POSIX_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $< tests/performance/recognition_profile.c $(UI_LIBRARY) $(LIBRARY) -Wl,--wrap=boxrig_regex_cursor_resume -Wl,--wrap=boxrig_text_line_projection_cursor_resume -Wl,--wrap=boxrig_terminal_resume_text_recognizers -o $@

.PHONY: test-regex-work
test: test-regex-work
test-regex-work: $(BUILD_DIR)/tests/test_regex_work
	$<

$(BUILD_DIR)/tests/test_regex_work: tests/test_regex_work.c tests/performance/regex_observe.c tests/performance/regex_observe.h $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $< tests/performance/regex_observe.c $(LIBRARY) -Wl,--wrap=utf8proc_category -Wl,--wrap=memcpy -o $@

$(BUILD_DIR)/tests/performance/regex_semantics: tests/performance/regex_semantics.c $(LIBRARY) | $(BUILD_DIR)/tests/performance
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $< $(LIBRARY) -o $@

.PHONY: test-regex-semantics
test: test-regex-semantics
test-regex-semantics: $(BUILD_DIR)/tests/performance/regex_semantics
	$< 1 > $(BUILD_DIR)/tests/performance/regex-budget-1.txt
	$< 7 > $(BUILD_DIR)/tests/performance/regex-budget-7.txt
	$< 4096 > $(BUILD_DIR)/tests/performance/regex-budget-4096.txt
	cmp $(BUILD_DIR)/tests/performance/regex-budget-1.txt $(BUILD_DIR)/tests/performance/regex-budget-7.txt
	cmp $(BUILD_DIR)/tests/performance/regex-budget-1.txt $(BUILD_DIR)/tests/performance/regex-budget-4096.txt

.PHONY: test-regex-allocations
test: test-regex-allocations
test-regex-allocations: $(BUILD_DIR)/tests/test_regex_allocations
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $<

$(BUILD_DIR)/tests/test_regex_allocations: tests/test_regex_allocations.c $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) $< $(LIBRARY) -Wl,--wrap=malloc -Wl,--wrap=calloc $(SANITIZERS) -o $@

.PHONY: test-annotation-eviction
test: test-annotation-eviction
test-annotation-eviction: $(BUILD_DIR)/tests/test_annotation_eviction
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $<

$(BUILD_DIR)/tests/test_annotation_eviction: tests/test_annotation_eviction.c $(UI_HEADERS) $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) $< $(UI_LIBRARY) $(LIBRARY) $(SANITIZERS) -o $@

.PHONY: test-annotation-updates
test: test-annotation-updates
test-annotation-updates: $(BUILD_DIR)/tests/test_annotation_updates
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $<

$(BUILD_DIR)/tests/test_annotation_updates: tests/test_annotation_updates.c $(UI_HEADERS) $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) $< $(UI_LIBRARY) $(LIBRARY) $(SANITIZERS) -Wl,--wrap=malloc -Wl,--wrap=realloc -o $@

.PHONY: test-recognizer-delivery
test: test-recognizer-delivery
test-recognizer-delivery: $(BUILD_DIR)/tests/test_recognizer_delivery
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $<

$(BUILD_DIR)/tests/test_recognizer_delivery: tests/test_recognizer_delivery.c $(CORE_HEADERS) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) $< $(LIBRARY) $(SANITIZERS) -Wl,--wrap=realloc -o $@

.PHONY: test-recognizer-lookup
test: test-recognizer-lookup
test-recognizer-lookup: $(BUILD_DIR)/tests/test_recognizer_lookup
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $<

$(BUILD_DIR)/tests/test_recognizer_lookup: tests/test_recognizer_lookup.c $(CORE_HEADERS) $(TEXT_MEASUREMENT_LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) -DBOXRIG_MEASURE_TEXT_WORKLOADS $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) $< $(TEXT_MEASUREMENT_LIBRARY) $(SANITIZERS) -o $@

.PHONY: test-text-projection-budget
test: test-text-projection-budget
test-text-projection-budget: $(BUILD_DIR)/tests/test_text_projection_budget
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $<

$(BUILD_DIR)/tests/test_text_projection_budget: tests/test_text_projection_budget.c $(CORE_HEADERS) $(TEXT_MEASUREMENT_LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) -DBOXRIG_MEASURE_TEXT_WORKLOADS $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) $< $(TEXT_MEASUREMENT_LIBRARY) $(SANITIZERS) -o $@

.PHONY: test-osc8-references
test: test-osc8-references
test-osc8-references: $(BUILD_DIR)/tests/test_osc8_references
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $<

$(BUILD_DIR)/tests/test_osc8_references: tests/test_osc8_references.c $(CORE_HEADERS) $(TEXT_MEASUREMENT_LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) -DBOXRIG_MEASURE_TEXT_WORKLOADS $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) -Wl,--wrap=calloc $< $(TEXT_MEASUREMENT_LIBRARY) $(SANITIZERS) -o $@
