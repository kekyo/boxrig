RECOGNITION_COST_BINARY := $(BUILD_DIR)/tests/performance/recognition_cost
RECOGNIZER_RESCAN_BINARY := $(BUILD_DIR)/tests/performance/recognizer_event_rescan
OSC8_COST_BINARY := $(BUILD_DIR)/tests/performance/osc8_cost

$(RECOGNITION_COST_BINARY): tests/performance/recognition_cost.c $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests/performance
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(POSIX_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $< $(UI_LIBRARY) $(LIBRARY) -o $@

$(RECOGNIZER_RESCAN_BINARY): tests/performance/recognizer_event_rescan.c $(LIBRARY) | $(BUILD_DIR)/tests/performance
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $< $(LIBRARY) -o $@

$(OSC8_COST_BINARY): tests/performance/osc8_cost.c $(LIBRARY) | $(BUILD_DIR)/tests/performance
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(POSIX_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $< $(LIBRARY) -o $@

.PHONY: recognition-drivers test-recognition-drivers
recognition-drivers: $(RECOGNITION_COST_BINARY) $(RECOGNIZER_RESCAN_BINARY) $(OSC8_COST_BINARY)

test: test-recognition-drivers
test-recognition-drivers: recognition-drivers
	$(RECOGNITION_COST_BINARY) 0 2 2 65536 64 2 80
	$(RECOGNITION_COST_BINARY) 10 3 1 0 4096 2 80
	$(RECOGNIZER_RESCAN_BINARY) 8
	$(OSC8_COST_BINARY) repeat live 10 10 40 4 16 20 1048576
	$(OSC8_COST_BINARY) unique dead 10 10 40 4 16 20 400
