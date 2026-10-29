# Every installation carries the core codec notices and the project's license.
LICENSE_INSTALL_DIR = $(DESTDIR)$(PREFIX)/share/licenses/boxrig

.PHONY: install-common-licenses install-linux-licenses install-win32-licenses
install-common-licenses:
	$(INSTALL) -d $(LICENSE_INSTALL_DIR)
	$(INSTALL) -m 644 LICENSE $(LICENSE_INSTALL_DIR)/LICENSE
	$(INSTALL) -m 644 packaging/licenses/README.md packaging/licenses/README_ja.md packaging/licenses/sources.json $(LICENSE_INSTALL_DIR)
	$(INSTALL) -m 644 deps/utf8proc/LICENSE.md $(LICENSE_INSTALL_DIR)/utf8proc-LICENSE.md
	$(INSTALL) -m 644 deps/libpng/LICENSE $(LICENSE_INSTALL_DIR)/libpng-LICENSE
	$(INSTALL) -m 644 deps/zlib/LICENSE $(LICENSE_INSTALL_DIR)/zlib-LICENSE
	$(INSTALL) -m 644 resources/unicode/17.0.0/LICENSE.txt $(LICENSE_INSTALL_DIR)/Unicode-LICENSE.txt
	$(INSTALL) -m 644 data/encoding/whatwg/LICENSE $(LICENSE_INSTALL_DIR)/WHATWG-Encoding-LICENSE

install-linux-licenses: install-common-licenses $(WAYLAND_LICENSE_NOTICES)
	$(INSTALL) -m 644 deps/cardio/LICENSE $(LICENSE_INSTALL_DIR)/cardio-LICENSE
	$(INSTALL) -m 644 deps/wayland-protocols/COPYING $(LICENSE_INSTALL_DIR)/Wayland-protocols-COPYING
	$(INSTALL) -m 644 $(WAYLAND_LICENSE_NOTICES) packaging/licenses/LGPL-2.1.txt packaging/licenses/GPL-3.0.txt packaging/licenses/GCC-Runtime-Library-Exception.txt $(LICENSE_INSTALL_DIR)

install-win32-licenses: install-common-licenses
	$(INSTALL) -m 644 packaging/licenses/MinGW-w64-LICENSE.txt packaging/licenses/GPL-3.0.txt packaging/licenses/GCC-Runtime-Library-Exception.txt $(LICENSE_INSTALL_DIR)

install-gtk3 install-gtk4 install-wayland install-render-ogl install-x11-opengl install-wayland-opengl install-notification install-background-wayland install-background-gtk3 install-background-gtk4: install-linux-licenses
install-shared install-shared-gtk3 install-shared-gtk4 install-shared-native install-shared-opengl install-shared-effects: install-linux-licenses
install-core-win32-amd64 install-win32-amd64 install-win32-opengl-amd64 install-render-d3d11-amd64 install-win32-d3d11-amd64 install-core-win32-i686 install-win32-i686 install-win32-opengl-i686 install-render-d3d11-i686 install-win32-d3d11-i686: install-win32-licenses

CONPTY_RUNTIME_DIRECTORIES := $(BUILD_DIR)/boxrig_term/win32/amd64 $(BUILD_DIR)/boxrig_term/win32/i686 \
	$(MINGW_AMD64_BUILD_DIR) $(MINGW_I686_BUILD_DIR) \
	$(MINGW_AMD64_BUILD_DIR)/tests $(MINGW_I686_BUILD_DIR)/tests
CONPTY_LICENSE_ASSETS := $(addsuffix /LICENSE.conpty.txt,$(CONPTY_RUNTIME_DIRECTORIES))
CONPTY_NOTICE_ASSETS := $(addsuffix /NOTICE.conpty.md,$(CONPTY_RUNTIME_DIRECTORIES))

# Only explicit packaged-runtime preparation installs Microsoft's notices.
conpty-runtime-assets: $(CONPTY_LICENSE_ASSETS) $(CONPTY_NOTICE_ASSETS)
$(CONPTY_LICENSE_ASSETS): deps/conpty/LICENSE.txt
	mkdir -p $(@D)
	$(INSTALL) -m 644 $< $@
$(CONPTY_NOTICE_ASSETS): packaging/licenses/ConPTY-NOTICE.md
	mkdir -p $(@D)
	$(INSTALL) -m 644 $< $@

.PHONY: test-conpty-license-notices
test-package-licenses: test-conpty-license-notices
test-conpty-license-notices:
	sh tests/check_conpty_notices.sh "$(MAKE)"
