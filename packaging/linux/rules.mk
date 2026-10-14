# Shared objects use the existing source lists in an isolated PIC build.
# Keep the ABI number independent of package/release version metadata.
BOXRIG_VERSION ?= 0.0.0
BOXRIG_SOVERSION := 0
SHARED_BASE_NAMES := boxrig boxrig-ui boxrig-session boxrig-posix
SHARED_BASE_LINKS := $(addprefix $(BUILD_DIR)/lib,$(addsuffix .so,$(SHARED_BASE_NAMES)))
SHARED_BASE_METADATA := $(addprefix $(BUILD_DIR)/pkgconfig/,$(addsuffix .pc,$(SHARED_BASE_NAMES)))
SHARED_LDFLAGS := -shared -Wl,-z,defs,-z,text,--no-undefined-version,-z,origin -Wl,-rpath,'$$ORIGIN'
SHARED_MAP_FLAGS = -Wl,--version-script=packaging/linux/$(patsubst lib%.so.$(BOXRIG_SOVERSION),%,$(@F)).map -Wl,-soname,$(@F)
SHARED_ARCHIVE_FLAGS = -Wl,--whole-archive $< -Wl,--no-whole-archive

.PHONY: shared-base install-shared-base _shared-base test-shared-base
shared-base:
	+$(MAKE) BUILD_DIR="$(BUILD_DIR)/shared" BOXRIG_NATIVE_SHARED=1 _shared-base

_shared-base: $(SHARED_BASE_LINKS) $(SHARED_BASE_METADATA)

ifeq ($(BOXRIG_NATIVE_SHARED),1)
override CFLAGS += -fPIC
SHARED_COMPILE_SIGNATURE := $(CC) | $(CPPFLAGS) | $(CFLAGS) | $(LDFLAGS) | $(GTK_OPENGL)
ifneq ($(file <$(BUILD_DIR)/shared-compile.config),$(SHARED_COMPILE_SIGNATURE))
.PHONY: force-shared-compile-config
$(BUILD_DIR)/shared-compile.config: force-shared-compile-config
endif
$(BUILD_DIR)/shared-compile.config: | $(BUILD_DIR)
	@printf '%s\n' '$(SHARED_COMPILE_SIGNATURE)' > $@
$(CORE_OBJECTS) $(UTF8PROC_OBJECT) $(LIBPNG_OBJECTS) $(ZLIB_OBJECTS) $(UI_OBJECTS) $(SESSION_OBJECTS) $(POSIX_OBJECTS): $(BUILD_DIR)/shared-compile.config packaging/linux/rules.mk
endif

SHARED_METADATA_SIGNATURE := $(BOXRIG_VERSION) | $(PREFIX) | $(LIBDIR) | $(INCLUDEDIR) | $(PKGCONFIGDIR)
ifneq ($(file <$(BUILD_DIR)/shared-metadata.config),$(SHARED_METADATA_SIGNATURE))
.PHONY: force-shared-metadata-config
$(BUILD_DIR)/shared-metadata.config: force-shared-metadata-config
endif
$(BUILD_DIR)/shared-metadata.config: | $(BUILD_DIR)
	@printf '%s\n' '$(SHARED_METADATA_SIGNATURE)' > $@

$(SHARED_BASE_METADATA) &: $(addprefix packaging/linux/,$(addsuffix .pc.in,$(SHARED_BASE_NAMES))) $(CODEGEN_DIR)/generate-linux-metadata.mjs $(BUILD_DIR)/shared-metadata.config
	node $(CODEGEN_DIR)/generate-linux-metadata.mjs "$(BUILD_DIR)/pkgconfig" "$(BOXRIG_VERSION)" "$(PREFIX)" "$(LIBDIR)" "$(INCLUDEDIR)" "$(PKGCONFIGDIR)" $(SHARED_BASE_NAMES)

$(BUILD_DIR)/libboxrig.so.$(BOXRIG_SOVERSION): $(LIBRARY) packaging/linux/boxrig.map packaging/linux/rules.mk
	$(CC) $(CFLAGS) $(LDFLAGS) $(SHARED_LDFLAGS) $(SHARED_MAP_FLAGS) $(SHARED_ARCHIVE_FLAGS) -lm -o $@

$(BUILD_DIR)/libboxrig-ui.so.$(BOXRIG_SOVERSION): $(UI_LIBRARY) packaging/linux/boxrig-ui.map $(BUILD_DIR)/libboxrig.so packaging/linux/rules.mk
	$(CC) $(CFLAGS) $(LDFLAGS) $(SHARED_LDFLAGS) $(SHARED_MAP_FLAGS) $(SHARED_ARCHIVE_FLAGS) -L$(BUILD_DIR) -lboxrig -lm -o $@

$(BUILD_DIR)/libboxrig-session.so.$(BOXRIG_SOVERSION): $(SESSION_LIBRARY) packaging/linux/boxrig-session.map $(BUILD_DIR)/libboxrig.so packaging/linux/rules.mk
	$(CC) $(CFLAGS) $(LDFLAGS) $(SHARED_LDFLAGS) $(SHARED_MAP_FLAGS) $(SHARED_ARCHIVE_FLAGS) -L$(BUILD_DIR) -lboxrig -o $@

$(BUILD_DIR)/libboxrig-posix.so.$(BOXRIG_SOVERSION): $(POSIX_LIBRARY) packaging/linux/boxrig-posix.map $(BUILD_DIR)/libboxrig-session.so packaging/linux/rules.mk
	$(CC) $(CFLAGS) $(LDFLAGS) $(SHARED_LDFLAGS) $(SHARED_MAP_FLAGS) $(SHARED_ARCHIVE_FLAGS) -L$(BUILD_DIR) -lboxrig-session -lutil -o $@

$(SHARED_BASE_LINKS): %.so: %.so.$(BOXRIG_SOVERSION)
	ln -sfn $(<F) $@

install-shared-base: shared-base install-common-licenses
	$(INSTALL) -d $(DESTDIR)$(LIBDIR) $(DESTDIR)$(INCLUDEDIR)/boxrig/ui $(DESTDIR)$(PKGCONFIGDIR)
	$(INSTALL) -m 644 $(CORE_PUBLIC_HEADERS) include/boxrig/ui.h include/boxrig/session.h include/boxrig/posix.h $(DESTDIR)$(INCLUDEDIR)/boxrig
	$(INSTALL) -m 644 include/boxrig/ui/*.h $(DESTDIR)$(INCLUDEDIR)/boxrig/ui
	@set -e; for name in $(SHARED_BASE_NAMES); do \
		$(INSTALL) -m 644 "$(BUILD_DIR)/shared/lib$$name.a" "$(BUILD_DIR)/shared/lib$$name.so.$(BOXRIG_SOVERSION)" "$(DESTDIR)$(LIBDIR)"; \
		ln -sfn "lib$$name.so.$(BOXRIG_SOVERSION)" "$(DESTDIR)$(LIBDIR)/lib$$name.so"; \
		$(INSTALL) -m 644 "$(BUILD_DIR)/shared/pkgconfig/$$name.pc" "$(DESTDIR)$(PKGCONFIGDIR)"; \
	done

test: test-shared-base
test-shared-base:
	bash tests/shared/check_base.sh "$(MAKE)"
