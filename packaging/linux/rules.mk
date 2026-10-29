# Shared objects use the existing source lists in an isolated PIC build.
# Keep the ABI number independent of package/release version metadata.
BOXRIG_VERSION ?= 0.0.0
BOXRIG_SOVERSION := 0
SHARED_PKGCONFIG_DIR := $(if $(BOXRIG_PREGENERATED_PKGCONFIG),$(BOXRIG_PREGENERATED_PKGCONFIG),$(BUILD_DIR)/pkgconfig)
SHARED_INSTALL_PKGCONFIG_DIR := $(if $(BOXRIG_PREGENERATED_PKGCONFIG),$(BOXRIG_PREGENERATED_PKGCONFIG),$(BUILD_DIR)/shared/pkgconfig)
SHARED_BASE_NAMES := boxrig boxrig-ui boxrig-session boxrig-posix
SHARED_BASE_LINKS := $(addprefix $(BUILD_DIR)/lib,$(addsuffix .so,$(SHARED_BASE_NAMES)))
SHARED_BASE_METADATA := $(addprefix $(SHARED_PKGCONFIG_DIR)/,$(addsuffix .pc,$(SHARED_BASE_NAMES)))
SHARED_GTK_NAMES := boxrig-gtk3 boxrig-gtk4
SHARED_GTK_LINKS := $(addprefix $(BUILD_DIR)/lib,$(addsuffix .so,$(SHARED_GTK_NAMES)))
SHARED_GTK_METADATA := $(addprefix $(SHARED_PKGCONFIG_DIR)/,$(addsuffix .pc,$(SHARED_GTK_NAMES)))
SHARED_NATIVE_NAMES := boxrig-x11 boxrig-wayland
SHARED_NATIVE_LINKS := $(addprefix $(BUILD_DIR)/lib,$(addsuffix .so,$(SHARED_NATIVE_NAMES)))
SHARED_NATIVE_METADATA := $(addprefix $(SHARED_PKGCONFIG_DIR)/,$(addsuffix .pc,$(SHARED_NATIVE_NAMES)))
SHARED_OPENGL_NAMES := boxrig-render-ogl boxrig-x11-ogl boxrig-wayland-ogl
SHARED_OPENGL_LINKS := $(addprefix $(BUILD_DIR)/lib,$(addsuffix .so,$(SHARED_OPENGL_NAMES)))
SHARED_OPENGL_METADATA := $(addprefix $(SHARED_PKGCONFIG_DIR)/,$(addsuffix .pc,$(SHARED_OPENGL_NAMES)))
SHARED_LDFLAGS := -shared -Wl,-z,defs,-z,text,--no-undefined-version,-z,origin -Wl,-rpath,'$$ORIGIN'
SHARED_MAP_FLAGS = -Wl,--version-script=packaging/linux/$(patsubst lib%.so.$(BOXRIG_SOVERSION),%,$(@F)).map -Wl,-soname,$(@F)
SHARED_ARCHIVE_FLAGS = -Wl,--whole-archive $< -Wl,--no-whole-archive

.PHONY: shared-base install-shared-base _shared-base test-shared-base
.PHONY: shared shared-gtk3 shared-gtk4 install-shared install-shared-gtk3 install-shared-gtk4 _shared-gtk3 _shared-gtk4 test-shared-gtk
shared-base: $(if $(BOXRIG_PREGENERATED_PKGCONFIG),,codegen-tools)
	+$(MAKE) BUILD_DIR="$(BUILD_DIR)/shared" CODEGEN_DIR="$(CODEGEN_DIR)" BOXRIG_NATIVE_SHARED=1 _shared-base

_shared-base: $(SHARED_BASE_LINKS) $(SHARED_BASE_METADATA)
shared: shared-base
	+$(MAKE) BUILD_DIR="$(BUILD_DIR)/shared" CODEGEN_DIR="$(CODEGEN_DIR)" BOXRIG_NATIVE_SHARED=1 _shared-gtk3 _shared-gtk4
shared-gtk3 shared-gtk4: shared-%: shared-base
	+$(MAKE) BUILD_DIR="$(BUILD_DIR)/shared" CODEGEN_DIR="$(CODEGEN_DIR)" BOXRIG_NATIVE_SHARED=1 _shared-$*
_shared-gtk3: $(BUILD_DIR)/libboxrig-gtk3.so $(SHARED_PKGCONFIG_DIR)/boxrig-gtk3.pc
_shared-gtk4: $(BUILD_DIR)/libboxrig-gtk4.so $(SHARED_PKGCONFIG_DIR)/boxrig-gtk4.pc

.PHONY: shared-native _shared-native install-shared-native test-shared-native
shared-native: shared-base
	+$(MAKE) BUILD_DIR="$(BUILD_DIR)/shared" CODEGEN_DIR="$(CODEGEN_DIR)" BOXRIG_NATIVE_SHARED=1 _shared-native
_shared-native: $(SHARED_NATIVE_LINKS) $(SHARED_NATIVE_METADATA)

.PHONY: shared-opengl _shared-opengl install-shared-opengl test-shared-opengl
shared-opengl: shared-native
	+$(MAKE) BUILD_DIR="$(BUILD_DIR)/shared" CODEGEN_DIR="$(CODEGEN_DIR)" BOXRIG_NATIVE_SHARED=1 _shared-opengl
_shared-opengl: $(SHARED_OPENGL_LINKS) $(SHARED_OPENGL_METADATA)

ifeq ($(BOXRIG_NATIVE_SHARED),1)
override CFLAGS += -fPIC
SHARED_COMPILE_SIGNATURE := $(CC) | $(CPPFLAGS) | $(CFLAGS) | $(LDFLAGS) | $(GTK_OPENGL)
ifneq ($(file <$(BUILD_DIR)/shared-compile.config),$(SHARED_COMPILE_SIGNATURE))
.PHONY: force-shared-compile-config
$(BUILD_DIR)/shared-compile.config: force-shared-compile-config
endif
$(BUILD_DIR)/shared-compile.config: | $(BUILD_DIR)
	@printf '%s\n' '$(SHARED_COMPILE_SIGNATURE)' > $@
$(CORE_OBJECTS) $(UTF8PROC_OBJECT) $(LIBPNG_OBJECTS) $(ZLIB_OBJECTS) $(UI_OBJECTS) $(SESSION_OBJECTS) $(POSIX_OBJECTS) $(GTK3_OBJECTS) $(GTK4_OBJECTS) $(X11_OBJECTS) $(WAYLAND_OBJECTS) $(OPENGL_RENDERER_OBJECTS) $(X11_OPENGL_OBJECTS) $(WAYLAND_OPENGL_OBJECTS): $(BUILD_DIR)/shared-compile.config packaging/linux/rules.mk
endif

ifeq ($(BOXRIG_PREGENERATED_PKGCONFIG),)
SHARED_METADATA_SIGNATURE := $(BOXRIG_VERSION) | $(PREFIX) | $(LIBDIR) | $(INCLUDEDIR) | $(PKGCONFIGDIR) | $(GTK_OPENGL)
ifneq ($(file <$(BUILD_DIR)/shared-metadata.config),$(SHARED_METADATA_SIGNATURE))
.PHONY: force-shared-metadata-config
$(BUILD_DIR)/shared-metadata.config: force-shared-metadata-config
endif
$(BUILD_DIR)/shared-metadata.config: | $(BUILD_DIR)
	@printf '%s\n' '$(SHARED_METADATA_SIGNATURE)' > $@

$(SHARED_BASE_METADATA) &: $(addprefix packaging/linux/,$(addsuffix .pc.in,$(SHARED_BASE_NAMES))) $(CODEGEN_DIR)/generate-linux-metadata.mjs $(BUILD_DIR)/shared-metadata.config
	node $(CODEGEN_DIR)/generate-linux-metadata.mjs "$(BUILD_DIR)/pkgconfig" "$(BOXRIG_VERSION)" "$(PREFIX)" "$(LIBDIR)" "$(INCLUDEDIR)" "$(PKGCONFIGDIR)" $(SHARED_BASE_NAMES)

$(SHARED_GTK_METADATA) $(SHARED_NATIVE_METADATA) $(SHARED_OPENGL_METADATA): $(BUILD_DIR)/pkgconfig/%.pc: packaging/linux/%.pc.in $(CODEGEN_DIR)/generate-linux-metadata.mjs $(BUILD_DIR)/shared-metadata.config
	BOXRIG_GTK_OPENGL=$(GTK_OPENGL) node $(CODEGEN_DIR)/generate-linux-metadata.mjs "$(BUILD_DIR)/pkgconfig" "$(BOXRIG_VERSION)" "$(PREFIX)" "$(LIBDIR)" "$(INCLUDEDIR)" "$(PKGCONFIGDIR)" $*
endif

$(BUILD_DIR)/libboxrig.so.$(BOXRIG_SOVERSION): $(LIBRARY) packaging/linux/boxrig.map packaging/linux/rules.mk
	$(CC) $(CFLAGS) $(LDFLAGS) $(SHARED_LDFLAGS) $(SHARED_MAP_FLAGS) $(SHARED_ARCHIVE_FLAGS) -lm -o $@

$(BUILD_DIR)/libboxrig-ui.so.$(BOXRIG_SOVERSION): $(UI_LIBRARY) packaging/linux/boxrig-ui.map $(BUILD_DIR)/libboxrig.so packaging/linux/rules.mk
	$(CC) $(CFLAGS) $(LDFLAGS) $(SHARED_LDFLAGS) $(SHARED_MAP_FLAGS) $(SHARED_ARCHIVE_FLAGS) -L$(BUILD_DIR) -lboxrig -lm -o $@

$(BUILD_DIR)/libboxrig-session.so.$(BOXRIG_SOVERSION): $(SESSION_LIBRARY) packaging/linux/boxrig-session.map $(BUILD_DIR)/libboxrig.so packaging/linux/rules.mk
	$(CC) $(CFLAGS) $(LDFLAGS) $(SHARED_LDFLAGS) $(SHARED_MAP_FLAGS) $(SHARED_ARCHIVE_FLAGS) -L$(BUILD_DIR) -lboxrig -o $@

$(BUILD_DIR)/libboxrig-posix.so.$(BOXRIG_SOVERSION): $(POSIX_LIBRARY) packaging/linux/boxrig-posix.map $(BUILD_DIR)/libboxrig-session.so packaging/linux/rules.mk
	$(CC) $(CFLAGS) $(LDFLAGS) $(SHARED_LDFLAGS) $(SHARED_MAP_FLAGS) $(SHARED_ARCHIVE_FLAGS) -L$(BUILD_DIR) -lboxrig-session -lutil -o $@

$(SHARED_BASE_LINKS) $(SHARED_GTK_LINKS) $(SHARED_NATIVE_LINKS) $(SHARED_OPENGL_LINKS): %.so: %.so.$(BOXRIG_SOVERSION)
	ln -sfn $(<F) $@

$(BUILD_DIR)/libboxrig-gtk3.so.$(BOXRIG_SOVERSION): $(GTK3_LIBRARY) packaging/linux/boxrig-gtk3.map $(BUILD_DIR)/libboxrig-ui.so packaging/linux/rules.mk
	$(CC) $(CFLAGS) $(LDFLAGS) $(SHARED_LDFLAGS) $(SHARED_MAP_FLAGS) $(SHARED_ARCHIVE_FLAGS) -L$(BUILD_DIR) -lboxrig-ui -lboxrig $(GTK3_LIBS) $(if $(filter 1,$(GTK_OPENGL)),$(OPENGL_RENDERER_LIBS)) -o $@

$(BUILD_DIR)/libboxrig-gtk4.so.$(BOXRIG_SOVERSION): $(GTK4_LIBRARY) packaging/linux/boxrig-gtk4.map $(BUILD_DIR)/libboxrig-ui.so packaging/linux/rules.mk
	$(CC) $(CFLAGS) $(LDFLAGS) $(SHARED_LDFLAGS) $(SHARED_MAP_FLAGS) $(SHARED_ARCHIVE_FLAGS) -L$(BUILD_DIR) -lboxrig-ui -lboxrig $(GTK4_LIBS) $(if $(filter 1,$(GTK_OPENGL)),$(OPENGL_RENDERER_LIBS)) -o $@

$(BUILD_DIR)/libboxrig-x11.so.$(BOXRIG_SOVERSION): $(X11_LIBRARY) packaging/linux/boxrig-x11.map $(BUILD_DIR)/libboxrig-ui.so packaging/linux/rules.mk
	$(CC) $(CFLAGS) $(LDFLAGS) $(SHARED_LDFLAGS) $(SHARED_MAP_FLAGS) $(SHARED_ARCHIVE_FLAGS) -L$(BUILD_DIR) -lboxrig-ui -lboxrig $(X11_LIBS) -lm -o $@

$(BUILD_DIR)/libboxrig-wayland.so.$(BOXRIG_SOVERSION): $(WAYLAND_LIBRARY) packaging/linux/boxrig-wayland.map $(BUILD_DIR)/libboxrig-ui.so packaging/linux/rules.mk
	$(CC) $(CFLAGS) $(LDFLAGS) $(SHARED_LDFLAGS) $(SHARED_MAP_FLAGS) $(SHARED_ARCHIVE_FLAGS) -L$(BUILD_DIR) -lboxrig-ui -lboxrig $(WAYLAND_CLIENT_LIBS) $(PANGO_CAIRO_LIBS) -lm -o $@

$(BUILD_DIR)/libboxrig-render-ogl.so.$(BOXRIG_SOVERSION): $(OPENGL_RENDERER_LIBRARY) packaging/linux/boxrig-render-ogl.map $(BUILD_DIR)/libboxrig-ui.so packaging/linux/rules.mk
	$(CC) $(CFLAGS) $(LDFLAGS) $(SHARED_LDFLAGS) $(SHARED_MAP_FLAGS) $(SHARED_ARCHIVE_FLAGS) -L$(BUILD_DIR) -lboxrig-ui -lboxrig $(OPENGL_RENDERER_LIBS) -o $@

$(BUILD_DIR)/libboxrig-x11-ogl.so.$(BOXRIG_SOVERSION): $(X11_OPENGL_LIBRARY) packaging/linux/boxrig-x11-ogl.map $(BUILD_DIR)/libboxrig-x11.so $(BUILD_DIR)/libboxrig-render-ogl.so packaging/linux/rules.mk
	$(CC) $(CFLAGS) $(LDFLAGS) $(SHARED_LDFLAGS) $(SHARED_MAP_FLAGS) $(SHARED_ARCHIVE_FLAGS) -L$(BUILD_DIR) -lboxrig-x11 -lboxrig-render-ogl -lboxrig-ui -lboxrig $(X11_LIBS) $(OPENGL_LIBS) -lm -o $@

$(BUILD_DIR)/libboxrig-wayland-ogl.so.$(BOXRIG_SOVERSION): $(WAYLAND_OPENGL_LIBRARY) packaging/linux/boxrig-wayland-ogl.map $(BUILD_DIR)/libboxrig-wayland.so $(BUILD_DIR)/libboxrig-render-ogl.so packaging/linux/rules.mk
	$(CC) $(CFLAGS) $(LDFLAGS) $(SHARED_LDFLAGS) $(SHARED_MAP_FLAGS) $(SHARED_ARCHIVE_FLAGS) -L$(BUILD_DIR) -lboxrig-wayland -lboxrig-render-ogl -lboxrig-ui $(WAYLAND_OPENGL_LIBS) -o $@

install-shared-base: shared-base install-common-licenses
	$(INSTALL) -d $(DESTDIR)$(LIBDIR) $(DESTDIR)$(INCLUDEDIR)/boxrig/ui $(DESTDIR)$(PKGCONFIGDIR)
	$(INSTALL) -m 644 $(CORE_PUBLIC_HEADERS) include/boxrig/ui.h include/boxrig/session.h include/boxrig/posix.h $(DESTDIR)$(INCLUDEDIR)/boxrig
	$(INSTALL) -m 644 include/boxrig/ui/*.h $(DESTDIR)$(INCLUDEDIR)/boxrig/ui
	@set -e; for name in $(SHARED_BASE_NAMES); do \
		$(INSTALL) -m 644 "$(BUILD_DIR)/shared/lib$$name.a" "$(BUILD_DIR)/shared/lib$$name.so.$(BOXRIG_SOVERSION)" "$(DESTDIR)$(LIBDIR)"; \
		ln -sfn "lib$$name.so.$(BOXRIG_SOVERSION)" "$(DESTDIR)$(LIBDIR)/lib$$name.so"; \
		$(INSTALL) -m 644 "$(SHARED_INSTALL_PKGCONFIG_DIR)/$$name.pc" "$(DESTDIR)$(PKGCONFIGDIR)"; \
	done

define install_shared_gtk
	$(INSTALL) -d $(DESTDIR)$(INCLUDEDIR)/boxrig/$(1) $(DESTDIR)$(INCLUDEDIR)/boxrig/gtk $(DESTDIR)$(PREFIX)/share/boxrig/examples/$(1)
	$(INSTALL) -m 644 include/boxrig/$(1).h include/boxrig/ime.h $(DESTDIR)$(INCLUDEDIR)/boxrig
	$(INSTALL) -m 644 include/boxrig/$(1)/*.h $(DESTDIR)$(INCLUDEDIR)/boxrig/$(1)
	$(INSTALL) -m 644 include/boxrig/gtk/*.h $(DESTDIR)$(INCLUDEDIR)/boxrig/gtk
	$(INSTALL) -m 644 examples/$(1)/external_scrollbar.h $(DESTDIR)$(PREFIX)/share/boxrig/examples/$(1)
	$(INSTALL) -m 644 $(BUILD_DIR)/shared/libboxrig-$(1).a $(BUILD_DIR)/shared/libboxrig-$(1).so.$(BOXRIG_SOVERSION) $(DESTDIR)$(LIBDIR)
	ln -sfn libboxrig-$(1).so.$(BOXRIG_SOVERSION) $(DESTDIR)$(LIBDIR)/libboxrig-$(1).so
	$(INSTALL) -m 644 $(SHARED_INSTALL_PKGCONFIG_DIR)/boxrig-$(1).pc $(DESTDIR)$(PKGCONFIGDIR)
endef

install-shared: shared install-shared-base
	$(call install_shared_gtk,gtk3)
	$(call install_shared_gtk,gtk4)
install-shared-gtk3 install-shared-gtk4: install-shared-%: shared-% install-shared-base
	$(call install_shared_gtk,$*)

install-shared-native: shared-native install-shared-base
	$(INSTALL) -d $(DESTDIR)$(INCLUDEDIR)/boxrig/x11 $(DESTDIR)$(PREFIX)/share/boxrig/examples/x11 $(DESTDIR)$(PREFIX)/share/boxrig/examples/wayland
	$(INSTALL) -m 644 include/boxrig/x11.h include/boxrig/wayland.h include/boxrig/ime.h $(DESTDIR)$(INCLUDEDIR)/boxrig
	$(INSTALL) -m 644 $(filter-out include/boxrig/x11/opengl_component.h,$(wildcard include/boxrig/x11/*.h)) $(DESTDIR)$(INCLUDEDIR)/boxrig/x11
	$(INSTALL) -m 644 examples/x11/external_scrollbar.h $(DESTDIR)$(PREFIX)/share/boxrig/examples/x11
	$(INSTALL) -m 644 examples/wayland/external_scrollbar.h $(DESTDIR)$(PREFIX)/share/boxrig/examples/wayland
	@set -e; for name in $(SHARED_NATIVE_NAMES); do \
		$(INSTALL) -m 644 "$(BUILD_DIR)/shared/lib$$name.a" "$(BUILD_DIR)/shared/lib$$name.so.$(BOXRIG_SOVERSION)" "$(DESTDIR)$(LIBDIR)"; \
		ln -sfn "lib$$name.so.$(BOXRIG_SOVERSION)" "$(DESTDIR)$(LIBDIR)/lib$$name.so"; \
		$(INSTALL) -m 644 "$(SHARED_INSTALL_PKGCONFIG_DIR)/$$name.pc" "$(DESTDIR)$(PKGCONFIGDIR)"; \
	done

test: test-shared-native
test-shared-native:
	bash tests/shared/check_native.sh "$(MAKE)"

install-shared-opengl: shared-opengl install-shared-native
	$(INSTALL) -d $(DESTDIR)$(INCLUDEDIR)/boxrig/render $(DESTDIR)$(INCLUDEDIR)/boxrig/wayland
	$(INSTALL) -m 644 $(OPENGL_RENDERER_HEADERS) $(DESTDIR)$(INCLUDEDIR)/boxrig/render
	$(INSTALL) -m 644 include/boxrig/x11/opengl_component.h $(DESTDIR)$(INCLUDEDIR)/boxrig/x11
	$(INSTALL) -m 644 include/boxrig/wayland/opengl_component.h $(DESTDIR)$(INCLUDEDIR)/boxrig/wayland
	@set -e; for name in $(SHARED_OPENGL_NAMES); do \
		$(INSTALL) -m 644 "$(BUILD_DIR)/shared/lib$$name.a" "$(BUILD_DIR)/shared/lib$$name.so.$(BOXRIG_SOVERSION)" "$(DESTDIR)$(LIBDIR)"; \
		ln -sfn "lib$$name.so.$(BOXRIG_SOVERSION)" "$(DESTDIR)$(LIBDIR)/lib$$name.so"; \
		$(INSTALL) -m 644 "$(SHARED_INSTALL_PKGCONFIG_DIR)/$$name.pc" "$(DESTDIR)$(PKGCONFIGDIR)"; \
	done

test: test-shared-opengl
test-shared-opengl:
	bash tests/shared/check_opengl.sh "$(MAKE)"

test: test-shared-base
test-shared-base:
	bash tests/shared/check_base.sh "$(MAKE)"
test: test-shared-gtk
test-shared-gtk:
	bash tests/shared/check_gtk.sh "$(MAKE)"
.PHONY: test-shared-prebuilt
test: test-shared-prebuilt
test-shared-prebuilt:
	bash tests/shared/check_prebuilt.sh "$(MAKE)"
