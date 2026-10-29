$(BUILD_DIR)/examples/gtk3_transfer: examples/gtk/transfer.c $(GTK3_LIBRARY) $(POSIX_LIBRARY) $(SESSION_LIBRARY) $(UI_LIBRARY) $(LIBRARY)
	mkdir -p $(BUILD_DIR)/examples
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(POSIX_CPPFLAGS) $(GTK3_CPPFLAGS) $(CFLAGS) $(GTK3_PROJECT_CFLAGS) -DBOXRIG_TRANSFER_GTK3 $< $(GTK3_LIBRARY) $(POSIX_LIBRARY) $(SESSION_LIBRARY) $(UI_LIBRARY) $(LIBRARY) $(GTK3_LIBS) -o $@

$(BUILD_DIR)/examples/gtk4_transfer: examples/gtk/transfer.c $(GTK4_LIBRARY) $(POSIX_LIBRARY) $(SESSION_LIBRARY) $(UI_LIBRARY) $(LIBRARY)
	mkdir -p $(BUILD_DIR)/examples
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(POSIX_CPPFLAGS) $(GTK4_CPPFLAGS) $(CFLAGS) $(GTK4_PROJECT_CFLAGS) -DBOXRIG_TRANSFER_GTK4 $< $(GTK4_LIBRARY) $(POSIX_LIBRARY) $(SESSION_LIBRARY) $(UI_LIBRARY) $(LIBRARY) $(GTK4_LIBS) -o $@

.PHONY: test-gtk-transfer test-gtk-transfer-x11 test-gtk-transfer-wayland
test: test-gtk-transfer
test-gtk-transfer: test-gtk-transfer-x11 test-gtk-transfer-wayland

test-gtk-transfer-x11: $(BUILD_DIR)/examples/gtk3_transfer $(BUILD_DIR)/examples/gtk4_transfer
	$(XVFB_RUN) -a env GDK_BACKEND=x11 NO_AT_BRIDGE=1 $(BUILD_DIR)/examples/gtk3_transfer --verify
	$(XVFB_RUN) -a env GDK_BACKEND=x11 GSK_RENDERER=cairo NO_AT_BRIDGE=1 $(BUILD_DIR)/examples/gtk4_transfer --verify
	$(XVFB_RUN) -a env GDK_BACKEND=x11 NO_AT_BRIDGE=1 LIBGL_ALWAYS_SOFTWARE=1 $(BUILD_DIR)/examples/gtk3_transfer --verify --gl
	$(XVFB_RUN) -a env GDK_BACKEND=x11 GSK_RENDERER=gl NO_AT_BRIDGE=1 LIBGL_ALWAYS_SOFTWARE=1 $(BUILD_DIR)/examples/gtk4_transfer --verify --gl

test-gtk-transfer-wayland: $(BUILD_DIR)/examples/gtk3_transfer $(BUILD_DIR)/examples/gtk4_transfer
	$(XVFB_RUN) -a env BOXRIG_TEST_WESTON_BACKEND=x11 bash tests/gtk/run_gl_wayland.sh $(BUILD_DIR)/examples/gtk3_transfer --verify
	$(XVFB_RUN) -a env BOXRIG_TEST_WESTON_BACKEND=x11 bash tests/gtk/run_gl_wayland.sh $(BUILD_DIR)/examples/gtk4_transfer --verify
	$(XVFB_RUN) -a env BOXRIG_TEST_WESTON_BACKEND=x11 bash tests/gtk/run_gl_wayland.sh $(BUILD_DIR)/examples/gtk3_transfer --verify --gl
	$(XVFB_RUN) -a env BOXRIG_TEST_WESTON_BACKEND=x11 bash tests/gtk/run_gl_wayland.sh env GSK_RENDERER=gl $(BUILD_DIR)/examples/gtk4_transfer --verify --gl
