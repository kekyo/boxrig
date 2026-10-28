$(BUILD_DIR)/examples/x11_transfer: examples/x11/transfer.c $(X11_LIBRARY) $(POSIX_LIBRARY) $(SESSION_LIBRARY) $(UI_LIBRARY) $(LIBRARY)
	mkdir -p $(BUILD_DIR)/examples
	$(CC) $(CPPFLAGS) $(FRONTEND_CPPFLAGS) $(POSIX_CPPFLAGS) $(X11_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $< $(X11_LIBRARY) $(POSIX_LIBRARY) $(SESSION_LIBRARY) $(UI_LIBRARY) $(LIBRARY) $(X11_LIBS) $(POSIX_LIBS) -o $@

.PHONY: test-x11-transfer
test-x11-transfer: $(BUILD_DIR)/examples/x11_transfer
	$(XVFB_RUN) -a $(BUILD_DIR)/examples/x11_transfer --verify
