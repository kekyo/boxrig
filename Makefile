.DEFAULT_GOAL := all

.PHONY: test-package-licenses
test-package-licenses:
	sh tests/check_package_licenses.sh "$(MAKE)"


CC ?= cc
AR ?= ar
NM ?= nm
INSTALL ?= install
CPPFLAGS ?=
CFLAGS ?= -O2 -g
PREFIX ?= /usr/local
LIBDIR ?= $(PREFIX)/lib
INCLUDEDIR ?= $(PREFIX)/include
PKGCONFIGDIR ?= $(LIBDIR)/pkgconfig
BUILD_DIR ?= build

WARNINGS := -Wall -Wextra -Wpedantic -Werror
PNG_CONFIG_DIR := $(BUILD_DIR)/generated/libpng
PNG_CONFIG := $(PNG_CONFIG_DIR)/pnglibconf.h
DEPENDENCY_CPPFLAGS := -Ideps/zlib -Ideps/libpng -I$(PNG_CONFIG_DIR) -DPNG_STATIC
# Private static codecs must not interpose GTK or another shared library's codec.
NATIVE_DEPENDENCY_CFLAGS := -fvisibility=hidden
PUBLIC_CPPFLAGS := -Iinclude
CORE_PRIVATE_CPPFLAGS := -Isrc/core -Ideps/utf8proc -DUTF8PROC_STATIC
CORE_CPPFLAGS := $(PUBLIC_CPPFLAGS) $(CORE_PRIVATE_CPPFLAGS) $(DEPENDENCY_CPPFLAGS)
UI_CPPFLAGS := $(PUBLIC_CPPFLAGS)
FRONTEND_CPPFLAGS := $(PUBLIC_CPPFLAGS) -Isrc/ui
PROJECT_CPPFLAGS := $(CORE_CPPFLAGS)
PROJECT_CFLAGS := -std=c99 $(WARNINGS)
SANITIZERS := -fsanitize=address,undefined -fno-omit-frame-pointer
POSIX_CPPFLAGS := -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700 -D_DEFAULT_SOURCE
X11_CPPFLAGS := $(shell pkg-config --cflags x11 xcomposite xrender xfixes 'pango >= 1.50' 'cairo >= 1.18' pangocairo pangoft2 fontconfig)
X11_LIBS := $(shell pkg-config --libs x11 xcomposite xrender xfixes 'pango >= 1.50' 'cairo >= 1.18' pangocairo pangoft2 fontconfig)
OPENGL_CPPFLAGS := $(shell pkg-config --cflags gl egl x11)
OPENGL_LIBS := $(shell pkg-config --libs gl egl x11) -lpthread
PANGO_CAIRO_CPPFLAGS := $(shell pkg-config --cflags 'pango >= 1.50' 'cairo >= 1.18' pangocairo pangoft2 fontconfig)
PANGO_CAIRO_LIBS := $(shell pkg-config --libs 'pango >= 1.50' 'cairo >= 1.18' pangocairo pangoft2 fontconfig)
OPENGL_RENDERER_CPPFLAGS := $(OPENGL_CPPFLAGS) $(PANGO_CAIRO_CPPFLAGS)
OPENGL_RENDERER_LIBS := $(OPENGL_LIBS) $(PANGO_CAIRO_LIBS) -lm
GTK4_CPPFLAGS := $(shell pkg-config --cflags 'gtk4 >= 4.14' 'cairo >= 1.18' pangocairo pangoft2 fontconfig)
GTK4_LIBS := $(shell pkg-config --libs 'gtk4 >= 4.14' 'cairo >= 1.18' pangocairo pangoft2 fontconfig) -lm
GTK4_PROJECT_CFLAGS := $(PROJECT_CFLAGS) -Wno-pedantic
GTK3_CPPFLAGS := $(shell pkg-config --cflags 'gtk+-3.0 >= 3.24' 'cairo >= 1.18' pangocairo pangoft2 fontconfig)
GTK3_LIBS := $(shell pkg-config --libs 'gtk+-3.0 >= 3.24' 'cairo >= 1.18' pangocairo pangoft2 fontconfig) -lm
GTK_OPENGL ?= 1
ifneq ($(filter $(GTK_OPENGL),0 1),$(GTK_OPENGL))
$(error GTK_OPENGL must be 0 or 1)
endif
GTK3_CPPFLAGS += -DBOXRIG_GTK_OPENGL=$(GTK_OPENGL)
GTK4_CPPFLAGS += -DBOXRIG_GTK_OPENGL=$(GTK_OPENGL)
ifeq ($(GTK_OPENGL),1)
GTK3_CPPFLAGS += $(shell pkg-config --cflags epoxy)
GTK3_LIBS += $(shell pkg-config --libs epoxy)
GTK4_CPPFLAGS += $(shell pkg-config --cflags epoxy)
GTK4_LIBS += $(shell pkg-config --libs epoxy)
endif
WAYLAND_CPPFLAGS := $(shell pkg-config --cflags 'wayland-client >= 1.22' 'wayland-server >= 1.22' 'xkbcommon >= 1.6')
WAYLAND_LIBS := $(shell pkg-config --libs 'wayland-client >= 1.22' 'wayland-server >= 1.22' 'xkbcommon >= 1.6')
WAYLAND_CLIENT_CPPFLAGS := $(shell pkg-config --cflags 'wayland-client >= 1.22' 'xkbcommon >= 1.6')
WAYLAND_CLIENT_LIBS := $(shell pkg-config --libs 'wayland-client >= 1.22' 'xkbcommon >= 1.6')
IBUS_CPPFLAGS := $(shell pkg-config --cflags 'ibus-1.0 >= 1.5.29')
IBUS_LIBS := $(shell pkg-config --libs 'ibus-1.0 >= 1.5.29')
GTK3_CPPFLAGS += $(IBUS_CPPFLAGS) $(shell pkg-config --cflags wayland-client)
GTK3_LIBS += $(IBUS_LIBS) $(shell pkg-config --libs wayland-client)
GTK4_CPPFLAGS += $(IBUS_CPPFLAGS) $(shell pkg-config --cflags wayland-client)
GTK4_LIBS += $(IBUS_LIBS) $(shell pkg-config --libs wayland-client)
WAYLAND_CPPFLAGS += $(IBUS_CPPFLAGS)
WAYLAND_CLIENT_CPPFLAGS += $(IBUS_CPPFLAGS)
WAYLAND_LIBS += $(IBUS_LIBS)
WAYLAND_CLIENT_LIBS += $(IBUS_LIBS)
WAYLAND_OPENGL_CPPFLAGS := $(shell pkg-config --cflags gl egl wayland-egl)
WAYLAND_OPENGL_LIBS := $(shell pkg-config --libs gl egl wayland-egl) -lpthread -lm
WAYLAND_EXAMPLE_CURSOR_CPPFLAGS := $(shell pkg-config --cflags wayland-cursor)
WAYLAND_EXAMPLE_CURSOR_LIBS := $(shell pkg-config --libs wayland-cursor)
WAYLAND_EXAMPLE_PRIVATE_ARCHIVE_LDFLAGS := -Wl,--exclude-libs,ALL
WAYLAND_LIBDECOR ?= auto
WAYLAND_LIBDECOR_DETECTED := $(shell pkg-config --exists 'libdecor-0 >= 0.2.0' && printf 1 || printf 0)
ifeq ($(WAYLAND_LIBDECOR),auto)
WAYLAND_LIBDECOR_ENABLED := $(WAYLAND_LIBDECOR_DETECTED)
else ifeq ($(WAYLAND_LIBDECOR),1)
ifeq ($(WAYLAND_LIBDECOR_DETECTED),0)
$(error WAYLAND_LIBDECOR=1 requires libdecor-0 >= 0.2.0)
endif
WAYLAND_LIBDECOR_ENABLED := 1
else ifeq ($(WAYLAND_LIBDECOR),0)
WAYLAND_LIBDECOR_ENABLED := 0
else
$(error WAYLAND_LIBDECOR must be auto, 1, or 0)
endif
ifeq ($(WAYLAND_LIBDECOR_ENABLED),1)
WAYLAND_LIBDECOR_VERSION := $(shell pkg-config --modversion 'libdecor-0 >= 0.2.0')
WAYLAND_SAMPLE_EXPECTED_VIEWS := decorated
WAYLAND_EXAMPLE_LIBDECOR_CPPFLAGS := -DBOXRIG_HAVE_LIBDECOR=1 \
	$(shell pkg-config --cflags 'libdecor-0 >= 0.2.0')
WAYLAND_EXAMPLE_LIBDECOR_LIBS := $(shell pkg-config --libs 'libdecor-0 >= 0.2.0')
else
WAYLAND_LIBDECOR_VERSION := disabled
WAYLAND_SAMPLE_EXPECTED_VIEWS := undecorated
WAYLAND_EXAMPLE_LIBDECOR_CPPFLAGS := -DBOXRIG_HAVE_LIBDECOR=0
WAYLAND_EXAMPLE_LIBDECOR_LIBS :=
endif
WAYLAND_SCANNER ?= wayland-scanner
GTK3_PROJECT_CFLAGS := $(PROJECT_CFLAGS) -Wno-deprecated-declarations
XEXT_CPPFLAGS := $(shell pkg-config --cflags xext)
XEXT_LIBS := $(shell pkg-config --libs xext)
NOTIFICATION_CPPFLAGS := $(shell pkg-config --cflags gio-2.0)
NOTIFICATION_LIBS := $(shell pkg-config --libs gio-2.0)
X11_LIBS += $(NOTIFICATION_LIBS)
DBUS_CPPFLAGS := $(shell pkg-config --cflags dbus-1)
DBUS_LIBS := $(shell pkg-config --libs dbus-1)
XTEST_CPPFLAGS := $(shell pkg-config --cflags xtst)
XTEST_LIBS := $(shell pkg-config --libs xtst)
XDIRTY_CPPFLAGS := $(shell pkg-config --cflags xdamage)
XDIRTY_LIBS := $(shell pkg-config --libs xdamage)
MINGW_AMD64_CC ?= x86_64-w64-mingw32-gcc
MINGW_AMD64_AR ?= x86_64-w64-mingw32-ar
MINGW_AMD64_NM ?= x86_64-w64-mingw32-nm
MINGW_AMD64_WINDRES ?= x86_64-w64-mingw32-windres
MINGW_I686_CC ?= i686-w64-mingw32-gcc
MINGW_I686_AR ?= i686-w64-mingw32-ar
MINGW_I686_NM ?= i686-w64-mingw32-nm
MINGW_I686_WINDRES ?= i686-w64-mingw32-windres
# Keep portable archives, basic native UI, and ConPTY consumers on separate
# SDK floors. Probes can name newer APIs without raising the library floor.
MINGW_CPPFLAGS := -DUNICODE -D_UNICODE
MINGW_AMD64_CORE_CPPFLAGS := $(MINGW_CPPFLAGS) -DWINVER=0x0502 \
	-D_WIN32_WINNT=0x0502 -DNTDDI_VERSION=0x05020000
MINGW_I686_CORE_CPPFLAGS := $(MINGW_CPPFLAGS) -DWINVER=0x0500 \
	-D_WIN32_WINNT=0x0500 -DNTDDI_VERSION=0x05000000
MINGW_WIN32_CPPFLAGS := $(MINGW_CPPFLAGS) -DWINVER=0x0a00 \
	-D_WIN32_WINNT=0x0a00 -DNTDDI_VERSION=0x0a000006
MINGW_UI_CPPFLAGS := $(MINGW_CPPFLAGS) -DWINVER=0x0600 \
	-D_WIN32_WINNT=0x0600 -DNTDDI_VERSION=0x06000200
MINGW_TERM_CPPFLAGS := $(MINGW_CPPFLAGS) -DWINVER=0x0a00 \
	-D_WIN32_WINNT=0x0a00 -DNTDDI_VERSION=0x0a000006
MINGW_PROBE_CPPFLAGS := $(MINGW_TERM_CPPFLAGS)
MINGW_AMD64_CORE_LDFLAGS := -Wl,--major-os-version,5,--minor-os-version,2 \
	-Wl,--major-subsystem-version,5,--minor-subsystem-version,2
MINGW_I686_CORE_LDFLAGS := -Wl,--major-os-version,5,--minor-os-version,0 \
	-Wl,--major-subsystem-version,5,--minor-subsystem-version,0
MINGW_IMM_TEST_CPPFLAGS := \
	-DImmGetContext=boxrig_test_ImmGetContext \
	-DImmReleaseContext=boxrig_test_ImmReleaseContext \
	-DImmGetProperty=boxrig_test_ImmGetProperty \
	-DImmGetCompositionStringW=boxrig_test_ImmGetCompositionStringW \
	-DImmSetCompositionStringW=boxrig_test_ImmSetCompositionStringW \
	-DImmSetCompositionWindow=boxrig_test_ImmSetCompositionWindow \
	-DImmSetCandidateWindow=boxrig_test_ImmSetCandidateWindow \
	-DImmGetVirtualKey=boxrig_test_ImmGetVirtualKey \
	-DImmNotifyIME=boxrig_test_ImmNotifyIME
MINGW_CLIPBOARD_TEST_CPPFLAGS := \
	-DDECLSPEC_IMPORT= \
	-DOpenClipboard=boxrig_test_OpenClipboard \
	-DCloseClipboard=boxrig_test_CloseClipboard \
	-DEmptyClipboard=boxrig_test_EmptyClipboard \
	-DSetClipboardData=boxrig_test_SetClipboardData \
	-DGlobalAlloc=boxrig_test_GlobalAlloc \
	-DGlobalSize=boxrig_test_GlobalSize \
	-DGlobalUnlock=boxrig_test_GlobalUnlock \
	-DGlobalFree=boxrig_test_GlobalFree
MINGW_KEYMAP_TEST_CPPFLAGS := \
	-DToUnicodeEx=boxrig_test_ToUnicodeEx
WIN32_RENDERER_LIBS := -lole32 -luuid -lgdi32 -limm32 -luser32
WIN32_HOST_LIBS := -lshell32 -luser32
WIN32_TRANSPARENCY_LIBS := -lgdi32 -luser32
WINE ?= wine
XVFB_RUN ?= xvfb-run
WESTON ?= weston
GTK4_TEST_IM_MODULE := gtk-im-context-simple
GTK4_TEST_IM_ENV := BOXRIG_TEST_GTK4_IM_MODULE=$(GTK4_TEST_IM_MODULE) \
	GTK_IM_MODULE=$(GTK4_TEST_IM_MODULE)
TEST_FONT_FILE ?= $(shell fc-match -f '%{file}\n' 'DejaVu Sans Mono' | head -n 1)

include tools/codegen/rules.mk
GTK_RENDER_CONFIG := $(BUILD_DIR)/gtk-render.config
ifneq ($(shell cat $(GTK_RENDER_CONFIG) 2>/dev/null),$(GTK_OPENGL))
GTK_RENDER_CONFIG_PREREQUISITE := force-gtk-render-config
endif

WAYLAND_EXAMPLE_CONFIG_STAMP := \
	$(BUILD_DIR)/generated/wayland/boxrig-term-libdecor.config
WAYLAND_EXAMPLE_PREVIOUS_CONFIG := \
	$(shell sed -n '1p' $(WAYLAND_EXAMPLE_CONFIG_STAMP) 2>/dev/null)
ifneq ($(WAYLAND_EXAMPLE_PREVIOUS_CONFIG),$(WAYLAND_LIBDECOR_ENABLED))
WAYLAND_EXAMPLE_CONFIG_PREREQUISITE := force-wayland-example-config
else
WAYLAND_EXAMPLE_CONFIG_PREREQUISITE :=
endif
CONPTY_VERSION := 1.24.260710001
CONPTY_PACKAGE_SHA256 := 175640566a3b59c4b132070ee96c2c77e5ab7edd2e92732a5eb3610bbf63d90e
CONPTY_BUILD_DIR := $(BUILD_DIR)/generated/conpty
CONPTY_PACKAGE := $(CONPTY_BUILD_DIR)/Microsoft.Windows.Console.ConPTY.$(CONPTY_VERSION).nupkg
CONPTY_PACKAGE_URL := https://api.nuget.org/v3-flatcontainer/microsoft.windows.console.conpty/$(CONPTY_VERSION)/microsoft.windows.console.conpty.$(CONPTY_VERSION).nupkg
CONPTY_VERIFIED := $(CONPTY_BUILD_DIR)/package.verified
CONPTY_HEADER := src/win32/conpty_api.h
CONPTY_AMD64_DLL := $(CONPTY_BUILD_DIR)/amd64/conpty.dll
CONPTY_I686_DLL := $(CONPTY_BUILD_DIR)/i686/conpty.dll
CONPTY_X86_HOST := $(CONPTY_BUILD_DIR)/hosts/x86/OpenConsole.exe
CONPTY_X64_HOST := $(CONPTY_BUILD_DIR)/hosts/x64/OpenConsole.exe
CONPTY_ARM64_HOST := $(CONPTY_BUILD_DIR)/hosts/arm64/OpenConsole.exe
CORE_SOURCES := $(wildcard src/core/*.c)
CORE_OBJECTS := $(patsubst src/core/%.c,$(BUILD_DIR)/core/%.o,$(CORE_SOURCES))
TEXT_MEASUREMENT_BUILD_DIR := $(BUILD_DIR)/text-measurement
TEXT_MEASUREMENT_TEXT_OBJECT := $(TEXT_MEASUREMENT_BUILD_DIR)/text.o
TEXT_MEASUREMENT_RECOGNIZER_OBJECT := \
	$(TEXT_MEASUREMENT_BUILD_DIR)/recognizer.o
TEXT_MEASUREMENT_GRAPHICS_OBJECT := \
	$(TEXT_MEASUREMENT_BUILD_DIR)/graphics_protocol.o
TEXT_MEASUREMENT_INTERPRETER_OBJECT := \
	$(TEXT_MEASUREMENT_BUILD_DIR)/interpreter.o
TEXT_MEASUREMENT_TERMINAL_OBJECT := $(TEXT_MEASUREMENT_BUILD_DIR)/terminal.o
TEXT_MEASUREMENT_SCREEN_OBJECT := $(TEXT_MEASUREMENT_BUILD_DIR)/screen.o
TEXT_MEASUREMENT_CORE_OBJECTS := $(filter-out \
	$(BUILD_DIR)/core/text.o $(BUILD_DIR)/core/recognizer.o \
	$(BUILD_DIR)/core/graphics_protocol.o $(BUILD_DIR)/core/interpreter.o \
	$(BUILD_DIR)/core/terminal.o $(BUILD_DIR)/core/screen.o,$(CORE_OBJECTS)) \
	$(TEXT_MEASUREMENT_TEXT_OBJECT) $(TEXT_MEASUREMENT_RECOGNIZER_OBJECT) \
	$(TEXT_MEASUREMENT_GRAPHICS_OBJECT) $(TEXT_MEASUREMENT_INTERPRETER_OBJECT) \
	$(TEXT_MEASUREMENT_TERMINAL_OBJECT) $(TEXT_MEASUREMENT_SCREEN_OBJECT)
UI_SOURCES := $(wildcard src/ui/*.c)
UI_OBJECTS := $(patsubst src/ui/%.c,$(BUILD_DIR)/ui/%.o,$(UI_SOURCES))
UI_HEADERS := include/boxrig/ui.h $(wildcard include/boxrig/ui/*.h)
SESSION_SOURCES := $(wildcard src/session/*.c)
SESSION_OBJECTS := $(patsubst src/session/%.c,$(BUILD_DIR)/session/%.o,$(SESSION_SOURCES))
POSIX_SOURCES := $(wildcard src/posix/*.c)
POSIX_OBJECTS := $(patsubst src/posix/%.c,$(BUILD_DIR)/posix/%.o,$(POSIX_SOURCES))
X11_SOURCES := $(wildcard src/x11/*.c)
X11_OBJECTS := $(patsubst src/x11/%.c,$(BUILD_DIR)/x11/%.o,$(X11_SOURCES)) \
	$(BUILD_DIR)/posix/graphics_transport.o
NOTIFICATION_LIBRARY := $(BUILD_DIR)/libboxrig-notification.a
NOTIFICATION_OBJECTS := $(BUILD_DIR)/notification/freedesktop.o $(BUILD_DIR)/notification/glib_source.o
NOTIFICATION_HEADERS := $(wildcard include/boxrig/notification/*.h)
X11_OBJECTS += $(BUILD_DIR)/notification/freedesktop.o
X11_HEADERS := include/boxrig/x11.h $(wildcard include/boxrig/x11/*.h)
GTK4_SOURCES := $(wildcard src/gtk4/*.c)
GTK4_OBJECTS := $(patsubst src/gtk4/%.c,$(BUILD_DIR)/gtk4/%.o,$(GTK4_SOURCES)) \
	$(BUILD_DIR)/gtk4/pangocairo_renderer.o $(BUILD_DIR)/gtk4/accessible_text.o
GTK4_HEADERS := include/boxrig/gtk4.h include/boxrig/ime.h $(wildcard include/boxrig/gtk4/*.h) $(wildcard include/boxrig/gtk/*.h)
GTK3_SOURCES := $(wildcard src/gtk3/*.c)
GTK3_OBJECTS := $(patsubst src/gtk3/%.c,$(BUILD_DIR)/gtk3/%.o,$(GTK3_SOURCES)) \
	$(BUILD_DIR)/gtk3/pangocairo_renderer.o $(BUILD_DIR)/gtk3/accessible_text.o
GTK3_HEADERS := include/boxrig/gtk3.h include/boxrig/ime.h $(wildcard include/boxrig/gtk3/*.h) $(wildcard include/boxrig/gtk/*.h)
PANGO_CANDIDATE_OBJECT := $(BUILD_DIR)/pangocairo/candidates.o
GTK3_OBJECTS += $(PANGO_CANDIDATE_OBJECT)
GTK4_OBJECTS += $(PANGO_CANDIDATE_OBJECT)
IME_SOURCES := src/ime/client.c src/ime/detect.c src/ibus/client.c src/fcitx5/client.c src/fcitx4/client.c src/fcitx4/connection.c src/ime/source.c src/ime/preedit.c
IME_HEADERS := src/fcitx4/connection.h include/boxrig/ime.h src/ime/client.h src/ime/client_private.h src/ime/source.h src/ime/preedit.h
IME_OBJECTS := $(BUILD_DIR)/ime/client.o $(BUILD_DIR)/ime/detect.o $(BUILD_DIR)/ime/ibus.o $(BUILD_DIR)/ime/fcitx5.o $(BUILD_DIR)/ime/fcitx4.o $(BUILD_DIR)/ime/fcitx4_connection.o $(BUILD_DIR)/ime/source.o $(BUILD_DIR)/ime/preedit.o
GTK3_OBJECTS += $(IME_OBJECTS) $(BUILD_DIR)/gtk3/ime_probe.o
GTK4_OBJECTS += $(IME_OBJECTS) $(BUILD_DIR)/gtk4/ime_probe.o
WAYLAND_HEADERS := include/boxrig/wayland.h include/boxrig/ime.h
OPENGL_RENDERER_HEADERS := include/boxrig/render/opengl.h include/boxrig/render/runtime.h
WAYLAND_OPENGL_HEADERS := $(wildcard include/boxrig/wayland/*.h)
WAYLAND_PROTOCOL_DIR := $(BUILD_DIR)/generated/wayland
WAYLAND_VIEWPORTER_HEADER := $(WAYLAND_PROTOCOL_DIR)/viewporter-client-protocol.h
WAYLAND_VIEWPORTER_SERVER_HEADER := $(WAYLAND_PROTOCOL_DIR)/viewporter-server-protocol.h
WAYLAND_VIEWPORTER_SOURCE := $(WAYLAND_PROTOCOL_DIR)/viewporter-protocol.c
WAYLAND_FRACTIONAL_SCALE_HEADER := $(WAYLAND_PROTOCOL_DIR)/fractional-scale-v1-client-protocol.h
WAYLAND_FRACTIONAL_SCALE_SERVER_HEADER := $(WAYLAND_PROTOCOL_DIR)/fractional-scale-v1-server-protocol.h
WAYLAND_FRACTIONAL_SCALE_SOURCE := $(WAYLAND_PROTOCOL_DIR)/fractional-scale-v1-protocol.c
WAYLAND_XDG_SHELL_HEADER := $(WAYLAND_PROTOCOL_DIR)/xdg-shell-client-protocol.h
WAYLAND_XDG_SHELL_SOURCE := $(WAYLAND_PROTOCOL_DIR)/xdg-shell-protocol.c
WAYLAND_XDG_DECORATION_HEADER := $(WAYLAND_PROTOCOL_DIR)/xdg-decoration-v1-client-protocol.h
WAYLAND_XDG_DECORATION_SOURCE := $(WAYLAND_PROTOCOL_DIR)/xdg-decoration-v1-protocol.c
WAYLAND_TEXT_INPUT_HEADER := $(WAYLAND_PROTOCOL_DIR)/text-input-v3-client-protocol.h
WAYLAND_TEXT_INPUT_SERVER_HEADER := $(WAYLAND_PROTOCOL_DIR)/text-input-v3-server-protocol.h
WAYLAND_TEXT_INPUT_SOURCE := $(WAYLAND_PROTOCOL_DIR)/text-input-v3-protocol.c
WAYLAND_PRIMARY_SELECTION_HEADER := $(WAYLAND_PROTOCOL_DIR)/primary-selection-v1-client-protocol.h
WAYLAND_PRIMARY_SELECTION_SERVER_HEADER := $(WAYLAND_PROTOCOL_DIR)/primary-selection-v1-server-protocol.h
WAYLAND_PRIMARY_SELECTION_SOURCE := $(WAYLAND_PROTOCOL_DIR)/primary-selection-v1-protocol.c
WAYLAND_GENERATED_HEADERS := $(WAYLAND_VIEWPORTER_HEADER) $(WAYLAND_FRACTIONAL_SCALE_HEADER) \
	$(WAYLAND_TEXT_INPUT_HEADER) $(WAYLAND_PRIMARY_SELECTION_HEADER)
WAYLAND_GENERATED_SOURCES := $(WAYLAND_VIEWPORTER_SOURCE) $(WAYLAND_FRACTIONAL_SCALE_SOURCE) \
	$(WAYLAND_TEXT_INPUT_SOURCE) $(WAYLAND_PRIMARY_SELECTION_SOURCE)
WAYLAND_SOURCES := $(wildcard src/wayland/*.c)
WAYLAND_PRIVATE_HEADERS := $(wildcard src/wayland/*.h)
WAYLAND_OBJECTS := $(patsubst src/wayland/%.c,$(BUILD_DIR)/wayland/%.o,$(WAYLAND_SOURCES)) \
	$(BUILD_DIR)/wayland/pangocairo_renderer.o \
	$(BUILD_DIR)/wayland/viewporter-protocol.o \
	$(BUILD_DIR)/wayland/fractional-scale-v1-protocol.o \
	$(BUILD_DIR)/wayland/text-input-v3-protocol.o \
	$(BUILD_DIR)/wayland/primary-selection-v1-protocol.o
WAYLAND_OBJECTS += $(IME_OBJECTS) $(PANGO_CANDIDATE_OBJECT)
WAYLAND_OBJECTS += $(BUILD_DIR)/background_effect/wayland.o $(BUILD_DIR)/background_effect/protocol.o
UTF8PROC_SOURCE := deps/utf8proc/utf8proc.c
UTF8PROC_OBJECT := $(BUILD_DIR)/core/utf8proc.o
ZLIB_SOURCES := deps/zlib/adler32.c deps/zlib/compress.c deps/zlib/crc32.c \
	deps/zlib/deflate.c deps/zlib/infback.c deps/zlib/inffast.c \
	deps/zlib/inflate.c deps/zlib/inftrees.c deps/zlib/trees.c \
	deps/zlib/uncompr.c deps/zlib/zutil.c
LIBPNG_SOURCES := deps/libpng/png.c deps/libpng/pngerror.c \
	deps/libpng/pngget.c deps/libpng/pngmem.c deps/libpng/pngpread.c \
	deps/libpng/pngread.c deps/libpng/pngrio.c deps/libpng/pngrtran.c \
	deps/libpng/pngrutil.c deps/libpng/pngset.c deps/libpng/pngtrans.c \
	deps/libpng/pngwio.c deps/libpng/pngwrite.c deps/libpng/pngwtran.c \
	deps/libpng/pngwutil.c
# libpng selects NEON from target compiler macros; the ARM sources use the same
# guards, so they must accompany the generic decoder even in cross builds.
LIBPNG_SOURCES += deps/libpng/arm/arm_init.c \
	deps/libpng/arm/filter_neon_intrinsics.c deps/libpng/arm/palette_neon_intrinsics.c
ZLIB_OBJECTS := $(patsubst deps/zlib/%.c,$(BUILD_DIR)/deps/zlib/%.o,$(ZLIB_SOURCES))
LIBPNG_OBJECTS := $(patsubst deps/libpng/%.c,$(BUILD_DIR)/deps/libpng/%.o,$(LIBPNG_SOURCES))
CORE_PUBLIC_HEADERS := include/boxrig/terminal.h include/boxrig/regex.h include/boxrig/status.h
CORE_HEADERS := src/core/codec.h src/core/shift_jis_table.h $(CORE_PUBLIC_HEADERS) src/core/dcs.h src/core/graphics.h src/core/parser.h src/core/regex.h src/core/terminal_internal.h src/core/unicode.h
LIBRARY := $(BUILD_DIR)/libboxrig.a
UI_LIBRARY := $(BUILD_DIR)/libboxrig-ui.a
SESSION_LIBRARY := $(BUILD_DIR)/libboxrig-session.a
POSIX_LIBRARY := $(BUILD_DIR)/libboxrig-posix.a
X11_LIBRARY := $(BUILD_DIR)/libboxrig-x11.a
GTK4_LIBRARY := $(BUILD_DIR)/libboxrig-gtk4.a
GTK3_LIBRARY := $(BUILD_DIR)/libboxrig-gtk3.a
WAYLAND_LIBRARY := $(BUILD_DIR)/libboxrig-wayland.a
OPENGL_RENDERER_SOURCES := $(wildcard src/opengl/*.c)
OPENGL_RENDERER_OBJECTS := $(patsubst src/opengl/%.c,$(BUILD_DIR)/opengl/%.o,$(OPENGL_RENDERER_SOURCES))
OPENGL_RENDERER_OBJECTS += $(PANGO_CANDIDATE_OBJECT)
OPENGL_RENDERER_LIBRARY := $(BUILD_DIR)/libboxrig-render-ogl.a
ifeq ($(GTK_OPENGL),1)
# The candidate painter is already included for the Cairo backend.
GTK3_OBJECTS += $(BUILD_DIR)/gtk3/gl_area.o $(BUILD_DIR)/gtk3/gl_surface.o $(filter-out $(PANGO_CANDIDATE_OBJECT),$(OPENGL_RENDERER_OBJECTS))
GTK4_OBJECTS += $(BUILD_DIR)/gtk4/gl_area.o $(BUILD_DIR)/gtk4/gl_surface.o $(filter-out $(PANGO_CANDIDATE_OBJECT),$(OPENGL_RENDERER_OBJECTS))
endif
X11_OPENGL_SOURCES := $(wildcard src/x11_opengl/*.c)
X11_OPENGL_OBJECTS := $(patsubst src/x11_opengl/%.c,$(BUILD_DIR)/x11_opengl/%.o,$(X11_OPENGL_SOURCES))
X11_OPENGL_LIBRARY := $(BUILD_DIR)/libboxrig-x11-ogl.a
WAYLAND_OPENGL_SOURCES := $(wildcard src/wayland_opengl/*.c)
WAYLAND_OPENGL_OBJECTS := $(patsubst src/wayland_opengl/%.c,$(BUILD_DIR)/wayland_opengl/%.o,$(WAYLAND_OPENGL_SOURCES))
WAYLAND_OPENGL_LIBRARY := $(BUILD_DIR)/libboxrig-wayland-ogl.a
GTK4_PKG_CONFIG := $(if $(filter 0,$(GTK_OPENGL)),pkgconfig/cairo/boxrig-gtk4.pc,pkgconfig/boxrig-gtk4.pc)
GTK3_PKG_CONFIG := $(if $(filter 0,$(GTK_OPENGL)),pkgconfig/cairo/boxrig-gtk3.pc,pkgconfig/boxrig-gtk3.pc)
WAYLAND_PKG_CONFIG := pkgconfig/boxrig-wayland.pc
OPENGL_RENDERER_PKG_CONFIG := pkgconfig/boxrig-render-ogl.pc
X11_OPENGL_PKG_CONFIG := pkgconfig/boxrig-x11-ogl.pc
WAYLAND_OPENGL_PKG_CONFIG := pkgconfig/boxrig-wayland-ogl.pc
TEST_BINARY := $(BUILD_DIR)/tests/test_terminal
TEXT_WORKLOAD_MEASUREMENT_BINARY := \
	$(TEXT_MEASUREMENT_BUILD_DIR)/measure_text_workloads
TEXT_MEASUREMENT_LIBRARY := $(TEXT_MEASUREMENT_BUILD_DIR)/libboxrig.a
PARSER_TEST_BINARY := $(BUILD_DIR)/tests/test_parser
GRAPHICS_TEST_BINARY := $(BUILD_DIR)/tests/test_graphics
SCREEN_TEST_BINARY := $(BUILD_DIR)/tests/test_screen
TEXT_SYNC_TEST_BINARY := $(BUILD_DIR)/tests/test_text_sync
TEXT_LAYOUT_TEST_BINARY := $(BUILD_DIR)/tests/test_text_layout
SEARCH_TRANSFORM_PROTOTYPE_TEST_BINARY := $(BUILD_DIR)/tests/test_search_transform_prototype
COMPOSITION_TEST_BINARY := $(BUILD_DIR)/tests/test_composition
UTF8_TEST_BINARY := $(BUILD_DIR)/tests/test_utf8
IMAGE_COMPOSITOR_TEST_BINARY := $(BUILD_DIR)/tests/test_image_compositor
UI_LIFECYCLE_TEST_BINARY := $(BUILD_DIR)/tests/test_ui_lifecycle
RENDERER_LIFECYCLE_TEST_BINARY := \
	$(BUILD_DIR)/tests/test_renderer_lifecycle
OPENGL_RUNTIME_TEST_BINARY := $(BUILD_DIR)/tests/test_opengl_runtime
OPENGL_RENDERER_FIXTURE_TEST_BINARY := \
	$(BUILD_DIR)/tests/test_opengl_renderer_fixture
UI_TEST_BINARY := $(BUILD_DIR)/tests/test_ui
ANNOTATION_COST_BINARY := $(BUILD_DIR)/tests/performance/annotation_cost
ANNOTATION_COST_UNWRAPPED_BINARY := \
	$(BUILD_DIR)/tests/performance/annotation_cost_unwrapped
SELECTION_TEST_BINARY := $(BUILD_DIR)/tests/test_selection
UI_PLATFORM_FIXTURE_TEST_BINARY := $(BUILD_DIR)/tests/test_ui_platform_fixture
PANGO_CAIRO_FIXTURE_TEST_BINARY := \
	$(BUILD_DIR)/tests/test_pango_cairo_fixture
FRONTEND_COMPONENT_FIXTURE_SOURCES := tests/frontend_component_fixture.c
FRONTEND_CONFORMANCE_MATRIX := tests/frontend_conformance_matrix.def
FRONTEND_PUBLIC_CONTRACTS := tests/frontend_public_contracts.def
# Enumerate nested public headers on every invocation, including new directories.
frontend_public_headers = $(wildcard $(1)/*.h) $(foreach directory,$(wildcard $(1)/*/),$(call frontend_public_headers,$(patsubst %/,%,$(directory))))
FRONTEND_PUBLIC_HEADERS := $(call frontend_public_headers,include/boxrig)
FRONTEND_COMPONENT_FIXTURE_HEADER := tests/frontend_component_fixture.h \
	tests/external_scrollbar_example.h $(wildcard examples/*/external_scrollbar.h) \
	tests/frontend_conformance_cases.h $(FRONTEND_CONFORMANCE_MATRIX)
FRONTEND_CONFORMANCE_TEST_BINARY := \
	$(BUILD_DIR)/tests/test_frontend_conformance
FRONTEND_SAMPLE_SCENARIO_SOURCE := tests/frontend_sample_scenario.c
FRONTEND_SAMPLE_SCENARIO_HEADER := tests/frontend_sample_scenario.h
FRONTEND_SAMPLE_SCENARIO_TEST_BINARY := \
	$(BUILD_DIR)/tests/test_frontend_sample_scenario
FRONTEND_SAMPLE_X11_TEST_BINARY := \
	$(BUILD_DIR)/tests/test_frontend_sample_x11
FRONTEND_SAMPLE_PROBE_SOURCE := tests/frontend_sample_probe.c
FRONTEND_SAMPLE_PROBE_TEST_BINARY := $(BUILD_DIR)/tests/test_frontend_sample_probe
FRONTEND_SAMPLE_PROBE_LDFLAGS := -Wl,--wrap=boxrig_terminal_create \
	-Wl,--wrap=boxrig_terminal_destroy \
	-Wl,--wrap=boxrig_ui_model_create -Wl,--wrap=boxrig_ui_model_destroy \
	-Wl,--wrap=boxrig_ui_host_item_consumer_create \
	-Wl,--wrap=boxrig_ui_host_item_consumer_destroy \
	-Wl,--wrap=boxrig_ui_view_create \
	-Wl,--wrap=boxrig_ui_view_create_selection_snapshot \
	-Wl,--wrap=boxrig_text_snapshot_destroy \
	-Wl,--wrap=boxrig_composition_destroy \
	-Wl,--wrap=boxrig_session_create -Wl,--wrap=boxrig_session_destroy \
	-Wl,--wrap=boxrig_terminal_send_paste \
	-Wl,--wrap=boxrig_composition_create \
	-Wl,--wrap=boxrig_term_text_policy_create \
	-Wl,--wrap=boxrig_terminal_host_item_peek \
	-Wl,--wrap=boxrig_term_text_policy_resume \
	-Wl,--wrap=boxrig_term_text_policy_update_pointer_hover \
	-Wl,--wrap=boxrig_ui_view_consume_dirty \
	-Wl,--wrap=boxrig_ui_view_accept_allocation \
	-Wl,--wrap=boxrig_ui_view_destroy \
	-Wl,--wrap=boxrig_term_text_policy_destroy \
	-Wl,--wrap=boxrig_term_runtime_info_write
FRONTEND_SAMPLE_POSIX_PROBE_LDFLAGS := $(FRONTEND_SAMPLE_PROBE_LDFLAGS) \
	-Wl,--wrap=boxrig_posix_pty_spawn -Wl,--wrap=boxrig_posix_pty_destroy
FRONTEND_SAMPLE_GTK_PROBE_LDFLAGS := $(FRONTEND_SAMPLE_POSIX_PROBE_LDFLAGS) \
	-Wl,--wrap=gtk_application_window_new -Wl,--wrap=g_source_new \
	-Wl,--wrap=g_source_set_name \
	-Wl,--wrap=g_child_watch_source_new -Wl,--wrap=g_source_set_callback \
	-Wl,--wrap=g_source_unref
FRONTEND_SAMPLE_WIN32_PROBE_LDFLAGS := \
	$(FRONTEND_SAMPLE_PROBE_LDFLAGS) \
	-Wl,--wrap=boxrig_win32_pty_spawn_with_config -Wl,--wrap=boxrig_win32_pty_destroy \
	-Wl,--wrap=boxrig_win32_pty_session_create \
	-Wl,--wrap=boxrig_win32_pty_session_destroy \
	-Wl,--wrap=boxrig_win32_transparency_context_get_state \
	-Wl,--wrap=boxrig_win32_transparency_context_destroy \
	-Wl,--wrap=boxrig_win32_transparency_host_create \
	-Wl,--wrap=boxrig_win32_transparency_host_destroy \
	-Wl,--wrap=boxrig_win32_terminal_component_create_child \
	-Wl,--wrap=boxrig_win32_terminal_component_create_on_target \
	-Wl,--wrap=boxrig_win32_terminal_component_destroy \
	-Wl,--wrap=boxrig_win32_terminal_component_set_key_router \
	-Wl,--wrap=boxrig_win32_terminal_component_set_mouse_router \
	-Wl,--wrap=boxrig_win32_host_adapter_create \
	-Wl,--wrap=boxrig_win32_host_adapter_destroy
FRONTEND_SAMPLE_PROBE_DIR := $(BUILD_DIR)/tests/frontend_sample_probes
FRONTEND_SAMPLE_X11_PROBE_BINARY := $(FRONTEND_SAMPLE_PROBE_DIR)/x11/boxrig_term
FRONTEND_SAMPLE_GTK3_PROBE_BINARY := $(FRONTEND_SAMPLE_PROBE_DIR)/gtk3/boxrig_term
FRONTEND_SAMPLE_GTK4_PROBE_BINARY := $(FRONTEND_SAMPLE_PROBE_DIR)/gtk4/boxrig_term
FRONTEND_SAMPLE_WAYLAND_PROBE_BINARY := $(FRONTEND_SAMPLE_PROBE_DIR)/wayland/boxrig_term
X11_COMPONENT_FIXTURE_TEST_BINARY := \
	$(BUILD_DIR)/tests/test_x11_component_fixture
X11_OPENGL_COMPONENT_FIXTURE_TEST_BINARY := \
	$(BUILD_DIR)/tests/test_x11_opengl_component_fixture
X11_OPENGL_COMPONENT_TEST_BINARY := \
	$(BUILD_DIR)/tests/test_x11_opengl_component
GTK4_WIDGET_TEST_BINARY := $(BUILD_DIR)/tests/test_gtk4_widget
GTK3_WIDGET_TEST_BINARY := $(BUILD_DIR)/tests/test_gtk3_widget
GTK4_SAMPLE_TEST_BINARY := $(BUILD_DIR)/tests/test_gtk4_sample
GTK3_SAMPLE_TEST_BINARY := $(BUILD_DIR)/tests/test_gtk3_sample
GTK4_WAYLAND_TEST_RUNNER := tests/gtk4/run_wayland_test.sh
GTK3_WAYLAND_TEST_RUNNER := tests/gtk3/run_wayland_test.sh
GTK3_WAYLAND_SAMPLE_TEST_RUNNER := tests/gtk3/run_wayland_sample_test.sh
GTK3_WAYLAND_SAMPLE_CHILD_SOURCE := tests/gtk3/wayland_sample_child.c
GTK3_WAYLAND_SAMPLE_CHILD_BINARY := \
	$(BUILD_DIR)/tests/gtk3_wayland_sample_child
GTK3_CLIPBOARD_PEER_SOURCE := tests/gtk3/clipboard_peer.c
GTK3_CLIPBOARD_PEER_BINARY := $(BUILD_DIR)/tests/gtk3_clipboard_peer
GTK3_GESTAMENT_TEST_DIR := tests/gtk3/gestament
GTK3_GESTAMENT_PACKAGE_FILES := \
	$(GTK3_GESTAMENT_TEST_DIR)/package.json \
	$(GTK3_GESTAMENT_TEST_DIR)/package-lock.json
GTK3_GESTAMENT_INSTALL_STAMP := \
	$(GTK3_GESTAMENT_TEST_DIR)/node_modules/.package-lock.json
GTK4_PACKAGE_TEST_SOURCE := tests/gtk4/test_gtk4_package.c
GTK4_PACKAGE_TEST_BINARY := $(BUILD_DIR)/tests/test_gtk4_package
GTK4_PACKAGE_TEST_ROOT := $(abspath $(BUILD_DIR)/tests/gtk4-package-root)
GTK3_PACKAGE_TEST_SOURCE := tests/gtk3/test_gtk3_package.c
GTK3_PACKAGE_TEST_BINARY := $(BUILD_DIR)/tests/test_gtk3_package
GTK3_PACKAGE_TEST_ROOT := $(abspath $(BUILD_DIR)/tests/gtk3-package-root)
WAYLAND_CONTEXT_TEST_BINARY := $(BUILD_DIR)/tests/test_wayland_context
WAYLAND_WESTON_TEST_SOURCE := tests/wayland/test_wayland_weston.c
WAYLAND_WESTON_TEST_BINARY := $(BUILD_DIR)/tests/test_wayland_weston
WAYLAND_OPENGL_WESTON_TEST_BINARY := \
	$(BUILD_DIR)/tests/test_wayland_opengl_weston
WAYLAND_WESTON_TEST_RUNNER := tests/wayland/run_weston_test.sh
WAYLAND_SAMPLE_TEST_RUNNER := tests/wayland/run_sample_test.sh
WAYLAND_INTERACTIVE_SAMPLE_TEST_RUNNER := \
	tests/wayland/run_interactive_sample_test.sh
WAYLAND_FRONTEND_SAMPLE_TEST_RUNNER := \
	tests/wayland/run_frontend_sample_scenario.sh
WAYLAND_X11_PIXEL_PROBE_SOURCE := tests/wayland/x11_pixel_probe.c
WAYLAND_X11_PIXEL_PROBE_BINARY := $(BUILD_DIR)/tests/wayland_x11_pixel_probe
WAYLAND_SAMPLE_BUILD_TEST_RUNNER := tests/wayland/test_sample_build.sh
WAYLAND_NO_LIBDECOR_TEST_BINARY := \
	$(BUILD_DIR)/tests/boxrig_term_wayland_no_libdecor
WAYLAND_ROOT_POLICY_TEST_BINARY := \
	$(BUILD_DIR)/tests/test_wayland_root_backing_policy
WAYLAND_SHORTCUT_POLICY_TEST_BINARY := \
	$(BUILD_DIR)/tests/test_wayland_shortcut_policy
WAYLAND_DECORATION_POLICY_TEST_BINARY := \
	$(BUILD_DIR)/tests/test_wayland_decoration_policy
WAYLAND_PACKAGE_TEST_SOURCE := tests/wayland/test_wayland_package.c
WAYLAND_PACKAGE_TEST_BINARY := $(BUILD_DIR)/tests/test_wayland_package
WAYLAND_PACKAGE_TEST_ROOT := $(abspath $(BUILD_DIR)/tests/wayland-package-root)
OPENGL_PACKAGE_TEST_ROOT := $(abspath $(BUILD_DIR)/tests/opengl-package-root)
OPENGL_PACKAGE_TEST_SOURCE := examples/opengl_renderer/main.c
OPENGL_PACKAGE_TEST_BINARY := $(BUILD_DIR)/tests/test_opengl_package
X11_OPENGL_PACKAGE_TEST_SOURCE := tests/x11/test_x11_opengl_package.c
X11_OPENGL_PACKAGE_TEST_BINARY := $(BUILD_DIR)/tests/test_x11_opengl_package
WAYLAND_OPENGL_PACKAGE_TEST_SOURCE := \
	tests/wayland/test_wayland_opengl_package.c
WAYLAND_OPENGL_PACKAGE_TEST_BINARY := \
	$(BUILD_DIR)/tests/test_wayland_opengl_package
CORE_UI_BOUNDARY_TEST_SOURCE := tests/test_custom_ui.c
CORE_UI_BOUNDARY_TEST_BINARY := $(BUILD_DIR)/tests/test_custom_ui
CORE_UI_BOUNDARY_TEST_ROOT := \
	$(abspath $(BUILD_DIR)/tests/core-ui-boundary-root)
DEPENDENCY_BOUNDARY_TEST_SCRIPT := tools/test-dependency-boundary.mjs
SESSION_TEST_BINARY := $(BUILD_DIR)/tests/test_session
POSIX_SESSION_TEST_BINARY := $(BUILD_DIR)/tests/test_posix_session
POSIX_EXAMPLE_COMMON_TEST_BINARY := \
	$(BUILD_DIR)/tests/test_posix_example_common
EXAMPLE_RUNTIME_INFO_TEST_BINARY := \
	$(BUILD_DIR)/tests/test_example_runtime_info
EXAMPLE_TEXT_POLICY_TEST_BINARY := \
	$(BUILD_DIR)/tests/test_example_text_policy
EXAMPLE_MOUSE_POLICY_TEST_BINARY := \
	$(BUILD_DIR)/tests/test_example_mouse_policy
OSC_RECOGNIZER_REGRESSION_TEST_BINARY := \
	$(BUILD_DIR)/tests/test_osc_recognizer_regression
INPUT_SHORTCUT_TEST_BINARY := $(BUILD_DIR)/tests/test_input_shortcuts
NOTIFICATION_POLICY_TEST_BINARY := $(BUILD_DIR)/tests/test_notification_policy
SCROLLBAR_TEST_BINARY := $(BUILD_DIR)/tests/test_scrollbar
EXAMPLE_LAYOUT_TEST_BINARY := $(BUILD_DIR)/tests/test_example_layout
UI_GUIDE_TEST_BINARY := $(BUILD_DIR)/tests/test_ui_guide
EMITTER_TEST_BINARY := $(BUILD_DIR)/tests/test_emitter
TERMINFO_TEST_BINARY := $(BUILD_DIR)/tests/test_terminfo
PTY_TEST_BINARY := $(BUILD_DIR)/tests/test_posix_pty
PTY_HELPER_BINARY := $(BUILD_DIR)/tests/pty_helper
X11_TEST_BINARY := $(BUILD_DIR)/tests/test_x11_renderer
X11_HOST_TEST_BINARY := $(BUILD_DIR)/tests/test_x11_host_adapter
X11_NOTIFICATION_TEST_BINARY := $(BUILD_DIR)/tests/test_x11_notifications
X11_INPUT_TEST_BINARY := $(BUILD_DIR)/tests/test_x11_input_adapter
X11_EVENT_WAIT_TEST_BINARY := $(BUILD_DIR)/tests/test_x11_event_wait
X11_TRANSPARENCY_TEST_BINARY := $(BUILD_DIR)/tests/test_x11_transparency
X11_COMPOSITOR_TEST_BINARY := $(BUILD_DIR)/tests/test_x11_compositor
X11_COMPONENT_TEST_BINARY := $(BUILD_DIR)/tests/test_x11_component
X11_SAMPLE_TEST_BINARY := $(BUILD_DIR)/tests/test_x11_sample
X11_FAKE_NOTIFICATION_SERVICE := $(BUILD_DIR)/tests/fake_notification_service
P2_X11_IME_TEST_BINARY := $(BUILD_DIR)/tests/p2/test_x11_ime
P2_X11_IME_CHILD_BINARY := $(BUILD_DIR)/tests/p2/x11_ime_child
P2_WIN32_IME_CHILD_BINARY = $(MINGW_AMD64_BUILD_DIR)/tests/p2/win32_ime_child.exe
P2_WIN32_IME_CONTROL_BINARY = $(MINGW_AMD64_BUILD_DIR)/tests/p2/win32_ime_control.exe
P2_WIN32_IME_MOVEMENT_BINARY = $(BUILD_DIR)/boxrig_term/win32/amd64/boxrig_term_ime_probe.exe
P2_WIN32_RECONVERSION_HOST = $(MINGW_AMD64_BUILD_DIR)/tests/p2/win32_reconversion_host.exe
MINGW_I686_RECONVERSION_HOST = $(MINGW_I686_BUILD_DIR)/tests/p2/win32_reconversion_host.exe
WIN32_RECONVERSION_REPORT_TEST := tools/test-win32-reconversion-report.mjs
P2_WIN32_TRANSPARENCY_BINARY = $(MINGW_AMD64_BUILD_DIR)/tests/p2/win32_transparency_host.exe
P2_WIN32_TRANSPARENCY_RESOURCE = $(MINGW_AMD64_BUILD_DIR)/tests/p2/win32_transparency_manifest.o
MINGW_AMD64_WGL_PROBE = $(MINGW_AMD64_BUILD_DIR)/tests/p2/win32_opengl_probe.exe
MINGW_I686_WGL_PROBE = $(MINGW_I686_BUILD_DIR)/tests/p2/win32_opengl_probe.exe
P2_WIN32_TEST_DIR := tests/p2/win32
P2_WIN32_TEST_PREREQUISITES = $(MINGW_AMD64_EXAMPLE) $(MINGW_AMD64_LAUNCHER) \
	$(MINGW_AMD64_SAMPLE_TEST) \
	$(MINGW_AMD64_FRONTEND_SAMPLE_PROBE) $(MINGW_I686_FRONTEND_SAMPLE_PROBE) \
	$(MINGW_AMD64_TEST_LAUNCHER) $(MINGW_I686_TEST_LAUNCHER) \
	$(P2_WIN32_IME_CHILD_BINARY) $(P2_WIN32_IME_CONTROL_BINARY) \
	$(P2_WIN32_IME_MOVEMENT_BINARY) \
	$(P2_WIN32_RECONVERSION_HOST) \
	$(P2_WIN32_TRANSPARENCY_BINARY) \
	$(MINGW_AMD64_WGL_PROBE) $(MINGW_I686_WGL_PROBE) \
	$(MINGW_AMD64_PTY_TEST) $(MINGW_AMD64_PTY_HELPER) \
	$(MINGW_AMD64_EMITTER) $(MINGW_I686_EXAMPLE) \
	$(MINGW_I686_LAUNCHER) $(MINGW_I686_SAMPLE_TEST) \
	$(MINGW_I686_PTY_TEST) \
	$(MINGW_I686_PTY_HELPER) $(MINGW_I686_EMITTER)
# The XDG opener is sample-owned; installed C libraries have no cardio/GIO dependency.
POSIX_URI_CPPFLAGS := $(shell pkg-config --cflags gio-2.0)
POSIX_URI_LIBS := $(shell pkg-config --libs gio-2.0) -lstdc++ -pthread
POSIX_URI_SOURCE := examples/boxrig_term/posix/uri_launcher.cpp
POSIX_URI_HEADERS := examples/boxrig_term/posix/uri_launcher.h examples/boxrig_term/posix/uri_launcher_glib.h
POSIX_URI_OBJECT := $(BUILD_DIR)/boxrig_term/uri_launcher.o
POSIX_URI_TEST_OBJECT := $(BUILD_DIR)/tests/uri_launcher.o

EXAMPLE_BINARY := $(BUILD_DIR)/boxrig_term/x11/boxrig_term
GTK4_EXAMPLE_BINARY := $(BUILD_DIR)/boxrig_term/gtk4/boxrig_term
GTK3_EXAMPLE_BINARY := $(BUILD_DIR)/boxrig_term/gtk3/boxrig_term
WAYLAND_EXAMPLE_BINARY := $(BUILD_DIR)/boxrig_term/wayland/boxrig_term
POSIX_EMITTER_BINARY := $(BUILD_DIR)/boxrig_emitter/posix/boxrig_emitter
EXAMPLE_SOURCES := examples/boxrig_term/x11/main.c \
	examples/boxrig_term/x11/application.c \
	examples/boxrig_term/x11/native_host.c \
	examples/boxrig_term/x11/event_loop.c \
	examples/boxrig_term/x11/wm_sync.c \
	examples/boxrig_term/common/runtime_info.c \
	examples/boxrig_term/common/posix_application.c \
	examples/boxrig_term/common/mouse_policy.c \
	examples/boxrig_term/common/text_policy.c \
	examples/boxrig_term/common/color_demo.c \
	examples/boxrig_term/common/font_demo.c \
	examples/boxrig_term/common/decoration_demo.c \
	examples/boxrig_term/common/background_demo.c \
	examples/boxrig_term/common/scrollbar_demo.c \
	examples/boxrig_term/common/link_gesture.c \
	$(POSIX_URI_OBJECT)
GTK4_EXAMPLE_SOURCES := examples/boxrig_term/gtk4/main.c \
	examples/boxrig_term/gtk4/application.c \
	examples/boxrig_term/gtk4/event_source.c \
	examples/boxrig_term/gtk4/event_source_policy.c \
	examples/boxrig_term/gtk4/native_host.c \
	examples/boxrig_term/common/runtime_info.c \
	examples/boxrig_term/common/posix_application.c \
	examples/boxrig_term/common/mouse_policy.c \
	examples/boxrig_term/common/text_policy.c \
	examples/boxrig_term/common/color_demo.c \
	examples/boxrig_term/common/font_demo.c \
	examples/boxrig_term/common/decoration_demo.c \
	examples/boxrig_term/common/background_demo.c \
	examples/boxrig_term/common/scrollbar_demo.c \
	examples/boxrig_term/common/link_gesture.c \
	$(POSIX_URI_OBJECT)
GTK3_EXAMPLE_SOURCES := examples/boxrig_term/gtk3/main.c \
	examples/boxrig_term/gtk3/application.c \
	examples/boxrig_term/gtk3/event_source.c \
	examples/boxrig_term/gtk3/event_source_policy.c \
	examples/boxrig_term/gtk3/geometry_trace.c \
	examples/boxrig_term/gtk3/native_host.c \
	examples/boxrig_term/gtk3/resize_frame.c \
	examples/boxrig_term/common/runtime_info.c \
	examples/boxrig_term/common/posix_application.c \
	examples/boxrig_term/common/mouse_policy.c \
	examples/boxrig_term/common/text_policy.c \
	examples/boxrig_term/common/color_demo.c \
	examples/boxrig_term/common/font_demo.c \
	examples/boxrig_term/common/decoration_demo.c \
	examples/boxrig_term/common/background_demo.c \
	examples/boxrig_term/common/scrollbar_demo.c \
	examples/boxrig_term/common/link_gesture.c \
	$(POSIX_URI_OBJECT)
WAYLAND_EXAMPLE_SOURCES := examples/boxrig_term/wayland/main.c \
	examples/boxrig_term/wayland/application.c \
	examples/boxrig_term/wayland/native_host.c \
	examples/boxrig_term/wayland/pointer.c \
	examples/boxrig_term/wayland/event_loop.c \
	examples/boxrig_term/wayland/decoration_policy.c \
	examples/boxrig_term/wayland/root_backing_policy.c \
	examples/boxrig_term/wayland/shortcut_policy.c \
	examples/boxrig_term/common/runtime_info.c \
	examples/boxrig_term/common/posix_application.c \
	examples/boxrig_term/common/mouse_policy.c \
	examples/boxrig_term/common/text_policy.c \
	examples/boxrig_term/common/color_demo.c \
	examples/boxrig_term/common/font_demo.c \
	examples/boxrig_term/common/decoration_demo.c \
	examples/boxrig_term/common/background_demo.c \
	examples/boxrig_term/common/scrollbar_demo.c \
	examples/boxrig_term/common/link_gesture.c \
	$(POSIX_URI_OBJECT)
MINGW_AMD64_BUILD_DIR := $(BUILD_DIR)/win32/amd64
MINGW_I686_BUILD_DIR := $(BUILD_DIR)/win32/i686
MINGW_AMD64_CORE_OBJECTS := $(patsubst src/core/%.c,$(MINGW_AMD64_BUILD_DIR)/core/%.o,$(CORE_SOURCES))
MINGW_I686_CORE_OBJECTS := $(patsubst src/core/%.c,$(MINGW_I686_BUILD_DIR)/core/%.o,$(CORE_SOURCES))
MINGW_AMD64_UI_OBJECTS := $(patsubst src/ui/%.c,$(MINGW_AMD64_BUILD_DIR)/ui/%.o,$(UI_SOURCES))
MINGW_I686_UI_OBJECTS := $(patsubst src/ui/%.c,$(MINGW_I686_BUILD_DIR)/ui/%.o,$(UI_SOURCES))
MINGW_AMD64_SESSION_OBJECTS := $(patsubst src/session/%.c,$(MINGW_AMD64_BUILD_DIR)/session/%.o,$(SESSION_SOURCES))
MINGW_I686_SESSION_OBJECTS := $(patsubst src/session/%.c,$(MINGW_I686_BUILD_DIR)/session/%.o,$(SESSION_SOURCES))
WIN32_SOURCES := $(wildcard src/win32/*.c)
WIN32_HEADERS := include/boxrig/win32.h $(wildcard include/boxrig/win32/*.h)
MINGW_AMD64_WIN32_OBJECTS := $(patsubst src/win32/%.c,$(MINGW_AMD64_BUILD_DIR)/win32/%.o,$(WIN32_SOURCES))
MINGW_I686_WIN32_OBJECTS := $(patsubst src/win32/%.c,$(MINGW_I686_BUILD_DIR)/win32/%.o,$(WIN32_SOURCES))
MINGW_AMD64_UTF8PROC_OBJECT := $(MINGW_AMD64_BUILD_DIR)/core/utf8proc.o
MINGW_I686_UTF8PROC_OBJECT := $(MINGW_I686_BUILD_DIR)/core/utf8proc.o
MINGW_AMD64_ZLIB_OBJECTS := $(patsubst deps/zlib/%.c,$(MINGW_AMD64_BUILD_DIR)/deps/zlib/%.o,$(ZLIB_SOURCES))
MINGW_I686_ZLIB_OBJECTS := $(patsubst deps/zlib/%.c,$(MINGW_I686_BUILD_DIR)/deps/zlib/%.o,$(ZLIB_SOURCES))
MINGW_AMD64_LIBPNG_OBJECTS := $(patsubst deps/libpng/%.c,$(MINGW_AMD64_BUILD_DIR)/deps/libpng/%.o,$(LIBPNG_SOURCES))
MINGW_I686_LIBPNG_OBJECTS := $(patsubst deps/libpng/%.c,$(MINGW_I686_BUILD_DIR)/deps/libpng/%.o,$(LIBPNG_SOURCES))
MINGW_AMD64_LIBRARY := $(MINGW_AMD64_BUILD_DIR)/libboxrig.a
MINGW_I686_LIBRARY := $(MINGW_I686_BUILD_DIR)/libboxrig.a
MINGW_AMD64_UI_LIBRARY := $(MINGW_AMD64_BUILD_DIR)/libboxrig-ui.a
MINGW_I686_UI_LIBRARY := $(MINGW_I686_BUILD_DIR)/libboxrig-ui.a
MINGW_AMD64_SESSION_LIBRARY := $(MINGW_AMD64_BUILD_DIR)/libboxrig-session.a
MINGW_I686_SESSION_LIBRARY := $(MINGW_I686_BUILD_DIR)/libboxrig-session.a
MINGW_AMD64_WIN32_LIBRARY := $(MINGW_AMD64_BUILD_DIR)/libboxrig-win32.a
MINGW_I686_WIN32_LIBRARY := $(MINGW_I686_BUILD_DIR)/libboxrig-win32.a
MINGW_AMD64_CORE_TEST := $(MINGW_AMD64_BUILD_DIR)/tests/test_terminal.exe
MINGW_I686_CORE_TEST := $(MINGW_I686_BUILD_DIR)/tests/test_terminal.exe
MINGW_AMD64_GRAPHICS_TEST := $(MINGW_AMD64_BUILD_DIR)/tests/test_graphics.exe
MINGW_I686_GRAPHICS_TEST := $(MINGW_I686_BUILD_DIR)/tests/test_graphics.exe
MINGW_AMD64_UI_PLATFORM_FIXTURE_TEST := $(MINGW_AMD64_BUILD_DIR)/tests/test_ui_platform_fixture.exe
MINGW_I686_UI_PLATFORM_FIXTURE_TEST := $(MINGW_I686_BUILD_DIR)/tests/test_ui_platform_fixture.exe
MINGW_AMD64_COMPONENT_FIXTURE_TEST := \
	$(MINGW_AMD64_BUILD_DIR)/tests/test_win32_component_fixture.exe
MINGW_I686_COMPONENT_FIXTURE_TEST := \
	$(MINGW_I686_BUILD_DIR)/tests/test_win32_component_fixture.exe
MINGW_AMD64_SESSION_TEST := $(MINGW_AMD64_BUILD_DIR)/tests/test_win32_session.exe
MINGW_I686_SESSION_TEST := $(MINGW_I686_BUILD_DIR)/tests/test_win32_session.exe
MINGW_AMD64_PTY_TEST := $(MINGW_AMD64_BUILD_DIR)/tests/test_win32_pty.exe
MINGW_I686_PTY_TEST := $(MINGW_I686_BUILD_DIR)/tests/test_win32_pty.exe
MINGW_AMD64_PTY_HELPER := $(MINGW_AMD64_BUILD_DIR)/tests/pty_helper.exe
MINGW_I686_PTY_HELPER := $(MINGW_I686_BUILD_DIR)/tests/pty_helper.exe
MINGW_AMD64_RENDERER_TEST := $(MINGW_AMD64_BUILD_DIR)/tests/test_win32_renderer.exe
MINGW_I686_RENDERER_TEST := $(MINGW_I686_BUILD_DIR)/tests/test_win32_renderer.exe
MINGW_AMD64_OPACITY_NATIVE_TEST := $(MINGW_AMD64_BUILD_DIR)/tests/test_win32_opacity_native.exe
MINGW_I686_OPACITY_NATIVE_TEST := $(MINGW_I686_BUILD_DIR)/tests/test_win32_opacity_native.exe
MINGW_AMD64_HOST_TRANSPARENCY_TEST := $(MINGW_AMD64_BUILD_DIR)/tests/test_win32_host_transparency.exe
MINGW_I686_HOST_TRANSPARENCY_TEST := $(MINGW_I686_BUILD_DIR)/tests/test_win32_host_transparency.exe
MINGW_AMD64_TRANSPARENCY_TEST := $(MINGW_AMD64_BUILD_DIR)/tests/test_win32_transparency.exe
MINGW_I686_TRANSPARENCY_TEST := $(MINGW_I686_BUILD_DIR)/tests/test_win32_transparency.exe
MINGW_AMD64_EVENT_LOOP_TEST := $(MINGW_AMD64_BUILD_DIR)/tests/test_win32_event_loop.exe
MINGW_I686_EVENT_LOOP_TEST := $(MINGW_I686_BUILD_DIR)/tests/test_win32_event_loop.exe
MINGW_AMD64_HOST_TEST := $(MINGW_AMD64_BUILD_DIR)/tests/test_win32_host_adapter.exe
MINGW_I686_HOST_TEST := $(MINGW_I686_BUILD_DIR)/tests/test_win32_host_adapter.exe
MINGW_AMD64_INPUT_TEST := $(MINGW_AMD64_BUILD_DIR)/tests/test_win32_input_adapter.exe
MINGW_I686_INPUT_TEST := $(MINGW_I686_BUILD_DIR)/tests/test_win32_input_adapter.exe
MINGW_AMD64_COMPONENT_TEST := $(MINGW_AMD64_BUILD_DIR)/tests/test_win32_component.exe
MINGW_I686_COMPONENT_TEST := $(MINGW_I686_BUILD_DIR)/tests/test_win32_component.exe
MINGW_AMD64_EXAMPLE := $(BUILD_DIR)/boxrig_term/win32/amd64/boxrig_term.exe
MINGW_I686_EXAMPLE := $(BUILD_DIR)/boxrig_term/win32/i686/boxrig_term.exe
MINGW_AMD64_EXAMPLE_RESOURCE := $(MINGW_AMD64_BUILD_DIR)/boxrig_term_manifest.o
MINGW_I686_EXAMPLE_RESOURCE := $(MINGW_I686_BUILD_DIR)/boxrig_term_manifest.o
MINGW_AMD64_EMITTER := $(BUILD_DIR)/boxrig_emitter/win32/amd64/boxrig_emitter.exe
MINGW_I686_EMITTER := $(BUILD_DIR)/boxrig_emitter/win32/i686/boxrig_emitter.exe
MINGW_AMD64_SHUTDOWN_PROBE_EXAMPLE := $(MINGW_AMD64_BUILD_DIR)/boxrig-win32-shutdown-probe.exe
MINGW_I686_SHUTDOWN_PROBE_EXAMPLE := $(MINGW_I686_BUILD_DIR)/boxrig-win32-shutdown-probe.exe
MINGW_AMD64_IME_MOVEMENT_PROBE_EXAMPLE := $(MINGW_AMD64_BUILD_DIR)/boxrig-win32-ime-movement-probe.exe
MINGW_I686_IME_MOVEMENT_PROBE_EXAMPLE := $(MINGW_I686_BUILD_DIR)/boxrig-win32-ime-movement-probe.exe
MINGW_AMD64_FRONTEND_SAMPLE_PROBE := $(MINGW_AMD64_BUILD_DIR)/boxrig-sample-probe.exe
MINGW_I686_FRONTEND_SAMPLE_PROBE := $(MINGW_I686_BUILD_DIR)/boxrig-sample-probe.exe
MINGW_AMD64_LAUNCHER := $(BUILD_DIR)/boxrig_term/win32/amd64/boxrig_term_launcher.exe
MINGW_I686_LAUNCHER := $(BUILD_DIR)/boxrig_term/win32/i686/boxrig_term_launcher.exe
MINGW_AMD64_TEST_LAUNCHER := $(MINGW_AMD64_BUILD_DIR)/boxrig_term_launcher.exe
MINGW_I686_TEST_LAUNCHER := $(MINGW_I686_BUILD_DIR)/boxrig_term_launcher.exe
MINGW_AMD64_SAMPLE_TEST := $(MINGW_AMD64_BUILD_DIR)/tests/test_win32_sample.exe
MINGW_I686_SAMPLE_TEST := $(MINGW_I686_BUILD_DIR)/tests/test_win32_sample.exe
MINGW_AMD64_NOTIFICATION_EXAMPLE := $(MINGW_AMD64_BUILD_DIR)/boxrig-win32-notification-test.exe
MINGW_I686_NOTIFICATION_EXAMPLE := $(MINGW_I686_BUILD_DIR)/boxrig-win32-notification-test.exe
MINGW_AMD64_EXAMPLE_CONPTY_ASSETS := \
	$(BUILD_DIR)/boxrig_term/win32/amd64/conpty.dll \
	$(BUILD_DIR)/boxrig_term/win32/amd64/LICENSE.conpty.txt \
	$(BUILD_DIR)/boxrig_term/win32/amd64/x64/OpenConsole.exe \
	$(BUILD_DIR)/boxrig_term/win32/amd64/arm64/OpenConsole.exe
MINGW_I686_EXAMPLE_CONPTY_ASSETS := \
	$(BUILD_DIR)/boxrig_term/win32/i686/conpty.dll \
	$(BUILD_DIR)/boxrig_term/win32/i686/LICENSE.conpty.txt \
	$(BUILD_DIR)/boxrig_term/win32/i686/x86/OpenConsole.exe \
	$(BUILD_DIR)/boxrig_term/win32/i686/x64/OpenConsole.exe \
	$(BUILD_DIR)/boxrig_term/win32/i686/arm64/OpenConsole.exe
MINGW_AMD64_BUILD_CONPTY_ASSETS := \
	$(MINGW_AMD64_BUILD_DIR)/conpty.dll \
	$(MINGW_AMD64_BUILD_DIR)/x64/OpenConsole.exe \
	$(MINGW_AMD64_BUILD_DIR)/arm64/OpenConsole.exe
MINGW_I686_BUILD_CONPTY_ASSETS := \
	$(MINGW_I686_BUILD_DIR)/conpty.dll \
	$(MINGW_I686_BUILD_DIR)/x86/OpenConsole.exe \
	$(MINGW_I686_BUILD_DIR)/x64/OpenConsole.exe \
	$(MINGW_I686_BUILD_DIR)/arm64/OpenConsole.exe
MINGW_AMD64_TEST_CONPTY_ASSETS := \
	$(MINGW_AMD64_BUILD_DIR)/tests/conpty.dll \
	$(MINGW_AMD64_BUILD_DIR)/tests/x64/OpenConsole.exe \
	$(MINGW_AMD64_BUILD_DIR)/tests/arm64/OpenConsole.exe
MINGW_I686_TEST_CONPTY_ASSETS := \
	$(MINGW_I686_BUILD_DIR)/tests/conpty.dll \
	$(MINGW_I686_BUILD_DIR)/tests/x86/OpenConsole.exe \
	$(MINGW_I686_BUILD_DIR)/tests/x64/OpenConsole.exe \
	$(MINGW_I686_BUILD_DIR)/tests/arm64/OpenConsole.exe
WIN32_EXAMPLE_SOURCES := examples/boxrig_term/win32/main.c \
	examples/boxrig_term/win32/arguments.c \
	examples/boxrig_term/win32/application.c \
	examples/boxrig_term/win32/first_frame_fence.c \
	examples/boxrig_term/win32/native_host.c \
	examples/boxrig_term/win32/opacity.c \
	examples/boxrig_term/win32/message_loop.c \
	examples/boxrig_term/win32/phase_trace.c \
	examples/boxrig_term/win32/renderer_trace.c \
	examples/boxrig_term/win32/presentation_wait.c \
	examples/boxrig_term/win32/process.c \
	examples/boxrig_term/common/runtime_info.c \
	examples/boxrig_term/common/mouse_policy.c \
	examples/boxrig_term/common/text_policy.c \
	examples/boxrig_term/common/color_demo.c \
	examples/boxrig_term/common/decoration_demo.c \
	examples/boxrig_term/common/background_demo.c \
	examples/boxrig_term/common/scrollbar_demo.c \
	examples/boxrig_term/common/link_gesture.c \
	examples/boxrig_term/win32/uri_launcher.c
$(MINGW_AMD64_EXAMPLE) $(MINGW_I686_EXAMPLE) \
	$(MINGW_AMD64_SHUTDOWN_PROBE_EXAMPLE) $(MINGW_I686_SHUTDOWN_PROBE_EXAMPLE) \
	$(MINGW_AMD64_NOTIFICATION_EXAMPLE) $(MINGW_I686_NOTIFICATION_EXAMPLE): \
	include/boxrig/ui/scrollbar.h \
	include/boxrig/win32/event.h
WINE_PREFIX := $(abspath $(BUILD_DIR)/wine)
WINESERVER ?= wineserver
WINE_RUN := env WINEARCH=win64 WINEDEBUG=-all \
	WINEDLLOVERRIDES=mscoree,mshtml= WINEPREFIX=$(WINE_PREFIX) \
	BOXRIG_TEST_ALLOW_CONPTY_NORMALIZATION=wine sh -c \
	'$(WINESERVER) -k >/dev/null 2>&1 || true; \
	$(WINESERVER) -w >/dev/null 2>&1 || true; \
	$(XVFB_RUN) -a $(WINE) "$$@"; wine_status=$$?; \
	$(WINESERVER) -k >/dev/null 2>&1 || true; \
	$(WINESERVER) -w >/dev/null 2>&1 || true; \
	exit $$wine_status' boxrig-wine-run

define verify_archive_members
	@if test -f $(1); then \
		actual="$$($(2) t $(1) | tr '\n' ' ')"; \
		expected="$(notdir $(3)) "; \
		if test "$$actual" != "$$expected"; then \
			$(RM) $(1); \
			$(2) rcs $(1) $(3); \
		fi; \
	fi
endef


# Optional Win32 OpenGL packages; software-only targets keep their imports.
.PHONY: win32-opengl install-win32-opengl-amd64 install-win32-opengl-i686
win32-opengl: win32-opengl-amd64 win32-opengl-i686

MINGW_AMD64_OGL_OBJECTS := $(MINGW_AMD64_BUILD_DIR)/opengl/runtime.o $(MINGW_AMD64_BUILD_DIR)/opengl/renderer.o $(MINGW_AMD64_BUILD_DIR)/opengl/text_provider.o
MINGW_AMD64_WGL_OBJECTS := $(MINGW_AMD64_BUILD_DIR)/wgl/binding.o $(MINGW_AMD64_BUILD_DIR)/wgl/surface.o $(MINGW_AMD64_BUILD_DIR)/wgl/component.o
MINGW_AMD64_OGL_LIBRARY := $(MINGW_AMD64_BUILD_DIR)/libboxrig-render-ogl.a
MINGW_AMD64_WGL_LIBRARY := $(MINGW_AMD64_BUILD_DIR)/libboxrig-win32-ogl.a
MINGW_AMD64_OGL_DLL := $(MINGW_AMD64_BUILD_DIR)/boxrig-render-ogl.dll
MINGW_AMD64_WGL_DLL := $(MINGW_AMD64_BUILD_DIR)/boxrig-win32-ogl.dll
.PHONY: win32-opengl-amd64
win32-opengl-amd64: $(MINGW_AMD64_OGL_LIBRARY) $(MINGW_AMD64_WGL_LIBRARY) $(MINGW_AMD64_OGL_DLL) $(MINGW_AMD64_WGL_DLL)

$(MINGW_AMD64_BUILD_DIR)/opengl/%.o: src/opengl/%.c src/opengl/runtime.h src/opengl/renderer.h src/opengl/text_provider.h $(OPENGL_RENDERER_HEADERS)
	mkdir -p $(@D)
	$(MINGW_AMD64_CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(MINGW_UI_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) -c $< -o $@

$(MINGW_AMD64_BUILD_DIR)/opengl/text_provider.o: src/win32_opengl/text_provider.c src/win32/text_private.h src/win32/renderer_private.h src/opengl/text_provider.h $(OPENGL_RENDERER_HEADERS)
	mkdir -p $(@D)
	$(MINGW_AMD64_CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(MINGW_UI_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) -c $< -o $@

$(MINGW_AMD64_BUILD_DIR)/wgl/%.o: src/win32_opengl/%.c src/win32_opengl/surface_private.h include/boxrig/win32/wgl.h include/boxrig/win32/opengl.h $(OPENGL_RENDERER_HEADERS)
	mkdir -p $(@D)
	$(MINGW_AMD64_CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(MINGW_UI_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) -c $< -o $@

$(MINGW_AMD64_OGL_LIBRARY): $(MINGW_AMD64_OGL_OBJECTS)
	rm -f $@
	$(MINGW_AMD64_AR) rcs $@ $^

$(MINGW_AMD64_WGL_LIBRARY): $(MINGW_AMD64_WGL_OBJECTS)
	rm -f $@
	$(MINGW_AMD64_AR) rcs $@ $^

$(MINGW_AMD64_OGL_DLL): $(MINGW_AMD64_OGL_OBJECTS) $(MINGW_AMD64_WIN32_LIBRARY) $(MINGW_AMD64_UI_LIBRARY) $(MINGW_AMD64_LIBRARY) packaging/win32/boxrig-render-ogl.def
	$(MINGW_AMD64_CC) -shared -static-libgcc $(MINGW_AMD64_OGL_OBJECTS) packaging/win32/boxrig-render-ogl.def $(MINGW_AMD64_WIN32_LIBRARY) $(MINGW_AMD64_UI_LIBRARY) $(MINGW_AMD64_LIBRARY) $(WIN32_RENDERER_LIBS) -lm -Wl,--out-implib,$(MINGW_AMD64_BUILD_DIR)/libboxrig-render-ogl.dll.a -o $@

$(MINGW_AMD64_BUILD_DIR)/libboxrig-render-ogl.dll.a: $(MINGW_AMD64_OGL_DLL)
	@test -f $@ || { rm -f $<; $(MAKE) $<; }

$(MINGW_AMD64_WGL_DLL): $(MINGW_AMD64_WIN32_LIBRARY) $(MINGW_AMD64_UI_LIBRARY) $(MINGW_AMD64_LIBRARY) $(MINGW_AMD64_WGL_OBJECTS) $(MINGW_AMD64_BUILD_DIR)/libboxrig-render-ogl.dll.a packaging/win32/boxrig-win32-ogl.def
	$(MINGW_AMD64_CC) -shared -static-libgcc $(MINGW_AMD64_WGL_OBJECTS) packaging/win32/boxrig-win32-ogl.def $(MINGW_AMD64_BUILD_DIR)/libboxrig-render-ogl.dll.a $(MINGW_AMD64_WIN32_LIBRARY) $(MINGW_AMD64_UI_LIBRARY) $(MINGW_AMD64_LIBRARY) $(WIN32_RENDERER_LIBS) -lcomctl32 -lopengl32 -lgdi32 -luser32 -Wl,--out-implib,$(MINGW_AMD64_BUILD_DIR)/libboxrig-win32-ogl.dll.a -o $@

$(MINGW_AMD64_BUILD_DIR)/libboxrig-win32-ogl.dll.a: $(MINGW_AMD64_WGL_DLL)
	@test -f $@ || { rm -f $<; $(MAKE) $<; }

install-win32-opengl-amd64: win32-opengl-amd64 $(MINGW_AMD64_BUILD_DIR)/libboxrig-render-ogl.dll.a $(MINGW_AMD64_BUILD_DIR)/libboxrig-win32-ogl.dll.a
	$(INSTALL) -d $(DESTDIR)$(LIBDIR) $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(PKGCONFIGDIR) $(DESTDIR)$(INCLUDEDIR)/boxrig/ui $(DESTDIR)$(INCLUDEDIR)/boxrig/render $(DESTDIR)$(INCLUDEDIR)/boxrig/win32
	$(INSTALL) -m 644 $(MINGW_AMD64_WIN32_LIBRARY) $(MINGW_AMD64_UI_LIBRARY) $(MINGW_AMD64_LIBRARY) $(MINGW_AMD64_OGL_LIBRARY) $(MINGW_AMD64_WGL_LIBRARY) $(MINGW_AMD64_BUILD_DIR)/libboxrig-render-ogl.dll.a $(MINGW_AMD64_BUILD_DIR)/libboxrig-win32-ogl.dll.a $(DESTDIR)$(LIBDIR)
	$(INSTALL) -m 755 $(MINGW_AMD64_OGL_DLL) $(MINGW_AMD64_WGL_DLL) $(DESTDIR)$(PREFIX)/bin
	$(INSTALL) -m 644 $(CORE_PUBLIC_HEADERS) include/boxrig/ui.h $(DESTDIR)$(INCLUDEDIR)/boxrig
	$(INSTALL) -m 644 $(wildcard include/boxrig/ui/*.h) $(DESTDIR)$(INCLUDEDIR)/boxrig/ui
	$(INSTALL) -m 644 $(OPENGL_RENDERER_HEADERS) $(DESTDIR)$(INCLUDEDIR)/boxrig/render
	$(INSTALL) -m 644 $(filter-out include/boxrig/win32/d3d11_component.h,$(wildcard include/boxrig/win32/*.h)) $(DESTDIR)$(INCLUDEDIR)/boxrig/win32
	$(INSTALL) -m 644 packaging/win32/boxrig-render-ogl.pc packaging/win32/boxrig-win32-ogl.pc $(DESTDIR)$(PKGCONFIGDIR)
	$(INSTALL) -m 644 examples/win32/renderer.c $(DESTDIR)$(PREFIX)/share/boxrig/examples/win32

MINGW_I686_OGL_OBJECTS := $(MINGW_I686_BUILD_DIR)/opengl/runtime.o $(MINGW_I686_BUILD_DIR)/opengl/renderer.o $(MINGW_I686_BUILD_DIR)/opengl/text_provider.o
MINGW_I686_WGL_OBJECTS := $(MINGW_I686_BUILD_DIR)/wgl/binding.o $(MINGW_I686_BUILD_DIR)/wgl/surface.o $(MINGW_I686_BUILD_DIR)/wgl/component.o
MINGW_I686_OGL_LIBRARY := $(MINGW_I686_BUILD_DIR)/libboxrig-render-ogl.a
MINGW_I686_WGL_LIBRARY := $(MINGW_I686_BUILD_DIR)/libboxrig-win32-ogl.a
MINGW_I686_OGL_DLL := $(MINGW_I686_BUILD_DIR)/boxrig-render-ogl.dll
MINGW_I686_WGL_DLL := $(MINGW_I686_BUILD_DIR)/boxrig-win32-ogl.dll
.PHONY: win32-opengl-i686
win32-opengl-i686: $(MINGW_I686_OGL_LIBRARY) $(MINGW_I686_WGL_LIBRARY) $(MINGW_I686_OGL_DLL) $(MINGW_I686_WGL_DLL)

$(MINGW_I686_BUILD_DIR)/opengl/%.o: src/opengl/%.c src/opengl/runtime.h src/opengl/renderer.h src/opengl/text_provider.h $(OPENGL_RENDERER_HEADERS)
	mkdir -p $(@D)
	$(MINGW_I686_CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(MINGW_UI_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) -c $< -o $@

$(MINGW_I686_BUILD_DIR)/opengl/text_provider.o: src/win32_opengl/text_provider.c src/win32/text_private.h src/win32/renderer_private.h src/opengl/text_provider.h $(OPENGL_RENDERER_HEADERS)
	mkdir -p $(@D)
	$(MINGW_I686_CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(MINGW_UI_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) -c $< -o $@

$(MINGW_I686_BUILD_DIR)/wgl/%.o: src/win32_opengl/%.c src/win32_opengl/surface_private.h include/boxrig/win32/wgl.h include/boxrig/win32/opengl.h $(OPENGL_RENDERER_HEADERS)
	mkdir -p $(@D)
	$(MINGW_I686_CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(MINGW_UI_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) -c $< -o $@

$(MINGW_I686_OGL_LIBRARY): $(MINGW_I686_OGL_OBJECTS)
	rm -f $@
	$(MINGW_I686_AR) rcs $@ $^

$(MINGW_I686_WGL_LIBRARY): $(MINGW_I686_WGL_OBJECTS)
	rm -f $@
	$(MINGW_I686_AR) rcs $@ $^

$(MINGW_I686_OGL_DLL): $(MINGW_I686_OGL_OBJECTS) $(MINGW_I686_WIN32_LIBRARY) $(MINGW_I686_UI_LIBRARY) $(MINGW_I686_LIBRARY) packaging/win32/boxrig-render-ogl.def
	$(MINGW_I686_CC) -shared -static-libgcc $(MINGW_I686_OGL_OBJECTS) packaging/win32/boxrig-render-ogl.def $(MINGW_I686_WIN32_LIBRARY) $(MINGW_I686_UI_LIBRARY) $(MINGW_I686_LIBRARY) $(WIN32_RENDERER_LIBS) -lm -Wl,--out-implib,$(MINGW_I686_BUILD_DIR)/libboxrig-render-ogl.dll.a -o $@

$(MINGW_I686_BUILD_DIR)/libboxrig-render-ogl.dll.a: $(MINGW_I686_OGL_DLL)
	@test -f $@ || { rm -f $<; $(MAKE) $<; }

$(MINGW_I686_WGL_DLL): $(MINGW_I686_WIN32_LIBRARY) $(MINGW_I686_UI_LIBRARY) $(MINGW_I686_LIBRARY) $(MINGW_I686_WGL_OBJECTS) $(MINGW_I686_BUILD_DIR)/libboxrig-render-ogl.dll.a packaging/win32/boxrig-win32-ogl.def
	$(MINGW_I686_CC) -shared -static-libgcc $(MINGW_I686_WGL_OBJECTS) packaging/win32/boxrig-win32-ogl.def $(MINGW_I686_BUILD_DIR)/libboxrig-render-ogl.dll.a $(MINGW_I686_WIN32_LIBRARY) $(MINGW_I686_UI_LIBRARY) $(MINGW_I686_LIBRARY) $(WIN32_RENDERER_LIBS) -lcomctl32 -lopengl32 -lgdi32 -luser32 -Wl,--out-implib,$(MINGW_I686_BUILD_DIR)/libboxrig-win32-ogl.dll.a -o $@

$(MINGW_I686_BUILD_DIR)/libboxrig-win32-ogl.dll.a: $(MINGW_I686_WGL_DLL)
	@test -f $@ || { rm -f $<; $(MAKE) $<; }

install-win32-opengl-i686: win32-opengl-i686 $(MINGW_I686_BUILD_DIR)/libboxrig-render-ogl.dll.a $(MINGW_I686_BUILD_DIR)/libboxrig-win32-ogl.dll.a
	$(INSTALL) -d $(DESTDIR)$(LIBDIR) $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(PKGCONFIGDIR) $(DESTDIR)$(INCLUDEDIR)/boxrig/ui $(DESTDIR)$(INCLUDEDIR)/boxrig/render $(DESTDIR)$(INCLUDEDIR)/boxrig/win32
	$(INSTALL) -m 644 $(MINGW_I686_WIN32_LIBRARY) $(MINGW_I686_UI_LIBRARY) $(MINGW_I686_LIBRARY) $(MINGW_I686_OGL_LIBRARY) $(MINGW_I686_WGL_LIBRARY) $(MINGW_I686_BUILD_DIR)/libboxrig-render-ogl.dll.a $(MINGW_I686_BUILD_DIR)/libboxrig-win32-ogl.dll.a $(DESTDIR)$(LIBDIR)
	$(INSTALL) -m 755 $(MINGW_I686_OGL_DLL) $(MINGW_I686_WGL_DLL) $(DESTDIR)$(PREFIX)/bin
	$(INSTALL) -m 644 $(CORE_PUBLIC_HEADERS) include/boxrig/ui.h $(DESTDIR)$(INCLUDEDIR)/boxrig
	$(INSTALL) -m 644 $(wildcard include/boxrig/ui/*.h) $(DESTDIR)$(INCLUDEDIR)/boxrig/ui
	$(INSTALL) -m 644 $(OPENGL_RENDERER_HEADERS) $(DESTDIR)$(INCLUDEDIR)/boxrig/render
	$(INSTALL) -m 644 $(filter-out include/boxrig/win32/d3d11_component.h,$(wildcard include/boxrig/win32/*.h)) $(DESTDIR)$(INCLUDEDIR)/boxrig/win32
	$(INSTALL) -m 644 packaging/win32/boxrig-render-ogl.pc packaging/win32/boxrig-win32-ogl.pc $(DESTDIR)$(PKGCONFIGDIR)
	$(INSTALL) -m 644 examples/win32/renderer.c $(DESTDIR)$(PREFIX)/share/boxrig/examples/win32

.PHONY: all native boxrig-term-gtk4 boxrig-term-gtk3 boxrig-term-wayland install-gtk4 install-gtk3 install-wayland install-render-ogl install-x11-opengl install-wayland-opengl win32 win32-amd64 \
	win32-i686 test \
	test-default-goal measure-text-workloads test-p2 test-p2-x11 \
	test-p2-x11-opengl test-p2-opengl test-p2-win32 test-gtk4-package \
	test-gtk3-package test-wayland-package test-opengl-packages \
	test-core-ui-boundary test-gtk3-gestament force-wayland-example-config clean
.PHONY: verify-native-core-archive verify-native-ui-archive \
	verify-native-session-archive verify-native-posix-archive \
	verify-native-x11-archive verify-native-x11-opengl-archive \
	verify-native-gtk4-archive \
	verify-native-gtk3-archive verify-native-wayland-archive \
	verify-amd64-core-archive \
	verify-i686-core-archive verify-amd64-ui-archive \
	verify-i686-ui-archive verify-amd64-session-archive \
	verify-i686-session-archive verify-amd64-win32-archive \
	verify-i686-win32-archive

all: native win32

.PHONY: test-wayland-protocol-build
test: test-wayland-protocol-build
test-wayland-protocol-build:
	sh tests/wayland/test_protocol_build.sh "$(MAKE)" "$(CC)"

test-default-goal:
	@test "$(.DEFAULT_GOAL)" = all || { \
		printf 'default goal: expected all, got %s\n' "$(.DEFAULT_GOAL)" >&2; \
		exit 1; \
	}

native: $(LIBRARY) $(UI_LIBRARY) $(SESSION_LIBRARY) $(POSIX_LIBRARY) \
	$(X11_LIBRARY) $(GTK4_LIBRARY) $(GTK3_LIBRARY) $(WAYLAND_LIBRARY) \
	$(OPENGL_RENDERER_LIBRARY) $(X11_OPENGL_LIBRARY) \
	$(WAYLAND_OPENGL_LIBRARY) $(EXAMPLE_BINARY) \
	$(GTK4_EXAMPLE_BINARY) $(GTK3_EXAMPLE_BINARY) $(WAYLAND_EXAMPLE_BINARY) \
	$(POSIX_EMITTER_BINARY)

boxrig-term-gtk4: $(GTK4_EXAMPLE_BINARY)

boxrig-term-gtk3: $(GTK3_EXAMPLE_BINARY)

boxrig-term-wayland: $(WAYLAND_EXAMPLE_BINARY)

install-gtk4: $(LIBRARY) $(UI_LIBRARY) $(GTK4_LIBRARY) \
		$(GTK4_HEADERS) $(UI_HEADERS) $(GTK4_PKG_CONFIG)
	$(INSTALL) -d $(DESTDIR)$(LIBDIR) \
		$(DESTDIR)$(INCLUDEDIR)/boxrig/gtk4 \
		$(DESTDIR)$(INCLUDEDIR)/boxrig/gtk \
		$(DESTDIR)$(INCLUDEDIR)/boxrig/ui \
		$(DESTDIR)$(PKGCONFIGDIR)
	$(INSTALL) -m 644 $(LIBRARY) $(UI_LIBRARY) $(GTK4_LIBRARY) \
		$(DESTDIR)$(LIBDIR)
	$(INSTALL) -m 644 $(CORE_PUBLIC_HEADERS) include/boxrig/ui.h \
		include/boxrig/gtk4.h include/boxrig/ime.h $(DESTDIR)$(INCLUDEDIR)/boxrig
	$(INSTALL) -m 644 include/boxrig/ui/*.h \
		$(DESTDIR)$(INCLUDEDIR)/boxrig/ui
	$(INSTALL) -m 644 include/boxrig/gtk4/*.h \
		$(DESTDIR)$(INCLUDEDIR)/boxrig/gtk4
	$(INSTALL) -m 644 include/boxrig/gtk/*.h $(DESTDIR)$(INCLUDEDIR)/boxrig/gtk
	$(INSTALL) -m 644 $(GTK4_PKG_CONFIG) \
		$(DESTDIR)$(PKGCONFIGDIR)
	$(INSTALL) -d $(DESTDIR)$(PREFIX)/share/boxrig/examples/gtk4
	$(INSTALL) -m 644 examples/gtk4/external_scrollbar.h $(DESTDIR)$(PREFIX)/share/boxrig/examples/gtk4

install-gtk3: $(LIBRARY) $(UI_LIBRARY) $(GTK3_LIBRARY) \
		$(GTK3_HEADERS) $(UI_HEADERS) $(GTK3_PKG_CONFIG)
	$(INSTALL) -d $(DESTDIR)$(LIBDIR) \
		$(DESTDIR)$(INCLUDEDIR)/boxrig/gtk3 \
		$(DESTDIR)$(INCLUDEDIR)/boxrig/gtk \
		$(DESTDIR)$(INCLUDEDIR)/boxrig/ui \
		$(DESTDIR)$(PKGCONFIGDIR)
	$(INSTALL) -m 644 $(LIBRARY) $(UI_LIBRARY) $(GTK3_LIBRARY) \
		$(DESTDIR)$(LIBDIR)
	$(INSTALL) -m 644 $(CORE_PUBLIC_HEADERS) include/boxrig/ui.h \
		include/boxrig/gtk3.h include/boxrig/ime.h $(DESTDIR)$(INCLUDEDIR)/boxrig
	$(INSTALL) -m 644 include/boxrig/ui/*.h \
		$(DESTDIR)$(INCLUDEDIR)/boxrig/ui
	$(INSTALL) -m 644 include/boxrig/gtk3/*.h \
		$(DESTDIR)$(INCLUDEDIR)/boxrig/gtk3
	$(INSTALL) -m 644 include/boxrig/gtk/*.h $(DESTDIR)$(INCLUDEDIR)/boxrig/gtk
	$(INSTALL) -m 644 $(GTK3_PKG_CONFIG) \
		$(DESTDIR)$(PKGCONFIGDIR)
	$(INSTALL) -d $(DESTDIR)$(PREFIX)/share/boxrig/examples/gtk3
	$(INSTALL) -m 644 examples/gtk3/external_scrollbar.h $(DESTDIR)$(PREFIX)/share/boxrig/examples/gtk3

install-wayland: $(LIBRARY) $(UI_LIBRARY) $(WAYLAND_LIBRARY) \
		$(WAYLAND_HEADERS) $(UI_HEADERS) $(WAYLAND_PKG_CONFIG)
	$(INSTALL) -d $(DESTDIR)$(LIBDIR) \
		$(DESTDIR)$(INCLUDEDIR)/boxrig/ui \
		$(DESTDIR)$(PKGCONFIGDIR)
	$(INSTALL) -m 644 $(LIBRARY) $(UI_LIBRARY) $(WAYLAND_LIBRARY) \
		$(DESTDIR)$(LIBDIR)
	$(INSTALL) -m 644 $(CORE_PUBLIC_HEADERS) include/boxrig/ui.h \
		include/boxrig/wayland.h include/boxrig/ime.h $(DESTDIR)$(INCLUDEDIR)/boxrig
	$(INSTALL) -m 644 include/boxrig/ui/*.h \
		$(DESTDIR)$(INCLUDEDIR)/boxrig/ui
	$(INSTALL) -m 644 $(WAYLAND_PKG_CONFIG) \
		$(DESTDIR)$(PKGCONFIGDIR)
	$(INSTALL) -d $(DESTDIR)$(INCLUDEDIR)/boxrig/wayland
	$(INSTALL) -m 644 include/boxrig/wayland/background_effect.h $(DESTDIR)$(INCLUDEDIR)/boxrig/wayland
	$(INSTALL) -d $(DESTDIR)$(PREFIX)/share/boxrig/examples/wayland
	$(INSTALL) -m 644 examples/wayland/external_scrollbar.h $(DESTDIR)$(PREFIX)/share/boxrig/examples/wayland

install-render-ogl: $(LIBRARY) $(UI_LIBRARY) $(OPENGL_RENDERER_LIBRARY) \
		$(OPENGL_RENDERER_HEADERS) $(UI_HEADERS) $(OPENGL_RENDERER_PKG_CONFIG)
	$(INSTALL) -d $(DESTDIR)$(LIBDIR) \
		$(DESTDIR)$(INCLUDEDIR)/boxrig/render \
		$(DESTDIR)$(INCLUDEDIR)/boxrig/ui \
		$(DESTDIR)$(PKGCONFIGDIR)
	$(INSTALL) -m 644 $(LIBRARY) $(UI_LIBRARY) $(OPENGL_RENDERER_LIBRARY) \
		$(DESTDIR)$(LIBDIR)
	$(INSTALL) -m 644 $(CORE_PUBLIC_HEADERS) include/boxrig/ui.h \
		$(DESTDIR)$(INCLUDEDIR)/boxrig
	$(INSTALL) -m 644 include/boxrig/ui/*.h \
		$(DESTDIR)$(INCLUDEDIR)/boxrig/ui
	$(INSTALL) -m 644 $(OPENGL_RENDERER_HEADERS) \
		$(DESTDIR)$(INCLUDEDIR)/boxrig/render
	$(INSTALL) -m 644 $(OPENGL_RENDERER_PKG_CONFIG) \
		$(DESTDIR)$(PKGCONFIGDIR)

install-x11-opengl: $(LIBRARY) $(UI_LIBRARY) $(X11_LIBRARY) \
		$(OPENGL_RENDERER_LIBRARY) $(X11_OPENGL_LIBRARY) $(X11_HEADERS) \
		$(OPENGL_RENDERER_HEADERS) $(UI_HEADERS) \
		$(OPENGL_RENDERER_PKG_CONFIG) $(X11_OPENGL_PKG_CONFIG)
	$(INSTALL) -d $(DESTDIR)$(LIBDIR) \
		$(DESTDIR)$(INCLUDEDIR)/boxrig/render \
		$(DESTDIR)$(INCLUDEDIR)/boxrig/ui \
		$(DESTDIR)$(INCLUDEDIR)/boxrig/x11 \
		$(DESTDIR)$(PKGCONFIGDIR)
	$(INSTALL) -m 644 $(LIBRARY) $(UI_LIBRARY) $(X11_LIBRARY) \
		$(OPENGL_RENDERER_LIBRARY) $(X11_OPENGL_LIBRARY) \
		$(DESTDIR)$(LIBDIR)
	$(INSTALL) -m 644 $(CORE_PUBLIC_HEADERS) include/boxrig/ui.h \
		include/boxrig/x11.h $(DESTDIR)$(INCLUDEDIR)/boxrig
	$(INSTALL) -m 644 include/boxrig/ui/*.h \
		$(DESTDIR)$(INCLUDEDIR)/boxrig/ui
	$(INSTALL) -m 644 include/boxrig/x11/*.h \
		$(DESTDIR)$(INCLUDEDIR)/boxrig/x11
	$(INSTALL) -m 644 $(OPENGL_RENDERER_HEADERS) \
		$(DESTDIR)$(INCLUDEDIR)/boxrig/render
	$(INSTALL) -m 644 $(OPENGL_RENDERER_PKG_CONFIG) \
		$(X11_OPENGL_PKG_CONFIG) $(DESTDIR)$(PKGCONFIGDIR)
	$(INSTALL) -d $(DESTDIR)$(PREFIX)/share/boxrig/examples/x11
	$(INSTALL) -m 644 examples/x11/external_scrollbar.h $(DESTDIR)$(PREFIX)/share/boxrig/examples/x11

install-wayland-opengl: $(LIBRARY) $(UI_LIBRARY) $(WAYLAND_LIBRARY) \
		$(OPENGL_RENDERER_LIBRARY) $(WAYLAND_OPENGL_LIBRARY) \
		$(WAYLAND_HEADERS) $(WAYLAND_OPENGL_HEADERS) \
		$(OPENGL_RENDERER_HEADERS) $(UI_HEADERS) $(WAYLAND_PKG_CONFIG) \
		$(OPENGL_RENDERER_PKG_CONFIG) $(WAYLAND_OPENGL_PKG_CONFIG)
	$(INSTALL) -d $(DESTDIR)$(LIBDIR) \
		$(DESTDIR)$(INCLUDEDIR)/boxrig/render \
		$(DESTDIR)$(INCLUDEDIR)/boxrig/ui \
		$(DESTDIR)$(INCLUDEDIR)/boxrig/wayland \
		$(DESTDIR)$(PKGCONFIGDIR)
	$(INSTALL) -m 644 $(LIBRARY) $(UI_LIBRARY) $(WAYLAND_LIBRARY) \
		$(OPENGL_RENDERER_LIBRARY) $(WAYLAND_OPENGL_LIBRARY) \
		$(DESTDIR)$(LIBDIR)
	$(INSTALL) -m 644 $(CORE_PUBLIC_HEADERS) include/boxrig/ui.h \
		include/boxrig/wayland.h include/boxrig/ime.h $(DESTDIR)$(INCLUDEDIR)/boxrig
	$(INSTALL) -m 644 include/boxrig/ui/*.h \
		$(DESTDIR)$(INCLUDEDIR)/boxrig/ui
	$(INSTALL) -m 644 $(WAYLAND_OPENGL_HEADERS) \
		$(DESTDIR)$(INCLUDEDIR)/boxrig/wayland
	$(INSTALL) -m 644 include/boxrig/wayland/background_effect.h $(DESTDIR)$(INCLUDEDIR)/boxrig/wayland
	$(INSTALL) -m 644 $(OPENGL_RENDERER_HEADERS) \
		$(DESTDIR)$(INCLUDEDIR)/boxrig/render
	$(INSTALL) -m 644 $(WAYLAND_PKG_CONFIG) \
		$(OPENGL_RENDERER_PKG_CONFIG) $(WAYLAND_OPENGL_PKG_CONFIG) \
		$(DESTDIR)$(PKGCONFIGDIR)
	$(INSTALL) -d $(DESTDIR)$(PREFIX)/share/boxrig/examples/wayland
	$(INSTALL) -m 644 examples/wayland/external_scrollbar.h $(DESTDIR)$(PREFIX)/share/boxrig/examples/wayland

win32: win32-amd64 win32-i686

win32-amd64: $(MINGW_AMD64_LIBRARY) $(MINGW_AMD64_UI_LIBRARY) \
	$(MINGW_AMD64_SESSION_LIBRARY) $(MINGW_AMD64_WIN32_LIBRARY) \
	$(MINGW_AMD64_EXAMPLE) $(MINGW_AMD64_LAUNCHER) $(MINGW_AMD64_EMITTER)

win32-i686: $(MINGW_I686_LIBRARY) $(MINGW_I686_UI_LIBRARY) \
	$(MINGW_I686_SESSION_LIBRARY) $(MINGW_I686_WIN32_LIBRARY) \
	$(MINGW_I686_EXAMPLE) $(MINGW_I686_LAUNCHER) $(MINGW_I686_EMITTER)

measure-text-workloads: $(TEXT_WORKLOAD_MEASUREMENT_BINARY)
	$(TEXT_WORKLOAD_MEASUREMENT_BINARY) maximum-scrollback
	$(TEXT_WORKLOAD_MEASUREMENT_BINARY) long-soft-wrap
	$(TEXT_WORKLOAD_MEASUREMENT_BINARY) many-matches
	$(TEXT_WORKLOAD_MEASUREMENT_BINARY) continuous-output
	$(TEXT_WORKLOAD_MEASUREMENT_BINARY) large-copy-paste
	$(TEXT_WORKLOAD_MEASUREMENT_BINARY) recognizer-long-line
	$(TEXT_WORKLOAD_MEASUREMENT_BINARY) recognizer-many-patterns
	$(TEXT_WORKLOAD_MEASUREMENT_BINARY) recognizer-continuous-output
	$(TEXT_WORKLOAD_MEASUREMENT_BINARY) recognizer-off-viewport
	$(TEXT_WORKLOAD_MEASUREMENT_BINARY) recognizer-retained-matches

test: test-terminal-io-integration test-text-blink test-x11-scheduler-required test-blink test-colors test-win32-alpha-report test-win32-opacity-policy test-win32-core-artifacts test-win32-ui-artifacts $(MINGW_AMD64_OPACITY_NATIVE_TEST) $(MINGW_I686_OPACITY_NATIVE_TEST) $(MINGW_AMD64_HOST_TRANSPARENCY_TEST) $(MINGW_I686_HOST_TRANSPARENCY_TEST)

.PHONY: test-text-sync
test: test-text-sync
test-text-sync: $(TEXT_SYNC_TEST_BINARY)
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $<

$(TEXT_SYNC_TEST_BINARY): tests/test_text_sync.c $(CORE_HEADERS) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) $< $(LIBRARY) $(SANITIZERS) -o $@

.PHONY: test-recognizer-projection
test: test-recognizer-projection
test-recognizer-projection: $(BUILD_DIR)/tests/test_recognizer_projection
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $<

$(BUILD_DIR)/tests/test_recognizer_projection: tests/test_recognizer_projection.c $(CORE_HEADERS) $(TEXT_MEASUREMENT_LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) -DBOXRIG_MEASURE_TEXT_WORKLOADS $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) $< $(TEXT_MEASUREMENT_LIBRARY) $(SANITIZERS) -o $@

.PHONY: test-recognizer-budget
test: test-recognizer-budget
test-recognizer-budget: $(BUILD_DIR)/tests/test_recognizer_budget
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $<

$(BUILD_DIR)/tests/test_recognizer_budget: tests/test_recognizer_budget.c $(CORE_HEADERS) $(TEXT_MEASUREMENT_LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) -DBOXRIG_MEASURE_TEXT_WORKLOADS $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) $< $(TEXT_MEASUREMENT_LIBRARY) $(SANITIZERS) -o $@

.PHONY: test-recognizer-reuse
test: test-recognizer-reuse
test-recognizer-reuse: $(BUILD_DIR)/tests/test_recognizer_reuse
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $<

$(BUILD_DIR)/tests/test_recognizer_reuse: tests/test_recognizer_reuse.c $(CORE_HEADERS) $(TEXT_MEASUREMENT_LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) -DBOXRIG_MEASURE_TEXT_WORKLOADS $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) $< $(TEXT_MEASUREMENT_LIBRARY) $(SANITIZERS) -Wl,--wrap=calloc -Wl,--wrap=realloc -o $@

.PHONY: test-last-printed-storage
test: test-last-printed-storage
test-last-printed-storage: $(BUILD_DIR)/tests/test_last_printed_storage
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $<

$(BUILD_DIR)/tests/test_last_printed_storage: tests/test_last_printed_storage.c $(CORE_HEADERS) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) $< $(LIBRARY) $(SANITIZERS) -Wl,--wrap=malloc -Wl,--wrap=realloc -o $@

.PHONY: test-screen-row-move
test: test-screen-row-move
test-screen-row-move: $(BUILD_DIR)/tests/test_screen_row_move
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $<

$(BUILD_DIR)/tests/test_screen_row_move: tests/test_screen_row_move.c $(CORE_HEADERS) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) $< $(LIBRARY) $(SANITIZERS) -Wl,--wrap=free -o $@

.PHONY: test-history-transfer
test: test-history-transfer
test-history-transfer: $(BUILD_DIR)/tests/test_history_transfer
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $<

$(BUILD_DIR)/tests/test_history_transfer: tests/test_history_transfer.c $(CORE_HEADERS) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) $< $(LIBRARY) $(SANITIZERS) -Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=realloc -o $@

.PHONY: test-resize-history
test: test-resize-history
test-resize-history: $(BUILD_DIR)/tests/test_resize_history
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $<

$(BUILD_DIR)/tests/test_resize_history: tests/test_resize_history.c $(CORE_HEADERS) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) $< $(LIBRARY) $(SANITIZERS) -Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=realloc -o $@

$(BUILD_DIR)/tests/measure_resize_history: tests/performance/resize_history.c $(CORE_HEADERS) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(POSIX_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) $< $(LIBRARY) $(SANITIZERS) -o $@

.PHONY: test-cell-text-storage
test: test-cell-text-storage
test-cell-text-storage: $(BUILD_DIR)/tests/test_cell_text_storage
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $<

$(BUILD_DIR)/tests/test_cell_text_storage: tests/test_cell_text_storage.c $(CORE_HEADERS) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) $< $(LIBRARY) $(SANITIZERS) -Wl,--wrap=malloc -Wl,--wrap=realloc -o $@

.PHONY: test-colors
test-colors: $(BUILD_DIR)/tests/test_color $(BUILD_DIR)/tests/color-filter-example $(BUILD_DIR)/tests/test_x11_colors \
	$(BUILD_DIR)/tests/test_pangocairo_colors $(BUILD_DIR)/tests/test_opengl_colors \
	$(MINGW_AMD64_BUILD_DIR)/tests/test_win32_colors.exe $(MINGW_I686_BUILD_DIR)/tests/test_win32_colors.exe
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $<
	$(BUILD_DIR)/tests/color-filter-example
	$(XVFB_RUN) -a env ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(BUILD_DIR)/tests/test_x11_colors
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(BUILD_DIR)/tests/test_pangocairo_colors
	$(XVFB_RUN) -a env LIBGL_ALWAYS_SOFTWARE=1 ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(BUILD_DIR)/tests/test_opengl_colors
	$(WINE_RUN) $(abspath $(MINGW_AMD64_BUILD_DIR)/tests/test_win32_colors.exe)
	$(WINE_RUN) $(abspath $(MINGW_I686_BUILD_DIR)/tests/test_win32_colors.exe)

$(BUILD_DIR)/tests/test_color: tests/test_color.c $(UI_HEADERS) $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(CORE_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) $< $(UI_LIBRARY) $(LIBRARY) -Wl,--wrap=calloc -o $@

$(BUILD_DIR)/tests/test_x11_colors: tests/x11/test_x11_colors.c \
		tests/color_renderer_fixture.c tests/color_renderer_fixture.h $(UI_HEADERS) $(X11_LIBRARY) $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(CORE_CPPFLAGS) $(X11_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) $< tests/color_renderer_fixture.c $(X11_LIBRARY) $(UI_LIBRARY) $(LIBRARY) $(X11_LIBS) -o $@

$(BUILD_DIR)/tests/test_pangocairo_colors: tests/pangocairo/test_pangocairo_colors.c \
		tests/color_renderer_fixture.c tests/color_renderer_fixture.h \
		src/pangocairo/renderer.c $(PANGO_CANDIDATE_OBJECT) src/pangocairo/renderer.h \
		src/pangocairo/renderer_private.h src/pangocairo/glyph_fit.h src/ui/glyph_fit.h src/ui/view_private.h src/ui/color_private.h \
		$(UI_HEADERS) $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) -Isrc $(POSIX_CPPFLAGS) \
		$(PANGO_CAIRO_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) \
		tests/pangocairo/test_pangocairo_colors.c tests/color_renderer_fixture.c \
		src/pangocairo/renderer.c $(PANGO_CANDIDATE_OBJECT) $(UI_LIBRARY) $(LIBRARY) \
		$(PANGO_CAIRO_LIBS) $(SANITIZERS) -o $@

.PHONY: test-pangocairo-backgrounds
test: test-pangocairo-backgrounds
test-pangocairo-backgrounds: $(BUILD_DIR)/tests/measure_pangocairo_backgrounds
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $< self-test

.PHONY: test-pangocairo-partial
test: test-pangocairo-partial
test-pangocairo-partial: $(BUILD_DIR)/tests/measure_pangocairo_partial
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $< self-test

$(BUILD_DIR)/tests/measure_pangocairo_partial: PARTIAL_MEASUREMENT_FLAGS = \
	-Wl,--wrap=cairo_fill -Wl,--wrap=boxrig_text_layout_visual_to_logical \
	-Wl,--wrap=boxrig_ui_color_resolve_cell -Wl,--wrap=pango_cairo_show_glyph_item
$(BUILD_DIR)/tests/measure_pangocairo_partial_unwrapped: PARTIAL_MEASUREMENT_FLAGS = \
	-DBOXRIG_BACKGROUND_MEASURE_UNWRAPPED

$(BUILD_DIR)/tests/measure_pangocairo_partial \
$(BUILD_DIR)/tests/measure_pangocairo_partial_unwrapped: tests/pangocairo/measure_partial.c \
		tests/pangocairo/measure_backgrounds.c src/pangocairo/renderer.c \
		src/pangocairo/renderer.h src/pangocairo/renderer_private.h src/pangocairo/glyph_fit.h src/ui/glyph_fit.h \
		src/ui/view_private.h src/ui/color_private.h $(PANGO_CANDIDATE_OBJECT) \
		$(UI_HEADERS) $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) -Isrc $(POSIX_CPPFLAGS) \
		$(PANGO_CAIRO_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) \
		tests/pangocairo/measure_partial.c src/pangocairo/renderer.c \
		$(PANGO_CANDIDATE_OBJECT) $(UI_LIBRARY) $(LIBRARY) \
		$(PANGO_CAIRO_LIBS) $(SANITIZERS) $(PARTIAL_MEASUREMENT_FLAGS) -o $@

$(BUILD_DIR)/tests/measure_pangocairo_backgrounds: BACKGROUND_MEASUREMENT_FLAGS = \
	-Wl,--wrap=cairo_fill -Wl,--wrap=boxrig_text_layout_visual_to_logical
$(BUILD_DIR)/tests/measure_pangocairo_backgrounds_unwrapped: BACKGROUND_MEASUREMENT_FLAGS = \
	-DBOXRIG_BACKGROUND_MEASURE_UNWRAPPED

$(BUILD_DIR)/tests/measure_pangocairo_backgrounds \
$(BUILD_DIR)/tests/measure_pangocairo_backgrounds_unwrapped: tests/pangocairo/measure_backgrounds.c \
		src/pangocairo/renderer.c src/pangocairo/renderer.h \
		src/pangocairo/renderer_private.h src/pangocairo/glyph_fit.h src/ui/glyph_fit.h src/ui/view_private.h src/ui/color_private.h \
		$(PANGO_CANDIDATE_OBJECT) $(UI_HEADERS) $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) -Isrc $(POSIX_CPPFLAGS) \
		$(PANGO_CAIRO_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) \
		tests/pangocairo/measure_backgrounds.c src/pangocairo/renderer.c \
		$(PANGO_CANDIDATE_OBJECT) $(UI_LIBRARY) $(LIBRARY) \
		$(PANGO_CAIRO_LIBS) $(SANITIZERS) $(BACKGROUND_MEASUREMENT_FLAGS) -o $@

$(BUILD_DIR)/tests/test_opengl_colors: tests/test_opengl_colors.c \
		tests/color_renderer_fixture.c tests/color_renderer_fixture.h \
		tests/opengl_host.c tests/opengl_host.h src/opengl/renderer.h \
		$(OPENGL_RENDERER_LIBRARY) $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) -Isrc -Itests $(POSIX_CPPFLAGS) \
		$(OPENGL_RENDERER_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) \
		tests/test_opengl_colors.c tests/color_renderer_fixture.c tests/opengl_host.c \
		$(OPENGL_RENDERER_LIBRARY) $(UI_LIBRARY) $(LIBRARY) \
		$(OPENGL_RENDERER_LIBS) $(SANITIZERS) -o $@

.PHONY: test-win32-alpha-report
test-win32-alpha-report: $(MINGW_AMD64_BUILD_DIR)/tests/test_win32_alpha_report.exe $(MINGW_I686_BUILD_DIR)/tests/test_win32_alpha_report.exe
	$(WINE_RUN) $(abspath $(MINGW_AMD64_BUILD_DIR)/tests/test_win32_alpha_report.exe)
	$(WINE_RUN) $(abspath $(MINGW_I686_BUILD_DIR)/tests/test_win32_alpha_report.exe)

test-p2-win32: $(MINGW_AMD64_BUILD_DIR)/tests/test_win32_alpha_report.exe $(MINGW_I686_BUILD_DIR)/tests/test_win32_alpha_report.exe

.PHONY: test-win32-transparency-report
test: test-win32-transparency-report
test-win32-transparency-report: $(MINGW_AMD64_BUILD_DIR)/tests/test_win32_transparency_report.exe $(MINGW_I686_BUILD_DIR)/tests/test_win32_transparency_report.exe
	$(WINE_RUN) $(abspath $(MINGW_AMD64_BUILD_DIR)/tests/test_win32_transparency_report.exe)
	$(WINE_RUN) $(abspath $(MINGW_I686_BUILD_DIR)/tests/test_win32_transparency_report.exe)

test-p2-win32: $(MINGW_AMD64_BUILD_DIR)/tests/test_win32_transparency_report.exe

$(MINGW_AMD64_BUILD_DIR)/tests/test_win32_transparency_report.exe: tests/win32/test_win32_transparency_report.c tests/p2/win32_transparency_host.c $(MINGW_AMD64_WIN32_LIBRARY) $(MINGW_AMD64_UI_LIBRARY) $(MINGW_AMD64_LIBRARY) | $(MINGW_AMD64_BUILD_DIR)/tests
	$(MINGW_AMD64_CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(MINGW_WIN32_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $< $(MINGW_AMD64_WIN32_LIBRARY) $(MINGW_AMD64_UI_LIBRARY) $(MINGW_AMD64_LIBRARY) -municode $(WIN32_TRANSPARENCY_LIBS) -ldwmapi -limm32 -o $@

$(MINGW_I686_BUILD_DIR)/tests/test_win32_transparency_report.exe: tests/win32/test_win32_transparency_report.c tests/p2/win32_transparency_host.c $(MINGW_I686_WIN32_LIBRARY) $(MINGW_I686_UI_LIBRARY) $(MINGW_I686_LIBRARY) | $(MINGW_I686_BUILD_DIR)/tests
	$(MINGW_I686_CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(MINGW_WIN32_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $< $(MINGW_I686_WIN32_LIBRARY) $(MINGW_I686_UI_LIBRARY) $(MINGW_I686_LIBRARY) -municode $(WIN32_TRANSPARENCY_LIBS) -ldwmapi -limm32 -o $@

$(MINGW_AMD64_BUILD_DIR)/tests/test_win32_alpha_report.exe: tests/win32/test_win32_alpha_report.cpp tests/p2/win32_alpha_feasibility.cpp
	mkdir -p $(@D)
	x86_64-w64-mingw32-g++ -std=c++20 -Wall -Wextra -Werror -O2 -municode -static -fno-exceptions -fno-rtti $< -lopengl32 -lgdi32 -luser32 -lole32 -o $@

$(MINGW_I686_BUILD_DIR)/tests/test_win32_alpha_report.exe: tests/win32/test_win32_alpha_report.cpp tests/p2/win32_alpha_feasibility.cpp
	mkdir -p $(@D)
	i686-w64-mingw32-g++ -std=c++20 -Wall -Wextra -Werror -O2 -municode -static -fno-exceptions -fno-rtti $< -lopengl32 -lgdi32 -luser32 -lole32 -o $@

test: test-default-goal test-core-ui-boundary $(FRONTEND_CONFORMANCE_TEST_BINARY) $(FRONTEND_SAMPLE_SCENARIO_TEST_BINARY) $(FRONTEND_SAMPLE_X11_TEST_BINARY) $(WAYLAND_CONTEXT_TEST_BINARY) $(WAYLAND_WESTON_TEST_BINARY) $(WAYLAND_ROOT_POLICY_TEST_BINARY) $(WAYLAND_SHORTCUT_POLICY_TEST_BINARY) $(WAYLAND_DECORATION_POLICY_TEST_BINARY) $(WAYLAND_NO_LIBDECOR_TEST_BINARY) $(WAYLAND_X11_PIXEL_PROBE_BINARY) $(TEST_BINARY) $(TEXT_WORKLOAD_MEASUREMENT_BINARY) $(PARSER_TEST_BINARY) $(GRAPHICS_TEST_BINARY) $(SCREEN_TEST_BINARY) $(TEXT_LAYOUT_TEST_BINARY) $(SEARCH_TRANSFORM_PROTOTYPE_TEST_BINARY) $(COMPOSITION_TEST_BINARY) $(UTF8_TEST_BINARY) $(IMAGE_COMPOSITOR_TEST_BINARY) $(UI_LIFECYCLE_TEST_BINARY) $(RENDERER_LIFECYCLE_TEST_BINARY) $(OPENGL_RUNTIME_TEST_BINARY) $(UI_TEST_BINARY) $(SELECTION_TEST_BINARY) $(UI_PLATFORM_FIXTURE_TEST_BINARY) $(PANGO_CAIRO_FIXTURE_TEST_BINARY) $(X11_COMPONENT_FIXTURE_TEST_BINARY) $(X11_OPENGL_COMPONENT_FIXTURE_TEST_BINARY) $(X11_OPENGL_COMPONENT_TEST_BINARY) $(GTK4_WIDGET_TEST_BINARY) $(GTK3_WIDGET_TEST_BINARY) $(GTK4_SAMPLE_TEST_BINARY) $(GTK3_SAMPLE_TEST_BINARY) $(GTK3_WAYLAND_SAMPLE_CHILD_BINARY) test-gtk4-package test-gtk3-package test-wayland-package $(SESSION_TEST_BINARY) $(POSIX_SESSION_TEST_BINARY) $(POSIX_EXAMPLE_COMMON_TEST_BINARY) $(EXAMPLE_RUNTIME_INFO_TEST_BINARY) $(EXAMPLE_TEXT_POLICY_TEST_BINARY) $(EXAMPLE_MOUSE_POLICY_TEST_BINARY) $(OSC_RECOGNIZER_REGRESSION_TEST_BINARY) $(INPUT_SHORTCUT_TEST_BINARY) $(NOTIFICATION_POLICY_TEST_BINARY) $(SCROLLBAR_TEST_BINARY) $(EXAMPLE_LAYOUT_TEST_BINARY) $(UI_GUIDE_TEST_BINARY) $(EMITTER_TEST_BINARY) $(TERMINFO_TEST_BINARY) $(PTY_TEST_BINARY) $(PTY_HELPER_BINARY) $(X11_TEST_BINARY) $(X11_HOST_TEST_BINARY) $(X11_NOTIFICATION_TEST_BINARY) $(X11_INPUT_TEST_BINARY) $(X11_EVENT_WAIT_TEST_BINARY) $(X11_TRANSPARENCY_TEST_BINARY) $(X11_COMPOSITOR_TEST_BINARY) $(X11_COMPONENT_TEST_BINARY) $(X11_SAMPLE_TEST_BINARY) $(X11_FAKE_NOTIFICATION_SERVICE) $(EXAMPLE_BINARY) $(GTK4_EXAMPLE_BINARY) $(GTK3_EXAMPLE_BINARY) $(WAYLAND_EXAMPLE_BINARY) $(POSIX_EMITTER_BINARY) $(MINGW_AMD64_CORE_TEST) $(MINGW_I686_CORE_TEST) $(MINGW_AMD64_GRAPHICS_TEST) $(MINGW_I686_GRAPHICS_TEST) $(MINGW_AMD64_UI_PLATFORM_FIXTURE_TEST) $(MINGW_I686_UI_PLATFORM_FIXTURE_TEST) $(MINGW_AMD64_COMPONENT_FIXTURE_TEST) $(MINGW_I686_COMPONENT_FIXTURE_TEST) $(MINGW_AMD64_SESSION_TEST) $(MINGW_I686_SESSION_TEST) $(MINGW_AMD64_SESSION_LIBRARY) $(MINGW_I686_SESSION_LIBRARY) $(MINGW_AMD64_PTY_TEST) $(MINGW_I686_PTY_TEST) $(MINGW_AMD64_PTY_HELPER) $(MINGW_I686_PTY_HELPER) $(MINGW_AMD64_RENDERER_TEST) $(MINGW_I686_RENDERER_TEST) $(MINGW_AMD64_TRANSPARENCY_TEST) $(MINGW_I686_TRANSPARENCY_TEST) $(MINGW_AMD64_EVENT_LOOP_TEST) $(MINGW_I686_EVENT_LOOP_TEST) $(MINGW_AMD64_HOST_TEST) $(MINGW_I686_HOST_TEST) $(MINGW_AMD64_INPUT_TEST) $(MINGW_I686_INPUT_TEST) $(MINGW_AMD64_COMPONENT_TEST) $(MINGW_I686_COMPONENT_TEST) $(MINGW_AMD64_SAMPLE_TEST) $(MINGW_I686_SAMPLE_TEST) $(P2_WIN32_RECONVERSION_HOST) $(MINGW_I686_RECONVERSION_HOST) $(MINGW_AMD64_EXAMPLE) $(MINGW_I686_EXAMPLE) $(MINGW_AMD64_EMITTER) $(MINGW_I686_EMITTER) $(MINGW_AMD64_SHUTDOWN_PROBE_EXAMPLE) $(MINGW_I686_SHUTDOWN_PROBE_EXAMPLE) $(MINGW_AMD64_IME_MOVEMENT_PROBE_EXAMPLE) $(MINGW_I686_IME_MOVEMENT_PROBE_EXAMPLE) $(MINGW_AMD64_NOTIFICATION_EXAMPLE) $(MINGW_I686_NOTIFICATION_EXAMPLE) $(MINGW_AMD64_LAUNCHER) $(MINGW_I686_LAUNCHER) $(MINGW_AMD64_TEST_LAUNCHER) $(MINGW_I686_TEST_LAUNCHER)

test: test-package-licenses test-wayland-candidates-startup test-wayland-input test-gtk-ime-x11 test-opengl-candidates test-gtk4-candidates test-gtk3-candidates test-ibus-candidates test-gtk-ime-startup test-gtk-ime-routing test-gtk-ibus-auto test-gtk-ibus-direct test-xim-editable test-ime-clause test-wayland-input test-terminal-codec test-terminal-key-mapping test-terminal-newline test-decoration-backends test-selection-style test-example-link-required test-example-copy test-wheel-policy test-animation-integration test-cursor-style test-text-blink-core test-native-blink test-colors test-win32-arguments $(MINGW_AMD64_BUILD_DIR)/tests/test_win32_opengl_component.exe $(MINGW_I686_BUILD_DIR)/tests/test_win32_opengl_component.exe $(MINGW_AMD64_BUILD_DIR)/tests/test_win32_opengl_component_fixture.exe $(MINGW_I686_BUILD_DIR)/tests/test_win32_opengl_component_fixture.exe $(OPENGL_RENDERER_FIXTURE_TEST_BINARY) \
	$(MINGW_AMD64_WGL_PROBE) $(MINGW_I686_WGL_PROBE) \
	$(WAYLAND_OPENGL_WESTON_TEST_BINARY) test-opengl-packages
	$(WINE_RUN) $(abspath $(MINGW_AMD64_OPACITY_NATIVE_TEST))
	$(WINE_RUN) $(abspath $(MINGW_I686_OPACITY_NATIVE_TEST))
	$(WINE_RUN) $(abspath $(MINGW_AMD64_HOST_TRANSPARENCY_TEST))
	$(WINE_RUN) $(abspath $(MINGW_I686_HOST_TRANSPARENCY_TEST))
	@for architecture in amd64 i686; do \
		for mode in ordinary static; do \
			$(WINE_RUN) "$(abspath $(BUILD_DIR))/tests/win32-sdk-$$architecture/core/consumer-$$mode.exe" || exit $$?; \
			$(WINE_RUN) "$(abspath $(BUILD_DIR))/tests/win32-sdk-$$architecture/render-d3d11/consumer-$$mode.exe" || exit $$?; \
			$(WINE_RUN) "$(abspath $(BUILD_DIR))/tests/win32-sdk-$$architecture/win32-d3d11/consumer-$$mode.exe" || exit $$?; \
			$(WINE_RUN) "$(abspath $(BUILD_DIR))/tests/win32-sdk-$$architecture/win32-d3d11/example-$$mode.exe" --check --fallback --color-filter || exit $$?; \
			$(WINE_RUN) "$(abspath $(BUILD_DIR))/tests/win32-sdk-$$architecture/software/consumer-$$mode.exe" || exit $$?; \
		done; \
		$(WINE_RUN) "$(abspath $(BUILD_DIR))/tests/win32-sdk-$$architecture/software/win32-consumer.exe" "Z:$(TEST_FONT_FILE)" || exit $$?; \
		$(WINE_RUN) "$(abspath $(BUILD_DIR))/tests/win32-sdk-$$architecture/software/component-software.exe" --check --color-filter || exit $$?; \
		$(WINE_RUN) "$(abspath $(BUILD_DIR))/tests/win32-sdk-$$architecture/software/host-opacity.exe" --check || exit $$?; \
		$(WINE_RUN) "$(abspath $(BUILD_DIR))/tests/win32-sdk-$$architecture/software/host-opacity.exe" --check --decline || exit $$?; \
		$(WINE_RUN) "$(abspath $(BUILD_DIR))/tests/win32-sdk-$$architecture/software/host-opacity.exe" --check --apply || exit $$?; \
		$(WINE_RUN) "$(abspath $(BUILD_DIR))/tests/win32-sdk-$$architecture/software/host-opacity.exe" --check --apply --opaque || exit $$?; \
		$(WINE_RUN) "$(abspath $(BUILD_DIR))/tests/win32-sdk-$$architecture/software/host-opacity.exe" --check --helper  || exit $$?; \
		$(WINE_RUN) "$(abspath $(BUILD_DIR))/tests/win32-sdk-$$architecture/software/host-opacity.exe" --check --helper --decline || exit $$?; \
		$(WINE_RUN) "$(abspath $(BUILD_DIR))/tests/win32-sdk-$$architecture/software/host-opacity.exe" --check --helper --apply || exit $$?; \
		$(WINE_RUN) "$(abspath $(BUILD_DIR))/tests/win32-sdk-$$architecture/software/host-opacity.exe" --check --helper --apply --two || exit $$?; \
		$(WINE_RUN) "$(abspath $(BUILD_DIR))/tests/win32-sdk-$$architecture/software/host-opacity.exe" --check --helper --apply --opaque || exit $$?; \
		$(WINE_RUN) "$(abspath $(BUILD_DIR))/tests/win32-sdk-$$architecture/software/host-opacity.exe" --check --helper --apply --background || exit $$?; \
		$(WINE_RUN) "$(abspath $(BUILD_DIR))/tests/win32-sdk-$$architecture/software/host-opacity.exe" --check --helper --apply --fallback || exit $$?; \
		for mode in static shared; do \
			$(WINE_RUN) "$(abspath $(BUILD_DIR))/tests/win32-sdk-$$architecture/ogl/renderer-$$mode.exe" --check || exit $$?; \
			$(WINE_RUN) "$(abspath $(BUILD_DIR))/tests/win32-sdk-$$architecture/ogl/component-$$mode.exe" --check --require-opengl --color-filter || exit $$?; \
		done; \
	done
	$(WINE_RUN) $(abspath $(MINGW_AMD64_BUILD_DIR)/tests/test_win32_opengl_component.exe)
	$(WINE_RUN) $(abspath $(MINGW_I686_BUILD_DIR)/tests/test_win32_opengl_component.exe)
	$(WINE_RUN) $(abspath $(MINGW_AMD64_BUILD_DIR)/tests/test_win32_opengl_component_fixture.exe) "Z:$(TEST_FONT_FILE)"
	$(WINE_RUN) $(abspath $(MINGW_I686_BUILD_DIR)/tests/test_win32_opengl_component_fixture.exe) "Z:$(TEST_FONT_FILE)"
	$(WINE_RUN) $(abspath $(MINGW_AMD64_RENDERER_TEST)) "Z:$(TEST_FONT_FILE)"
	$(WINE_RUN) $(abspath $(MINGW_AMD64_BUILD_DIR)/tests/test_basic_ui.exe) "Z:$(TEST_FONT_FILE)"
	$(WINE_RUN) $(abspath $(MINGW_I686_RENDERER_TEST)) "Z:$(TEST_FONT_FILE)"
	$(WINE_RUN) $(abspath $(MINGW_I686_BUILD_DIR)/tests/test_basic_ui.exe) "Z:$(TEST_FONT_FILE)"
	$(WINE_RUN) $(abspath $(MINGW_AMD64_BUILD_DIR)/tests/test_legacy_host.exe)
	$(WINE_RUN) $(abspath $(MINGW_I686_BUILD_DIR)/tests/test_legacy_host.exe)
	$(XVFB_RUN) -a env LIBGL_ALWAYS_SOFTWARE=1 ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(OPENGL_RENDERER_FIXTURE_TEST_BINARY)
	$(XVFB_RUN) -a env LIBGL_ALWAYS_SOFTWARE=1 BOXRIG_TEST_GL_API=gles ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(OPENGL_RENDERER_FIXTURE_TEST_BINARY)
	$(WINE_RUN) $(abspath $(BUILD_DIR)/tests/wgl-package-amd64/test_wgl_package-static.exe) static
	$(WINE_RUN) $(abspath $(BUILD_DIR)/tests/wgl-package-amd64/test_wgl_package-shared.exe) shared
	$(WINE_RUN) $(abspath $(BUILD_DIR)/tests/wgl-package-i686/test_wgl_package-static.exe) static
	$(WINE_RUN) $(abspath $(BUILD_DIR)/tests/wgl-package-i686/test_wgl_package-shared.exe) shared
	$(WINE_RUN) $(abspath $(MINGW_AMD64_WGL_PROBE)) \
		--expect-environment wine \
		--report "Z:$(abspath $(MINGW_AMD64_BUILD_DIR)/tests/p2/wgl-probe-wine.json)"
	$(WINE_RUN) $(abspath $(MINGW_I686_WGL_PROBE)) \
		--expect-environment wine \
		--report "Z:$(abspath $(MINGW_I686_BUILD_DIR)/tests/p2/wgl-probe-wine.json)"
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(TEST_BINARY)
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(FRONTEND_CONFORMANCE_TEST_BINARY) $(FRONTEND_PUBLIC_HEADERS)
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(EXAMPLE_LAYOUT_TEST_BINARY) $(EXAMPLE_BINARY) $(GTK4_EXAMPLE_BINARY) $(GTK3_EXAMPLE_BINARY) $(WAYLAND_EXAMPLE_BINARY) $(MINGW_AMD64_EXAMPLE) $(MINGW_AMD64_LAUNCHER) $(MINGW_I686_EXAMPLE) $(MINGW_I686_LAUNCHER) $(POSIX_EMITTER_BINARY) $(MINGW_AMD64_EMITTER) $(MINGW_I686_EMITTER)
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(FRONTEND_SAMPLE_PROBE_TEST_BINARY)
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(FRONTEND_SAMPLE_SCENARIO_TEST_BINARY)
	$(XVFB_RUN) -a env LIBGL_ALWAYS_SOFTWARE=1 BOXRIG_TEST_SAMPLE_GRAPHICS_API='OpenGL ES' ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(FRONTEND_SAMPLE_X11_TEST_BINARY) X11 $(EXAMPLE_BINARY) $(PTY_HELPER_BINARY) --color-demo
	$(XVFB_RUN) -a env LIBGL_ALWAYS_SOFTWARE=1 BOXRIG_TEST_SAMPLE_RENDERER=--renderer=opengl BOXRIG_TEST_SAMPLE_GRAPHICS_API='OpenGL ES' ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(FRONTEND_SAMPLE_X11_TEST_BINARY) X11 $(EXAMPLE_BINARY) $(PTY_HELPER_BINARY) --color-demo
	$(XVFB_RUN) -a env LIBGL_ALWAYS_SOFTWARE=1 MESA_GLES_VERSION_OVERRIDE=2.0 BOXRIG_TEST_SAMPLE_GRAPHICS_API=OpenGL ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(FRONTEND_SAMPLE_X11_TEST_BINARY) X11 $(EXAMPLE_BINARY) $(PTY_HELPER_BINARY) --color-demo
	$(XVFB_RUN) -a env LIBGL_ALWAYS_SOFTWARE=1 MESA_GLES_VERSION_OVERRIDE=2.0 MESA_GL_VERSION_OVERRIDE=2.1 BOXRIG_TEST_SAMPLE_GRAPHICS_API=Cairo ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(FRONTEND_SAMPLE_X11_TEST_BINARY) X11 $(EXAMPLE_BINARY) $(PTY_HELPER_BINARY) --color-demo
	$(XVFB_RUN) -a env ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(FRONTEND_SAMPLE_X11_TEST_BINARY) GTK3 $(GTK3_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) --color-demo
	$(XVFB_RUN) -a env $(GTK4_TEST_IM_ENV) ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(FRONTEND_SAMPLE_X11_TEST_BINARY) GTK4 $(GTK4_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) --color-demo
	$(XVFB_RUN) -a env ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(FRONTEND_SAMPLE_X11_TEST_BINARY) X11 $(EXAMPLE_BINARY) $(PTY_HELPER_BINARY)
	$(XVFB_RUN) -a env ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(FRONTEND_SAMPLE_X11_TEST_BINARY) GTK3 $(GTK3_EXAMPLE_BINARY) $(PTY_HELPER_BINARY)
	$(XVFB_RUN) -a env $(GTK4_TEST_IM_ENV) ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(FRONTEND_SAMPLE_X11_TEST_BINARY) GTK4 $(GTK4_EXAMPLE_BINARY) $(PTY_HELPER_BINARY)
	$(XVFB_RUN) -a env ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(FRONTEND_SAMPLE_X11_TEST_BINARY) GTK3 $(GTK3_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) --continuous-output
	$(XVFB_RUN) -a env $(GTK4_TEST_IM_ENV) ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(FRONTEND_SAMPLE_X11_TEST_BINARY) GTK4 $(GTK4_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) --continuous-output
	$(XVFB_RUN) -a env ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(FRONTEND_SAMPLE_X11_TEST_BINARY) GTK3 $(GTK3_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) --sync-recovery
	$(XVFB_RUN) -a env $(GTK4_TEST_IM_ENV) ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(FRONTEND_SAMPLE_X11_TEST_BINARY) GTK4 $(GTK4_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) --sync-recovery
	$(XVFB_RUN) -a env BOXRIG_TEST_SAMPLE_COLORS=1 $(WAYLAND_FRONTEND_SAMPLE_TEST_RUNNER) \
		GTK3 "$(WESTON)" "$(GTK3_EXAMPLE_BINARY)" \
		"$(PTY_HELPER_BINARY)" "$(WAYLAND_X11_PIXEL_PROBE_BINARY)"
	$(XVFB_RUN) -a env $(GTK4_TEST_IM_ENV) BOXRIG_TEST_SAMPLE_COLORS=1 \
		$(WAYLAND_FRONTEND_SAMPLE_TEST_RUNNER) \
		GTK4 "$(WESTON)" "$(GTK4_EXAMPLE_BINARY)" \
		"$(PTY_HELPER_BINARY)" "$(WAYLAND_X11_PIXEL_PROBE_BINARY)"
	@if [ "$(WAYLAND_LIBDECOR_ENABLED)" = 1 ]; then \
		$(XVFB_RUN) -a env BOXRIG_TEST_SAMPLE_COLORS=1 BOXRIG_TEST_SAMPLE_RENDERER=opengl \
			$(WAYLAND_FRONTEND_SAMPLE_TEST_RUNNER) \
			native-Wayland "$(WESTON)" "$(WAYLAND_EXAMPLE_BINARY)" \
			"$(PTY_HELPER_BINARY)" "$(WAYLAND_X11_PIXEL_PROBE_BINARY)" && \
		$(XVFB_RUN) -a env BOXRIG_TEST_SAMPLE_COLORS=1 BOXRIG_TEST_SAMPLE_RENDERER=wayland \
			$(WAYLAND_FRONTEND_SAMPLE_TEST_RUNNER) \
			native-Wayland "$(WESTON)" "$(WAYLAND_EXAMPLE_BINARY)" \
			"$(PTY_HELPER_BINARY)" "$(WAYLAND_X11_PIXEL_PROBE_BINARY)"; \
	else \
		echo "SKIP shared native Wayland sample scenario (disabled)"; \
	fi
	$(XVFB_RUN) -a env BOXRIG_TEST_SAMPLE_PASTE_PRESSURE=1 BOXRIG_TEST_SAMPLE_OVERLAYS=1 BOXRIG_TEST_SAMPLE_GEOMETRY=1 BOXRIG_TEST_SAMPLE_PENDING_CLIPBOARD=1 ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(FRONTEND_SAMPLE_X11_TEST_BINARY) X11 $(FRONTEND_SAMPLE_X11_PROBE_BINARY) $(PTY_HELPER_BINARY)
	$(XVFB_RUN) -a env BOXRIG_TEST_SAMPLE_PASTE_PRESSURE=1 BOXRIG_TEST_SAMPLE_OVERLAYS=1 BOXRIG_TEST_SAMPLE_GEOMETRY=1 BOXRIG_TEST_SAMPLE_PENDING_CLIPBOARD=1 ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(FRONTEND_SAMPLE_X11_TEST_BINARY) GTK3 $(FRONTEND_SAMPLE_GTK3_PROBE_BINARY) $(PTY_HELPER_BINARY)
	$(XVFB_RUN) -a env $(GTK4_TEST_IM_ENV) BOXRIG_TEST_SAMPLE_PASTE_PRESSURE=1 BOXRIG_TEST_SAMPLE_OVERLAYS=1 BOXRIG_TEST_SAMPLE_GEOMETRY=1 BOXRIG_TEST_SAMPLE_PENDING_CLIPBOARD=1 ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(FRONTEND_SAMPLE_X11_TEST_BINARY) GTK4 $(FRONTEND_SAMPLE_GTK4_PROBE_BINARY) $(PTY_HELPER_BINARY)
	$(XVFB_RUN) -a env BOXRIG_TEST_SAMPLE_PASTE_PRESSURE=1 BOXRIG_TEST_SAMPLE_OVERLAYS=1 BOXRIG_TEST_SAMPLE_GEOMETRY=1 \
		BOXRIG_TEST_SAMPLE_PENDING_CLIPBOARD=1 \
		BOXRIG_TEST_CLIPBOARD_PEER="$(abspath $(GTK3_CLIPBOARD_PEER_BINARY))" \
		$(WAYLAND_FRONTEND_SAMPLE_TEST_RUNNER) GTK3 "$(WESTON)" \
		"$(FRONTEND_SAMPLE_GTK3_PROBE_BINARY)" \
		"$(PTY_HELPER_BINARY)" "$(WAYLAND_X11_PIXEL_PROBE_BINARY)"
	$(XVFB_RUN) -a env $(GTK4_TEST_IM_ENV) BOXRIG_TEST_SAMPLE_PASTE_PRESSURE=1 BOXRIG_TEST_SAMPLE_OVERLAYS=1 BOXRIG_TEST_SAMPLE_GEOMETRY=1 \
		BOXRIG_TEST_SAMPLE_PENDING_CLIPBOARD=1 \
		BOXRIG_TEST_CLIPBOARD_PEER="$(abspath $(GTK3_CLIPBOARD_PEER_BINARY))" \
		$(WAYLAND_FRONTEND_SAMPLE_TEST_RUNNER) GTK4 "$(WESTON)" \
		"$(FRONTEND_SAMPLE_GTK4_PROBE_BINARY)" \
		"$(PTY_HELPER_BINARY)" "$(WAYLAND_X11_PIXEL_PROBE_BINARY)"
	@if [ "$(WAYLAND_LIBDECOR_ENABLED)" = 1 ]; then \
		$(XVFB_RUN) -a env BOXRIG_TEST_SAMPLE_PASTE_PRESSURE=1 BOXRIG_TEST_SAMPLE_OVERLAYS=1 BOXRIG_TEST_SAMPLE_GEOMETRY=1 \
			BOXRIG_TEST_SAMPLE_PENDING_CLIPBOARD=1 \
			BOXRIG_TEST_CLIPBOARD_PEER="$(abspath $(GTK3_CLIPBOARD_PEER_BINARY))" \
			$(WAYLAND_FRONTEND_SAMPLE_TEST_RUNNER) native-Wayland "$(WESTON)" \
			"$(FRONTEND_SAMPLE_WAYLAND_PROBE_BINARY)" \
			"$(PTY_HELPER_BINARY)" "$(WAYLAND_X11_PIXEL_PROBE_BINARY)"; \
	else \
		echo "SKIP shared native Wayland sample paste pressure (disabled)"; \
	fi
	@if [ "$(WAYLAND_LIBDECOR_ENABLED)" = 1 ]; then \
		$(XVFB_RUN) -a $(WAYLAND_INTERACTIVE_SAMPLE_TEST_RUNNER) \
			"$(WESTON)" "$(WAYLAND_EXAMPLE_BINARY)" \
			"$(GTK3_WAYLAND_SAMPLE_CHILD_BINARY)" \
			"$(WAYLAND_X11_PIXEL_PROBE_BINARY)"; \
	else \
		echo "SKIP native Wayland libdecor interaction (disabled)"; \
	fi
	$(WAYLAND_WESTON_TEST_RUNNER) "$(WESTON)" "$(WAYLAND_WESTON_TEST_BINARY)"
	$(WAYLAND_WESTON_TEST_RUNNER) "$(WESTON)" \
		"$(WAYLAND_OPENGL_WESTON_TEST_BINARY)" gl
	$(WAYLAND_SAMPLE_TEST_RUNNER) "$(WESTON)" \
		"$(WAYLAND_EXAMPLE_BINARY)" "$(GTK3_WAYLAND_SAMPLE_CHILD_BINARY)" \
		auto $(WAYLAND_SAMPLE_EXPECTED_VIEWS)
	$(WAYLAND_SAMPLE_TEST_RUNNER) "$(WESTON)" \
		"$(WAYLAND_NO_LIBDECOR_TEST_BINARY)" \
		"$(GTK3_WAYLAND_SAMPLE_CHILD_BINARY)" auto undecorated
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
		$(WAYLAND_ROOT_POLICY_TEST_BINARY)
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
		$(WAYLAND_SHORTCUT_POLICY_TEST_BINARY)
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
		$(WAYLAND_DECORATION_POLICY_TEST_BINARY)
	$(WAYLAND_SAMPLE_BUILD_TEST_RUNNER) $(WAYLAND_LIBDECOR_ENABLED) \
		"$(WAYLAND_EXAMPLE_BINARY)" "$(WAYLAND_NO_LIBDECOR_TEST_BINARY)"
	$(TEXT_WORKLOAD_MEASUREMENT_BINARY) self-test
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(PARSER_TEST_BINARY)
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(GRAPHICS_TEST_BINARY)
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(SCREEN_TEST_BINARY)
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(TEXT_LAYOUT_TEST_BINARY)
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(SEARCH_TRANSFORM_PROTOTYPE_TEST_BINARY)
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(COMPOSITION_TEST_BINARY)
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(UTF8_TEST_BINARY)
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(IMAGE_COMPOSITOR_TEST_BINARY)
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(UI_LIFECYCLE_TEST_BINARY)
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(RENDERER_LIFECYCLE_TEST_BINARY)
	$(XVFB_RUN) -a env LIBGL_ALWAYS_SOFTWARE=1 ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(OPENGL_RUNTIME_TEST_BINARY) egl
	$(XVFB_RUN) -a env LIBGL_ALWAYS_SOFTWARE=1 ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(OPENGL_RUNTIME_TEST_BINARY) egl-gles
	$(XVFB_RUN) -a env LIBGL_ALWAYS_SOFTWARE=1 MESA_GLES_VERSION_OVERRIDE=3.0 MESA_EXTENSION_OVERRIDE=-GL_EXT_sRGB_write_control ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(OPENGL_RUNTIME_TEST_BINARY) egl-gles
	$(XVFB_RUN) -a env LIBGL_ALWAYS_SOFTWARE=1 ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(OPENGL_RUNTIME_TEST_BINARY) glx
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(UI_TEST_BINARY)
	node tools/unicode/generate-word-break.mjs --check
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(SELECTION_TEST_BINARY) resources/unicode/17.0.0/WordBreakTest.txt
	$(XVFB_RUN) -a env ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(UI_PLATFORM_FIXTURE_TEST_BINARY)
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(PANGO_CAIRO_FIXTURE_TEST_BINARY)
	$(XVFB_RUN) -a env ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(X11_COMPONENT_FIXTURE_TEST_BINARY) "$(TEST_FONT_FILE)"
	$(XVFB_RUN) -a env LIBGL_ALWAYS_SOFTWARE=1 ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(X11_OPENGL_COMPONENT_FIXTURE_TEST_BINARY) "$(TEST_FONT_FILE)"
	$(XVFB_RUN) -a env LIBGL_ALWAYS_SOFTWARE=1 ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(X11_OPENGL_COMPONENT_TEST_BINARY)
	$(XVFB_RUN) -a env LIBGL_ALWAYS_SOFTWARE=1 ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(X11_OPENGL_COMPONENT_TEST_BINARY) desktop-context
	$(XVFB_RUN) -a env LIBGL_ALWAYS_SOFTWARE=1 ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(X11_OPENGL_COMPONENT_TEST_BINARY) desktop-renderer
	$(XVFB_RUN) -a env LIBGL_ALWAYS_SOFTWARE=1 ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(X11_OPENGL_COMPONENT_TEST_BINARY) desktop-config
	$(XVFB_RUN) -a env LIBGL_ALWAYS_SOFTWARE=1 ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(X11_OPENGL_COMPONENT_TEST_BINARY) gles-config
	$(XVFB_RUN) -a env LIBGL_ALWAYS_SOFTWARE=1 MESA_GLES_VERSION_OVERRIDE=3.0 MESA_EXTENSION_OVERRIDE=-GL_EXT_sRGB_write_control ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(X11_OPENGL_COMPONENT_TEST_BINARY)
	$(XVFB_RUN) -a env $(GTK4_TEST_IM_ENV) GDK_BACKEND=x11 GSK_RENDERER=cairo NO_AT_BRIDGE=1 ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(GTK4_SAMPLE_TEST_BINARY) $(GTK4_EXAMPLE_BINARY) $(PTY_HELPER_BINARY)
	$(XVFB_RUN) -a env GDK_BACKEND=x11 NO_AT_BRIDGE=1 ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(GTK3_SAMPLE_TEST_BINARY) $(GTK3_EXAMPLE_BINARY) $(PTY_HELPER_BINARY)
	$(MAKE) test-gtk3-gestament
	$(GTK3_WAYLAND_SAMPLE_TEST_RUNNER) "$(WESTON)" \
		"$(GTK3_EXAMPLE_BINARY)" "$(GTK3_WAYLAND_SAMPLE_CHILD_BINARY)"
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(SESSION_TEST_BINARY)
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(POSIX_SESSION_TEST_BINARY) $(PTY_HELPER_BINARY)
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(EXAMPLE_RUNTIME_INFO_TEST_BINARY)
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(EXAMPLE_TEXT_POLICY_TEST_BINARY)
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(EXAMPLE_MOUSE_POLICY_TEST_BINARY)
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(OSC_RECOGNIZER_REGRESSION_TEST_BINARY)
	@if nm -u $(POSIX_EXAMPLE_COMMON_TEST_BINARY) | rg -q 'X(Open|Close|Create|Destroy)|gtk_|gdk_'; then \
		echo "FAIL portable POSIX example common code has native symbols"; \
		exit 1; \
	fi
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(INPUT_SHORTCUT_TEST_BINARY)
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(NOTIFICATION_POLICY_TEST_BINARY)
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(SCROLLBAR_TEST_BINARY)
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(EMITTER_TEST_BINARY)
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(TERMINFO_TEST_BINARY)
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(PTY_TEST_BINARY) $(PTY_HELPER_BINARY) $(POSIX_EMITTER_BINARY)
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(X11_TEST_BINARY) "$(TEST_FONT_FILE)"
	$(XVFB_RUN) -a env ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(X11_HOST_TEST_BINARY)
	dbus-run-session -- $(XVFB_RUN) -a env ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(X11_NOTIFICATION_TEST_BINARY)
	$(XVFB_RUN) -a env ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(X11_INPUT_TEST_BINARY)
	$(XVFB_RUN) -a env ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(X11_EVENT_WAIT_TEST_BINARY)
	$(XVFB_RUN) -a env ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(X11_TRANSPARENCY_TEST_BINARY)
	$(XVFB_RUN) -a env ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(X11_COMPOSITOR_TEST_BINARY)
	$(XVFB_RUN) -a env ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(X11_COMPONENT_TEST_BINARY) "$(TEST_FONT_FILE)"
	dbus-run-session -- env ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(X11_SAMPLE_TEST_BINARY) $(EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(X11_FAKE_NOTIFICATION_SERVICE) $(POSIX_EMITTER_BINARY)
	$(WINE_RUN) $(abspath $(MINGW_AMD64_BUILD_DIR)/tests/test_custom_ui.exe)
	$(WINE_RUN) $(abspath $(MINGW_AMD64_CORE_TEST))
	$(WINE_RUN) $(abspath $(MINGW_I686_BUILD_DIR)/tests/test_custom_ui.exe)
	$(WINE_RUN) $(abspath $(MINGW_I686_CORE_TEST))
	$(WINE_RUN) $(abspath $(MINGW_AMD64_GRAPHICS_TEST))
	$(WINE_RUN) $(abspath $(MINGW_I686_GRAPHICS_TEST))
	$(WINE_RUN) $(abspath $(MINGW_AMD64_UI_PLATFORM_FIXTURE_TEST))
	$(WINE_RUN) $(abspath $(MINGW_I686_UI_PLATFORM_FIXTURE_TEST))
	$(WINE_RUN) $(abspath $(MINGW_AMD64_COMPONENT_FIXTURE_TEST)) "Z:$(TEST_FONT_FILE)"
	$(WINE_RUN) $(abspath $(MINGW_I686_COMPONENT_FIXTURE_TEST)) "Z:$(TEST_FONT_FILE)"
	$(WINE_RUN) $(abspath $(MINGW_AMD64_SESSION_TEST)) $(abspath $(MINGW_AMD64_PTY_HELPER))
	$(WINE_RUN) $(abspath $(MINGW_I686_SESSION_TEST)) $(abspath $(MINGW_I686_PTY_HELPER))
	$(WINE_RUN) $(abspath $(MINGW_AMD64_PTY_TEST)) $(abspath $(MINGW_AMD64_PTY_HELPER)) $(abspath $(MINGW_AMD64_LAUNCHER)) $(abspath $(MINGW_AMD64_EMITTER))
	$(WINE_RUN) $(abspath $(MINGW_I686_PTY_TEST)) $(abspath $(MINGW_I686_PTY_HELPER)) $(abspath $(MINGW_I686_LAUNCHER)) $(abspath $(MINGW_I686_EMITTER))
	$(WINE_RUN) $(abspath $(MINGW_AMD64_TRANSPARENCY_TEST))
	$(WINE_RUN) $(abspath $(MINGW_I686_TRANSPARENCY_TEST))
	$(WINE_RUN) $(abspath $(MINGW_AMD64_EVENT_LOOP_TEST))
	$(WINE_RUN) $(abspath $(MINGW_I686_EVENT_LOOP_TEST))
	$(WINE_RUN) $(abspath $(MINGW_AMD64_HOST_TEST))
	$(WINE_RUN) $(abspath $(MINGW_I686_HOST_TEST))
	$(WINE_RUN) $(abspath $(MINGW_AMD64_INPUT_TEST))
	$(WINE_RUN) $(abspath $(MINGW_I686_INPUT_TEST))
	$(WINE_RUN) $(abspath $(P2_WIN32_RECONVERSION_HOST)) --self-test "Z:$(abspath $(MINGW_AMD64_BUILD_DIR)/tests/p2/reconversion-report.json)"
	node $(WIN32_RECONVERSION_REPORT_TEST) $(MINGW_AMD64_BUILD_DIR)/tests/p2/reconversion-report.json
	$(WINE_RUN) $(abspath $(MINGW_I686_RECONVERSION_HOST)) --self-test "Z:$(abspath $(MINGW_I686_BUILD_DIR)/tests/p2/reconversion-report.json)"
	node $(WIN32_RECONVERSION_REPORT_TEST) $(MINGW_I686_BUILD_DIR)/tests/p2/reconversion-report.json
	$(WINE_RUN) $(abspath $(MINGW_AMD64_COMPONENT_TEST)) "Z:$(TEST_FONT_FILE)"
	$(WINE_RUN) $(abspath $(MINGW_I686_COMPONENT_TEST)) "Z:$(TEST_FONT_FILE)"
	$(WINE_RUN) $(abspath $(MINGW_AMD64_SAMPLE_TEST)) --pointer $(abspath $(MINGW_AMD64_EXAMPLE)) $(abspath $(MINGW_AMD64_PTY_HELPER))
	$(WINE_RUN) $(abspath $(MINGW_I686_SAMPLE_TEST)) --pointer $(abspath $(MINGW_I686_EXAMPLE)) $(abspath $(MINGW_I686_PTY_HELPER))
	env BOXRIG_TEST_SAMPLE_COLORS=1 $(WINE_RUN) $(abspath $(MINGW_AMD64_SAMPLE_TEST)) --shared-scenario $(abspath $(MINGW_AMD64_EXAMPLE)) $(abspath $(MINGW_AMD64_PTY_HELPER))
	env BOXRIG_TEST_SAMPLE_COLORS=1 $(WINE_RUN) $(abspath $(MINGW_I686_SAMPLE_TEST)) --shared-scenario $(abspath $(MINGW_I686_EXAMPLE)) $(abspath $(MINGW_I686_PTY_HELPER))
	env BOXRIG_TEST_SAMPLE_PASTE_PRESSURE=1 BOXRIG_TEST_SAMPLE_OVERLAYS=1 BOXRIG_TEST_SAMPLE_GEOMETRY=1 $(WINE_RUN) $(abspath $(MINGW_AMD64_SAMPLE_TEST)) --shared-scenario $(abspath $(MINGW_AMD64_FRONTEND_SAMPLE_PROBE)) $(abspath $(MINGW_AMD64_PTY_HELPER))
	env BOXRIG_TEST_SAMPLE_PASTE_PRESSURE=1 BOXRIG_TEST_SAMPLE_OVERLAYS=1 BOXRIG_TEST_SAMPLE_GEOMETRY=1 $(WINE_RUN) $(abspath $(MINGW_I686_SAMPLE_TEST)) --shared-scenario $(abspath $(MINGW_I686_FRONTEND_SAMPLE_PROBE)) $(abspath $(MINGW_I686_PTY_HELPER))
	$(WINE_RUN) $(abspath $(MINGW_AMD64_SAMPLE_TEST)) --ime-movement $(abspath $(MINGW_AMD64_IME_MOVEMENT_PROBE_EXAMPLE)) $(abspath $(MINGW_AMD64_PTY_HELPER))
	$(WINE_RUN) $(abspath $(MINGW_I686_SAMPLE_TEST)) --ime-movement $(abspath $(MINGW_I686_IME_MOVEMENT_PROBE_EXAMPLE)) $(abspath $(MINGW_I686_PTY_HELPER))
	$(WINE_RUN) $(abspath $(MINGW_AMD64_SAMPLE_TEST)) $(abspath $(MINGW_AMD64_EXAMPLE)) $(abspath $(MINGW_AMD64_NOTIFICATION_EXAMPLE)) $(abspath $(MINGW_AMD64_PTY_HELPER)) $(abspath $(MINGW_AMD64_EMITTER))
	$(WINE_RUN) $(abspath $(MINGW_I686_SAMPLE_TEST)) $(abspath $(MINGW_I686_EXAMPLE)) $(abspath $(MINGW_I686_NOTIFICATION_EXAMPLE)) $(abspath $(MINGW_I686_PTY_HELPER)) $(abspath $(MINGW_I686_EMITTER))
	$(WINE_RUN) $(abspath $(MINGW_AMD64_SAMPLE_TEST)) $(abspath $(MINGW_AMD64_SHUTDOWN_PROBE_EXAMPLE)) $(abspath $(MINGW_AMD64_NOTIFICATION_EXAMPLE)) $(abspath $(MINGW_AMD64_PTY_HELPER)) -
	$(WINE_RUN) $(abspath $(MINGW_I686_SAMPLE_TEST)) $(abspath $(MINGW_I686_SHUTDOWN_PROBE_EXAMPLE)) $(abspath $(MINGW_I686_NOTIFICATION_EXAMPLE)) $(abspath $(MINGW_I686_PTY_HELPER)) -

test-core-ui-boundary: $(CORE_UI_BOUNDARY_TEST_SOURCE) \
		$(DEPENDENCY_BOUNDARY_TEST_SCRIPT) $(LIBRARY) $(UI_LIBRARY) \
		$(X11_LIBRARY) $(GTK4_LIBRARY) $(GTK3_LIBRARY) $(WAYLAND_LIBRARY) \
		$(MINGW_AMD64_LIBRARY) $(MINGW_AMD64_UI_LIBRARY) \
		$(MINGW_AMD64_WIN32_LIBRARY) $(MINGW_I686_LIBRARY) \
		$(MINGW_I686_UI_LIBRARY) $(MINGW_I686_WIN32_LIBRARY) \
		| $(BUILD_DIR)/tests
	$(RM) -r $(CORE_UI_BOUNDARY_TEST_ROOT)
	$(INSTALL) -d $(CORE_UI_BOUNDARY_TEST_ROOT)/usr/lib \
		$(CORE_UI_BOUNDARY_TEST_ROOT)/usr/include/boxrig/ui
	$(INSTALL) -m 644 $(LIBRARY) $(UI_LIBRARY) \
		$(CORE_UI_BOUNDARY_TEST_ROOT)/usr/lib
	$(INSTALL) -m 644 $(CORE_PUBLIC_HEADERS) include/boxrig/ui.h \
		$(CORE_UI_BOUNDARY_TEST_ROOT)/usr/include/boxrig
	$(INSTALL) -m 644 include/boxrig/ui/*.h \
		$(CORE_UI_BOUNDARY_TEST_ROOT)/usr/include/boxrig/ui
	$(CC) $(CFLAGS) $(PROJECT_CFLAGS) \
		-I$(CORE_UI_BOUNDARY_TEST_ROOT)/usr/include \
		$(CORE_UI_BOUNDARY_TEST_SOURCE) \
		$(CORE_UI_BOUNDARY_TEST_ROOT)/usr/lib/libboxrig-ui.a \
		$(CORE_UI_BOUNDARY_TEST_ROOT)/usr/lib/libboxrig.a \
		-o $(CORE_UI_BOUNDARY_TEST_BINARY)
	$(CORE_UI_BOUNDARY_TEST_BINARY)
	node $(DEPENDENCY_BOUNDARY_TEST_SCRIPT) \
		$(CORE_UI_BOUNDARY_TEST_ROOT)/usr/include/boxrig/terminal.h \
		native $(NM) $(LIBRARY) $(UI_LIBRARY) $(X11_LIBRARY) \
		$(GTK4_LIBRARY) $(GTK3_LIBRARY) $(WAYLAND_LIBRARY) :: \
		mingw-amd64 $(MINGW_AMD64_NM) $(MINGW_AMD64_LIBRARY) \
		$(MINGW_AMD64_UI_LIBRARY) $(MINGW_AMD64_WIN32_LIBRARY) :: \
		mingw-i686 $(MINGW_I686_NM) $(MINGW_I686_LIBRARY) \
		$(MINGW_I686_UI_LIBRARY) $(MINGW_I686_WIN32_LIBRARY)
	@if nm -u $(X11_LIBRARY) | awk '$$1 == "U" { print $$2 }' | \
		rg -q '^(egl|gl[A-Z])'; then \
		echo "FAIL software X11 archive has EGL or OpenGL symbols"; \
		exit 1; \
	fi

test-gtk4-package: $(GTK4_PACKAGE_TEST_SOURCE) $(GTK4_LIBRARY) \
		$(UI_LIBRARY) $(LIBRARY) $(GTK4_EXAMPLE_BINARY)
	$(RM) -r $(GTK4_PACKAGE_TEST_ROOT)
	$(MAKE) install-gtk4 PREFIX=$(GTK4_PACKAGE_TEST_ROOT)/usr
	test "$$(PKG_CONFIG_PATH=$(GTK4_PACKAGE_TEST_ROOT)/usr/lib/pkgconfig \
		pkg-config --modversion boxrig-gtk4)" = "0.0.0"
	test "$$(PKG_CONFIG_PATH=$(GTK4_PACKAGE_TEST_ROOT)/usr/lib/pkgconfig \
		pkg-config --print-requires boxrig-gtk4)" = "gtk4 >= 4.14"
	test "$$(PKG_CONFIG_PATH=$(GTK4_PACKAGE_TEST_ROOT)/usr/lib/pkgconfig \
		pkg-config --print-requires-private boxrig-gtk4 | \
		sed -n '/^pango /p')" = "pango >= 1.50"
	@for dependency in pangocairo pangoft2 'cairo >= 1.18' fontconfig; do \
		PKG_CONFIG_PATH=$(GTK4_PACKAGE_TEST_ROOT)/usr/lib/pkgconfig \
		pkg-config --print-requires-private boxrig-gtk4 | \
		rg -qx "$$dependency" || { \
			echo "FAIL missing private dependency: $$dependency"; \
			exit 1; \
		}; \
	done
	$(CC) $(CFLAGS) $(GTK4_PROJECT_CFLAGS) \
		$$(PKG_CONFIG_PATH=$(GTK4_PACKAGE_TEST_ROOT)/usr/lib/pkgconfig \
		pkg-config --cflags boxrig-gtk4) \
		$(GTK4_PACKAGE_TEST_SOURCE) -o $(GTK4_PACKAGE_TEST_BINARY) \
		$$(PKG_CONFIG_PATH=$(GTK4_PACKAGE_TEST_ROOT)/usr/lib/pkgconfig \
		pkg-config --libs boxrig-gtk4 \
		$$(PKG_CONFIG_PATH=$(GTK4_PACKAGE_TEST_ROOT)/usr/lib/pkgconfig \
		pkg-config --print-requires-private boxrig-gtk4 | \
		sed 's/[[:space:]].*//'))
	$(GTK4_PACKAGE_TEST_BINARY)
	$(XVFB_RUN) -a env GDK_BACKEND=x11 GDK_DEBUG=gl-prefer-gl GSK_RENDERER=gl $(GTK4_TEST_IM_ENV) bash tests/gtk/check_installed_rendering.sh $(GTK4_PACKAGE_TEST_ROOT) 4 $(GTK_OPENGL) "$(CC)" tests/gtk/test_installed_rendering.c
	@if nm -u $(LIBRARY) $(UI_LIBRARY) | \
		awk '$$1 == "U" { print $$2 }' | \
		rg -q '^(gtk_|gdk_|pango_|cairo_|Fc[A-Z])'; then \
		echo "FAIL portable archives have GTK or PangoCairo symbols"; \
		exit 1; \
	fi
	@if nm -u $(GTK4_LIBRARY) | awk '$$1 == "U" { print $$2 }' | \
		rg -v '^(gdk_wayland_display_(get_type|get_wl_display)|wl_(proxy_(marshal_flags|get_version|add_listener|destroy)|registry_interface|callback_interface))(@.*)?$$' | \
		rg -q '^(X[A-Z]|wl_|gdk_x11_|gdk_wayland_|vk)'; then \
		echo "FAIL GTK 4 archive has window-system-specific symbols"; \
		exit 1; \
	fi
	@if nm -D --undefined-only $(GTK4_EXAMPLE_BINARY) | \
		awk '$$1 == "U" { print $$2 }' | \
		rg -v '^(gdk_wayland_display_(get_type|get_wl_display)|wl_(proxy_(marshal_flags|get_version|add_listener|destroy)|registry_interface|callback_interface))(@.*)?$$' | \
		rg -q '^(X[A-Z]|wl_|gdk_x11_|gdk_wayland_)'; then \
		echo "FAIL GTK 4 sample has window-system-specific symbols"; \
		exit 1; \
	fi
	@nm -D --undefined-only $(GTK4_EXAMPLE_BINARY) | \
		awk '$$1 == "U" { print $$2 }' | rg -q '^gtk_' || { \
		echo "FAIL GTK 4 sample does not link GTK directly"; \
		exit 1; \
	}

test-gtk3-package: $(GTK3_PACKAGE_TEST_SOURCE) $(GTK3_PKG_CONFIG) \
		$(GTK3_LIBRARY) $(UI_LIBRARY) $(LIBRARY) $(GTK3_EXAMPLE_BINARY)
	$(RM) -r $(GTK3_PACKAGE_TEST_ROOT)
	$(MAKE) install-gtk3 PREFIX=$(GTK3_PACKAGE_TEST_ROOT)/usr
	test "$$(PKG_CONFIG_PATH=$(GTK3_PACKAGE_TEST_ROOT)/usr/lib/pkgconfig \
		pkg-config --modversion boxrig-gtk3)" = "0.0.0"
	test "$$(PKG_CONFIG_PATH=$(GTK3_PACKAGE_TEST_ROOT)/usr/lib/pkgconfig \
		pkg-config --print-requires boxrig-gtk3)" = "gtk+-3.0 >= 3.24"
	@for dependency in pangocairo 'cairo >= 1.18' fontconfig; do \
		PKG_CONFIG_PATH=$(GTK3_PACKAGE_TEST_ROOT)/usr/lib/pkgconfig \
		pkg-config --print-requires-private boxrig-gtk3 | \
		rg -qx "$$dependency" || { \
			echo "FAIL missing private dependency: $$dependency"; \
			exit 1; \
		}; \
	done
	$(CC) $(CFLAGS) $(GTK3_PROJECT_CFLAGS) \
		$$(PKG_CONFIG_PATH=$(GTK3_PACKAGE_TEST_ROOT)/usr/lib/pkgconfig \
		pkg-config --cflags boxrig-gtk3) \
		$(GTK3_PACKAGE_TEST_SOURCE) -o $(GTK3_PACKAGE_TEST_BINARY) \
		$$(PKG_CONFIG_PATH=$(GTK3_PACKAGE_TEST_ROOT)/usr/lib/pkgconfig \
		pkg-config --libs boxrig-gtk3 \
		$$(PKG_CONFIG_PATH=$(GTK3_PACKAGE_TEST_ROOT)/usr/lib/pkgconfig \
		pkg-config --print-requires-private boxrig-gtk3 | \
		sed 's/[[:space:]].*//'))
	$(GTK3_PACKAGE_TEST_BINARY)
	$(XVFB_RUN) -a env GDK_BACKEND=x11 GDK_DEBUG=gl-prefer-gl GSK_RENDERER=gl $(GTK4_TEST_IM_ENV) bash tests/gtk/check_installed_rendering.sh $(GTK3_PACKAGE_TEST_ROOT) 3 $(GTK_OPENGL) "$(CC)" tests/gtk/test_installed_rendering.c
	@if rg -n 'gtk4|Gtk4|GtkSnapshot|Gsk' $(GTK3_HEADERS); then \
		echo "FAIL GTK 3 public headers reference GTK 4"; \
		exit 1; \
	fi
	@if nm -u $(GTK3_LIBRARY) | awk '$$1 == "U" { print $$2 }' | \
		rg -v '^(gdk_wayland_display_(get_type|get_wl_display)|wl_(proxy_(marshal_flags|get_version|add_listener|destroy)|registry_interface|callback_interface))(@.*)?$$' | \
		rg -q '^(X[A-Z]|wl_|gdk_x11_|gdk_wayland_|gsk_|vk)'; then \
		echo "FAIL GTK 3 archive has GTK 4 or window-system-specific symbols"; \
		exit 1; \
	fi
	@if nm -g --defined-only $(GTK3_LIBRARY) | \
		awk '{ print $$3 }' | rg -q '^boxrig_gtk4_'; then \
		echo "FAIL GTK 3 archive exports GTK 4 boxrig symbols"; \
		exit 1; \
	fi
	@if nm -D --undefined-only $(GTK3_EXAMPLE_BINARY) | \
		awk '$$1 == "U" { print $$2 }' | \
		rg -v '^(gdk_wayland_display_(get_type|get_wl_display)|wl_(proxy_(marshal_flags|get_version|add_listener|destroy)|registry_interface|callback_interface))(@.*)?$$' | \
		rg -q '^(X[A-Z]|wl_|gdk_x11_|gdk_wayland_|gsk_|vk)'; then \
		echo "FAIL GTK 3 sample has GTK 4 or window-system-specific symbols"; \
		exit 1; \
	fi
	@nm -D --undefined-only $(GTK3_EXAMPLE_BINARY) | \
		awk '$$1 == "U" { print $$2 }' | rg -q '^gtk_' || { \
		echo "FAIL GTK 3 sample does not link GTK directly"; \
		exit 1; \
	}
	@if readelf -d $(GTK3_EXAMPLE_BINARY) | \
		rg -q 'Shared library: \[(libgtk-4|libX11)'; then \
		echo "FAIL GTK 3 sample directly needs GTK 4 or X11"; \
		exit 1; \
	fi

test-wayland-package: $(WAYLAND_PACKAGE_TEST_SOURCE) $(WAYLAND_PKG_CONFIG) \
		$(WAYLAND_LIBRARY) $(UI_LIBRARY) $(LIBRARY)
	$(RM) -r $(WAYLAND_PACKAGE_TEST_ROOT)
	$(MAKE) install-wayland PREFIX=$(WAYLAND_PACKAGE_TEST_ROOT)/usr
	test "$$(PKG_CONFIG_PATH=$(WAYLAND_PACKAGE_TEST_ROOT)/usr/lib/pkgconfig \
		pkg-config --modversion boxrig-wayland)" = "0.0.0"
	test "$$(PKG_CONFIG_PATH=$(WAYLAND_PACKAGE_TEST_ROOT)/usr/lib/pkgconfig \
		pkg-config --print-requires boxrig-wayland)" = \
		"$$(printf 'wayland-client >= 1.22\nxkbcommon >= 1.6')"
	test "$$(PKG_CONFIG_PATH=$(WAYLAND_PACKAGE_TEST_ROOT)/usr/lib/pkgconfig \
		pkg-config --print-requires-private boxrig-wayland | \
		sed -n '/^pango /p')" = "pango >= 1.50"
	@for dependency in pangocairo pangoft2 'cairo >= 1.18' fontconfig; do \
		PKG_CONFIG_PATH=$(WAYLAND_PACKAGE_TEST_ROOT)/usr/lib/pkgconfig \
		pkg-config --print-requires-private boxrig-wayland | \
		rg -qx "$$dependency" || { \
			echo "FAIL missing private dependency: $$dependency"; \
			exit 1; \
		}; \
	done
	$(CC) $(CFLAGS) $(PROJECT_CFLAGS) \
		$$(PKG_CONFIG_PATH=$(WAYLAND_PACKAGE_TEST_ROOT)/usr/lib/pkgconfig \
		pkg-config --cflags --static boxrig-wayland) \
		$(WAYLAND_PACKAGE_TEST_SOURCE) -o $(WAYLAND_PACKAGE_TEST_BINARY) \
		$$(PKG_CONFIG_PATH=$(WAYLAND_PACKAGE_TEST_ROOT)/usr/lib/pkgconfig \
		pkg-config --libs --static boxrig-wayland)
	$(WAYLAND_PACKAGE_TEST_BINARY)
	@test ! -e \
		$(WAYLAND_PACKAGE_TEST_ROOT)/usr/include/viewporter-client-protocol.h
	@test ! -e \
		$(WAYLAND_PACKAGE_TEST_ROOT)/usr/include/fractional-scale-v1-client-protocol.h
	@test ! -e \
		$(WAYLAND_PACKAGE_TEST_ROOT)/usr/include/text-input-v3-client-protocol.h
	@if nm -u $(LIBRARY) $(UI_LIBRARY) | \
		awk '$$1 == "U" { print $$2 }' | \
		rg -q '^(wl_|pango_|cairo_|Fc[A-Z])'; then \
		echo "FAIL portable archives have Wayland or PangoCairo symbols"; \
		exit 1; \
	fi
	@if nm -u $(WAYLAND_LIBRARY) | awk '$$1 == "U" { print $$2 }' | \
		rg -q '^(gtk_|gdk_|X[A-Z]|vk|egl|gl[A-Z])'; then \
		echo "FAIL Wayland archive has toolkit or other window-system symbols"; \
		exit 1; \
	fi

test-opengl-packages: $(OPENGL_PACKAGE_TEST_SOURCE) \
		$(X11_OPENGL_PACKAGE_TEST_SOURCE) $(WAYLAND_OPENGL_PACKAGE_TEST_SOURCE) \
		$(OPENGL_RENDERER_PKG_CONFIG) $(X11_OPENGL_PKG_CONFIG) \
		$(WAYLAND_OPENGL_PKG_CONFIG) $(OPENGL_RENDERER_LIBRARY) \
		$(X11_OPENGL_LIBRARY) $(WAYLAND_OPENGL_LIBRARY) $(X11_LIBRARY) \
		$(WAYLAND_LIBRARY) $(UI_LIBRARY) $(LIBRARY)
	$(RM) -r $(OPENGL_PACKAGE_TEST_ROOT)
	$(MAKE) install-x11-opengl PREFIX=$(OPENGL_PACKAGE_TEST_ROOT)/usr
	$(MAKE) install-wayland-opengl PREFIX=$(OPENGL_PACKAGE_TEST_ROOT)/usr
	test "$$(PKG_CONFIG_PATH=$(OPENGL_PACKAGE_TEST_ROOT)/usr/lib/pkgconfig \
		pkg-config --modversion boxrig-render-ogl)" = "0.0.0"
	test "$$(PKG_CONFIG_PATH=$(OPENGL_PACKAGE_TEST_ROOT)/usr/lib/pkgconfig \
		pkg-config --modversion boxrig-x11-ogl)" = "0.0.0"
	test "$$(PKG_CONFIG_PATH=$(OPENGL_PACKAGE_TEST_ROOT)/usr/lib/pkgconfig \
		pkg-config --modversion boxrig-wayland-ogl)" = "0.0.0"
	$(CC) $(CFLAGS) $(PROJECT_CFLAGS) \
		$$(PKG_CONFIG_PATH=$(OPENGL_PACKAGE_TEST_ROOT)/usr/lib/pkgconfig \
		pkg-config --cflags --static boxrig-render-ogl) \
		$(OPENGL_PACKAGE_TEST_SOURCE) -o $(OPENGL_PACKAGE_TEST_BINARY) \
		$$(PKG_CONFIG_PATH=$(OPENGL_PACKAGE_TEST_ROOT)/usr/lib/pkgconfig \
		pkg-config --libs --static boxrig-render-ogl) \
		$$(pkg-config --libs egl)
	$(XVFB_RUN) -a env LIBGL_ALWAYS_SOFTWARE=1 \
		$(OPENGL_PACKAGE_TEST_BINARY)
	$(CC) $(CFLAGS) $(PROJECT_CFLAGS) \
		$$(PKG_CONFIG_PATH=$(OPENGL_PACKAGE_TEST_ROOT)/usr/lib/pkgconfig \
		pkg-config --cflags --static boxrig-x11-ogl) \
		$(X11_OPENGL_PACKAGE_TEST_SOURCE) \
		-o $(X11_OPENGL_PACKAGE_TEST_BINARY) \
		$$(PKG_CONFIG_PATH=$(OPENGL_PACKAGE_TEST_ROOT)/usr/lib/pkgconfig \
		pkg-config --libs --static boxrig-x11-ogl)
	$(X11_OPENGL_PACKAGE_TEST_BINARY)
	$(CC) $(CFLAGS) $(PROJECT_CFLAGS) \
		$$(PKG_CONFIG_PATH=$(OPENGL_PACKAGE_TEST_ROOT)/usr/lib/pkgconfig \
		pkg-config --cflags --static boxrig-wayland-ogl) \
		$(WAYLAND_OPENGL_PACKAGE_TEST_SOURCE) \
		-o $(WAYLAND_OPENGL_PACKAGE_TEST_BINARY) \
		$$(PKG_CONFIG_PATH=$(OPENGL_PACKAGE_TEST_ROOT)/usr/lib/pkgconfig \
		pkg-config --libs --static boxrig-wayland-ogl)
	$(WAYLAND_OPENGL_PACKAGE_TEST_BINARY)
	@if rg -n '\b(EGLDisplay|EGLContext|EGLSurface|GLXContext|Display|Window|wl_display|wl_surface)\b' \
		$(OPENGL_RENDERER_HEADERS); then \
		echo "FAIL public OpenGL renderer headers expose window-system types"; \
		exit 1; \
	fi
	@if nm -u $(X11_LIBRARY) $(WAYLAND_LIBRARY) | \
		awk '$$1 == "U" { print $$2 }' | rg -q '^(egl|gl[A-Z])'; then \
		echo "FAIL software frontend archives have EGL or OpenGL symbols"; \
		exit 1; \
	fi

test-p2: test
	$(MAKE) test-gtk-surrounding-widget test-gtk-ime-video test-xim-capabilities test-wayland-surrounding-compositor
	$(MAKE) test-p2-x11 test-p2-x11-opengl test-p2-opengl test-p2-win32

test-p2-x11-opengl: $(X11_OPENGL_COMPONENT_TEST_BINARY)
	@if [ -z "$${DISPLAY:-}" ]; then \
		printf '%s\n' 'SKIP: P2 X11 OpenGL component probe (DISPLAY is undefined)'; \
	else \
		ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
			$(X11_OPENGL_COMPONENT_TEST_BINARY) native; \
	fi

test-p2-opengl: $(OPENGL_RUNTIME_TEST_BINARY)
	@if [ -z "$${DISPLAY:-}" ]; then \
		printf '%s\n' 'SKIP: P2 OpenGL physical-X11 probe (DISPLAY is undefined)'; \
	else \
		ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
			$(OPENGL_RUNTIME_TEST_BINARY) egl && \
		ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
			$(OPENGL_RUNTIME_TEST_BINARY) glx; \
	fi

test-p2-x11: $(P2_X11_IME_TEST_BINARY) $(P2_X11_IME_CHILD_BINARY) $(EXAMPLE_BINARY)
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
		UBSAN_OPTIONS=halt_on_error=1 \
		$(P2_X11_IME_TEST_BINARY) \
		$(abspath $(EXAMPLE_BINARY)) \
		$(abspath $(P2_X11_IME_CHILD_BINARY))

test-p2-win32: $(P2_WIN32_TEST_PREREQUISITES)
	$(MAKE) conpty-runtime-assets
	cd $(P2_WIN32_TEST_DIR) && npm ci --ignore-scripts && \
		BOXRIG_P2_WIN32_SOURCE_DIR="$(abspath .)" npm test

test-p2-win32: conpty-runtime-assets \
	$(MINGW_AMD64_NOTIFICATION_EXAMPLE) $(MINGW_I686_NOTIFICATION_EXAMPLE) \
	$(MINGW_AMD64_SHUTDOWN_PROBE_EXAMPLE) $(MINGW_I686_SHUTDOWN_PROBE_EXAMPLE) \
	$(MINGW_AMD64_IME_MOVEMENT_PROBE_EXAMPLE) $(MINGW_I686_IME_MOVEMENT_PROBE_EXAMPLE)

test-gtk3-gestament: $(GTK3_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) \
		$(GTK3_CLIPBOARD_PEER_BINARY) $(GTK3_GESTAMENT_INSTALL_STAMP)
	cd $(GTK3_GESTAMENT_TEST_DIR) && \
		BOXRIG_GTK3_SAMPLE="$(abspath $(GTK3_EXAMPLE_BINARY))" \
		BOXRIG_GTK3_PTY_HELPER="$(abspath $(PTY_HELPER_BINARY))" \
		BOXRIG_GTK3_CLIPBOARD_PEER="$(abspath $(GTK3_CLIPBOARD_PEER_BINARY))" \
		npm test

$(GTK3_GESTAMENT_INSTALL_STAMP): $(GTK3_GESTAMENT_PACKAGE_FILES)
	cd $(GTK3_GESTAMENT_TEST_DIR) && npm ci --ignore-scripts
	test -f $@

.PHONY: conpty-runtime-assets
conpty-runtime-assets: $(MINGW_AMD64_EXAMPLE_CONPTY_ASSETS) $(MINGW_I686_EXAMPLE_CONPTY_ASSETS) \
	$(MINGW_AMD64_BUILD_CONPTY_ASSETS) $(MINGW_I686_BUILD_CONPTY_ASSETS) \
	$(MINGW_AMD64_TEST_CONPTY_ASSETS) $(MINGW_I686_TEST_CONPTY_ASSETS)

$(CONPTY_PACKAGE): | $(CONPTY_BUILD_DIR)
	curl --fail --location --retry 2 '$(CONPTY_PACKAGE_URL)' --output '$@.tmp'
	printf '%s  %s\n' '$(CONPTY_PACKAGE_SHA256)' '$@.tmp' | sha256sum -c -
	mv '$@.tmp' '$@'

$(CONPTY_VERIFIED): $(CONPTY_PACKAGE) Makefile | $(CONPTY_BUILD_DIR)
	printf '%s  %s\n' '$(CONPTY_PACKAGE_SHA256)' '$(CONPTY_PACKAGE)' | sha256sum -c -
	touch $@

$(CONPTY_AMD64_DLL): $(CONPTY_VERIFIED) | $(CONPTY_BUILD_DIR)/amd64
	unzip -p $(CONPTY_PACKAGE) runtimes/win-x64/native/conpty.dll > $@

$(CONPTY_I686_DLL): $(CONPTY_VERIFIED) | $(CONPTY_BUILD_DIR)/i686
	unzip -p $(CONPTY_PACKAGE) runtimes/win-x86/native/conpty.dll > $@

$(CONPTY_X86_HOST): $(CONPTY_VERIFIED) | $(CONPTY_BUILD_DIR)/hosts/x86
	unzip -p $(CONPTY_PACKAGE) build/native/runtimes/x86/OpenConsole.exe > $@

$(CONPTY_X64_HOST): $(CONPTY_VERIFIED) | $(CONPTY_BUILD_DIR)/hosts/x64
	unzip -p $(CONPTY_PACKAGE) build/native/runtimes/x64/OpenConsole.exe > $@

$(CONPTY_ARM64_HOST): $(CONPTY_VERIFIED) | $(CONPTY_BUILD_DIR)/hosts/arm64
	unzip -p $(CONPTY_PACKAGE) build/native/runtimes/arm64/OpenConsole.exe > $@

$(BUILD_DIR)/boxrig_term/win32/amd64/conpty.dll \
$(MINGW_AMD64_BUILD_DIR)/conpty.dll \
$(MINGW_AMD64_BUILD_DIR)/tests/conpty.dll: $(CONPTY_AMD64_DLL) | \
	$(BUILD_DIR)/boxrig_term/win32/amd64 $(MINGW_AMD64_BUILD_DIR) \
	$(MINGW_AMD64_BUILD_DIR)/tests
	cp $< $@

$(BUILD_DIR)/boxrig_term/win32/i686/conpty.dll \
$(MINGW_I686_BUILD_DIR)/conpty.dll \
$(MINGW_I686_BUILD_DIR)/tests/conpty.dll: $(CONPTY_I686_DLL) | \
	$(BUILD_DIR)/boxrig_term/win32/i686 $(MINGW_I686_BUILD_DIR) \
	$(MINGW_I686_BUILD_DIR)/tests
	cp $< $@

$(BUILD_DIR)/boxrig_term/win32/i686/x86/OpenConsole.exe \
$(MINGW_I686_BUILD_DIR)/x86/OpenConsole.exe \
$(MINGW_I686_BUILD_DIR)/tests/x86/OpenConsole.exe: $(CONPTY_X86_HOST) | \
	$(BUILD_DIR)/boxrig_term/win32/i686/x86 $(MINGW_I686_BUILD_DIR)/x86 \
	$(MINGW_I686_BUILD_DIR)/tests/x86
	cp $< $@

$(BUILD_DIR)/boxrig_term/win32/amd64/x64/OpenConsole.exe \
$(MINGW_AMD64_BUILD_DIR)/x64/OpenConsole.exe \
$(MINGW_AMD64_BUILD_DIR)/tests/x64/OpenConsole.exe \
$(BUILD_DIR)/boxrig_term/win32/i686/x64/OpenConsole.exe \
$(MINGW_I686_BUILD_DIR)/x64/OpenConsole.exe \
$(MINGW_I686_BUILD_DIR)/tests/x64/OpenConsole.exe: $(CONPTY_X64_HOST) | \
	$(BUILD_DIR)/boxrig_term/win32/amd64/x64 \
	$(MINGW_AMD64_BUILD_DIR)/x64 $(MINGW_AMD64_BUILD_DIR)/tests/x64 \
	$(BUILD_DIR)/boxrig_term/win32/i686/x64 \
	$(MINGW_I686_BUILD_DIR)/x64 $(MINGW_I686_BUILD_DIR)/tests/x64
	cp $< $@

$(BUILD_DIR)/boxrig_term/win32/amd64/arm64/OpenConsole.exe \
$(MINGW_AMD64_BUILD_DIR)/arm64/OpenConsole.exe \
$(MINGW_AMD64_BUILD_DIR)/tests/arm64/OpenConsole.exe \
$(BUILD_DIR)/boxrig_term/win32/i686/arm64/OpenConsole.exe \
$(MINGW_I686_BUILD_DIR)/arm64/OpenConsole.exe \
$(MINGW_I686_BUILD_DIR)/tests/arm64/OpenConsole.exe: $(CONPTY_ARM64_HOST) | \
	$(BUILD_DIR)/boxrig_term/win32/amd64/arm64 \
	$(MINGW_AMD64_BUILD_DIR)/arm64 $(MINGW_AMD64_BUILD_DIR)/tests/arm64 \
	$(BUILD_DIR)/boxrig_term/win32/i686/arm64 \
	$(MINGW_I686_BUILD_DIR)/arm64 $(MINGW_I686_BUILD_DIR)/tests/arm64
	cp $< $@


verify-native-core-archive: $(CORE_OBJECTS) $(UTF8PROC_OBJECT) $(LIBPNG_OBJECTS) $(ZLIB_OBJECTS)
	$(call verify_archive_members,$(LIBRARY),$(AR),$(CORE_OBJECTS) $(UTF8PROC_OBJECT) $(LIBPNG_OBJECTS) $(ZLIB_OBJECTS))

verify-native-ui-archive: $(UI_OBJECTS)
	$(call verify_archive_members,$(UI_LIBRARY),$(AR),$(UI_OBJECTS))

verify-native-session-archive: $(SESSION_OBJECTS)
	$(call verify_archive_members,$(SESSION_LIBRARY),$(AR),$(SESSION_OBJECTS))

verify-native-posix-archive: $(POSIX_OBJECTS)
	$(call verify_archive_members,$(POSIX_LIBRARY),$(AR),$(POSIX_OBJECTS))

verify-native-x11-archive: $(X11_OBJECTS)
	$(call verify_archive_members,$(X11_LIBRARY),$(AR),$(X11_OBJECTS))

verify-native-x11-opengl-archive: $(X11_OPENGL_OBJECTS)
	$(call verify_archive_members,$(X11_OPENGL_LIBRARY),$(AR),$(X11_OPENGL_OBJECTS))

verify-native-gtk4-archive: $(GTK4_OBJECTS)
	$(call verify_archive_members,$(GTK4_LIBRARY),$(AR),$(GTK4_OBJECTS))

verify-native-gtk3-archive: $(GTK3_OBJECTS)
	$(call verify_archive_members,$(GTK3_LIBRARY),$(AR),$(GTK3_OBJECTS))

verify-native-wayland-archive: $(WAYLAND_OBJECTS)
	$(call verify_archive_members,$(WAYLAND_LIBRARY),$(AR),$(WAYLAND_OBJECTS))

verify-native-wayland-opengl-archive: $(WAYLAND_OPENGL_OBJECTS)
	$(call verify_archive_members,$(WAYLAND_OPENGL_LIBRARY),$(AR),$(WAYLAND_OPENGL_OBJECTS))

verify-amd64-core-archive: $(MINGW_AMD64_CORE_OBJECTS) $(MINGW_AMD64_UTF8PROC_OBJECT) $(MINGW_AMD64_LIBPNG_OBJECTS) $(MINGW_AMD64_ZLIB_OBJECTS)
	$(call verify_archive_members,$(MINGW_AMD64_LIBRARY),$(MINGW_AMD64_AR),$(MINGW_AMD64_CORE_OBJECTS) $(MINGW_AMD64_UTF8PROC_OBJECT) $(MINGW_AMD64_LIBPNG_OBJECTS) $(MINGW_AMD64_ZLIB_OBJECTS))

verify-i686-core-archive: $(MINGW_I686_CORE_OBJECTS) $(MINGW_I686_UTF8PROC_OBJECT) $(MINGW_I686_LIBPNG_OBJECTS) $(MINGW_I686_ZLIB_OBJECTS)
	$(call verify_archive_members,$(MINGW_I686_LIBRARY),$(MINGW_I686_AR),$(MINGW_I686_CORE_OBJECTS) $(MINGW_I686_UTF8PROC_OBJECT) $(MINGW_I686_LIBPNG_OBJECTS) $(MINGW_I686_ZLIB_OBJECTS))

verify-amd64-ui-archive: $(MINGW_AMD64_UI_OBJECTS)
	$(call verify_archive_members,$(MINGW_AMD64_UI_LIBRARY),$(MINGW_AMD64_AR),$(MINGW_AMD64_UI_OBJECTS))

verify-i686-ui-archive: $(MINGW_I686_UI_OBJECTS)
	$(call verify_archive_members,$(MINGW_I686_UI_LIBRARY),$(MINGW_I686_AR),$(MINGW_I686_UI_OBJECTS))

verify-amd64-session-archive: $(MINGW_AMD64_SESSION_OBJECTS)
	$(call verify_archive_members,$(MINGW_AMD64_SESSION_LIBRARY),$(MINGW_AMD64_AR),$(MINGW_AMD64_SESSION_OBJECTS))

verify-i686-session-archive: $(MINGW_I686_SESSION_OBJECTS)
	$(call verify_archive_members,$(MINGW_I686_SESSION_LIBRARY),$(MINGW_I686_AR),$(MINGW_I686_SESSION_OBJECTS))

verify-amd64-win32-archive: $(MINGW_AMD64_WIN32_OBJECTS)
	$(call verify_archive_members,$(MINGW_AMD64_WIN32_LIBRARY),$(MINGW_AMD64_AR),$(MINGW_AMD64_WIN32_OBJECTS))

verify-i686-win32-archive: $(MINGW_I686_WIN32_OBJECTS)
	$(call verify_archive_members,$(MINGW_I686_WIN32_LIBRARY),$(MINGW_I686_AR),$(MINGW_I686_WIN32_OBJECTS))

$(LIBRARY): $(CORE_OBJECTS) $(UTF8PROC_OBJECT) $(LIBPNG_OBJECTS) $(ZLIB_OBJECTS) | $(BUILD_DIR) verify-native-core-archive
	$(RM) $@
	$(AR) rcs $@ $^

$(UI_LIBRARY): $(UI_OBJECTS) | $(BUILD_DIR) verify-native-ui-archive
	$(RM) $@
	$(AR) rcs $@ $^

$(SESSION_LIBRARY): $(SESSION_OBJECTS) | $(BUILD_DIR) verify-native-session-archive
	$(RM) $@
	$(AR) rcs $@ $^

$(POSIX_LIBRARY): $(POSIX_OBJECTS) | $(BUILD_DIR) verify-native-posix-archive
	$(RM) $@
	$(AR) rcs $@ $^

$(X11_LIBRARY): $(X11_OBJECTS) | $(BUILD_DIR) verify-native-x11-archive
	$(RM) $@
	$(AR) rcs $@ $^

$(GTK4_LIBRARY): $(GTK4_OBJECTS) | $(BUILD_DIR) verify-native-gtk4-archive
	$(RM) $@
	$(AR) rcs $@ $^

$(GTK3_LIBRARY): $(GTK3_OBJECTS) | $(BUILD_DIR) verify-native-gtk3-archive
	$(RM) $@
	$(AR) rcs $@ $^

$(WAYLAND_LIBRARY): $(WAYLAND_OBJECTS) | $(BUILD_DIR) verify-native-wayland-archive
	$(RM) $@
	$(AR) rcs $@ $^

$(OPENGL_RENDERER_LIBRARY): $(OPENGL_RENDERER_OBJECTS) | $(BUILD_DIR)
	$(RM) $@
	$(AR) rcs $@ $^

$(X11_OPENGL_LIBRARY): $(X11_OPENGL_OBJECTS) | $(BUILD_DIR) \
		verify-native-x11-opengl-archive
	$(RM) $@
	$(AR) rcs $@ $^

$(WAYLAND_OPENGL_LIBRARY): $(WAYLAND_OPENGL_OBJECTS) | $(BUILD_DIR) \
		verify-native-wayland-opengl-archive
	$(RM) $@
	$(AR) rcs $@ $^

$(BUILD_DIR)/core/%.o: src/core/%.c | $(BUILD_DIR)/core
	$(CC) $(CPPFLAGS) $(CORE_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) -c $< -o $@

$(BUILD_DIR)/ui/%.o: src/ui/%.c | $(BUILD_DIR)/ui
	$(CC) $(CPPFLAGS) $(UI_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) -c $< -o $@

$(BUILD_DIR)/ui/word_break.o: src/ui/word_break_data.inc

$(BUILD_DIR)/opengl/%.o: src/opengl/%.c | $(BUILD_DIR)/opengl
	$(CC) $(CPPFLAGS) -Isrc $(PROJECT_CPPFLAGS) $(POSIX_CPPFLAGS) \
		$(OPENGL_RENDERER_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) -c $< -o $@

$(BUILD_DIR)/x11_opengl/%.o: src/x11_opengl/%.c | $(BUILD_DIR)/x11_opengl
	$(CC) $(CPPFLAGS) -Isrc $(PROJECT_CPPFLAGS) $(POSIX_CPPFLAGS) \
		$(X11_CPPFLAGS) $(OPENGL_RENDERER_CPPFLAGS) $(CFLAGS) \
		$(PROJECT_CFLAGS) -c $< -o $@

$(BUILD_DIR)/wayland_opengl/%.o: src/wayland_opengl/%.c \
		$(WAYLAND_PRIVATE_HEADERS) $(WAYLAND_GENERATED_HEADERS) | \
		$(BUILD_DIR)/wayland_opengl
	$(CC) $(CPPFLAGS) -Isrc $(PROJECT_CPPFLAGS) $(POSIX_CPPFLAGS) \
		-I$(WAYLAND_PROTOCOL_DIR) $(WAYLAND_CPPFLAGS) \
		$(WAYLAND_OPENGL_CPPFLAGS) $(PANGO_CAIRO_CPPFLAGS) $(CFLAGS) \
		$(PROJECT_CFLAGS) -c $< -o $@

$(BUILD_DIR)/session/%.o: src/session/%.c | $(BUILD_DIR)/session
	$(CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) -c $< -o $@

$(BUILD_DIR)/posix/%.o: src/posix/%.c | $(BUILD_DIR)/posix
	$(CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(POSIX_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) -c $< -o $@

$(BUILD_DIR)/x11/%.o: src/x11/%.c | $(BUILD_DIR)/x11
	$(CC) $(CPPFLAGS) $(FRONTEND_CPPFLAGS) $(POSIX_CPPFLAGS) \
		$(X11_CPPFLAGS) $(DBUS_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) -c $< -o $@

$(BUILD_DIR)/gtk4/%.o: src/gtk4/%.c | $(BUILD_DIR)/gtk4
	$(CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(POSIX_CPPFLAGS) \
		$(GTK4_CPPFLAGS) $(CFLAGS) $(GTK4_PROJECT_CFLAGS) -c $< -o $@

$(BUILD_DIR)/gtk4/gl_%.o: src/gtk/gl_%.c src/gtk/gl_area.h src/gtk/gl_surface.h | $(BUILD_DIR)/gtk4
	$(CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(POSIX_CPPFLAGS) \
		$(GTK4_CPPFLAGS) $(CFLAGS) $(GTK4_PROJECT_CFLAGS) -c $< -o $@

$(BUILD_DIR)/gtk4/pangocairo_renderer.o: src/pangocairo/renderer.c $(PANGO_CANDIDATE_OBJECT) \
		src/pangocairo/renderer.h src/pangocairo/renderer_private.h src/pangocairo/glyph_fit.h src/ui/glyph_fit.h \
		src/ui/view_private.h src/ui/color_private.h | \
		$(BUILD_DIR)/gtk4
	$(CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(POSIX_CPPFLAGS) \
		$(GTK4_CPPFLAGS) $(CFLAGS) $(GTK4_PROJECT_CFLAGS) -c $< -o $@

$(BUILD_DIR)/gtk3/%.o: src/gtk3/%.c | $(BUILD_DIR)/gtk3
	$(CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(POSIX_CPPFLAGS) \
		$(GTK3_CPPFLAGS) $(CFLAGS) $(GTK3_PROJECT_CFLAGS) -c $< -o $@

$(BUILD_DIR)/gtk3/accessible_text.o: src/gtk/accessible_text.c src/gtk/accessible_text.h src/ui/accessible.h | $(BUILD_DIR)/gtk3
	$(CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(GTK3_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) -c $< -o $@

$(BUILD_DIR)/gtk3/accessible.o $(BUILD_DIR)/gtk3/terminal_widget.o: src/gtk3/accessible.h src/gtk/accessible_text.h src/ui/accessible.h

$(BUILD_DIR)/gtk4/accessible_text.o: src/gtk/accessible_text.c src/gtk/accessible_text.h src/ui/accessible.h | $(BUILD_DIR)/gtk4
	$(CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(GTK4_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) -c $< -o $@

$(BUILD_DIR)/gtk4/accessible.o $(BUILD_DIR)/gtk4/terminal_widget.o: src/gtk4/accessible.h src/gtk/accessible_text.h src/ui/accessible.h

$(BUILD_DIR)/gtk3/gl_%.o: src/gtk/gl_%.c src/gtk/gl_area.h src/gtk/gl_surface.h | $(BUILD_DIR)/gtk3
	$(CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(POSIX_CPPFLAGS) \
		$(GTK3_CPPFLAGS) $(CFLAGS) $(GTK3_PROJECT_CFLAGS) -c $< -o $@

$(BUILD_DIR)/gtk3/pangocairo_renderer.o: src/pangocairo/renderer.c $(PANGO_CANDIDATE_OBJECT) \
		src/pangocairo/renderer.h src/pangocairo/renderer_private.h src/pangocairo/glyph_fit.h src/ui/glyph_fit.h \
		src/ui/view_private.h src/ui/color_private.h | \
		$(BUILD_DIR)/gtk3
	$(CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(POSIX_CPPFLAGS) \
		$(GTK3_CPPFLAGS) $(CFLAGS) $(GTK3_PROJECT_CFLAGS) -c $< -o $@

$(BUILD_DIR)/wayland/%.o: src/wayland/%.c $(WAYLAND_PRIVATE_HEADERS) \
		$(WAYLAND_GENERATED_HEADERS) | $(BUILD_DIR)/wayland
	$(CC) $(CPPFLAGS) $(FRONTEND_CPPFLAGS) $(POSIX_CPPFLAGS) \
		-I$(WAYLAND_PROTOCOL_DIR) $(WAYLAND_CPPFLAGS) $(PANGO_CAIRO_CPPFLAGS) $(CFLAGS) \
		$(PROJECT_CFLAGS) -c $< -o $@

$(BUILD_DIR)/wayland/pangocairo_renderer.o: src/pangocairo/renderer.c $(PANGO_CANDIDATE_OBJECT) \
		src/pangocairo/renderer.h src/pangocairo/renderer_private.h src/pangocairo/glyph_fit.h src/ui/glyph_fit.h \
		src/ui/view_private.h src/ui/color_private.h | \
		$(BUILD_DIR)/wayland
	$(CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(POSIX_CPPFLAGS) \
		$(PANGO_CAIRO_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) -c $< -o $@

$(BUILD_DIR)/wayland/viewporter-protocol.o: $(WAYLAND_VIEWPORTER_SOURCE) | $(BUILD_DIR)/wayland
	$(CC) $(CPPFLAGS) -I$(WAYLAND_PROTOCOL_DIR) $(WAYLAND_CPPFLAGS) \
		$(CFLAGS) $(PROJECT_CFLAGS) -c $< -o $@

$(BUILD_DIR)/wayland/fractional-scale-v1-protocol.o: \
		$(WAYLAND_FRACTIONAL_SCALE_SOURCE) | $(BUILD_DIR)/wayland
	$(CC) $(CPPFLAGS) -I$(WAYLAND_PROTOCOL_DIR) $(WAYLAND_CPPFLAGS) \
		$(CFLAGS) $(PROJECT_CFLAGS) -c $< -o $@

$(BUILD_DIR)/wayland/text-input-v3-protocol.o: \
		$(WAYLAND_TEXT_INPUT_SOURCE) | $(BUILD_DIR)/wayland
	$(CC) $(CPPFLAGS) -I$(WAYLAND_PROTOCOL_DIR) $(WAYLAND_CPPFLAGS) \
		$(CFLAGS) $(PROJECT_CFLAGS) -c $< -o $@

$(BUILD_DIR)/wayland/primary-selection-v1-protocol.o: \
		$(WAYLAND_PRIMARY_SELECTION_SOURCE) | $(BUILD_DIR)/wayland
	$(CC) $(CPPFLAGS) -I$(WAYLAND_PROTOCOL_DIR) $(WAYLAND_CPPFLAGS) \
		$(CFLAGS) $(PROJECT_CFLAGS) -c $< -o $@

# Initialize all definitions once, including when parallel generators need them.
$(WAYLAND_PROTOCOL_XMLS) &:
	git submodule update --init --checkout -- $(WAYLAND_PROTOCOLS_DIR)
	@for protocol in $(WAYLAND_PROTOCOL_XMLS); do \
		test -f "$$protocol" || { \
			printf 'Missing Wayland protocol definition: %s\n' "$$protocol" >&2; \
			exit 1; \
		}; \
	done

$(WAYLAND_VIEWPORTER_HEADER): $(WAYLAND_VIEWPORTER_XML) | $(WAYLAND_PROTOCOL_DIR)
	@printf 'wayland protocol: %s (wayland-protocols %s, wp_viewporter max 1)\n' '$<' '$(WAYLAND_PROTOCOLS_VERSION)'
	$(WAYLAND_SCANNER) client-header $< $@

$(WAYLAND_VIEWPORTER_SOURCE): $(WAYLAND_VIEWPORTER_XML) | $(WAYLAND_PROTOCOL_DIR)
	$(WAYLAND_SCANNER) private-code $< $@

$(WAYLAND_VIEWPORTER_SERVER_HEADER): $(WAYLAND_VIEWPORTER_XML) | $(WAYLAND_PROTOCOL_DIR)
	$(WAYLAND_SCANNER) server-header $< $@

$(WAYLAND_FRACTIONAL_SCALE_HEADER): $(WAYLAND_FRACTIONAL_SCALE_XML) | $(WAYLAND_PROTOCOL_DIR)
	@printf 'wayland protocol: %s (wayland-protocols %s, wp_fractional_scale_manager_v1 max 1)\n' '$<' '$(WAYLAND_PROTOCOLS_VERSION)'
	$(WAYLAND_SCANNER) client-header $< $@

$(WAYLAND_FRACTIONAL_SCALE_SOURCE): $(WAYLAND_FRACTIONAL_SCALE_XML) | $(WAYLAND_PROTOCOL_DIR)
	$(WAYLAND_SCANNER) private-code $< $@

$(WAYLAND_FRACTIONAL_SCALE_SERVER_HEADER): $(WAYLAND_FRACTIONAL_SCALE_XML) | $(WAYLAND_PROTOCOL_DIR)
	$(WAYLAND_SCANNER) server-header $< $@

$(WAYLAND_TEXT_INPUT_HEADER): $(WAYLAND_TEXT_INPUT_XML) | $(WAYLAND_PROTOCOL_DIR)
	@printf 'wayland protocol: %s (wayland-protocols %s, zwp_text_input_manager_v3 max 2)\n' '$<' '$(WAYLAND_PROTOCOLS_VERSION)'
	$(WAYLAND_SCANNER) client-header $< $@

$(WAYLAND_TEXT_INPUT_SOURCE): $(WAYLAND_TEXT_INPUT_XML) | $(WAYLAND_PROTOCOL_DIR)
	$(WAYLAND_SCANNER) private-code $< $@

$(WAYLAND_TEXT_INPUT_SERVER_HEADER): $(WAYLAND_TEXT_INPUT_XML) | $(WAYLAND_PROTOCOL_DIR)
	$(WAYLAND_SCANNER) server-header $< $@

$(WAYLAND_PRIMARY_SELECTION_HEADER): $(WAYLAND_PRIMARY_SELECTION_XML) | $(WAYLAND_PROTOCOL_DIR)
	@printf 'wayland protocol: %s (wayland-protocols %s, zwp_primary_selection_device_manager_v1 max 1)\n' '$<' '$(WAYLAND_PROTOCOLS_VERSION)'
	$(WAYLAND_SCANNER) client-header $< $@

$(WAYLAND_PRIMARY_SELECTION_SOURCE): $(WAYLAND_PRIMARY_SELECTION_XML) | $(WAYLAND_PROTOCOL_DIR)
	$(WAYLAND_SCANNER) private-code $< $@

$(WAYLAND_PRIMARY_SELECTION_SERVER_HEADER): $(WAYLAND_PRIMARY_SELECTION_XML) | $(WAYLAND_PROTOCOL_DIR)
	$(WAYLAND_SCANNER) server-header $< $@

$(WAYLAND_XDG_SHELL_HEADER): $(WAYLAND_XDG_SHELL_XML) | $(WAYLAND_PROTOCOL_DIR)
	@printf 'wayland sample protocol: %s (wayland-protocols %s, xdg_wm_base max 7)\n' '$<' '$(WAYLAND_PROTOCOLS_VERSION)'
	$(WAYLAND_SCANNER) client-header $< $@

$(WAYLAND_XDG_SHELL_SOURCE): $(WAYLAND_XDG_SHELL_XML) | $(WAYLAND_PROTOCOL_DIR)
	$(WAYLAND_SCANNER) private-code $< $@

$(WAYLAND_XDG_DECORATION_HEADER): $(WAYLAND_XDG_DECORATION_XML) | $(WAYLAND_PROTOCOL_DIR)
	@printf 'wayland sample protocol: %s (wayland-protocols %s, zxdg_decoration_manager_v1 max 1)\n' '$<' '$(WAYLAND_PROTOCOLS_VERSION)'
	$(WAYLAND_SCANNER) client-header $< $@

$(WAYLAND_XDG_DECORATION_SOURCE): $(WAYLAND_XDG_DECORATION_XML) | $(WAYLAND_PROTOCOL_DIR)
	$(WAYLAND_SCANNER) private-code $< $@

$(UTF8PROC_OBJECT): $(UTF8PROC_SOURCE) deps/utf8proc/utf8proc.h | $(BUILD_DIR)/core
	$(CC) $(CPPFLAGS) -Ideps/utf8proc -DUTF8PROC_STATIC $(CFLAGS) $(NATIVE_DEPENDENCY_CFLAGS) -std=c99 -c $< -o $@

$(BUILD_DIR)/deps/zlib/%.o: deps/zlib/%.c | $(BUILD_DIR)/deps/zlib
	$(CC) $(CPPFLAGS) -Ideps/zlib $(CFLAGS) $(NATIVE_DEPENDENCY_CFLAGS) -std=c99 -c $< -o $@

$(BUILD_DIR)/deps/libpng/%.o: deps/libpng/%.c $(PNG_CONFIG) | $(BUILD_DIR)/deps/libpng
	mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(DEPENDENCY_CPPFLAGS) $(CFLAGS) $(NATIVE_DEPENDENCY_CFLAGS) -std=c99 -c $< -o $@

$(UTF8PROC_OBJECT) $(LIBPNG_OBJECTS) $(ZLIB_OBJECTS): Makefile

$(CORE_OBJECTS): $(CORE_HEADERS) $(PNG_CONFIG) Makefile

$(MINGW_AMD64_LIBRARY): $(MINGW_AMD64_CORE_OBJECTS) $(MINGW_AMD64_UTF8PROC_OBJECT) $(MINGW_AMD64_LIBPNG_OBJECTS) $(MINGW_AMD64_ZLIB_OBJECTS) | $(MINGW_AMD64_BUILD_DIR) verify-amd64-core-archive
	$(RM) $@
	$(MINGW_AMD64_AR) rcs $@ $^

$(MINGW_I686_LIBRARY): $(MINGW_I686_CORE_OBJECTS) $(MINGW_I686_UTF8PROC_OBJECT) $(MINGW_I686_LIBPNG_OBJECTS) $(MINGW_I686_ZLIB_OBJECTS) | $(MINGW_I686_BUILD_DIR) verify-i686-core-archive
	$(RM) $@
	$(MINGW_I686_AR) rcs $@ $^

$(MINGW_AMD64_UI_LIBRARY): $(MINGW_AMD64_UI_OBJECTS) | $(MINGW_AMD64_BUILD_DIR) verify-amd64-ui-archive
	$(RM) $@
	$(MINGW_AMD64_AR) rcs $@ $^

$(MINGW_I686_UI_LIBRARY): $(MINGW_I686_UI_OBJECTS) | $(MINGW_I686_BUILD_DIR) verify-i686-ui-archive
	$(RM) $@
	$(MINGW_I686_AR) rcs $@ $^

$(MINGW_AMD64_SESSION_LIBRARY): $(MINGW_AMD64_SESSION_OBJECTS) | $(MINGW_AMD64_BUILD_DIR) verify-amd64-session-archive
	$(RM) $@
	$(MINGW_AMD64_AR) rcs $@ $^

$(MINGW_I686_SESSION_LIBRARY): $(MINGW_I686_SESSION_OBJECTS) | $(MINGW_I686_BUILD_DIR) verify-i686-session-archive
	$(RM) $@
	$(MINGW_I686_AR) rcs $@ $^

$(MINGW_AMD64_WIN32_LIBRARY): $(MINGW_AMD64_WIN32_OBJECTS) | $(MINGW_AMD64_BUILD_DIR) verify-amd64-win32-archive
	$(RM) $@
	$(MINGW_AMD64_AR) rcs $@ $^

$(MINGW_I686_WIN32_LIBRARY): $(MINGW_I686_WIN32_OBJECTS) | $(MINGW_I686_BUILD_DIR) verify-i686-win32-archive
	$(RM) $@
	$(MINGW_I686_AR) rcs $@ $^

$(MINGW_AMD64_BUILD_DIR)/core/%.o: src/core/%.c | $(MINGW_AMD64_BUILD_DIR)/core
	$(MINGW_AMD64_CC) $(CPPFLAGS) $(CORE_CPPFLAGS) $(MINGW_AMD64_CORE_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) -c $< -o $@

$(MINGW_I686_BUILD_DIR)/core/%.o: src/core/%.c | $(MINGW_I686_BUILD_DIR)/core
	$(MINGW_I686_CC) $(CPPFLAGS) $(CORE_CPPFLAGS) $(MINGW_I686_CORE_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) -c $< -o $@

$(MINGW_AMD64_BUILD_DIR)/ui/%.o: src/ui/%.c | $(MINGW_AMD64_BUILD_DIR)/ui
	$(MINGW_AMD64_CC) $(CPPFLAGS) $(UI_CPPFLAGS) $(MINGW_AMD64_CORE_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) -c $< -o $@

$(MINGW_I686_BUILD_DIR)/ui/%.o: src/ui/%.c | $(MINGW_I686_BUILD_DIR)/ui
	$(MINGW_I686_CC) $(CPPFLAGS) $(UI_CPPFLAGS) $(MINGW_I686_CORE_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) -c $< -o $@

$(MINGW_AMD64_BUILD_DIR)/ui/word_break.o: src/ui/word_break_data.inc

$(MINGW_I686_BUILD_DIR)/ui/word_break.o: src/ui/word_break_data.inc

$(MINGW_AMD64_BUILD_DIR)/session/%.o: src/session/%.c | $(MINGW_AMD64_BUILD_DIR)/session
	$(MINGW_AMD64_CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(MINGW_AMD64_CORE_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) -c $< -o $@

$(MINGW_I686_BUILD_DIR)/session/%.o: src/session/%.c | $(MINGW_I686_BUILD_DIR)/session
	$(MINGW_I686_CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(MINGW_I686_CORE_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) -c $< -o $@

$(MINGW_AMD64_BUILD_DIR)/win32/session.o \
$(MINGW_I686_BUILD_DIR)/win32/session.o: \
	private MINGW_UI_CPPFLAGS := $(MINGW_TERM_CPPFLAGS)

$(MINGW_AMD64_BUILD_DIR)/win32/%.o: src/win32/%.c | $(MINGW_AMD64_BUILD_DIR)/win32
	$(MINGW_AMD64_CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(MINGW_UI_CPPFLAGS) \
		$(CFLAGS) $(PROJECT_CFLAGS) -c $< -o $@

$(MINGW_I686_BUILD_DIR)/win32/%.o: src/win32/%.c | $(MINGW_I686_BUILD_DIR)/win32
	$(MINGW_I686_CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(MINGW_UI_CPPFLAGS) \
		$(CFLAGS) $(PROJECT_CFLAGS) -c $< -o $@

$(MINGW_AMD64_UTF8PROC_OBJECT): $(UTF8PROC_SOURCE) deps/utf8proc/utf8proc.h | $(MINGW_AMD64_BUILD_DIR)/core
	$(MINGW_AMD64_CC) $(CPPFLAGS) -Ideps/utf8proc -DUTF8PROC_STATIC $(MINGW_AMD64_CORE_CPPFLAGS) $(CFLAGS) -std=c99 -c $< -o $@

$(MINGW_I686_UTF8PROC_OBJECT): $(UTF8PROC_SOURCE) deps/utf8proc/utf8proc.h | $(MINGW_I686_BUILD_DIR)/core
	$(MINGW_I686_CC) $(CPPFLAGS) -Ideps/utf8proc -DUTF8PROC_STATIC $(MINGW_I686_CORE_CPPFLAGS) $(CFLAGS) -std=c99 -c $< -o $@

$(MINGW_AMD64_BUILD_DIR)/deps/zlib/%.o: deps/zlib/%.c | $(MINGW_AMD64_BUILD_DIR)/deps/zlib
	$(MINGW_AMD64_CC) $(CPPFLAGS) -Ideps/zlib $(MINGW_AMD64_CORE_CPPFLAGS) $(CFLAGS) -std=c99 -c $< -o $@

$(MINGW_I686_BUILD_DIR)/deps/zlib/%.o: deps/zlib/%.c | $(MINGW_I686_BUILD_DIR)/deps/zlib
	$(MINGW_I686_CC) $(CPPFLAGS) -Ideps/zlib $(MINGW_I686_CORE_CPPFLAGS) $(CFLAGS) -std=c99 -c $< -o $@

$(MINGW_AMD64_BUILD_DIR)/deps/libpng/%.o: deps/libpng/%.c $(PNG_CONFIG) | $(MINGW_AMD64_BUILD_DIR)/deps/libpng
	mkdir -p $(@D)
	$(MINGW_AMD64_CC) $(CPPFLAGS) $(DEPENDENCY_CPPFLAGS) $(MINGW_AMD64_CORE_CPPFLAGS) $(CFLAGS) -std=c99 -c $< -o $@

$(MINGW_I686_BUILD_DIR)/deps/libpng/%.o: deps/libpng/%.c $(PNG_CONFIG) | $(MINGW_I686_BUILD_DIR)/deps/libpng
	mkdir -p $(@D)
	$(MINGW_I686_CC) $(CPPFLAGS) $(DEPENDENCY_CPPFLAGS) $(MINGW_I686_CORE_CPPFLAGS) $(CFLAGS) -std=c99 -c $< -o $@

$(MINGW_AMD64_UTF8PROC_OBJECT) $(MINGW_I686_UTF8PROC_OBJECT) \
$(MINGW_AMD64_ZLIB_OBJECTS) $(MINGW_I686_ZLIB_OBJECTS) \
$(MINGW_AMD64_LIBPNG_OBJECTS) $(MINGW_I686_LIBPNG_OBJECTS): Makefile

$(MINGW_AMD64_CORE_OBJECTS) $(MINGW_I686_CORE_OBJECTS): \
	$(CORE_HEADERS) $(PNG_CONFIG) Makefile

$(UI_OBJECTS) $(MINGW_AMD64_UI_OBJECTS) $(MINGW_I686_UI_OBJECTS): \
	include/boxrig/ui.h include/boxrig/ui/view.h include/boxrig/ui/color.h \
	include/boxrig/ui/composition.h include/boxrig/ui/image.h \
	include/boxrig/ui/input.h include/boxrig/ui/input_text.h \
	include/boxrig/ui/lifecycle.h include/boxrig/ui/notification.h \
	include/boxrig/ui/scrollbar.h include/boxrig/ui/text_layout.h \
	src/ui/color_private.h src/ui/renderer_lifecycle.h src/ui/utf8.h \
	src/ui/view_private.h src/ui/blink_private.h $(UI_HEADERS) Makefile

$(UI_OBJECTS) $(MINGW_AMD64_UI_OBJECTS) $(MINGW_I686_UI_OBJECTS): src/ui/accessible.h src/ui/glyph_fit.h

$(SESSION_OBJECTS) $(MINGW_AMD64_SESSION_OBJECTS) \
$(MINGW_I686_SESSION_OBJECTS): include/boxrig/session.h \
	$(CORE_PUBLIC_HEADERS) Makefile

$(POSIX_OBJECTS): include/boxrig/posix.h Makefile

$(BUILD_DIR)/ui/input.o $(MINGW_AMD64_BUILD_DIR)/ui/input.o \
$(MINGW_I686_BUILD_DIR)/ui/input.o $(BUILD_DIR)/gtk3/input.o \
$(BUILD_DIR)/gtk4/input.o $(BUILD_DIR)/x11/input.o $(WAYLAND_OBJECTS) \
$(MINGW_AMD64_BUILD_DIR)/win32/input.o $(MINGW_I686_BUILD_DIR)/win32/input.o \
$(MINGW_AMD64_INPUT_TEST) $(MINGW_I686_INPUT_TEST): src/ui/input_control.h

$(X11_OBJECTS): $(X11_HEADERS) $(UI_HEADERS) include/boxrig/terminal.h \
	src/ui/view_private.h src/ui/color_private.h \
	include/boxrig/posix.h src/ui/renderer_lifecycle.h src/ui/utf8.h \
	src/x11/component_private.h src/x11/input_private.h src/x11/renderer_private.h src/pangocairo/glyph_fit.h src/ui/glyph_fit.h Makefile

$(OPENGL_RENDERER_OBJECTS): $(OPENGL_RENDERER_HEADERS) $(UI_HEADERS) \
	src/ui/view_private.h src/ui/color_private.h src/ui/glyph_fit.h src/pangocairo/glyph_fit.h src/opengl/renderer.h src/opengl/runtime.h src/opengl/text_provider.h Makefile

$(X11_OPENGL_OBJECTS): $(X11_HEADERS) $(UI_HEADERS) $(OPENGL_RENDERER_HEADERS) \
	src/x11/component_private.h src/opengl/renderer.h src/opengl/runtime.h \
	src/opengl/text_provider.h \
	src/ui/renderer_lifecycle.h Makefile

$(WAYLAND_OPENGL_OBJECTS): $(WAYLAND_HEADERS) $(WAYLAND_OPENGL_HEADERS) \
	$(UI_HEADERS) $(OPENGL_RENDERER_HEADERS) $(CORE_PUBLIC_HEADERS) Makefile

$(GTK4_OBJECTS): $(GTK4_HEADERS) $(UI_HEADERS) include/boxrig/terminal.h \
	src/pangocairo/renderer.h src/pangocairo/renderer_private.h src/pangocairo/glyph_fit.h src/ui/glyph_fit.h \
	src/ui/renderer_lifecycle.h Makefile

$(GTK3_OBJECTS): $(GTK3_HEADERS) $(UI_HEADERS) include/boxrig/terminal.h \
	src/pangocairo/renderer.h src/pangocairo/renderer_private.h src/pangocairo/glyph_fit.h src/ui/glyph_fit.h \
	src/ui/renderer_lifecycle.h Makefile

$(filter $(BUILD_DIR)/gtk3/% $(BUILD_DIR)/gtk4/%,$(GTK3_OBJECTS) $(GTK4_OBJECTS)): $(GTK_RENDER_CONFIG)

.PHONY: force-gtk-render-config
force-gtk-render-config:

$(GTK_RENDER_CONFIG): $(GTK_RENDER_CONFIG_PREREQUISITE) | $(BUILD_DIR)
	@printf '%s\n' '$(GTK_OPENGL)' > $@

$(WAYLAND_OBJECTS): $(WAYLAND_HEADERS) $(UI_HEADERS) \
	include/boxrig/terminal.h src/pangocairo/renderer.h \
	src/pangocairo/renderer_private.h src/pangocairo/glyph_fit.h src/ui/glyph_fit.h src/ui/renderer_lifecycle.h \
	src/ui/utf8.h Makefile

$(MINGW_AMD64_WIN32_OBJECTS) $(MINGW_I686_WIN32_OBJECTS): \
	$(WIN32_HEADERS) $(UI_HEADERS) include/boxrig/terminal.h \
	src/ui/view_private.h src/ui/color_private.h \
	src/win32/host_private.h src/win32/renderer_private.h src/win32/system_library.h src/win32/dpi.h src/win32/transparency_private.h $(CONPTY_HEADER) Makefile

$(TEST_BINARY): tests/test_terminal.c $(CORE_SOURCES) $(CORE_HEADERS) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) tests/test_terminal.c $(CORE_SOURCES) $(LIBRARY) $(SANITIZERS) -o $@

$(TEXT_MEASUREMENT_TEXT_OBJECT): src/core/text.c $(CORE_HEADERS) | \
		$(TEXT_MEASUREMENT_BUILD_DIR)
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(POSIX_CPPFLAGS) $(CFLAGS) \
		$(PROJECT_CFLAGS) -DBOXRIG_MEASURE_TEXT_WORKLOADS -c $< -o $@

$(TEXT_MEASUREMENT_RECOGNIZER_OBJECT): src/core/recognizer.c \
		$(CORE_HEADERS) | $(TEXT_MEASUREMENT_BUILD_DIR)
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(POSIX_CPPFLAGS) $(CFLAGS) \
		$(PROJECT_CFLAGS) -DBOXRIG_MEASURE_TEXT_WORKLOADS -c $< -o $@

$(TEXT_MEASUREMENT_GRAPHICS_OBJECT): src/core/graphics_protocol.c \
		$(CORE_HEADERS) | $(TEXT_MEASUREMENT_BUILD_DIR)
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(POSIX_CPPFLAGS) $(CFLAGS) \
		$(PROJECT_CFLAGS) -DBOXRIG_MEASURE_TEXT_WORKLOADS -c $< -o $@

$(TEXT_MEASUREMENT_INTERPRETER_OBJECT): src/core/interpreter.c \
		$(CORE_HEADERS) | $(TEXT_MEASUREMENT_BUILD_DIR)
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(POSIX_CPPFLAGS) $(CFLAGS) \
		$(PROJECT_CFLAGS) -DBOXRIG_MEASURE_TEXT_WORKLOADS -c $< -o $@

$(TEXT_MEASUREMENT_TERMINAL_OBJECT): src/core/terminal.c \
		$(CORE_HEADERS) | $(TEXT_MEASUREMENT_BUILD_DIR)
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(POSIX_CPPFLAGS) $(CFLAGS) \
		$(PROJECT_CFLAGS) -DBOXRIG_MEASURE_TEXT_WORKLOADS -c $< -o $@

$(TEXT_MEASUREMENT_SCREEN_OBJECT): src/core/screen.c \
		$(CORE_HEADERS) | $(TEXT_MEASUREMENT_BUILD_DIR)
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(POSIX_CPPFLAGS) $(CFLAGS) \
		$(PROJECT_CFLAGS) -DBOXRIG_MEASURE_TEXT_WORKLOADS -c $< -o $@

$(TEXT_MEASUREMENT_LIBRARY): $(TEXT_MEASUREMENT_CORE_OBJECTS) \
		$(UTF8PROC_OBJECT) $(LIBPNG_OBJECTS) $(ZLIB_OBJECTS) | \
		$(TEXT_MEASUREMENT_BUILD_DIR)
	$(RM) $@
	$(AR) rcs $@ $^

$(TEXT_WORKLOAD_MEASUREMENT_BINARY): tests/measure_text_workloads.c \
		$(TEXT_MEASUREMENT_LIBRARY) | $(TEXT_MEASUREMENT_BUILD_DIR)
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(POSIX_CPPFLAGS) $(CFLAGS) \
		$(PROJECT_CFLAGS) -DBOXRIG_MEASURE_TEXT_WORKLOADS $< \
		$(TEXT_MEASUREMENT_LIBRARY) -Wl,--wrap=malloc \
		-Wl,--wrap=calloc -Wl,--wrap=realloc -Wl,--wrap=free \
		-Wl,--wrap=memmove -o $@

$(TEXT_MEASUREMENT_BUILD_DIR)/measure_text_workloads_unwrapped: \
		tests/measure_text_workloads.c $(TEXT_MEASUREMENT_LIBRARY) | \
		$(TEXT_MEASUREMENT_BUILD_DIR)
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(POSIX_CPPFLAGS) $(CFLAGS) \
		$(PROJECT_CFLAGS) -DBOXRIG_MEASURE_TEXT_WORKLOADS \
		-DBOXRIG_MEASURE_UNWRAPPED $< $(TEXT_MEASUREMENT_LIBRARY) -o $@

$(PARSER_TEST_BINARY): tests/test_parser.c src/core/parser.c src/core/parser.h | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) tests/test_parser.c src/core/parser.c $(SANITIZERS) -o $@

$(GRAPHICS_TEST_BINARY): tests/test_graphics.c $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) $< $(LIBRARY) $(SANITIZERS) -o $@

$(SCREEN_TEST_BINARY): tests/test_screen.c src/core/screen.c src/core/text.c src/core/regex.c src/core/unicode.c $(UTF8PROC_SOURCE) $(CORE_HEADERS) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) tests/test_screen.c src/core/screen.c src/core/text.c src/core/regex.c src/core/unicode.c $(UTF8PROC_SOURCE) $(SANITIZERS) -o $@

$(TEXT_LAYOUT_TEST_BINARY): tests/test_text_layout.c $(UI_LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) tests/test_text_layout.c $(UI_LIBRARY) $(SANITIZERS) -o $@

$(SEARCH_TRANSFORM_PROTOTYPE_TEST_BINARY): \
		tests/test_search_transform_prototype.c \
		tests/search_transform_prototype.c \
		tests/search_transform_prototype.h $(UTF8PROC_OBJECT) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) \
		$(SANITIZERS) tests/test_search_transform_prototype.c \
		tests/search_transform_prototype.c $(UTF8PROC_OBJECT) \
		$(SANITIZERS) -o $@

$(COMPOSITION_TEST_BINARY): tests/test_composition.c $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) \
		$(SANITIZERS) tests/test_composition.c $(UI_LIBRARY) $(LIBRARY) \
		-Wl,--wrap=malloc -Wl,--wrap=calloc $(SANITIZERS) -o $@

$(UTF8_TEST_BINARY): tests/test_utf8.c src/ui/utf8.c src/ui/utf8.h | \
		$(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) -Isrc/ui $(CFLAGS) \
		$(PROJECT_CFLAGS) $(SANITIZERS) tests/test_utf8.c src/ui/utf8.c \
		$(SANITIZERS) -o $@

$(IMAGE_COMPOSITOR_TEST_BINARY): tests/test_image_compositor.c $(UI_LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) tests/test_image_compositor.c $(UI_LIBRARY) $(SANITIZERS) -o $@

$(UI_LIFECYCLE_TEST_BINARY): tests/test_ui_lifecycle.c $(UI_LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) tests/test_ui_lifecycle.c $(UI_LIBRARY) $(SANITIZERS) -o $@

$(RENDERER_LIFECYCLE_TEST_BINARY): tests/test_renderer_lifecycle.c \
		$(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) -Isrc $(CFLAGS) \
		$(PROJECT_CFLAGS) $(SANITIZERS) tests/test_renderer_lifecycle.c \
		$(UI_LIBRARY) $(LIBRARY) $(SANITIZERS) -o $@

$(OPENGL_RUNTIME_TEST_BINARY): tests/test_opengl_runtime.c \
		tests/opengl_host.c tests/opengl_host.h src/opengl/runtime.c \
		src/opengl/runtime.h | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) -Isrc -Itests $(POSIX_CPPFLAGS) $(OPENGL_CPPFLAGS) \
		$(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) tests/test_opengl_runtime.c \
		tests/opengl_host.c src/opengl/runtime.c $(OPENGL_LIBS) \
		$(SANITIZERS) -o $@

$(OPENGL_RENDERER_FIXTURE_TEST_BINARY): tests/test_ui_platform_fixture.c \
		tests/opengl_host.c tests/opengl_host.h src/opengl/renderer.h \
		$(OPENGL_RENDERER_LIBRARY) $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) -DBOXRIG_TEST_FRONTEND_OPENGL=1 -Isrc -Itests \
		$(PROJECT_CPPFLAGS) $(POSIX_CPPFLAGS) $(OPENGL_RENDERER_CPPFLAGS) \
		$(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) \
		tests/test_ui_platform_fixture.c tests/opengl_host.c \
		$(OPENGL_RENDERER_LIBRARY) $(UI_LIBRARY) $(LIBRARY) \
		$(OPENGL_RENDERER_LIBS) $(SANITIZERS) -o $@

$(UI_TEST_BINARY): tests/test_ui.c $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) tests/test_ui.c $(UI_LIBRARY) $(LIBRARY) -Wl,--wrap=malloc $(SANITIZERS) -o $@

$(ANNOTATION_COST_BINARY): ANNOTATION_COST_FLAGS = \
	-Wl,--wrap=boxrig_terminal_get_text_fragment
$(ANNOTATION_COST_UNWRAPPED_BINARY): ANNOTATION_COST_FLAGS = \
	-DBOXRIG_ANNOTATION_UNWRAPPED
$(ANNOTATION_COST_BINARY) $(ANNOTATION_COST_UNWRAPPED_BINARY): \
		tests/performance/annotation_cost.c $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests/performance
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(POSIX_CPPFLAGS) $(CFLAGS) \
		$(PROJECT_CFLAGS) $< $(UI_LIBRARY) $(LIBRARY) \
		$(ANNOTATION_COST_FLAGS) -o $@

.PHONY: test-annotation-cost
test: test-annotation-cost
test-annotation-cost: $(ANNOTATION_COST_BINARY) $(ANNOTATION_COST_UNWRAPPED_BINARY)
	$(ANNOTATION_COST_BINARY) self-test
	$(ANNOTATION_COST_UNWRAPPED_BINARY) self-test
	$(ANNOTATION_COST_BINARY) 16 10 45 2 bound
	$(ANNOTATION_COST_BINARY) 10000 10 45 2 bound

$(BUILD_DIR)/tests/performance/selection_cost: \
		tests/performance/selection_cost.c $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests/performance
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(POSIX_CPPFLAGS) $(CFLAGS) \
		$(PROJECT_CFLAGS) $< $(UI_LIBRARY) $(LIBRARY) \
		-Wl,--wrap=boxrig_terminal_get_text_fragment \
		-Wl,--wrap=boxrig_terminal_create_text_snapshot -o $@

.PHONY: test-selection-cost
test: test-selection-cost
test-selection-cost: $(BUILD_DIR)/tests/performance/selection_cost
	$<

$(BUILD_DIR)/tests/performance/selection_cost_cairo: \
		tests/performance/selection_cost.c src/pangocairo/renderer.c \
		$(PANGO_CANDIDATE_OBJECT) $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests/performance
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) -Isrc $(POSIX_CPPFLAGS) \
		$(PANGO_CAIRO_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) \
		-DBOXRIG_SELECTION_COST_CAIRO $< src/pangocairo/renderer.c \
		$(PANGO_CANDIDATE_OBJECT) $(UI_LIBRARY) $(LIBRARY) \
		$(PANGO_CAIRO_LIBS) -Wl,--wrap=boxrig_terminal_get_text_fragment \
		-Wl,--wrap=boxrig_terminal_create_text_snapshot -o $@

$(SELECTION_TEST_BINARY): tests/test_selection.c src/ui/word_break.h \
		$(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) -Isrc/ui $(CFLAGS) \
		$(PROJECT_CFLAGS) $(SANITIZERS) tests/test_selection.c \
		$(UI_LIBRARY) $(LIBRARY) $(SANITIZERS) -o $@

.PHONY: test-selection-history test-rectangular-copy
test: test-selection-history test-selection-layout-provider test-rectangular-copy
test-rectangular-copy: $(BUILD_DIR)/tests/test_rectangular_copy
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $<

$(BUILD_DIR)/tests/test_rectangular_copy: tests/test_rectangular_copy.c \
		$(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) \
		$(SANITIZERS) $< $(UI_LIBRARY) $(LIBRARY) $(SANITIZERS) \
		-Wl,--wrap=malloc -Wl,--wrap=realloc -o $@

test-selection-history: $(BUILD_DIR)/tests/test_selection_history
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $<

$(BUILD_DIR)/tests/test_selection_history: tests/test_selection_history.c \
		$(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) \
		-DBOXRIG_TEST_SELECTION_ALLOCATIONS $(SANITIZERS) $< $(UI_LIBRARY) \
		$(LIBRARY) $(SANITIZERS) -Wl,--wrap=malloc -Wl,--wrap=realloc -o $@

.PHONY: test-selection-layout-provider
test-selection-layout-provider: $(BUILD_DIR)/tests/test_selection_layout_provider
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $<

$(BUILD_DIR)/tests/test_selection_layout_provider: tests/test_selection_layout_provider.c \
		$(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) \
		$(SANITIZERS) $< $(UI_LIBRARY) $(LIBRARY) $(SANITIZERS) -o $@

$(FRONTEND_CONFORMANCE_TEST_BINARY): tests/test_frontend_conformance.c \
		$(FRONTEND_CONFORMANCE_MATRIX) $(FRONTEND_COMPONENT_FIXTURE_HEADER) \
		$(FRONTEND_SAMPLE_SCENARIO_HEADER) $(FRONTEND_PUBLIC_CONTRACTS) \
		$(FRONTEND_PUBLIC_HEADERS) | \
		$(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) \
		$(SANITIZERS) tests/test_frontend_conformance.c $(SANITIZERS) -o $@

test: $(FRONTEND_SAMPLE_PROBE_TEST_BINARY) \
	$(FRONTEND_SAMPLE_X11_PROBE_BINARY) $(FRONTEND_SAMPLE_GTK3_PROBE_BINARY) \
	$(FRONTEND_SAMPLE_GTK4_PROBE_BINARY) $(FRONTEND_SAMPLE_WAYLAND_PROBE_BINARY) \
	$(MINGW_AMD64_FRONTEND_SAMPLE_PROBE) $(MINGW_I686_FRONTEND_SAMPLE_PROBE)

$(FRONTEND_SAMPLE_PROBE_DIR)/x11 $(FRONTEND_SAMPLE_PROBE_DIR)/gtk3 \
		$(FRONTEND_SAMPLE_PROBE_DIR)/gtk4 $(FRONTEND_SAMPLE_PROBE_DIR)/wayland:
	mkdir -p $@

$(FRONTEND_SAMPLE_X11_PROBE_BINARY) $(FRONTEND_SAMPLE_GTK3_PROBE_BINARY) \
$(FRONTEND_SAMPLE_GTK4_PROBE_BINARY) $(FRONTEND_SAMPLE_WAYLAND_PROBE_BINARY) \
$(MINGW_AMD64_FRONTEND_SAMPLE_PROBE) $(MINGW_I686_FRONTEND_SAMPLE_PROBE): \
	$(FRONTEND_SAMPLE_SCENARIO_HEADER) \
	examples/boxrig_term/common/runtime_info.h \
	examples/boxrig_term/common/text_policy.h

# These binaries keep the real application and event loop; only the test-only
# linker seam applies overlays, observes lifetimes, and fills the paste queue.
$(FRONTEND_SAMPLE_X11_PROBE_BINARY): $(EXAMPLE_BINARY) \
		$(FRONTEND_SAMPLE_PROBE_SOURCE) Makefile | $(FRONTEND_SAMPLE_PROBE_DIR)/x11
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(POSIX_URI_CPPFLAGS) $(POSIX_CPPFLAGS) \
		$(X11_CPPFLAGS) $(XEXT_CPPFLAGS) $(DBUS_CPPFLAGS) $(CFLAGS) \
		$(PROJECT_CFLAGS) -DBOXRIG_TEST_SAMPLE_X11 \
		$(EXAMPLE_SOURCES) $(FRONTEND_SAMPLE_PROBE_SOURCE) \
		$(X11_LIBRARY) $(POSIX_LIBRARY) $(SESSION_LIBRARY) $(UI_LIBRARY) \
		$(LIBRARY) $(X11_LIBS) $(XEXT_LIBS) $(DBUS_LIBS) \
		$(FRONTEND_SAMPLE_POSIX_PROBE_LDFLAGS) \
		-Wl,--wrap=boxrig_x11_opengl_component_config_default \
		-Wl,--wrap=boxrig_x11_opengl_terminal_component_get_window_requirements \
		-Wl,--wrap=boxrig_x11_opengl_terminal_component_create_on_target \
		-Wl,--wrap=boxrig_x11_terminal_component_create_on_target \
		-Wl,--wrap=boxrig_x11_terminal_component_destroy \
		-Wl,--wrap=boxrig_x11_terminal_component_frame \
		-Wl,--wrap=boxrig_x11_terminal_component_set_key_router \
		-Wl,--wrap=boxrig_x11_terminal_component_set_mouse_router \
		-Wl,--wrap=boxrig_terminal_send_key \
		-Wl,--wrap=boxrig_session_dispatch \
		-Wl,--wrap=boxrig_x11_host_adapter_create \
		-Wl,--wrap=boxrig_x11_host_adapter_destroy \
		-Wl,--wrap=boxrig_x11_input_adapter_quiesce \
		-Wl,--wrap=XConvertSelection \
		-Wl,--wrap=XCloseDisplay $(OPENGL_RENDERER_LIBS) -lutil $(POSIX_URI_LIBS) -o $@

$(FRONTEND_SAMPLE_GTK3_PROBE_BINARY): $(GTK3_EXAMPLE_BINARY) \
		$(FRONTEND_SAMPLE_PROBE_SOURCE) Makefile | $(FRONTEND_SAMPLE_PROBE_DIR)/gtk3
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(POSIX_URI_CPPFLAGS) $(POSIX_CPPFLAGS) \
		$(GTK3_CPPFLAGS) $(CFLAGS) $(GTK3_PROJECT_CFLAGS) \
		-DBOXRIG_TEST_SAMPLE_GTK3 \
		$(GTK3_EXAMPLE_SOURCES) $(FRONTEND_SAMPLE_PROBE_SOURCE) \
		$(GTK3_LIBRARY) $(NOTIFICATION_LIBRARY) $(POSIX_LIBRARY) $(SESSION_LIBRARY) $(UI_LIBRARY) \
		$(LIBRARY) $(GTK3_LIBS) $(FRONTEND_SAMPLE_GTK_PROBE_LDFLAGS) \
		-Wl,--wrap=gtk_clipboard_request_contents \
		-Wl,--wrap=boxrig_gtk3_terminal_widget_create \
		-Wl,--wrap=boxrig_gtk3_terminal_widget_take_layout_action \
		-Wl,--wrap=boxrig_gtk3_terminal_widget_complete_size_command \
		-Wl,--wrap=boxrig_gtk3_terminal_widget_complete_grid_resize \
		-Wl,--wrap=boxrig_gtk3_terminal_widget_quiesce -lutil $(POSIX_URI_LIBS) -o $@

$(FRONTEND_SAMPLE_GTK4_PROBE_BINARY): $(GTK4_EXAMPLE_BINARY) \
		$(FRONTEND_SAMPLE_PROBE_SOURCE) Makefile | $(FRONTEND_SAMPLE_PROBE_DIR)/gtk4
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(POSIX_URI_CPPFLAGS) $(POSIX_CPPFLAGS) \
		$(GTK4_CPPFLAGS) $(CFLAGS) $(GTK4_PROJECT_CFLAGS) \
		-DBOXRIG_TEST_SAMPLE_GTK4 \
		$(GTK4_EXAMPLE_SOURCES) $(FRONTEND_SAMPLE_PROBE_SOURCE) \
		$(GTK4_LIBRARY) $(NOTIFICATION_LIBRARY) $(POSIX_LIBRARY) $(SESSION_LIBRARY) $(UI_LIBRARY) \
		$(LIBRARY) $(GTK4_LIBS) $(FRONTEND_SAMPLE_GTK_PROBE_LDFLAGS) \
		-Wl,--wrap=gdk_clipboard_read_async -Wl,--wrap=gdk_clipboard_read_finish \
		-Wl,--wrap=g_input_stream_read_async -Wl,--wrap=g_cancellable_cancel \
		-Wl,--wrap=boxrig_gtk4_terminal_widget_create \
		-Wl,--wrap=boxrig_gtk4_terminal_widget_frame \
		-Wl,--wrap=boxrig_terminal_send_mouse \
		-Wl,--wrap=boxrig_gtk4_terminal_widget_quiesce -lutil $(POSIX_URI_LIBS) -o $@

$(FRONTEND_SAMPLE_WAYLAND_PROBE_BINARY): $(WAYLAND_EXAMPLE_BINARY) \
		$(FRONTEND_SAMPLE_PROBE_SOURCE) tests/frontend_sample_wayland_probe.h \
		$(GTK3_CLIPBOARD_PEER_BINARY) Makefile | $(FRONTEND_SAMPLE_PROBE_DIR)/wayland
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(POSIX_URI_CPPFLAGS) $(POSIX_CPPFLAGS) \
		-I$(WAYLAND_PROTOCOL_DIR) $(WAYLAND_CLIENT_CPPFLAGS) $(WAYLAND_EXAMPLE_CURSOR_CPPFLAGS) \
		$(WAYLAND_EXAMPLE_LIBDECOR_CPPFLAGS) \
		$(PANGO_CAIRO_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) \
		-DBOXRIG_TEST_SAMPLE_WAYLAND -include tests/frontend_sample_wayland_probe.h \
		$(WAYLAND_EXAMPLE_SOURCES) $(FRONTEND_SAMPLE_PROBE_SOURCE) \
		$(WAYLAND_XDG_SHELL_SOURCE) $(WAYLAND_XDG_DECORATION_SOURCE) \
		$(WAYLAND_LIBRARY) $(NOTIFICATION_LIBRARY) $(POSIX_LIBRARY) $(SESSION_LIBRARY) \
		$(UI_LIBRARY) $(LIBRARY) $(WAYLAND_EXAMPLE_PRIVATE_ARCHIVE_LDFLAGS) \
		$(WAYLAND_CLIENT_LIBS) $(WAYLAND_EXAMPLE_CURSOR_LIBS) $(PANGO_CAIRO_LIBS) \
		$(WAYLAND_EXAMPLE_LIBDECOR_LIBS) $(FRONTEND_SAMPLE_POSIX_PROBE_LDFLAGS) \
		-Wl,--wrap=boxrig_wayland_opengl_component_config_default \
		-Wl,--wrap=boxrig_wayland_opengl_terminal_component_create_with_config \
		-Wl,--wrap=boxrig_wayland_terminal_component_create \
		-Wl,--wrap=boxrig_wayland_terminal_component_destroy \
		-Wl,--wrap=boxrig_wayland_terminal_component_set_key_router \
		-Wl,--wrap=boxrig_wayland_terminal_component_set_mouse_router \
		-Wl,--wrap=boxrig_wayland_terminal_component_apply_view_action \
		-Wl,--wrap=close \
		-Wl,--wrap=boxrig_wayland_context_create \
		-Wl,--wrap=boxrig_wayland_context_destroy \
		-Wl,--wrap=wl_event_queue_destroy -Wl,--wrap=wl_display_disconnect \
		$(WAYLAND_OPENGL_LIBS) -lutil $(POSIX_URI_LIBS) -o $@

$(MINGW_AMD64_FRONTEND_SAMPLE_PROBE): $(MINGW_AMD64_EXAMPLE) \
		$(FRONTEND_SAMPLE_PROBE_SOURCE) Makefile
	$(MINGW_AMD64_CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(MINGW_TERM_CPPFLAGS) \
		$(CFLAGS) $(PROJECT_CFLAGS) $(WIN32_EXAMPLE_SOURCES) \
		$(FRONTEND_SAMPLE_PROBE_SOURCE) $(MINGW_AMD64_EXAMPLE_RESOURCE) \
		$(MINGW_AMD64_WIN32_LIBRARY) $(MINGW_AMD64_SESSION_LIBRARY) \
		$(MINGW_AMD64_UI_LIBRARY) $(MINGW_AMD64_LIBRARY) \
		-municode -mwindows \
		$(WIN32_RENDERER_LIBS) $(WIN32_HOST_LIBS) $(WIN32_TRANSPARENCY_LIBS) \
		-ladvapi32 $(FRONTEND_SAMPLE_WIN32_PROBE_LDFLAGS) -lcomctl32 -o $@

$(MINGW_I686_FRONTEND_SAMPLE_PROBE): $(MINGW_I686_EXAMPLE) \
		$(FRONTEND_SAMPLE_PROBE_SOURCE) Makefile
	$(MINGW_I686_CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(MINGW_TERM_CPPFLAGS) \
		$(CFLAGS) $(PROJECT_CFLAGS) $(WIN32_EXAMPLE_SOURCES) \
		$(FRONTEND_SAMPLE_PROBE_SOURCE) $(MINGW_I686_EXAMPLE_RESOURCE) \
		$(MINGW_I686_WIN32_LIBRARY) $(MINGW_I686_SESSION_LIBRARY) \
		$(MINGW_I686_UI_LIBRARY) $(MINGW_I686_LIBRARY) \
		-municode -mwindows \
		$(WIN32_RENDERER_LIBS) $(WIN32_HOST_LIBS) $(WIN32_TRANSPARENCY_LIBS) \
		-ladvapi32 $(FRONTEND_SAMPLE_WIN32_PROBE_LDFLAGS) -lcomctl32 -o $@

$(FRONTEND_SAMPLE_PROBE_TEST_BINARY): tests/test_frontend_sample_probe.c \
		$(FRONTEND_SAMPLE_PROBE_SOURCE) $(FRONTEND_SAMPLE_SCENARIO_HEADER) \
		examples/boxrig_term/common/text_policy.c \
		examples/boxrig_term/common/text_policy.h \
		$(CORE_HEADERS) $(UI_HEADERS) $(POSIX_LIBRARY) $(SESSION_LIBRARY) \
		$(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) \
		$(SANITIZERS) tests/test_frontend_sample_probe.c \
		$(FRONTEND_SAMPLE_PROBE_SOURCE) examples/boxrig_term/common/text_policy.c \
		$(POSIX_LIBRARY) $(SESSION_LIBRARY) $(UI_LIBRARY) $(LIBRARY) \
		$(FRONTEND_SAMPLE_POSIX_PROBE_LDFLAGS) $(SANITIZERS) -lutil -o $@

$(FRONTEND_SAMPLE_SCENARIO_TEST_BINARY): \
		tests/test_frontend_sample_scenario.c \
		$(FRONTEND_SAMPLE_SCENARIO_SOURCE) \
		$(FRONTEND_SAMPLE_SCENARIO_HEADER) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(POSIX_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) \
		$(SANITIZERS) tests/test_frontend_sample_scenario.c \
		$(FRONTEND_SAMPLE_SCENARIO_SOURCE) $(SANITIZERS) -o $@

$(FRONTEND_SAMPLE_X11_TEST_BINARY): \
		tests/x11_video.h \
		tests/x11/test_frontend_sample_scenario.c \
		$(FRONTEND_SAMPLE_SCENARIO_HEADER) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(POSIX_CPPFLAGS) $(X11_CPPFLAGS) \
		$(XTEST_CPPFLAGS) $(XEXT_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) \
		tests/x11/test_frontend_sample_scenario.c \
		$(X11_LIBS) $(XTEST_LIBS) $(XEXT_LIBS) $(SANITIZERS) -o $@

$(UI_PLATFORM_FIXTURE_TEST_BINARY): tests/test_ui_platform_fixture.c \
		src/ui/renderer_lifecycle.h src/x11/renderer_private.h src/pangocairo/glyph_fit.h src/ui/glyph_fit.h \
		$(X11_LIBRARY) $(POSIX_LIBRARY) $(SESSION_LIBRARY) $(UI_LIBRARY) \
		$(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) -Isrc $(POSIX_CPPFLAGS) \
		$(X11_CPPFLAGS) $(DBUS_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) \
		$(SANITIZERS) -DBOXRIG_TEST_FRONTEND_X11 $< \
		$(X11_LIBRARY) $(POSIX_LIBRARY) $(SESSION_LIBRARY) $(UI_LIBRARY) \
		$(LIBRARY) \
		$(X11_LIBS) $(DBUS_LIBS) -lutil $(SANITIZERS) -o $@

$(PANGO_CAIRO_FIXTURE_TEST_BINARY): tests/test_ui_platform_fixture.c \
		src/ui/view_private.h src/ui/color_private.h \
		src/pangocairo/renderer.c $(PANGO_CANDIDATE_OBJECT) src/pangocairo/renderer.h \
		src/pangocairo/renderer_private.h src/pangocairo/glyph_fit.h src/ui/glyph_fit.h src/ui/renderer_lifecycle.h \
		$(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) -Isrc $(POSIX_CPPFLAGS) \
		$(PANGO_CAIRO_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) \
		-DBOXRIG_TEST_FRONTEND_PANGO_CAIRO $< \
		src/pangocairo/renderer.c $(PANGO_CANDIDATE_OBJECT) $(UI_LIBRARY) $(LIBRARY) \
		$(PANGO_CAIRO_LIBS) $(SANITIZERS) -o $@

$(X11_COMPONENT_FIXTURE_TEST_BINARY): \
		tests/x11/test_x11_component_fixture.c \
		$(FRONTEND_COMPONENT_FIXTURE_SOURCES) \
		$(FRONTEND_COMPONENT_FIXTURE_HEADER) $(X11_LIBRARY) $(UI_LIBRARY) \
		$(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(POSIX_CPPFLAGS) \
		$(X11_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) \
		tests/x11/test_x11_component_fixture.c \
		$(FRONTEND_COMPONENT_FIXTURE_SOURCES) \
		$(X11_LIBRARY) $(UI_LIBRARY) $(LIBRARY) $(X11_LIBS) \
		$(DBUS_LIBS) $(SANITIZERS) -o $@

$(X11_OPENGL_COMPONENT_FIXTURE_TEST_BINARY): \
		tests/x11/test_x11_component_fixture.c \
		$(FRONTEND_COMPONENT_FIXTURE_SOURCES) \
		$(FRONTEND_COMPONENT_FIXTURE_HEADER) \
		$(X11_OPENGL_LIBRARY) $(X11_LIBRARY) $(OPENGL_RENDERER_LIBRARY) \
		$(UI_LIBRARY) $(LIBRARY) | \
		$(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) -DBOXRIG_TEST_X11_OPENGL=1 -Isrc \
		$(PROJECT_CPPFLAGS) $(POSIX_CPPFLAGS) $(X11_CPPFLAGS) \
		$(OPENGL_RENDERER_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) \
		tests/x11/test_x11_component_fixture.c \
		$(FRONTEND_COMPONENT_FIXTURE_SOURCES) \
		$(X11_OPENGL_LIBRARY) $(X11_LIBRARY) $(OPENGL_RENDERER_LIBRARY) \
		$(UI_LIBRARY) $(LIBRARY) \
		$(X11_LIBS) $(DBUS_LIBS) $(OPENGL_RENDERER_LIBS) \
		$(SANITIZERS) -o $@

$(X11_OPENGL_COMPONENT_TEST_BINARY): \
		tests/x11/test_x11_opengl_component.c \
		$(X11_OPENGL_LIBRARY) $(X11_LIBRARY) $(OPENGL_RENDERER_LIBRARY) \
		$(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) -Isrc $(PROJECT_CPPFLAGS) $(POSIX_CPPFLAGS) \
		$(X11_CPPFLAGS) $(OPENGL_RENDERER_CPPFLAGS) $(CFLAGS) \
		$(PROJECT_CFLAGS) $(SANITIZERS) $< \
		$(X11_OPENGL_LIBRARY) $(X11_LIBRARY) $(OPENGL_RENDERER_LIBRARY) \
		$(UI_LIBRARY) $(LIBRARY) \
		$(X11_LIBS) $(DBUS_LIBS) $(OPENGL_RENDERER_LIBS) \
		-Wl,--wrap=eglCreateContext -Wl,--wrap=eglMakeCurrent \
		-Wl,--wrap=eglCreateWindowSurface -Wl,--wrap=eglDestroySurface \
		-Wl,--wrap=eglQuerySurface \
		-Wl,--wrap=eglSwapBuffers -Wl,--wrap=eglGetError \
		-Wl,--wrap=eglGetConfigAttrib -Wl,--wrap=eglGetProcAddress \
		-Wl,--wrap=FcConfigParseAndLoad \
		$(SANITIZERS) -o $@

$(GTK4_WIDGET_TEST_BINARY): tests/gtk4/test_gtk4_widget.c \
		$(FRONTEND_COMPONENT_FIXTURE_SOURCES) \
		$(FRONTEND_COMPONENT_FIXTURE_HEADER) $(GTK4_LIBRARY) $(UI_LIBRARY) \
		$(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(POSIX_CPPFLAGS) \
		$(GTK4_CPPFLAGS) $(X11_CPPFLAGS) $(CFLAGS) $(GTK4_PROJECT_CFLAGS) $(SANITIZERS) \
		tests/gtk4/test_gtk4_widget.c $(FRONTEND_COMPONENT_FIXTURE_SOURCES) \
		$(GTK4_LIBRARY) $(UI_LIBRARY) $(LIBRARY) $(GTK4_LIBS) $(X11_LIBS) \
		$(SANITIZERS) \
		-Wl,--wrap=gtk_event_controller_get_current_event_state \
		-Wl,--wrap=gtk_im_context_set_cursor_location,--wrap=gtk_im_multicontext_new -o $@

$(GTK3_WIDGET_TEST_BINARY): tests/gtk3/test_gtk3_widget.c \
		$(FRONTEND_COMPONENT_FIXTURE_SOURCES) \
		$(FRONTEND_COMPONENT_FIXTURE_HEADER) include/boxrig/gtk3.h \
		$(GTK3_LIBRARY) $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(POSIX_CPPFLAGS) \
		$(GTK3_CPPFLAGS) $(X11_CPPFLAGS) $(CFLAGS) $(GTK3_PROJECT_CFLAGS) $(SANITIZERS) \
		tests/gtk3/test_gtk3_widget.c $(FRONTEND_COMPONENT_FIXTURE_SOURCES) \
		$(GTK3_LIBRARY) $(UI_LIBRARY) $(LIBRARY) $(GTK3_LIBS) $(X11_LIBS) \
		-Wl,--wrap=cairo_image_surface_create \
		-Wl,--wrap=gtk_im_context_set_cursor_location $(SANITIZERS) -o $@

$(GTK4_SAMPLE_TEST_BINARY): tests/gtk4/test_gtk4_sample.c \
		examples/boxrig_term/gtk4/event_source_policy.c \
		examples/boxrig_term/gtk4/event_source_policy.h \
		| $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(POSIX_CPPFLAGS) $(X11_CPPFLAGS) \
		$(XTEST_CPPFLAGS) $(XDIRTY_CPPFLAGS) \
		-Iexamples/boxrig_term/gtk4 $(CFLAGS) $(PROJECT_CFLAGS) \
		$(SANITIZERS) tests/gtk4/test_gtk4_sample.c \
		examples/boxrig_term/gtk4/event_source_policy.c $(X11_LIBS) \
		$(XTEST_LIBS) $(XDIRTY_LIBS) $(SANITIZERS) -o $@

$(GTK3_SAMPLE_TEST_BINARY): tests/gtk3/test_gtk3_sample.c \
		examples/boxrig_term/gtk3/event_source_policy.c \
		examples/boxrig_term/gtk3/event_source_policy.h \
		| $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(POSIX_CPPFLAGS) $(X11_CPPFLAGS) \
		$(XTEST_CPPFLAGS) $(XDIRTY_CPPFLAGS) \
		-Iexamples/boxrig_term/gtk3 $(CFLAGS) $(PROJECT_CFLAGS) \
		$(SANITIZERS) tests/gtk3/test_gtk3_sample.c \
		examples/boxrig_term/gtk3/event_source_policy.c $(X11_LIBS) \
		$(XTEST_LIBS) $(XDIRTY_LIBS) $(SANITIZERS) -o $@

$(GTK3_WAYLAND_SAMPLE_CHILD_BINARY): \
		$(GTK3_WAYLAND_SAMPLE_CHILD_SOURCE) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(POSIX_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) \
		$(SANITIZERS) $< $(SANITIZERS) -o $@

$(WAYLAND_CONTEXT_TEST_BINARY): tests/wayland/test_wayland_context.c tests/editable_fixture.h $(UI_LIBRARY) $(LIBRARY) \
        src/wayland/input.c src/wayland/ibus.c $(IME_SOURCES) $(IME_HEADERS) $(WAYLAND_PRIVATE_HEADERS) \
		$(FRONTEND_COMPONENT_FIXTURE_SOURCES) \
		$(FRONTEND_COMPONENT_FIXTURE_HEADER) \
		$(WAYLAND_HEADERS) $(WAYLAND_LIBRARY) $(WAYLAND_VIEWPORTER_SERVER_HEADER) \
		$(WAYLAND_FRACTIONAL_SCALE_SERVER_HEADER) $(WAYLAND_TEXT_INPUT_SERVER_HEADER) \
		$(WAYLAND_PRIMARY_SELECTION_SERVER_HEADER) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(POSIX_CPPFLAGS) \
		-I$(WAYLAND_PROTOCOL_DIR) $(WAYLAND_CPPFLAGS) $(PANGO_CAIRO_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) \
		-Isrc/ui tests/wayland/test_wayland_context.c src/wayland/input.c src/wayland/ibus.c $(IME_SOURCES) \
		$(FRONTEND_COMPONENT_FIXTURE_SOURCES) \
		$(WAYLAND_LIBRARY) $(UI_LIBRARY) $(LIBRARY) $(WAYLAND_LIBS) \
		$(PANGO_CAIRO_LIBS) $(SANITIZERS) -Wl,--wrap=boxrig_ui_editable_document_apply_edit,--wrap=boxrig_composition_apply_event -o $@

$(WAYLAND_WESTON_TEST_BINARY): $(WAYLAND_WESTON_TEST_SOURCE) \
		$(WAYLAND_HEADERS) $(WAYLAND_LIBRARY) $(WAYLAND_XDG_SHELL_HEADER) \
		$(WAYLAND_XDG_SHELL_SOURCE) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(POSIX_CPPFLAGS) \
		-I$(WAYLAND_PROTOCOL_DIR) $(WAYLAND_CPPFLAGS) $(PANGO_CAIRO_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) \
		$(SANITIZERS) $(WAYLAND_WESTON_TEST_SOURCE) $(WAYLAND_XDG_SHELL_SOURCE) \
		$(WAYLAND_LIBRARY) $(UI_LIBRARY) $(LIBRARY) $(WAYLAND_LIBS) \
		$(PANGO_CAIRO_LIBS) $(SANITIZERS) -Wl,--wrap=boxrig_wayland_shm_pool_copy_cairo,--wrap=boxrig_wayland_shm_buffer_copy_cairo -o $@

$(WAYLAND_OPENGL_WESTON_TEST_BINARY): $(WAYLAND_WESTON_TEST_SOURCE) \
		include/boxrig/wayland/opengl_component.h $(WAYLAND_HEADERS) \
		$(WAYLAND_OPENGL_LIBRARY) $(WAYLAND_LIBRARY) \
		$(OPENGL_RENDERER_LIBRARY) $(WAYLAND_XDG_SHELL_HEADER) \
		$(WAYLAND_XDG_SHELL_SOURCE) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(POSIX_CPPFLAGS) \
		-DBOXRIG_TEST_WAYLAND_OPENGL=1 -Isrc -I$(WAYLAND_PROTOCOL_DIR) \
		$(WAYLAND_CPPFLAGS) $(WAYLAND_OPENGL_CPPFLAGS) \
		$(PANGO_CAIRO_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) \
		$(WAYLAND_WESTON_TEST_SOURCE) $(WAYLAND_XDG_SHELL_SOURCE) \
		$(WAYLAND_OPENGL_LIBRARY) $(WAYLAND_LIBRARY) \
		$(OPENGL_RENDERER_LIBRARY) $(UI_LIBRARY) $(LIBRARY) \
		$(WAYLAND_LIBS) $(WAYLAND_OPENGL_LIBS) $(PANGO_CAIRO_LIBS) \
		$(SANITIZERS) -Wl,--wrap=boxrig_ogl_renderer_create,--wrap=eglCreateWindowSurface,--wrap=eglSwapBuffers,--wrap=glBlitFramebuffer -o $@

$(WAYLAND_X11_PIXEL_PROBE_BINARY): $(WAYLAND_X11_PIXEL_PROBE_SOURCE) \
		| $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(X11_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) \
		$< $(X11_LIBS) -o $@

$(WAYLAND_ROOT_POLICY_TEST_BINARY): \
		tests/wayland/test_root_backing_policy.c \
		examples/boxrig_term/wayland/root_backing_policy.c \
		examples/boxrig_term/wayland/root_backing_policy.h | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(POSIX_CPPFLAGS) \
		-Iexamples/boxrig_term/wayland $(CFLAGS) $(PROJECT_CFLAGS) \
		$(SANITIZERS) tests/wayland/test_root_backing_policy.c \
		examples/boxrig_term/wayland/root_backing_policy.c \
		$(SANITIZERS) -o $@

$(WAYLAND_SHORTCUT_POLICY_TEST_BINARY): \
		tests/wayland/test_shortcut_policy.c \
		examples/boxrig_term/wayland/shortcut_policy.c \
		examples/boxrig_term/wayland/shortcut_policy.h $(UI_LIBRARY) $(LIBRARY) \
		| $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(POSIX_CPPFLAGS) \
		-Iexamples/boxrig_term/wayland $(CFLAGS) $(PROJECT_CFLAGS) \
		$(SANITIZERS) tests/wayland/test_shortcut_policy.c \
		examples/boxrig_term/wayland/shortcut_policy.c $(UI_LIBRARY) \
		$(LIBRARY) $(SANITIZERS) -o $@

$(WAYLAND_DECORATION_POLICY_TEST_BINARY): \
		tests/wayland/test_decoration_policy.c \
		examples/boxrig_term/wayland/decoration_policy.c \
		examples/boxrig_term/wayland/decoration_policy.h | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(POSIX_CPPFLAGS) \
		-Iexamples/boxrig_term/wayland $(CFLAGS) $(PROJECT_CFLAGS) \
		$(SANITIZERS) tests/wayland/test_decoration_policy.c \
		examples/boxrig_term/wayland/decoration_policy.c \
		$(SANITIZERS) -o $@

$(GTK3_CLIPBOARD_PEER_BINARY): $(GTK3_CLIPBOARD_PEER_SOURCE) | $(BUILD_DIR)/tests
	$(CC) $(GTK3_CPPFLAGS) $(CFLAGS) $(GTK3_PROJECT_CFLAGS) \
		$< $(GTK3_LIBS) -o $@

$(SESSION_TEST_BINARY): tests/test_session.c $(SESSION_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) tests/test_session.c $(SESSION_LIBRARY) $(LIBRARY) $(SANITIZERS) -o $@

$(POSIX_SESSION_TEST_BINARY): tests/test_posix_session.c $(POSIX_LIBRARY) \
		$(SESSION_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(POSIX_CPPFLAGS) \
		$(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) $< $(POSIX_LIBRARY) \
		$(SESSION_LIBRARY) $(LIBRARY) -lutil $(SANITIZERS) -o $@

$(POSIX_EXAMPLE_COMMON_TEST_BINARY): tests/test_posix_example_common.c \
		examples/boxrig_term/common/posix_application.c \
		examples/boxrig_term/common/posix_application.h $(POSIX_LIBRARY) \
		$(SESSION_LIBRARY) $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(POSIX_CPPFLAGS) \
		-Iexamples/boxrig_term/common $(CFLAGS) $(PROJECT_CFLAGS) \
		$(SANITIZERS) tests/test_posix_example_common.c \
		examples/boxrig_term/common/posix_application.c $(POSIX_LIBRARY) \
		$(SESSION_LIBRARY) $(UI_LIBRARY) $(LIBRARY) -lutil $(SANITIZERS) -o $@

.PHONY: test-tty-erase
test: test-tty-erase
test-tty-erase: $(BUILD_DIR)/tests/test_posix_tty_erase
	$(BUILD_DIR)/tests/test_posix_tty_erase

$(BUILD_DIR)/tests/test_posix_tty_erase: tests/test_posix_tty_erase.c \
		examples/boxrig_term/common/posix_application.c \
		examples/boxrig_term/common/posix_application.h $(POSIX_LIBRARY) \
		$(SESSION_LIBRARY) $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(POSIX_CPPFLAGS) \
		-Iexamples/boxrig_term/common $(CFLAGS) $(PROJECT_CFLAGS) \
		$(SANITIZERS) $< examples/boxrig_term/common/posix_application.c \
		$(POSIX_LIBRARY) $(SESSION_LIBRARY) $(UI_LIBRARY) $(LIBRARY) \
		-lutil $(SANITIZERS) -o $@

.PHONY: test-input-control
test: test-input-control
test-input-control: $(BUILD_DIR)/tests/test_ui_input_control
	$(BUILD_DIR)/tests/test_ui_input_control

$(BUILD_DIR)/tests/test_ui_input_control: tests/test_ui_input_control.c $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) $< $(UI_LIBRARY) $(LIBRARY) $(SANITIZERS) -o $@

$(EXAMPLE_RUNTIME_INFO_TEST_BINARY): tests/test_example_runtime_info.c \
		examples/boxrig_term/common/runtime_info.c \
		examples/boxrig_term/common/runtime_info.h | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) \
		$(SANITIZERS) tests/test_example_runtime_info.c \
		examples/boxrig_term/common/runtime_info.c $(SANITIZERS) -o $@

$(EXAMPLE_TEXT_POLICY_TEST_BINARY): tests/test_example_text_policy.c \
		examples/boxrig_term/common/text_policy.c \
		examples/boxrig_term/common/text_policy.h $(UI_LIBRARY) $(LIBRARY) | \
		$(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) \
		-Iexamples/boxrig_term/common $(CFLAGS) $(PROJECT_CFLAGS) \
		$(SANITIZERS) tests/test_example_text_policy.c \
		examples/boxrig_term/common/text_policy.c $(UI_LIBRARY) $(LIBRARY) \
		$(SANITIZERS) -o $@

$(EXAMPLE_MOUSE_POLICY_TEST_BINARY): tests/test_example_mouse_policy.c \
		examples/boxrig_term/common/mouse_policy.c \
		examples/boxrig_term/common/mouse_policy.h $(UI_LIBRARY) $(LIBRARY) | \
		$(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) \
		-Iexamples/boxrig_term/common $(CFLAGS) $(PROJECT_CFLAGS) \
		$(SANITIZERS) tests/test_example_mouse_policy.c \
		examples/boxrig_term/common/mouse_policy.c $(UI_LIBRARY) $(LIBRARY) \
		$(SANITIZERS) -o $@

$(OSC_RECOGNIZER_REGRESSION_TEST_BINARY): \
		tests/test_osc_recognizer_regression.c $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) \
		$(SANITIZERS) tests/test_osc_recognizer_regression.c $(LIBRARY) \
		$(SANITIZERS) -o $@

$(INPUT_SHORTCUT_TEST_BINARY): tests/test_input_shortcuts.c $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) tests/test_input_shortcuts.c $(UI_LIBRARY) $(LIBRARY) $(SANITIZERS) -o $@

$(NOTIFICATION_POLICY_TEST_BINARY): tests/test_notification_policy.c $(UI_LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) tests/test_notification_policy.c $(UI_LIBRARY) $(SANITIZERS) -o $@

$(SCROLLBAR_TEST_BINARY): tests/test_scrollbar.c $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) tests/test_scrollbar.c $(UI_LIBRARY) $(LIBRARY) $(SANITIZERS) -o $@

$(EXAMPLE_LAYOUT_TEST_BINARY): tests/test_example_layout.c | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) $< $(SANITIZERS) -o $@

$(UI_GUIDE_TEST_BINARY): tests/test_ui_guide.c | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(POSIX_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) $< $(SANITIZERS) -o $@

.PHONY: test-simple-ui-guide
test: test-simple-ui-guide
test-simple-ui-guide:
	$(MAKE) -C docs/ja/step-by-step check

$(EMITTER_TEST_BINARY): tests/test_emitter.c examples/boxrig_emitter/common/emitter.c examples/boxrig_emitter/common/emitter.h $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) -Iexamples/boxrig_emitter/common $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) tests/test_emitter.c examples/boxrig_emitter/common/emitter.c $(LIBRARY) $(SANITIZERS) -o $@

$(POSIX_EMITTER_BINARY): examples/boxrig_emitter/posix/main.c examples/boxrig_emitter/common/emitter.c examples/boxrig_emitter/common/emitter.h | $(BUILD_DIR)/boxrig_emitter/posix
	$(CC) $(CPPFLAGS) $(POSIX_CPPFLAGS) -Iexamples/boxrig_emitter/common $(CFLAGS) $(PROJECT_CFLAGS) examples/boxrig_emitter/posix/main.c examples/boxrig_emitter/common/emitter.c -o $@

$(MINGW_AMD64_EMITTER): examples/boxrig_emitter/win32/main.c examples/boxrig_emitter/common/emitter.c examples/boxrig_emitter/common/emitter.h | $(BUILD_DIR)/boxrig_emitter/win32/amd64
	$(MINGW_AMD64_CC) $(CPPFLAGS) $(MINGW_WIN32_CPPFLAGS) -Iexamples/boxrig_emitter/common $(CFLAGS) $(PROJECT_CFLAGS) examples/boxrig_emitter/win32/main.c examples/boxrig_emitter/common/emitter.c -o $@

$(MINGW_I686_EMITTER): examples/boxrig_emitter/win32/main.c examples/boxrig_emitter/common/emitter.c examples/boxrig_emitter/common/emitter.h | $(BUILD_DIR)/boxrig_emitter/win32/i686
	$(MINGW_I686_CC) $(CPPFLAGS) $(MINGW_WIN32_CPPFLAGS) -Iexamples/boxrig_emitter/common $(CFLAGS) $(PROJECT_CFLAGS) examples/boxrig_emitter/win32/main.c examples/boxrig_emitter/common/emitter.c -o $@

$(TERMINFO_TEST_BINARY): tests/test_terminfo.c $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(POSIX_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) $< $(LIBRARY) $(SANITIZERS) -o $@

$(MINGW_AMD64_CORE_TEST): tests/test_terminal.c Makefile $(MINGW_AMD64_LIBRARY) tests/win32/core_target.h | $(MINGW_AMD64_BUILD_DIR)/tests
	$(MINGW_AMD64_CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(MINGW_AMD64_CORE_CPPFLAGS) -include tests/win32/core_target.h $(CFLAGS) $(PROJECT_CFLAGS) $< $(MINGW_AMD64_LIBRARY) $(MINGW_AMD64_CORE_LDFLAGS) -o $@

$(MINGW_I686_CORE_TEST): tests/test_terminal.c Makefile $(MINGW_I686_LIBRARY) tests/win32/core_target.h | $(MINGW_I686_BUILD_DIR)/tests
	$(MINGW_I686_CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(MINGW_I686_CORE_CPPFLAGS) -include tests/win32/core_target.h $(CFLAGS) $(PROJECT_CFLAGS) $< $(MINGW_I686_LIBRARY) $(MINGW_I686_CORE_LDFLAGS) -o $@

$(MINGW_AMD64_GRAPHICS_TEST): tests/test_graphics.c Makefile $(MINGW_AMD64_LIBRARY) tests/win32/core_target.h | $(MINGW_AMD64_BUILD_DIR)/tests
	$(MINGW_AMD64_CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(MINGW_AMD64_CORE_CPPFLAGS) -include tests/win32/core_target.h $(CFLAGS) $(PROJECT_CFLAGS) $< $(MINGW_AMD64_LIBRARY) $(MINGW_AMD64_CORE_LDFLAGS) -o $@

$(MINGW_I686_GRAPHICS_TEST): tests/test_graphics.c Makefile $(MINGW_I686_LIBRARY) tests/win32/core_target.h | $(MINGW_I686_BUILD_DIR)/tests
	$(MINGW_I686_CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(MINGW_I686_CORE_CPPFLAGS) -include tests/win32/core_target.h $(CFLAGS) $(PROJECT_CFLAGS) $< $(MINGW_I686_LIBRARY) $(MINGW_I686_CORE_LDFLAGS) -o $@

$(MINGW_AMD64_UI_PLATFORM_FIXTURE_TEST): \
		tests/test_ui_platform_fixture.c $(MINGW_AMD64_WIN32_LIBRARY) \
		$(MINGW_AMD64_SESSION_LIBRARY) $(MINGW_AMD64_UI_LIBRARY) \
		$(MINGW_AMD64_LIBRARY) \
		| $(MINGW_AMD64_BUILD_DIR)/tests
	$(MINGW_AMD64_CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(MINGW_WIN32_CPPFLAGS) \
		$(CFLAGS) $(PROJECT_CFLAGS) -DBOXRIG_TEST_FRONTEND_WIN32 $< \
		$(MINGW_AMD64_WIN32_LIBRARY) $(MINGW_AMD64_SESSION_LIBRARY) \
		$(MINGW_AMD64_UI_LIBRARY) \
		$(MINGW_AMD64_LIBRARY) \
		$(WIN32_RENDERER_LIBS) $(WIN32_HOST_LIBS) \
		$(WIN32_TRANSPARENCY_LIBS) -ladvapi32 -o $@

$(MINGW_I686_UI_PLATFORM_FIXTURE_TEST): \
		tests/test_ui_platform_fixture.c $(MINGW_I686_WIN32_LIBRARY) \
		$(MINGW_I686_SESSION_LIBRARY) $(MINGW_I686_UI_LIBRARY) \
		$(MINGW_I686_LIBRARY) \
		| $(MINGW_I686_BUILD_DIR)/tests
	$(MINGW_I686_CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(MINGW_WIN32_CPPFLAGS) \
		$(CFLAGS) $(PROJECT_CFLAGS) -DBOXRIG_TEST_FRONTEND_WIN32 $< \
		$(MINGW_I686_WIN32_LIBRARY) $(MINGW_I686_SESSION_LIBRARY) \
		$(MINGW_I686_UI_LIBRARY) \
		$(MINGW_I686_LIBRARY) \
		$(WIN32_RENDERER_LIBS) $(WIN32_HOST_LIBS) \
		$(WIN32_TRANSPARENCY_LIBS) -ladvapi32 -o $@

$(MINGW_AMD64_COMPONENT_FIXTURE_TEST): \
		tests/win32/test_win32_component_fixture.c \
		$(FRONTEND_COMPONENT_FIXTURE_SOURCES) \
		$(FRONTEND_COMPONENT_FIXTURE_HEADER) $(MINGW_AMD64_WIN32_LIBRARY) \
		$(MINGW_AMD64_UI_LIBRARY) $(MINGW_AMD64_LIBRARY) \
		| $(MINGW_AMD64_BUILD_DIR)/tests
	$(MINGW_AMD64_CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(MINGW_WIN32_CPPFLAGS) \
		$(CFLAGS) $(PROJECT_CFLAGS) \
		tests/win32/test_win32_component_fixture.c \
		$(FRONTEND_COMPONENT_FIXTURE_SOURCES) \
		$(MINGW_AMD64_WIN32_LIBRARY) $(MINGW_AMD64_UI_LIBRARY) \
		$(MINGW_AMD64_LIBRARY) $(WIN32_RENDERER_LIBS) -o $@

$(MINGW_I686_COMPONENT_FIXTURE_TEST): \
		tests/win32/test_win32_component_fixture.c \
		$(FRONTEND_COMPONENT_FIXTURE_SOURCES) \
		$(FRONTEND_COMPONENT_FIXTURE_HEADER) $(MINGW_I686_WIN32_LIBRARY) \
		$(MINGW_I686_UI_LIBRARY) $(MINGW_I686_LIBRARY) \
		| $(MINGW_I686_BUILD_DIR)/tests
	$(MINGW_I686_CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(MINGW_WIN32_CPPFLAGS) \
		$(CFLAGS) $(PROJECT_CFLAGS) \
		tests/win32/test_win32_component_fixture.c \
		$(FRONTEND_COMPONENT_FIXTURE_SOURCES) \
		$(MINGW_I686_WIN32_LIBRARY) $(MINGW_I686_UI_LIBRARY) \
		$(MINGW_I686_LIBRARY) $(WIN32_RENDERER_LIBS) -o $@

$(MINGW_AMD64_SESSION_TEST): tests/win32/test_win32_session.c \
		$(MINGW_AMD64_WIN32_LIBRARY) $(MINGW_AMD64_SESSION_LIBRARY) \
		$(MINGW_AMD64_LIBRARY) \
		 | $(MINGW_AMD64_BUILD_DIR)/tests
	$(MINGW_AMD64_CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(MINGW_WIN32_CPPFLAGS) \
		$(CFLAGS) $(PROJECT_CFLAGS) $< $(MINGW_AMD64_WIN32_LIBRARY) \
		$(MINGW_AMD64_SESSION_LIBRARY) $(MINGW_AMD64_LIBRARY) \
		-municode -o $@

$(MINGW_I686_SESSION_TEST): tests/win32/test_win32_session.c \
		$(MINGW_I686_WIN32_LIBRARY) $(MINGW_I686_SESSION_LIBRARY) \
		$(MINGW_I686_LIBRARY) \
		 | $(MINGW_I686_BUILD_DIR)/tests
	$(MINGW_I686_CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(MINGW_WIN32_CPPFLAGS) \
		$(CFLAGS) $(PROJECT_CFLAGS) $< $(MINGW_I686_WIN32_LIBRARY) \
		$(MINGW_I686_SESSION_LIBRARY) $(MINGW_I686_LIBRARY) \
		-municode -o $@

$(MINGW_AMD64_PTY_TEST): tests/win32/test_win32_pty.c $(MINGW_AMD64_WIN32_LIBRARY) $(MINGW_AMD64_LIBRARY) | $(MINGW_AMD64_BUILD_DIR)/tests
	$(MINGW_AMD64_CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(MINGW_TERM_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) tests/win32/test_win32_pty.c $(MINGW_AMD64_WIN32_LIBRARY) $(MINGW_AMD64_LIBRARY) -municode -o $@

$(MINGW_I686_PTY_TEST): tests/win32/test_win32_pty.c $(MINGW_I686_WIN32_LIBRARY) $(MINGW_I686_LIBRARY) | $(MINGW_I686_BUILD_DIR)/tests
	$(MINGW_I686_CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(MINGW_TERM_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) tests/win32/test_win32_pty.c $(MINGW_I686_WIN32_LIBRARY) $(MINGW_I686_LIBRARY) -municode -o $@

$(MINGW_AMD64_PTY_HELPER): tests/win32/pty_helper.c \
		$(FRONTEND_SAMPLE_SCENARIO_SOURCE) \
		$(FRONTEND_SAMPLE_SCENARIO_HEADER) | $(MINGW_AMD64_BUILD_DIR)/tests
	$(MINGW_AMD64_CC) $(CPPFLAGS) $(MINGW_TERM_CPPFLAGS) $(CFLAGS) \
		$(PROJECT_CFLAGS) tests/win32/pty_helper.c \
		$(FRONTEND_SAMPLE_SCENARIO_SOURCE) -o $@

$(MINGW_I686_PTY_HELPER): tests/win32/pty_helper.c \
		$(FRONTEND_SAMPLE_SCENARIO_SOURCE) \
		$(FRONTEND_SAMPLE_SCENARIO_HEADER) | $(MINGW_I686_BUILD_DIR)/tests
	$(MINGW_I686_CC) $(CPPFLAGS) $(MINGW_TERM_CPPFLAGS) $(CFLAGS) \
		$(PROJECT_CFLAGS) tests/win32/pty_helper.c \
		$(FRONTEND_SAMPLE_SCENARIO_SOURCE) -o $@

$(MINGW_AMD64_RENDERER_TEST): tests/win32/test_win32_renderer.c src/win32/renderer_private.h $(MINGW_AMD64_WIN32_LIBRARY) $(MINGW_AMD64_UI_LIBRARY) $(MINGW_AMD64_LIBRARY) | $(MINGW_AMD64_BUILD_DIR)/tests
	$(MINGW_AMD64_CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(MINGW_WIN32_CPPFLAGS) -Isrc/win32 $(CFLAGS) $(PROJECT_CFLAGS) tests/win32/test_win32_renderer.c $(MINGW_AMD64_WIN32_LIBRARY) $(MINGW_AMD64_UI_LIBRARY) $(MINGW_AMD64_LIBRARY) $(WIN32_RENDERER_LIBS) -Wl,--wrap=boxrig_win32_renderer_create -o $@

$(MINGW_I686_RENDERER_TEST): tests/win32/test_win32_renderer.c src/win32/renderer_private.h $(MINGW_I686_WIN32_LIBRARY) $(MINGW_I686_UI_LIBRARY) $(MINGW_I686_LIBRARY) | $(MINGW_I686_BUILD_DIR)/tests
	$(MINGW_I686_CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(MINGW_WIN32_CPPFLAGS) -Isrc/win32 $(CFLAGS) $(PROJECT_CFLAGS) tests/win32/test_win32_renderer.c $(MINGW_I686_WIN32_LIBRARY) $(MINGW_I686_UI_LIBRARY) $(MINGW_I686_LIBRARY) $(WIN32_RENDERER_LIBS) -Wl,--wrap=boxrig_win32_renderer_create -o $@

$(MINGW_AMD64_HOST_TRANSPARENCY_TEST): tests/win32/test_win32_host_transparency.c include/boxrig/win32/host_transparency.h $(MINGW_AMD64_WIN32_LIBRARY) $(MINGW_AMD64_UI_LIBRARY) $(MINGW_AMD64_LIBRARY) | $(MINGW_AMD64_BUILD_DIR)/tests
	$(MINGW_AMD64_CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(MINGW_WIN32_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $< $(MINGW_AMD64_WIN32_LIBRARY) $(MINGW_AMD64_UI_LIBRARY) $(MINGW_AMD64_LIBRARY) $(WIN32_TRANSPARENCY_LIBS) -o $@

$(MINGW_I686_HOST_TRANSPARENCY_TEST): tests/win32/test_win32_host_transparency.c include/boxrig/win32/host_transparency.h $(MINGW_I686_WIN32_LIBRARY) $(MINGW_I686_UI_LIBRARY) $(MINGW_I686_LIBRARY) | $(MINGW_I686_BUILD_DIR)/tests
	$(MINGW_I686_CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(MINGW_WIN32_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $< $(MINGW_I686_WIN32_LIBRARY) $(MINGW_I686_UI_LIBRARY) $(MINGW_I686_LIBRARY) $(WIN32_TRANSPARENCY_LIBS) -o $@

# Layered child windows require the Windows 8 compatibility manifest on Windows.
$(MINGW_AMD64_TRANSPARENCY_TEST): tests/win32/test_win32_transparency.c src/win32/transparency_private.h $(MINGW_AMD64_BUILD_DIR)/tests/basic_ui_manifest.o Makefile $(MINGW_AMD64_WIN32_LIBRARY) $(MINGW_AMD64_UI_LIBRARY) $(MINGW_AMD64_LIBRARY) | $(MINGW_AMD64_BUILD_DIR)/tests
	$(MINGW_AMD64_CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(MINGW_WIN32_CPPFLAGS) -Isrc/win32 $(CFLAGS) $(PROJECT_CFLAGS) tests/win32/test_win32_transparency.c $(MINGW_AMD64_BUILD_DIR)/tests/basic_ui_manifest.o $(MINGW_AMD64_WIN32_LIBRARY) $(MINGW_AMD64_UI_LIBRARY) $(MINGW_AMD64_LIBRARY) $(WIN32_TRANSPARENCY_LIBS) -o $@

$(MINGW_I686_TRANSPARENCY_TEST): tests/win32/test_win32_transparency.c src/win32/transparency_private.h $(MINGW_I686_BUILD_DIR)/tests/basic_ui_manifest.o Makefile $(MINGW_I686_WIN32_LIBRARY) $(MINGW_I686_UI_LIBRARY) $(MINGW_I686_LIBRARY) | $(MINGW_I686_BUILD_DIR)/tests
	$(MINGW_I686_CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(MINGW_WIN32_CPPFLAGS) -Isrc/win32 $(CFLAGS) $(PROJECT_CFLAGS) tests/win32/test_win32_transparency.c $(MINGW_I686_BUILD_DIR)/tests/basic_ui_manifest.o $(MINGW_I686_WIN32_LIBRARY) $(MINGW_I686_UI_LIBRARY) $(MINGW_I686_LIBRARY) $(WIN32_TRANSPARENCY_LIBS) -o $@

$(MINGW_AMD64_EVENT_LOOP_TEST): tests/win32/test_win32_event_loop.c $(MINGW_AMD64_WIN32_LIBRARY) $(MINGW_AMD64_SESSION_LIBRARY) $(MINGW_AMD64_UI_LIBRARY) $(MINGW_AMD64_LIBRARY) | $(MINGW_AMD64_BUILD_DIR)/tests
	$(MINGW_AMD64_CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(MINGW_TERM_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) tests/win32/test_win32_event_loop.c $(MINGW_AMD64_WIN32_LIBRARY) $(MINGW_AMD64_SESSION_LIBRARY) $(MINGW_AMD64_UI_LIBRARY) $(MINGW_AMD64_LIBRARY) $(WIN32_RENDERER_LIBS) $(WIN32_HOST_LIBS) $(WIN32_TRANSPARENCY_LIBS) -ladvapi32 -o $@

$(MINGW_I686_EVENT_LOOP_TEST): tests/win32/test_win32_event_loop.c $(MINGW_I686_WIN32_LIBRARY) $(MINGW_I686_SESSION_LIBRARY) $(MINGW_I686_UI_LIBRARY) $(MINGW_I686_LIBRARY) | $(MINGW_I686_BUILD_DIR)/tests
	$(MINGW_I686_CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(MINGW_TERM_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) tests/win32/test_win32_event_loop.c $(MINGW_I686_WIN32_LIBRARY) $(MINGW_I686_SESSION_LIBRARY) $(MINGW_I686_UI_LIBRARY) $(MINGW_I686_LIBRARY) $(WIN32_RENDERER_LIBS) $(WIN32_HOST_LIBS) $(WIN32_TRANSPARENCY_LIBS) -ladvapi32 -o $@

$(MINGW_AMD64_HOST_TEST): tests/win32/test_win32_host_adapter.c src/win32/host_private.h $(MINGW_AMD64_WIN32_LIBRARY) $(MINGW_AMD64_UI_LIBRARY) $(MINGW_AMD64_LIBRARY) | $(MINGW_AMD64_BUILD_DIR)/tests
	$(MINGW_AMD64_CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(MINGW_WIN32_CPPFLAGS) -Isrc/win32 $(CFLAGS) $(PROJECT_CFLAGS) tests/win32/test_win32_host_adapter.c $(MINGW_AMD64_WIN32_LIBRARY) $(MINGW_AMD64_UI_LIBRARY) $(MINGW_AMD64_LIBRARY) $(WIN32_HOST_LIBS) -o $@

$(MINGW_I686_HOST_TEST): tests/win32/test_win32_host_adapter.c src/win32/host_private.h $(MINGW_I686_WIN32_LIBRARY) $(MINGW_I686_UI_LIBRARY) $(MINGW_I686_LIBRARY) | $(MINGW_I686_BUILD_DIR)/tests
	$(MINGW_I686_CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(MINGW_WIN32_CPPFLAGS) -Isrc/win32 $(CFLAGS) $(PROJECT_CFLAGS) tests/win32/test_win32_host_adapter.c $(MINGW_I686_WIN32_LIBRARY) $(MINGW_I686_UI_LIBRARY) $(MINGW_I686_LIBRARY) $(WIN32_HOST_LIBS) -o $@

$(MINGW_AMD64_INPUT_TEST): tests/win32/test_win32_input_adapter.c tests/editable_fixture.h tests/composition_fixture.h src/win32/input.c include/boxrig/win32/input.h $(MINGW_AMD64_UI_LIBRARY) $(MINGW_AMD64_LIBRARY) | $(MINGW_AMD64_BUILD_DIR)/tests
	$(MINGW_AMD64_CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(MINGW_WIN32_CPPFLAGS) $(MINGW_IMM_TEST_CPPFLAGS) $(MINGW_CLIPBOARD_TEST_CPPFLAGS) $(MINGW_KEYMAP_TEST_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) tests/win32/test_win32_input_adapter.c src/win32/input.c $(MINGW_AMD64_UI_LIBRARY) $(MINGW_AMD64_LIBRARY) -municode -limm32 -luser32 -o $@

$(MINGW_I686_INPUT_TEST): tests/win32/test_win32_input_adapter.c tests/editable_fixture.h tests/composition_fixture.h src/win32/input.c include/boxrig/win32/input.h $(MINGW_I686_UI_LIBRARY) $(MINGW_I686_LIBRARY) | $(MINGW_I686_BUILD_DIR)/tests
	$(MINGW_I686_CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(MINGW_WIN32_CPPFLAGS) $(MINGW_IMM_TEST_CPPFLAGS) $(MINGW_CLIPBOARD_TEST_CPPFLAGS) $(MINGW_KEYMAP_TEST_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) tests/win32/test_win32_input_adapter.c src/win32/input.c $(MINGW_I686_UI_LIBRARY) $(MINGW_I686_LIBRARY) -municode -limm32 -luser32 -o $@

$(MINGW_AMD64_COMPONENT_TEST): tests/win32/test_win32_component.c src/win32/renderer_private.h $(MINGW_AMD64_WIN32_LIBRARY) $(MINGW_AMD64_UI_LIBRARY) $(MINGW_AMD64_LIBRARY) | $(MINGW_AMD64_BUILD_DIR)/tests
	$(MINGW_AMD64_CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(MINGW_WIN32_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $< $(MINGW_AMD64_WIN32_LIBRARY) $(MINGW_AMD64_UI_LIBRARY) $(MINGW_AMD64_LIBRARY) $(WIN32_RENDERER_LIBS) -DBOXRIG_TEST_OMIT_OPTIONAL_INTERFACES -Wl,--wrap=boxrig_win32_input_adapter_resynchronize_candidate -Wl,--wrap=boxrig_win32_renderer_create -o $@

$(MINGW_I686_COMPONENT_TEST): tests/win32/test_win32_component.c src/win32/renderer_private.h $(MINGW_I686_WIN32_LIBRARY) $(MINGW_I686_UI_LIBRARY) $(MINGW_I686_LIBRARY) | $(MINGW_I686_BUILD_DIR)/tests
	$(MINGW_I686_CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(MINGW_WIN32_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $< $(MINGW_I686_WIN32_LIBRARY) $(MINGW_I686_UI_LIBRARY) $(MINGW_I686_LIBRARY) $(WIN32_RENDERER_LIBS) -DBOXRIG_TEST_OMIT_OPTIONAL_INTERFACES -Wl,--wrap=boxrig_win32_input_adapter_resynchronize_candidate -Wl,--wrap=boxrig_win32_renderer_create -o $@

$(MINGW_AMD64_EXAMPLE): $(WIN32_EXAMPLE_SOURCES) examples/boxrig_term/win32/application_internal.h examples/boxrig_term/common/color_demo.h examples/boxrig_term/common/scrollbar_demo.h $(MINGW_AMD64_EXAMPLE_RESOURCE) $(UI_HEADERS) $(WIN32_HEADERS) $(MINGW_AMD64_WIN32_LIBRARY) $(MINGW_AMD64_SESSION_LIBRARY) $(MINGW_AMD64_UI_LIBRARY) $(MINGW_AMD64_LIBRARY) $(CONPTY_HEADER) | $(BUILD_DIR)/boxrig_term/win32/amd64
	$(MINGW_AMD64_CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(MINGW_TERM_CPPFLAGS) $(WIN32_TERM_RENDERER_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(WIN32_EXAMPLE_SOURCES) $(MINGW_AMD64_EXAMPLE_RESOURCE) $(WIN32_TERM_AMD64_RENDERER_LIBRARIES) $(MINGW_AMD64_WIN32_LIBRARY) $(MINGW_AMD64_SESSION_LIBRARY) $(MINGW_AMD64_UI_LIBRARY) $(MINGW_AMD64_LIBRARY) -municode -mwindows $(WIN32_RENDERER_LIBS) $(WIN32_HOST_LIBS) $(WIN32_TRANSPARENCY_LIBS) $(WIN32_TERM_RENDERER_LIBS) -ladvapi32 -lcomctl32 -o $@

$(MINGW_I686_EXAMPLE): $(WIN32_EXAMPLE_SOURCES) examples/boxrig_term/win32/application_internal.h examples/boxrig_term/common/color_demo.h examples/boxrig_term/common/scrollbar_demo.h $(MINGW_I686_EXAMPLE_RESOURCE) $(UI_HEADERS) $(WIN32_HEADERS) $(MINGW_I686_WIN32_LIBRARY) $(MINGW_I686_SESSION_LIBRARY) $(MINGW_I686_UI_LIBRARY) $(MINGW_I686_LIBRARY) $(CONPTY_HEADER) | $(BUILD_DIR)/boxrig_term/win32/i686
	$(MINGW_I686_CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(MINGW_TERM_CPPFLAGS) $(WIN32_TERM_RENDERER_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(WIN32_EXAMPLE_SOURCES) $(MINGW_I686_EXAMPLE_RESOURCE) $(WIN32_TERM_I686_RENDERER_LIBRARIES) $(MINGW_I686_WIN32_LIBRARY) $(MINGW_I686_SESSION_LIBRARY) $(MINGW_I686_UI_LIBRARY) $(MINGW_I686_LIBRARY) -municode -mwindows $(WIN32_RENDERER_LIBS) $(WIN32_HOST_LIBS) $(WIN32_TRANSPARENCY_LIBS) $(WIN32_TERM_RENDERER_LIBS) -ladvapi32 -lcomctl32 -o $@

$(MINGW_AMD64_SHUTDOWN_PROBE_EXAMPLE): $(WIN32_EXAMPLE_SOURCES) examples/boxrig_term/win32/application_internal.h examples/boxrig_term/common/color_demo.h examples/boxrig_term/common/scrollbar_demo.h tests/win32/shutdown_probe.c $(UI_HEADERS) $(WIN32_HEADERS) $(MINGW_AMD64_WIN32_LIBRARY) $(MINGW_AMD64_SESSION_LIBRARY) $(MINGW_AMD64_UI_LIBRARY) $(MINGW_AMD64_LIBRARY) $(CONPTY_HEADER) | $(MINGW_AMD64_BUILD_DIR)
	$(MINGW_AMD64_CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(MINGW_TERM_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(WIN32_EXAMPLE_SOURCES) tests/win32/shutdown_probe.c $(MINGW_AMD64_WIN32_LIBRARY) $(MINGW_AMD64_SESSION_LIBRARY) $(MINGW_AMD64_UI_LIBRARY) $(MINGW_AMD64_LIBRARY) -municode -mwindows $(WIN32_RENDERER_LIBS) $(WIN32_HOST_LIBS) $(WIN32_TRANSPARENCY_LIBS) -ladvapi32 -Wl,--wrap=boxrig_win32_input_adapter_process_message -Wl,--wrap=boxrig_win32_input_adapter_destroy -lcomctl32 -o $@

$(MINGW_I686_SHUTDOWN_PROBE_EXAMPLE): $(WIN32_EXAMPLE_SOURCES) examples/boxrig_term/win32/application_internal.h examples/boxrig_term/common/color_demo.h examples/boxrig_term/common/scrollbar_demo.h tests/win32/shutdown_probe.c $(UI_HEADERS) $(WIN32_HEADERS) $(MINGW_I686_WIN32_LIBRARY) $(MINGW_I686_SESSION_LIBRARY) $(MINGW_I686_UI_LIBRARY) $(MINGW_I686_LIBRARY) $(CONPTY_HEADER) | $(MINGW_I686_BUILD_DIR)
	$(MINGW_I686_CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(MINGW_TERM_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(WIN32_EXAMPLE_SOURCES) tests/win32/shutdown_probe.c $(MINGW_I686_WIN32_LIBRARY) $(MINGW_I686_SESSION_LIBRARY) $(MINGW_I686_UI_LIBRARY) $(MINGW_I686_LIBRARY) -municode -mwindows $(WIN32_RENDERER_LIBS) $(WIN32_HOST_LIBS) $(WIN32_TRANSPARENCY_LIBS) -ladvapi32 -Wl,--wrap=boxrig_win32_input_adapter_process_message -Wl,--wrap=boxrig_win32_input_adapter_destroy -lcomctl32 -o $@

$(MINGW_AMD64_IME_MOVEMENT_PROBE_EXAMPLE): $(WIN32_EXAMPLE_SOURCES) examples/boxrig_term/win32/application_internal.h examples/boxrig_term/common/color_demo.h examples/boxrig_term/common/scrollbar_demo.h tests/win32/ime_movement_probe.c $(UI_HEADERS) $(WIN32_HEADERS) $(MINGW_AMD64_WIN32_LIBRARY) $(MINGW_AMD64_SESSION_LIBRARY) $(MINGW_AMD64_UI_LIBRARY) $(MINGW_AMD64_LIBRARY) $(CONPTY_HEADER) | $(MINGW_AMD64_BUILD_DIR)
	$(MINGW_AMD64_CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(MINGW_TERM_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(WIN32_EXAMPLE_SOURCES) tests/win32/ime_movement_probe.c $(MINGW_AMD64_WIN32_LIBRARY) $(MINGW_AMD64_SESSION_LIBRARY) $(MINGW_AMD64_UI_LIBRARY) $(MINGW_AMD64_LIBRARY) -municode -mwindows $(WIN32_RENDERER_LIBS) $(WIN32_HOST_LIBS) $(WIN32_TRANSPARENCY_LIBS) -ladvapi32 -Wl,--wrap=boxrig_win32_terminal_component_resynchronize_ime_placement -lcomctl32 -o $@

$(MINGW_I686_IME_MOVEMENT_PROBE_EXAMPLE): $(WIN32_EXAMPLE_SOURCES) examples/boxrig_term/win32/application_internal.h examples/boxrig_term/common/color_demo.h examples/boxrig_term/common/scrollbar_demo.h tests/win32/ime_movement_probe.c $(UI_HEADERS) $(WIN32_HEADERS) $(MINGW_I686_WIN32_LIBRARY) $(MINGW_I686_SESSION_LIBRARY) $(MINGW_I686_UI_LIBRARY) $(MINGW_I686_LIBRARY) $(CONPTY_HEADER) | $(MINGW_I686_BUILD_DIR)
	$(MINGW_I686_CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(MINGW_TERM_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(WIN32_EXAMPLE_SOURCES) tests/win32/ime_movement_probe.c $(MINGW_I686_WIN32_LIBRARY) $(MINGW_I686_SESSION_LIBRARY) $(MINGW_I686_UI_LIBRARY) $(MINGW_I686_LIBRARY) -municode -mwindows $(WIN32_RENDERER_LIBS) $(WIN32_HOST_LIBS) $(WIN32_TRANSPARENCY_LIBS) -ladvapi32 -Wl,--wrap=boxrig_win32_terminal_component_resynchronize_ime_placement -lcomctl32 -o $@

$(MINGW_AMD64_NOTIFICATION_EXAMPLE): $(WIN32_EXAMPLE_SOURCES) examples/boxrig_term/win32/application_internal.h examples/boxrig_term/common/color_demo.h examples/boxrig_term/common/scrollbar_demo.h src/win32/host.c src/win32/host_private.h tests/win32/fake_shell_notify_icon.c $(UI_HEADERS) $(WIN32_HEADERS) $(MINGW_AMD64_WIN32_LIBRARY) $(MINGW_AMD64_SESSION_LIBRARY) $(MINGW_AMD64_UI_LIBRARY) $(MINGW_AMD64_LIBRARY) $(CONPTY_HEADER) | $(MINGW_AMD64_BUILD_DIR)
	$(MINGW_AMD64_CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(MINGW_TERM_CPPFLAGS) -DBOXRIG_WIN32_SHELL_NOTIFY_ICON=boxrig_test_Shell_NotifyIconW -Isrc/win32 $(CFLAGS) $(PROJECT_CFLAGS) $(WIN32_EXAMPLE_SOURCES) src/win32/host.c tests/win32/fake_shell_notify_icon.c $(MINGW_AMD64_WIN32_LIBRARY) $(MINGW_AMD64_SESSION_LIBRARY) $(MINGW_AMD64_UI_LIBRARY) $(MINGW_AMD64_LIBRARY) -municode -mwindows $(WIN32_RENDERER_LIBS) $(WIN32_HOST_LIBS) $(WIN32_TRANSPARENCY_LIBS) -ladvapi32 -lcomctl32 -o $@

$(MINGW_I686_NOTIFICATION_EXAMPLE): $(WIN32_EXAMPLE_SOURCES) examples/boxrig_term/win32/application_internal.h examples/boxrig_term/common/color_demo.h examples/boxrig_term/common/scrollbar_demo.h src/win32/host.c src/win32/host_private.h tests/win32/fake_shell_notify_icon.c $(UI_HEADERS) $(WIN32_HEADERS) $(MINGW_I686_WIN32_LIBRARY) $(MINGW_I686_SESSION_LIBRARY) $(MINGW_I686_UI_LIBRARY) $(MINGW_I686_LIBRARY) $(CONPTY_HEADER) | $(MINGW_I686_BUILD_DIR)
	$(MINGW_I686_CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(MINGW_TERM_CPPFLAGS) -DBOXRIG_WIN32_SHELL_NOTIFY_ICON=boxrig_test_Shell_NotifyIconW -Isrc/win32 $(CFLAGS) $(PROJECT_CFLAGS) $(WIN32_EXAMPLE_SOURCES) src/win32/host.c tests/win32/fake_shell_notify_icon.c $(MINGW_I686_WIN32_LIBRARY) $(MINGW_I686_SESSION_LIBRARY) $(MINGW_I686_UI_LIBRARY) $(MINGW_I686_LIBRARY) -municode -mwindows $(WIN32_RENDERER_LIBS) $(WIN32_HOST_LIBS) $(WIN32_TRANSPARENCY_LIBS) -ladvapi32 -lcomctl32 -o $@

$(MINGW_AMD64_EXAMPLE_RESOURCE): examples/boxrig_term/win32/boxrig_term.rc \
	examples/boxrig_term/win32/boxrig_term.manifest | $(MINGW_AMD64_BUILD_DIR)
	$(MINGW_AMD64_WINDRES) $(CPPFLAGS) $(MINGW_TERM_CPPFLAGS) $< -o $@

$(MINGW_I686_EXAMPLE_RESOURCE): examples/boxrig_term/win32/boxrig_term.rc \
	examples/boxrig_term/win32/boxrig_term.manifest | $(MINGW_I686_BUILD_DIR)
	$(MINGW_I686_WINDRES) $(CPPFLAGS) $(MINGW_TERM_CPPFLAGS) $< -o $@

$(MINGW_AMD64_LAUNCHER): examples/boxrig_term/win32/console_launcher.c | $(BUILD_DIR)/boxrig_term/win32/amd64
	$(MINGW_AMD64_CC) $(CPPFLAGS) $(MINGW_TERM_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $< -municode -o $@

$(MINGW_I686_LAUNCHER): examples/boxrig_term/win32/console_launcher.c | $(BUILD_DIR)/boxrig_term/win32/i686
	$(MINGW_I686_CC) $(CPPFLAGS) $(MINGW_TERM_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $< -municode -o $@

$(MINGW_AMD64_TEST_LAUNCHER): examples/boxrig_term/win32/console_launcher.c | $(MINGW_AMD64_BUILD_DIR)
	$(MINGW_AMD64_CC) $(CPPFLAGS) $(MINGW_TERM_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $< -municode -o $@

$(MINGW_I686_TEST_LAUNCHER): examples/boxrig_term/win32/console_launcher.c | $(MINGW_I686_BUILD_DIR)
	$(MINGW_I686_CC) $(CPPFLAGS) $(MINGW_TERM_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $< -municode -o $@

$(MINGW_AMD64_SAMPLE_TEST): tests/win32/test_win32_sample.c \
		$(FRONTEND_SAMPLE_SCENARIO_HEADER) | $(MINGW_AMD64_BUILD_DIR)/tests
	$(MINGW_AMD64_CC) $(CPPFLAGS) $(MINGW_TERM_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $< -municode -lgdi32 -luser32 -o $@

$(MINGW_I686_SAMPLE_TEST): tests/win32/test_win32_sample.c \
		$(FRONTEND_SAMPLE_SCENARIO_HEADER) | $(MINGW_I686_BUILD_DIR)/tests
	$(MINGW_I686_CC) $(CPPFLAGS) $(MINGW_TERM_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $< -municode -lgdi32 -luser32 -o $@

$(PTY_TEST_BINARY): tests/x11/test_posix_pty.c $(POSIX_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(POSIX_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) tests/x11/test_posix_pty.c $(POSIX_LIBRARY) $(LIBRARY) -lutil $(SANITIZERS) -o $@

$(PTY_HELPER_BINARY): tests/x11/pty_helper.c \
		$(FRONTEND_SAMPLE_SCENARIO_SOURCE) \
		$(FRONTEND_SAMPLE_SCENARIO_HEADER) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(POSIX_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) \
		$(SANITIZERS) tests/x11/pty_helper.c \
		$(FRONTEND_SAMPLE_SCENARIO_SOURCE) $(SANITIZERS) -o $@

$(BUILD_DIR)/tests/graphics_child: tests/graphics_child.c | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(POSIX_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) $< -o $@

$(BUILD_DIR)/tests/test_posix_graphics_transport: tests/test_posix_graphics_transport.c $(POSIX_LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(POSIX_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) $< $(POSIX_LIBRARY) -o $@

.PHONY: test-graphics-transport test-native-graphics-transport test-native-frame-deletion
test: test-graphics-transport test-native-graphics-transport test-native-frame-deletion
test-native-frame-deletion:
	BOXRIG_TEST_FRAME_DELETE=1 $(MAKE) test-native-graphics-transport

test-graphics-transport: $(BUILD_DIR)/tests/test_posix_graphics_transport $(X11_HOST_TEST_BINARY) $(GRAPHICS_TEST_BINARY)
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(BUILD_DIR)/tests/test_posix_graphics_transport
	$(XVFB_RUN) -a env ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(X11_HOST_TEST_BINARY)
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(GRAPHICS_TEST_BINARY)

test-native-graphics-transport: $(EXAMPLE_BINARY) $(GTK3_EXAMPLE_BINARY) $(GTK4_EXAMPLE_BINARY) $(WAYLAND_EXAMPLE_BINARY) $(BUILD_DIR)/tests/graphics_child
	$(XVFB_RUN) -a node tests/run_native_graphics.mjs $(EXAMPLE_BINARY) $(BUILD_DIR)/tests/graphics_child $(BUILD_DIR)/test-results/step26-graphics/x11 x11 x11
	$(XVFB_RUN) -a node tests/run_native_graphics.mjs $(EXAMPLE_BINARY) $(BUILD_DIR)/tests/graphics_child $(BUILD_DIR)/test-results/step26-graphics/x11-opengl opengl x11
	$(XVFB_RUN) -a node tests/run_native_graphics.mjs $(WAYLAND_EXAMPLE_BINARY) $(BUILD_DIR)/tests/graphics_child $(BUILD_DIR)/test-results/step26-graphics/wayland wayland wayland
	$(XVFB_RUN) -a node tests/run_native_graphics.mjs $(WAYLAND_EXAMPLE_BINARY) $(BUILD_DIR)/tests/graphics_child $(BUILD_DIR)/test-results/step26-graphics/wayland-opengl opengl wayland
	$(XVFB_RUN) -a node tests/run_native_graphics.mjs $(GTK3_EXAMPLE_BINARY) $(BUILD_DIR)/tests/graphics_child $(BUILD_DIR)/test-results/step26-graphics/gtk3-x11 cairo x11
	$(XVFB_RUN) -a node tests/run_native_graphics.mjs $(GTK3_EXAMPLE_BINARY) $(BUILD_DIR)/tests/graphics_child $(BUILD_DIR)/test-results/step26-graphics/gtk3-x11-opengl opengl x11
	$(XVFB_RUN) -a node tests/run_native_graphics.mjs $(GTK3_EXAMPLE_BINARY) $(BUILD_DIR)/tests/graphics_child $(BUILD_DIR)/test-results/step26-graphics/gtk3-wayland cairo wayland
	$(XVFB_RUN) -a node tests/run_native_graphics.mjs $(GTK3_EXAMPLE_BINARY) $(BUILD_DIR)/tests/graphics_child $(BUILD_DIR)/test-results/step26-graphics/gtk3-wayland-opengl opengl wayland
	$(XVFB_RUN) -a env $(GTK4_TEST_IM_ENV) GSK_RENDERER=cairo node tests/run_native_graphics.mjs $(GTK4_EXAMPLE_BINARY) $(BUILD_DIR)/tests/graphics_child $(BUILD_DIR)/test-results/step26-graphics/gtk4-x11 cairo x11
	$(XVFB_RUN) -a env $(GTK4_TEST_IM_ENV) GSK_RENDERER=cairo node tests/run_native_graphics.mjs $(GTK4_EXAMPLE_BINARY) $(BUILD_DIR)/tests/graphics_child $(BUILD_DIR)/test-results/step26-graphics/gtk4-x11-opengl opengl x11
	$(XVFB_RUN) -a env $(GTK4_TEST_IM_ENV) GSK_RENDERER=cairo node tests/run_native_graphics.mjs $(GTK4_EXAMPLE_BINARY) $(BUILD_DIR)/tests/graphics_child $(BUILD_DIR)/test-results/step26-graphics/gtk4-wayland cairo wayland
	$(XVFB_RUN) -a env $(GTK4_TEST_IM_ENV) GSK_RENDERER=cairo node tests/run_native_graphics.mjs $(GTK4_EXAMPLE_BINARY) $(BUILD_DIR)/tests/graphics_child $(BUILD_DIR)/test-results/step26-graphics/gtk4-wayland-opengl opengl wayland
ifneq ($(BOXRIG_TEST_FRAME_DELETE),1)
	$(XVFB_RUN) -a env $(GTK4_TEST_IM_ENV) GSK_RENDERER=cairo node tests/run_native_graphics.mjs $(GTK4_EXAMPLE_BINARY) $(BUILD_DIR)/tests/graphics_child $(BUILD_DIR)/test-results/step26-graphics/gtk4-denied cairo wayland deny
endif

$(X11_TEST_BINARY): tests/x11/test_x11_renderer.c $(X11_LIBRARY) $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(POSIX_CPPFLAGS) $(X11_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) tests/x11/test_x11_renderer.c $(X11_LIBRARY) $(UI_LIBRARY) $(LIBRARY) $(X11_LIBS) $(DBUS_LIBS) $(SANITIZERS) -o $@

$(X11_HOST_TEST_BINARY): tests/x11/test_x11_host_adapter.c $(X11_LIBRARY) $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(POSIX_CPPFLAGS) $(X11_CPPFLAGS) $(DBUS_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) tests/x11/test_x11_host_adapter.c $(X11_LIBRARY) $(UI_LIBRARY) $(LIBRARY) $(X11_LIBS) $(DBUS_LIBS) $(SANITIZERS) -o $@

$(X11_NOTIFICATION_TEST_BINARY): tests/x11/test_x11_notifications.c $(X11_LIBRARY) $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(POSIX_CPPFLAGS) $(X11_CPPFLAGS) $(DBUS_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) tests/x11/test_x11_notifications.c $(X11_LIBRARY) $(UI_LIBRARY) $(LIBRARY) $(X11_LIBS) $(DBUS_LIBS) $(SANITIZERS) -Wl,--wrap=boxrig_terminal_complete_host_request -o $@

.PHONY: test-x11-input-portability
test: test-x11-input-portability
test-x11-input-portability:
	# Exercise unsigned wchar_t without linking against the host's wchar_t ABI.
	$(CC) $(CPPFLAGS) $(FRONTEND_CPPFLAGS) $(POSIX_CPPFLAGS) \
		$(X11_CPPFLAGS) $(DBUS_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) \
		-fshort-wchar -fsyntax-only src/x11/input.c

$(X11_INPUT_TEST_BINARY): tests/x11/test_x11_input_adapter.c \
		tests/composition_fixture.h src/x11/input.c \
		include/boxrig/x11/input.h src/ui/utf8.h $(UI_LIBRARY) $(LIBRARY) \
		| $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(FRONTEND_CPPFLAGS) $(POSIX_CPPFLAGS) \
		$(X11_CPPFLAGS) $(CFLAGS) \
		$(PROJECT_CFLAGS) $(SANITIZERS) \
		tests/x11/test_x11_input_adapter.c src/x11/input.c \
		$(UI_LIBRARY) $(LIBRARY) $(X11_LIBS) \
		$(SANITIZERS) -Wl,--wrap=XFilterEvent \
		-Wl,--wrap=Xutf8LookupString -Wl,--wrap=XSetICValues \
		-Wl,--wrap=XkbKeycodeToKeysym \
		-Wl,--wrap=XMaxRequestSize \
		-Wl,--wrap=XExtendedMaxRequestSize -o $@

$(X11_EVENT_WAIT_TEST_BINARY): tests/x11/test_x11_event_wait.c \
		$(X11_LIBRARY) $(POSIX_LIBRARY) $(SESSION_LIBRARY) $(UI_LIBRARY) \
		$(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(POSIX_CPPFLAGS) $(X11_CPPFLAGS) \
		$(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) \
		tests/x11/test_x11_event_wait.c $(X11_LIBRARY) $(POSIX_LIBRARY) \
		$(SESSION_LIBRARY) $(UI_LIBRARY) $(LIBRARY) $(X11_LIBS) \
		$(DBUS_LIBS) -lutil \
		$(SANITIZERS) -o $@

$(X11_TRANSPARENCY_TEST_BINARY): tests/x11/test_x11_transparency.c \
		$(X11_LIBRARY) $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(POSIX_CPPFLAGS) $(X11_CPPFLAGS) \
		$(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) $< \
		$(X11_LIBRARY) $(UI_LIBRARY) $(LIBRARY) $(X11_LIBS) \
		$(DBUS_LIBS) $(SANITIZERS) -o $@

$(X11_COMPOSITOR_TEST_BINARY): tests/x11/test_x11_compositor.c \
		$(X11_LIBRARY) $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(POSIX_CPPFLAGS) $(X11_CPPFLAGS) \
		$(XDIRTY_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) $< \
		$(X11_LIBRARY) $(UI_LIBRARY) $(LIBRARY) $(X11_LIBS) \
		$(XDIRTY_LIBS) $(DBUS_LIBS) $(SANITIZERS) -o $@

$(X11_COMPONENT_TEST_BINARY): tests/x11/test_x11_component.c \
		$(X11_LIBRARY) $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(POSIX_CPPFLAGS) $(X11_CPPFLAGS) \
		$(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) $< \
		$(X11_LIBRARY) $(UI_LIBRARY) $(LIBRARY) $(X11_LIBS) \
		$(DBUS_LIBS) $(SANITIZERS) -o $@

$(X11_SAMPLE_TEST_BINARY): tests/x11/test_x11_sample.c \
		tests/x11_video.h \
		$(X11_LIBRARY) $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(POSIX_CPPFLAGS) $(X11_CPPFLAGS) \
		$(XEXT_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) $< \
		$(X11_LIBRARY) $(UI_LIBRARY) $(LIBRARY) $(X11_LIBS) \
		$(XEXT_LIBS) $(DBUS_LIBS) $(SANITIZERS) -o $@

$(X11_FAKE_NOTIFICATION_SERVICE): tests/x11/fake_notification_service.c | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(POSIX_CPPFLAGS) $(DBUS_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) $< $(DBUS_LIBS) $(SANITIZERS) -o $@

$(P2_X11_IME_TEST_BINARY): tests/p2/test_x11_ime.c tests/x11_video.h | $(BUILD_DIR)/tests/p2
	$(CC) $(CPPFLAGS) $(POSIX_CPPFLAGS) $(X11_CPPFLAGS) \
		$(XTEST_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) \
		$< $(X11_LIBS) $(XTEST_LIBS) $(SANITIZERS) -o $@

$(P2_X11_IME_CHILD_BINARY): tests/p2/x11_ime_child.c | $(BUILD_DIR)/tests/p2
	$(CC) $(CPPFLAGS) $(POSIX_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) \
		$(SANITIZERS) $< $(SANITIZERS) -o $@

$(P2_WIN32_IME_CHILD_BINARY): tests/p2/win32_ime_child.c | $(MINGW_AMD64_BUILD_DIR)/tests/p2
	$(MINGW_AMD64_CC) $(CPPFLAGS) $(MINGW_TERM_CPPFLAGS) $(CFLAGS) \
		$(PROJECT_CFLAGS) $< -municode -o $@

$(P2_WIN32_IME_CONTROL_BINARY): tests/p2/win32_ime_control.c \
		tests/p2/win32_reconversion_control.h | $(MINGW_AMD64_BUILD_DIR)/tests/p2
	$(MINGW_AMD64_CC) $(CPPFLAGS) $(MINGW_TERM_CPPFLAGS) $(CFLAGS) \
		$(PROJECT_CFLAGS) $< -limm32 -luser32 -o $@

$(P2_WIN32_IME_MOVEMENT_BINARY): $(WIN32_EXAMPLE_SOURCES) examples/boxrig_term/win32/application_internal.h examples/boxrig_term/common/color_demo.h examples/boxrig_term/common/scrollbar_demo.h \
		tests/p2/win32_ime_placement_probe.c $(MINGW_AMD64_EXAMPLE_RESOURCE) \
		$(UI_HEADERS) $(WIN32_HEADERS) $(MINGW_AMD64_WIN32_LIBRARY) \
		$(MINGW_AMD64_SESSION_LIBRARY) $(MINGW_AMD64_UI_LIBRARY) \
		$(MINGW_AMD64_LIBRARY) $(CONPTY_HEADER) \
		| $(BUILD_DIR)/boxrig_term/win32/amd64
	$(MINGW_AMD64_CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(MINGW_TERM_CPPFLAGS) \
		$(CFLAGS) $(PROJECT_CFLAGS) $(WIN32_EXAMPLE_SOURCES) \
		tests/p2/win32_ime_placement_probe.c $(MINGW_AMD64_EXAMPLE_RESOURCE) \
		$(MINGW_AMD64_WIN32_LIBRARY) $(MINGW_AMD64_SESSION_LIBRARY) \
		$(MINGW_AMD64_UI_LIBRARY) $(MINGW_AMD64_LIBRARY) \
		-municode -mwindows \
		$(WIN32_RENDERER_LIBS) $(WIN32_HOST_LIBS) $(WIN32_TRANSPARENCY_LIBS) \
		-ladvapi32 -Wl,--wrap=boxrig_win32_input_adapter_create \
		-Wl,--wrap=boxrig_win32_input_adapter_synchronize_candidate \
		-Wl,--wrap=boxrig_win32_input_adapter_resynchronize_candidate -lcomctl32 -o $@

$(P2_WIN32_RECONVERSION_HOST): tests/p2/win32_reconversion_host.c \
		tests/p2/win32_reconversion_control.h \
		$(MINGW_AMD64_WIN32_LIBRARY) $(MINGW_AMD64_UI_LIBRARY) \
		$(MINGW_AMD64_LIBRARY) | $(MINGW_AMD64_BUILD_DIR)/tests/p2
	$(MINGW_AMD64_CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(MINGW_WIN32_CPPFLAGS) \
		$(CFLAGS) $(PROJECT_CFLAGS) $< $(MINGW_AMD64_WIN32_LIBRARY) \
		$(MINGW_AMD64_UI_LIBRARY) $(MINGW_AMD64_LIBRARY) -municode \
		-mwindows $(WIN32_RENDERER_LIBS) -limm32 -luser32 -lshell32 -o $@

$(MINGW_I686_RECONVERSION_HOST): tests/p2/win32_reconversion_host.c \
		tests/p2/win32_reconversion_control.h \
		$(MINGW_I686_WIN32_LIBRARY) $(MINGW_I686_UI_LIBRARY) \
		$(MINGW_I686_LIBRARY) | $(MINGW_I686_BUILD_DIR)/tests/p2
	$(MINGW_I686_CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(MINGW_WIN32_CPPFLAGS) \
		$(CFLAGS) $(PROJECT_CFLAGS) $< $(MINGW_I686_WIN32_LIBRARY) \
		$(MINGW_I686_UI_LIBRARY) $(MINGW_I686_LIBRARY) -municode \
		-mwindows $(WIN32_RENDERER_LIBS) -limm32 -luser32 -lshell32 -o $@

$(P2_WIN32_TRANSPARENCY_RESOURCE): tests/p2/win32_transparency.rc \
	tests/p2/win32_transparency.manifest | $(MINGW_AMD64_BUILD_DIR)/tests/p2
	$(MINGW_AMD64_WINDRES) $(CPPFLAGS) $(MINGW_WIN32_CPPFLAGS) $< -o $@

$(P2_WIN32_TRANSPARENCY_BINARY): tests/p2/win32_transparency_host.c \
	$(P2_WIN32_TRANSPARENCY_RESOURCE) $(MINGW_AMD64_WIN32_LIBRARY) \
	$(MINGW_AMD64_UI_LIBRARY) $(MINGW_AMD64_LIBRARY) | $(MINGW_AMD64_BUILD_DIR)/tests/p2
	$(MINGW_AMD64_CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(MINGW_WIN32_CPPFLAGS) \
		$(CFLAGS) $(PROJECT_CFLAGS) $< $(P2_WIN32_TRANSPARENCY_RESOURCE) \
		$(MINGW_AMD64_WIN32_LIBRARY) $(MINGW_AMD64_UI_LIBRARY) \
		$(MINGW_AMD64_LIBRARY) \
		-municode -mwindows $(WIN32_TRANSPARENCY_LIBS) -ldwmapi -limm32 -o $@

$(MINGW_AMD64_WGL_PROBE): tests/p2/win32_opengl_probe.c tests/win32/wgl_surface_fixture.c \
		src/win32_opengl/surface_private.h include/boxrig/win32/wgl.h include/boxrig/win32/opengl.h \
		$(MINGW_AMD64_WGL_LIBRARY) $(MINGW_AMD64_OGL_LIBRARY) $(MINGW_AMD64_WIN32_LIBRARY) \
		$(MINGW_AMD64_UI_LIBRARY) $(MINGW_AMD64_LIBRARY) | $(MINGW_AMD64_BUILD_DIR)/tests/p2
	$(MINGW_AMD64_CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(MINGW_PROBE_CPPFLAGS) \
		$(CFLAGS) $(PROJECT_CFLAGS) tests/p2/win32_opengl_probe.c tests/win32/wgl_surface_fixture.c \
		$(MINGW_AMD64_WGL_LIBRARY) $(MINGW_AMD64_OGL_LIBRARY) $(MINGW_AMD64_WIN32_LIBRARY) \
		$(MINGW_AMD64_UI_LIBRARY) $(MINGW_AMD64_LIBRARY) $(WIN32_RENDERER_LIBS) -lopengl32 -lm -Wl,--wrap=boxrig_win32_renderer_create -o $@

$(MINGW_I686_WGL_PROBE): tests/p2/win32_opengl_probe.c tests/win32/wgl_surface_fixture.c \
		src/win32_opengl/surface_private.h include/boxrig/win32/wgl.h include/boxrig/win32/opengl.h \
		$(MINGW_I686_WGL_LIBRARY) $(MINGW_I686_OGL_LIBRARY) $(MINGW_I686_WIN32_LIBRARY) \
		$(MINGW_I686_UI_LIBRARY) $(MINGW_I686_LIBRARY) | $(MINGW_I686_BUILD_DIR)/tests/p2
	$(MINGW_I686_CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(MINGW_PROBE_CPPFLAGS) \
		$(CFLAGS) $(PROJECT_CFLAGS) tests/p2/win32_opengl_probe.c tests/win32/wgl_surface_fixture.c \
		$(MINGW_I686_WGL_LIBRARY) $(MINGW_I686_OGL_LIBRARY) $(MINGW_I686_WIN32_LIBRARY) \
		$(MINGW_I686_UI_LIBRARY) $(MINGW_I686_LIBRARY) $(WIN32_RENDERER_LIBS) -lopengl32 -lm -Wl,--wrap=boxrig_win32_renderer_create -o $@

$(PNG_CONFIG): deps/libpng/scripts/pnglibconf.h.prebuilt | $(PNG_CONFIG_DIR)
	sed -e '/^#define PNG_FLOATING_ARITHMETIC_SUPPORTED$$/d' \
		-e '/^#define PNG_FLOATING_POINT_SUPPORTED$$/d' $< > $@

$(EXAMPLE_BINARY): $(EXAMPLE_SOURCES) \
	$(UI_HEADERS) $(X11_HEADERS) include/boxrig/posix.h \
	$(X11_OPENGL_LIBRARY) $(X11_LIBRARY) $(OPENGL_RENDERER_LIBRARY) \
	$(POSIX_LIBRARY) $(SESSION_LIBRARY) $(UI_LIBRARY) $(LIBRARY) \
	| $(BUILD_DIR)/boxrig_term/x11
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(POSIX_URI_CPPFLAGS) $(POSIX_CPPFLAGS) \
		$(X11_CPPFLAGS) $(XEXT_CPPFLAGS) $(DBUS_CPPFLAGS) $(CFLAGS) \
		$(PROJECT_CFLAGS) $(EXAMPLE_SOURCES) \
		$(X11_OPENGL_LIBRARY) $(X11_LIBRARY) $(OPENGL_RENDERER_LIBRARY) \
		$(POSIX_LIBRARY) $(SESSION_LIBRARY) $(UI_LIBRARY) $(LIBRARY) \
		$(X11_LIBS) $(XEXT_LIBS) $(DBUS_LIBS) $(OPENGL_RENDERER_LIBS) \
		-lutil $(POSIX_URI_LIBS) -o $@

$(GTK4_EXAMPLE_BINARY): $(GTK4_EXAMPLE_SOURCES) \
	$(UI_HEADERS) $(GTK4_HEADERS) include/boxrig/posix.h \
	$(GTK4_LIBRARY) $(NOTIFICATION_LIBRARY) $(POSIX_LIBRARY) $(SESSION_LIBRARY) $(UI_LIBRARY) \
	$(LIBRARY) \
	| $(BUILD_DIR)/boxrig_term/gtk4
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(POSIX_URI_CPPFLAGS) $(POSIX_CPPFLAGS) \
		$(GTK4_CPPFLAGS) $(CFLAGS) $(GTK4_PROJECT_CFLAGS) \
		$(GTK4_EXAMPLE_SOURCES) \
		$(GTK4_LIBRARY) $(NOTIFICATION_LIBRARY) $(POSIX_LIBRARY) $(SESSION_LIBRARY) $(UI_LIBRARY) \
		$(LIBRARY) $(GTK4_LIBS) -lutil $(POSIX_URI_LIBS) -o $@

$(GTK3_EXAMPLE_BINARY): $(GTK3_EXAMPLE_SOURCES) \
	examples/boxrig_term/gtk3/geometry_trace.h \
	examples/boxrig_term/gtk3/resize_frame.h \
	$(UI_HEADERS) $(GTK3_HEADERS) include/boxrig/posix.h \
	$(GTK3_LIBRARY) $(NOTIFICATION_LIBRARY) $(POSIX_LIBRARY) $(SESSION_LIBRARY) $(UI_LIBRARY) \
	$(LIBRARY) \
	| $(BUILD_DIR)/boxrig_term/gtk3
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(POSIX_URI_CPPFLAGS) $(POSIX_CPPFLAGS) \
		$(GTK3_CPPFLAGS) $(CFLAGS) $(GTK3_PROJECT_CFLAGS) \
		$(GTK3_EXAMPLE_SOURCES) \
		$(GTK3_LIBRARY) $(NOTIFICATION_LIBRARY) $(POSIX_LIBRARY) $(SESSION_LIBRARY) $(UI_LIBRARY) \
		$(LIBRARY) $(GTK3_LIBS) -lutil $(POSIX_URI_LIBS) -o $@

$(WAYLAND_EXAMPLE_BINARY): $(WAYLAND_EXAMPLE_SOURCES) \
		Makefile \
		$(WAYLAND_EXAMPLE_CONFIG_PREREQUISITE) \
		examples/boxrig_term/wayland/application.h \
		examples/boxrig_term/wayland/application_internal.h \
		examples/boxrig_term/wayland/decoration_policy.h \
		examples/boxrig_term/wayland/root_backing_policy.h \
		examples/boxrig_term/wayland/shortcut_policy.h \
		$(UI_HEADERS) $(WAYLAND_HEADERS) include/boxrig/posix.h \
		include/boxrig/session.h $(WAYLAND_XDG_SHELL_HEADER) \
		$(WAYLAND_XDG_SHELL_SOURCE) $(WAYLAND_XDG_DECORATION_HEADER) \
		$(WAYLAND_XDG_DECORATION_SOURCE) $(WAYLAND_OPENGL_LIBRARY) \
		$(WAYLAND_LIBRARY) $(NOTIFICATION_LIBRARY) $(OPENGL_RENDERER_LIBRARY) \
		$(POSIX_LIBRARY) $(SESSION_LIBRARY) $(UI_LIBRARY) $(LIBRARY) \
		| $(BUILD_DIR)/boxrig_term/wayland
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(POSIX_URI_CPPFLAGS) $(POSIX_CPPFLAGS) \
		-I$(WAYLAND_PROTOCOL_DIR) $(WAYLAND_CLIENT_CPPFLAGS) $(WAYLAND_EXAMPLE_CURSOR_CPPFLAGS) \
		$(WAYLAND_EXAMPLE_LIBDECOR_CPPFLAGS) \
		$(PANGO_CAIRO_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) \
		$(WAYLAND_EXAMPLE_SOURCES) $(WAYLAND_XDG_SHELL_SOURCE) \
		$(WAYLAND_XDG_DECORATION_SOURCE) $(WAYLAND_OPENGL_LIBRARY) \
		$(WAYLAND_LIBRARY) $(NOTIFICATION_LIBRARY) $(OPENGL_RENDERER_LIBRARY) \
		$(POSIX_LIBRARY) $(SESSION_LIBRARY) $(UI_LIBRARY) $(LIBRARY) \
		$(WAYLAND_EXAMPLE_PRIVATE_ARCHIVE_LDFLAGS) \
		$(WAYLAND_CLIENT_LIBS) $(WAYLAND_EXAMPLE_CURSOR_LIBS) $(WAYLAND_OPENGL_LIBS) $(PANGO_CAIRO_LIBS) \
		$(WAYLAND_EXAMPLE_LIBDECOR_LIBS) -lutil $(POSIX_URI_LIBS) -o $@
	@printf 'native Wayland sample libdecor: %s (%s)\n' \
		"$(WAYLAND_LIBDECOR_ENABLED)" "$(WAYLAND_LIBDECOR_VERSION)"
	@printf '%s\n' "$(WAYLAND_LIBDECOR_ENABLED)" \
		>"$(WAYLAND_EXAMPLE_CONFIG_STAMP)"

force-wayland-example-config:

$(WAYLAND_NO_LIBDECOR_TEST_BINARY): $(WAYLAND_EXAMPLE_SOURCES) $(NOTIFICATION_LIBRARY) \
		Makefile \
		examples/boxrig_term/wayland/application.h \
		examples/boxrig_term/wayland/application_internal.h \
		examples/boxrig_term/wayland/decoration_policy.h \
		examples/boxrig_term/wayland/root_backing_policy.h \
		examples/boxrig_term/wayland/shortcut_policy.h \
		$(UI_HEADERS) $(WAYLAND_HEADERS) include/boxrig/posix.h \
		include/boxrig/session.h $(WAYLAND_XDG_SHELL_HEADER) \
		$(WAYLAND_XDG_SHELL_SOURCE) $(WAYLAND_XDG_DECORATION_HEADER) \
		$(WAYLAND_XDG_DECORATION_SOURCE) $(WAYLAND_LIBRARY) \
		$(WAYLAND_OPENGL_LIBRARY) $(OPENGL_RENDERER_LIBRARY) \
		$(POSIX_LIBRARY) $(SESSION_LIBRARY) $(UI_LIBRARY) $(LIBRARY) \
		| $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(POSIX_URI_CPPFLAGS) $(POSIX_CPPFLAGS) \
		-I$(WAYLAND_PROTOCOL_DIR) $(WAYLAND_CLIENT_CPPFLAGS) $(WAYLAND_EXAMPLE_CURSOR_CPPFLAGS) \
		-DBOXRIG_HAVE_LIBDECOR=0 \
		$(PANGO_CAIRO_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) \
		$(WAYLAND_EXAMPLE_SOURCES) $(WAYLAND_XDG_SHELL_SOURCE) \
		$(WAYLAND_XDG_DECORATION_SOURCE) $(WAYLAND_OPENGL_LIBRARY) \
		$(WAYLAND_LIBRARY) $(NOTIFICATION_LIBRARY) $(OPENGL_RENDERER_LIBRARY) \
		$(POSIX_LIBRARY) $(SESSION_LIBRARY) $(UI_LIBRARY) $(LIBRARY) \
		$(WAYLAND_EXAMPLE_PRIVATE_ARCHIVE_LDFLAGS) \
		$(WAYLAND_CLIENT_LIBS) $(WAYLAND_EXAMPLE_CURSOR_LIBS) $(WAYLAND_OPENGL_LIBS) $(PANGO_CAIRO_LIBS) \
		-lutil $(POSIX_URI_LIBS) -o $@

$(BUILD_DIR) $(BUILD_DIR)/core $(BUILD_DIR)/ui $(BUILD_DIR)/opengl $(BUILD_DIR)/x11_opengl $(BUILD_DIR)/wayland_opengl $(BUILD_DIR)/session \
$(BUILD_DIR)/posix $(BUILD_DIR)/x11 $(BUILD_DIR)/gtk4 $(BUILD_DIR)/gtk3 $(BUILD_DIR)/wayland $(BUILD_DIR)/deps/zlib \
$(BUILD_DIR)/deps/libpng $(BUILD_DIR)/tests $(BUILD_DIR)/tests/p2 \
$(TEXT_MEASUREMENT_BUILD_DIR) \
$(PNG_CONFIG_DIR) $(WAYLAND_PROTOCOL_DIR) $(CONPTY_BUILD_DIR) $(CONPTY_BUILD_DIR)/amd64 \
$(CONPTY_BUILD_DIR)/i686 $(CONPTY_BUILD_DIR)/hosts/x86 \
$(CONPTY_BUILD_DIR)/hosts/x64 $(CONPTY_BUILD_DIR)/hosts/arm64 \
$(MINGW_AMD64_BUILD_DIR) $(MINGW_AMD64_BUILD_DIR)/core $(MINGW_AMD64_BUILD_DIR)/ui $(MINGW_AMD64_BUILD_DIR)/session $(MINGW_AMD64_BUILD_DIR)/win32 $(MINGW_AMD64_BUILD_DIR)/tests \
$(MINGW_AMD64_BUILD_DIR)/tests/p2 $(MINGW_AMD64_BUILD_DIR)/deps/zlib \
$(MINGW_AMD64_BUILD_DIR)/deps/libpng $(MINGW_AMD64_BUILD_DIR)/x64 \
$(MINGW_AMD64_BUILD_DIR)/arm64 $(MINGW_AMD64_BUILD_DIR)/tests/x64 \
$(MINGW_AMD64_BUILD_DIR)/tests/arm64 \
$(MINGW_I686_BUILD_DIR) $(MINGW_I686_BUILD_DIR)/core $(MINGW_I686_BUILD_DIR)/ui $(MINGW_I686_BUILD_DIR)/session $(MINGW_I686_BUILD_DIR)/win32 $(MINGW_I686_BUILD_DIR)/tests \
$(MINGW_I686_BUILD_DIR)/tests/p2 $(MINGW_I686_BUILD_DIR)/deps/zlib \
$(MINGW_I686_BUILD_DIR)/deps/libpng \
$(MINGW_I686_BUILD_DIR)/x86 $(MINGW_I686_BUILD_DIR)/x64 \
$(MINGW_I686_BUILD_DIR)/arm64 $(MINGW_I686_BUILD_DIR)/tests/x86 \
$(MINGW_I686_BUILD_DIR)/tests/x64 $(MINGW_I686_BUILD_DIR)/tests/arm64 \
$(BUILD_DIR)/boxrig_term/x11 $(BUILD_DIR)/boxrig_term/gtk4 \
$(BUILD_DIR)/boxrig_term/gtk3 $(BUILD_DIR)/boxrig_term/wayland \
$(BUILD_DIR)/boxrig_term/win32/amd64 \
$(BUILD_DIR)/boxrig_term/win32/amd64/x64 $(BUILD_DIR)/boxrig_term/win32/amd64/arm64 \
$(BUILD_DIR)/boxrig_term/win32/i686 $(BUILD_DIR)/boxrig_term/win32/i686/x86 \
$(BUILD_DIR)/boxrig_term/win32/i686/x64 $(BUILD_DIR)/boxrig_term/win32/i686/arm64 \
$(BUILD_DIR)/boxrig_emitter/posix \
$(BUILD_DIR)/boxrig_emitter/win32/amd64 $(BUILD_DIR)/boxrig_emitter/win32/i686:
	mkdir -p $@

clean:
	rm -rf $(BUILD_DIR)

.PHONY: win32-core-drivers test-win32-core-artifacts
win32-core-drivers: $(MINGW_AMD64_CORE_TEST) $(MINGW_I686_CORE_TEST) \
	$(MINGW_AMD64_GRAPHICS_TEST) $(MINGW_I686_GRAPHICS_TEST) \
	$(MINGW_AMD64_BUILD_DIR)/tests/test_custom_ui.exe \
	$(MINGW_I686_BUILD_DIR)/tests/test_custom_ui.exe

$(MINGW_AMD64_BUILD_DIR)/tests/test_custom_ui.exe: \
		tests/test_custom_ui.c tests/win32/core_target.h Makefile \
		$(MINGW_AMD64_UI_LIBRARY) $(MINGW_AMD64_LIBRARY) \
		| $(MINGW_AMD64_BUILD_DIR)/tests
	$(MINGW_AMD64_CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) \
		$(MINGW_AMD64_CORE_CPPFLAGS) -include tests/win32/core_target.h \
		$(CFLAGS) $(PROJECT_CFLAGS) $< $(MINGW_AMD64_UI_LIBRARY) \
		$(MINGW_AMD64_LIBRARY) $(MINGW_AMD64_CORE_LDFLAGS) -o $@

$(MINGW_I686_BUILD_DIR)/tests/test_custom_ui.exe: \
		tests/test_custom_ui.c tests/win32/core_target.h Makefile \
		$(MINGW_I686_UI_LIBRARY) $(MINGW_I686_LIBRARY) \
		| $(MINGW_I686_BUILD_DIR)/tests
	$(MINGW_I686_CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) \
		$(MINGW_I686_CORE_CPPFLAGS) -include tests/win32/core_target.h \
		$(CFLAGS) $(PROJECT_CFLAGS) $< $(MINGW_I686_UI_LIBRARY) \
		$(MINGW_I686_LIBRARY) $(MINGW_I686_CORE_LDFLAGS) -o $@

test-win32-core-artifacts: win32-core-drivers
	sh tests/win32/check_core_pe.sh x86_64-w64-mingw32-objdump amd64 \
		$(BUILD_DIR)/test-results/win32-core/amd64 \
		$(MINGW_AMD64_CORE_TEST) $(MINGW_AMD64_GRAPHICS_TEST) \
		$(MINGW_AMD64_BUILD_DIR)/tests/test_custom_ui.exe
	sh tests/win32/check_core_pe.sh i686-w64-mingw32-objdump i686 \
		$(BUILD_DIR)/test-results/win32-core/i686 \
		$(MINGW_I686_CORE_TEST) $(MINGW_I686_GRAPHICS_TEST) \
		$(MINGW_I686_BUILD_DIR)/tests/test_custom_ui.exe

$(MINGW_AMD64_BUILD_DIR)/tests/basic_ui_manifest.o: \
		tests/win32/basic_ui.rc tests/win32/basic_ui.manifest \
		| $(MINGW_AMD64_BUILD_DIR)/tests
	$(MINGW_AMD64_WINDRES) -Itests/win32 $< -o $@

$(MINGW_AMD64_BUILD_DIR)/tests/test_basic_ui.exe: \
		tests/win32/test_win32_component_fixture.c tests/win32/base_directwrite.c \
		$(FRONTEND_COMPONENT_FIXTURE_SOURCES) $(FRONTEND_COMPONENT_FIXTURE_HEADER) \
		$(MINGW_AMD64_BUILD_DIR)/tests/basic_ui_manifest.o \
		src/win32/renderer_private.h Makefile $(MINGW_AMD64_WIN32_LIBRARY) \
		$(MINGW_AMD64_UI_LIBRARY) $(MINGW_AMD64_LIBRARY)
	$(MINGW_AMD64_CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) -Isrc/win32 \
		$(MINGW_CPPFLAGS) -DWINVER=0x0600 -D_WIN32_WINNT=0x0600 \
		-DNTDDI_VERSION=0x06000200 $(CFLAGS) $(PROJECT_CFLAGS) \
		tests/win32/test_win32_component_fixture.c tests/win32/base_directwrite.c \
		$(FRONTEND_COMPONENT_FIXTURE_SOURCES) \
		$(MINGW_AMD64_BUILD_DIR)/tests/basic_ui_manifest.o \
		$(MINGW_AMD64_WIN32_LIBRARY) $(MINGW_AMD64_UI_LIBRARY) \
		$(MINGW_AMD64_LIBRARY) $(WIN32_RENDERER_LIBS) \
		-Wl,--major-os-version,6,--minor-os-version,0 \
		-Wl,--major-subsystem-version,6,--minor-subsystem-version,0 \
		-Wl,--wrap=boxrig_win32_renderer_create -o $@

$(MINGW_I686_BUILD_DIR)/tests/basic_ui_manifest.o: \
		tests/win32/basic_ui.rc tests/win32/basic_ui.manifest \
		| $(MINGW_I686_BUILD_DIR)/tests
	$(MINGW_I686_WINDRES) -Itests/win32 $< -o $@

$(MINGW_I686_BUILD_DIR)/tests/test_basic_ui.exe: \
		tests/win32/test_win32_component_fixture.c tests/win32/base_directwrite.c \
		$(FRONTEND_COMPONENT_FIXTURE_SOURCES) $(FRONTEND_COMPONENT_FIXTURE_HEADER) \
		$(MINGW_I686_BUILD_DIR)/tests/basic_ui_manifest.o \
		src/win32/renderer_private.h Makefile $(MINGW_I686_WIN32_LIBRARY) \
		$(MINGW_I686_UI_LIBRARY) $(MINGW_I686_LIBRARY)
	$(MINGW_I686_CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) -Isrc/win32 \
		$(MINGW_CPPFLAGS) -DWINVER=0x0600 -D_WIN32_WINNT=0x0600 \
		-DNTDDI_VERSION=0x06000200 $(CFLAGS) $(PROJECT_CFLAGS) \
		tests/win32/test_win32_component_fixture.c tests/win32/base_directwrite.c \
		$(FRONTEND_COMPONENT_FIXTURE_SOURCES) \
		$(MINGW_I686_BUILD_DIR)/tests/basic_ui_manifest.o \
		$(MINGW_I686_WIN32_LIBRARY) $(MINGW_I686_UI_LIBRARY) \
		$(MINGW_I686_LIBRARY) $(WIN32_RENDERER_LIBS) \
		-Wl,--major-os-version,6,--minor-os-version,0 \
		-Wl,--major-subsystem-version,6,--minor-subsystem-version,0 \
		-Wl,--wrap=boxrig_win32_renderer_create -o $@

$(MINGW_AMD64_BUILD_DIR)/tests/test_legacy_host.exe: \
		tests/win32/test_win32_legacy_host.c src/win32/transparency_private.h \
		$(MINGW_AMD64_WIN32_LIBRARY) $(MINGW_AMD64_UI_LIBRARY) \
		$(MINGW_AMD64_LIBRARY) Makefile | $(MINGW_AMD64_BUILD_DIR)/tests
	$(MINGW_AMD64_CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) -Isrc/win32 \
		$(MINGW_WIN32_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $< \
		$(MINGW_AMD64_WIN32_LIBRARY) $(MINGW_AMD64_UI_LIBRARY) \
		$(MINGW_AMD64_LIBRARY) $(WIN32_TRANSPARENCY_LIBS) -o $@

$(MINGW_I686_BUILD_DIR)/tests/test_legacy_host.exe: \
		tests/win32/test_win32_legacy_host.c src/win32/transparency_private.h \
		$(MINGW_I686_WIN32_LIBRARY) $(MINGW_I686_UI_LIBRARY) \
		$(MINGW_I686_LIBRARY) Makefile | $(MINGW_I686_BUILD_DIR)/tests
	$(MINGW_I686_CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) -Isrc/win32 \
		$(MINGW_WIN32_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $< \
		$(MINGW_I686_WIN32_LIBRARY) $(MINGW_I686_UI_LIBRARY) \
		$(MINGW_I686_LIBRARY) $(WIN32_TRANSPARENCY_LIBS) -o $@

.PHONY: win32-ui-drivers test-win32-ui-artifacts
win32-ui-drivers: $(MINGW_AMD64_BUILD_DIR)/tests/test_basic_ui.exe \
	$(MINGW_I686_BUILD_DIR)/tests/test_basic_ui.exe \
	$(MINGW_AMD64_BUILD_DIR)/tests/test_legacy_host.exe \
	$(MINGW_I686_BUILD_DIR)/tests/test_legacy_host.exe \
	$(MINGW_AMD64_HOST_TEST) $(MINGW_I686_HOST_TEST)

test-win32-ui-artifacts: win32-ui-drivers
	sh tests/win32/check_ui_pe.sh x86_64-w64-mingw32-objdump amd64 \
		$(BUILD_DIR)/test-results/win32-ui/amd64 \
		$(MINGW_AMD64_BUILD_DIR)/tests/test_basic_ui.exe \
		$(MINGW_AMD64_BUILD_DIR)/tests/test_legacy_host.exe $(MINGW_AMD64_HOST_TEST)
	sh tests/win32/check_ui_pe.sh i686-w64-mingw32-objdump i686 \
		$(BUILD_DIR)/test-results/win32-ui/i686 \
		$(MINGW_I686_BUILD_DIR)/tests/test_basic_ui.exe \
		$(MINGW_I686_BUILD_DIR)/tests/test_legacy_host.exe $(MINGW_I686_HOST_TEST)

$(MINGW_AMD64_BUILD_DIR)/win32/renderer.o $(MINGW_I686_BUILD_DIR)/win32/renderer.o: src/win32/text_private.h src/win32/text_state.h src/opengl/text_provider.h

# Installed public ABI and binary dependency checks run in the complete suite.

.PHONY: test-wgl-package-amd64
test: test-wgl-package-amd64
test-wgl-package-amd64: tests/win32/test_wgl_package.c tools/check-wgl-package.mjs win32-opengl-amd64
	$(MAKE) install-win32-opengl-amd64 PREFIX=$(abspath $(BUILD_DIR)/tests/wgl-package-amd64)/usr
	$(MINGW_AMD64_CC) $(CFLAGS) $(PROJECT_CFLAGS) tests/win32/test_wgl_package.c $$(PKG_CONFIG_LIBDIR=$(abspath $(BUILD_DIR)/tests/wgl-package-amd64)/usr/lib/pkgconfig pkg-config --cflags boxrig-win32-ogl) -Wl,-Bstatic $$(PKG_CONFIG_LIBDIR=$(abspath $(BUILD_DIR)/tests/wgl-package-amd64)/usr/lib/pkgconfig pkg-config --libs --static boxrig-win32-ogl) -Wl,-Bdynamic -o $(abspath $(BUILD_DIR)/tests/wgl-package-amd64)/test_wgl_package-static.exe
	$(MINGW_AMD64_CC) $(CFLAGS) $(PROJECT_CFLAGS) tests/win32/test_wgl_package.c $$(PKG_CONFIG_LIBDIR=$(abspath $(BUILD_DIR)/tests/wgl-package-amd64)/usr/lib/pkgconfig pkg-config --cflags --libs boxrig-win32-ogl) -o $(abspath $(BUILD_DIR)/tests/wgl-package-amd64)/test_wgl_package-shared.exe
	cp $(abspath $(BUILD_DIR)/tests/wgl-package-amd64)/usr/bin/*.dll $(abspath $(BUILD_DIR)/tests/wgl-package-amd64)/usr/lib/*.a $(abspath $(BUILD_DIR)/tests/wgl-package-amd64)
	node tools/check-wgl-package.mjs x86_64-w64-mingw32-objdump $(MINGW_AMD64_NM) $(abspath $(BUILD_DIR)/tests/wgl-package-amd64) $(BUILD_DIR)/test-results/step20-8/package-amd64

.PHONY: test-wgl-package-i686
test: test-wgl-package-i686
test-wgl-package-i686: tests/win32/test_wgl_package.c tools/check-wgl-package.mjs win32-opengl-i686
	$(MAKE) install-win32-opengl-i686 PREFIX=$(abspath $(BUILD_DIR)/tests/wgl-package-i686)/usr
	$(MINGW_I686_CC) $(CFLAGS) $(PROJECT_CFLAGS) tests/win32/test_wgl_package.c $$(PKG_CONFIG_LIBDIR=$(abspath $(BUILD_DIR)/tests/wgl-package-i686)/usr/lib/pkgconfig pkg-config --cflags boxrig-win32-ogl) -Wl,-Bstatic $$(PKG_CONFIG_LIBDIR=$(abspath $(BUILD_DIR)/tests/wgl-package-i686)/usr/lib/pkgconfig pkg-config --libs --static boxrig-win32-ogl) -Wl,-Bdynamic -o $(abspath $(BUILD_DIR)/tests/wgl-package-i686)/test_wgl_package-static.exe
	$(MINGW_I686_CC) $(CFLAGS) $(PROJECT_CFLAGS) tests/win32/test_wgl_package.c $$(PKG_CONFIG_LIBDIR=$(abspath $(BUILD_DIR)/tests/wgl-package-i686)/usr/lib/pkgconfig pkg-config --cflags --libs boxrig-win32-ogl) -o $(abspath $(BUILD_DIR)/tests/wgl-package-i686)/test_wgl_package-shared.exe
	cp $(abspath $(BUILD_DIR)/tests/wgl-package-i686)/usr/bin/*.dll $(abspath $(BUILD_DIR)/tests/wgl-package-i686)/usr/lib/*.a $(abspath $(BUILD_DIR)/tests/wgl-package-i686)
	node tools/check-wgl-package.mjs i686-w64-mingw32-objdump $(MINGW_I686_NM) $(abspath $(BUILD_DIR)/tests/wgl-package-i686) $(BUILD_DIR)/test-results/step20-8/package-i686

$(MINGW_AMD64_BUILD_DIR)/tests/test_win32_opengl_component_fixture.exe: tests/win32/test_win32_component_fixture.c $(FRONTEND_COMPONENT_FIXTURE_SOURCES) $(FRONTEND_COMPONENT_FIXTURE_HEADER) $(MINGW_AMD64_WGL_LIBRARY) $(MINGW_AMD64_OGL_LIBRARY) $(MINGW_AMD64_WIN32_LIBRARY) $(MINGW_AMD64_UI_LIBRARY) $(MINGW_AMD64_LIBRARY)
	$(MINGW_AMD64_CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(MINGW_WIN32_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) -DBOXRIG_TEST_OPENGL_COMPONENT tests/win32/test_win32_component_fixture.c $(FRONTEND_COMPONENT_FIXTURE_SOURCES) $(MINGW_AMD64_WGL_LIBRARY) $(MINGW_AMD64_OGL_LIBRARY) $(MINGW_AMD64_WIN32_LIBRARY) $(MINGW_AMD64_UI_LIBRARY) $(MINGW_AMD64_LIBRARY) $(WIN32_RENDERER_LIBS) -lcomctl32 -lopengl32 -o $@

$(MINGW_I686_BUILD_DIR)/tests/test_win32_opengl_component_fixture.exe: tests/win32/test_win32_component_fixture.c $(FRONTEND_COMPONENT_FIXTURE_SOURCES) $(FRONTEND_COMPONENT_FIXTURE_HEADER) $(MINGW_I686_WGL_LIBRARY) $(MINGW_I686_OGL_LIBRARY) $(MINGW_I686_WIN32_LIBRARY) $(MINGW_I686_UI_LIBRARY) $(MINGW_I686_LIBRARY)
	$(MINGW_I686_CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(MINGW_WIN32_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) -DBOXRIG_TEST_OPENGL_COMPONENT tests/win32/test_win32_component_fixture.c $(FRONTEND_COMPONENT_FIXTURE_SOURCES) $(MINGW_I686_WGL_LIBRARY) $(MINGW_I686_OGL_LIBRARY) $(MINGW_I686_WIN32_LIBRARY) $(MINGW_I686_UI_LIBRARY) $(MINGW_I686_LIBRARY) $(WIN32_RENDERER_LIBS) -lcomctl32 -lopengl32 -o $@

$(MINGW_AMD64_BUILD_DIR)/win32/component.o $(MINGW_I686_BUILD_DIR)/win32/component.o: src/win32/component_private.h

$(MINGW_AMD64_BUILD_DIR)/wgl/component.o $(MINGW_I686_BUILD_DIR)/wgl/component.o $(MINGW_AMD64_BUILD_DIR)/opengl/text_provider.o $(MINGW_I686_BUILD_DIR)/opengl/text_provider.o: src/win32_opengl/component_private.h src/win32/component_private.h include/boxrig/win32/opengl_component.h

$(MINGW_AMD64_BUILD_DIR)/tests/test_win32_opengl_component.exe: tests/win32/test_win32_opengl_component.c tests/win32/opengl_component_capture.h src/win32_opengl/component_private.h $(MINGW_AMD64_WGL_LIBRARY) $(MINGW_AMD64_OGL_LIBRARY) $(MINGW_AMD64_WIN32_LIBRARY) $(MINGW_AMD64_UI_LIBRARY) $(MINGW_AMD64_LIBRARY)
	$(MINGW_AMD64_CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(MINGW_WIN32_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $< $(MINGW_AMD64_WGL_LIBRARY) $(MINGW_AMD64_OGL_LIBRARY) $(MINGW_AMD64_WIN32_LIBRARY) $(MINGW_AMD64_UI_LIBRARY) $(MINGW_AMD64_LIBRARY) $(WIN32_RENDERER_LIBS) -lcomctl32 -lopengl32 -Wl,--wrap=boxrig_wgl_resolve_procedure -Wl,--wrap=boxrig_wgl_surface_create -Wl,--wrap=boxrig_wgl_surface_make_current -Wl,--wrap=boxrig_wgl_surface_swap -Wl,--wrap=boxrig_win32_input_adapter_create -Wl,--wrap=boxrig_ogl_renderer_lifecycle_create -Wl,--wrap=boxrig_win32_renderer_create -Wl,--wrap=boxrig_wgl_surface_get_info -Wl,--wrap=boxrig_ogl_renderer_create_with_win32_font -o $@

$(MINGW_I686_BUILD_DIR)/tests/test_win32_opengl_component.exe: tests/win32/test_win32_opengl_component.c tests/win32/opengl_component_capture.h src/win32_opengl/component_private.h $(MINGW_I686_WGL_LIBRARY) $(MINGW_I686_OGL_LIBRARY) $(MINGW_I686_WIN32_LIBRARY) $(MINGW_I686_UI_LIBRARY) $(MINGW_I686_LIBRARY)
	$(MINGW_I686_CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(MINGW_WIN32_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $< $(MINGW_I686_WGL_LIBRARY) $(MINGW_I686_OGL_LIBRARY) $(MINGW_I686_WIN32_LIBRARY) $(MINGW_I686_UI_LIBRARY) $(MINGW_I686_LIBRARY) $(WIN32_RENDERER_LIBS) -lcomctl32 -lopengl32 -Wl,--wrap=boxrig_wgl_resolve_procedure -Wl,--wrap=boxrig_wgl_surface_create -Wl,--wrap=boxrig_wgl_surface_make_current -Wl,--wrap=boxrig_wgl_surface_swap -Wl,--wrap=boxrig_win32_input_adapter_create -Wl,--wrap=boxrig_ogl_renderer_lifecycle_create -Wl,--wrap=boxrig_win32_renderer_create -Wl,--wrap=boxrig_wgl_surface_get_info -Wl,--wrap=boxrig_ogl_renderer_create_with_win32_font -o $@

$(BUILD_DIR)/boxrig_term/win32/amd64/boxrig_term_ogl.exe: $(WIN32_EXAMPLE_SOURCES) examples/boxrig_term/win32/application_internal.h examples/boxrig_term/common/color_demo.h examples/boxrig_term/common/scrollbar_demo.h tests/win32/opengl_sample.c  $(MINGW_AMD64_EXAMPLE_RESOURCE) $(MINGW_AMD64_WGL_LIBRARY) $(MINGW_AMD64_OGL_LIBRARY) $(MINGW_AMD64_WIN32_LIBRARY) $(MINGW_AMD64_SESSION_LIBRARY) $(MINGW_AMD64_UI_LIBRARY) $(MINGW_AMD64_LIBRARY) $(CONPTY_HEADER)
	$(MINGW_AMD64_CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(MINGW_TERM_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) -DBOXRIG_TERM_ENABLE_OPENGL $(WIN32_EXAMPLE_SOURCES) tests/win32/opengl_sample.c  $(MINGW_AMD64_EXAMPLE_RESOURCE) $(MINGW_AMD64_WGL_LIBRARY) $(MINGW_AMD64_OGL_LIBRARY) $(MINGW_AMD64_WIN32_LIBRARY) $(MINGW_AMD64_SESSION_LIBRARY) $(MINGW_AMD64_UI_LIBRARY) $(MINGW_AMD64_LIBRARY) -municode -mwindows $(WIN32_RENDERER_LIBS) $(WIN32_HOST_LIBS) $(WIN32_TRANSPARENCY_LIBS) -lcomctl32 -lopengl32 -ladvapi32 -Wl,--wrap=boxrig_term_win32_run -Wl,--wrap=boxrig_win32_opengl_terminal_component_create_child  -lcomctl32 -o $@
test-p2-win32: $(BUILD_DIR)/boxrig_term/win32/amd64/boxrig_term_ogl.exe

$(BUILD_DIR)/boxrig_term/win32/amd64/boxrig_term_ogl_ime_probe.exe: $(WIN32_EXAMPLE_SOURCES) examples/boxrig_term/win32/application_internal.h examples/boxrig_term/common/color_demo.h examples/boxrig_term/common/scrollbar_demo.h tests/win32/opengl_sample.c tests/p2/win32_ime_placement_probe.c $(MINGW_AMD64_EXAMPLE_RESOURCE) $(MINGW_AMD64_WGL_LIBRARY) $(MINGW_AMD64_OGL_LIBRARY) $(MINGW_AMD64_WIN32_LIBRARY) $(MINGW_AMD64_SESSION_LIBRARY) $(MINGW_AMD64_UI_LIBRARY) $(MINGW_AMD64_LIBRARY) $(CONPTY_HEADER)
	$(MINGW_AMD64_CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(MINGW_TERM_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) -DBOXRIG_TERM_ENABLE_OPENGL $(WIN32_EXAMPLE_SOURCES) tests/win32/opengl_sample.c tests/p2/win32_ime_placement_probe.c $(MINGW_AMD64_EXAMPLE_RESOURCE) $(MINGW_AMD64_WGL_LIBRARY) $(MINGW_AMD64_OGL_LIBRARY) $(MINGW_AMD64_WIN32_LIBRARY) $(MINGW_AMD64_SESSION_LIBRARY) $(MINGW_AMD64_UI_LIBRARY) $(MINGW_AMD64_LIBRARY) -municode -mwindows $(WIN32_RENDERER_LIBS) $(WIN32_HOST_LIBS) $(WIN32_TRANSPARENCY_LIBS) -lcomctl32 -lopengl32 -ladvapi32 -Wl,--wrap=boxrig_term_win32_run -Wl,--wrap=boxrig_win32_opengl_terminal_component_create_child -Wl,--wrap=boxrig_win32_input_adapter_create -Wl,--wrap=boxrig_win32_input_adapter_synchronize_candidate -Wl,--wrap=boxrig_win32_input_adapter_resynchronize_candidate -lcomctl32 -o $@
test-p2-win32: $(BUILD_DIR)/boxrig_term/win32/amd64/boxrig_term_ogl_ime_probe.exe

$(BUILD_DIR)/boxrig_term/win32/i686/boxrig_term_ogl.exe: $(WIN32_EXAMPLE_SOURCES) examples/boxrig_term/win32/application_internal.h examples/boxrig_term/common/color_demo.h examples/boxrig_term/common/scrollbar_demo.h tests/win32/opengl_sample.c  $(MINGW_I686_EXAMPLE_RESOURCE) $(MINGW_I686_WGL_LIBRARY) $(MINGW_I686_OGL_LIBRARY) $(MINGW_I686_WIN32_LIBRARY) $(MINGW_I686_SESSION_LIBRARY) $(MINGW_I686_UI_LIBRARY) $(MINGW_I686_LIBRARY) $(CONPTY_HEADER)
	$(MINGW_I686_CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(MINGW_TERM_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) -DBOXRIG_TERM_ENABLE_OPENGL $(WIN32_EXAMPLE_SOURCES) tests/win32/opengl_sample.c  $(MINGW_I686_EXAMPLE_RESOURCE) $(MINGW_I686_WGL_LIBRARY) $(MINGW_I686_OGL_LIBRARY) $(MINGW_I686_WIN32_LIBRARY) $(MINGW_I686_SESSION_LIBRARY) $(MINGW_I686_UI_LIBRARY) $(MINGW_I686_LIBRARY) -municode -mwindows $(WIN32_RENDERER_LIBS) $(WIN32_HOST_LIBS) $(WIN32_TRANSPARENCY_LIBS) -lcomctl32 -lopengl32 -ladvapi32 -Wl,--wrap=boxrig_term_win32_run -Wl,--wrap=boxrig_win32_opengl_terminal_component_create_child  -lcomctl32 -o $@
test-p2-win32: $(BUILD_DIR)/boxrig_term/win32/i686/boxrig_term_ogl.exe

$(BUILD_DIR)/boxrig_term/win32/i686/boxrig_term_ogl_ime_probe.exe: $(WIN32_EXAMPLE_SOURCES) examples/boxrig_term/win32/application_internal.h examples/boxrig_term/common/color_demo.h examples/boxrig_term/common/scrollbar_demo.h tests/win32/opengl_sample.c tests/p2/win32_ime_placement_probe.c $(MINGW_I686_EXAMPLE_RESOURCE) $(MINGW_I686_WGL_LIBRARY) $(MINGW_I686_OGL_LIBRARY) $(MINGW_I686_WIN32_LIBRARY) $(MINGW_I686_SESSION_LIBRARY) $(MINGW_I686_UI_LIBRARY) $(MINGW_I686_LIBRARY) $(CONPTY_HEADER)
	$(MINGW_I686_CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(MINGW_TERM_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) -DBOXRIG_TERM_ENABLE_OPENGL $(WIN32_EXAMPLE_SOURCES) tests/win32/opengl_sample.c tests/p2/win32_ime_placement_probe.c $(MINGW_I686_EXAMPLE_RESOURCE) $(MINGW_I686_WGL_LIBRARY) $(MINGW_I686_OGL_LIBRARY) $(MINGW_I686_WIN32_LIBRARY) $(MINGW_I686_SESSION_LIBRARY) $(MINGW_I686_UI_LIBRARY) $(MINGW_I686_LIBRARY) -municode -mwindows $(WIN32_RENDERER_LIBS) $(WIN32_HOST_LIBS) $(WIN32_TRANSPARENCY_LIBS) -lcomctl32 -lopengl32 -ladvapi32 -Wl,--wrap=boxrig_term_win32_run -Wl,--wrap=boxrig_win32_opengl_terminal_component_create_child -Wl,--wrap=boxrig_win32_input_adapter_create -Wl,--wrap=boxrig_win32_input_adapter_synchronize_candidate -Wl,--wrap=boxrig_win32_input_adapter_resynchronize_candidate -lcomctl32 -o $@
test-p2-win32: $(BUILD_DIR)/boxrig_term/win32/i686/boxrig_term_ogl_ime_probe.exe


# Keep the renderer-only example responsive while GPU completion is pending.
$(MINGW_AMD64_BUILD_DIR)/tests/test_renderer_example.exe: tests/win32/test_renderer_example.c examples/win32/renderer.c $(MINGW_AMD64_WGL_LIBRARY) $(MINGW_AMD64_OGL_LIBRARY) $(MINGW_AMD64_WIN32_LIBRARY) $(MINGW_AMD64_UI_LIBRARY) $(MINGW_AMD64_LIBRARY)
	$(MINGW_AMD64_CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(MINGW_WIN32_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $< $(MINGW_AMD64_WGL_LIBRARY) $(MINGW_AMD64_OGL_LIBRARY) $(MINGW_AMD64_WIN32_LIBRARY) $(MINGW_AMD64_UI_LIBRARY) $(MINGW_AMD64_LIBRARY) $(WIN32_RENDERER_LIBS) -lcomctl32 -lopengl32 -o $@

$(MINGW_I686_BUILD_DIR)/tests/test_renderer_example.exe: tests/win32/test_renderer_example.c examples/win32/renderer.c $(MINGW_I686_WGL_LIBRARY) $(MINGW_I686_OGL_LIBRARY) $(MINGW_I686_WIN32_LIBRARY) $(MINGW_I686_UI_LIBRARY) $(MINGW_I686_LIBRARY)
	$(MINGW_I686_CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(MINGW_WIN32_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $< $(MINGW_I686_WGL_LIBRARY) $(MINGW_I686_OGL_LIBRARY) $(MINGW_I686_WIN32_LIBRARY) $(MINGW_I686_UI_LIBRARY) $(MINGW_I686_LIBRARY) $(WIN32_RENDERER_LIBS) -lcomctl32 -lopengl32 -o $@

.PHONY: test-win32-renderer-example
test: test-win32-renderer-example
test-win32-renderer-example: $(MINGW_AMD64_BUILD_DIR)/tests/test_renderer_example.exe $(MINGW_I686_BUILD_DIR)/tests/test_renderer_example.exe
	$(WINE_RUN) $(abspath $(MINGW_AMD64_BUILD_DIR)/tests/test_renderer_example.exe)
	$(WINE_RUN) $(abspath $(MINGW_I686_BUILD_DIR)/tests/test_renderer_example.exe)

.PHONY: test-win32-sdk
test: test-win32-sdk
test-win32-sdk: win32-opengl
	sh tests/win32/check_sdk.sh amd64 $(MINGW_AMD64_CC) x86_64-w64-mingw32-objdump
	sh tests/win32/check_sdk.sh i686 $(MINGW_I686_CC) i686-w64-mingw32-objdump

# Basic Win32 SDK installation has no optional renderer dependencies.
WIN32_SOFTWARE_HEADERS := $(filter-out include/boxrig/win32/d3d11_component.h include/boxrig/win32/opengl.h include/boxrig/win32/opengl_component.h include/boxrig/win32/wgl.h,$(wildcard include/boxrig/win32/*.h))

.PHONY: install-win32-amd64
install-win32-amd64: $(MINGW_AMD64_WIN32_LIBRARY) $(MINGW_AMD64_UI_LIBRARY) $(MINGW_AMD64_LIBRARY) $(MINGW_AMD64_SESSION_LIBRARY) examples/win32/component.c examples/win32/host_opacity.c examples/win32/pty.c packaging/win32/boxrig-win32.pc
	$(INSTALL) -d $(DESTDIR)$(LIBDIR) $(DESTDIR)$(PKGCONFIGDIR) $(DESTDIR)$(INCLUDEDIR)/boxrig/ui $(DESTDIR)$(INCLUDEDIR)/boxrig/win32 $(DESTDIR)$(PREFIX)/share/boxrig/examples/win32
	$(INSTALL) -m 644 $(MINGW_AMD64_WIN32_LIBRARY) $(MINGW_AMD64_UI_LIBRARY) $(MINGW_AMD64_LIBRARY) $(MINGW_AMD64_SESSION_LIBRARY) $(DESTDIR)$(LIBDIR)
	$(INSTALL) -m 644 $(CORE_PUBLIC_HEADERS) include/boxrig/ui.h include/boxrig/session.h include/boxrig/win32.h $(DESTDIR)$(INCLUDEDIR)/boxrig
	$(INSTALL) -m 644 $(wildcard include/boxrig/ui/*.h) $(DESTDIR)$(INCLUDEDIR)/boxrig/ui
	$(INSTALL) -m 644 $(WIN32_SOFTWARE_HEADERS) $(DESTDIR)$(INCLUDEDIR)/boxrig/win32
	$(INSTALL) -m 644 packaging/win32/boxrig-win32.pc $(DESTDIR)$(PKGCONFIGDIR)
	$(INSTALL) -m 644 examples/win32/component.c examples/win32/host_opacity.c examples/win32/pty.c $(DESTDIR)$(PREFIX)/share/boxrig/examples/win32
	$(INSTALL) -m 644 examples/win32/external_scrollbar.h $(DESTDIR)$(PREFIX)/share/boxrig/examples/win32

install-win32-opengl-amd64: install-win32-amd64

.PHONY: install-win32-i686
install-win32-i686: $(MINGW_I686_WIN32_LIBRARY) $(MINGW_I686_UI_LIBRARY) $(MINGW_I686_LIBRARY) $(MINGW_I686_SESSION_LIBRARY) examples/win32/component.c examples/win32/host_opacity.c examples/win32/pty.c packaging/win32/boxrig-win32.pc
	$(INSTALL) -d $(DESTDIR)$(LIBDIR) $(DESTDIR)$(PKGCONFIGDIR) $(DESTDIR)$(INCLUDEDIR)/boxrig/ui $(DESTDIR)$(INCLUDEDIR)/boxrig/win32 $(DESTDIR)$(PREFIX)/share/boxrig/examples/win32
	$(INSTALL) -m 644 $(MINGW_I686_WIN32_LIBRARY) $(MINGW_I686_UI_LIBRARY) $(MINGW_I686_LIBRARY) $(MINGW_I686_SESSION_LIBRARY) $(DESTDIR)$(LIBDIR)
	$(INSTALL) -m 644 $(CORE_PUBLIC_HEADERS) include/boxrig/ui.h include/boxrig/session.h include/boxrig/win32.h $(DESTDIR)$(INCLUDEDIR)/boxrig
	$(INSTALL) -m 644 $(wildcard include/boxrig/ui/*.h) $(DESTDIR)$(INCLUDEDIR)/boxrig/ui
	$(INSTALL) -m 644 $(WIN32_SOFTWARE_HEADERS) $(DESTDIR)$(INCLUDEDIR)/boxrig/win32
	$(INSTALL) -m 644 packaging/win32/boxrig-win32.pc $(DESTDIR)$(PKGCONFIGDIR)
	$(INSTALL) -m 644 examples/win32/component.c examples/win32/host_opacity.c examples/win32/pty.c $(DESTDIR)$(PREFIX)/share/boxrig/examples/win32
	$(INSTALL) -m 644 examples/win32/external_scrollbar.h $(DESTDIR)$(PREFIX)/share/boxrig/examples/win32

install-win32-opengl-i686: install-win32-i686

test-p2-win32: test-win32-sdk $(MINGW_AMD64_BUILD_DIR)/tests/test_pty_runtime.exe $(MINGW_I686_BUILD_DIR)/tests/test_pty_runtime.exe

# Step 20-11 is a private experiment; no installed library acquires DirectX imports.
D3D11_PROTOTYPE_SOURCES := tests/win32/test_d3d11_prototype.c tests/win32/d3d11_prototype.c
D3D11_PROTOTYPE_HEADERS := tests/win32/d3d11_prototype.h src/win32/text_private.h
D3D11_PROTOTYPE_AMD64 := $(MINGW_AMD64_BUILD_DIR)/tests/test_d3d11_prototype.exe
D3D11_PROTOTYPE_I686 := $(MINGW_I686_BUILD_DIR)/tests/test_d3d11_prototype.exe

$(D3D11_PROTOTYPE_AMD64): $(D3D11_PROTOTYPE_SOURCES) $(D3D11_PROTOTYPE_HEADERS) $(MINGW_AMD64_WIN32_LIBRARY) $(MINGW_AMD64_UI_LIBRARY) $(MINGW_AMD64_LIBRARY)
	$(MINGW_AMD64_CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(MINGW_PROBE_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(D3D11_PROTOTYPE_SOURCES) $(MINGW_AMD64_WIN32_LIBRARY) $(MINGW_AMD64_UI_LIBRARY) $(MINGW_AMD64_LIBRARY) $(WIN32_RENDERER_LIBS) -ld3d11 -ld2d1 -ldxguid -o $@

$(D3D11_PROTOTYPE_I686): $(D3D11_PROTOTYPE_SOURCES) $(D3D11_PROTOTYPE_HEADERS) $(MINGW_I686_WIN32_LIBRARY) $(MINGW_I686_UI_LIBRARY) $(MINGW_I686_LIBRARY)
	$(MINGW_I686_CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(MINGW_PROBE_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(D3D11_PROTOTYPE_SOURCES) $(MINGW_I686_WIN32_LIBRARY) $(MINGW_I686_UI_LIBRARY) $(MINGW_I686_LIBRARY) $(WIN32_RENDERER_LIBS) -ld3d11 -ld2d1 -ldxguid -o $@

.PHONY: test-d3d11-prototype
test: test-d3d11-prototype
test-d3d11-prototype: $(D3D11_PROTOTYPE_AMD64) $(D3D11_PROTOTYPE_I686)
	$(WINE_RUN) $(abspath $(D3D11_PROTOTYPE_AMD64))
	$(WINE_RUN) $(abspath $(D3D11_PROTOTYPE_I686))

test-p2-win32: $(D3D11_PROTOTYPE_AMD64) $(D3D11_PROTOTYPE_I686)

# Optional renderer-only Direct3D 11 backend; software archives stay independent.
D3D11_FIXTURE_SOURCES := tests/win32/test_d3d11_renderer.c tests/test_ui_platform_fixture.c tests/color_renderer_fixture.c tests/blink_renderer_fixture.c tests/selection_renderer_fixture.c tests/decoration_integration_fixture.c
D3D11_RENDERER_SOURCES := $(wildcard src/d3d11/*.c)
D3D11_RENDERER_HEADERS := $(wildcard include/boxrig/render/d3d11.h src/d3d11/*.h)
D3D11_AMD64_OBJECTS := $(patsubst src/d3d11/%.c,$(MINGW_AMD64_BUILD_DIR)/d3d11/%.o,$(D3D11_RENDERER_SOURCES))
D3D11_I686_OBJECTS := $(patsubst src/d3d11/%.c,$(MINGW_I686_BUILD_DIR)/d3d11/%.o,$(D3D11_RENDERER_SOURCES))
D3D11_AMD64_OBJECTS += $(MINGW_AMD64_BUILD_DIR)/d3d11/background.o
D3D11_I686_OBJECTS += $(MINGW_I686_BUILD_DIR)/d3d11/background.o
D3D11_AMD64_LIBRARY := $(MINGW_AMD64_BUILD_DIR)/libboxrig-render-d3d11.a
D3D11_I686_LIBRARY := $(MINGW_I686_BUILD_DIR)/libboxrig-render-d3d11.a
D3D11_RENDERER_AMD64 := $(MINGW_AMD64_BUILD_DIR)/tests/test_d3d11_renderer.exe
D3D11_RENDERER_I686 := $(MINGW_I686_BUILD_DIR)/tests/test_d3d11_renderer.exe

$(D3D11_RENDERER_AMD64): $(MINGW_AMD64_BUILD_DIR)/tests/decoration_gdi.o $(D3D11_FIXTURE_SOURCES) tests/color_renderer_fixture.h tests/blink_renderer_fixture.h tests/selection_renderer_fixture.h $(D3D11_AMD64_LIBRARY) $(D3D11_RENDERER_HEADERS) $(MINGW_AMD64_WIN32_LIBRARY) $(MINGW_AMD64_UI_LIBRARY) $(MINGW_AMD64_LIBRARY)
	$(MINGW_AMD64_CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) -DBOXRIG_TEST_FRONTEND_D3D11 $(MINGW_PROBE_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(D3D11_FIXTURE_SOURCES) $(MINGW_AMD64_BUILD_DIR)/tests/decoration_gdi.o $(D3D11_AMD64_LIBRARY) $(MINGW_AMD64_WIN32_LIBRARY) $(MINGW_AMD64_UI_LIBRARY) $(MINGW_AMD64_LIBRARY) $(WIN32_RENDERER_LIBS) -ld3d11 -ld2d1 -ldxguid -o $@

$(D3D11_RENDERER_I686): $(MINGW_I686_BUILD_DIR)/tests/decoration_gdi.o $(D3D11_FIXTURE_SOURCES) tests/color_renderer_fixture.h tests/blink_renderer_fixture.h tests/selection_renderer_fixture.h $(D3D11_I686_LIBRARY) $(D3D11_RENDERER_HEADERS) $(MINGW_I686_WIN32_LIBRARY) $(MINGW_I686_UI_LIBRARY) $(MINGW_I686_LIBRARY)
	$(MINGW_I686_CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) -DBOXRIG_TEST_FRONTEND_D3D11 $(MINGW_PROBE_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(D3D11_FIXTURE_SOURCES) $(MINGW_I686_BUILD_DIR)/tests/decoration_gdi.o $(D3D11_I686_LIBRARY) $(MINGW_I686_WIN32_LIBRARY) $(MINGW_I686_UI_LIBRARY) $(MINGW_I686_LIBRARY) $(WIN32_RENDERER_LIBS) -ld3d11 -ld2d1 -ldxguid -o $@

.PHONY: test-d3d11-renderer
test: test-d3d11-renderer
test-d3d11-renderer: $(D3D11_RENDERER_AMD64) $(D3D11_RENDERER_I686)
	$(WINE_RUN) $(abspath $(D3D11_RENDERER_AMD64))
	$(WINE_RUN) $(abspath $(D3D11_RENDERER_I686))
test-p2-win32: $(D3D11_RENDERER_AMD64) $(D3D11_RENDERER_I686)

$(MINGW_AMD64_BUILD_DIR)/win32/text.o $(MINGW_I686_BUILD_DIR)/win32/text.o: src/win32/text_private.h src/win32/text_state.h src/opengl/text_provider.h

$(MINGW_AMD64_BUILD_DIR)/d3d11/%.o: src/d3d11/%.c $(D3D11_RENDERER_HEADERS) src/win32/text_private.h
	mkdir -p $(@D)
	$(MINGW_AMD64_CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(MINGW_WIN32_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) -c $< -o $@
$(MINGW_I686_BUILD_DIR)/d3d11/%.o: src/d3d11/%.c $(D3D11_RENDERER_HEADERS) src/win32/text_private.h
	mkdir -p $(@D)
	$(MINGW_I686_CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(MINGW_WIN32_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) -c $< -o $@
$(D3D11_AMD64_LIBRARY): $(D3D11_AMD64_OBJECTS)
	$(MINGW_AMD64_AR) rcs $@ $^
$(D3D11_I686_LIBRARY): $(D3D11_I686_OBJECTS)
	$(MINGW_I686_AR) rcs $@ $^
$(MINGW_AMD64_BUILD_DIR)/d3d11/background.o: src/d3d11/background.cpp src/d3d11/background.h
	mkdir -p $(@D)
	$(MINGW_AMD64_CXX) $(CPPFLAGS) $(MINGW_WIN32_CPPFLAGS) $(CFLAGS) -std=c++20 $(WARNINGS) -fno-exceptions -fno-rtti -c $< -o $@
$(MINGW_I686_BUILD_DIR)/d3d11/background.o: src/d3d11/background.cpp src/d3d11/background.h
	mkdir -p $(@D)
	$(MINGW_I686_CXX) $(CPPFLAGS) $(MINGW_WIN32_CPPFLAGS) $(CFLAGS) -std=c++20 $(WARNINGS) -fno-exceptions -fno-rtti -c $< -o $@
.PHONY: render-d3d11-amd64 render-d3d11-i686
render-d3d11-amd64: $(D3D11_AMD64_LIBRARY) $(MINGW_AMD64_WIN32_LIBRARY) $(MINGW_AMD64_UI_LIBRARY) $(MINGW_AMD64_LIBRARY)
render-d3d11-i686: $(D3D11_I686_LIBRARY) $(MINGW_I686_WIN32_LIBRARY) $(MINGW_I686_UI_LIBRARY) $(MINGW_I686_LIBRARY)

MINGW_AMD64_CXX ?= x86_64-w64-mingw32-g++
MINGW_I686_CXX ?= i686-w64-mingw32-g++
D3D11_MANAGED_HEADERS := include/boxrig/win32/d3d11_component.h src/win32_d3d11/native.h src/win32/component_private.h src/d3d11/renderer_private.h
D3D11_MANAGED_AMD64_LIBRARY := $(MINGW_AMD64_BUILD_DIR)/libboxrig-win32-d3d11.a
D3D11_MANAGED_I686_LIBRARY := $(MINGW_I686_BUILD_DIR)/libboxrig-win32-d3d11.a
# DirectComposition managed adapter, separate from the borrowed renderer archive.
D3D11_COMPONENT_AMD64 := $(MINGW_AMD64_BUILD_DIR)/tests/test_d3d11_component.exe
D3D11_COMPONENT_I686 := $(MINGW_I686_BUILD_DIR)/tests/test_d3d11_component.exe
$(D3D11_COMPONENT_AMD64): tests/win32/test_d3d11_component.c $(D3D11_MANAGED_AMD64_LIBRARY) $(D3D11_AMD64_LIBRARY) $(MINGW_AMD64_WIN32_LIBRARY) $(MINGW_AMD64_UI_LIBRARY) $(MINGW_AMD64_LIBRARY) $(MINGW_AMD64_BUILD_DIR)/tests/d3d11_completion.o
	$(MINGW_AMD64_CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(MINGW_PROBE_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $< $(MINGW_AMD64_BUILD_DIR)/tests/d3d11_completion.o $(D3D11_MANAGED_AMD64_LIBRARY) $(D3D11_AMD64_LIBRARY) $(MINGW_AMD64_WIN32_LIBRARY) $(MINGW_AMD64_UI_LIBRARY) $(MINGW_AMD64_LIBRARY) $(WIN32_RENDERER_LIBS) -ld3d11 -ld2d1 -ldxguid -ldcomp -ldwmapi -lcomctl32 -Wl,--wrap=boxrig_dcomp_chain_activity -Wl,--wrap=boxrig_dcomp_chain_buffer -Wl,--wrap=boxrig_dcomp_chain_present -o $@
$(D3D11_COMPONENT_I686): tests/win32/test_d3d11_component.c $(D3D11_MANAGED_I686_LIBRARY) $(D3D11_I686_LIBRARY) $(MINGW_I686_WIN32_LIBRARY) $(MINGW_I686_UI_LIBRARY) $(MINGW_I686_LIBRARY) $(MINGW_I686_BUILD_DIR)/tests/d3d11_completion.o
	$(MINGW_I686_CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(MINGW_PROBE_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $< $(MINGW_I686_BUILD_DIR)/tests/d3d11_completion.o $(D3D11_MANAGED_I686_LIBRARY) $(D3D11_I686_LIBRARY) $(MINGW_I686_WIN32_LIBRARY) $(MINGW_I686_UI_LIBRARY) $(MINGW_I686_LIBRARY) $(WIN32_RENDERER_LIBS) -ld3d11 -ld2d1 -ldxguid -ldcomp -ldwmapi -lcomctl32 -Wl,--wrap=boxrig_dcomp_chain_activity -Wl,--wrap=boxrig_dcomp_chain_buffer -Wl,--wrap=boxrig_dcomp_chain_present -o $@
.PHONY: test-d3d11-component
test: test-d3d11-component
test-d3d11-component: $(D3D11_COMPONENT_AMD64) $(D3D11_COMPONENT_I686)
	$(WINE_RUN) $(abspath $(D3D11_COMPONENT_AMD64))
	$(WINE_RUN) $(abspath $(D3D11_COMPONENT_I686))
test-p2-win32: $(D3D11_COMPONENT_AMD64) $(D3D11_COMPONENT_I686)

$(MINGW_AMD64_BUILD_DIR)/dcomp/component.o: src/win32_d3d11/component.c $(D3D11_MANAGED_HEADERS)
	mkdir -p $(@D)
	$(MINGW_AMD64_CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(MINGW_WIN32_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) -c $< -o $@
$(MINGW_AMD64_BUILD_DIR)/dcomp/native.o: src/win32_d3d11/native.cpp $(D3D11_MANAGED_HEADERS)
	mkdir -p $(@D)
	$(MINGW_AMD64_CXX) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(MINGW_WIN32_CPPFLAGS) $(CFLAGS) -std=c++20 $(WARNINGS) -fno-exceptions -fno-rtti -c $< -o $@
$(D3D11_MANAGED_AMD64_LIBRARY): $(MINGW_AMD64_BUILD_DIR)/dcomp/component.o $(MINGW_AMD64_BUILD_DIR)/dcomp/native.o
	$(MINGW_AMD64_AR) rcs $@ $^
.PHONY: win32-d3d11-amd64
win32-d3d11-amd64: $(D3D11_MANAGED_AMD64_LIBRARY) render-d3d11-amd64

$(BUILD_DIR)/boxrig_term/win32/amd64/boxrig_term_d3d11.exe: $(WIN32_EXAMPLE_SOURCES) examples/boxrig_term/win32/application_internal.h examples/boxrig_term/common/color_demo.h examples/boxrig_term/common/scrollbar_demo.h tests/win32/opengl_sample.c $(MINGW_AMD64_EXAMPLE_RESOURCE) $(D3D11_MANAGED_AMD64_LIBRARY) $(D3D11_AMD64_LIBRARY) $(MINGW_AMD64_WIN32_LIBRARY) $(MINGW_AMD64_SESSION_LIBRARY) $(MINGW_AMD64_UI_LIBRARY) $(MINGW_AMD64_LIBRARY) $(CONPTY_HEADER)
	$(MINGW_AMD64_CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(MINGW_TERM_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) -DBOXRIG_TEST_D3D11_COMPONENT -DBOXRIG_TERM_ENABLE_D3D11 $(WIN32_EXAMPLE_SOURCES) tests/win32/opengl_sample.c $(MINGW_AMD64_EXAMPLE_RESOURCE) $(D3D11_MANAGED_AMD64_LIBRARY) $(D3D11_AMD64_LIBRARY) $(MINGW_AMD64_WIN32_LIBRARY) $(MINGW_AMD64_SESSION_LIBRARY) $(MINGW_AMD64_UI_LIBRARY) $(MINGW_AMD64_LIBRARY) -municode -mwindows $(WIN32_RENDERER_LIBS) $(WIN32_HOST_LIBS) $(WIN32_TRANSPARENCY_LIBS) -lcomctl32 -ld3d11 -ld2d1 -ldxguid -ldcomp -Wl,--wrap=boxrig_term_win32_run -Wl,--wrap=boxrig_win32_d3d11_terminal_component_create_child -lcomctl32 -o $@
test-p2-win32: $(BUILD_DIR)/boxrig_term/win32/amd64/boxrig_term_d3d11.exe

$(BUILD_DIR)/boxrig_term/win32/amd64/boxrig_term_d3d11_ime_probe.exe: $(WIN32_EXAMPLE_SOURCES) examples/boxrig_term/win32/application_internal.h examples/boxrig_term/common/color_demo.h examples/boxrig_term/common/scrollbar_demo.h tests/win32/opengl_sample.c tests/p2/win32_ime_placement_probe.c $(MINGW_AMD64_EXAMPLE_RESOURCE) $(D3D11_MANAGED_AMD64_LIBRARY) $(D3D11_AMD64_LIBRARY) $(MINGW_AMD64_WIN32_LIBRARY) $(MINGW_AMD64_SESSION_LIBRARY) $(MINGW_AMD64_UI_LIBRARY) $(MINGW_AMD64_LIBRARY) $(CONPTY_HEADER)
	$(MINGW_AMD64_CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(MINGW_TERM_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) -DBOXRIG_TEST_D3D11_COMPONENT -DBOXRIG_TERM_ENABLE_D3D11 $(WIN32_EXAMPLE_SOURCES) tests/win32/opengl_sample.c tests/p2/win32_ime_placement_probe.c $(MINGW_AMD64_EXAMPLE_RESOURCE) $(D3D11_MANAGED_AMD64_LIBRARY) $(D3D11_AMD64_LIBRARY) $(MINGW_AMD64_WIN32_LIBRARY) $(MINGW_AMD64_SESSION_LIBRARY) $(MINGW_AMD64_UI_LIBRARY) $(MINGW_AMD64_LIBRARY) -municode -mwindows $(WIN32_RENDERER_LIBS) $(WIN32_HOST_LIBS) $(WIN32_TRANSPARENCY_LIBS) -lcomctl32 -ld3d11 -ld2d1 -ldxguid -ldcomp -Wl,--wrap=boxrig_term_win32_run -Wl,--wrap=boxrig_win32_d3d11_terminal_component_create_child -Wl,--wrap=boxrig_win32_input_adapter_create -Wl,--wrap=boxrig_win32_input_adapter_synchronize_candidate -Wl,--wrap=boxrig_win32_input_adapter_resynchronize_candidate -lcomctl32 -o $@
test-p2-win32: $(BUILD_DIR)/boxrig_term/win32/amd64/boxrig_term_d3d11_ime_probe.exe

$(MINGW_I686_BUILD_DIR)/dcomp/component.o: src/win32_d3d11/component.c $(D3D11_MANAGED_HEADERS)
	mkdir -p $(@D)
	$(MINGW_I686_CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(MINGW_WIN32_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) -c $< -o $@
$(MINGW_I686_BUILD_DIR)/dcomp/native.o: src/win32_d3d11/native.cpp $(D3D11_MANAGED_HEADERS)
	mkdir -p $(@D)
	$(MINGW_I686_CXX) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(MINGW_WIN32_CPPFLAGS) $(CFLAGS) -std=c++20 $(WARNINGS) -fno-exceptions -fno-rtti -c $< -o $@
$(D3D11_MANAGED_I686_LIBRARY): $(MINGW_I686_BUILD_DIR)/dcomp/component.o $(MINGW_I686_BUILD_DIR)/dcomp/native.o
	$(MINGW_I686_AR) rcs $@ $^
.PHONY: win32-d3d11-i686
win32-d3d11-i686: $(D3D11_MANAGED_I686_LIBRARY) render-d3d11-i686

$(BUILD_DIR)/boxrig_term/win32/i686/boxrig_term_d3d11.exe: $(WIN32_EXAMPLE_SOURCES) examples/boxrig_term/win32/application_internal.h examples/boxrig_term/common/color_demo.h examples/boxrig_term/common/scrollbar_demo.h tests/win32/opengl_sample.c $(MINGW_I686_EXAMPLE_RESOURCE) $(D3D11_MANAGED_I686_LIBRARY) $(D3D11_I686_LIBRARY) $(MINGW_I686_WIN32_LIBRARY) $(MINGW_I686_SESSION_LIBRARY) $(MINGW_I686_UI_LIBRARY) $(MINGW_I686_LIBRARY) $(CONPTY_HEADER)
	$(MINGW_I686_CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(MINGW_TERM_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) -DBOXRIG_TEST_D3D11_COMPONENT -DBOXRIG_TERM_ENABLE_D3D11 $(WIN32_EXAMPLE_SOURCES) tests/win32/opengl_sample.c $(MINGW_I686_EXAMPLE_RESOURCE) $(D3D11_MANAGED_I686_LIBRARY) $(D3D11_I686_LIBRARY) $(MINGW_I686_WIN32_LIBRARY) $(MINGW_I686_SESSION_LIBRARY) $(MINGW_I686_UI_LIBRARY) $(MINGW_I686_LIBRARY) -municode -mwindows $(WIN32_RENDERER_LIBS) $(WIN32_HOST_LIBS) $(WIN32_TRANSPARENCY_LIBS) -lcomctl32 -ld3d11 -ld2d1 -ldxguid -ldcomp -Wl,--wrap=boxrig_term_win32_run -Wl,--wrap=boxrig_win32_d3d11_terminal_component_create_child -lcomctl32 -o $@
test-p2-win32: $(BUILD_DIR)/boxrig_term/win32/i686/boxrig_term_d3d11.exe

$(BUILD_DIR)/boxrig_term/win32/i686/boxrig_term_d3d11_ime_probe.exe: $(WIN32_EXAMPLE_SOURCES) examples/boxrig_term/win32/application_internal.h examples/boxrig_term/common/color_demo.h examples/boxrig_term/common/scrollbar_demo.h tests/win32/opengl_sample.c tests/p2/win32_ime_placement_probe.c $(MINGW_I686_EXAMPLE_RESOURCE) $(D3D11_MANAGED_I686_LIBRARY) $(D3D11_I686_LIBRARY) $(MINGW_I686_WIN32_LIBRARY) $(MINGW_I686_SESSION_LIBRARY) $(MINGW_I686_UI_LIBRARY) $(MINGW_I686_LIBRARY) $(CONPTY_HEADER)
	$(MINGW_I686_CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(MINGW_TERM_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) -DBOXRIG_TEST_D3D11_COMPONENT -DBOXRIG_TERM_ENABLE_D3D11 $(WIN32_EXAMPLE_SOURCES) tests/win32/opengl_sample.c tests/p2/win32_ime_placement_probe.c $(MINGW_I686_EXAMPLE_RESOURCE) $(D3D11_MANAGED_I686_LIBRARY) $(D3D11_I686_LIBRARY) $(MINGW_I686_WIN32_LIBRARY) $(MINGW_I686_SESSION_LIBRARY) $(MINGW_I686_UI_LIBRARY) $(MINGW_I686_LIBRARY) -municode -mwindows $(WIN32_RENDERER_LIBS) $(WIN32_HOST_LIBS) $(WIN32_TRANSPARENCY_LIBS) -lcomctl32 -ld3d11 -ld2d1 -ldxguid -ldcomp -Wl,--wrap=boxrig_term_win32_run -Wl,--wrap=boxrig_win32_d3d11_terminal_component_create_child -Wl,--wrap=boxrig_win32_input_adapter_create -Wl,--wrap=boxrig_win32_input_adapter_synchronize_candidate -Wl,--wrap=boxrig_win32_input_adapter_resynchronize_candidate -lcomctl32 -o $@
test-p2-win32: $(BUILD_DIR)/boxrig_term/win32/i686/boxrig_term_d3d11_ime_probe.exe

$(D3D11_COMPONENT_AMD64) $(D3D11_COMPONENT_I686): $(D3D11_MANAGED_HEADERS)
$(D3D11_AMD64_OBJECTS) $(D3D11_I686_OBJECTS): src/win32/text_state.h

# Relocatable optional Direct3D SDKs, independently removable.

.PHONY: install-render-d3d11-amd64 install-win32-d3d11-amd64 uninstall-render-d3d11-amd64 uninstall-win32-d3d11-amd64
install-render-d3d11-amd64: render-d3d11-amd64 packaging/win32/boxrig-render-d3d11.pc examples/win32/d3d11_renderer.c
	$(INSTALL) -d $(DESTDIR)$(LIBDIR) $(DESTDIR)$(PKGCONFIGDIR) $(DESTDIR)$(INCLUDEDIR)/boxrig/ui $(DESTDIR)$(INCLUDEDIR)/boxrig/render
	$(INSTALL) -m 644 $(D3D11_AMD64_LIBRARY) $(MINGW_AMD64_WIN32_LIBRARY) $(MINGW_AMD64_UI_LIBRARY) $(MINGW_AMD64_LIBRARY) $(DESTDIR)$(LIBDIR)
	$(INSTALL) -m 644 $(CORE_PUBLIC_HEADERS) include/boxrig/ui.h $(DESTDIR)$(INCLUDEDIR)/boxrig
	$(INSTALL) -m 644 $(wildcard include/boxrig/ui/*.h) $(DESTDIR)$(INCLUDEDIR)/boxrig/ui
	$(INSTALL) -m 644 include/boxrig/render/d3d11.h $(DESTDIR)$(INCLUDEDIR)/boxrig/render
	$(INSTALL) -m 644 packaging/win32/boxrig-render-d3d11.pc $(DESTDIR)$(PKGCONFIGDIR)
	$(INSTALL) -d $(DESTDIR)$(PREFIX)/share/boxrig/examples/win32
	$(INSTALL) -m 644 examples/win32/d3d11_renderer.c $(DESTDIR)$(PREFIX)/share/boxrig/examples/win32

install-win32-d3d11-amd64: install-render-d3d11-amd64 install-win32-amd64 win32-d3d11-amd64 packaging/win32/boxrig-win32-d3d11.pc examples/win32/d3d11_component.c
	$(INSTALL) -m 644 $(D3D11_MANAGED_AMD64_LIBRARY) $(DESTDIR)$(LIBDIR)
	$(INSTALL) -m 644 include/boxrig/win32/d3d11_component.h $(DESTDIR)$(INCLUDEDIR)/boxrig/win32
	$(INSTALL) -m 644 packaging/win32/boxrig-win32-d3d11.pc $(DESTDIR)$(PKGCONFIGDIR)
	$(INSTALL) -m 644 examples/win32/d3d11_component.c $(DESTDIR)$(PREFIX)/share/boxrig/examples/win32

uninstall-render-d3d11-amd64:
	rm -f $(DESTDIR)$(LIBDIR)/libboxrig-render-d3d11.a $(DESTDIR)$(PKGCONFIGDIR)/boxrig-render-d3d11.pc $(DESTDIR)$(INCLUDEDIR)/boxrig/render/d3d11.h $(DESTDIR)$(PREFIX)/share/boxrig/examples/win32/d3d11_renderer.c

uninstall-win32-d3d11-amd64:
	rm -f $(DESTDIR)$(LIBDIR)/libboxrig-win32-d3d11.a $(DESTDIR)$(PKGCONFIGDIR)/boxrig-win32-d3d11.pc $(DESTDIR)$(INCLUDEDIR)/boxrig/win32/d3d11_component.h $(DESTDIR)$(PREFIX)/share/boxrig/examples/win32/d3d11_component.c

.PHONY: install-render-d3d11-i686 install-win32-d3d11-i686 uninstall-render-d3d11-i686 uninstall-win32-d3d11-i686
install-render-d3d11-i686: render-d3d11-i686 packaging/win32/boxrig-render-d3d11.pc examples/win32/d3d11_renderer.c
	$(INSTALL) -d $(DESTDIR)$(LIBDIR) $(DESTDIR)$(PKGCONFIGDIR) $(DESTDIR)$(INCLUDEDIR)/boxrig/ui $(DESTDIR)$(INCLUDEDIR)/boxrig/render
	$(INSTALL) -m 644 $(D3D11_I686_LIBRARY) $(MINGW_I686_WIN32_LIBRARY) $(MINGW_I686_UI_LIBRARY) $(MINGW_I686_LIBRARY) $(DESTDIR)$(LIBDIR)
	$(INSTALL) -m 644 $(CORE_PUBLIC_HEADERS) include/boxrig/ui.h $(DESTDIR)$(INCLUDEDIR)/boxrig
	$(INSTALL) -m 644 $(wildcard include/boxrig/ui/*.h) $(DESTDIR)$(INCLUDEDIR)/boxrig/ui
	$(INSTALL) -m 644 include/boxrig/render/d3d11.h $(DESTDIR)$(INCLUDEDIR)/boxrig/render
	$(INSTALL) -m 644 packaging/win32/boxrig-render-d3d11.pc $(DESTDIR)$(PKGCONFIGDIR)
	$(INSTALL) -d $(DESTDIR)$(PREFIX)/share/boxrig/examples/win32
	$(INSTALL) -m 644 examples/win32/d3d11_renderer.c $(DESTDIR)$(PREFIX)/share/boxrig/examples/win32

install-win32-d3d11-i686: install-render-d3d11-i686 install-win32-i686 win32-d3d11-i686 packaging/win32/boxrig-win32-d3d11.pc examples/win32/d3d11_component.c
	$(INSTALL) -m 644 $(D3D11_MANAGED_I686_LIBRARY) $(DESTDIR)$(LIBDIR)
	$(INSTALL) -m 644 include/boxrig/win32/d3d11_component.h $(DESTDIR)$(INCLUDEDIR)/boxrig/win32
	$(INSTALL) -m 644 packaging/win32/boxrig-win32-d3d11.pc $(DESTDIR)$(PKGCONFIGDIR)
	$(INSTALL) -m 644 examples/win32/d3d11_component.c $(DESTDIR)$(PREFIX)/share/boxrig/examples/win32

uninstall-render-d3d11-i686:
	rm -f $(DESTDIR)$(LIBDIR)/libboxrig-render-d3d11.a $(DESTDIR)$(PKGCONFIGDIR)/boxrig-render-d3d11.pc $(DESTDIR)$(INCLUDEDIR)/boxrig/render/d3d11.h $(DESTDIR)$(PREFIX)/share/boxrig/examples/win32/d3d11_renderer.c

uninstall-win32-d3d11-i686:
	rm -f $(DESTDIR)$(LIBDIR)/libboxrig-win32-d3d11.a $(DESTDIR)$(PKGCONFIGDIR)/boxrig-win32-d3d11.pc $(DESTDIR)$(INCLUDEDIR)/boxrig/win32/d3d11_component.h $(DESTDIR)$(PREFIX)/share/boxrig/examples/win32/d3d11_component.c

# Win32 renderer publication and real GUI startup conformance.

$(MINGW_AMD64_BUILD_DIR)/tests/test_win32_startup.exe: tests/win32/test_win32_startup.c | $(MINGW_AMD64_BUILD_DIR)/tests
	$(MINGW_AMD64_CC) $(CFLAGS) $(PROJECT_CFLAGS) -municode $< -o $@

$(MINGW_AMD64_BUILD_DIR)/tests/test_win32_d3d11_component_fixture.exe: tests/win32/test_win32_component_fixture.c tests/win32/d3d11_component_capture.h $(FRONTEND_COMPONENT_FIXTURE_SOURCES) $(FRONTEND_COMPONENT_FIXTURE_HEADER) $(D3D11_MANAGED_AMD64_LIBRARY) $(D3D11_AMD64_LIBRARY) $(MINGW_AMD64_WIN32_LIBRARY) $(MINGW_AMD64_UI_LIBRARY) $(MINGW_AMD64_LIBRARY)
	$(MINGW_AMD64_CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(MINGW_PROBE_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) -DBOXRIG_TEST_D3D11_COMPONENT tests/win32/test_win32_component_fixture.c $(FRONTEND_COMPONENT_FIXTURE_SOURCES) $(D3D11_MANAGED_AMD64_LIBRARY) $(D3D11_AMD64_LIBRARY) $(MINGW_AMD64_WIN32_LIBRARY) $(MINGW_AMD64_UI_LIBRARY) $(MINGW_AMD64_LIBRARY) $(WIN32_RENDERER_LIBS) -ld3d11 -ld2d1 -ldxguid -ldcomp -lcomctl32 -Wl,--wrap=boxrig_dcomp_chain_present -Wl,--wrap=boxrig_dcomp_chain_destroy -o $@

test: $(MINGW_AMD64_BUILD_DIR)/tests/test_win32_d3d11_component_fixture.exe
test-p2-win32: $(MINGW_AMD64_BUILD_DIR)/tests/test_win32_startup.exe $(MINGW_AMD64_BUILD_DIR)/tests/test_win32_d3d11_component_fixture.exe

$(MINGW_I686_BUILD_DIR)/tests/test_win32_startup.exe: tests/win32/test_win32_startup.c | $(MINGW_I686_BUILD_DIR)/tests
	$(MINGW_I686_CC) $(CFLAGS) $(PROJECT_CFLAGS) -municode $< -o $@

$(MINGW_I686_BUILD_DIR)/tests/test_win32_d3d11_component_fixture.exe: tests/win32/test_win32_component_fixture.c tests/win32/d3d11_component_capture.h $(FRONTEND_COMPONENT_FIXTURE_SOURCES) $(FRONTEND_COMPONENT_FIXTURE_HEADER) $(D3D11_MANAGED_I686_LIBRARY) $(D3D11_I686_LIBRARY) $(MINGW_I686_WIN32_LIBRARY) $(MINGW_I686_UI_LIBRARY) $(MINGW_I686_LIBRARY)
	$(MINGW_I686_CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(MINGW_PROBE_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) -DBOXRIG_TEST_D3D11_COMPONENT tests/win32/test_win32_component_fixture.c $(FRONTEND_COMPONENT_FIXTURE_SOURCES) $(D3D11_MANAGED_I686_LIBRARY) $(D3D11_I686_LIBRARY) $(MINGW_I686_WIN32_LIBRARY) $(MINGW_I686_UI_LIBRARY) $(MINGW_I686_LIBRARY) $(WIN32_RENDERER_LIBS) -ld3d11 -ld2d1 -ldxguid -ldcomp -lcomctl32 -Wl,--wrap=boxrig_dcomp_chain_present -Wl,--wrap=boxrig_dcomp_chain_destroy -o $@

test: $(MINGW_I686_BUILD_DIR)/tests/test_win32_d3d11_component_fixture.exe
test-p2-win32: $(MINGW_I686_BUILD_DIR)/tests/test_win32_startup.exe $(MINGW_I686_BUILD_DIR)/tests/test_win32_d3d11_component_fixture.exe

.PHONY: test-win32-startup
test: test-win32-startup
test-win32-startup: $(MINGW_AMD64_BUILD_DIR)/tests/test_win32_startup.exe $(MINGW_I686_BUILD_DIR)/tests/test_win32_startup.exe $(MINGW_AMD64_EXAMPLE) $(MINGW_I686_EXAMPLE)
	$(WINE_RUN) $(abspath $(MINGW_AMD64_BUILD_DIR)/tests/test_win32_startup.exe) "Z:$(abspath $(MINGW_AMD64_EXAMPLE))"
	$(WINE_RUN) $(abspath $(MINGW_I686_BUILD_DIR)/tests/test_win32_startup.exe) "Z:$(abspath $(MINGW_I686_EXAMPLE))"

MINGW_AMD64_PHASE_TRACE_TEST := $(MINGW_AMD64_BUILD_DIR)/tests/test_win32_phase_trace.exe
MINGW_I686_PHASE_TRACE_TEST := $(MINGW_I686_BUILD_DIR)/tests/test_win32_phase_trace.exe
WIN32_PHASE_TRACE_TEST_SOURCES := tests/win32/test_win32_phase_trace.c examples/boxrig_term/win32/phase_trace.c
WIN32_PHASE_TRACE_TEST_HEADER := examples/boxrig_term/win32/phase_trace.h
$(MINGW_AMD64_PHASE_TRACE_TEST): $(WIN32_PHASE_TRACE_TEST_SOURCES) $(WIN32_PHASE_TRACE_TEST_HEADER) | $(MINGW_AMD64_BUILD_DIR)/tests
	$(MINGW_AMD64_CC) $(CPPFLAGS) $(MINGW_TERM_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(WIN32_PHASE_TRACE_TEST_SOURCES) -o $@
$(MINGW_I686_PHASE_TRACE_TEST): $(WIN32_PHASE_TRACE_TEST_SOURCES) $(WIN32_PHASE_TRACE_TEST_HEADER) | $(MINGW_I686_BUILD_DIR)/tests
	$(MINGW_I686_CC) $(CPPFLAGS) $(MINGW_TERM_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(WIN32_PHASE_TRACE_TEST_SOURCES) -o $@
.PHONY: test-win32-phase-trace
test: test-win32-phase-trace
test-win32-phase-trace: $(MINGW_AMD64_PHASE_TRACE_TEST) $(MINGW_I686_PHASE_TRACE_TEST)
	$(WINE_RUN) $(abspath $(MINGW_AMD64_PHASE_TRACE_TEST))
	$(WINE_RUN) $(abspath $(MINGW_I686_PHASE_TRACE_TEST))

MINGW_AMD64_RENDERER_TRACE_TEST := $(MINGW_AMD64_BUILD_DIR)/tests/test_win32_renderer_trace.exe
MINGW_I686_RENDERER_TRACE_TEST := $(MINGW_I686_BUILD_DIR)/tests/test_win32_renderer_trace.exe
WIN32_RENDERER_TRACE_TEST_SOURCES := tests/win32/test_win32_renderer_trace.c \
	examples/boxrig_term/win32/renderer_trace.c examples/boxrig_term/win32/phase_trace.c
WIN32_RENDERER_TRACE_TEST_HEADER := examples/boxrig_term/win32/renderer_trace.h \
	examples/boxrig_term/win32/phase_trace.h src/win32/renderer_private.h
$(MINGW_AMD64_RENDERER_TRACE_TEST): $(WIN32_RENDERER_TRACE_TEST_SOURCES) $(WIN32_RENDERER_TRACE_TEST_HEADER) | $(MINGW_AMD64_BUILD_DIR)/tests
	$(MINGW_AMD64_CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(MINGW_TERM_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(WIN32_RENDERER_TRACE_TEST_SOURCES) -o $@
$(MINGW_I686_RENDERER_TRACE_TEST): $(WIN32_RENDERER_TRACE_TEST_SOURCES) $(WIN32_RENDERER_TRACE_TEST_HEADER) | $(MINGW_I686_BUILD_DIR)/tests
	$(MINGW_I686_CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(MINGW_TERM_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(WIN32_RENDERER_TRACE_TEST_SOURCES) -o $@
.PHONY: test-win32-renderer-trace
test: test-win32-renderer-trace
test-win32-renderer-trace: $(MINGW_AMD64_RENDERER_TRACE_TEST) $(MINGW_I686_RENDERER_TRACE_TEST)
	$(WINE_RUN) $(abspath $(MINGW_AMD64_RENDERER_TRACE_TEST))
	$(WINE_RUN) $(abspath $(MINGW_I686_RENDERER_TRACE_TEST))

MINGW_AMD64_FIRST_FRAME_FENCE_TEST := $(MINGW_AMD64_BUILD_DIR)/tests/test_win32_first_frame_fence.exe
MINGW_I686_FIRST_FRAME_FENCE_TEST := $(MINGW_I686_BUILD_DIR)/tests/test_win32_first_frame_fence.exe
WIN32_FIRST_FRAME_FENCE_TEST_SOURCES := tests/win32/test_win32_first_frame_fence.c \
	examples/boxrig_term/win32/first_frame_fence.c
WIN32_FIRST_FRAME_FENCE_TEST_HEADER := examples/boxrig_term/win32/first_frame_fence.h
$(MINGW_AMD64_FIRST_FRAME_FENCE_TEST): $(WIN32_FIRST_FRAME_FENCE_TEST_SOURCES) $(WIN32_FIRST_FRAME_FENCE_TEST_HEADER) | $(MINGW_AMD64_BUILD_DIR)/tests
	$(MINGW_AMD64_CC) $(CPPFLAGS) $(MINGW_TERM_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(WIN32_FIRST_FRAME_FENCE_TEST_SOURCES) -o $@
$(MINGW_I686_FIRST_FRAME_FENCE_TEST): $(WIN32_FIRST_FRAME_FENCE_TEST_SOURCES) $(WIN32_FIRST_FRAME_FENCE_TEST_HEADER) | $(MINGW_I686_BUILD_DIR)/tests
	$(MINGW_I686_CC) $(CPPFLAGS) $(MINGW_TERM_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(WIN32_FIRST_FRAME_FENCE_TEST_SOURCES) -o $@
.PHONY: test-win32-first-frame-fence
test: test-win32-first-frame-fence
test-win32-first-frame-fence: $(MINGW_AMD64_FIRST_FRAME_FENCE_TEST) $(MINGW_I686_FIRST_FRAME_FENCE_TEST)
	$(WINE_RUN) $(abspath $(MINGW_AMD64_FIRST_FRAME_FENCE_TEST))
	$(WINE_RUN) $(abspath $(MINGW_I686_FIRST_FRAME_FENCE_TEST))

.PHONY: test-win32-sample-first-frame-fence test-win32-sample-dpi
test: test-win32-sample-first-frame-fence test-win32-sample-dpi

test-win32-sample-dpi: $(MINGW_AMD64_SAMPLE_TEST) $(MINGW_I686_SAMPLE_TEST)
	$(WINE_RUN) $(abspath $(MINGW_AMD64_SAMPLE_TEST)) --dpi-probe
	$(WINE_RUN) $(abspath $(MINGW_I686_SAMPLE_TEST)) --dpi-probe
test-p2-win32: $(MINGW_AMD64_BUILD_DIR)/tests/first_frame_child.exe $(MINGW_I686_BUILD_DIR)/tests/first_frame_child.exe
$(MINGW_AMD64_BUILD_DIR)/tests/first_frame_child.exe: tests/win32/first_frame_child.c | $(MINGW_AMD64_BUILD_DIR)/tests
	$(MINGW_AMD64_CC) $(CPPFLAGS) $(MINGW_TERM_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $< -o $@
$(MINGW_I686_BUILD_DIR)/tests/first_frame_child.exe: tests/win32/first_frame_child.c | $(MINGW_I686_BUILD_DIR)/tests
	$(MINGW_I686_CC) $(CPPFLAGS) $(MINGW_TERM_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $< -o $@
test-win32-sample-first-frame-fence: $(MINGW_AMD64_SAMPLE_TEST) $(MINGW_I686_SAMPLE_TEST) $(MINGW_AMD64_EXAMPLE) $(MINGW_I686_EXAMPLE) $(MINGW_AMD64_BUILD_DIR)/tests/first_frame_child.exe $(MINGW_I686_BUILD_DIR)/tests/first_frame_child.exe
	$(WINE_RUN) $(abspath $(MINGW_AMD64_SAMPLE_TEST)) --first-frame-fence $(abspath $(MINGW_AMD64_EXAMPLE)) $(abspath $(MINGW_AMD64_BUILD_DIR)/tests/first_frame_child.exe)
	$(WINE_RUN) $(abspath $(MINGW_I686_SAMPLE_TEST)) --first-frame-fence $(abspath $(MINGW_I686_EXAMPLE)) $(abspath $(MINGW_I686_BUILD_DIR)/tests/first_frame_child.exe)

# Win32 application options are also checked without a Windows runtime.
.PHONY: test-win32-arguments
test-win32-arguments: $(BUILD_DIR)/tests/test_win32_arguments
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $<

$(BUILD_DIR)/tests/test_win32_arguments: tests/test_win32_arguments.c examples/boxrig_term/win32/arguments.c examples/boxrig_term/win32/arguments.h | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) tests/test_win32_arguments.c examples/boxrig_term/win32/arguments.c $(SANITIZERS) -o $@

# The test application links only its explicitly enabled backends.
WIN32_TERM_RENDERERS ?= win32 opengl d3d11
ifneq ($(filter opengl,$(WIN32_TERM_RENDERERS)),)
WIN32_TERM_RENDERER_CPPFLAGS += -DBOXRIG_TERM_ENABLE_OPENGL
WIN32_TERM_AMD64_RENDERER_LIBRARIES += $(MINGW_AMD64_WGL_LIBRARY) $(MINGW_AMD64_OGL_LIBRARY)
WIN32_TERM_I686_RENDERER_LIBRARIES += $(MINGW_I686_WGL_LIBRARY) $(MINGW_I686_OGL_LIBRARY)
WIN32_TERM_RENDERER_LIBS += -lcomctl32 -lopengl32
endif
ifneq ($(filter d3d11,$(WIN32_TERM_RENDERERS)),)
WIN32_TERM_RENDERER_CPPFLAGS += -DBOXRIG_TERM_ENABLE_D3D11
WIN32_TERM_AMD64_RENDERER_LIBRARIES += $(D3D11_MANAGED_AMD64_LIBRARY) $(D3D11_AMD64_LIBRARY)
WIN32_TERM_I686_RENDERER_LIBRARIES += $(D3D11_MANAGED_I686_LIBRARY) $(D3D11_I686_LIBRARY)
WIN32_TERM_RENDERER_LIBS += -lcomctl32 -ldxguid
endif
$(MINGW_AMD64_EXAMPLE): $(WIN32_TERM_AMD64_RENDERER_LIBRARIES) examples/boxrig_term/win32/arguments.h
$(MINGW_I686_EXAMPLE): $(WIN32_TERM_I686_RENDERER_LIBRARIES) examples/boxrig_term/win32/arguments.h

$(MINGW_AMD64_BUILD_DIR)/tests/test_win32_renderer_cli.exe: tests/win32/test_win32_renderer_cli.c | $(MINGW_AMD64_BUILD_DIR)/tests
	$(MINGW_AMD64_CC) $(CFLAGS) $(PROJECT_CFLAGS) -municode $< -o $@

test-p2-win32: $(MINGW_AMD64_BUILD_DIR)/tests/test_win32_renderer_cli.exe

$(MINGW_I686_BUILD_DIR)/tests/test_win32_renderer_cli.exe: tests/win32/test_win32_renderer_cli.c | $(MINGW_I686_BUILD_DIR)/tests
	$(MINGW_I686_CC) $(CFLAGS) $(PROJECT_CFLAGS) -municode $< -o $@

test-p2-win32: $(MINGW_I686_BUILD_DIR)/tests/test_win32_renderer_cli.exe

.PHONY: test-win32-renderer-cli
test: test-win32-renderer-cli
test-win32-renderer-cli: $(MINGW_AMD64_BUILD_DIR)/tests/test_win32_renderer_cli.exe $(MINGW_I686_BUILD_DIR)/tests/test_win32_renderer_cli.exe $(MINGW_AMD64_EXAMPLE) $(MINGW_I686_EXAMPLE) $(BUILD_DIR)/boxrig_term/win32/amd64/boxrig_term_gl_unavailable.exe $(BUILD_DIR)/boxrig_term/win32/i686/boxrig_term_gl_unavailable.exe
	$(WINE_RUN) $(abspath $(MINGW_AMD64_BUILD_DIR)/tests/test_win32_renderer_cli.exe) "Z:$(abspath $(MINGW_AMD64_EXAMPLE))"
	$(WINE_RUN) $(abspath $(MINGW_I686_BUILD_DIR)/tests/test_win32_renderer_cli.exe) "Z:$(abspath $(MINGW_I686_EXAMPLE))"
	$(WINE_RUN) $(abspath $(MINGW_AMD64_BUILD_DIR)/tests/test_win32_renderer_cli.exe) "Z:$(abspath $(BUILD_DIR)/boxrig_term/win32/amd64/boxrig_term_gl_unavailable.exe)" --gl-unavailable
	$(WINE_RUN) $(abspath $(MINGW_I686_BUILD_DIR)/tests/test_win32_renderer_cli.exe) "Z:$(abspath $(BUILD_DIR)/boxrig_term/win32/i686/boxrig_term_gl_unavailable.exe)" --gl-unavailable

$(BUILD_DIR)/boxrig_term/win32/amd64/boxrig_term_gl_unavailable.exe: $(MINGW_AMD64_EXAMPLE) tests/win32/unavailable_opengl_sample.c Makefile
	$(MINGW_AMD64_CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(MINGW_TERM_CPPFLAGS) $(WIN32_TERM_RENDERER_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(WIN32_EXAMPLE_SOURCES) tests/win32/unavailable_opengl_sample.c $(MINGW_AMD64_EXAMPLE_RESOURCE) $(WIN32_TERM_AMD64_RENDERER_LIBRARIES) $(MINGW_AMD64_WIN32_LIBRARY) $(MINGW_AMD64_SESSION_LIBRARY) $(MINGW_AMD64_UI_LIBRARY) $(MINGW_AMD64_LIBRARY) -municode -mwindows $(WIN32_RENDERER_LIBS) $(WIN32_HOST_LIBS) $(WIN32_TRANSPARENCY_LIBS) $(WIN32_TERM_RENDERER_LIBS) -ladvapi32 -lcomctl32 -Wl,--wrap=boxrig_win32_opengl_terminal_component_create_child -o $@

$(BUILD_DIR)/boxrig_term/win32/i686/boxrig_term_gl_unavailable.exe: $(MINGW_I686_EXAMPLE) tests/win32/unavailable_opengl_sample.c Makefile
	$(MINGW_I686_CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(MINGW_TERM_CPPFLAGS) $(WIN32_TERM_RENDERER_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(WIN32_EXAMPLE_SOURCES) tests/win32/unavailable_opengl_sample.c $(MINGW_I686_EXAMPLE_RESOURCE) $(WIN32_TERM_I686_RENDERER_LIBRARIES) $(MINGW_I686_WIN32_LIBRARY) $(MINGW_I686_SESSION_LIBRARY) $(MINGW_I686_UI_LIBRARY) $(MINGW_I686_LIBRARY) -municode -mwindows $(WIN32_RENDERER_LIBS) $(WIN32_HOST_LIBS) $(WIN32_TRANSPARENCY_LIBS) $(WIN32_TERM_RENDERER_LIBS) -ladvapi32 -lcomctl32 -Wl,--wrap=boxrig_win32_opengl_terminal_component_create_child -o $@

$(BUILD_DIR)/boxrig_term/win32/i686/boxrig_term_ime_probe.exe: $(WIN32_EXAMPLE_SOURCES) examples/boxrig_term/win32/application_internal.h examples/boxrig_term/common/color_demo.h examples/boxrig_term/common/scrollbar_demo.h \
		tests/p2/win32_ime_placement_probe.c $(MINGW_I686_EXAMPLE_RESOURCE) \
		$(UI_HEADERS) $(WIN32_HEADERS) $(MINGW_I686_WIN32_LIBRARY) \
		$(MINGW_I686_SESSION_LIBRARY) $(MINGW_I686_UI_LIBRARY) \
		$(MINGW_I686_LIBRARY) $(CONPTY_HEADER) \
		| $(BUILD_DIR)/boxrig_term/win32/i686
	$(MINGW_I686_CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(MINGW_TERM_CPPFLAGS) \
		$(CFLAGS) $(PROJECT_CFLAGS) $(WIN32_EXAMPLE_SOURCES) \
		tests/p2/win32_ime_placement_probe.c $(MINGW_I686_EXAMPLE_RESOURCE) \
		$(MINGW_I686_WIN32_LIBRARY) $(MINGW_I686_SESSION_LIBRARY) \
		$(MINGW_I686_UI_LIBRARY) $(MINGW_I686_LIBRARY) \
		-municode -mwindows \
		$(WIN32_RENDERER_LIBS) $(WIN32_HOST_LIBS) $(WIN32_TRANSPARENCY_LIBS) \
		-ladvapi32 -Wl,--wrap=boxrig_win32_input_adapter_create \
		-Wl,--wrap=boxrig_win32_input_adapter_synchronize_candidate \
		-Wl,--wrap=boxrig_win32_input_adapter_resynchronize_candidate -lcomctl32 -o $@

test-p2-win32: $(BUILD_DIR)/boxrig_term/win32/i686/boxrig_term_ime_probe.exe

.PHONY: install-core-win32-amd64 uninstall-core-win32-amd64 uninstall-win32-amd64 uninstall-win32-opengl-amd64
install-core-win32-amd64: $(MINGW_AMD64_LIBRARY) packaging/win32/boxrig.pc
	$(INSTALL) -d $(DESTDIR)$(LIBDIR) $(DESTDIR)$(PKGCONFIGDIR) $(DESTDIR)$(INCLUDEDIR)/boxrig
	$(INSTALL) -m 644 $(MINGW_AMD64_LIBRARY) $(DESTDIR)$(LIBDIR)
	$(INSTALL) -m 644 $(CORE_PUBLIC_HEADERS) $(DESTDIR)$(INCLUDEDIR)/boxrig
	$(INSTALL) -m 644 packaging/win32/boxrig.pc $(DESTDIR)$(PKGCONFIGDIR)

uninstall-core-win32-amd64:
	# Other installed modules may still contain core codec code and its notices.
	rm -f $(DESTDIR)$(LIBDIR)/libboxrig.a $(DESTDIR)$(PKGCONFIGDIR)/boxrig.pc $(addprefix $(DESTDIR)$(INCLUDEDIR)/boxrig/,$(notdir $(CORE_PUBLIC_HEADERS)))

# Shared core/UI files remain available for separately installed backends.
uninstall-win32-amd64:
	rm -f $(DESTDIR)$(LIBDIR)/libboxrig-win32.a $(DESTDIR)$(LIBDIR)/libboxrig-session.a $(DESTDIR)$(PKGCONFIGDIR)/boxrig-win32.pc
	rm -f $(DESTDIR)$(INCLUDEDIR)/boxrig/win32.h $(DESTDIR)$(INCLUDEDIR)/boxrig/session.h $(addprefix $(DESTDIR)$(INCLUDEDIR)/boxrig/win32/,$(notdir $(WIN32_SOFTWARE_HEADERS)))
	rm -f $(DESTDIR)$(PREFIX)/share/boxrig/examples/win32/component.c $(DESTDIR)$(PREFIX)/share/boxrig/examples/win32/pty.c $(DESTDIR)$(PREFIX)/share/boxrig/examples/win32/external_scrollbar.h

uninstall-win32-opengl-amd64:
	rm -f $(DESTDIR)$(LIBDIR)/libboxrig-render-ogl.a $(DESTDIR)$(LIBDIR)/libboxrig-win32-ogl.a $(DESTDIR)$(LIBDIR)/libboxrig-render-ogl.dll.a $(DESTDIR)$(LIBDIR)/libboxrig-win32-ogl.dll.a
	rm -f $(DESTDIR)$(PREFIX)/bin/boxrig-render-ogl.dll $(DESTDIR)$(PREFIX)/bin/boxrig-win32-ogl.dll $(DESTDIR)$(PKGCONFIGDIR)/boxrig-render-ogl.pc $(DESTDIR)$(PKGCONFIGDIR)/boxrig-win32-ogl.pc
	rm -f $(DESTDIR)$(INCLUDEDIR)/boxrig/win32/opengl.h $(DESTDIR)$(INCLUDEDIR)/boxrig/win32/opengl_component.h $(DESTDIR)$(INCLUDEDIR)/boxrig/win32/wgl.h $(addprefix $(DESTDIR)$(INCLUDEDIR)/boxrig/render/,$(notdir $(OPENGL_RENDERER_HEADERS)))
	rm -f $(DESTDIR)$(PREFIX)/share/boxrig/examples/win32/renderer.c

.PHONY: install-core-win32-i686 uninstall-core-win32-i686 uninstall-win32-i686 uninstall-win32-opengl-i686
install-core-win32-i686: $(MINGW_I686_LIBRARY) packaging/win32/boxrig.pc
	$(INSTALL) -d $(DESTDIR)$(LIBDIR) $(DESTDIR)$(PKGCONFIGDIR) $(DESTDIR)$(INCLUDEDIR)/boxrig
	$(INSTALL) -m 644 $(MINGW_I686_LIBRARY) $(DESTDIR)$(LIBDIR)
	$(INSTALL) -m 644 $(CORE_PUBLIC_HEADERS) $(DESTDIR)$(INCLUDEDIR)/boxrig
	$(INSTALL) -m 644 packaging/win32/boxrig.pc $(DESTDIR)$(PKGCONFIGDIR)

uninstall-core-win32-i686:
	# Other installed modules may still contain core codec code and its notices.
	rm -f $(DESTDIR)$(LIBDIR)/libboxrig.a $(DESTDIR)$(PKGCONFIGDIR)/boxrig.pc $(addprefix $(DESTDIR)$(INCLUDEDIR)/boxrig/,$(notdir $(CORE_PUBLIC_HEADERS)))

# Shared core/UI files remain available for separately installed backends.
uninstall-win32-i686:
	rm -f $(DESTDIR)$(LIBDIR)/libboxrig-win32.a $(DESTDIR)$(LIBDIR)/libboxrig-session.a $(DESTDIR)$(PKGCONFIGDIR)/boxrig-win32.pc
	rm -f $(DESTDIR)$(INCLUDEDIR)/boxrig/win32.h $(DESTDIR)$(INCLUDEDIR)/boxrig/session.h $(addprefix $(DESTDIR)$(INCLUDEDIR)/boxrig/win32/,$(notdir $(WIN32_SOFTWARE_HEADERS)))
	rm -f $(DESTDIR)$(PREFIX)/share/boxrig/examples/win32/component.c $(DESTDIR)$(PREFIX)/share/boxrig/examples/win32/pty.c $(DESTDIR)$(PREFIX)/share/boxrig/examples/win32/external_scrollbar.h

uninstall-win32-opengl-i686:
	rm -f $(DESTDIR)$(LIBDIR)/libboxrig-render-ogl.a $(DESTDIR)$(LIBDIR)/libboxrig-win32-ogl.a $(DESTDIR)$(LIBDIR)/libboxrig-render-ogl.dll.a $(DESTDIR)$(LIBDIR)/libboxrig-win32-ogl.dll.a
	rm -f $(DESTDIR)$(PREFIX)/bin/boxrig-render-ogl.dll $(DESTDIR)$(PREFIX)/bin/boxrig-win32-ogl.dll $(DESTDIR)$(PKGCONFIGDIR)/boxrig-render-ogl.pc $(DESTDIR)$(PKGCONFIGDIR)/boxrig-win32-ogl.pc
	rm -f $(DESTDIR)$(INCLUDEDIR)/boxrig/win32/opengl.h $(DESTDIR)$(INCLUDEDIR)/boxrig/win32/opengl_component.h $(DESTDIR)$(INCLUDEDIR)/boxrig/win32/wgl.h $(addprefix $(DESTDIR)$(INCLUDEDIR)/boxrig/render/,$(notdir $(OPENGL_RENDERER_HEADERS)))
	rm -f $(DESTDIR)$(PREFIX)/share/boxrig/examples/win32/renderer.c

# Layered child windows require the Windows 8 compatibility manifest on Windows.
$(MINGW_AMD64_OPACITY_NATIVE_TEST): tests/win32/test_win32_opacity_native.c include/boxrig/win32/opacity_native.h src/win32/opacity_native_private.h $(MINGW_AMD64_BUILD_DIR)/tests/basic_ui_manifest.o Makefile $(MINGW_AMD64_WIN32_LIBRARY) $(MINGW_AMD64_UI_LIBRARY) $(MINGW_AMD64_LIBRARY) | $(MINGW_AMD64_BUILD_DIR)/tests
	$(MINGW_AMD64_CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(MINGW_WIN32_CPPFLAGS) -Isrc/win32 $(CFLAGS) $(PROJECT_CFLAGS) $< $(MINGW_AMD64_BUILD_DIR)/tests/basic_ui_manifest.o $(MINGW_AMD64_WIN32_LIBRARY) $(MINGW_AMD64_UI_LIBRARY) $(MINGW_AMD64_LIBRARY) $(WIN32_TRANSPARENCY_LIBS) -o $@

$(MINGW_I686_OPACITY_NATIVE_TEST): tests/win32/test_win32_opacity_native.c include/boxrig/win32/opacity_native.h src/win32/opacity_native_private.h $(MINGW_I686_BUILD_DIR)/tests/basic_ui_manifest.o Makefile $(MINGW_I686_WIN32_LIBRARY) $(MINGW_I686_UI_LIBRARY) $(MINGW_I686_LIBRARY) | $(MINGW_I686_BUILD_DIR)/tests
	$(MINGW_I686_CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(MINGW_WIN32_CPPFLAGS) -Isrc/win32 $(CFLAGS) $(PROJECT_CFLAGS) $< $(MINGW_I686_BUILD_DIR)/tests/basic_ui_manifest.o $(MINGW_I686_WIN32_LIBRARY) $(MINGW_I686_UI_LIBRARY) $(MINGW_I686_LIBRARY) $(WIN32_TRANSPARENCY_LIBS) -o $@

# Exercise new application policies before the remaining complete regression suite.
# Each sample resolves the console launcher beside its own executable.
.PHONY: test-win32-opacity-policy
test-p2-win32: $(MINGW_AMD64_BUILD_DIR)/boxrig-opacity-unavailable.exe $(MINGW_I686_BUILD_DIR)/boxrig-opacity-unavailable.exe
test-win32-opacity-policy: $(MINGW_AMD64_BUILD_DIR)/boxrig-opacity-unavailable.exe \
	$(MINGW_AMD64_SAMPLE_TEST) $(MINGW_AMD64_EXAMPLE) $(MINGW_AMD64_PTY_HELPER) \
	$(MINGW_AMD64_LAUNCHER) $(MINGW_AMD64_TEST_LAUNCHER) \
	$(MINGW_I686_BUILD_DIR)/boxrig-opacity-unavailable.exe \
	$(MINGW_I686_SAMPLE_TEST) $(MINGW_I686_EXAMPLE) $(MINGW_I686_PTY_HELPER) \
	$(MINGW_I686_LAUNCHER) $(MINGW_I686_TEST_LAUNCHER)
	BOXRIG_TRANSPARENCY_MODE=opaque $(WINE_RUN) $(abspath $(MINGW_AMD64_SAMPLE_TEST)) --background-opacity $(abspath $(MINGW_AMD64_EXAMPLE)) $(abspath $(MINGW_AMD64_PTY_HELPER))
	$(WINE_RUN) $(abspath $(MINGW_AMD64_SAMPLE_TEST)) --fallback-opacity $(abspath $(MINGW_AMD64_BUILD_DIR)/boxrig-opacity-unavailable.exe) $(abspath $(MINGW_AMD64_PTY_HELPER))
	BOXRIG_TRANSPARENCY_MODE=opaque $(WINE_RUN) $(abspath $(MINGW_I686_SAMPLE_TEST)) --background-opacity $(abspath $(MINGW_I686_EXAMPLE)) $(abspath $(MINGW_I686_PTY_HELPER))
	$(WINE_RUN) $(abspath $(MINGW_I686_SAMPLE_TEST)) --fallback-opacity $(abspath $(MINGW_I686_BUILD_DIR)/boxrig-opacity-unavailable.exe) $(abspath $(MINGW_I686_PTY_HELPER))

$(MINGW_AMD64_BUILD_DIR)/boxrig-opacity-unavailable.exe: $(WIN32_EXAMPLE_SOURCES) examples/boxrig_term/win32/application_internal.h examples/boxrig_term/common/color_demo.h examples/boxrig_term/common/scrollbar_demo.h tests/win32/opacity_unavailable.c $(MINGW_AMD64_EXAMPLE_RESOURCE) $(UI_HEADERS) $(WIN32_HEADERS) $(MINGW_AMD64_WIN32_LIBRARY) $(MINGW_AMD64_SESSION_LIBRARY) $(MINGW_AMD64_UI_LIBRARY) $(MINGW_AMD64_LIBRARY) $(CONPTY_HEADER) | $(MINGW_AMD64_BUILD_DIR)
	$(MINGW_AMD64_CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(MINGW_TERM_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(WIN32_EXAMPLE_SOURCES) tests/win32/opacity_unavailable.c $(MINGW_AMD64_EXAMPLE_RESOURCE) $(MINGW_AMD64_WIN32_LIBRARY) $(MINGW_AMD64_SESSION_LIBRARY) $(MINGW_AMD64_UI_LIBRARY) $(MINGW_AMD64_LIBRARY) -municode -mwindows $(WIN32_RENDERER_LIBS) $(WIN32_HOST_LIBS) $(WIN32_TRANSPARENCY_LIBS) -ladvapi32 -Wl,--wrap=boxrig_win32_transparency_context_create -Wl,--wrap=boxrig_win32_terminal_component_frame -lcomctl32 -o $@

$(MINGW_I686_BUILD_DIR)/boxrig-opacity-unavailable.exe: $(WIN32_EXAMPLE_SOURCES) examples/boxrig_term/win32/application_internal.h examples/boxrig_term/common/color_demo.h examples/boxrig_term/common/scrollbar_demo.h tests/win32/opacity_unavailable.c $(MINGW_I686_EXAMPLE_RESOURCE) $(UI_HEADERS) $(WIN32_HEADERS) $(MINGW_I686_WIN32_LIBRARY) $(MINGW_I686_SESSION_LIBRARY) $(MINGW_I686_UI_LIBRARY) $(MINGW_I686_LIBRARY) $(CONPTY_HEADER) | $(MINGW_I686_BUILD_DIR)
	$(MINGW_I686_CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(MINGW_TERM_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(WIN32_EXAMPLE_SOURCES) tests/win32/opacity_unavailable.c $(MINGW_I686_EXAMPLE_RESOURCE) $(MINGW_I686_WIN32_LIBRARY) $(MINGW_I686_SESSION_LIBRARY) $(MINGW_I686_UI_LIBRARY) $(MINGW_I686_LIBRARY) -municode -mwindows $(WIN32_RENDERER_LIBS) $(WIN32_HOST_LIBS) $(WIN32_TRANSPARENCY_LIBS) -ladvapi32 -Wl,--wrap=boxrig_win32_transparency_context_create -Wl,--wrap=boxrig_win32_terminal_component_frame -lcomctl32 -o $@

# Optional GPU APIs must fail locally without disabling the basic UI.
D3D11_OPTIONAL_AMD64 := $(MINGW_AMD64_BUILD_DIR)/tests/test_d3d11_optional.exe
$(D3D11_OPTIONAL_AMD64): tests/win32/test_d3d11_optional.c src/win32/system_library.h src/win32_d3d11/native.h $(D3D11_MANAGED_AMD64_LIBRARY) $(D3D11_AMD64_LIBRARY) $(MINGW_AMD64_WIN32_LIBRARY) $(MINGW_AMD64_UI_LIBRARY) $(MINGW_AMD64_LIBRARY)
	$(MINGW_AMD64_CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(MINGW_PROBE_CPPFLAGS) -Isrc/win32 -Isrc/win32_d3d11 $(CFLAGS) $(PROJECT_CFLAGS) $< $(D3D11_MANAGED_AMD64_LIBRARY) $(D3D11_AMD64_LIBRARY) $(MINGW_AMD64_WIN32_LIBRARY) $(MINGW_AMD64_UI_LIBRARY) $(MINGW_AMD64_LIBRARY) $(WIN32_RENDERER_LIBS) -ld3d11 -ld2d1 -ldxguid -ldcomp -lcomctl32 -Wl,--wrap=boxrig_win32_library_system_default -o $@
D3D11_OPTIONAL_I686 := $(MINGW_I686_BUILD_DIR)/tests/test_d3d11_optional.exe
$(D3D11_OPTIONAL_I686): tests/win32/test_d3d11_optional.c src/win32/system_library.h src/win32_d3d11/native.h $(D3D11_MANAGED_I686_LIBRARY) $(D3D11_I686_LIBRARY) $(MINGW_I686_WIN32_LIBRARY) $(MINGW_I686_UI_LIBRARY) $(MINGW_I686_LIBRARY)
	$(MINGW_I686_CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(MINGW_PROBE_CPPFLAGS) -Isrc/win32 -Isrc/win32_d3d11 $(CFLAGS) $(PROJECT_CFLAGS) $< $(D3D11_MANAGED_I686_LIBRARY) $(D3D11_I686_LIBRARY) $(MINGW_I686_WIN32_LIBRARY) $(MINGW_I686_UI_LIBRARY) $(MINGW_I686_LIBRARY) $(WIN32_RENDERER_LIBS) -ld3d11 -ld2d1 -ldxguid -ldcomp -lcomctl32 -Wl,--wrap=boxrig_win32_library_system_default -o $@
.PHONY: test-d3d11-optional
test: test-d3d11-optional
test-d3d11-optional: $(D3D11_OPTIONAL_AMD64) $(D3D11_OPTIONAL_I686)
	$(WINE_RUN) $(abspath $(D3D11_OPTIONAL_AMD64))
	$(WINE_RUN) $(abspath $(D3D11_OPTIONAL_I686))
test-p2-win32: $(D3D11_OPTIONAL_AMD64) $(D3D11_OPTIONAL_I686)
$(MINGW_AMD64_BUILD_DIR)/d3d11/renderer.o $(MINGW_I686_BUILD_DIR)/d3d11/renderer.o $(MINGW_AMD64_BUILD_DIR)/dcomp/native.o $(MINGW_I686_BUILD_DIR)/dcomp/native.o: src/win32/system_library.h

# Modal sizing must consume a presentation notification on its owning thread.
$(MINGW_AMD64_BUILD_DIR)/tests/test_presentation_wait.exe: tests/win32/test_presentation_wait.c examples/boxrig_term/win32/presentation_wait.c examples/boxrig_term/win32/application_internal.h | $(MINGW_AMD64_BUILD_DIR)/tests
	$(MINGW_AMD64_CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(MINGW_WIN32_CPPFLAGS) -Iexamples/boxrig_term/win32 $(CFLAGS) $(PROJECT_CFLAGS) tests/win32/test_presentation_wait.c examples/boxrig_term/win32/presentation_wait.c -o $@
$(MINGW_I686_BUILD_DIR)/tests/test_presentation_wait.exe: tests/win32/test_presentation_wait.c examples/boxrig_term/win32/presentation_wait.c examples/boxrig_term/win32/application_internal.h | $(MINGW_I686_BUILD_DIR)/tests
	$(MINGW_I686_CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(MINGW_WIN32_CPPFLAGS) -Iexamples/boxrig_term/win32 $(CFLAGS) $(PROJECT_CFLAGS) tests/win32/test_presentation_wait.c examples/boxrig_term/win32/presentation_wait.c -o $@
.PHONY: test-presentation-wait
test: test-presentation-wait
test-presentation-wait: $(MINGW_AMD64_BUILD_DIR)/tests/test_presentation_wait.exe $(MINGW_I686_BUILD_DIR)/tests/test_presentation_wait.exe
	$(WINE_RUN) $(abspath $(MINGW_AMD64_BUILD_DIR)/tests/test_presentation_wait.exe)
	$(WINE_RUN) $(abspath $(MINGW_I686_BUILD_DIR)/tests/test_presentation_wait.exe)


$(MINGW_AMD64_BUILD_DIR)/tests/d3d11_completion.o: tests/win32/d3d11_completion.cpp $(D3D11_MANAGED_HEADERS) | $(MINGW_AMD64_BUILD_DIR)/tests
	$(MINGW_AMD64_CXX) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(MINGW_WIN32_CPPFLAGS) -Isrc/win32_d3d11 $(CFLAGS) -std=c++20 $(WARNINGS) -fno-exceptions -fno-rtti -c $< -o $@

$(MINGW_I686_BUILD_DIR)/tests/d3d11_completion.o: tests/win32/d3d11_completion.cpp $(D3D11_MANAGED_HEADERS) | $(MINGW_I686_BUILD_DIR)/tests
	$(MINGW_I686_CXX) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(MINGW_WIN32_CPPFLAGS) -Isrc/win32_d3d11 $(CFLAGS) -std=c++20 $(WARNINGS) -fno-exceptions -fno-rtti -c $< -o $@

# Retain opaque D3D11 when composition alone is unavailable.
D3D11_FALLBACK_AMD64 := $(MINGW_AMD64_BUILD_DIR)/tests/test_d3d11_fallback.exe
$(D3D11_FALLBACK_AMD64): tests/win32/test_d3d11_fallback.c src/win32/system_library.h src/win32_d3d11/native.h $(D3D11_MANAGED_AMD64_LIBRARY) $(D3D11_AMD64_LIBRARY) $(MINGW_AMD64_WIN32_LIBRARY) $(MINGW_AMD64_UI_LIBRARY) $(MINGW_AMD64_LIBRARY)
	$(MINGW_AMD64_CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(MINGW_PROBE_CPPFLAGS) -Isrc/win32 -Isrc/win32_d3d11 $(CFLAGS) $(PROJECT_CFLAGS) $< $(D3D11_MANAGED_AMD64_LIBRARY) $(D3D11_AMD64_LIBRARY) $(MINGW_AMD64_WIN32_LIBRARY) $(MINGW_AMD64_UI_LIBRARY) $(MINGW_AMD64_LIBRARY) $(WIN32_RENDERER_LIBS) -ld3d11 -ld2d1 -ldxguid -ldcomp -ldwmapi -lcomctl32 -Wl,--wrap=boxrig_win32_library_system_default -o $@
D3D11_FALLBACK_I686 := $(MINGW_I686_BUILD_DIR)/tests/test_d3d11_fallback.exe
$(D3D11_FALLBACK_I686): tests/win32/test_d3d11_fallback.c src/win32/system_library.h src/win32_d3d11/native.h $(D3D11_MANAGED_I686_LIBRARY) $(D3D11_I686_LIBRARY) $(MINGW_I686_WIN32_LIBRARY) $(MINGW_I686_UI_LIBRARY) $(MINGW_I686_LIBRARY)
	$(MINGW_I686_CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(MINGW_PROBE_CPPFLAGS) -Isrc/win32 -Isrc/win32_d3d11 $(CFLAGS) $(PROJECT_CFLAGS) $< $(D3D11_MANAGED_I686_LIBRARY) $(D3D11_I686_LIBRARY) $(MINGW_I686_WIN32_LIBRARY) $(MINGW_I686_UI_LIBRARY) $(MINGW_I686_LIBRARY) $(WIN32_RENDERER_LIBS) -ld3d11 -ld2d1 -ldxguid -ldcomp -ldwmapi -lcomctl32 -Wl,--wrap=boxrig_win32_library_system_default -o $@
.PHONY: test-d3d11-fallback
test: test-d3d11-fallback
test-d3d11-fallback: $(D3D11_FALLBACK_AMD64) $(D3D11_FALLBACK_I686)
	$(WINE_RUN) $(abspath $(D3D11_FALLBACK_AMD64))
	$(WINE_RUN) $(abspath $(D3D11_FALLBACK_I686))
test-p2-win32: $(D3D11_FALLBACK_AMD64) $(D3D11_FALLBACK_I686)


# The same distributable CLI exercises application-owned fallback with optional DComp absent.
$(BUILD_DIR)/boxrig_term/win32/amd64/boxrig_term_no_dcomp.exe: tests/win32/dcomp_unavailable.c $(WIN32_TERM_AMD64_RENDERER_LIBRARIES) $(WIN32_EXAMPLE_SOURCES) examples/boxrig_term/win32/application_internal.h examples/boxrig_term/common/color_demo.h examples/boxrig_term/common/scrollbar_demo.h $(MINGW_AMD64_EXAMPLE_RESOURCE) $(UI_HEADERS) $(WIN32_HEADERS) $(MINGW_AMD64_WIN32_LIBRARY) $(MINGW_AMD64_SESSION_LIBRARY) $(MINGW_AMD64_UI_LIBRARY) $(MINGW_AMD64_LIBRARY) $(CONPTY_HEADER) | $(BUILD_DIR)/boxrig_term/win32/amd64
	$(MINGW_AMD64_CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(MINGW_TERM_CPPFLAGS) $(WIN32_TERM_RENDERER_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(WIN32_EXAMPLE_SOURCES) tests/win32/dcomp_unavailable.c $(MINGW_AMD64_EXAMPLE_RESOURCE) $(WIN32_TERM_AMD64_RENDERER_LIBRARIES) $(MINGW_AMD64_WIN32_LIBRARY) $(MINGW_AMD64_SESSION_LIBRARY) $(MINGW_AMD64_UI_LIBRARY) $(MINGW_AMD64_LIBRARY) -municode -mwindows $(WIN32_RENDERER_LIBS) $(WIN32_HOST_LIBS) $(WIN32_TRANSPARENCY_LIBS) $(WIN32_TERM_RENDERER_LIBS) -ladvapi32 -Wl,--wrap=boxrig_win32_library_system_default -lcomctl32 -o $@

test-p2-win32: $(BUILD_DIR)/boxrig_term/win32/amd64/boxrig_term_no_dcomp.exe
$(BUILD_DIR)/boxrig_term/win32/i686/boxrig_term_no_dcomp.exe: tests/win32/dcomp_unavailable.c $(WIN32_TERM_I686_RENDERER_LIBRARIES) $(WIN32_EXAMPLE_SOURCES) examples/boxrig_term/win32/application_internal.h examples/boxrig_term/common/color_demo.h examples/boxrig_term/common/scrollbar_demo.h $(MINGW_I686_EXAMPLE_RESOURCE) $(UI_HEADERS) $(WIN32_HEADERS) $(MINGW_I686_WIN32_LIBRARY) $(MINGW_I686_SESSION_LIBRARY) $(MINGW_I686_UI_LIBRARY) $(MINGW_I686_LIBRARY) $(CONPTY_HEADER) | $(BUILD_DIR)/boxrig_term/win32/i686
	$(MINGW_I686_CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(MINGW_TERM_CPPFLAGS) $(WIN32_TERM_RENDERER_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(WIN32_EXAMPLE_SOURCES) tests/win32/dcomp_unavailable.c $(MINGW_I686_EXAMPLE_RESOURCE) $(WIN32_TERM_I686_RENDERER_LIBRARIES) $(MINGW_I686_WIN32_LIBRARY) $(MINGW_I686_SESSION_LIBRARY) $(MINGW_I686_UI_LIBRARY) $(MINGW_I686_LIBRARY) -municode -mwindows $(WIN32_RENDERER_LIBS) $(WIN32_HOST_LIBS) $(WIN32_TRANSPARENCY_LIBS) $(WIN32_TERM_RENDERER_LIBS) -ladvapi32 -Wl,--wrap=boxrig_win32_library_system_default -lcomctl32 -o $@

test-p2-win32: $(BUILD_DIR)/boxrig_term/win32/i686/boxrig_term_no_dcomp.exe


# The same distributable CLI exercises application-owned fallback with owner mode exchanges and frame rollback.
$(BUILD_DIR)/boxrig_term/win32/amd64/boxrig_term_opacity_probe.exe: tests/win32/opacity_mode_probe.c $(WIN32_TERM_AMD64_RENDERER_LIBRARIES) $(WIN32_EXAMPLE_SOURCES) examples/boxrig_term/win32/application_internal.h examples/boxrig_term/common/color_demo.h examples/boxrig_term/common/scrollbar_demo.h $(MINGW_AMD64_EXAMPLE_RESOURCE) $(UI_HEADERS) $(WIN32_HEADERS) $(MINGW_AMD64_WIN32_LIBRARY) $(MINGW_AMD64_SESSION_LIBRARY) $(MINGW_AMD64_UI_LIBRARY) $(MINGW_AMD64_LIBRARY) $(CONPTY_HEADER) | $(BUILD_DIR)/boxrig_term/win32/amd64
	$(MINGW_AMD64_CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(MINGW_TERM_CPPFLAGS) $(WIN32_TERM_RENDERER_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(WIN32_EXAMPLE_SOURCES) tests/win32/opacity_mode_probe.c $(MINGW_AMD64_EXAMPLE_RESOURCE) $(WIN32_TERM_AMD64_RENDERER_LIBRARIES) $(MINGW_AMD64_WIN32_LIBRARY) $(MINGW_AMD64_SESSION_LIBRARY) $(MINGW_AMD64_UI_LIBRARY) $(MINGW_AMD64_LIBRARY) -municode -mwindows $(WIN32_RENDERER_LIBS) $(WIN32_HOST_LIBS) $(WIN32_TRANSPARENCY_LIBS) $(WIN32_TERM_RENDERER_LIBS) -ladvapi32 -Wl,--wrap=boxrig_term_win32_run -Wl,--wrap=boxrig_win32_terminal_component_set_key_router -Wl,--wrap=boxrig_win32_terminal_component_frame -lcomctl32 -o $@

test-p2-win32: $(BUILD_DIR)/boxrig_term/win32/amd64/boxrig_term_opacity_probe.exe
$(BUILD_DIR)/boxrig_term/win32/i686/boxrig_term_opacity_probe.exe: tests/win32/opacity_mode_probe.c $(WIN32_TERM_I686_RENDERER_LIBRARIES) $(WIN32_EXAMPLE_SOURCES) examples/boxrig_term/win32/application_internal.h examples/boxrig_term/common/color_demo.h examples/boxrig_term/common/scrollbar_demo.h $(MINGW_I686_EXAMPLE_RESOURCE) $(UI_HEADERS) $(WIN32_HEADERS) $(MINGW_I686_WIN32_LIBRARY) $(MINGW_I686_SESSION_LIBRARY) $(MINGW_I686_UI_LIBRARY) $(MINGW_I686_LIBRARY) $(CONPTY_HEADER) | $(BUILD_DIR)/boxrig_term/win32/i686
	$(MINGW_I686_CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(MINGW_TERM_CPPFLAGS) $(WIN32_TERM_RENDERER_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(WIN32_EXAMPLE_SOURCES) tests/win32/opacity_mode_probe.c $(MINGW_I686_EXAMPLE_RESOURCE) $(WIN32_TERM_I686_RENDERER_LIBRARIES) $(MINGW_I686_WIN32_LIBRARY) $(MINGW_I686_SESSION_LIBRARY) $(MINGW_I686_UI_LIBRARY) $(MINGW_I686_LIBRARY) -municode -mwindows $(WIN32_RENDERER_LIBS) $(WIN32_HOST_LIBS) $(WIN32_TRANSPARENCY_LIBS) $(WIN32_TERM_RENDERER_LIBS) -ladvapi32 -Wl,--wrap=boxrig_term_win32_run -Wl,--wrap=boxrig_win32_terminal_component_set_key_router -Wl,--wrap=boxrig_win32_terminal_component_frame -lcomctl32 -o $@

test-p2-win32: $(BUILD_DIR)/boxrig_term/win32/i686/boxrig_term_opacity_probe.exe

$(EXAMPLE_BINARY) $(GTK3_EXAMPLE_BINARY) $(GTK4_EXAMPLE_BINARY) $(WAYLAND_EXAMPLE_BINARY) \
$(FRONTEND_SAMPLE_X11_PROBE_BINARY) $(FRONTEND_SAMPLE_GTK3_PROBE_BINARY) \
$(FRONTEND_SAMPLE_GTK4_PROBE_BINARY) $(FRONTEND_SAMPLE_WAYLAND_PROBE_BINARY) \
$(WAYLAND_NO_LIBDECOR_TEST_BINARY): examples/boxrig_term/common/color_demo.h

$(MINGW_AMD64_BUILD_DIR)/tests/test_win32_colors.exe: tests/win32/test_win32_colors.c \
		tests/color_renderer_fixture.c tests/color_renderer_fixture.h \
		$(MINGW_AMD64_WIN32_LIBRARY) $(MINGW_AMD64_UI_LIBRARY) \
		$(MINGW_AMD64_LIBRARY) | $(MINGW_AMD64_BUILD_DIR)/tests
	$(MINGW_AMD64_CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(MINGW_WIN32_CPPFLAGS) \
		$(CFLAGS) $(PROJECT_CFLAGS) tests/win32/test_win32_colors.c \
		tests/color_renderer_fixture.c $(MINGW_AMD64_WIN32_LIBRARY) \
		$(MINGW_AMD64_UI_LIBRARY) $(MINGW_AMD64_LIBRARY) $(WIN32_RENDERER_LIBS) -o $@

$(MINGW_I686_BUILD_DIR)/tests/test_win32_colors.exe: tests/win32/test_win32_colors.c \
		tests/color_renderer_fixture.c tests/color_renderer_fixture.h \
		$(MINGW_I686_WIN32_LIBRARY) $(MINGW_I686_UI_LIBRARY) \
		$(MINGW_I686_LIBRARY) | $(MINGW_I686_BUILD_DIR)/tests
	$(MINGW_I686_CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(MINGW_WIN32_CPPFLAGS) \
		$(CFLAGS) $(PROJECT_CFLAGS) tests/win32/test_win32_colors.c \
		tests/color_renderer_fixture.c $(MINGW_I686_WIN32_LIBRARY) \
		$(MINGW_I686_UI_LIBRARY) $(MINGW_I686_LIBRARY) $(WIN32_RENDERER_LIBS) -o $@

$(BUILD_DIR)/tests/color-filter-example: examples/ui/color_filter.c $(UI_HEADERS) $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $< $(UI_LIBRARY) $(LIBRARY) -o $@

.PHONY: test-blink
test-blink: $(BUILD_DIR)/tests/test_blink_phase $(BUILD_DIR)/tests/test_animation_clock \
        $(BUILD_DIR)/tests/test_animation_runtime $(BUILD_DIR)/tests/test_animation_poll \
        $(BUILD_DIR)/tests/test_x11_blink
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(BUILD_DIR)/tests/test_blink_phase
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(BUILD_DIR)/tests/test_animation_clock
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(BUILD_DIR)/tests/test_animation_runtime
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(BUILD_DIR)/tests/test_animation_poll
	$(XVFB_RUN) -a env BOXRIG_TEST_BLINK_VIDEO=$(abspath $(BUILD_DIR)/tests/x11-blink-fake.mkv) ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(BUILD_DIR)/tests/test_x11_blink
	$(XVFB_RUN) -a env BOXRIG_TEST_BLINK_REAL_CLOCK=1 BOXRIG_TEST_BLINK_VIDEO=$(abspath $(BUILD_DIR)/tests/x11-blink-real.mkv) ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(BUILD_DIR)/tests/test_x11_blink

$(BUILD_DIR)/tests/test_blink_phase: tests/test_blink_phase.c src/ui/blink_private.h $(UI_HEADERS) $(UI_LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) -Isrc/ui $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) $< $(UI_LIBRARY) -o $@

$(BUILD_DIR)/tests/test_animation_clock: tests/test_animation_clock.c $(CORE_HEADERS) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) $< $(LIBRARY) -Wl,--wrap=calloc -o $@

$(BUILD_DIR)/tests/test_animation_runtime: tests/test_animation_runtime.c $(UI_HEADERS) $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) $< $(UI_LIBRARY) $(LIBRARY) -o $@

$(BUILD_DIR)/tests/test_animation_poll: tests/test_animation_poll.c $(UI_HEADERS) $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) $< $(UI_LIBRARY) $(LIBRARY) -o $@

$(BUILD_DIR)/tests/test_x11_blink: tests/x11/test_x11_blink.c $(UI_HEADERS) $(X11_HEADERS) $(X11_LIBRARY) $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(POSIX_CPPFLAGS) $(X11_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) $< $(X11_LIBRARY) $(UI_LIBRARY) $(LIBRARY) $(X11_LIBS) $(DBUS_LIBS) -o $@

# Native cursor phase tests share the public view contract.
.PHONY: test-native-blink
test-blink: test-native-blink
test-native-blink: $(BUILD_DIR)/tests/test_gtk_animation \
	$(BUILD_DIR)/tests/test_gtk3_widget_animation $(BUILD_DIR)/tests/test_gtk4_widget_animation \
	$(BUILD_DIR)/tests/test_pangocairo_blink $(BUILD_DIR)/tests/test_opengl_blink \
	$(MINGW_AMD64_BUILD_DIR)/tests/test_win32_blink.exe $(MINGW_I686_BUILD_DIR)/tests/test_win32_blink.exe \
	$(MINGW_AMD64_BUILD_DIR)/tests/test_win32_animation.exe $(MINGW_I686_BUILD_DIR)/tests/test_win32_animation.exe
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(BUILD_DIR)/tests/test_gtk_animation
	$(XVFB_RUN) -a env GDK_BACKEND=x11 NO_AT_BRIDGE=1 ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(BUILD_DIR)/tests/test_gtk3_widget_animation
	$(XVFB_RUN) -a env BOXRIG_TEST_GTK4_IM_MODULE=gtk-im-context-simple GTK_IM_MODULE=gtk-im-context-simple GDK_BACKEND=x11 GSK_RENDERER=cairo NO_AT_BRIDGE=1 ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(BUILD_DIR)/tests/test_gtk4_widget_animation
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(BUILD_DIR)/tests/test_pangocairo_blink
	$(XVFB_RUN) -a env LIBGL_ALWAYS_SOFTWARE=1 ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(BUILD_DIR)/tests/test_opengl_blink
	$(WINE_RUN) $(abspath $(MINGW_AMD64_BUILD_DIR)/tests/test_win32_blink.exe)
	$(WINE_RUN) $(abspath $(MINGW_I686_BUILD_DIR)/tests/test_win32_blink.exe)
	$(WINE_RUN) $(abspath $(MINGW_AMD64_BUILD_DIR)/tests/test_win32_animation.exe)
	$(WINE_RUN) $(abspath $(MINGW_I686_BUILD_DIR)/tests/test_win32_animation.exe)

$(BUILD_DIR)/gtk3/terminal_widget.o $(BUILD_DIR)/gtk4/terminal_widget.o: src/gtk/animation.h
$(MINGW_AMD64_BUILD_DIR)/win32/component.o $(MINGW_I686_BUILD_DIR)/win32/component.o: src/win32/animation.h src/win32/input_private.h
$(MINGW_AMD64_BUILD_DIR)/win32/input.o $(MINGW_I686_BUILD_DIR)/win32/input.o: src/win32/input_private.h
$(MINGW_AMD64_BUILD_DIR)/win32/animation.o $(MINGW_I686_BUILD_DIR)/win32/animation.o: src/win32/animation.h

$(BUILD_DIR)/tests/test_gtk_animation: tests/gtk/test_animation.c src/gtk/animation.h $(UI_HEADERS) $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) -Isrc/gtk $(GTK3_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) $< $(UI_LIBRARY) $(LIBRARY) $(GTK3_LIBS) -Wl,--wrap=g_get_monotonic_time -o $@

$(BUILD_DIR)/tests/test_gtk3_widget_animation: tests/gtk/test_widget_animation.c $(GTK3_HEADERS) $(GTK3_LIBRARY) $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(GTK3_CPPFLAGS) -DBOXRIG_TEST_GTK3 $(CFLAGS) $(GTK3_PROJECT_CFLAGS) $(SANITIZERS) $< $(GTK3_LIBRARY) $(UI_LIBRARY) $(LIBRARY) $(GTK3_LIBS) -lm -Wl,--wrap=g_get_monotonic_time -o $@

$(BUILD_DIR)/tests/test_gtk4_widget_animation: tests/gtk/test_widget_animation.c $(GTK4_HEADERS) $(GTK4_LIBRARY) $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(GTK4_CPPFLAGS) $(CFLAGS) $(GTK4_PROJECT_CFLAGS) $(SANITIZERS) $< $(GTK4_LIBRARY) $(UI_LIBRARY) $(LIBRARY) $(GTK4_LIBS) -lm -Wl,--wrap=g_get_monotonic_time -o $@

.PHONY: test-gtk-initial-font
test: test-gtk-initial-font
test-gtk-initial-font: $(BUILD_DIR)/tests/test_gtk3_initial_font $(BUILD_DIR)/tests/test_gtk4_initial_font
	@set -e; for version in 3 4; do for scale in 1 2; do \
		$(XVFB_RUN) -a env GDK_BACKEND=x11 GDK_SCALE=$$scale NO_AT_BRIDGE=1 ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(BUILD_DIR)/tests/test_gtk$${version}_initial_font; \
	done; done

$(BUILD_DIR)/tests/test_gtk3_initial_font: tests/gtk/test_widget_initial_font.c $(GTK3_HEADERS) $(GTK3_LIBRARY) $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(GTK3_CPPFLAGS) -DBOXRIG_TEST_GTK3 $(CFLAGS) $(GTK3_PROJECT_CFLAGS) $(SANITIZERS) $< $(GTK3_LIBRARY) $(UI_LIBRARY) $(LIBRARY) $(GTK3_LIBS) -lm -o $@

$(BUILD_DIR)/tests/test_gtk4_initial_font: tests/gtk/test_widget_initial_font.c $(GTK4_HEADERS) $(GTK4_LIBRARY) $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(GTK4_CPPFLAGS) $(CFLAGS) $(GTK4_PROJECT_CFLAGS) $(SANITIZERS) $< $(GTK4_LIBRARY) $(UI_LIBRARY) $(LIBRARY) $(GTK4_LIBS) -lm -o $@

$(BUILD_DIR)/tests/test_pangocairo_blink: tests/pangocairo/test_pangocairo_blink.c tests/blink_renderer_fixture.c tests/selection_renderer_fixture.c tests/blink_renderer_fixture.h tests/selection_renderer_fixture.h tests/color_renderer_fixture.h src/pangocairo/renderer.c $(PANGO_CANDIDATE_OBJECT) src/pangocairo/renderer.h src/pangocairo/renderer_private.h src/pangocairo/glyph_fit.h src/ui/glyph_fit.h src/ui/view_private.h $(UI_HEADERS) $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) -Isrc -Itests $(POSIX_CPPFLAGS) $(PANGO_CAIRO_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) $< tests/blink_renderer_fixture.c tests/selection_renderer_fixture.c src/pangocairo/renderer.c $(PANGO_CANDIDATE_OBJECT) $(UI_LIBRARY) $(LIBRARY) $(PANGO_CAIRO_LIBS) -o $@

$(BUILD_DIR)/tests/test_opengl_blink: tests/test_opengl_blink.c tests/blink_renderer_fixture.c tests/selection_renderer_fixture.c tests/blink_renderer_fixture.h tests/selection_renderer_fixture.h tests/color_renderer_fixture.h tests/opengl_host.c tests/opengl_host.h $(OPENGL_RENDERER_LIBRARY) $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) -Isrc -Itests $(POSIX_CPPFLAGS) $(OPENGL_RENDERER_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) $< tests/blink_renderer_fixture.c tests/selection_renderer_fixture.c tests/opengl_host.c $(OPENGL_RENDERER_LIBRARY) $(UI_LIBRARY) $(LIBRARY) $(OPENGL_RENDERER_LIBS) -o $@

$(MINGW_AMD64_BUILD_DIR)/tests/test_win32_blink.exe: tests/win32/test_win32_blink.c tests/blink_renderer_fixture.c tests/selection_renderer_fixture.c tests/decoration_integration_fixture.c tests/blink_renderer_fixture.h tests/selection_renderer_fixture.h tests/color_renderer_fixture.h $(MINGW_AMD64_WIN32_LIBRARY) $(MINGW_AMD64_UI_LIBRARY) $(MINGW_AMD64_LIBRARY) | $(MINGW_AMD64_BUILD_DIR)/tests
	$(MINGW_AMD64_CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(MINGW_WIN32_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $< tests/blink_renderer_fixture.c tests/selection_renderer_fixture.c tests/decoration_integration_fixture.c $(MINGW_AMD64_WIN32_LIBRARY) $(MINGW_AMD64_UI_LIBRARY) $(MINGW_AMD64_LIBRARY) $(WIN32_RENDERER_LIBS) -o $@

$(MINGW_I686_BUILD_DIR)/tests/test_win32_blink.exe: tests/win32/test_win32_blink.c tests/blink_renderer_fixture.c tests/selection_renderer_fixture.c tests/decoration_integration_fixture.c tests/blink_renderer_fixture.h tests/selection_renderer_fixture.h tests/color_renderer_fixture.h $(MINGW_I686_WIN32_LIBRARY) $(MINGW_I686_UI_LIBRARY) $(MINGW_I686_LIBRARY) | $(MINGW_I686_BUILD_DIR)/tests
	$(MINGW_I686_CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(MINGW_WIN32_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $< tests/blink_renderer_fixture.c tests/selection_renderer_fixture.c tests/decoration_integration_fixture.c $(MINGW_I686_WIN32_LIBRARY) $(MINGW_I686_UI_LIBRARY) $(MINGW_I686_LIBRARY) $(WIN32_RENDERER_LIBS) -o $@

$(MINGW_AMD64_BUILD_DIR)/tests/test_win32_animation.exe: tests/win32/test_win32_animation.c src/win32/animation.c src/win32/animation.h $(MINGW_AMD64_WIN32_LIBRARY) $(MINGW_AMD64_UI_LIBRARY) $(MINGW_AMD64_LIBRARY) | $(MINGW_AMD64_BUILD_DIR)/tests
	$(MINGW_AMD64_CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(MINGW_WIN32_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) -mnop-fun-dllimport $< src/win32/animation.c $(MINGW_AMD64_WIN32_LIBRARY) $(MINGW_AMD64_UI_LIBRARY) $(MINGW_AMD64_LIBRARY) $(WIN32_RENDERER_LIBS) -Wl,--wrap=GetTickCount64 -Wl,--wrap=SetTimer -Wl,--wrap=KillTimer -o $@

$(MINGW_I686_BUILD_DIR)/tests/test_win32_animation.exe: tests/win32/test_win32_animation.c src/win32/animation.c src/win32/animation.h $(MINGW_I686_WIN32_LIBRARY) $(MINGW_I686_UI_LIBRARY) $(MINGW_I686_LIBRARY) | $(MINGW_I686_BUILD_DIR)/tests
	$(MINGW_I686_CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(MINGW_WIN32_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) -mnop-fun-dllimport $< src/win32/animation.c $(MINGW_I686_WIN32_LIBRARY) $(MINGW_I686_UI_LIBRARY) $(MINGW_I686_LIBRARY) $(WIN32_RENDERER_LIBS) -Wl,--wrap=GetTickCount64@0 -Wl,--wrap=SetTimer@16 -Wl,--wrap=KillTimer@8 -o $@

test-p2-win32: $(MINGW_AMD64_BUILD_DIR)/tests/test_win32_animation.exe $(MINGW_I686_BUILD_DIR)/tests/test_win32_animation.exe $(MINGW_AMD64_BUILD_DIR)/tests/test_win32_blink.exe $(MINGW_I686_BUILD_DIR)/tests/test_win32_blink.exe

# Adapters embed frontend configuration values whose layout depends on UI headers.
$(MINGW_AMD64_WGL_OBJECTS) $(MINGW_I686_WGL_OBJECTS): $(WIN32_HEADERS) $(UI_HEADERS)
$(MINGW_AMD64_BUILD_DIR)/dcomp/component.o $(MINGW_I686_BUILD_DIR)/dcomp/component.o: $(WIN32_HEADERS) $(UI_HEADERS)

.PHONY: test-x11-scheduler-required
test-x11-scheduler-required: $(BUILD_DIR)/tests/test_x11_scheduler_required
	$(XVFB_RUN) -a env ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $<

$(BUILD_DIR)/tests/test_x11_scheduler_required: tests/x11/test_x11_scheduler_required.c $(X11_LIBRARY) $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(POSIX_CPPFLAGS) $(X11_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) $< $(X11_LIBRARY) $(UI_LIBRARY) $(LIBRARY) $(X11_LIBS) $(DBUS_LIBS) -o $@

.PHONY: test-native-blink-videos
test-native-blink: test-native-blink-videos test-sync-recovery-videos
test-native-blink-videos: $(EXAMPLE_BINARY) $(GTK3_EXAMPLE_BINARY) $(GTK4_EXAMPLE_BINARY) $(WAYLAND_EXAMPLE_BINARY) $(PTY_HELPER_BINARY)
	$(XVFB_RUN) -a -s '-screen 0 1280x1024x24' node tests/run_native_cursor_video.mjs $(EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/test-results/step23-blink/x11-software x11
	$(XVFB_RUN) -a -s '-screen 0 1280x1024x24' env LIBGL_ALWAYS_SOFTWARE=1 node tests/run_native_cursor_video.mjs $(EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/test-results/step23-blink/x11-opengl opengl
	$(XVFB_RUN) -a -s '-screen 0 1280x1024x24' env GDK_BACKEND=x11 node tests/run_native_cursor_video.mjs $(GTK3_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/test-results/step23-blink/gtk3-x11
	$(XVFB_RUN) -a -s '-screen 0 1280x1024x24' env $(GTK4_TEST_IM_ENV) GDK_BACKEND=x11 GSK_RENDERER=cairo node tests/run_native_cursor_video.mjs $(GTK4_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/test-results/step23-blink/gtk4-x11
	$(XVFB_RUN) -a -s '-screen 0 1280x1024x24' env BOXRIG_TEST_BLINK_WAYLAND=1 GDK_BACKEND=wayland node tests/run_native_cursor_video.mjs $(GTK3_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/test-results/step23-blink/gtk3-wayland
	$(XVFB_RUN) -a -s '-screen 0 1280x1024x24' env $(GTK4_TEST_IM_ENV) BOXRIG_TEST_BLINK_WAYLAND=1 GDK_BACKEND=wayland GSK_RENDERER=cairo node tests/run_native_cursor_video.mjs $(GTK4_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/test-results/step23-blink/gtk4-wayland
	$(XVFB_RUN) -a -s '-screen 0 1280x1024x24' env BOXRIG_TEST_BLINK_WAYLAND=1 node tests/run_native_cursor_video.mjs $(WAYLAND_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/test-results/step23-blink/native-wayland-software wayland
	$(XVFB_RUN) -a -s '-screen 0 1280x1024x24' env BOXRIG_TEST_BLINK_WAYLAND=1 LIBGL_ALWAYS_SOFTWARE=1 node tests/run_native_cursor_video.mjs $(WAYLAND_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/test-results/step23-blink/native-wayland-opengl opengl

$(MINGW_AMD64_BUILD_DIR)/tests/blink_focus_peer.exe: tests/win32/blink_focus_peer.c | $(MINGW_AMD64_BUILD_DIR)/tests
	$(MINGW_AMD64_CC) $(CPPFLAGS) $(MINGW_TERM_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $< -luser32 -o $@

$(MINGW_I686_BUILD_DIR)/tests/blink_focus_peer.exe: tests/win32/blink_focus_peer.c | $(MINGW_I686_BUILD_DIR)/tests
	$(MINGW_I686_CC) $(CPPFLAGS) $(MINGW_TERM_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $< -luser32 -o $@

test-p2-win32: $(MINGW_AMD64_BUILD_DIR)/tests/blink_focus_peer.exe $(MINGW_I686_BUILD_DIR)/tests/blink_focus_peer.exe

.PHONY: test-text-blink-core
test-text-blink-core: $(BUILD_DIR)/tests/test_text_blink_core
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $<

$(BUILD_DIR)/tests/test_text_blink_core: tests/test_text_blink_core.c $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) $< $(LIBRARY) $(SANITIZERS) -o $@

.PHONY: test-text-blink
test-text-blink: test-text-blink-core $(BUILD_DIR)/tests/test_text_blink_runtime $(BUILD_DIR)/tests/test_x11_text_blink test-native-text-blink-videos
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(BUILD_DIR)/tests/test_text_blink_runtime
	$(XVFB_RUN) -a $(BUILD_DIR)/tests/test_x11_text_blink

$(BUILD_DIR)/tests/test_text_blink_runtime: tests/test_text_blink_runtime.c $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) $< $(UI_LIBRARY) $(LIBRARY) -Wl,--wrap=boxrig_terminal_get_cell $(SANITIZERS) -o $@

$(BUILD_DIR)/tests/test_x11_text_blink: tests/x11/test_x11_text_blink.c tests/blink_renderer_fixture.c tests/selection_renderer_fixture.c tests/blink_renderer_fixture.h tests/selection_renderer_fixture.h $(X11_LIBRARY) $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(POSIX_CPPFLAGS) $(X11_CPPFLAGS) -Itests $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) tests/x11/test_x11_text_blink.c tests/blink_renderer_fixture.c tests/selection_renderer_fixture.c $(X11_LIBRARY) $(UI_LIBRARY) $(LIBRARY) $(X11_LIBS) $(DBUS_LIBS) $(SANITIZERS) -o $@

.PHONY: test-native-text-blink-videos
test-native-text-blink-videos: $(EXAMPLE_BINARY) $(GTK3_EXAMPLE_BINARY) $(GTK4_EXAMPLE_BINARY) $(WAYLAND_EXAMPLE_BINARY) $(PTY_HELPER_BINARY)
	$(XVFB_RUN) -a -s '-screen 0 1280x1024x24' node tests/run_native_text_blink_video.mjs $(EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/test-results/step23-text-blink/x11-software x11
	$(XVFB_RUN) -a -s '-screen 0 1280x1024x24' env LIBGL_ALWAYS_SOFTWARE=1 node tests/run_native_text_blink_video.mjs $(EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/test-results/step23-text-blink/x11-opengl opengl
	$(XVFB_RUN) -a -s '-screen 0 1280x1024x24' env GDK_BACKEND=x11 node tests/run_native_text_blink_video.mjs $(GTK3_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/test-results/step23-text-blink/gtk3-x11
	$(XVFB_RUN) -a -s '-screen 0 1280x1024x24' env $(GTK4_TEST_IM_ENV) GDK_BACKEND=x11 GSK_RENDERER=cairo node tests/run_native_text_blink_video.mjs $(GTK4_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/test-results/step23-text-blink/gtk4-x11
	$(XVFB_RUN) -a -s '-screen 0 1280x1024x24' env BOXRIG_TEST_BLINK_WAYLAND=1 GDK_BACKEND=wayland node tests/run_native_text_blink_video.mjs $(GTK3_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/test-results/step23-text-blink/gtk3-wayland
	$(XVFB_RUN) -a -s '-screen 0 1280x1024x24' env $(GTK4_TEST_IM_ENV) BOXRIG_TEST_BLINK_WAYLAND=1 GDK_BACKEND=wayland GSK_RENDERER=cairo node tests/run_native_text_blink_video.mjs $(GTK4_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/test-results/step23-text-blink/gtk4-wayland
	$(XVFB_RUN) -a -s '-screen 0 1280x1024x24' env BOXRIG_TEST_BLINK_WAYLAND=1 node tests/run_native_text_blink_video.mjs $(WAYLAND_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/test-results/step23-text-blink/native-wayland-software wayland
	$(XVFB_RUN) -a -s '-screen 0 1280x1024x24' env BOXRIG_TEST_BLINK_WAYLAND=1 LIBGL_ALWAYS_SOFTWARE=1 node tests/run_native_text_blink_video.mjs $(WAYLAND_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/test-results/step23-text-blink/native-wayland-opengl opengl

.PHONY: test-cursor-style
test-cursor-style: $(BUILD_DIR)/tests/test_cursor_style $(BUILD_DIR)/tests/test_decoration_demo
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(BUILD_DIR)/tests/test_cursor_style
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(BUILD_DIR)/tests/test_decoration_demo

.PHONY: test-native-scrollbar
test: test-native-scrollbar
$(EXAMPLE_BINARY) $(GTK3_EXAMPLE_BINARY) $(GTK4_EXAMPLE_BINARY) $(WAYLAND_EXAMPLE_BINARY): examples/boxrig_term/common/scrollbar_demo.h
test-native-scrollbar: $(EXAMPLE_BINARY) $(GTK3_EXAMPLE_BINARY) $(GTK4_EXAMPLE_BINARY) $(WAYLAND_EXAMPLE_BINARY) $(PTY_HELPER_BINARY)
	$(XVFB_RUN) -a -s '-screen 0 1280x1024x24' node tests/run_native_scrollbar.mjs $(EXAMPLE_BINARY) $(PTY_HELPER_BINARY) x11
	$(XVFB_RUN) -a -s '-screen 0 1280x1024x24' env LIBGL_ALWAYS_SOFTWARE=1 node tests/run_native_scrollbar.mjs $(EXAMPLE_BINARY) $(PTY_HELPER_BINARY) opengl
	$(XVFB_RUN) -a -s '-screen 0 1280x1024x24' env GDK_BACKEND=x11 node tests/run_native_scrollbar.mjs $(GTK3_EXAMPLE_BINARY) $(PTY_HELPER_BINARY)
	$(XVFB_RUN) -a -s '-screen 0 1280x1024x24' env $(GTK4_TEST_IM_ENV) GDK_BACKEND=x11 GSK_RENDERER=cairo node tests/run_native_scrollbar.mjs $(GTK4_EXAMPLE_BINARY) $(PTY_HELPER_BINARY)
	$(XVFB_RUN) -a -s '-screen 0 1280x1024x24' env BOXRIG_TEST_SCROLLBAR_WAYLAND=1 GDK_BACKEND=wayland node tests/run_native_scrollbar.mjs $(GTK3_EXAMPLE_BINARY) $(PTY_HELPER_BINARY)
	$(XVFB_RUN) -a -s '-screen 0 1280x1024x24' env $(GTK4_TEST_IM_ENV) BOXRIG_TEST_SCROLLBAR_WAYLAND=1 GDK_BACKEND=wayland GSK_RENDERER=cairo node tests/run_native_scrollbar.mjs $(GTK4_EXAMPLE_BINARY) $(PTY_HELPER_BINARY)
	$(XVFB_RUN) -a -s '-screen 0 1280x1024x24' env BOXRIG_TEST_SCROLLBAR_WAYLAND=1 node tests/run_native_scrollbar.mjs $(WAYLAND_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) wayland
	$(XVFB_RUN) -a -s '-screen 0 1280x1024x24' env BOXRIG_TEST_SCROLLBAR_WAYLAND=1 LIBGL_ALWAYS_SOFTWARE=1 node tests/run_native_scrollbar.mjs $(WAYLAND_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) opengl
	$(XVFB_RUN) -a -s '-screen 0 2400x1800x24' env BOXRIG_TEST_SCROLLBAR_WAYLAND=1 BOXRIG_TEST_SCROLLBAR_SCALE=2 node tests/run_native_scrollbar.mjs $(WAYLAND_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) wayland
	$(XVFB_RUN) -a -s '-screen 0 2400x1800x24' env BOXRIG_TEST_SCROLLBAR_WAYLAND=1 BOXRIG_TEST_SCROLLBAR_SCALE=2 LIBGL_ALWAYS_SOFTWARE=1 node tests/run_native_scrollbar.mjs $(WAYLAND_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) opengl

.PHONY: test-native-gtk4-zoom-video
test: test-native-gtk4-zoom-video
test-native-gtk4-zoom-video: $(GTK4_EXAMPLE_BINARY) $(PTY_HELPER_BINARY)
	$(XVFB_RUN) -a -s '-screen 0 1280x1024x24' env $(GTK4_TEST_IM_ENV) GDK_BACKEND=x11 GSK_RENDERER=cairo node tests/run_native_gtk4_zoom_video.mjs $(GTK4_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/test-results/plan13/gtk4-cairo cairo
	$(XVFB_RUN) -a -s '-screen 0 1280x1024x24' env $(GTK4_TEST_IM_ENV) GDK_BACKEND=x11 GDK_DEBUG=gl-prefer-gl GSK_RENDERER=gl LIBGL_ALWAYS_SOFTWARE=1 node tests/run_native_gtk4_zoom_video.mjs $(GTK4_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/test-results/plan13/gtk4-opengl opengl

.PHONY: test-scrollbar-demo
test: test-scrollbar-demo
test-scrollbar-demo: $(BUILD_DIR)/tests/test_scrollbar_demo
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $<

$(BUILD_DIR)/tests/test_scrollbar_demo: tests/test_scrollbar_demo.c examples/boxrig_term/common/scrollbar_demo.c examples/boxrig_term/common/scrollbar_demo.h $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(POSIX_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) $< examples/boxrig_term/common/scrollbar_demo.c $(UI_LIBRARY) $(LIBRARY) -o $@

$(BUILD_DIR)/tests/test_cursor_style: tests/test_cursor_style.c $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) $< $(UI_LIBRARY) $(LIBRARY) -o $@

# The same private ABI is compiled at the basic Win32 floor; consumers need
# neither newer SDK declarations nor statically imported ConPTY functions.
PTY_RUNTIME_TEST_DEFINES := -mnop-fun-dllimport \
	-DCreatePipe=boxrig_test_CreatePipe -DCloseHandle=boxrig_test_CloseHandle \
	-DGetProcAddress=boxrig_test_GetProcAddress \
	-DUpdateProcThreadAttribute=boxrig_test_UpdateProcThreadAttribute \
	-DCreateProcessW=boxrig_test_CreateProcessW

$(MINGW_AMD64_BUILD_DIR)/tests/pty_runtime_impl.o: src/win32/pty.c src/win32/conpty_api.h include/boxrig/win32/pty.h Makefile | $(MINGW_AMD64_BUILD_DIR)/tests
	$(MINGW_AMD64_CC) $(PUBLIC_CPPFLAGS) $(MINGW_UI_CPPFLAGS) $(PROJECT_CFLAGS) $(PTY_RUNTIME_TEST_DEFINES) -c $< -o $@

$(MINGW_I686_BUILD_DIR)/tests/pty_runtime_impl.o: src/win32/pty.c src/win32/conpty_api.h include/boxrig/win32/pty.h Makefile | $(MINGW_I686_BUILD_DIR)/tests
	$(MINGW_I686_CC) $(PUBLIC_CPPFLAGS) $(MINGW_UI_CPPFLAGS) $(PROJECT_CFLAGS) $(PTY_RUNTIME_TEST_DEFINES) -c $< -o $@

$(MINGW_AMD64_BUILD_DIR)/tests/conpty_runtime_impl.o: src/win32/conpty_runtime.c src/win32/conpty_api.h include/boxrig/win32/pty.h Makefile | $(MINGW_AMD64_BUILD_DIR)/tests
	$(MINGW_AMD64_CC) $(PUBLIC_CPPFLAGS) $(MINGW_UI_CPPFLAGS) $(PROJECT_CFLAGS) -mnop-fun-dllimport -DGetProcAddress=boxrig_test_GetProcAddress -c $< -o $@

$(MINGW_I686_BUILD_DIR)/tests/conpty_runtime_impl.o: src/win32/conpty_runtime.c src/win32/conpty_api.h include/boxrig/win32/pty.h Makefile | $(MINGW_I686_BUILD_DIR)/tests
	$(MINGW_I686_CC) $(PUBLIC_CPPFLAGS) $(MINGW_UI_CPPFLAGS) $(PROJECT_CFLAGS) -mnop-fun-dllimport -DGetProcAddress=boxrig_test_GetProcAddress -c $< -o $@

$(MINGW_AMD64_BUILD_DIR)/tests/pty_runtime_fixture.dll: tests/win32/pty_runtime_fixture.c | $(MINGW_AMD64_BUILD_DIR)/tests
	$(MINGW_AMD64_CC) $(MINGW_UI_CPPFLAGS) $(PROJECT_CFLAGS) -shared $< -Wl,--kill-at -o $@

$(MINGW_I686_BUILD_DIR)/tests/pty_runtime_fixture.dll: tests/win32/pty_runtime_fixture.c | $(MINGW_I686_BUILD_DIR)/tests
	$(MINGW_I686_CC) $(MINGW_UI_CPPFLAGS) $(PROJECT_CFLAGS) -shared $< -Wl,--kill-at -o $@

$(MINGW_AMD64_BUILD_DIR)/tests/pty_runtime_missing.dll: tests/win32/pty_runtime_fixture.c | $(MINGW_AMD64_BUILD_DIR)/tests
	$(MINGW_AMD64_CC) $(MINGW_UI_CPPFLAGS) $(PROJECT_CFLAGS) -DBOXRIG_TEST_MISSING_EXPORT -shared $< -Wl,--kill-at -o $@

$(MINGW_I686_BUILD_DIR)/tests/pty_runtime_missing.dll: tests/win32/pty_runtime_fixture.c | $(MINGW_I686_BUILD_DIR)/tests
	$(MINGW_I686_CC) $(MINGW_UI_CPPFLAGS) $(PROJECT_CFLAGS) -DBOXRIG_TEST_MISSING_EXPORT -shared $< -Wl,--kill-at -o $@

$(MINGW_AMD64_BUILD_DIR)/tests/test_pty_runtime.exe: tests/win32/test_pty_runtime.c $(MINGW_AMD64_BUILD_DIR)/tests/pty_runtime_impl.o $(MINGW_AMD64_BUILD_DIR)/tests/conpty_runtime_impl.o | $(MINGW_AMD64_BUILD_DIR)/tests/pty_runtime_fixture.dll $(MINGW_AMD64_BUILD_DIR)/tests/pty_runtime_missing.dll
	$(MINGW_AMD64_CC) $(PUBLIC_CPPFLAGS) $(MINGW_UI_CPPFLAGS) $(PROJECT_CFLAGS) $^ -o $@

$(MINGW_I686_BUILD_DIR)/tests/test_pty_runtime.exe: tests/win32/test_pty_runtime.c $(MINGW_I686_BUILD_DIR)/tests/pty_runtime_impl.o $(MINGW_I686_BUILD_DIR)/tests/conpty_runtime_impl.o | $(MINGW_I686_BUILD_DIR)/tests/pty_runtime_fixture.dll $(MINGW_I686_BUILD_DIR)/tests/pty_runtime_missing.dll
	$(MINGW_I686_CC) $(PUBLIC_CPPFLAGS) $(MINGW_UI_CPPFLAGS) $(PROJECT_CFLAGS) $^ -o $@

.PHONY: test-win32-pty-runtime
test-win32-pty-runtime: $(MINGW_AMD64_BUILD_DIR)/tests/test_pty_runtime.exe $(MINGW_I686_BUILD_DIR)/tests/test_pty_runtime.exe
	$(WINE_RUN) $(abspath $(MINGW_AMD64_BUILD_DIR)/tests/test_pty_runtime.exe)
	$(WINE_RUN) $(abspath $(MINGW_I686_BUILD_DIR)/tests/test_pty_runtime.exe)

test: test-win32-pty-runtime

$(MINGW_AMD64_BUILD_DIR)/tests/test_pty_spawn.exe: tests/win32/test_pty_spawn.cpp $(MINGW_AMD64_WIN32_LIBRARY) deps/cardio/include/cardio.h | $(MINGW_AMD64_BUILD_DIR)/tests
	$(MINGW_AMD64_CXX) $(PUBLIC_CPPFLAGS) $(MINGW_WIN32_CPPFLAGS) -Ideps/cardio/include $(CFLAGS) -std=c++20 $(WARNINGS) -static -municode $< $(MINGW_AMD64_WIN32_LIBRARY) -lversion -o $@

$(MINGW_I686_BUILD_DIR)/tests/test_pty_spawn.exe: tests/win32/test_pty_spawn.cpp $(MINGW_I686_WIN32_LIBRARY) deps/cardio/include/cardio.h | $(MINGW_I686_BUILD_DIR)/tests
	$(MINGW_I686_CXX) $(PUBLIC_CPPFLAGS) $(MINGW_WIN32_CPPFLAGS) -Ideps/cardio/include $(CFLAGS) -std=c++20 $(WARNINGS) -static -municode $< $(MINGW_I686_WIN32_LIBRARY) -lversion -o $@

test-p2-win32: $(MINGW_AMD64_BUILD_DIR)/tests/test_pty_spawn.exe $(MINGW_I686_BUILD_DIR)/tests/test_pty_spawn.exe

$(BUILD_DIR)/tests/test_decoration_demo: tests/test_decoration_demo.c examples/boxrig_term/common/decoration_demo.c examples/boxrig_term/common/color_demo.c $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(POSIX_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) tests/test_decoration_demo.c examples/boxrig_term/common/decoration_demo.c examples/boxrig_term/common/color_demo.c $(UI_LIBRARY) $(LIBRARY) -o $@

.PHONY: test-native-cursor-style-videos
test-native-cursor-style-videos: $(EXAMPLE_BINARY) $(GTK3_EXAMPLE_BINARY) $(GTK4_EXAMPLE_BINARY) $(WAYLAND_EXAMPLE_BINARY) $(PTY_HELPER_BINARY)
	$(XVFB_RUN) -a -s '-screen 0 1280x1024x24' node tests/run_native_cursor_style_video.mjs $(EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/test-results/step23-cursor-style/x11-software x11
	$(XVFB_RUN) -a -s '-screen 0 1280x1024x24' env LIBGL_ALWAYS_SOFTWARE=1 node tests/run_native_cursor_style_video.mjs $(EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/test-results/step23-cursor-style/x11-opengl opengl
	$(XVFB_RUN) -a -s '-screen 0 1280x1024x24' env GDK_BACKEND=x11 node tests/run_native_cursor_style_video.mjs $(GTK3_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/test-results/step23-cursor-style/gtk3-x11
	$(XVFB_RUN) -a -s '-screen 0 1280x1024x24' env $(GTK4_TEST_IM_ENV) GDK_BACKEND=x11 GSK_RENDERER=cairo node tests/run_native_cursor_style_video.mjs $(GTK4_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/test-results/step23-cursor-style/gtk4-x11
	$(XVFB_RUN) -a -s '-screen 0 1280x1024x24' env BOXRIG_TEST_BLINK_WAYLAND=1 GDK_BACKEND=wayland node tests/run_native_cursor_style_video.mjs $(GTK3_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/test-results/step23-cursor-style/gtk3-wayland
	$(XVFB_RUN) -a -s '-screen 0 1280x1024x24' env $(GTK4_TEST_IM_ENV) BOXRIG_TEST_BLINK_WAYLAND=1 GDK_BACKEND=wayland GSK_RENDERER=cairo node tests/run_native_cursor_style_video.mjs $(GTK4_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/test-results/step23-cursor-style/gtk4-wayland
	$(XVFB_RUN) -a -s '-screen 0 1280x1024x24' env BOXRIG_TEST_BLINK_WAYLAND=1 node tests/run_native_cursor_style_video.mjs $(WAYLAND_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/test-results/step23-cursor-style/native-wayland-software wayland
	$(XVFB_RUN) -a -s '-screen 0 1280x1024x24' env BOXRIG_TEST_BLINK_WAYLAND=1 LIBGL_ALWAYS_SOFTWARE=1 node tests/run_native_cursor_style_video.mjs $(WAYLAND_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/test-results/step23-cursor-style/native-wayland-opengl opengl

test: test-native-cursor-style-videos

.PHONY: test-native-cursor-box-videos
test-native-cursor-box-videos: $(EXAMPLE_BINARY) $(GTK3_EXAMPLE_BINARY) $(GTK4_EXAMPLE_BINARY) $(PTY_HELPER_BINARY)
	$(XVFB_RUN) -a -s '-screen 0 1280x1024x24' env BOXRIG_TEST_CURSOR_BOX=1 node tests/run_native_cursor_style_video.mjs $(EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/test-results/cursor-box/x11 x11
	$(XVFB_RUN) -a -s '-screen 0 1280x1024x24' env BOXRIG_TEST_CURSOR_BOX=1 LIBGL_ALWAYS_SOFTWARE=1 node tests/run_native_cursor_style_video.mjs $(EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/test-results/cursor-box/opengl opengl
	$(XVFB_RUN) -a -s '-screen 0 1280x1024x24' env BOXRIG_TEST_CURSOR_BOX=1 GDK_BACKEND=x11 node tests/run_native_cursor_style_video.mjs $(GTK3_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/test-results/cursor-box/gtk3
	$(XVFB_RUN) -a -s '-screen 0 1280x1024x24' env BOXRIG_TEST_CURSOR_BOX=1 $(GTK4_TEST_IM_ENV) GDK_BACKEND=x11 GSK_RENDERER=cairo node tests/run_native_cursor_style_video.mjs $(GTK4_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/test-results/cursor-box/gtk4

test: test-native-cursor-box-videos

.PHONY: test-native-wide-cursor-videos
test-native-wide-cursor-videos: $(EXAMPLE_BINARY) $(GTK3_EXAMPLE_BINARY) $(PTY_HELPER_BINARY)
	$(XVFB_RUN) -a -s '-screen 0 1280x1024x24' env BOXRIG_TEST_WIDE_CURSOR=1 GDK_BACKEND=x11 node tests/run_native_cursor_style_video.mjs $(GTK3_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/test-results/plan10-wide-cursor/gtk3
	$(XVFB_RUN) -a -s '-screen 0 1280x1024x24' env BOXRIG_TEST_WIDE_CURSOR=1 LIBGL_ALWAYS_SOFTWARE=1 node tests/run_native_cursor_style_video.mjs $(EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/test-results/plan10-wide-cursor/opengl opengl

test: test-native-wide-cursor-videos

.PHONY: test-native-font-cursor-videos
test-native-font-cursor-videos: $(EXAMPLE_BINARY) $(GTK3_EXAMPLE_BINARY) $(PTY_HELPER_BINARY)
	$(XVFB_RUN) -a -s '-screen 0 1280x1024x24' env BOXRIG_TEST_FONT_CURSOR=1 GDK_BACKEND=x11 node tests/run_native_cursor_style_video.mjs $(GTK3_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/test-results/plan10-font-cursor/gtk3
	$(XVFB_RUN) -a -s '-screen 0 1280x1024x24' env BOXRIG_TEST_FONT_CURSOR=1 LIBGL_ALWAYS_SOFTWARE=1 node tests/run_native_cursor_style_video.mjs $(EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/test-results/plan10-font-cursor/opengl opengl

test: test-native-font-cursor-videos

.PHONY: test-animation-integration
test-animation-integration: $(BUILD_DIR)/tests/test_cursor_animation_dirty $(BUILD_DIR)/tests/test_animation_integration $(BUILD_DIR)/tests/animation-scheduler-example $(BUILD_DIR)/tests/animation-manual-example
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(BUILD_DIR)/tests/test_cursor_animation_dirty
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(BUILD_DIR)/tests/test_animation_integration
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(BUILD_DIR)/tests/animation-scheduler-example
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(BUILD_DIR)/tests/animation-manual-example

$(BUILD_DIR)/tests/test_cursor_animation_dirty: tests/test_cursor_animation_dirty.c $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) $< $(UI_LIBRARY) $(LIBRARY) -lm -o $@

$(BUILD_DIR)/tests/test_animation_integration: tests/test_animation_integration.c $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) $< $(UI_LIBRARY) $(LIBRARY) -lm -Wl,--wrap=boxrig_animation_clock_update_monotonic_time -o $@

$(BUILD_DIR)/tests/animation-scheduler-example: examples/ui/animation_scheduler.c $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) $< $(UI_LIBRARY) $(LIBRARY) -lm -o $@

$(BUILD_DIR)/tests/animation-manual-example: examples/ui/animation_manual.c $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) $< $(UI_LIBRARY) $(LIBRARY) -lm -o $@

$(BUILD_DIR)/tests/test_example_wheel_policy: tests/test_example_wheel_policy.c examples/boxrig_term/common/mouse_policy.c examples/boxrig_term/common/mouse_policy.h $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) -Iexamples/boxrig_term/common $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) tests/test_example_wheel_policy.c examples/boxrig_term/common/mouse_policy.c $(UI_LIBRARY) $(LIBRARY) $(SANITIZERS) -o $@

.PHONY: test-wheel-policy test-native-wheel-policy
.PHONY: test-native-pixel-mouse
test: test-native-pixel-mouse
test-native-pixel-mouse: $(EXAMPLE_BINARY) $(GTK3_EXAMPLE_BINARY) $(GTK4_EXAMPLE_BINARY) $(WAYLAND_EXAMPLE_BINARY) $(PTY_HELPER_BINARY)
	BOXRIG_TEST_PIXEL_MOUSE=1 $(MAKE) test-native-wheel-policy

test-wheel-policy: $(BUILD_DIR)/tests/test_example_wheel_policy $(BUILD_DIR)/tests/test_gtk3_wheel $(BUILD_DIR)/tests/test_gtk4_wheel
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(BUILD_DIR)/tests/test_example_wheel_policy
	$(XVFB_RUN) -a env GDK_BACKEND=x11 NO_AT_BRIDGE=1 ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(BUILD_DIR)/tests/test_gtk3_wheel
	$(XVFB_RUN) -a env $(GTK4_TEST_IM_ENV) GDK_BACKEND=x11 GSK_RENDERER=cairo NO_AT_BRIDGE=1 ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(BUILD_DIR)/tests/test_gtk4_wheel
	$(XVFB_RUN) -a env GDK_BACKEND=x11 GDK_SCALE=2 NO_AT_BRIDGE=1 ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(BUILD_DIR)/tests/test_gtk3_wheel
	$(XVFB_RUN) -a env $(GTK4_TEST_IM_ENV) GDK_BACKEND=x11 GDK_SCALE=2 GSK_RENDERER=cairo NO_AT_BRIDGE=1 ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(BUILD_DIR)/tests/test_gtk4_wheel

$(BUILD_DIR)/tests/test_gtk3_wheel: tests/gtk3/test_gtk3_wheel.c $(GTK3_HEADERS) $(GTK3_LIBRARY) $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(GTK3_CPPFLAGS) $(CFLAGS) $(GTK3_PROJECT_CFLAGS) $(SANITIZERS) $< $(GTK3_LIBRARY) $(UI_LIBRARY) $(LIBRARY) $(GTK3_LIBS) -lm -o $@

$(BUILD_DIR)/tests/test_gtk4_wheel: tests/gtk4/test_gtk4_wheel.c $(GTK4_HEADERS) $(GTK4_LIBRARY) $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(GTK4_CPPFLAGS) $(CFLAGS) $(GTK4_PROJECT_CFLAGS) $(SANITIZERS) $< $(GTK4_LIBRARY) $(UI_LIBRARY) $(LIBRARY) $(GTK4_LIBS) -lm -o $@

test: test-native-wheel-policy
test-native-wheel-policy: $(EXAMPLE_BINARY) $(GTK3_EXAMPLE_BINARY) $(GTK4_EXAMPLE_BINARY) $(WAYLAND_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(X11_SAMPLE_TEST_BINARY) $(X11_FAKE_NOTIFICATION_SERVICE) $(POSIX_EMITTER_BINARY)
	dbus-run-session -- env ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(X11_SAMPLE_TEST_BINARY) $(EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(X11_FAKE_NOTIFICATION_SERVICE) $(POSIX_EMITTER_BINARY) --wheel-zoom
	$(XVFB_RUN) -a -s '-screen 0 1920x1200x24' env  node tests/run_native_wheel_policy.mjs $(EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/test-results/step23-wheel/x11-software x11
	$(XVFB_RUN) -a -s '-screen 0 1920x1200x24' env LIBGL_ALWAYS_SOFTWARE=1 node tests/run_native_wheel_policy.mjs $(EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/test-results/step23-wheel/x11-opengl opengl
	$(XVFB_RUN) -a -s '-screen 0 1920x1200x24' env GDK_BACKEND=x11 node tests/run_native_wheel_policy.mjs $(GTK3_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/test-results/step23-wheel/gtk3-x11
	$(XVFB_RUN) -a -s '-screen 0 1920x1200x24' env GDK_BACKEND=x11 GDK_SCALE=2 node tests/run_native_wheel_policy.mjs $(GTK3_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/test-results/step23-wheel/gtk3-x11-2x
	$(XVFB_RUN) -a -s '-screen 0 1920x1200x24' env $(GTK4_TEST_IM_ENV) GDK_BACKEND=x11 GSK_RENDERER=cairo node tests/run_native_wheel_policy.mjs $(GTK4_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/test-results/step23-wheel/gtk4-x11
	$(XVFB_RUN) -a -s '-screen 0 1920x1200x24' env $(GTK4_TEST_IM_ENV) GDK_BACKEND=x11 GSK_RENDERER=cairo GDK_SCALE=2 node tests/run_native_wheel_policy.mjs $(GTK4_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/test-results/step23-wheel/gtk4-x11-2x
	$(XVFB_RUN) -a -s '-screen 0 1920x1200x24' env BOXRIG_TEST_WHEEL_WAYLAND=1 GDK_BACKEND=wayland node tests/run_native_wheel_policy.mjs $(GTK3_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/test-results/step23-wheel/gtk3-wayland
	$(XVFB_RUN) -a -s '-screen 0 1920x1200x24' env BOXRIG_TEST_WHEEL_WAYLAND=1 GDK_BACKEND=wayland BOXRIG_TEST_WHEEL_SCALE=2 node tests/run_native_wheel_policy.mjs $(GTK3_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/test-results/step23-wheel/gtk3-wayland-2x
	$(XVFB_RUN) -a -s '-screen 0 1920x1200x24' env $(GTK4_TEST_IM_ENV) BOXRIG_TEST_WHEEL_WAYLAND=1 GDK_BACKEND=wayland GSK_RENDERER=cairo node tests/run_native_wheel_policy.mjs $(GTK4_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/test-results/step23-wheel/gtk4-wayland
	$(XVFB_RUN) -a -s '-screen 0 1920x1200x24' env $(GTK4_TEST_IM_ENV) BOXRIG_TEST_WHEEL_WAYLAND=1 GDK_BACKEND=wayland GSK_RENDERER=cairo BOXRIG_TEST_WHEEL_SCALE=2 node tests/run_native_wheel_policy.mjs $(GTK4_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/test-results/step23-wheel/gtk4-wayland-2x
	$(XVFB_RUN) -a -s '-screen 0 1920x1200x24' env BOXRIG_TEST_WHEEL_WAYLAND=1 node tests/run_native_wheel_policy.mjs $(WAYLAND_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/test-results/step23-wheel/wayland-software wayland
	$(XVFB_RUN) -a -s '-screen 0 1920x1200x24' env BOXRIG_TEST_WHEEL_WAYLAND=1 BOXRIG_TEST_WHEEL_SCALE=2 node tests/run_native_wheel_policy.mjs $(WAYLAND_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/test-results/step23-wheel/wayland-software-2x wayland
	$(XVFB_RUN) -a -s '-screen 0 1920x1200x24' env BOXRIG_TEST_WHEEL_WAYLAND=1 LIBGL_ALWAYS_SOFTWARE=1 node tests/run_native_wheel_policy.mjs $(WAYLAND_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/test-results/step23-wheel/wayland-opengl opengl
	$(XVFB_RUN) -a -s '-screen 0 1920x1200x24' env BOXRIG_TEST_WHEEL_WAYLAND=1 LIBGL_ALWAYS_SOFTWARE=1 BOXRIG_TEST_WHEEL_SCALE=2 node tests/run_native_wheel_policy.mjs $(WAYLAND_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/test-results/step23-wheel/wayland-opengl-2x opengl
	$(XVFB_RUN) -a -s '-screen 0 1920x1200x24' env GDK_BACKEND=x11 LIBGL_ALWAYS_SOFTWARE=1 node tests/run_native_wheel_policy.mjs $(GTK3_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/test-results/step23-wheel/gtk3-x11-opengl opengl
	$(XVFB_RUN) -a -s '-screen 0 1920x1200x24' env $(GTK4_TEST_IM_ENV) GDK_BACKEND=x11 GSK_RENDERER=cairo LIBGL_ALWAYS_SOFTWARE=1 node tests/run_native_wheel_policy.mjs $(GTK4_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/test-results/step23-wheel/gtk4-x11-opengl opengl
	$(XVFB_RUN) -a -s '-screen 0 1920x1200x24' env BOXRIG_TEST_WHEEL_WAYLAND=1 GDK_BACKEND=wayland LIBGL_ALWAYS_SOFTWARE=1 node tests/run_native_wheel_policy.mjs $(GTK3_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/test-results/step23-wheel/gtk3-wayland-opengl opengl
	$(XVFB_RUN) -a -s '-screen 0 1920x1200x24' env $(GTK4_TEST_IM_ENV) BOXRIG_TEST_WHEEL_WAYLAND=1 GDK_BACKEND=wayland GSK_RENDERER=cairo LIBGL_ALWAYS_SOFTWARE=1 node tests/run_native_wheel_policy.mjs $(GTK4_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/test-results/step23-wheel/gtk4-wayland-opengl opengl

.PHONY: test-example-copy test-native-copy-policy
test-example-copy: $(EXAMPLE_MOUSE_POLICY_TEST_BINARY)
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $<

test: test-native-copy-policy
test-native-copy-policy: $(EXAMPLE_BINARY) $(GTK3_EXAMPLE_BINARY) $(GTK4_EXAMPLE_BINARY) $(WAYLAND_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(GTK3_CLIPBOARD_PEER_BINARY)
	$(XVFB_RUN) -a -s '-screen 0 1920x1200x24' env  node tests/run_native_copy_policy.mjs $(EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(GTK3_CLIPBOARD_PEER_BINARY) $(BUILD_DIR)/test-results/step23-copy/x11 x11
	$(XVFB_RUN) -a -s '-screen 0 1920x1200x24' env LIBGL_ALWAYS_SOFTWARE=1 node tests/run_native_copy_policy.mjs $(EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(GTK3_CLIPBOARD_PEER_BINARY) $(BUILD_DIR)/test-results/step23-copy/x11-opengl opengl
	$(XVFB_RUN) -a -s '-screen 0 1920x1200x24' env GDK_BACKEND=x11 node tests/run_native_copy_policy.mjs $(GTK3_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(GTK3_CLIPBOARD_PEER_BINARY) $(BUILD_DIR)/test-results/step23-copy/gtk3-x11 cairo
	$(XVFB_RUN) -a -s '-screen 0 1920x1200x24' env GDK_BACKEND=x11 LIBGL_ALWAYS_SOFTWARE=1 node tests/run_native_copy_policy.mjs $(GTK3_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(GTK3_CLIPBOARD_PEER_BINARY) $(BUILD_DIR)/test-results/step23-copy/gtk3-x11-opengl opengl
	$(XVFB_RUN) -a -s '-screen 0 1920x1200x24' env $(GTK4_TEST_IM_ENV) GDK_BACKEND=x11 GSK_RENDERER=cairo node tests/run_native_copy_policy.mjs $(GTK4_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(GTK3_CLIPBOARD_PEER_BINARY) $(BUILD_DIR)/test-results/step23-copy/gtk4-x11
	$(XVFB_RUN) -a -s '-screen 0 1920x1200x24' env BOXRIG_TEST_COPY_WAYLAND=1 GDK_BACKEND=wayland node tests/run_native_copy_policy.mjs $(GTK3_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(GTK3_CLIPBOARD_PEER_BINARY) $(BUILD_DIR)/test-results/step23-copy/gtk3-wayland
	$(XVFB_RUN) -a -s '-screen 0 1920x1200x24' env $(GTK4_TEST_IM_ENV) BOXRIG_TEST_COPY_WAYLAND=1 GDK_BACKEND=wayland GSK_RENDERER=cairo node tests/run_native_copy_policy.mjs $(GTK4_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(GTK3_CLIPBOARD_PEER_BINARY) $(BUILD_DIR)/test-results/step23-copy/gtk4-wayland
	$(XVFB_RUN) -a -s '-screen 0 1920x1200x24' env BOXRIG_TEST_COPY_WAYLAND=1 node tests/run_native_copy_policy.mjs $(WAYLAND_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(GTK3_CLIPBOARD_PEER_BINARY) $(BUILD_DIR)/test-results/step23-copy/wayland wayland
	$(XVFB_RUN) -a -s '-screen 0 1920x1200x24' env BOXRIG_TEST_COPY_WAYLAND=1 LIBGL_ALWAYS_SOFTWARE=1 node tests/run_native_copy_policy.mjs $(WAYLAND_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(GTK3_CLIPBOARD_PEER_BINARY) $(BUILD_DIR)/test-results/step23-copy/wayland-opengl opengl

$(MINGW_AMD64_BUILD_DIR)/tests/animation_modules.dll: tests/win32/test_animation_modules.c src/win32/animation.c src/win32/animation.h $(MINGW_AMD64_BUILD_DIR)/libboxrig-ui.a $(MINGW_AMD64_BUILD_DIR)/libboxrig.a | $(MINGW_AMD64_BUILD_DIR)/tests
	$(MINGW_AMD64_CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(MINGW_WIN32_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) -shared -static-libgcc -DBOXRIG_TEST_ANIMATION_DLL tests/win32/test_animation_modules.c src/win32/animation.c $(MINGW_AMD64_BUILD_DIR)/libboxrig-ui.a $(MINGW_AMD64_BUILD_DIR)/libboxrig.a -luser32 -Wl,--out-implib,$(MINGW_AMD64_BUILD_DIR)/tests/animation_modules.dll.a -o $@

$(MINGW_AMD64_BUILD_DIR)/tests/test_animation_modules.exe: tests/win32/test_animation_modules.c src/win32/animation.c src/win32/animation.h $(MINGW_AMD64_BUILD_DIR)/libboxrig-ui.a $(MINGW_AMD64_BUILD_DIR)/libboxrig.a $(MINGW_AMD64_BUILD_DIR)/tests/animation_modules.dll
	$(MINGW_AMD64_CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(MINGW_WIN32_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) tests/win32/test_animation_modules.c src/win32/animation.c $(MINGW_AMD64_BUILD_DIR)/tests/animation_modules.dll.a $(MINGW_AMD64_BUILD_DIR)/libboxrig-ui.a $(MINGW_AMD64_BUILD_DIR)/libboxrig.a -luser32 -o $@

$(MINGW_I686_BUILD_DIR)/tests/animation_modules.dll: tests/win32/test_animation_modules.c src/win32/animation.c src/win32/animation.h $(MINGW_I686_BUILD_DIR)/libboxrig-ui.a $(MINGW_I686_BUILD_DIR)/libboxrig.a | $(MINGW_I686_BUILD_DIR)/tests
	$(MINGW_I686_CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(MINGW_WIN32_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) -shared -static-libgcc -DBOXRIG_TEST_ANIMATION_DLL tests/win32/test_animation_modules.c src/win32/animation.c $(MINGW_I686_BUILD_DIR)/libboxrig-ui.a $(MINGW_I686_BUILD_DIR)/libboxrig.a -luser32 -Wl,--out-implib,$(MINGW_I686_BUILD_DIR)/tests/animation_modules.dll.a -o $@

$(MINGW_I686_BUILD_DIR)/tests/test_animation_modules.exe: tests/win32/test_animation_modules.c src/win32/animation.c src/win32/animation.h $(MINGW_I686_BUILD_DIR)/libboxrig-ui.a $(MINGW_I686_BUILD_DIR)/libboxrig.a $(MINGW_I686_BUILD_DIR)/tests/animation_modules.dll
	$(MINGW_I686_CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(MINGW_WIN32_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) tests/win32/test_animation_modules.c src/win32/animation.c $(MINGW_I686_BUILD_DIR)/tests/animation_modules.dll.a $(MINGW_I686_BUILD_DIR)/libboxrig-ui.a $(MINGW_I686_BUILD_DIR)/libboxrig.a -luser32 -o $@

.PHONY: test-win32-animation-modules
test-native-blink: test-win32-animation-modules
test-win32-animation-modules: $(MINGW_AMD64_BUILD_DIR)/tests/test_animation_modules.exe $(MINGW_I686_BUILD_DIR)/tests/test_animation_modules.exe
	$(WINE_RUN) $(abspath $(MINGW_AMD64_BUILD_DIR)/tests/test_animation_modules.exe)
	$(WINE_RUN) $(abspath $(MINGW_I686_BUILD_DIR)/tests/test_animation_modules.exe)

test-p2-win32: $(MINGW_AMD64_BUILD_DIR)/tests/test_animation_modules.exe $(MINGW_I686_BUILD_DIR)/tests/test_animation_modules.exe

.PHONY: test-example-link-required
test-example-link-required: $(EXAMPLE_TEXT_POLICY_TEST_BINARY)
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $<

.PHONY: test-native-links
test: test-native-links
$(BUILD_DIR)/tests/link_cursor_probe: tests/x11/link_cursor_probe.c | $(BUILD_DIR)/tests
	$(CC) $(CFLAGS) $(PROJECT_CFLAGS) $< $(shell pkg-config --cflags --libs x11 xfixes xcursor) -o $@

$(MINGW_AMD64_BUILD_DIR)/tests/link_cursor_probe.exe: tests/win32/link_cursor_probe.c | $(MINGW_AMD64_BUILD_DIR)/tests
	$(MINGW_AMD64_CC) $(CFLAGS) $(PROJECT_CFLAGS) $(MINGW_WIN32_CPPFLAGS) $< -luser32 -o $@

test-p2-win32: $(MINGW_AMD64_BUILD_DIR)/tests/link_cursor_probe.exe

$(MINGW_I686_BUILD_DIR)/tests/link_cursor_probe.exe: tests/win32/link_cursor_probe.c | $(MINGW_I686_BUILD_DIR)/tests
	$(MINGW_I686_CC) $(CFLAGS) $(PROJECT_CFLAGS) $(MINGW_WIN32_CPPFLAGS) $< -luser32 -o $@

test-p2-win32: $(MINGW_I686_BUILD_DIR)/tests/link_cursor_probe.exe

test-native-links: $(EXAMPLE_BINARY) $(GTK3_EXAMPLE_BINARY) $(GTK4_EXAMPLE_BINARY) $(WAYLAND_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/tests/link_cursor_probe
	$(XVFB_RUN) -a -s '-screen 0 1920x1200x24' env BOXRIG_AUTO_URLS=1 XCURSOR_THEME=Adwaita XCURSOR_SIZE=24 node tests/run_native_link_policy.mjs $(EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/tests/link_cursor_probe $(BUILD_DIR)/test-results/step23-link-pointer/x11-enabled x11
	$(XVFB_RUN) -a -s '-screen 0 1920x1200x24' env BOXRIG_AUTO_URLS=1 XCURSOR_THEME=Adwaita XCURSOR_SIZE=24 LIBGL_ALWAYS_SOFTWARE=1 node tests/run_native_link_policy.mjs $(EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/tests/link_cursor_probe $(BUILD_DIR)/test-results/step23-link-pointer/x11-opengl-enabled opengl
	$(XVFB_RUN) -a -s '-screen 0 1920x1200x24' env BOXRIG_AUTO_URLS=1 XCURSOR_THEME=Adwaita XCURSOR_SIZE=24 BOXRIG_TEST_LINK_GTK=1 GDK_BACKEND=x11 node tests/run_native_link_policy.mjs $(GTK3_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/tests/link_cursor_probe $(BUILD_DIR)/test-results/step23-link-pointer/gtk3-x11-enabled
	$(XVFB_RUN) -a -s '-screen 0 1920x1200x24' env BOXRIG_AUTO_URLS=1 XCURSOR_THEME=Adwaita XCURSOR_SIZE=24 BOXRIG_TEST_LINK_GTK=1 GDK_BACKEND=x11 $(GTK4_TEST_IM_ENV) GSK_RENDERER=cairo node tests/run_native_link_policy.mjs $(GTK4_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/tests/link_cursor_probe $(BUILD_DIR)/test-results/step23-link-pointer/gtk4-x11-enabled
	$(XVFB_RUN) -a -s '-screen 0 1920x1200x24' env BOXRIG_AUTO_URLS=1 XCURSOR_THEME=Adwaita XCURSOR_SIZE=24 BOXRIG_TEST_LINK_GTK=1 GDK_BACKEND=wayland BOXRIG_TEST_LINK_WAYLAND=1 node tests/run_native_link_policy.mjs $(GTK3_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/tests/link_cursor_probe $(BUILD_DIR)/test-results/step23-link-pointer/gtk3-wayland-enabled
	$(XVFB_RUN) -a -s '-screen 0 1920x1200x24' env BOXRIG_AUTO_URLS=1 XCURSOR_THEME=Adwaita XCURSOR_SIZE=24 BOXRIG_TEST_LINK_GTK=1 GDK_BACKEND=wayland $(GTK4_TEST_IM_ENV) GSK_RENDERER=cairo BOXRIG_TEST_LINK_WAYLAND=1 node tests/run_native_link_policy.mjs $(GTK4_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/tests/link_cursor_probe $(BUILD_DIR)/test-results/step23-link-pointer/gtk4-wayland-enabled
	$(XVFB_RUN) -a -s '-screen 0 1920x1200x24' env BOXRIG_AUTO_URLS=1 XCURSOR_THEME=Adwaita XCURSOR_SIZE=24 BOXRIG_TEST_LINK_WAYLAND=1 node tests/run_native_link_policy.mjs $(WAYLAND_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/tests/link_cursor_probe $(BUILD_DIR)/test-results/step23-link-pointer/wayland-enabled wayland
	$(XVFB_RUN) -a -s '-screen 0 1920x1200x24' env BOXRIG_AUTO_URLS=1 XCURSOR_THEME=Adwaita XCURSOR_SIZE=24 BOXRIG_TEST_LINK_WAYLAND=1 LIBGL_ALWAYS_SOFTWARE=1 node tests/run_native_link_policy.mjs $(WAYLAND_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/tests/link_cursor_probe $(BUILD_DIR)/test-results/step23-link-pointer/wayland-opengl-enabled opengl
	$(XVFB_RUN) -a -s '-screen 0 1920x1200x24' env BOXRIG_AUTO_URLS=0 XCURSOR_THEME=Adwaita XCURSOR_SIZE=24 node tests/run_native_link_policy.mjs $(EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/tests/link_cursor_probe $(BUILD_DIR)/test-results/step23-link-pointer/x11-disabled x11
	$(XVFB_RUN) -a -s '-screen 0 1920x1200x24' env BOXRIG_AUTO_URLS=0 XCURSOR_THEME=Adwaita XCURSOR_SIZE=24 LIBGL_ALWAYS_SOFTWARE=1 node tests/run_native_link_policy.mjs $(EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/tests/link_cursor_probe $(BUILD_DIR)/test-results/step23-link-pointer/x11-opengl-disabled opengl
	$(XVFB_RUN) -a -s '-screen 0 1920x1200x24' env BOXRIG_AUTO_URLS=0 XCURSOR_THEME=Adwaita XCURSOR_SIZE=24 BOXRIG_TEST_LINK_GTK=1 GDK_BACKEND=x11 node tests/run_native_link_policy.mjs $(GTK3_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/tests/link_cursor_probe $(BUILD_DIR)/test-results/step23-link-pointer/gtk3-x11-disabled
	$(XVFB_RUN) -a -s '-screen 0 1920x1200x24' env BOXRIG_AUTO_URLS=0 XCURSOR_THEME=Adwaita XCURSOR_SIZE=24 BOXRIG_TEST_LINK_GTK=1 GDK_BACKEND=x11 $(GTK4_TEST_IM_ENV) GSK_RENDERER=cairo node tests/run_native_link_policy.mjs $(GTK4_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/tests/link_cursor_probe $(BUILD_DIR)/test-results/step23-link-pointer/gtk4-x11-disabled
	$(XVFB_RUN) -a -s '-screen 0 1920x1200x24' env BOXRIG_AUTO_URLS=0 XCURSOR_THEME=Adwaita XCURSOR_SIZE=24 BOXRIG_TEST_LINK_GTK=1 GDK_BACKEND=wayland BOXRIG_TEST_LINK_WAYLAND=1 node tests/run_native_link_policy.mjs $(GTK3_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/tests/link_cursor_probe $(BUILD_DIR)/test-results/step23-link-pointer/gtk3-wayland-disabled
	$(XVFB_RUN) -a -s '-screen 0 1920x1200x24' env BOXRIG_AUTO_URLS=0 XCURSOR_THEME=Adwaita XCURSOR_SIZE=24 BOXRIG_TEST_LINK_GTK=1 GDK_BACKEND=wayland $(GTK4_TEST_IM_ENV) GSK_RENDERER=cairo BOXRIG_TEST_LINK_WAYLAND=1 node tests/run_native_link_policy.mjs $(GTK4_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/tests/link_cursor_probe $(BUILD_DIR)/test-results/step23-link-pointer/gtk4-wayland-disabled
	$(XVFB_RUN) -a -s '-screen 0 1920x1200x24' env BOXRIG_AUTO_URLS=0 XCURSOR_THEME=Adwaita XCURSOR_SIZE=24 BOXRIG_TEST_LINK_WAYLAND=1 node tests/run_native_link_policy.mjs $(WAYLAND_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/tests/link_cursor_probe $(BUILD_DIR)/test-results/step23-link-pointer/wayland-disabled wayland
	$(XVFB_RUN) -a -s '-screen 0 1920x1200x24' env BOXRIG_AUTO_URLS=0 XCURSOR_THEME=Adwaita XCURSOR_SIZE=24 BOXRIG_TEST_LINK_WAYLAND=1 LIBGL_ALWAYS_SOFTWARE=1 node tests/run_native_link_policy.mjs $(WAYLAND_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/tests/link_cursor_probe $(BUILD_DIR)/test-results/step23-link-pointer/wayland-opengl-disabled opengl

.PHONY: test-sync-recovery-videos
test-sync-recovery-videos: $(BUILD_DIR)/tests/test_frontend_sample_x11 $(GTK3_EXAMPLE_BINARY) $(GTK4_EXAMPLE_BINARY) $(PTY_HELPER_BINARY)
	$(XVFB_RUN) -a env GDK_BACKEND=x11 node tests/run_sync_recovery_video.mjs $(BUILD_DIR)/tests/test_frontend_sample_x11 GTK3 $(GTK3_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/test-results/step23-blink/sync-gtk3
	$(XVFB_RUN) -a env $(GTK4_TEST_IM_ENV) GDK_BACKEND=x11 node tests/run_sync_recovery_video.mjs $(BUILD_DIR)/tests/test_frontend_sample_x11 GTK4 $(GTK4_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/test-results/step23-blink/sync-gtk4

.PHONY: test-continuous-output-videos
test: test-continuous-output-videos
test-continuous-output-videos: $(BUILD_DIR)/tests/test_frontend_sample_x11 $(GTK3_EXAMPLE_BINARY) $(GTK4_EXAMPLE_BINARY) $(PTY_HELPER_BINARY)
	$(XVFB_RUN) -a env GDK_BACKEND=x11 node tests/run_continuous_output_video.mjs $(BUILD_DIR)/tests/test_frontend_sample_x11 GTK3 $(GTK3_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/test-results/step23-i4-continuous-output/gtk3
	$(XVFB_RUN) -a env $(GTK4_TEST_IM_ENV) GDK_BACKEND=x11 node tests/run_continuous_output_video.mjs $(BUILD_DIR)/tests/test_frontend_sample_x11 GTK4 $(GTK4_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/test-results/step23-i4-continuous-output/gtk4

# Step 23 I3b: native link gestures and nonblocking external URI dispatch.
$(POSIX_URI_OBJECT): $(POSIX_URI_SOURCE) $(POSIX_URI_HEADERS) deps/cardio/include/cardio.h Makefile
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(POSIX_URI_CPPFLAGS) -Ideps/cardio/include -DCARDIO_WITH_GLIB=1 -DCARDIO_WITH_GIO=1 $(CFLAGS) -std=c++20 $(WARNINGS) -c $< -o $@

$(POSIX_URI_TEST_OBJECT): $(POSIX_URI_SOURCE) $(POSIX_URI_HEADERS) deps/cardio/include/cardio.h Makefile
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(POSIX_URI_CPPFLAGS) -Ideps/cardio/include -DCARDIO_WITH_GLIB=1 -DCARDIO_WITH_GIO=1 $(CFLAGS) -std=c++20 $(WARNINGS) $(SANITIZERS) -c $< -o $@

$(EXAMPLE_BINARY) $(GTK3_EXAMPLE_BINARY) $(GTK4_EXAMPLE_BINARY) $(WAYLAND_EXAMPLE_BINARY) $(WAYLAND_NO_LIBDECOR_TEST_BINARY): examples/boxrig_term/common/link_gesture.h $(POSIX_URI_HEADERS)
$(MINGW_AMD64_EXAMPLE) $(MINGW_I686_EXAMPLE): examples/boxrig_term/common/link_gesture.h examples/boxrig_term/win32/uri_launcher.h

$(BUILD_DIR)/tests/test_example_link_gesture: tests/test_example_link_gesture.c examples/boxrig_term/common/link_gesture.c examples/boxrig_term/common/link_gesture.h examples/boxrig_term/common/text_policy.c examples/boxrig_term/common/mouse_policy.c $(UI_LIBRARY) $(LIBRARY) Makefile | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) -Iexamples/boxrig_term/common $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) tests/test_example_link_gesture.c examples/boxrig_term/common/link_gesture.c examples/boxrig_term/common/text_policy.c examples/boxrig_term/common/mouse_policy.c $(UI_LIBRARY) $(LIBRARY) -lm -o $@

$(BUILD_DIR)/tests/test_uri_launcher.o: tests/test_uri_launcher.c $(POSIX_URI_HEADERS) Makefile | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(POSIX_URI_CPPFLAGS) -Iexamples/boxrig_term/posix $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) -c $< -o $@

$(BUILD_DIR)/tests/test_uri_launcher: $(BUILD_DIR)/tests/test_uri_launcher.o $(POSIX_URI_TEST_OBJECT)
	$(CXX) $(CFLAGS) $(SANITIZERS) $^ $(POSIX_URI_LIBS) -o $@

$(BUILD_DIR)/tests/uri_open_peer: tests/uri_open_peer.c | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $< -o $@

$(MINGW_AMD64_BUILD_DIR)/tests/test_uri_launcher.exe: tests/win32/test_uri_launcher.c examples/boxrig_term/win32/uri_launcher.c examples/boxrig_term/win32/uri_launcher.h Makefile | $(MINGW_AMD64_BUILD_DIR)/tests
	$(MINGW_AMD64_CC) $(CPPFLAGS) $(MINGW_TERM_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) -mnop-fun-dllimport tests/win32/test_uri_launcher.c examples/boxrig_term/win32/uri_launcher.c -lole32 -lshell32 -Wl,--wrap=CoInitializeEx -Wl,--wrap=CoUninitialize -Wl,--wrap=ShellExecuteExW -o $@

$(MINGW_I686_BUILD_DIR)/tests/test_uri_launcher.exe: tests/win32/test_uri_launcher.c examples/boxrig_term/win32/uri_launcher.c examples/boxrig_term/win32/uri_launcher.h Makefile | $(MINGW_I686_BUILD_DIR)/tests
	$(MINGW_I686_CC) $(CPPFLAGS) $(MINGW_TERM_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) -mnop-fun-dllimport tests/win32/test_uri_launcher.c examples/boxrig_term/win32/uri_launcher.c -lole32 -lshell32 -Wl,--wrap=CoInitializeEx@8 -Wl,--wrap=CoUninitialize@0 -Wl,--wrap=ShellExecuteExW@4 -o $@

$(BUILD_DIR)/tests/frontend_link_probes/win32/amd64/boxrig_term.exe: $(MINGW_AMD64_EXAMPLE) $(WIN32_EXAMPLE_SOURCES) tests/win32/link_open_probe.c Makefile
	@mkdir -p $(@D)
	$(MINGW_AMD64_CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(MINGW_TERM_CPPFLAGS) $(WIN32_TERM_RENDERER_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) -mnop-fun-dllimport $(WIN32_EXAMPLE_SOURCES) tests/win32/link_open_probe.c $(MINGW_AMD64_EXAMPLE_RESOURCE) $(WIN32_TERM_AMD64_RENDERER_LIBRARIES) $(MINGW_AMD64_WIN32_LIBRARY) $(MINGW_AMD64_SESSION_LIBRARY) $(MINGW_AMD64_UI_LIBRARY) $(MINGW_AMD64_LIBRARY) -municode -mwindows $(WIN32_RENDERER_LIBS) $(WIN32_HOST_LIBS) $(WIN32_TRANSPARENCY_LIBS) $(WIN32_TERM_RENDERER_LIBS) -ladvapi32 -lcomctl32 -Wl,--wrap=ShellExecuteExW -Wl,--wrap=DefSubclassProc -o $@

$(BUILD_DIR)/tests/frontend_link_probes/win32/i686/boxrig_term.exe: $(MINGW_I686_EXAMPLE) $(WIN32_EXAMPLE_SOURCES) tests/win32/link_open_probe.c Makefile
	@mkdir -p $(@D)
	$(MINGW_I686_CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(MINGW_TERM_CPPFLAGS) $(WIN32_TERM_RENDERER_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) -mnop-fun-dllimport $(WIN32_EXAMPLE_SOURCES) tests/win32/link_open_probe.c $(MINGW_I686_EXAMPLE_RESOURCE) $(WIN32_TERM_I686_RENDERER_LIBRARIES) $(MINGW_I686_WIN32_LIBRARY) $(MINGW_I686_SESSION_LIBRARY) $(MINGW_I686_UI_LIBRARY) $(MINGW_I686_LIBRARY) -municode -mwindows $(WIN32_RENDERER_LIBS) $(WIN32_HOST_LIBS) $(WIN32_TRANSPARENCY_LIBS) $(WIN32_TERM_RENDERER_LIBS) -ladvapi32 -lcomctl32 -Wl,--wrap=ShellExecuteExW@4 -Wl,--wrap=DefSubclassProc@16 -o $@

.PHONY: test-link-activation test-native-link-activation
test: test-link-activation test-native-link-activation
test-p2-win32: $(BUILD_DIR)/tests/frontend_link_probes/win32/amd64/boxrig_term.exe $(BUILD_DIR)/tests/frontend_link_probes/win32/i686/boxrig_term.exe

test-link-activation: $(BUILD_DIR)/tests/test_example_link_gesture $(BUILD_DIR)/tests/test_uri_launcher $(BUILD_DIR)/tests/uri_open_peer $(MINGW_AMD64_BUILD_DIR)/tests/test_uri_launcher.exe $(MINGW_I686_BUILD_DIR)/tests/test_uri_launcher.exe
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(BUILD_DIR)/tests/test_example_link_gesture
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(BUILD_DIR)/tests/test_uri_launcher $(abspath $(BUILD_DIR)/tests/uri_open_peer)
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(BUILD_DIR)/tests/test_uri_launcher $(abspath $(BUILD_DIR)/tests/uri_open_peer) --glib
	$(XVFB_RUN) -a env WINEPREFIX=$(WINE_PREFIX) WINEDEBUG=-all $(WINE) $(MINGW_AMD64_BUILD_DIR)/tests/test_uri_launcher.exe
	$(XVFB_RUN) -a env WINEPREFIX=$(WINE_PREFIX) WINEDEBUG=-all $(WINE) $(MINGW_I686_BUILD_DIR)/tests/test_uri_launcher.exe

test-native-link-activation: $(EXAMPLE_BINARY) $(GTK3_EXAMPLE_BINARY) $(GTK4_EXAMPLE_BINARY) $(WAYLAND_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/tests/uri_open_peer
	$(XVFB_RUN) -a -s '-screen 0 1920x1200x24' node tests/run_native_link_activation.mjs $(EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/tests/uri_open_peer $(BUILD_DIR)/test-results/step23-link-activation/x11 x11
	$(XVFB_RUN) -a -s '-screen 0 1920x1200x24' env LIBGL_ALWAYS_SOFTWARE=1 node tests/run_native_link_activation.mjs $(EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/tests/uri_open_peer $(BUILD_DIR)/test-results/step23-link-activation/x11-opengl opengl
	$(XVFB_RUN) -a -s '-screen 0 1920x1200x24' env BOXRIG_TEST_LINK_GTK=1 GDK_BACKEND=x11 node tests/run_native_link_activation.mjs $(GTK3_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/tests/uri_open_peer $(BUILD_DIR)/test-results/step23-link-activation/gtk3-x11
	$(XVFB_RUN) -a -s '-screen 0 1920x1200x24' env BOXRIG_TEST_LINK_GTK=1 GDK_BACKEND=x11 $(GTK4_TEST_IM_ENV) GSK_RENDERER=cairo node tests/run_native_link_activation.mjs $(GTK4_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/tests/uri_open_peer $(BUILD_DIR)/test-results/step23-link-activation/gtk4-x11
	$(XVFB_RUN) -a -s '-screen 0 1920x1200x24' env BOXRIG_TEST_LINK_GTK=1 GDK_BACKEND=wayland BOXRIG_TEST_LINK_WAYLAND=1 node tests/run_native_link_activation.mjs $(GTK3_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/tests/uri_open_peer $(BUILD_DIR)/test-results/step23-link-activation/gtk3-wayland
	$(XVFB_RUN) -a -s '-screen 0 1920x1200x24' env BOXRIG_TEST_LINK_GTK=1 GDK_BACKEND=wayland $(GTK4_TEST_IM_ENV) GSK_RENDERER=cairo BOXRIG_TEST_LINK_WAYLAND=1 node tests/run_native_link_activation.mjs $(GTK4_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/tests/uri_open_peer $(BUILD_DIR)/test-results/step23-link-activation/gtk4-wayland
	$(XVFB_RUN) -a -s '-screen 0 1920x1200x24' env BOXRIG_TEST_LINK_WAYLAND=1 node tests/run_native_link_activation.mjs $(WAYLAND_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/tests/uri_open_peer $(BUILD_DIR)/test-results/step23-link-activation/wayland wayland
	$(XVFB_RUN) -a -s '-screen 0 1920x1200x24' env BOXRIG_TEST_LINK_WAYLAND=1 LIBGL_ALWAYS_SOFTWARE=1 node tests/run_native_link_activation.mjs $(WAYLAND_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/tests/uri_open_peer $(BUILD_DIR)/test-results/step23-link-activation/wayland-opengl opengl

.PHONY: test-native-interaction-integration
test: test-native-interaction-integration

test-native-interaction-integration: $(EXAMPLE_BINARY) $(GTK3_EXAMPLE_BINARY) $(GTK4_EXAMPLE_BINARY) $(WAYLAND_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/tests/uri_open_peer $(GTK3_CLIPBOARD_PEER_BINARY) $(BUILD_DIR)/tests/link_cursor_probe
	$(XVFB_RUN) -a -s '-screen 0 2000x1300x24' node tests/run_native_interaction_integration.mjs $(EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/tests/uri_open_peer $(GTK3_CLIPBOARD_PEER_BINARY) $(BUILD_DIR)/tests/link_cursor_probe $(BUILD_DIR)/test-results/step23-interaction/x11 x11
	$(XVFB_RUN) -a -s '-screen 0 2000x1300x24' env LIBGL_ALWAYS_SOFTWARE=1 node tests/run_native_interaction_integration.mjs $(EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/tests/uri_open_peer $(GTK3_CLIPBOARD_PEER_BINARY) $(BUILD_DIR)/tests/link_cursor_probe $(BUILD_DIR)/test-results/step23-interaction/x11-opengl opengl
	$(XVFB_RUN) -a -s '-screen 0 2000x1300x24' env BOXRIG_TEST_LINK_GTK=1 GDK_BACKEND=x11 node tests/run_native_interaction_integration.mjs $(GTK3_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/tests/uri_open_peer $(GTK3_CLIPBOARD_PEER_BINARY) $(BUILD_DIR)/tests/link_cursor_probe $(BUILD_DIR)/test-results/step23-interaction/gtk3-x11
	$(XVFB_RUN) -a -s '-screen 0 2000x1300x24' env BOXRIG_TEST_LINK_GTK=1 GDK_BACKEND=x11 $(GTK4_TEST_IM_ENV) GSK_RENDERER=cairo node tests/run_native_interaction_integration.mjs $(GTK4_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/tests/uri_open_peer $(GTK3_CLIPBOARD_PEER_BINARY) $(BUILD_DIR)/tests/link_cursor_probe $(BUILD_DIR)/test-results/step23-interaction/gtk4-x11
	$(XVFB_RUN) -a -s '-screen 0 2000x1300x24' env BOXRIG_TEST_LINK_GTK=1 GDK_BACKEND=wayland BOXRIG_TEST_LINK_WAYLAND=1 node tests/run_native_interaction_integration.mjs $(GTK3_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/tests/uri_open_peer $(GTK3_CLIPBOARD_PEER_BINARY) $(BUILD_DIR)/tests/link_cursor_probe $(BUILD_DIR)/test-results/step23-interaction/gtk3-wayland
	$(XVFB_RUN) -a -s '-screen 0 2000x1300x24' env BOXRIG_TEST_LINK_GTK=1 GDK_BACKEND=wayland $(GTK4_TEST_IM_ENV) GSK_RENDERER=cairo BOXRIG_TEST_LINK_WAYLAND=1 node tests/run_native_interaction_integration.mjs $(GTK4_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/tests/uri_open_peer $(GTK3_CLIPBOARD_PEER_BINARY) $(BUILD_DIR)/tests/link_cursor_probe $(BUILD_DIR)/test-results/step23-interaction/gtk4-wayland
	$(XVFB_RUN) -a -s '-screen 0 2000x1300x24' env BOXRIG_TEST_LINK_WAYLAND=1 node tests/run_native_interaction_integration.mjs $(WAYLAND_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/tests/uri_open_peer $(GTK3_CLIPBOARD_PEER_BINARY) $(BUILD_DIR)/tests/link_cursor_probe $(BUILD_DIR)/test-results/step23-interaction/wayland wayland
	$(XVFB_RUN) -a -s '-screen 0 2000x1300x24' env BOXRIG_TEST_LINK_WAYLAND=1 LIBGL_ALWAYS_SOFTWARE=1 node tests/run_native_interaction_integration.mjs $(WAYLAND_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/tests/uri_open_peer $(GTK3_CLIPBOARD_PEER_BINARY) $(BUILD_DIR)/tests/link_cursor_probe $(BUILD_DIR)/test-results/step23-interaction/wayland-opengl opengl

.PHONY: test-selection-style
test-selection-style: $(BUILD_DIR)/tests/test_selection_style $(BUILD_DIR)/tests/test_selection_colors $(BUILD_DIR)/tests/test_decoration_demo
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(BUILD_DIR)/tests/test_selection_style
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(BUILD_DIR)/tests/test_selection_colors
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(BUILD_DIR)/tests/test_decoration_demo

$(BUILD_DIR)/tests/test_selection_style: tests/test_selection_style.c $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) $< $(UI_LIBRARY) $(LIBRARY) -lm -o $@

$(BUILD_DIR)/tests/test_selection_colors: tests/test_selection_colors.c src/ui/view_private.h src/ui/color_private.h $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) -Isrc/ui $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) $< $(UI_LIBRARY) $(LIBRARY) -lm -o $@

.PHONY: test-native-selection-style-videos
test-native-selection-style-videos: $(EXAMPLE_BINARY) $(GTK3_EXAMPLE_BINARY) $(GTK4_EXAMPLE_BINARY) $(WAYLAND_EXAMPLE_BINARY) $(PTY_HELPER_BINARY)
	$(XVFB_RUN) -a -s '-screen 0 1280x1024x24' node tests/run_native_selection_style_video.mjs $(EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/test-results/step23-selection-style/x11-software x11
	$(XVFB_RUN) -a -s '-screen 0 1280x1024x24' env LIBGL_ALWAYS_SOFTWARE=1 node tests/run_native_selection_style_video.mjs $(EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/test-results/step23-selection-style/x11-opengl opengl
	$(XVFB_RUN) -a -s '-screen 0 1280x1024x24' env GDK_BACKEND=x11 node tests/run_native_selection_style_video.mjs $(GTK3_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/test-results/step23-selection-style/gtk3-x11
	$(XVFB_RUN) -a -s '-screen 0 1280x1024x24' env $(GTK4_TEST_IM_ENV) GDK_BACKEND=x11 GSK_RENDERER=cairo node tests/run_native_selection_style_video.mjs $(GTK4_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/test-results/step23-selection-style/gtk4-x11
	$(XVFB_RUN) -a -s '-screen 0 1280x1024x24' env BOXRIG_TEST_BLINK_WAYLAND=1 GDK_BACKEND=wayland node tests/run_native_selection_style_video.mjs $(GTK3_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/test-results/step23-selection-style/gtk3-wayland
	$(XVFB_RUN) -a -s '-screen 0 1280x1024x24' env $(GTK4_TEST_IM_ENV) BOXRIG_TEST_BLINK_WAYLAND=1 GDK_BACKEND=wayland GSK_RENDERER=cairo node tests/run_native_selection_style_video.mjs $(GTK4_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/test-results/step23-selection-style/gtk4-wayland
	$(XVFB_RUN) -a -s '-screen 0 1280x1024x24' env BOXRIG_TEST_BLINK_WAYLAND=1 node tests/run_native_selection_style_video.mjs $(WAYLAND_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/test-results/step23-selection-style/native-wayland-software wayland
	$(XVFB_RUN) -a -s '-screen 0 1280x1024x24' env BOXRIG_TEST_BLINK_WAYLAND=1 LIBGL_ALWAYS_SOFTWARE=1 node tests/run_native_selection_style_video.mjs $(WAYLAND_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/test-results/step23-selection-style/native-wayland-opengl opengl

test: test-native-selection-style-videos

.PHONY: test-decoration-backends
test-decoration-backends: $(BUILD_DIR)/tests/test_decoration_backends
	$(XVFB_RUN) -a env LIBGL_ALWAYS_SOFTWARE=1 ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $<

.PHONY: test-opengl-build-dependencies
test-opengl-build-dependencies: $(BUILD_DIR)/tests/decoration_opengl.o
	@$(MAKE) --no-print-directory --question $<
	@for header in tests/opengl_host.h $(OPENGL_RENDERER_HEADERS); do \
		status=0; $(MAKE) --no-print-directory --question --assume-new=$$header $< || status=$$?; \
		if [ $$status -ne 1 ]; then echo "FAIL changed $$header must rebuild $<" >&2; exit 1; fi; \
	done

test: test-opengl-build-dependencies

$(BUILD_DIR)/tests/decoration_pango.o: tests/pangocairo/test_pangocairo_blink.c tests/selection_renderer_fixture.h tests/blink_renderer_fixture.h tests/color_renderer_fixture.h src/pangocairo/renderer.h $(UI_HEADERS) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) -Isrc -Itests $(POSIX_CPPFLAGS) $(PANGO_CAIRO_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) -Dmain=boxrig_test_pango_standalone -c $< -o $@

$(BUILD_DIR)/tests/decoration_opengl.o: tests/test_opengl_blink.c tests/opengl_host.h tests/selection_renderer_fixture.h tests/blink_renderer_fixture.h tests/color_renderer_fixture.h src/opengl/renderer.h src/opengl/runtime.h $(OPENGL_RENDERER_HEADERS) $(UI_HEADERS) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) -Isrc -Itests $(POSIX_CPPFLAGS) $(OPENGL_RENDERER_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) -Dmain=boxrig_test_opengl_standalone -c $< -o $@

$(BUILD_DIR)/tests/decoration_x11.o: tests/x11/test_x11_text_blink.c tests/selection_renderer_fixture.h tests/blink_renderer_fixture.h tests/color_renderer_fixture.h $(UI_HEADERS) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) -Isrc -Itests $(POSIX_CPPFLAGS) $(X11_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) -Dmain=boxrig_test_x11_standalone -c $< -o $@

$(BUILD_DIR)/tests/test_decoration_backends: tests/test_decoration_backends.c tests/decoration_integration_fixture.c tests/selection_renderer_fixture.c tests/blink_renderer_fixture.c tests/selection_renderer_fixture.h tests/opengl_host.c $(BUILD_DIR)/tests/decoration_pango.o $(BUILD_DIR)/tests/decoration_opengl.o $(BUILD_DIR)/tests/decoration_x11.o src/pangocairo/renderer.c $(PANGO_CANDIDATE_OBJECT) $(OPENGL_RENDERER_LIBRARY) $(X11_LIBRARY) $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) -Isrc -Itests $(POSIX_CPPFLAGS) $(PANGO_CAIRO_CPPFLAGS) $(OPENGL_RENDERER_CPPFLAGS) $(X11_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) tests/test_decoration_backends.c tests/decoration_integration_fixture.c tests/selection_renderer_fixture.c tests/blink_renderer_fixture.c tests/opengl_host.c src/pangocairo/renderer.c $(PANGO_CANDIDATE_OBJECT) $(BUILD_DIR)/tests/decoration_pango.o $(BUILD_DIR)/tests/decoration_opengl.o $(BUILD_DIR)/tests/decoration_x11.o $(OPENGL_RENDERER_LIBRARY) $(X11_LIBRARY) $(UI_LIBRARY) $(LIBRARY) $(OPENGL_RENDERER_LIBS) $(X11_LIBS) $(DBUS_LIBS) $(PANGO_CAIRO_LIBS) -lm -o $@

$(MINGW_AMD64_BUILD_DIR)/tests/decoration_gdi.o: tests/win32/test_win32_blink.c tests/selection_renderer_fixture.h tests/blink_renderer_fixture.h tests/color_renderer_fixture.h $(UI_HEADERS) | $(MINGW_AMD64_BUILD_DIR)/tests
	$(MINGW_AMD64_CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(MINGW_WIN32_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) -Dmain=boxrig_test_win32_standalone -c $< -o $@

$(MINGW_I686_BUILD_DIR)/tests/decoration_gdi.o: tests/win32/test_win32_blink.c tests/selection_renderer_fixture.h tests/blink_renderer_fixture.h tests/color_renderer_fixture.h $(UI_HEADERS) | $(MINGW_I686_BUILD_DIR)/tests
	$(MINGW_I686_CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(MINGW_WIN32_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) -Dmain=boxrig_test_win32_standalone -c $< -o $@


.PHONY: test-terminal-newline test-terminal-newline-win32 example-newline
test-terminal-newline: $(BUILD_DIR)/tests/test_terminal_newline $(BUILD_DIR)/examples/newline
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(BUILD_DIR)/tests/test_terminal_newline
	$(BUILD_DIR)/examples/newline

$(BUILD_DIR)/tests/test_terminal_newline: tests/test_terminal_newline.c $(LIBRARY) $(CORE_HEADERS) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) $< $(LIBRARY) -lm -o $@

example-newline: $(BUILD_DIR)/examples/newline
$(BUILD_DIR)/examples/newline: examples/core/newline.c $(LIBRARY) $(CORE_HEADERS)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $< $(LIBRARY) -lm -o $@

test-terminal-newline-win32: $(MINGW_AMD64_BUILD_DIR)/tests/test_terminal_newline.exe $(MINGW_AMD64_BUILD_DIR)/tests/newline_example.exe $(MINGW_I686_BUILD_DIR)/tests/test_terminal_newline.exe $(MINGW_I686_BUILD_DIR)/tests/newline_example.exe
	$(WINE_RUN) $(abspath $(MINGW_AMD64_BUILD_DIR)/tests/test_terminal_newline.exe)
	$(WINE_RUN) $(abspath $(MINGW_AMD64_BUILD_DIR)/tests/newline_example.exe)
	$(WINE_RUN) $(abspath $(MINGW_I686_BUILD_DIR)/tests/test_terminal_newline.exe)
	$(WINE_RUN) $(abspath $(MINGW_I686_BUILD_DIR)/tests/newline_example.exe)

test: test-terminal-newline-win32
test-p2-win32: $(MINGW_AMD64_BUILD_DIR)/tests/test_terminal_newline.exe $(MINGW_I686_BUILD_DIR)/tests/test_terminal_newline.exe

$(MINGW_AMD64_BUILD_DIR)/tests/test_terminal_newline.exe: tests/test_terminal_newline.c $(MINGW_AMD64_LIBRARY) $(CORE_HEADERS) | $(MINGW_AMD64_BUILD_DIR)/tests
	$(MINGW_AMD64_CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(MINGW_AMD64_CORE_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) -static-libgcc $< $(MINGW_AMD64_LIBRARY) -lm -o $@

$(MINGW_AMD64_BUILD_DIR)/tests/newline_example.exe: examples/core/newline.c $(MINGW_AMD64_LIBRARY) $(CORE_HEADERS) | $(MINGW_AMD64_BUILD_DIR)/tests
	$(MINGW_AMD64_CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(MINGW_AMD64_CORE_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) -static-libgcc $< $(MINGW_AMD64_LIBRARY) -lm -o $@

$(MINGW_I686_BUILD_DIR)/tests/test_terminal_newline.exe: tests/test_terminal_newline.c $(MINGW_I686_LIBRARY) $(CORE_HEADERS) | $(MINGW_I686_BUILD_DIR)/tests
	$(MINGW_I686_CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(MINGW_I686_CORE_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) -static-libgcc $< $(MINGW_I686_LIBRARY) -lm -o $@

$(MINGW_I686_BUILD_DIR)/tests/newline_example.exe: examples/core/newline.c $(MINGW_I686_LIBRARY) $(CORE_HEADERS) | $(MINGW_I686_BUILD_DIR)/tests
	$(MINGW_I686_CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(MINGW_I686_CORE_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) -static-libgcc $< $(MINGW_I686_LIBRARY) -lm -o $@

.PHONY: test-terminal-key-mapping test-terminal-key-mapping-win32 example-key-mapping
test-terminal-key-mapping: $(BUILD_DIR)/tests/test_terminal_key_mapping $(BUILD_DIR)/examples/key_mapping
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(BUILD_DIR)/tests/test_terminal_key_mapping
	$(BUILD_DIR)/examples/key_mapping

$(BUILD_DIR)/tests/test_terminal_key_mapping: tests/test_terminal_key_mapping.c $(LIBRARY) $(CORE_HEADERS) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) $< $(LIBRARY) -lm -o $@

example-key-mapping: $(BUILD_DIR)/examples/key_mapping
$(BUILD_DIR)/examples/key_mapping: examples/core/key_mapping.c $(LIBRARY) $(CORE_HEADERS)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $< $(LIBRARY) -lm -o $@

test-terminal-key-mapping-win32: $(MINGW_AMD64_BUILD_DIR)/tests/test_terminal_key_mapping.exe $(MINGW_AMD64_BUILD_DIR)/tests/key_mapping_example.exe $(MINGW_I686_BUILD_DIR)/tests/test_terminal_key_mapping.exe $(MINGW_I686_BUILD_DIR)/tests/key_mapping_example.exe
	$(WINE_RUN) $(abspath $(MINGW_AMD64_BUILD_DIR)/tests/test_terminal_key_mapping.exe)
	$(WINE_RUN) $(abspath $(MINGW_AMD64_BUILD_DIR)/tests/key_mapping_example.exe)
	$(WINE_RUN) $(abspath $(MINGW_I686_BUILD_DIR)/tests/test_terminal_key_mapping.exe)
	$(WINE_RUN) $(abspath $(MINGW_I686_BUILD_DIR)/tests/key_mapping_example.exe)

test: test-terminal-key-mapping-win32
test-p2-win32: $(MINGW_AMD64_BUILD_DIR)/tests/test_terminal_key_mapping.exe $(MINGW_I686_BUILD_DIR)/tests/test_terminal_key_mapping.exe

$(MINGW_AMD64_BUILD_DIR)/tests/test_terminal_key_mapping.exe: tests/test_terminal_key_mapping.c $(MINGW_AMD64_LIBRARY) $(CORE_HEADERS) | $(MINGW_AMD64_BUILD_DIR)/tests
	$(MINGW_AMD64_CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(MINGW_AMD64_CORE_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) -static-libgcc $< $(MINGW_AMD64_LIBRARY) -lm -o $@

$(MINGW_AMD64_BUILD_DIR)/tests/key_mapping_example.exe: examples/core/key_mapping.c $(MINGW_AMD64_LIBRARY) $(CORE_HEADERS) | $(MINGW_AMD64_BUILD_DIR)/tests
	$(MINGW_AMD64_CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(MINGW_AMD64_CORE_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) -static-libgcc $< $(MINGW_AMD64_LIBRARY) -lm -o $@

$(MINGW_I686_BUILD_DIR)/tests/test_terminal_key_mapping.exe: tests/test_terminal_key_mapping.c $(MINGW_I686_LIBRARY) $(CORE_HEADERS) | $(MINGW_I686_BUILD_DIR)/tests
	$(MINGW_I686_CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(MINGW_I686_CORE_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) -static-libgcc $< $(MINGW_I686_LIBRARY) -lm -o $@

$(MINGW_I686_BUILD_DIR)/tests/key_mapping_example.exe: examples/core/key_mapping.c $(MINGW_I686_LIBRARY) $(CORE_HEADERS) | $(MINGW_I686_BUILD_DIR)/tests
	$(MINGW_I686_CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(MINGW_I686_CORE_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) -static-libgcc $< $(MINGW_I686_LIBRARY) -lm -o $@

.PHONY: test-terminal-codec test-terminal-codec-frontends test-terminal-codec-win32 example-encoding generate-shift-jis install-common-licenses
test-terminal-codec: $(BUILD_DIR)/tests/test_terminal_codec $(BUILD_DIR)/tests/test_codec_decoder $(BUILD_DIR)/tests/test_session_codec $(BUILD_DIR)/tests/test_input_text_chunk $(BUILD_DIR)/examples/encoding
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(BUILD_DIR)/tests/test_terminal_codec
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(BUILD_DIR)/tests/test_codec_decoder
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(BUILD_DIR)/tests/test_session_codec
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(BUILD_DIR)/tests/test_input_text_chunk
	$(BUILD_DIR)/examples/encoding

$(BUILD_DIR)/tests/test_terminal_codec: tests/test_terminal_codec.c $(LIBRARY) $(CORE_HEADERS) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) $< $(LIBRARY) -lm -o $@

$(BUILD_DIR)/tests/test_codec_decoder: tests/test_codec_decoder.c $(LIBRARY) $(CORE_HEADERS) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) $< $(LIBRARY) -lm -o $@

$(BUILD_DIR)/tests/test_session_codec: tests/test_session_codec.c $(SESSION_LIBRARY) $(LIBRARY) $(CORE_HEADERS) include/boxrig/session.h | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) $< $(SESSION_LIBRARY) $(LIBRARY) -lm -o $@

$(BUILD_DIR)/tests/test_input_text_chunk: tests/test_input_text_chunk.c $(UI_LIBRARY) include/boxrig/ui/input_text.h | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) $< $(UI_LIBRARY) -o $@

example-encoding: $(BUILD_DIR)/examples/encoding
$(BUILD_DIR)/examples/encoding: examples/core/encoding.c $(LIBRARY) $(CORE_HEADERS)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $< $(LIBRARY) -lm -o $@

test-terminal-codec-frontends: $(BUILD_DIR)/tests/test_gtk3_codec $(BUILD_DIR)/tests/test_gtk4_codec
	xvfb-run -a env GDK_BACKEND=x11 GSK_RENDERER=cairo NO_AT_BRIDGE=1 ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(BUILD_DIR)/tests/test_gtk3_codec
	xvfb-run -a env GDK_BACKEND=x11 GSK_RENDERER=cairo NO_AT_BRIDGE=1 ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(BUILD_DIR)/tests/test_gtk4_codec

$(BUILD_DIR)/tests/test_gtk3_codec: tests/gtk/test_widget_codec.c $(GTK3_LIBRARY) $(UI_LIBRARY) $(LIBRARY) $(GTK3_HEADERS) $(CORE_HEADERS) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(POSIX_CPPFLAGS) $(GTK3_CPPFLAGS) $(CFLAGS) $(GTK3_PROJECT_CFLAGS) $(SANITIZERS) -DBOXRIG_TEST_GTK3 -Wl,--wrap=gtk_im_multicontext_new,--wrap=gtk_im_context_filter_keypress $< $(GTK3_LIBRARY) $(UI_LIBRARY) $(LIBRARY) $(GTK3_LIBS) -lm -o $@

$(BUILD_DIR)/tests/test_gtk4_codec: tests/gtk/test_widget_codec.c $(GTK4_LIBRARY) $(UI_LIBRARY) $(LIBRARY) $(GTK4_HEADERS) $(CORE_HEADERS) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(POSIX_CPPFLAGS) $(GTK4_CPPFLAGS) $(CFLAGS) $(GTK4_PROJECT_CFLAGS) $(SANITIZERS) -Wl,--wrap=gtk_im_multicontext_new,--wrap=gtk_event_controller_key_new,--wrap=gtk_event_controller_focus_new $< $(GTK4_LIBRARY) $(UI_LIBRARY) $(LIBRARY) $(GTK4_LIBS) -lm -o $@

$(MINGW_AMD64_BUILD_DIR)/tests/test_terminal_codec.exe: tests/test_terminal_codec.c $(MINGW_AMD64_LIBRARY) $(CORE_HEADERS) | $(MINGW_AMD64_BUILD_DIR)/tests
	$(MINGW_AMD64_CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(MINGW_AMD64_CORE_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) -static-libgcc $< $(MINGW_AMD64_LIBRARY) -lm -o $@

$(MINGW_AMD64_BUILD_DIR)/tests/test_session_codec.exe: tests/test_session_codec.c $(MINGW_AMD64_SESSION_LIBRARY) $(MINGW_AMD64_LIBRARY) $(CORE_HEADERS) include/boxrig/session.h | $(MINGW_AMD64_BUILD_DIR)/tests
	$(MINGW_AMD64_CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(MINGW_AMD64_CORE_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) -static-libgcc $< $(MINGW_AMD64_SESSION_LIBRARY) $(MINGW_AMD64_LIBRARY) -lm -o $@

$(MINGW_AMD64_BUILD_DIR)/tests/encoding_example.exe: examples/core/encoding.c $(MINGW_AMD64_LIBRARY) $(CORE_HEADERS) | $(MINGW_AMD64_BUILD_DIR)/tests
	$(MINGW_AMD64_CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(MINGW_AMD64_CORE_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) -static-libgcc $< $(MINGW_AMD64_LIBRARY) -lm -o $@

$(MINGW_I686_BUILD_DIR)/tests/test_terminal_codec.exe: tests/test_terminal_codec.c $(MINGW_I686_LIBRARY) $(CORE_HEADERS) | $(MINGW_I686_BUILD_DIR)/tests
	$(MINGW_I686_CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(MINGW_I686_CORE_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) -static-libgcc $< $(MINGW_I686_LIBRARY) -lm -o $@

$(MINGW_I686_BUILD_DIR)/tests/test_session_codec.exe: tests/test_session_codec.c $(MINGW_I686_SESSION_LIBRARY) $(MINGW_I686_LIBRARY) $(CORE_HEADERS) include/boxrig/session.h | $(MINGW_I686_BUILD_DIR)/tests
	$(MINGW_I686_CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(MINGW_I686_CORE_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) -static-libgcc $< $(MINGW_I686_SESSION_LIBRARY) $(MINGW_I686_LIBRARY) -lm -o $@

$(MINGW_I686_BUILD_DIR)/tests/encoding_example.exe: examples/core/encoding.c $(MINGW_I686_LIBRARY) $(CORE_HEADERS) | $(MINGW_I686_BUILD_DIR)/tests
	$(MINGW_I686_CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(MINGW_I686_CORE_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) -static-libgcc $< $(MINGW_I686_LIBRARY) -lm -o $@

test-terminal-codec-win32: $(MINGW_AMD64_BUILD_DIR)/tests/test_terminal_codec.exe $(MINGW_AMD64_BUILD_DIR)/tests/test_session_codec.exe $(MINGW_AMD64_BUILD_DIR)/tests/encoding_example.exe $(MINGW_I686_BUILD_DIR)/tests/test_terminal_codec.exe $(MINGW_I686_BUILD_DIR)/tests/test_session_codec.exe $(MINGW_I686_BUILD_DIR)/tests/encoding_example.exe
	$(WINE_RUN) $(abspath $(MINGW_AMD64_BUILD_DIR)/tests/test_terminal_codec.exe)
	$(WINE_RUN) $(abspath $(MINGW_AMD64_BUILD_DIR)/tests/test_session_codec.exe)
	$(WINE_RUN) $(abspath $(MINGW_AMD64_BUILD_DIR)/tests/encoding_example.exe)
	$(WINE_RUN) $(abspath $(MINGW_I686_BUILD_DIR)/tests/test_terminal_codec.exe)
	$(WINE_RUN) $(abspath $(MINGW_I686_BUILD_DIR)/tests/test_session_codec.exe)
	$(WINE_RUN) $(abspath $(MINGW_I686_BUILD_DIR)/tests/encoding_example.exe)

test: test-terminal-codec-frontends test-terminal-codec-win32
test-p2-win32: $(MINGW_AMD64_BUILD_DIR)/tests/test_terminal_codec.exe $(MINGW_AMD64_BUILD_DIR)/tests/test_session_codec.exe $(MINGW_AMD64_BUILD_DIR)/tests/encoding_example.exe $(MINGW_I686_BUILD_DIR)/tests/test_terminal_codec.exe $(MINGW_I686_BUILD_DIR)/tests/test_session_codec.exe $(MINGW_I686_BUILD_DIR)/tests/encoding_example.exe

.PHONY: test-codegen
test: test-codegen
test-codegen: codegen-tools
	npm --prefix tools/codegen test

generate-shift-jis: codegen-tools
	node $(CODEGEN_DIR)/generate-shift-jis.mjs .

install-gtk3 install-gtk4 install-wayland install-render-ogl install-x11-opengl install-wayland-opengl install-core-win32-amd64 install-win32-amd64 install-win32-opengl-amd64 install-render-d3d11-amd64 install-win32-d3d11-amd64 install-core-win32-i686 install-win32-i686 install-win32-opengl-i686 install-render-d3d11-i686 install-win32-d3d11-i686: install-common-licenses

.PHONY: test-scrollbar-sdk
test-scrollbar-sdk:
	sh tests/check_scrollbar_sdk.sh "$(CC)" "$(MINGW_AMD64_CC)" "$(MINGW_I686_CC)"
	$(WINE_RUN) $(abspath $(BUILD_DIR)/tests/scrollbar-sdk/win32-amd64/consumer.exe)
	$(WINE_RUN) $(abspath $(BUILD_DIR)/tests/scrollbar-sdk/win32-i686/consumer.exe)
	$(WINE_RUN) $(abspath $(BUILD_DIR)/tests/scrollbar-sdk/win32-amd64/selection_history.exe)
	$(WINE_RUN) $(abspath $(BUILD_DIR)/tests/scrollbar-sdk/win32-amd64/selection_layout_provider.exe)
	$(WINE_RUN) $(abspath $(BUILD_DIR)/tests/scrollbar-sdk/win32-i686/selection_history.exe)
	$(WINE_RUN) $(abspath $(BUILD_DIR)/tests/scrollbar-sdk/win32-i686/selection_layout_provider.exe)

test: test-scrollbar-sdk

.PHONY: test-scrollbar-gtk-scale
test: test-scrollbar-gtk-scale
test-scrollbar-gtk-scale: $(GTK3_WIDGET_TEST_BINARY) $(GTK4_WIDGET_TEST_BINARY)
	$(XVFB_RUN) -a env GDK_BACKEND=x11 GDK_SCALE=2 NO_AT_BRIDGE=1 BOXRIG_TEST_SCROLLBAR_ONLY=1 ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(GTK3_WIDGET_TEST_BINARY) "$(TEST_FONT_FILE)"
	$(XVFB_RUN) -a env $(GTK4_TEST_IM_ENV) GDK_BACKEND=x11 GDK_SCALE=2 GSK_RENDERER=cairo NO_AT_BRIDGE=1 BOXRIG_TEST_SCROLLBAR_ONLY=1 ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(GTK4_WIDGET_TEST_BINARY) "$(TEST_FONT_FILE)"


.PHONY: test-terminal-io-integration
test: test-terminal-io-integration

test-terminal-io-integration: $(BUILD_DIR)/tests/test_terminal_io_work $(BUILD_DIR)/tests/test_terminal_io_views
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(BUILD_DIR)/tests/test_terminal_io_work
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(BUILD_DIR)/tests/test_terminal_io_views

$(BUILD_DIR)/tests/test_terminal_io_work: tests/test_terminal_io_work.c $(LIBRARY) $(CORE_HEADERS) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) $< $(LIBRARY) -lm -Wl,--wrap=boxrig_unicode_encode -o $@

$(BUILD_DIR)/tests/test_terminal_io_views: tests/test_terminal_io_views.c $(UI_LIBRARY) $(LIBRARY) $(UI_HEADERS) $(CORE_HEADERS) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) $< $(UI_LIBRARY) $(LIBRARY) -lm -o $@


test-p2-win32: $(MINGW_AMD64_BUILD_DIR)/tests/test_terminal_io_views.exe

$(MINGW_AMD64_BUILD_DIR)/tests/test_terminal_io_views.exe: tests/test_terminal_io_views.c $(MINGW_AMD64_UI_LIBRARY) $(MINGW_AMD64_LIBRARY) $(CORE_HEADERS) $(UI_HEADERS) | $(MINGW_AMD64_BUILD_DIR)/tests
	$(MINGW_AMD64_CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(MINGW_AMD64_CORE_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) -static-libgcc $< $(MINGW_AMD64_UI_LIBRARY) $(MINGW_AMD64_LIBRARY) -lm -o $@

test-p2-win32: $(MINGW_I686_BUILD_DIR)/tests/test_terminal_io_views.exe

# GTK-only GLArea drivers share the borrowed-context adapter across GTK versions.
.PHONY: gtk3-gl-driver gtk4-gl-driver test-gtk-gl-driver
gtk3-gl-driver: $(BUILD_DIR)/tests/gtk3_gl_driver
gtk4-gl-driver: $(BUILD_DIR)/tests/gtk4_gl_driver

$(BUILD_DIR)/tests/gtk3_gl_driver: tests/gtk/test_gl_area.c src/gtk/gl_area.c \
		src/gtk/gl_area.h include/boxrig/gtk/rendering.h $(GTK3_LIBRARY) \
		$(OPENGL_RENDERER_LIBRARY) $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) -Iinclude -Isrc $(GTK3_CPPFLAGS) $$(pkg-config --cflags epoxy) \
		$(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) $< src/gtk/gl_area.c \
		$(GTK3_LIBRARY) $(OPENGL_RENDERER_LIBRARY) $(UI_LIBRARY) $(LIBRARY) \
		$(GTK3_LIBS) $$(pkg-config --libs epoxy) -Wl,--wrap=gtk_gl_area_new -lm -o $@

$(BUILD_DIR)/tests/gtk4_gl_driver: tests/gtk/test_gl_area.c src/gtk/gl_area.c \
		src/gtk/gl_area.h include/boxrig/gtk/rendering.h $(GTK4_LIBRARY) \
		$(OPENGL_RENDERER_LIBRARY) $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) -Iinclude -Isrc $(GTK4_CPPFLAGS) $$(pkg-config --cflags epoxy) \
		$(CFLAGS) $(GTK4_PROJECT_CFLAGS) $(SANITIZERS) $< src/gtk/gl_area.c \
		$(GTK4_LIBRARY) $(OPENGL_RENDERER_LIBRARY) $(UI_LIBRARY) $(LIBRARY) \
		$(GTK4_LIBS) $$(pkg-config --libs epoxy) -Wl,--wrap=gtk_gl_area_new -lm -o $@

test-gtk-gl-driver: gtk3-gl-driver gtk4-gl-driver
	$(XVFB_RUN) -a env GDK_BACKEND=x11 GSK_RENDERER=cairo NO_AT_BRIDGE=1 \
		ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
		bash tests/gtk/run_gl_driver.sh $(BUILD_DIR)
	bash tests/gtk/run_gl_wayland.sh bash tests/gtk/run_gl_driver.sh $(BUILD_DIR)

test: test-gtk-gl-driver

.PHONY: test-gtk-gl-surface
$(BUILD_DIR)/tests/gtk3_gl_surface: tests/gtk/test_gl_surface.c src/gtk/gl_surface.c \
		src/gtk/gl_surface.h src/gtk/gl_area.c src/gtk/gl_area.h include/boxrig/gtk/rendering.h \
		$(OPENGL_RENDERER_LIBRARY) $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) -Iinclude -Isrc $(GTK3_CPPFLAGS) $$(pkg-config --cflags epoxy) \
		$(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) $< src/gtk/gl_area.c src/gtk/gl_surface.c \
		$(OPENGL_RENDERER_LIBRARY) $(UI_LIBRARY) $(LIBRARY) \
		$(GTK3_LIBS) $$(pkg-config --libs epoxy) -lm \
		-Wl,--wrap=boxrig_renderer_lifecycle_commit_target -o $@

test-gtk-gl-surface: $(BUILD_DIR)/tests/gtk3_gl_surface
	$(XVFB_RUN) -a env GDK_BACKEND=x11 ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 \
		UBSAN_OPTIONS=halt_on_error=1 $(BUILD_DIR)/tests/gtk3_gl_surface

test: test-gtk-gl-surface

$(MINGW_I686_BUILD_DIR)/tests/test_terminal_io_views.exe: tests/test_terminal_io_views.c $(MINGW_I686_UI_LIBRARY) $(MINGW_I686_LIBRARY) $(CORE_HEADERS) $(UI_HEADERS) | $(MINGW_I686_BUILD_DIR)/tests
	$(MINGW_I686_CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(MINGW_I686_CORE_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) -static-libgcc $< $(MINGW_I686_UI_LIBRARY) $(MINGW_I686_LIBRARY) -lm -o $@

.PHONY: test-gtk3-rendering
$(BUILD_DIR)/tests/test_gtk3_rendering: tests/gtk3/test_gtk3_rendering.c $(GTK3_HEADERS) $(GTK3_LIBRARY) $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) -Iinclude $(GTK3_CPPFLAGS) $(CFLAGS) $(GTK3_PROJECT_CFLAGS) $(SANITIZERS) $< \
		-Wl,--wrap=gtk_gl_area_new -Wl,--wrap=gdk_gl_context_set_use_es -Wl,--wrap=gdk_gl_context_realize \
		-Wl,--wrap=boxrig_gtk_gl_surface_create -Wl,--wrap=boxrig_gtk_gl_surface_draw \
		$(GTK3_LIBRARY) $(UI_LIBRARY) $(LIBRARY) $(GTK3_LIBS) -o $@

test-gtk3-rendering: $(BUILD_DIR)/tests/test_gtk3_rendering
	$(XVFB_RUN) -a env GDK_BACKEND=x11 ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 \
		UBSAN_OPTIONS=halt_on_error=1 bash -c 'for policy in cairo prefer require; do for fault in none context; do $(BUILD_DIR)/tests/test_gtk3_rendering $$policy $$fault || exit; done; done'
	$(XVFB_RUN) -a env GDK_BACKEND=x11 GDK_GL=gles LIBGL_ALWAYS_SOFTWARE=1 \
		ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
		$(BUILD_DIR)/tests/test_gtk3_rendering require toggle
	$(XVFB_RUN) -a env GDK_BACKEND=x11 GDK_GL=gles LIBGL_ALWAYS_SOFTWARE=1 \
		ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
		$(BUILD_DIR)/tests/test_gtk3_rendering prefer toggle

test: test-gtk3-rendering

.PHONY: test-gtk3-gles
test-gtk3-gles: $(BUILD_DIR)/tests/test_gtk3_rendering $(GTK3_WIDGET_TEST_BINARY) $(GTK3_EXAMPLE_BINARY) $(FRONTEND_SAMPLE_X11_TEST_BINARY) $(PTY_HELPER_BINARY)
	$(XVFB_RUN) -a env GDK_BACKEND=x11 GDK_GL=gles LIBGL_ALWAYS_SOFTWARE=1 BOXRIG_TEST_GL_API=gles ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(BUILD_DIR)/tests/test_gtk3_rendering require none
	$(XVFB_RUN) -a env GDK_BACKEND=x11 GDK_GL=gles LIBGL_ALWAYS_SOFTWARE=1 GDK_SCALE=2 BOXRIG_TEST_GL_API=gles ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(BUILD_DIR)/tests/test_gtk3_rendering require none
	$(XVFB_RUN) -a env GDK_BACKEND=x11 GDK_GL=gles LIBGL_ALWAYS_SOFTWARE=1 MESA_GLES_VERSION_OVERRIDE=3.0 MESA_EXTENSION_OVERRIDE=-GL_EXT_sRGB_write_control BOXRIG_TEST_GL_API=gles ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(BUILD_DIR)/tests/test_gtk3_rendering require none
	$(XVFB_RUN) -a env GDK_BACKEND=x11 GDK_GL= LIBGL_ALWAYS_SOFTWARE=1 BOXRIG_TEST_GL_API=gl ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(BUILD_DIR)/tests/test_gtk3_rendering require es-context
	$(XVFB_RUN) -a env GDK_BACKEND=x11 GDK_GL=gles LIBGL_ALWAYS_SOFTWARE=1 ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(BUILD_DIR)/tests/test_gtk3_rendering prefer es-renderer
	$(XVFB_RUN) -a env GDK_BACKEND=x11 GDK_GL=gles LIBGL_ALWAYS_SOFTWARE=1 BOXRIG_TEST_RENDER_OPENGL=1 ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(GTK3_WIDGET_TEST_BINARY) "$(TEST_FONT_FILE)"
	$(XVFB_RUN) -a env GDK_GL=gles LIBGL_ALWAYS_SOFTWARE=1 BOXRIG_TEST_GL_API=gles BOXRIG_TEST_WESTON_BACKEND=x11 bash tests/gtk/run_gl_wayland.sh $(BUILD_DIR)/tests/test_gtk3_rendering require none
	$(XVFB_RUN) -a env GDK_GL= LIBGL_ALWAYS_SOFTWARE=1 BOXRIG_TEST_GL_API=gl BOXRIG_TEST_WESTON_BACKEND=x11 bash tests/gtk/run_gl_wayland.sh $(BUILD_DIR)/tests/test_gtk3_rendering require es-context
	$(XVFB_RUN) -a env GDK_GL=gles LIBGL_ALWAYS_SOFTWARE=1 BOXRIG_TEST_WESTON_BACKEND=x11 bash tests/gtk/run_gl_wayland.sh $(BUILD_DIR)/tests/test_gtk3_rendering prefer es-renderer
	$(XVFB_RUN) -a env GDK_BACKEND=x11 GDK_GL=gles LIBGL_ALWAYS_SOFTWARE=1 BOXRIG_TEST_SAMPLE_RENDERER=--renderer=opengl BOXRIG_TEST_SAMPLE_GRAPHICS_API='OpenGL ES' ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(FRONTEND_SAMPLE_X11_TEST_BINARY) GTK3 $(GTK3_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) --color-demo

test: test-gtk3-gles

.PHONY: test-gtk3-opengl test-gtk3-opengl-videos

test-gtk3-opengl: $(GTK3_WIDGET_TEST_BINARY) $(BUILD_DIR)/tests/test_gtk3_rendering $(GTK3_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(FRONTEND_SAMPLE_X11_TEST_BINARY) $(WAYLAND_X11_PIXEL_PROBE_BINARY)
	$(XVFB_RUN) -a env GDK_BACKEND=x11 NO_AT_BRIDGE=1 BOXRIG_TEST_RENDER_OPENGL=1 ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(GTK3_WIDGET_TEST_BINARY) "$(TEST_FONT_FILE)"
	$(XVFB_RUN) -a env GDK_BACKEND=x11 GDK_SCALE=2 NO_AT_BRIDGE=1 BOXRIG_TEST_RENDER_OPENGL=1 ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(GTK3_WIDGET_TEST_BINARY) "$(TEST_FONT_FILE)"
	$(XVFB_RUN) -a env BOXRIG_TEST_WESTON_BACKEND=x11 bash tests/gtk/run_gl_wayland.sh env BOXRIG_TEST_RENDER_OPENGL=1 $(GTK3_WIDGET_TEST_BINARY) "$(TEST_FONT_FILE)"
	$(XVFB_RUN) -a env BOXRIG_TEST_WESTON_BACKEND=x11 bash tests/gtk/run_gl_wayland.sh $(BUILD_DIR)/tests/test_gtk3_rendering require none
	$(XVFB_RUN) -a env GDK_BACKEND=x11 BOXRIG_TEST_SAMPLE_RENDERER=--renderer=opengl ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(FRONTEND_SAMPLE_X11_TEST_BINARY) GTK3 $(GTK3_EXAMPLE_BINARY) $(PTY_HELPER_BINARY)
	$(XVFB_RUN) -a env BOXRIG_TEST_SAMPLE_RENDERER=opengl BOXRIG_TEST_WESTON_RENDERER=gl $(WAYLAND_FRONTEND_SAMPLE_TEST_RUNNER) GTK3 "$(WESTON)" "$(GTK3_EXAMPLE_BINARY)" "$(PTY_HELPER_BINARY)" "$(WAYLAND_X11_PIXEL_PROBE_BINARY)"
	BOXRIG_GTK_RENDERER=opengl $(MAKE) test-gtk3-gestament

test-gtk3-opengl-videos: $(GTK3_EXAMPLE_BINARY) $(PTY_HELPER_BINARY)
	$(XVFB_RUN) -a -s '-screen 0 1280x1024x24' env GDK_BACKEND=x11 node tests/run_native_cursor_video.mjs $(GTK3_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/test-results/step24/gtk3-x11 opengl
	$(XVFB_RUN) -a -s '-screen 0 1280x1024x24' env GDK_BACKEND=wayland BOXRIG_TEST_BLINK_WAYLAND=1 BOXRIG_TEST_WESTON_RENDERER=gl node tests/run_native_cursor_video.mjs $(GTK3_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/test-results/step24/gtk3-wayland opengl

test: test-gtk3-opengl test-gtk3-opengl-videos

.PHONY: test-gtk4-rendering
$(BUILD_DIR)/tests/test_gtk4_rendering: tests/gtk4/test_gtk4_rendering.c $(GTK4_HEADERS) $(GTK4_LIBRARY) $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) -Iinclude $(GTK4_CPPFLAGS) $(CFLAGS) $(GTK4_PROJECT_CFLAGS) $(SANITIZERS) $< \
		-Wl,--wrap=gtk_gl_area_new -Wl,--wrap=boxrig_gtk_gl_surface_draw \
		$(GTK4_LIBRARY) $(UI_LIBRARY) $(LIBRARY) $(GTK4_LIBS) -o $@

test-gtk4-rendering: $(BUILD_DIR)/tests/test_gtk4_rendering
	$(XVFB_RUN) -a env GDK_BACKEND=x11 GDK_DEBUG=gl-prefer-gl GSK_RENDERER=cairo $(GTK4_TEST_IM_ENV) ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 \
		UBSAN_OPTIONS=halt_on_error=1 bash -c 'for policy in cairo prefer require; do for fault in none context; do $(BUILD_DIR)/tests/test_gtk4_rendering $$policy $$fault || exit; done; done'

test: test-gtk4-rendering

.PHONY: test-gtk4-opengl test-gtk4-opengl-videos

test-gtk4-opengl: $(GTK4_WIDGET_TEST_BINARY) $(BUILD_DIR)/tests/test_gtk4_rendering $(GTK4_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(FRONTEND_SAMPLE_X11_TEST_BINARY) $(WAYLAND_X11_PIXEL_PROBE_BINARY)
	$(XVFB_RUN) -a env GDK_BACKEND=x11 GDK_DEBUG=gl-prefer-gl GSK_RENDERER=gl $(GTK4_TEST_IM_ENV) BOXRIG_TEST_RENDER_OPENGL=1 ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(GTK4_WIDGET_TEST_BINARY) "$(TEST_FONT_FILE)"
	$(XVFB_RUN) -a env GDK_BACKEND=x11 GDK_DEBUG=gl-prefer-gl GSK_RENDERER=gl GDK_SCALE=2 $(GTK4_TEST_IM_ENV) BOXRIG_TEST_RENDER_OPENGL=1 ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(GTK4_WIDGET_TEST_BINARY) "$(TEST_FONT_FILE)"
	$(XVFB_RUN) -a env GDK_DEBUG=gl-prefer-gl BOXRIG_TEST_WESTON_BACKEND=x11 bash tests/gtk/run_gl_wayland.sh env GSK_RENDERER=gl $(GTK4_TEST_IM_ENV) BOXRIG_TEST_RENDER_OPENGL=1 $(GTK4_WIDGET_TEST_BINARY) "$(TEST_FONT_FILE)"
	$(XVFB_RUN) -a env GDK_DEBUG=gl-prefer-gl BOXRIG_TEST_WESTON_BACKEND=x11 bash tests/gtk/run_gl_wayland.sh env GSK_RENDERER=gl $(GTK4_TEST_IM_ENV) $(BUILD_DIR)/tests/test_gtk4_rendering require none
	$(XVFB_RUN) -a env GDK_BACKEND=x11 GDK_DEBUG=gl-prefer-gl GSK_RENDERER=ngl GDK_SCALE=2 $(GTK4_TEST_IM_ENV) ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(BUILD_DIR)/tests/test_gtk4_rendering require none
	$(XVFB_RUN) -a env GDK_BACKEND=x11 GDK_DEBUG=gl-prefer-gl GSK_RENDERER=gl $(GTK4_TEST_IM_ENV) BOXRIG_TEST_SAMPLE_RENDERER=--renderer=opengl ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(FRONTEND_SAMPLE_X11_TEST_BINARY) GTK4 $(GTK4_EXAMPLE_BINARY) $(PTY_HELPER_BINARY)
	$(XVFB_RUN) -a env GDK_DEBUG=gl-prefer-gl $(GTK4_TEST_IM_ENV) BOXRIG_TEST_SAMPLE_RENDERER=opengl BOXRIG_TEST_WESTON_RENDERER=gl $(WAYLAND_FRONTEND_SAMPLE_TEST_RUNNER) GTK4 "$(WESTON)" "$(GTK4_EXAMPLE_BINARY)" "$(PTY_HELPER_BINARY)" "$(WAYLAND_X11_PIXEL_PROBE_BINARY)"

test-gtk4-opengl-videos: $(GTK4_EXAMPLE_BINARY) $(PTY_HELPER_BINARY)
	$(XVFB_RUN) -a -s '-screen 0 1280x1024x24' env GDK_BACKEND=x11 GDK_DEBUG=gl-prefer-gl GSK_RENDERER=gl $(GTK4_TEST_IM_ENV) node tests/run_native_cursor_video.mjs $(GTK4_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/test-results/step24/gtk4-x11 opengl
	$(XVFB_RUN) -a -s '-screen 0 1280x1024x24' env GDK_BACKEND=wayland GDK_DEBUG=gl-prefer-gl GSK_RENDERER=gl $(GTK4_TEST_IM_ENV) BOXRIG_TEST_BLINK_WAYLAND=1 BOXRIG_TEST_WESTON_RENDERER=gl node tests/run_native_cursor_video.mjs $(GTK4_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/test-results/step24/gtk4-wayland opengl

test: test-gtk4-opengl test-gtk4-opengl-videos

.PHONY: test-gtk4-gles
test-gtk4-gles: $(GTK4_WIDGET_TEST_BINARY) $(BUILD_DIR)/tests/test_gtk4_rendering
	$(XVFB_RUN) -a env GDK_BACKEND=x11 GDK_DEBUG=gl-disable-gl GSK_RENDERER=gl $(GTK4_TEST_IM_ENV) BOXRIG_TEST_RENDER_OPENGL=1 ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(GTK4_WIDGET_TEST_BINARY) "$(TEST_FONT_FILE)"
	$(XVFB_RUN) -a env GDK_BACKEND=x11 GDK_DEBUG=gl-disable-gl GSK_RENDERER=ngl GDK_SCALE=2 $(GTK4_TEST_IM_ENV) ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(BUILD_DIR)/tests/test_gtk4_rendering require none
	$(XVFB_RUN) -a env GDK_BACKEND=x11 GDK_DEBUG=gl-disable-gl GSK_RENDERER=cairo LIBGL_ALWAYS_SOFTWARE=1 MESA_GLES_VERSION_OVERRIDE=3.0 MESA_EXTENSION_OVERRIDE=-GL_EXT_sRGB_write_control $(GTK4_TEST_IM_ENV) ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(BUILD_DIR)/tests/test_gtk4_rendering require none
	$(XVFB_RUN) -a env GDK_DEBUG=gl-disable-gl BOXRIG_TEST_WESTON_BACKEND=x11 bash tests/gtk/run_gl_wayland.sh env GSK_RENDERER=gl $(GTK4_TEST_IM_ENV) $(BUILD_DIR)/tests/test_gtk4_rendering require none

test: test-gtk4-gles

.PHONY: test-gtk-render-switch
GTK_SWITCH_WRAPPERS := -Wl,--wrap=gtk_gl_area_new -Wl,--wrap=boxrig_gtk_gl_surface_create -Wl,--wrap=boxrig_gtk_gl_surface_destroy \
	-Wl,--wrap=boxrig_gtk_gl_surface_draw -Wl,--wrap=boxrig_gtk_gl_surface_present -Wl,--wrap=boxrig_pango_cairo_renderer_draw_view_composition

$(BUILD_DIR)/tests/test_gtk3_render_switch: tests/gtk/test_render_switch.c $(GTK3_HEADERS) $(GTK3_LIBRARY) $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) -Iinclude -Isrc $(GTK3_CPPFLAGS) $(CFLAGS) $(GTK3_PROJECT_CFLAGS) $(SANITIZERS) $< \
		$(GTK_SWITCH_WRAPPERS) $(GTK3_LIBRARY) $(UI_LIBRARY) $(LIBRARY) $(GTK3_LIBS) -o $@

$(BUILD_DIR)/tests/test_gtk4_render_switch: tests/gtk/test_render_switch.c $(GTK4_HEADERS) $(GTK4_LIBRARY) $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) -Iinclude -Isrc $(GTK4_CPPFLAGS) $(CFLAGS) $(GTK4_PROJECT_CFLAGS) $(SANITIZERS) $< \
		$(GTK_SWITCH_WRAPPERS) $(GTK4_LIBRARY) $(UI_LIBRARY) $(LIBRARY) $(GTK4_LIBS) -o $@

test-gtk-render-switch: $(BUILD_DIR)/tests/test_gtk3_render_switch $(BUILD_DIR)/tests/test_gtk4_render_switch
	$(XVFB_RUN) -a env GDK_BACKEND=x11 ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(BUILD_DIR)/tests/test_gtk3_render_switch
	$(XVFB_RUN) -a env GDK_BACKEND=x11 GDK_DEBUG=gl-prefer-gl GSK_RENDERER=gl $(GTK4_TEST_IM_ENV) ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(BUILD_DIR)/tests/test_gtk4_render_switch
	$(XVFB_RUN) -a env BOXRIG_TEST_WESTON_BACKEND=x11 bash tests/gtk/run_gl_wayland.sh $(BUILD_DIR)/tests/test_gtk3_render_switch
	$(XVFB_RUN) -a env GDK_DEBUG=gl-prefer-gl BOXRIG_TEST_WESTON_BACKEND=x11 bash tests/gtk/run_gl_wayland.sh env GSK_RENDERER=gl $(GTK4_TEST_IM_ENV) $(BUILD_DIR)/tests/test_gtk4_render_switch

test: test-gtk-render-switch

.PHONY: test-gtk-render-switch-videos
test-gtk-render-switch-videos: $(GTK3_EXAMPLE_BINARY) $(GTK4_EXAMPLE_BINARY) $(PTY_HELPER_BINARY)
	$(XVFB_RUN) -a -s '-screen 0 1280x1024x24' env GDK_BACKEND=x11 BOXRIG_TEST_RENDER_SWITCH_VIDEO=1 node tests/run_native_cursor_video.mjs $(GTK3_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/test-results/step24/switch-gtk3-x11 cairo
	$(XVFB_RUN) -a -s '-screen 0 1280x1024x24' env GDK_BACKEND=wayland BOXRIG_TEST_BLINK_WAYLAND=1 BOXRIG_TEST_WESTON_RENDERER=gl BOXRIG_TEST_RENDER_SWITCH_VIDEO=1 node tests/run_native_cursor_video.mjs $(GTK3_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/test-results/step24/switch-gtk3-wayland cairo
	$(XVFB_RUN) -a -s '-screen 0 1280x1024x24' env GDK_BACKEND=x11 GDK_DEBUG=gl-prefer-gl GSK_RENDERER=gl $(GTK4_TEST_IM_ENV) BOXRIG_TEST_RENDER_SWITCH_VIDEO=1 node tests/run_native_cursor_video.mjs $(GTK4_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/test-results/step24/switch-gtk4-x11 cairo
	$(XVFB_RUN) -a -s '-screen 0 1280x1024x24' env GDK_BACKEND=wayland GDK_DEBUG=gl-prefer-gl GSK_RENDERER=gl $(GTK4_TEST_IM_ENV) BOXRIG_TEST_BLINK_WAYLAND=1 BOXRIG_TEST_WESTON_RENDERER=gl BOXRIG_TEST_RENDER_SWITCH_VIDEO=1 node tests/run_native_cursor_video.mjs $(GTK4_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/test-results/step24/switch-gtk4-wayland cairo

test: test-gtk-render-switch-videos

.PHONY: test-gtk-build-variants
test-gtk-build-variants:
	$(MAKE) BUILD_DIR=$(BUILD_DIR)/gtk-build-variants GTK_OPENGL=1 test-gtk3-package test-gtk4-package
	$(MAKE) BUILD_DIR=$(BUILD_DIR)/gtk-build-variants GTK_OPENGL=0 test-gtk3-package test-gtk4-package
	$(MAKE) BUILD_DIR=$(BUILD_DIR)/gtk-build-variants GTK_OPENGL=1 test-gtk3-package test-gtk4-package

test: test-gtk-build-variants

.PHONY: test-gtk-opengl-continuous-output-videos
test-gtk-opengl-continuous-output-videos: $(FRONTEND_SAMPLE_X11_TEST_BINARY) $(GTK3_EXAMPLE_BINARY) $(GTK4_EXAMPLE_BINARY) $(PTY_HELPER_BINARY)
	$(XVFB_RUN) -a env GDK_BACKEND=x11 BOXRIG_TEST_SAMPLE_RENDERER=--renderer=opengl node tests/run_continuous_output_video.mjs $(FRONTEND_SAMPLE_X11_TEST_BINARY) GTK3 $(GTK3_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/test-results/step24/continuous-gtk3
	$(XVFB_RUN) -a env GDK_BACKEND=x11 GDK_DEBUG=gl-prefer-gl GSK_RENDERER=gl $(GTK4_TEST_IM_ENV) BOXRIG_TEST_SAMPLE_RENDERER=--renderer=opengl node tests/run_continuous_output_video.mjs $(FRONTEND_SAMPLE_X11_TEST_BINARY) GTK4 $(GTK4_EXAMPLE_BINARY) $(PTY_HELPER_BINARY) $(BUILD_DIR)/test-results/step24/continuous-gtk4

test: test-gtk-opengl-continuous-output-videos

# Real IBus transport with a deterministic surrounding-text engine.
.PHONY: gtk-surrounding-driver test-gtk-surrounding-driver
gtk-surrounding-driver: $(BUILD_DIR)/tests/surrounding_engine $(BUILD_DIR)/tests/gtk3_surrounding_driver $(BUILD_DIR)/tests/gtk4_surrounding_driver $(BUILD_DIR)/tests/ime_capabilities

$(BUILD_DIR)/tests/ime_capabilities: tests/p2/ime_capabilities.c $(X11_LIBRARY) $(UI_LIBRARY) $(LIBRARY) $(UI_HEADERS) $(X11_HEADERS) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) $$(pkg-config --cflags x11 wayland-client) $< $(X11_LIBRARY) $(UI_LIBRARY) $(LIBRARY) $(X11_LIBS) $$(pkg-config --libs wayland-client) -o $@

$(BUILD_DIR)/tests/surrounding_engine: tests/gtk/surrounding_engine.c | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) -Wno-pedantic $$(pkg-config --cflags ibus-1.0) $< $$(pkg-config --libs ibus-1.0) -o $@

$(BUILD_DIR)/tests/gtk3_surrounding_driver: tests/gtk/test_surrounding_driver.c $(LIBRARY) $(CORE_HEADERS) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) -Iinclude $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) -Wno-pedantic $(GTK3_CPPFLAGS) $$(pkg-config --cflags ibus-1.0) $< $(LIBRARY) $(GTK3_LIBS) $$(pkg-config --libs ibus-1.0) -o $@

$(BUILD_DIR)/tests/gtk4_surrounding_driver: tests/gtk/test_surrounding_driver.c $(LIBRARY) $(CORE_HEADERS) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) -Iinclude $(CFLAGS) $(GTK4_PROJECT_CFLAGS) $(SANITIZERS) $(GTK4_CPPFLAGS) $$(pkg-config --cflags ibus-1.0) $< $(LIBRARY) $(GTK4_LIBS) $$(pkg-config --libs ibus-1.0) -o $@

test-gtk-surrounding-driver: gtk-surrounding-driver
	$(XVFB_RUN) -a dbus-run-session -- env GDK_BACKEND=x11 GSK_RENDERER=cairo ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 bash tests/gtk/run_surrounding_driver.sh $(BUILD_DIR)
	$(XVFB_RUN) -a env BOXRIG_TEST_WESTON_BACKEND=x11 bash tests/gtk/run_gl_wayland.sh $(BUILD_DIR)/tests/ime_capabilities wayland

.PHONY: test-editable-context
$(BUILD_DIR)/tests/test_editable_context: tests/test_editable_context.c src/ui/editable.c src/ui/editable_private.h $(UI_HEADERS) $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) $< src/ui/editable.c $(UI_LIBRARY) $(LIBRARY) -Wl,--wrap=malloc -Wl,--wrap=calloc -lm -o $@

test-editable-context: $(BUILD_DIR)/tests/test_editable_context
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(BUILD_DIR)/tests/test_editable_context

test: test-editable-context

.PHONY: test-editable-edits
$(BUILD_DIR)/tests/test_editable_edits: tests/test_editable_edits.c src/ui/editable.c src/ui/editable_private.h $(UI_HEADERS) $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) $< src/ui/editable.c $(UI_LIBRARY) $(LIBRARY) -Wl,--wrap=malloc -Wl,--wrap=calloc -lm -o $@

test-editable-edits: $(BUILD_DIR)/tests/test_editable_edits
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(BUILD_DIR)/tests/test_editable_edits

test: test-editable-edits

.PHONY: test-editable-win32
test-editable-win32: $(MINGW_AMD64_INPUT_TEST) $(MINGW_I686_INPUT_TEST) $(P2_WIN32_RECONVERSION_HOST) $(MINGW_I686_RECONVERSION_HOST)
	$(WINE_RUN) $(abspath $(MINGW_AMD64_INPUT_TEST))
	$(WINE_RUN) $(abspath $(MINGW_I686_INPUT_TEST))

.PHONY: test-gtk-editable
$(BUILD_DIR)/tests/gtk3_editable_widget: tests/gtk/test_editable_widget.c tests/editable_fixture.h src/gtk3/input.c src/gtk3/input.h $(GTK3_LIBRARY) $(UI_LIBRARY) $(LIBRARY) $(GTK3_HEADERS) $(UI_HEADERS) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(POSIX_CPPFLAGS) $(GTK3_CPPFLAGS) $(CFLAGS) $(GTK3_PROJECT_CFLAGS) $(SANITIZERS) -Wl,--wrap=gtk_im_multicontext_new,--wrap=gtk_im_context_set_surrounding,--wrap=boxrig_ui_editable_document_apply_edit $< src/gtk3/input.c $(GTK3_LIBRARY) $(UI_LIBRARY) $(LIBRARY) $(GTK3_LIBS) -lm -o $@

$(BUILD_DIR)/tests/gtk4_editable_widget: tests/gtk/test_editable_widget.c tests/editable_fixture.h src/gtk4/input.c src/gtk4/input.h $(GTK4_LIBRARY) $(UI_LIBRARY) $(LIBRARY) $(GTK4_HEADERS) $(UI_HEADERS) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(POSIX_CPPFLAGS) $(GTK4_CPPFLAGS) $(CFLAGS) $(GTK4_PROJECT_CFLAGS) $(SANITIZERS) -Wl,--wrap=gtk_im_multicontext_new,--wrap=gtk_im_context_set_surrounding_with_selection,--wrap=gtk_event_controller_focus_new,--wrap=boxrig_ui_editable_document_apply_edit $< src/gtk4/input.c $(GTK4_LIBRARY) $(UI_LIBRARY) $(LIBRARY) $(GTK4_LIBS) -lm -o $@

test-gtk-editable: $(BUILD_DIR)/tests/gtk3_editable_widget $(BUILD_DIR)/tests/gtk4_editable_widget
	$(XVFB_RUN) -a env GDK_BACKEND=x11 GSK_RENDERER=cairo GTK_IM_MODULE=gtk-im-context-simple NO_AT_BRIDGE=1 ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(BUILD_DIR)/tests/gtk3_editable_widget
	$(XVFB_RUN) -a env GDK_BACKEND=x11 GSK_RENDERER=cairo GTK_IM_MODULE=gtk-im-context-simple NO_AT_BRIDGE=1 ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(BUILD_DIR)/tests/gtk4_editable_widget

test: test-gtk-editable

.PHONY: test-xim-editable
$(BUILD_DIR)/tests/test_xim_editable: tests/x11/test_xim_editable.c tests/editable_fixture.h src/x11/input.c include/boxrig/x11/input.h $(UI_HEADERS) $(X11_LIBRARY) $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(FRONTEND_CPPFLAGS) $(POSIX_CPPFLAGS) $(X11_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) $< src/x11/input.c $(X11_LIBRARY) $(UI_LIBRARY) $(LIBRARY) $(X11_LIBS) $(DBUS_LIBS) -Wl,--wrap=XOpenIM,--wrap=XCloseIM,--wrap=XGetIMValues,--wrap=XCreateIC,--wrap=XSetICValues,--wrap=XSetICFocus,--wrap=XUnsetICFocus,--wrap=XDestroyIC,--wrap=Xutf8ResetIC,--wrap=XFilterEvent,--wrap=Xutf8LookupString,--wrap=malloc,--wrap=calloc -o $@

# Each case reconnects; keep the isolated server from resetting between clients.
test-xim-editable: $(BUILD_DIR)/tests/test_xim_editable
	$(XVFB_RUN) -a -e /dev/stderr -s '-screen 0 1280x1024x24 -noreset' env ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $<

test: test-xim-editable

.PHONY: test-xim-capabilities
test-xim-capabilities: $(BUILD_DIR)/tests/surrounding_engine $(BUILD_DIR)/tests/ime_capabilities
	$(XVFB_RUN) -a dbus-run-session -- env GDK_BACKEND=x11 BOXRIG_PROBE_XIM_ONLY=1 ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 bash tests/gtk/run_surrounding_driver.sh $(BUILD_DIR) $(BUILD_DIR)/test-results/step25/25-6/xim

.PHONY: gtk-surrounding-widget test-gtk-surrounding-widget
gtk-surrounding-widget: $(BUILD_DIR)/tests/surrounding_engine $(BUILD_DIR)/tests/ime_capabilities $(BUILD_DIR)/tests/gtk3_surrounding_widget $(BUILD_DIR)/tests/gtk4_surrounding_widget

$(BUILD_DIR)/tests/gtk3_surrounding_widget: tests/gtk/test_surrounding_widget.c tests/x11_video.h $(GTK3_LIBRARY) $(UI_LIBRARY) $(LIBRARY) $(GTK3_HEADERS) $(UI_HEADERS) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(POSIX_CPPFLAGS) $(GTK3_CPPFLAGS) $$(pkg-config --cflags ibus-1.0) $(CFLAGS) $(GTK3_PROJECT_CFLAGS) $(SANITIZERS) -Wno-pedantic -Wl,--wrap=gtk_im_multicontext_new $< $(GTK3_LIBRARY) $(UI_LIBRARY) $(LIBRARY) $(GTK3_LIBS) $$(pkg-config --libs ibus-1.0) -lm -o $@

$(BUILD_DIR)/tests/gtk4_surrounding_widget: tests/gtk/test_surrounding_widget.c tests/x11_video.h $(GTK4_LIBRARY) $(UI_LIBRARY) $(LIBRARY) $(GTK4_HEADERS) $(UI_HEADERS) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(POSIX_CPPFLAGS) $(GTK4_CPPFLAGS) $$(pkg-config --cflags ibus-1.0) $(CFLAGS) $(GTK4_PROJECT_CFLAGS) $(SANITIZERS) -Wl,--wrap=gtk_im_multicontext_new $< $(GTK4_LIBRARY) $(UI_LIBRARY) $(LIBRARY) $(GTK4_LIBS) $$(pkg-config --libs ibus-1.0) -lm -o $@

test-gtk-surrounding-widget: gtk-surrounding-widget
	$(XVFB_RUN) -a dbus-run-session -- env BOXRIG_PROBE_WIDGET=1 GDK_BACKEND=x11 GSK_RENDERER=cairo ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 bash tests/gtk/run_surrounding_driver.sh $(BUILD_DIR) $(BUILD_DIR)/test-results/step25/25-3-x11
	$(XVFB_RUN) -a env BOXRIG_TEST_WESTON_BACKEND=x11 bash tests/gtk/run_gl_wayland.sh dbus-run-session -- env BOXRIG_PROBE_WIDGET=1 bash tests/gtk/run_surrounding_driver.sh $(BUILD_DIR) $(BUILD_DIR)/test-results/step25/25-3-wayland

.PHONY: test-gtk-ime-video
test-gtk-ime-video: gtk-surrounding-widget
	$(XVFB_RUN) -a dbus-run-session -- env BOXRIG_PROBE_WIDGET=1 BOXRIG_PROBE_VIDEO=1 BOXRIG_PROBE_GTK_VERSIONS=4 BOXRIG_PROBE_RENDERERS=cairo BOXRIG_PROBE_MODES=mozc GDK_BACKEND=x11 GSK_RENDERER=cairo ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 bash tests/gtk/run_surrounding_driver.sh $(BUILD_DIR) $(BUILD_DIR)/test-results/step25/25-3-video

# Unmodified test-only protocol from swaywm/wlroots commit
# 0855cdacb2eeeff35849e2e9c4db0aa996d78d10/protocol/input-method-unstable-v2.xml.
# https://github.com/swaywm/wlroots/blob/0855cdacb2eeeff35849e2e9c4db0aa996d78d10/protocol/input-method-unstable-v2.xml
$(WAYLAND_PROTOCOL_DIR)/input-method-v2-client-protocol.h: tests/wayland/protocols/input-method-unstable-v2.xml | $(WAYLAND_PROTOCOL_DIR)
	$(WAYLAND_SCANNER) client-header $< $@

$(WAYLAND_PROTOCOL_DIR)/input-method-v2-protocol.c: tests/wayland/protocols/input-method-unstable-v2.xml | $(WAYLAND_PROTOCOL_DIR)
	$(WAYLAND_SCANNER) private-code $< $@

$(BUILD_DIR)/tests/wayland_surrounding_compositor: tests/wayland/test_surrounding_compositor.c $(WAYLAND_PROTOCOL_DIR)/input-method-v2-client-protocol.h $(WAYLAND_PROTOCOL_DIR)/input-method-v2-protocol.c $(WAYLAND_XDG_SHELL_HEADER) $(WAYLAND_XDG_SHELL_SOURCE) $(WAYLAND_LIBRARY) $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(POSIX_CPPFLAGS) -I$(WAYLAND_PROTOCOL_DIR) $(WAYLAND_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) $< $(WAYLAND_PROTOCOL_DIR)/input-method-v2-protocol.c $(WAYLAND_XDG_SHELL_SOURCE) $(WAYLAND_LIBRARY) $(UI_LIBRARY) $(LIBRARY) $(WAYLAND_LIBS) $(PANGO_CAIRO_LIBS) -o $@

.PHONY: test-wayland-editable test-wayland-surrounding-compositor
test-wayland-editable: $(WAYLAND_CONTEXT_TEST_BINARY)
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 dbus-run-session -- node tests/wayland/run_ibus_context.mjs "$(WAYLAND_CONTEXT_TEST_BINARY)" "$(TEST_FONT_FILE)" "$(BUILD_DIR)/tests/wayland_ibus_engine" "$(BUILD_DIR)/test-results/ime-compat/ibus"

test-wayland-surrounding-compositor: $(BUILD_DIR)/tests/wayland_surrounding_compositor
	$(XVFB_RUN) -a bash tests/wayland/run_surrounding_compositor.sh $(BUILD_DIR)

$(EXAMPLE_BINARY) $(GTK3_EXAMPLE_BINARY) $(GTK4_EXAMPLE_BINARY) $(WAYLAND_EXAMPLE_BINARY) $(WAYLAND_NO_LIBDECOR_TEST_BINARY): examples/boxrig_term/common/font_demo.h

.PHONY: test-color-glyphs test-color-glyph-videos
test-color-glyphs: $(PANGO_CAIRO_FIXTURE_TEST_BINARY) $(UI_PLATFORM_FIXTURE_TEST_BINARY) $(OPENGL_RENDERER_FIXTURE_TEST_BINARY)
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 $(PANGO_CAIRO_FIXTURE_TEST_BINARY)
	$(XVFB_RUN) -a env ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 $(UI_PLATFORM_FIXTURE_TEST_BINARY)
	$(XVFB_RUN) -a env LIBGL_ALWAYS_SOFTWARE=1 ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 $(OPENGL_RENDERER_FIXTURE_TEST_BINARY)

test-color-glyph-videos: $(EXAMPLE_BINARY) $(GTK3_EXAMPLE_BINARY) $(GTK4_EXAMPLE_BINARY) $(WAYLAND_EXAMPLE_BINARY)
	@set -e; for frontend in gtk3 gtk4; do \
		for backend in x11 wayland; do \
			for renderer in cairo opengl; do \
				$(XVFB_RUN) -a env LIBGL_ALWAYS_SOFTWARE=1 node tests/run_color_glyph_video.mjs \
					$(BUILD_DIR)/boxrig_term/$$frontend/boxrig_term \
					$(BUILD_DIR)/tests/color-glyphs/$$frontend-$$backend-$$renderer $$renderer $$backend; \
			done; \
		done; \
	done
	@set -e; for frontend in x11 wayland; do \
		for renderer in $$frontend opengl; do \
			$(XVFB_RUN) -a env LIBGL_ALWAYS_SOFTWARE=1 node tests/run_color_glyph_video.mjs \
				$(BUILD_DIR)/boxrig_term/$$frontend/boxrig_term \
				$(BUILD_DIR)/tests/color-glyphs/$$frontend-$$renderer $$renderer $$frontend; \
		done; \
	done

test: test-color-glyph-videos

# The IME fixture uses its own bus, deterministic engine, and panel service.
test test-wayland-editable: $(BUILD_DIR)/tests/wayland_ibus_engine
.PHONY: test-wayland-input
test-wayland-input: $(WAYLAND_CONTEXT_TEST_BINARY) $(BUILD_DIR)/tests/wayland_ibus_engine
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 dbus-run-session -- node tests/wayland/run_ibus_context.mjs "$(WAYLAND_CONTEXT_TEST_BINARY)" "$(TEST_FONT_FILE)" "$(BUILD_DIR)/tests/wayland_ibus_engine" "$(BUILD_DIR)/test-results/ime-compat/ibus"
$(BUILD_DIR)/tests/wayland_ibus_engine: tests/wayland/ibus_engine.c | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) -Wno-pedantic $(IBUS_CPPFLAGS) $< $(IBUS_LIBS) -o $@

.PHONY: test-ime-clause
test-ime-clause: $(GTK3_WIDGET_TEST_BINARY) $(GTK4_WIDGET_TEST_BINARY) $(POSIX_EXAMPLE_COMMON_TEST_BINARY)
	$(XVFB_RUN) -a env -u GDK_SCALE GDK_BACKEND=x11 NO_AT_BRIDGE=1 ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 LSAN_OPTIONS=suppressions=$(abspath tests/gtk3/gdk_scale_leaks.supp):print_suppressions=1 UBSAN_OPTIONS=halt_on_error=1 $(GTK3_WIDGET_TEST_BINARY) "$(TEST_FONT_FILE)"
	$(XVFB_RUN) -a env GDK_SCALE=2 GDK_BACKEND=x11 NO_AT_BRIDGE=1 ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(GTK3_WIDGET_TEST_BINARY) "$(TEST_FONT_FILE)"
	$(GTK3_WAYLAND_TEST_RUNNER) "$(WESTON)" "$(GTK3_WIDGET_TEST_BINARY)" "$(TEST_FONT_FILE)"
	$(XVFB_RUN) -a env $(GTK4_TEST_IM_ENV) GDK_BACKEND=x11 GSK_RENDERER=cairo NO_AT_BRIDGE=1 ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(GTK4_WIDGET_TEST_BINARY) "$(TEST_FONT_FILE)"
	$(XVFB_RUN) -a env $(GTK4_TEST_IM_ENV) GDK_BACKEND=x11 GDK_SCALE=2 GSK_RENDERER=cairo NO_AT_BRIDGE=1 ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(GTK4_WIDGET_TEST_BINARY) "$(TEST_FONT_FILE)"
	env $(GTK4_TEST_IM_ENV) $(GTK4_WAYLAND_TEST_RUNNER) "$(WESTON)" "$(GTK4_WIDGET_TEST_BINARY)" "$(TEST_FONT_FILE)"
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(POSIX_EXAMPLE_COMMON_TEST_BINARY)

# Direct IBus uses a private daemon and engine, never the desktop input method.
.PHONY: test-gtk-ibus-direct
$(BUILD_DIR)/tests/test_gtk4_ibus: tests/gtk/test_ibus_widget.c include/boxrig/gtk/input.h $(IME_SOURCES) $(IME_HEADERS) $(GTK4_LIBRARY) $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(FRONTEND_CPPFLAGS) $(POSIX_CPPFLAGS) $(GTK4_CPPFLAGS) $(IBUS_CPPFLAGS) $(CFLAGS) $(GTK4_PROJECT_CFLAGS) $(SANITIZERS) $< $(IME_SOURCES) $(GTK4_LIBRARY) $(UI_LIBRARY) $(LIBRARY) $(GTK4_LIBS) $(IBUS_LIBS) $(SANITIZERS) -Wl,--wrap=gtk_im_multicontext_new,--wrap=gtk_event_controller_key_new,--wrap=gtk_event_controller_focus_new -o $@

$(BUILD_DIR)/tests/test_gtk3_ibus: tests/gtk/test_ibus_widget.c include/boxrig/gtk/input.h $(IME_SOURCES) $(IME_HEADERS) $(GTK3_LIBRARY) $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(FRONTEND_CPPFLAGS) $(POSIX_CPPFLAGS) $(GTK3_CPPFLAGS) $(IBUS_CPPFLAGS) $(CFLAGS) $(GTK3_PROJECT_CFLAGS) $(SANITIZERS) -DBOXRIG_TEST_GTK3=1 $< $(IME_SOURCES) $(GTK3_LIBRARY) $(UI_LIBRARY) $(LIBRARY) $(GTK3_LIBS) $(IBUS_LIBS) $(SANITIZERS) -o $@

test-gtk-ibus-direct: $(BUILD_DIR)/tests/test_gtk3_ibus $(BUILD_DIR)/tests/test_gtk4_ibus $(BUILD_DIR)/tests/wayland_ibus_engine
	@for gtk in 3 4; do for scale in 1 2; do for renderer in cairo opengl; do \
		$(XVFB_RUN) -a env BOXRIG_TEST_WESTON_BACKEND=x11 GDK_SCALE=$$scale BOXRIG_TEST_DELAY_ESCAPE_RELEASE=1 BOXRIG_TEST_IBUS_WAYLAND=1 GIO_USE_VFS=local GTK_A11Y=none dbus-run-session --config-file=tests/ime/session.conf -- bash tests/gtk/run_gl_wayland.sh node tests/wayland/run_ibus_context.mjs "$(BUILD_DIR)/tests/test_gtk$${gtk}_ibus" "$(TEST_FONT_FILE)" "$(BUILD_DIR)/tests/wayland_ibus_engine" "$(BUILD_DIR)/test-results/gtk$${gtk}-ibus/$$scale-$$renderer" $$renderer || exit $$?; \
	done; done; done

$(BUILD_DIR)/ime:
	mkdir -p $@

$(BUILD_DIR)/ime/client.o: src/ime/client.c $(IME_HEADERS) | $(BUILD_DIR)/ime
	$(CC) $(CPPFLAGS) $(FRONTEND_CPPFLAGS) $(IBUS_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) -c $< -o $@

$(BUILD_DIR)/ime/detect.o: src/ime/detect.c $(IME_HEADERS) | $(BUILD_DIR)/ime
	$(CC) $(CPPFLAGS) $(FRONTEND_CPPFLAGS) $(IBUS_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) -c $< -o $@

$(BUILD_DIR)/ime/fcitx5.o: src/fcitx5/client.c $(IME_HEADERS) | $(BUILD_DIR)/ime
	$(CC) $(CPPFLAGS) $(FRONTEND_CPPFLAGS) $(IBUS_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) -c $< -o $@

$(BUILD_DIR)/ime/fcitx4.o: src/fcitx4/client.c $(IME_HEADERS) | $(BUILD_DIR)/ime
	$(CC) $(CPPFLAGS) $(FRONTEND_CPPFLAGS) $(IBUS_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) -c $< -o $@

$(BUILD_DIR)/ime/fcitx4_connection.o: src/fcitx4/connection.c $(IME_HEADERS) | $(BUILD_DIR)/ime
	$(CC) $(CPPFLAGS) $(FRONTEND_CPPFLAGS) $(POSIX_CPPFLAGS) $(IBUS_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) -c $< -o $@

$(BUILD_DIR)/ime/ibus.o: src/ibus/client.c $(IME_HEADERS) | $(BUILD_DIR)/ime
	$(CC) $(CPPFLAGS) $(FRONTEND_CPPFLAGS) $(IBUS_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) -c $< -o $@

$(BUILD_DIR)/ime/source.o: src/ime/source.c $(IME_HEADERS) | $(BUILD_DIR)/ime
	$(CC) $(CPPFLAGS) $(FRONTEND_CPPFLAGS) $(IBUS_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) -c $< -o $@

$(BUILD_DIR)/ime/preedit.o: src/ime/preedit.c $(IME_HEADERS) | $(BUILD_DIR)/ime
	$(CC) $(CPPFLAGS) $(FRONTEND_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) -c $< -o $@

$(GTK3_OBJECTS) $(GTK4_OBJECTS) $(WAYLAND_OBJECTS): $(IME_HEADERS)

.PHONY: test-gtk-ibus-auto
test-gtk-ibus-auto: $(BUILD_DIR)/tests/test_gtk3_ibus $(BUILD_DIR)/tests/test_gtk4_ibus $(BUILD_DIR)/tests/wayland_ibus_engine
	@for gtk in 3 4; do \
		$(XVFB_RUN) -a env BOXRIG_TEST_WESTON_BACKEND=x11 GDK_SCALE=1 BOXRIG_TEST_IBUS_WAYLAND=1 GIO_USE_VFS=local GTK_A11Y=none dbus-run-session --config-file=tests/ime/session.conf -- bash tests/gtk/run_gl_wayland.sh node tests/wayland/run_ibus_context.mjs "$(BUILD_DIR)/tests/test_gtk$${gtk}_ibus" "$(TEST_FONT_FILE)" "$(BUILD_DIR)/tests/wayland_ibus_engine" "$(BUILD_DIR)/test-results/gtk$${gtk}-ibus/auto" cairo auto || exit $$?; \
	done

.PHONY: test-gtk-ime-startup
test-gtk-ime-startup: $(POSIX_EXAMPLE_COMMON_TEST_BINARY) $(GTK3_EXAMPLE_BINARY) $(GTK4_EXAMPLE_BINARY) $(BUILD_DIR)/tests/wayland_ibus_engine
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(POSIX_EXAMPLE_COMMON_TEST_BINARY)
	xvfb-run -a env GDK_BACKEND=x11 GTK_USE_PORTAL=0 dbus-run-session -- node tests/wayland/run_ibus_context.mjs "$(shell command -v node)" tests/gtk/test_sample_ime.mjs "$(BUILD_DIR)/tests/wayland_ibus_engine" "$(BUILD_DIR)/test-results/gtk-ime-startup" "$(GTK3_EXAMPLE_BINARY)" "$(GTK4_EXAMPLE_BINARY)"

$(BUILD_DIR)/gtk3/ime_probe.o: src/gtk/ime_probe.c src/gtk/ime_probe.h include/boxrig/gtk/input.h | $(BUILD_DIR)/gtk3
	$(CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(POSIX_CPPFLAGS) $(GTK3_CPPFLAGS) $(CFLAGS) $(GTK3_PROJECT_CFLAGS) -c $< -o $@

$(BUILD_DIR)/gtk4/ime_probe.o: src/gtk/ime_probe.c src/gtk/ime_probe.h include/boxrig/gtk/input.h | $(BUILD_DIR)/gtk4
	$(CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(POSIX_CPPFLAGS) $(GTK4_CPPFLAGS) $(CFLAGS) $(GTK4_PROJECT_CFLAGS) -c $< -o $@

$(GTK3_OBJECTS) $(GTK4_OBJECTS): src/gtk/ime_probe.h src/gtk/ime_cursor.h

# A private compositor drives the real loaded GTK library through text-input-v3.
GTK_IME_XDG_SERVER_HEADER := $(WAYLAND_PROTOCOL_DIR)/xdg-shell-server-protocol.h
$(GTK_IME_XDG_SERVER_HEADER): $(WAYLAND_XDG_SHELL_XML) | $(WAYLAND_PROTOCOL_DIR)
	$(WAYLAND_SCANNER) server-header $< $@

$(BUILD_DIR)/tests/test_gtk3_ime_protocol: tests/gtk/test_ime_protocol.c tests/gtk/ime_server.c tests/gtk/ime_server.h $(GTK_IME_XDG_SERVER_HEADER) $(WAYLAND_TEXT_INPUT_SERVER_HEADER) $(WAYLAND_TEXT_INPUT_SOURCE) $(WAYLAND_XDG_SHELL_SOURCE) $(GTK3_LIBRARY) $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(FRONTEND_CPPFLAGS) $(POSIX_CPPFLAGS) $(GTK3_CPPFLAGS) $(WAYLAND_CPPFLAGS) -I$(WAYLAND_PROTOCOL_DIR) $(CFLAGS) $(GTK3_PROJECT_CFLAGS) $(SANITIZERS) -DBOXRIG_TEST_GTK3=1 $< tests/gtk/ime_server.c $(WAYLAND_TEXT_INPUT_SOURCE) $(WAYLAND_XDG_SHELL_SOURCE) $(GTK3_LIBRARY) $(UI_LIBRARY) $(LIBRARY) $(GTK3_LIBS) $(WAYLAND_LIBS) $(SANITIZERS) -o $@

$(BUILD_DIR)/tests/test_gtk4_ime_protocol: tests/gtk/test_ime_protocol.c tests/gtk/ime_server.c tests/gtk/ime_server.h $(GTK_IME_XDG_SERVER_HEADER) $(WAYLAND_TEXT_INPUT_SERVER_HEADER) $(WAYLAND_TEXT_INPUT_SOURCE) $(WAYLAND_XDG_SHELL_SOURCE) $(GTK4_LIBRARY) $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(FRONTEND_CPPFLAGS) $(POSIX_CPPFLAGS) $(GTK4_CPPFLAGS) $(WAYLAND_CPPFLAGS) -I$(WAYLAND_PROTOCOL_DIR) $(CFLAGS) $(GTK4_PROJECT_CFLAGS) $(SANITIZERS) $< tests/gtk/ime_server.c $(WAYLAND_TEXT_INPUT_SOURCE) $(WAYLAND_XDG_SHELL_SOURCE) $(GTK4_LIBRARY) $(UI_LIBRARY) $(LIBRARY) $(GTK4_LIBS) $(WAYLAND_LIBS) $(SANITIZERS) -o $@

.PHONY: test-gtk-ime-routing
test-gtk-ime-routing: $(BUILD_DIR)/tests/test_gtk3_ime_protocol $(BUILD_DIR)/tests/test_gtk4_ime_protocol $(BUILD_DIR)/tests/wayland_ibus_engine
	node tests/gtk/run_ime_protocol.mjs "$(BUILD_DIR)/tests/test_gtk3_ime_protocol" "$(BUILD_DIR)/tests/test_gtk4_ime_protocol" "$(TEST_FONT_FILE)" "$(BUILD_DIR)/tests/wayland_ibus_engine" "$(BUILD_DIR)/test-results/gtk-ime-routing"

# X11 uses GTK IBus, including AUTO without GTK_IM_MODULE.

$(BUILD_DIR)/tests/test_ibus_candidates: tests/wayland/test_ibus_candidates.c $(IME_SOURCES) $(IME_HEADERS) $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(FRONTEND_CPPFLAGS) $(POSIX_CPPFLAGS) $(IBUS_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) $< $(IME_SOURCES) $(UI_LIBRARY) $(LIBRARY) $(IBUS_LIBS) $(SANITIZERS) -Wl,--wrap=ibus_input_context_new_async_finish -o $@

.PHONY: test-ibus-candidates
test-ibus-candidates: $(BUILD_DIR)/tests/test_ibus_candidates $(BUILD_DIR)/tests/wayland_ibus_engine
	env ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 dbus-run-session -- node tests/wayland/run_ibus_context.mjs "$(BUILD_DIR)/tests/test_ibus_candidates" "$(TEST_FONT_FILE)" "$(BUILD_DIR)/tests/wayland_ibus_engine" "$(BUILD_DIR)/test-results/ibus-candidates"

$(BUILD_DIR)/tests/test_gtk3_ime_x11: tests/gtk/test_ibus_widget.c $(GTK3_LIBRARY) $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(FRONTEND_CPPFLAGS) $(POSIX_CPPFLAGS) $(GTK3_CPPFLAGS) $(shell pkg-config --cflags xtst x11) $(CFLAGS) $(GTK3_PROJECT_CFLAGS) $(SANITIZERS) -DBOXRIG_TEST_GTK3=1 -DBOXRIG_TEST_IME_X11=1 $< $(GTK3_LIBRARY) $(UI_LIBRARY) $(LIBRARY) $(GTK3_LIBS) $(shell pkg-config --libs xtst x11) $(SANITIZERS) -o $@

$(BUILD_DIR)/tests/test_gtk4_ime_x11: tests/gtk/test_ibus_widget.c $(GTK4_LIBRARY) $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(FRONTEND_CPPFLAGS) $(POSIX_CPPFLAGS) $(GTK4_CPPFLAGS) $(shell pkg-config --cflags xtst x11) $(CFLAGS) $(GTK4_PROJECT_CFLAGS) $(SANITIZERS) -DBOXRIG_TEST_IME_X11=1 $< $(GTK4_LIBRARY) $(UI_LIBRARY) $(LIBRARY) $(GTK4_LIBS) $(shell pkg-config --libs xtst x11) $(SANITIZERS) -Wl,--wrap=gtk_im_multicontext_new,--wrap=gtk_event_controller_key_new,--wrap=gtk_event_controller_focus_new -o $@

.PHONY: test-gtk-ime-x11
test-gtk-ime-x11: $(BUILD_DIR)/tests/test_gtk3_ime_x11 $(BUILD_DIR)/tests/test_gtk4_ime_x11 $(BUILD_DIR)/tests/wayland_ibus_engine
	@for gtk in 4 3; do for scale in 1 2; do for renderer in cairo opengl; do \
		for module in auto ibus direct; do \
			xvfb-run -a -s '-screen 0 1600x1200x24' env GDK_BACKEND=x11 GDK_SCALE=$$scale GDK_DEBUG=gl-prefer-gl GSK_RENDERER=cairo GTK_USE_PORTAL=0 GTK_A11Y=none LIBGL_ALWAYS_SOFTWARE=1 ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 dbus-run-session -- node tests/wayland/run_ibus_context.mjs "$(BUILD_DIR)/tests/test_gtk$${gtk}_ime_x11" "$(TEST_FONT_FILE)" "$(BUILD_DIR)/tests/wayland_ibus_engine" "$(BUILD_DIR)/test-results/gtk$${gtk}-ibus/x11-$$scale-$$renderer-$$module" "$$renderer" "$$module" || exit $$?; \
		done; \
	done; done; done

# Client candidates are rendered inside the terminal viewport.
$(BUILD_DIR)/tests/test_gtk3_candidates: tests/gtk/test_gtk3_candidates.c $(GTK3_LIBRARY) $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(FRONTEND_CPPFLAGS) $(POSIX_CPPFLAGS) $(GTK3_CPPFLAGS) $(CFLAGS) $(GTK3_PROJECT_CFLAGS) $(SANITIZERS) $< $(GTK3_LIBRARY) $(UI_LIBRARY) $(LIBRARY) $(GTK3_LIBS) $(SANITIZERS) -o $@

.PHONY: test-gtk3-candidates
test-gtk3-candidates: $(BUILD_DIR)/tests/test_gtk3_candidates $(BUILD_DIR)/tests/wayland_ibus_engine
	xvfb-run -a env GDK_BACKEND=x11 GDK_SCALE=1 ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 dbus-run-session -- node tests/wayland/run_ibus_context.mjs "$(BUILD_DIR)/tests/test_gtk3_candidates" "$(TEST_FONT_FILE)" "$(BUILD_DIR)/tests/wayland_ibus_engine" "$(BUILD_DIR)/test-results/gtk3-candidates" "$(BUILD_DIR)/test-results/gtk3-candidates"

$(BUILD_DIR)/tests/test_ui_candidates: tests/test_ui_candidates.c src/ui/candidates_private.h $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) $< $(UI_LIBRARY) $(LIBRARY) $(SANITIZERS) -o $@

.PHONY: test-ui-candidates
test-ui-candidates: $(BUILD_DIR)/tests/test_ui_candidates
	env ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $<

$(BUILD_DIR)/tests/test_pangocairo_candidates: tests/pangocairo/test_candidates.c $(PANGO_CANDIDATE_OBJECT) $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(PANGO_CAIRO_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) $< $(PANGO_CANDIDATE_OBJECT) $(UI_LIBRARY) $(LIBRARY) $(PANGO_CAIRO_LIBS) $(SANITIZERS) -o $@

.PHONY: test-pangocairo-candidates
test-pangocairo-candidates: $(BUILD_DIR)/tests/test_pangocairo_candidates
	mkdir -p $(BUILD_DIR)/test-results/pangocairo-candidates
	env ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $< $(BUILD_DIR)/test-results/pangocairo-candidates/bounded.png

test: test-ui-candidates test-pangocairo-candidates

$(PANGO_CANDIDATE_OBJECT): src/pangocairo/candidates.c src/pangocairo/candidates.h src/ui/candidates_private.h $(UI_HEADERS) | $(BUILD_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(POSIX_CPPFLAGS) $(PANGO_CAIRO_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) -c $< -o $@

$(UI_OBJECTS) $(MINGW_AMD64_UI_OBJECTS) $(MINGW_I686_UI_OBJECTS): src/ui/candidates_private.h
$(BUILD_DIR)/gtk3/pangocairo_renderer.o $(BUILD_DIR)/gtk4/pangocairo_renderer.o $(BUILD_DIR)/wayland/pangocairo_renderer.o: src/pangocairo/candidates.h
$(BUILD_DIR)/opengl/text_provider.o: src/pangocairo/candidates.h src/ui/candidates_private.h

$(BUILD_DIR)/tests/test_opengl_candidates: tests/test_opengl_candidates.c tests/opengl_host.c tests/opengl_host.h $(OPENGL_RENDERER_LIBRARY) $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) -Isrc -Itests $(POSIX_CPPFLAGS) $(OPENGL_RENDERER_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) $< tests/opengl_host.c $(OPENGL_RENDERER_LIBRARY) $(UI_LIBRARY) $(LIBRARY) $(OPENGL_RENDERER_LIBS) -o $@

.PHONY: test-opengl-candidates
test-opengl-candidates: $(BUILD_DIR)/tests/test_opengl_candidates
	env LIBGL_ALWAYS_SOFTWARE=1 ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $<

$(BUILD_DIR)/tests/test_gtk4_candidates: tests/gtk/test_gtk4_candidates.c $(GTK4_LIBRARY) $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(FRONTEND_CPPFLAGS) $(POSIX_CPPFLAGS) $(GTK4_CPPFLAGS) $(CFLAGS) $(GTK4_PROJECT_CFLAGS) $(SANITIZERS) $< $(GTK4_LIBRARY) $(UI_LIBRARY) $(LIBRARY) $(GTK4_LIBS) $(SANITIZERS) -Wl,--wrap=gtk_event_controller_key_new,--wrap=gtk_event_controller_focus_new,--wrap=gtk_gesture_click_new,--wrap=gtk_event_controller_motion_new,--wrap=gtk_event_controller_scroll_new -o $@

.PHONY: test-gtk4-candidates
test-gtk4-candidates: $(BUILD_DIR)/tests/test_gtk4_candidates $(BUILD_DIR)/tests/wayland_ibus_engine
	xvfb-run -a env BOXRIG_TEST_WESTON_BACKEND=x11 BOXRIG_TEST_IBUS_WAYLAND=1 GDK_SCALE=1 GTK_USE_PORTAL=0 GTK_A11Y=none dbus-run-session -- bash tests/gtk/run_gl_wayland.sh node tests/wayland/run_ibus_context.mjs "$(BUILD_DIR)/tests/test_gtk4_candidates" "$(TEST_FONT_FILE)" "$(BUILD_DIR)/tests/wayland_ibus_engine" "$(BUILD_DIR)/test-results/gtk4-candidates" "$(BUILD_DIR)/test-results/gtk4-candidates"

.PHONY: test-wayland-candidates-startup
test-wayland-candidates-startup: $(WAYLAND_EXAMPLE_BINARY) $(BUILD_DIR)/tests/wayland_ibus_engine
	$(XVFB_RUN) -a env BOXRIG_TEST_WESTON_BACKEND=x11 BOXRIG_TEST_IBUS_WAYLAND=1 GIO_USE_VFS=local GTK_A11Y=none dbus-run-session --config-file=tests/ime/session.conf -- bash tests/gtk/run_gl_wayland.sh node tests/wayland/run_ibus_context.mjs "$(shell command -v node)" tests/wayland/test_sample_candidates.mjs "$(BUILD_DIR)/tests/wayland_ibus_engine" "$(BUILD_DIR)/test-results/wayland-candidates-startup" "$(WAYLAND_EXAMPLE_BINARY)"

.PHONY: test-notification-dispatch
test: test-notification-dispatch
test-notification-dispatch: $(BUILD_DIR)/tests/test_notification_dispatch
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $<

$(BUILD_DIR)/tests/test_notification_dispatch: tests/test_notification_dispatch.c $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) $< $(UI_LIBRARY) $(LIBRARY) -Wl,--wrap=boxrig_terminal_complete_host_request -o $@

.PHONY: test-win32-notifications
test-win32-notifications: $(MINGW_AMD64_HOST_TEST) $(MINGW_I686_HOST_TEST) $(MINGW_AMD64_SAMPLE_TEST) $(MINGW_I686_SAMPLE_TEST) $(MINGW_AMD64_NOTIFICATION_EXAMPLE) $(MINGW_I686_NOTIFICATION_EXAMPLE) $(MINGW_AMD64_PTY_HELPER) $(MINGW_I686_PTY_HELPER)
	$(WINE_RUN) $(abspath $(MINGW_AMD64_HOST_TEST))
	$(WINE_RUN) $(abspath $(MINGW_I686_HOST_TEST))
	$(WINE_RUN) $(abspath $(MINGW_AMD64_SAMPLE_TEST)) --notifications $(abspath $(MINGW_AMD64_NOTIFICATION_EXAMPLE)) $(abspath $(MINGW_AMD64_PTY_HELPER))
	$(WINE_RUN) $(abspath $(MINGW_I686_SAMPLE_TEST)) --notifications $(abspath $(MINGW_I686_NOTIFICATION_EXAMPLE)) $(abspath $(MINGW_I686_PTY_HELPER))

$(BUILD_DIR)/notification:
	$(INSTALL) -d $@

$(BUILD_DIR)/notification/%.o: src/notification/%.c $(NOTIFICATION_HEADERS) $(UI_HEADERS) Makefile | $(BUILD_DIR)/notification
	$(CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(NOTIFICATION_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) -c $< -o $@

$(NOTIFICATION_LIBRARY): $(NOTIFICATION_OBJECTS)
	$(AR) rcs $@ $^

.PHONY: test-freedesktop-notifications
test: test-freedesktop-notifications
test-freedesktop-notifications: $(BUILD_DIR)/tests/test_freedesktop_notifications
	dbus-run-session -- env ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $<

$(BUILD_DIR)/tests/test_freedesktop_notifications: tests/test_freedesktop_notifications.c $(BUILD_DIR)/notification/freedesktop.o $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(DBUS_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) $^ $(NOTIFICATION_LIBS) $(DBUS_LIBS) -Wl,--wrap=boxrig_terminal_complete_host_request -o $@

.PHONY: test-notification-samples install-notification
test: test-notification-samples
test-notification-samples: $(GTK3_EXAMPLE_BINARY) $(GTK4_EXAMPLE_BINARY) $(WAYLAND_EXAMPLE_BINARY) $(X11_FAKE_NOTIFICATION_SERVICE)
	dbus-run-session --config-file=tests/notification/session.conf -- $(XVFB_RUN) -a env NO_AT_BRIDGE=1 GTK_A11Y=none GDK_BACKEND=x11 tests/notification/sample.sh gtk3 cairo
	dbus-run-session --config-file=tests/notification/session.conf -- $(XVFB_RUN) -a env NO_AT_BRIDGE=1 GTK_A11Y=none GDK_BACKEND=x11 tests/notification/sample.sh gtk4 cairo
	dbus-run-session --config-file=tests/notification/session.conf -- tests/wayland/run_weston_test.sh $(WESTON) tests/notification/sample.sh
	dbus-run-session --config-file=tests/notification/session.conf -- env NO_AT_BRIDGE=1 GTK_A11Y=none GDK_BACKEND=wayland BOXRIG_TEST_NOTIFICATION_FRONTEND=gtk3 BOXRIG_TEST_NOTIFICATION_RENDERER=cairo tests/wayland/run_weston_test.sh $(WESTON) tests/notification/sample.sh
	dbus-run-session --config-file=tests/notification/session.conf -- env NO_AT_BRIDGE=1 GTK_A11Y=none GDK_BACKEND=wayland BOXRIG_TEST_NOTIFICATION_FRONTEND=gtk4 BOXRIG_TEST_NOTIFICATION_RENDERER=cairo tests/wayland/run_weston_test.sh $(WESTON) tests/notification/sample.sh

install-notification: $(NOTIFICATION_LIBRARY) $(UI_LIBRARY) $(LIBRARY) $(NOTIFICATION_HEADERS) pkgconfig/boxrig-notification.pc install-common-licenses
	$(INSTALL) -d $(DESTDIR)$(LIBDIR) $(DESTDIR)$(INCLUDEDIR)/boxrig/notification $(DESTDIR)$(INCLUDEDIR)/boxrig/ui $(DESTDIR)$(PKGCONFIGDIR)
	$(INSTALL) -m 644 $(NOTIFICATION_LIBRARY) $(UI_LIBRARY) $(LIBRARY) $(DESTDIR)$(LIBDIR)
	$(INSTALL) -m 644 $(CORE_PUBLIC_HEADERS) include/boxrig/ui.h $(DESTDIR)$(INCLUDEDIR)/boxrig
	$(INSTALL) -m 644 $(NOTIFICATION_HEADERS) $(DESTDIR)$(INCLUDEDIR)/boxrig/notification
	$(INSTALL) -m 644 include/boxrig/ui/*.h $(DESTDIR)$(INCLUDEDIR)/boxrig/ui
	$(INSTALL) -m 644 pkgconfig/boxrig-notification.pc $(DESTDIR)$(PKGCONFIGDIR)

.PHONY: test-background-layers
test: test-background-layers
test-background-layers: $(BUILD_DIR)/tests/test_background_layers $(MINGW_AMD64_RENDERER_TEST) $(MINGW_I686_RENDERER_TEST)
	$(XVFB_RUN) -a env LIBGL_ALWAYS_SOFTWARE=1 ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $<
	$(WINE_RUN) $(abspath $(MINGW_AMD64_RENDERER_TEST)) --background
	$(WINE_RUN) $(abspath $(MINGW_I686_RENDERER_TEST)) --background

$(BUILD_DIR)/tests/test_background_layers: tests/test_background_layers.c tests/opengl_host.c src/pangocairo/renderer.c $(PANGO_CANDIDATE_OBJECT) $(OPENGL_RENDERER_LIBRARY) $(X11_LIBRARY) $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) -Isrc -Itests $(POSIX_CPPFLAGS) $(PANGO_CAIRO_CPPFLAGS) $(OPENGL_RENDERER_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) $^ $(OPENGL_RENDERER_LIBS) $(X11_LIBS) $(PANGO_CAIRO_LIBS) -lm -o $@

$(EXAMPLE_BINARY) $(GTK3_EXAMPLE_BINARY) $(GTK4_EXAMPLE_BINARY) $(WAYLAND_EXAMPLE_BINARY) $(MINGW_AMD64_EXAMPLE) $(MINGW_I686_EXAMPLE): examples/boxrig_term/common/background_demo.h

.PHONY: test-win32-background-effect
test: test-win32-background-effect
test-win32-background-effect: $(MINGW_AMD64_BUILD_DIR)/tests/test_background_effect.exe $(MINGW_I686_BUILD_DIR)/tests/test_background_effect.exe
	$(WINE_RUN) $(abspath $(MINGW_AMD64_BUILD_DIR)/tests/test_background_effect.exe)
	$(WINE_RUN) $(abspath $(MINGW_I686_BUILD_DIR)/tests/test_background_effect.exe)
test-p2-win32: $(MINGW_AMD64_BUILD_DIR)/tests/test_background_effect.exe $(MINGW_I686_BUILD_DIR)/tests/test_background_effect.exe

$(MINGW_AMD64_BUILD_DIR)/tests/background_effect_scene.exe: tests/win32/background_effect_scene.c | $(MINGW_AMD64_BUILD_DIR)/tests
	$(MINGW_AMD64_CC) $(CFLAGS) $(PROJECT_CFLAGS) $< -lgdi32 -luser32 -o $@
$(BUILD_DIR)/boxrig_term/win32/amd64/boxrig_term_background_probe.exe: tests/win32/background_effect_probe.c examples/boxrig_term/win32/application_internal.h $(UI_HEADERS) $(WIN32_HEADERS) $(WIN32_EXAMPLE_SOURCES) $(WIN32_TERM_AMD64_RENDERER_LIBRARIES) $(MINGW_AMD64_EXAMPLE_RESOURCE) $(MINGW_AMD64_WIN32_LIBRARY) $(MINGW_AMD64_SESSION_LIBRARY) $(MINGW_AMD64_UI_LIBRARY) $(MINGW_AMD64_LIBRARY) $(CONPTY_HEADER) | $(BUILD_DIR)/boxrig_term/win32/amd64
	$(MINGW_AMD64_CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(MINGW_TERM_CPPFLAGS) $(WIN32_TERM_RENDERER_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(WIN32_EXAMPLE_SOURCES) $< $(MINGW_AMD64_EXAMPLE_RESOURCE) $(WIN32_TERM_AMD64_RENDERER_LIBRARIES) $(MINGW_AMD64_WIN32_LIBRARY) $(MINGW_AMD64_SESSION_LIBRARY) $(MINGW_AMD64_UI_LIBRARY) $(MINGW_AMD64_LIBRARY) -municode -mwindows $(WIN32_RENDERER_LIBS) $(WIN32_HOST_LIBS) $(WIN32_TRANSPARENCY_LIBS) $(WIN32_TERM_RENDERER_LIBS) -ladvapi32 -lcomctl32 -Wl,--wrap=boxrig_win32_terminal_component_set_key_router -o $@
test-p2-win32: $(MINGW_AMD64_BUILD_DIR)/tests/background_effect_scene.exe $(BUILD_DIR)/boxrig_term/win32/amd64/boxrig_term_background_probe.exe

$(MINGW_AMD64_BUILD_DIR)/tests/test_background_effect.exe: tests/win32/test_background_effect.c $(MINGW_AMD64_WIN32_LIBRARY) | $(MINGW_AMD64_BUILD_DIR)/tests
	$(MINGW_AMD64_CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) -Isrc/win32 $(WIN32_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $< $(MINGW_AMD64_WIN32_LIBRARY) -luser32 -o $@

$(MINGW_I686_BUILD_DIR)/tests/test_background_effect.exe: tests/win32/test_background_effect.c $(MINGW_I686_WIN32_LIBRARY) | $(MINGW_I686_BUILD_DIR)/tests
	$(MINGW_I686_CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) -Isrc/win32 $(WIN32_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $< $(MINGW_I686_WIN32_LIBRARY) -luser32 -o $@

.PHONY: test-x11-background-effect
test: test-x11-background-effect
test-x11-background-effect: $(BUILD_DIR)/tests/test_x11_background_effect
	$(XVFB_RUN) -a env ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $<

$(BUILD_DIR)/tests/test_x11_background_effect: tests/test_x11_background_effect.c $(X11_LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) $< $(X11_LIBRARY) -lX11 -lXfixes -o $@

BACKGROUND_WAYLAND_HEADER := $(WAYLAND_PROTOCOL_DIR)/background-effect-v1-client-protocol.h
BACKGROUND_WAYLAND_SERVER_HEADER := $(WAYLAND_PROTOCOL_DIR)/background-effect-v1-server-protocol.h
BACKGROUND_WAYLAND_SOURCE := $(WAYLAND_PROTOCOL_DIR)/background-effect-v1-protocol.c
BACKGROUND_WAYLAND_LIBRARY := $(BUILD_DIR)/libboxrig-background-wayland.a
$(BACKGROUND_WAYLAND_HEADER): $(BACKGROUND_WAYLAND_XML) | $(WAYLAND_PROTOCOL_DIR)
	$(WAYLAND_SCANNER) client-header $< $@
$(BACKGROUND_WAYLAND_SERVER_HEADER): $(BACKGROUND_WAYLAND_XML) | $(WAYLAND_PROTOCOL_DIR)
	$(WAYLAND_SCANNER) server-header $< $@
$(BACKGROUND_WAYLAND_SOURCE): $(BACKGROUND_WAYLAND_XML) | $(WAYLAND_PROTOCOL_DIR)
	$(WAYLAND_SCANNER) private-code $< $@
$(BUILD_DIR)/background_effect:
	mkdir -p $@
$(BUILD_DIR)/background_effect/wayland.o: src/background_effect/wayland.c include/boxrig/wayland/background_effect.h include/boxrig/ui/background_effect.h $(BACKGROUND_WAYLAND_HEADER) | $(BUILD_DIR)/background_effect
	$(CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) -I$(WAYLAND_PROTOCOL_DIR) $(shell pkg-config --cflags wayland-client) $(CFLAGS) $(PROJECT_CFLAGS) -c $< -o $@
$(BUILD_DIR)/background_effect/protocol.o: $(BACKGROUND_WAYLAND_SOURCE) | $(BUILD_DIR)/background_effect
	$(CC) $(CPPFLAGS) $(shell pkg-config --cflags wayland-client) $(CFLAGS) $(PROJECT_CFLAGS) -c $< -o $@
$(BACKGROUND_WAYLAND_LIBRARY): $(BUILD_DIR)/background_effect/wayland.o $(BUILD_DIR)/background_effect/protocol.o
	$(AR) rcs $@ $^
.PHONY: test-wayland-background-effect
test: test-wayland-background-effect
test-wayland-background-effect: $(BUILD_DIR)/tests/test_wayland_background_effect
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $<
$(BUILD_DIR)/tests/test_wayland_background_effect: tests/wayland/test_background_effect.c $(BACKGROUND_WAYLAND_SERVER_HEADER) $(BACKGROUND_WAYLAND_LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) -I$(WAYLAND_PROTOCOL_DIR) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) $< $(BACKGROUND_WAYLAND_LIBRARY) $(shell pkg-config --libs wayland-client wayland-server) -o $@

BACKGROUND_GTK3_LIBRARY := $(BUILD_DIR)/libboxrig-background-gtk3.a
BACKGROUND_GTK4_LIBRARY := $(BUILD_DIR)/libboxrig-background-gtk4.a
BACKGROUND_GTK4_PKG_CONFIG := $(if $(shell pkg-config --atleast-version=4.18 gtk4 && echo supported),pkgconfig/boxrig-background-gtk4.pc,pkgconfig/gtk4-legacy/boxrig-background-gtk4.pc)
$(BUILD_DIR)/background_effect/gtk3.o: src/background_effect/gtk.c include/boxrig/gtk3/background_effect.h include/boxrig/wayland/background_effect.h include/boxrig/x11/background_effect.h | $(BUILD_DIR)/background_effect
	$(CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(GTK3_CPPFLAGS) $(shell pkg-config --cflags wayland-client) $(CFLAGS) $(GTK3_PROJECT_CFLAGS) -Werror=deprecated-declarations -DBOXRIG_BACKGROUND_GTK3=1 -c $< -o $@
$(BUILD_DIR)/background_effect/gtk4.o: src/background_effect/gtk.c include/boxrig/gtk4/background_effect.h include/boxrig/wayland/background_effect.h | $(BUILD_DIR)/background_effect
	$(CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(GTK4_CPPFLAGS) $(shell pkg-config --cflags wayland-client) $(CFLAGS) $(GTK4_PROJECT_CFLAGS) -c $< -o $@
$(BACKGROUND_GTK3_LIBRARY): $(BUILD_DIR)/background_effect/gtk3.o $(BUILD_DIR)/x11/background_effect.o
	$(AR) rcs $@ $^
$(BACKGROUND_GTK4_LIBRARY): $(BUILD_DIR)/background_effect/gtk4.o
	$(AR) rcs $@ $^
.PHONY: test-gtk-background-effect
test: test-gtk-background-effect
test-gtk-background-effect: $(BUILD_DIR)/tests/test_gtk3_background_effect $(BUILD_DIR)/tests/test_gtk4_background_effect
	GDK_BACKEND=wayland GTK_A11Y=none NO_AT_BRIDGE=1 ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 $<
	GDK_BACKEND=wayland GSK_RENDERER=cairo GTK_A11Y=none NO_AT_BRIDGE=1 ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 $(BUILD_DIR)/tests/test_gtk4_background_effect
$(BUILD_DIR)/tests/test_gtk3_background_effect: tests/gtk/test_background_effect.c tests/gtk/ime_server.c tests/gtk/ime_server.h $(GTK_IME_XDG_SERVER_HEADER) $(WAYLAND_TEXT_INPUT_SERVER_HEADER) $(WAYLAND_TEXT_INPUT_SOURCE) $(WAYLAND_XDG_SHELL_SOURCE) $(BACKGROUND_WAYLAND_SERVER_HEADER) $(BACKGROUND_WAYLAND_LIBRARY) $(BACKGROUND_GTK3_LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(POSIX_CPPFLAGS) $(GTK3_CPPFLAGS) $(WAYLAND_CPPFLAGS) -I$(WAYLAND_PROTOCOL_DIR) $(CFLAGS) $(GTK3_PROJECT_CFLAGS) $(SANITIZERS) -DBOXRIG_TEST_GTK3=1 -DBOXRIG_TEST_BACKGROUND_EFFECT=1 $< tests/gtk/ime_server.c $(WAYLAND_TEXT_INPUT_SOURCE) $(WAYLAND_XDG_SHELL_SOURCE) $(BACKGROUND_GTK3_LIBRARY) $(BACKGROUND_WAYLAND_LIBRARY) $(GTK3_LIBS) $(WAYLAND_LIBS) -lX11 -lXfixes -o $@
$(BUILD_DIR)/tests/test_gtk4_background_effect: tests/gtk/test_background_effect.c tests/gtk/ime_server.c tests/gtk/ime_server.h $(GTK_IME_XDG_SERVER_HEADER) $(WAYLAND_TEXT_INPUT_SERVER_HEADER) $(WAYLAND_TEXT_INPUT_SOURCE) $(WAYLAND_XDG_SHELL_SOURCE) $(BACKGROUND_WAYLAND_SERVER_HEADER) $(BACKGROUND_WAYLAND_LIBRARY) $(BACKGROUND_GTK4_LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(POSIX_CPPFLAGS) $(GTK4_CPPFLAGS) $(WAYLAND_CPPFLAGS) -I$(WAYLAND_PROTOCOL_DIR) $(CFLAGS) $(GTK4_PROJECT_CFLAGS) $(SANITIZERS) -DBOXRIG_TEST_BACKGROUND_EFFECT=1 $< tests/gtk/ime_server.c $(WAYLAND_TEXT_INPUT_SOURCE) $(WAYLAND_XDG_SHELL_SOURCE) $(BACKGROUND_GTK4_LIBRARY) $(BACKGROUND_WAYLAND_LIBRARY) $(GTK4_LIBS) $(WAYLAND_LIBS) -o $@

.PHONY: gtk-background-effects
gtk-background-effects: $(BUILD_DIR)/boxrig_term/gtk3/boxrig_term_effects $(BUILD_DIR)/boxrig_term/gtk4/boxrig_term_effects
$(BUILD_DIR)/boxrig_term/gtk3/boxrig_term_effects: $(GTK3_EXAMPLE_SOURCES) $(GTK3_LIBRARY) $(BACKGROUND_GTK3_LIBRARY) $(BACKGROUND_WAYLAND_LIBRARY) $(NOTIFICATION_LIBRARY) $(POSIX_LIBRARY) $(SESSION_LIBRARY) $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/boxrig_term/gtk3
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(POSIX_URI_CPPFLAGS) $(POSIX_CPPFLAGS) $(GTK3_CPPFLAGS) $(CFLAGS) $(GTK3_PROJECT_CFLAGS) -DBOXRIG_TERM_ENABLE_BACKGROUND_EFFECT=1 $(GTK3_EXAMPLE_SOURCES) $(GTK3_LIBRARY) $(BACKGROUND_GTK3_LIBRARY) $(BACKGROUND_WAYLAND_LIBRARY) $(NOTIFICATION_LIBRARY) $(POSIX_LIBRARY) $(SESSION_LIBRARY) $(UI_LIBRARY) $(LIBRARY) $(GTK3_LIBS) -lX11 -lXfixes -lwayland-client -lutil $(POSIX_URI_LIBS) -o $@
$(BUILD_DIR)/boxrig_term/gtk4/boxrig_term_effects: $(GTK4_EXAMPLE_SOURCES) $(GTK4_LIBRARY) $(BACKGROUND_GTK4_LIBRARY) $(BACKGROUND_WAYLAND_LIBRARY) $(NOTIFICATION_LIBRARY) $(POSIX_LIBRARY) $(SESSION_LIBRARY) $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/boxrig_term/gtk4
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(POSIX_URI_CPPFLAGS) $(POSIX_CPPFLAGS) $(GTK4_CPPFLAGS) $(CFLAGS) $(GTK4_PROJECT_CFLAGS) -DBOXRIG_TERM_ENABLE_BACKGROUND_EFFECT=1 $(GTK4_EXAMPLE_SOURCES) $(GTK4_LIBRARY) $(BACKGROUND_GTK4_LIBRARY) $(BACKGROUND_WAYLAND_LIBRARY) $(NOTIFICATION_LIBRARY) $(POSIX_LIBRARY) $(SESSION_LIBRARY) $(UI_LIBRARY) $(LIBRARY) $(GTK4_LIBS) -lwayland-client -lutil $(POSIX_URI_LIBS) -o $@

.PHONY: test-gtk3-background-effect-x11
test: test-gtk3-background-effect-x11
test-gtk3-background-effect-x11: $(BUILD_DIR)/tests/test_gtk3_background_effect_x11
	xvfb-run -a env GDK_BACKEND=x11 GDK_SCALE=1 NO_AT_BRIDGE=1 ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $<
	xvfb-run -a env GDK_BACKEND=x11 GDK_SCALE=2 NO_AT_BRIDGE=1 ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $<
$(BUILD_DIR)/tests/test_gtk3_background_effect_x11: tests/gtk/test_background_effect_x11.c $(BACKGROUND_GTK3_LIBRARY) $(BACKGROUND_WAYLAND_LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(GTK3_CPPFLAGS) $(CFLAGS) $(GTK3_PROJECT_CFLAGS) -Werror=deprecated-declarations $(SANITIZERS) $< $(BACKGROUND_GTK3_LIBRARY) $(BACKGROUND_WAYLAND_LIBRARY) $(GTK3_LIBS) -lwayland-client -lX11 -lXfixes -o $@

.PHONY: test-background-effect-packages
test: test-background-effect-packages
test-background-effect-packages:
	sh tests/gtk/check_background_packages.sh $(abspath $(BUILD_DIR)/tests/background-effect-package-root) "$(CC)"

.PHONY: test-notification-package
test: test-notification-package
test-notification-package:
	sh tests/notification/check_package.sh $(abspath $(BUILD_DIR)/tests/notification-package-root) "$(CC)"

.PHONY: background-effect-drivers
background-effect-drivers: $(BUILD_DIR)/tests/background_effect_scene $(BUILD_DIR)/tests/background_effect_x11 $(BUILD_DIR)/tests/background_effect_gtk3 $(BUILD_DIR)/tests/background_effect_gtk4 $(BUILD_DIR)/tests/background_effect_wayland $(BUILD_DIR)/tests/kwin_background_capture
$(BUILD_DIR)/tests/kwin_background_capture: tests/wayland/kwin_background_capture.c | $(BUILD_DIR)/tests
	$(CC) $(CFLAGS) $(PROJECT_CFLAGS) $< $(shell pkg-config --cflags --libs gio-2.0 gio-unix-2.0) -o $@
$(BUILD_DIR)/tests/background_effect_scene: tests/x11/background_effect_scene.c | $(BUILD_DIR)/tests
	$(CC) $(CFLAGS) $(PROJECT_CFLAGS) $< -lX11 -o $@
$(BUILD_DIR)/tests/background_effect_x11: tests/background_effect_probe.c $(EXAMPLE_SOURCES) $(UI_HEADERS) $(X11_HEADERS) examples/boxrig_term/x11/application_internal.h $(X11_OPENGL_LIBRARY) $(X11_LIBRARY) $(OPENGL_RENDERER_LIBRARY) $(POSIX_LIBRARY) $(SESSION_LIBRARY) $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(POSIX_URI_CPPFLAGS) $(POSIX_CPPFLAGS) $(X11_CPPFLAGS) $(XEXT_CPPFLAGS) $(DBUS_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(EXAMPLE_SOURCES) $< $(X11_OPENGL_LIBRARY) $(X11_LIBRARY) $(OPENGL_RENDERER_LIBRARY) $(POSIX_LIBRARY) $(SESSION_LIBRARY) $(UI_LIBRARY) $(LIBRARY) $(X11_LIBS) $(XEXT_LIBS) $(DBUS_LIBS) $(OPENGL_RENDERER_LIBS) -lutil $(POSIX_URI_LIBS) -Wl,--wrap=boxrig_x11_terminal_component_set_key_router -o $@
$(BUILD_DIR)/tests/background_effect_gtk3: tests/background_effect_probe.c $(GTK3_EXAMPLE_SOURCES) examples/boxrig_term/gtk3/application_internal.h $(GTK3_LIBRARY) $(BACKGROUND_GTK3_LIBRARY) $(BACKGROUND_WAYLAND_LIBRARY) $(NOTIFICATION_LIBRARY) $(POSIX_LIBRARY) $(SESSION_LIBRARY) $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(POSIX_URI_CPPFLAGS) $(POSIX_CPPFLAGS) $(GTK3_CPPFLAGS) $(CFLAGS) $(GTK3_PROJECT_CFLAGS) -DBOXRIG_TERM_ENABLE_BACKGROUND_EFFECT=1 -DBOXRIG_PROBE_GTK3=1 $(GTK3_EXAMPLE_SOURCES) $< $(GTK3_LIBRARY) $(BACKGROUND_GTK3_LIBRARY) $(BACKGROUND_WAYLAND_LIBRARY) $(NOTIFICATION_LIBRARY) $(POSIX_LIBRARY) $(SESSION_LIBRARY) $(UI_LIBRARY) $(LIBRARY) $(GTK3_LIBS) -lX11 -lXfixes -lwayland-client -lutil $(POSIX_URI_LIBS) -Wl,--wrap=boxrig_gtk3_terminal_widget_set_key_router -o $@
$(BUILD_DIR)/tests/background_effect_gtk4: tests/background_effect_probe.c $(GTK4_EXAMPLE_SOURCES) examples/boxrig_term/gtk4/application_internal.h $(GTK4_LIBRARY) $(BACKGROUND_GTK4_LIBRARY) $(BACKGROUND_WAYLAND_LIBRARY) $(NOTIFICATION_LIBRARY) $(POSIX_LIBRARY) $(SESSION_LIBRARY) $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(POSIX_URI_CPPFLAGS) $(POSIX_CPPFLAGS) $(GTK4_CPPFLAGS) $(CFLAGS) $(GTK4_PROJECT_CFLAGS) -DBOXRIG_TERM_ENABLE_BACKGROUND_EFFECT=1 -DBOXRIG_PROBE_GTK4=1 $(GTK4_EXAMPLE_SOURCES) $< $(GTK4_LIBRARY) $(BACKGROUND_GTK4_LIBRARY) $(BACKGROUND_WAYLAND_LIBRARY) $(NOTIFICATION_LIBRARY) $(POSIX_LIBRARY) $(SESSION_LIBRARY) $(UI_LIBRARY) $(LIBRARY) $(GTK4_LIBS) -lwayland-client -lutil $(POSIX_URI_LIBS) -Wl,--wrap=boxrig_gtk4_terminal_widget_set_key_router -o $@
$(BUILD_DIR)/tests/background_effect_wayland: tests/background_effect_probe.c $(WAYLAND_EXAMPLE_SOURCES) examples/boxrig_term/wayland/application_internal.h $(WAYLAND_EXAMPLE_BINARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(POSIX_URI_CPPFLAGS) $(POSIX_CPPFLAGS) -I$(WAYLAND_PROTOCOL_DIR) $(WAYLAND_CLIENT_CPPFLAGS) $(WAYLAND_EXAMPLE_CURSOR_CPPFLAGS) $(WAYLAND_EXAMPLE_LIBDECOR_CPPFLAGS) $(PANGO_CAIRO_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) -DBOXRIG_PROBE_WAYLAND=1 $(WAYLAND_EXAMPLE_SOURCES) $< $(WAYLAND_XDG_SHELL_SOURCE) $(WAYLAND_XDG_DECORATION_SOURCE) $(WAYLAND_OPENGL_LIBRARY) $(WAYLAND_LIBRARY) $(NOTIFICATION_LIBRARY) $(OPENGL_RENDERER_LIBRARY) $(POSIX_LIBRARY) $(SESSION_LIBRARY) $(UI_LIBRARY) $(LIBRARY) $(WAYLAND_EXAMPLE_PRIVATE_ARCHIVE_LDFLAGS) $(WAYLAND_CLIENT_LIBS) $(WAYLAND_EXAMPLE_CURSOR_LIBS) $(WAYLAND_OPENGL_LIBS) $(PANGO_CAIRO_LIBS) $(WAYLAND_EXAMPLE_LIBDECOR_LIBS) -lutil $(POSIX_URI_LIBS) -Wl,--wrap=boxrig_wayland_terminal_component_set_key_router -o $@

.PHONY: install-background-wayland install-background-gtk3 install-background-gtk4
install-background-wayland: $(BACKGROUND_WAYLAND_LIBRARY) pkgconfig/boxrig-background-wayland.pc
	$(INSTALL) -d $(DESTDIR)$(LIBDIR) $(DESTDIR)$(INCLUDEDIR)/boxrig/wayland $(DESTDIR)$(INCLUDEDIR)/boxrig/ui $(DESTDIR)$(PKGCONFIGDIR)
	$(INSTALL) -m 644 $(BACKGROUND_WAYLAND_LIBRARY) $(DESTDIR)$(LIBDIR)
	$(INSTALL) -m 644 $(CORE_PUBLIC_HEADERS) include/boxrig/ui.h $(DESTDIR)$(INCLUDEDIR)/boxrig
	$(INSTALL) -m 644 include/boxrig/ui/*.h $(DESTDIR)$(INCLUDEDIR)/boxrig/ui
	$(INSTALL) -m 644 include/boxrig/wayland/background_effect.h $(DESTDIR)$(INCLUDEDIR)/boxrig/wayland
	$(INSTALL) -m 644 pkgconfig/boxrig-background-wayland.pc $(DESTDIR)$(PKGCONFIGDIR)
install-background-gtk3: install-background-wayland $(BACKGROUND_GTK3_LIBRARY) pkgconfig/boxrig-background-gtk3.pc
	$(INSTALL) -d $(DESTDIR)$(INCLUDEDIR)/boxrig/gtk3
	$(INSTALL) -m 644 $(BACKGROUND_GTK3_LIBRARY) $(DESTDIR)$(LIBDIR)
	$(INSTALL) -m 644 include/boxrig/gtk3/background_effect.h $(DESTDIR)$(INCLUDEDIR)/boxrig/gtk3
	$(INSTALL) -m 644 pkgconfig/boxrig-background-gtk3.pc $(DESTDIR)$(PKGCONFIGDIR)
install-background-gtk4: install-background-wayland $(BACKGROUND_GTK4_LIBRARY) $(BACKGROUND_GTK4_PKG_CONFIG)
	$(INSTALL) -d $(DESTDIR)$(INCLUDEDIR)/boxrig/gtk4
	$(INSTALL) -m 644 $(BACKGROUND_GTK4_LIBRARY) $(DESTDIR)$(LIBDIR)
	$(INSTALL) -m 644 include/boxrig/gtk4/background_effect.h $(DESTDIR)$(INCLUDEDIR)/boxrig/gtk4
	$(INSTALL) -m 644 $(BACKGROUND_GTK4_PKG_CONFIG) $(DESTDIR)$(PKGCONFIGDIR)

.PHONY: test-png-build
test: test-png-build
test-png-build:
	sh tests/check_png_build.sh "$(MAKE)"

.PHONY: test-png-isolation
test-png-isolation: $(BUILD_DIR)/tests/test_png_isolation
	$<

test: test-png-isolation

$(BUILD_DIR)/tests/test_png_isolation: tests/gtk/test_png_isolation.c $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(shell pkg-config --cflags gdk-pixbuf-2.0 gio-2.0) $(CFLAGS) $(PROJECT_CFLAGS) $< $(LIBRARY) $(shell pkg-config --libs gdk-pixbuf-2.0 gio-2.0) -o $@

include packaging/licenses/licenses.mk

.PHONY: test-packages
test: test-package-scripts
.PHONY: test-package-scripts
test-package-scripts:
	bash tests/check_package_scripts.sh

test: test-packages
test-packages:
	bash tests/check_packages.sh "$(CC)" "$(MINGW_AMD64_CC)" "$(MINGW_I686_CC)"
	$(WINE_RUN) $(abspath $(BUILD_DIR)/tests/package-consumers/amd64.exe)
	$(WINE_RUN) $(abspath $(BUILD_DIR)/tests/package-consumers/i686.exe)
	$(WINE_RUN) $(abspath $(BUILD_DIR)/tests/package-consumers/amd64-selection_history.exe)
	$(WINE_RUN) $(abspath $(BUILD_DIR)/tests/package-consumers/amd64-selection_layout_provider.exe)
	$(WINE_RUN) $(abspath $(BUILD_DIR)/tests/package-consumers/i686-selection_history.exe)
	$(WINE_RUN) $(abspath $(BUILD_DIR)/tests/package-consumers/i686-selection_layout_provider.exe)

include tests/headers/rules.mk
include packaging/linux/rules.mk

.PHONY: test-grid-resize test-grid-resize-win32
.PHONY: test-posix-spawn
.PHONY: test-async-spawn
.PHONY: example-pty-spawn example-pty-spawn-win32
example-pty-spawn: $(BUILD_DIR)/examples/pty_spawn
example-pty-spawn-win32: $(MINGW_AMD64_BUILD_DIR)/pty_spawn.exe $(MINGW_I686_BUILD_DIR)/pty_spawn.exe

$(BUILD_DIR)/examples/pty_spawn: examples/pty_spawn/main.cpp examples/pty_spawn/operation.cpp examples/pty_spawn/operation.h deps/cardio/include/cardio.h $(POSIX_LIBRARY) $(LIBRARY)
	mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(POSIX_CPPFLAGS) $(GTK3_CPPFLAGS) -Iexamples/pty_spawn -Ideps/cardio/include -DCARDIO_WITH_GLIB=1 $(CFLAGS) -std=c++20 $(WARNINGS) examples/pty_spawn/main.cpp examples/pty_spawn/operation.cpp $(POSIX_LIBRARY) $(LIBRARY) -lutil $(GTK3_LIBS) -pthread -o $@

$(MINGW_AMD64_BUILD_DIR)/pty_spawn.exe: examples/pty_spawn/main.cpp examples/pty_spawn/operation.cpp examples/pty_spawn/operation.h deps/cardio/include/cardio.h $(MINGW_AMD64_WIN32_LIBRARY)
	$(MINGW_AMD64_CXX) $(PUBLIC_CPPFLAGS) $(MINGW_WIN32_CPPFLAGS) -Iexamples/pty_spawn -Ideps/cardio/include $(CFLAGS) -std=c++20 $(WARNINGS) -static -municode examples/pty_spawn/main.cpp examples/pty_spawn/operation.cpp $(MINGW_AMD64_WIN32_LIBRARY) -lversion -luser32 -o $@

$(MINGW_I686_BUILD_DIR)/pty_spawn.exe: examples/pty_spawn/main.cpp examples/pty_spawn/operation.cpp examples/pty_spawn/operation.h deps/cardio/include/cardio.h $(MINGW_I686_WIN32_LIBRARY)
	$(MINGW_I686_CXX) $(PUBLIC_CPPFLAGS) $(MINGW_WIN32_CPPFLAGS) -Iexamples/pty_spawn -Ideps/cardio/include $(CFLAGS) -std=c++20 $(WARNINGS) -static -municode examples/pty_spawn/main.cpp examples/pty_spawn/operation.cpp $(MINGW_I686_WIN32_LIBRARY) -lversion -luser32 -o $@

test: test-async-spawn
test-async-spawn: $(BUILD_DIR)/tests/test_async_spawn $(BUILD_DIR)/examples/pty_spawn
	NO_AT_BRIDGE=1 GDK_BACKEND=x11 xvfb-run -a $<
	sh tests/check_spawn_example.sh $(BUILD_DIR)/examples/pty_spawn

ASYNC_SPAWN_SOURCE := examples/pty_spawn/operation.cpp
ASYNC_SPAWN_HEADERS := examples/pty_spawn/operation.h deps/cardio/include/cardio.h
ASYNC_SPAWN_POSIX_WRAP := -Wl,--wrap=boxrig_posix_pty_spawn_with_diagnostic,--wrap=boxrig_posix_pty_destroy
ASYNC_SPAWN_WIN32_WRAP := -Wl,--wrap=boxrig_win32_pty_spawn_with_config,--wrap=boxrig_win32_pty_close,--wrap=boxrig_win32_pty_destroy

$(BUILD_DIR)/tests/test_async_spawn: tests/test_async_spawn.cpp $(ASYNC_SPAWN_SOURCE) $(ASYNC_SPAWN_HEADERS) $(POSIX_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CXX) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(POSIX_CPPFLAGS) $(GTK3_CPPFLAGS) -Iexamples/pty_spawn -Ideps/cardio/include -DCARDIO_WITH_GLIB=1 $(CFLAGS) -std=c++20 $(WARNINGS) tests/test_async_spawn.cpp $(ASYNC_SPAWN_SOURCE) $(POSIX_LIBRARY) $(LIBRARY) -lutil $(GTK3_LIBS) -pthread $(ASYNC_SPAWN_POSIX_WRAP) -o $@

$(MINGW_AMD64_BUILD_DIR)/tests/test_async_spawn.exe: tests/test_async_spawn.cpp $(ASYNC_SPAWN_SOURCE) $(ASYNC_SPAWN_HEADERS) $(MINGW_AMD64_WIN32_LIBRARY) | $(MINGW_AMD64_BUILD_DIR)/tests
	$(MINGW_AMD64_CXX) $(PUBLIC_CPPFLAGS) $(MINGW_WIN32_CPPFLAGS) -Iexamples/pty_spawn -Ideps/cardio/include $(CFLAGS) -std=c++20 $(WARNINGS) -static tests/test_async_spawn.cpp $(ASYNC_SPAWN_SOURCE) $(MINGW_AMD64_WIN32_LIBRARY) -lversion -luser32 $(ASYNC_SPAWN_WIN32_WRAP) -o $@

$(MINGW_I686_BUILD_DIR)/tests/test_async_spawn.exe: tests/test_async_spawn.cpp $(ASYNC_SPAWN_SOURCE) $(ASYNC_SPAWN_HEADERS) $(MINGW_I686_WIN32_LIBRARY) | $(MINGW_I686_BUILD_DIR)/tests
	$(MINGW_I686_CXX) $(PUBLIC_CPPFLAGS) $(MINGW_WIN32_CPPFLAGS) -Iexamples/pty_spawn -Ideps/cardio/include $(CFLAGS) -std=c++20 $(WARNINGS) -static tests/test_async_spawn.cpp $(ASYNC_SPAWN_SOURCE) $(MINGW_I686_WIN32_LIBRARY) -lversion -luser32 $(ASYNC_SPAWN_WIN32_WRAP) -o $@

test-p2-win32: $(MINGW_AMD64_BUILD_DIR)/tests/test_async_spawn.exe $(MINGW_I686_BUILD_DIR)/tests/test_async_spawn.exe example-pty-spawn-win32

test: test-posix-spawn
test-posix-spawn: $(BUILD_DIR)/tests/test_posix_spawn
	$(BUILD_DIR)/tests/test_posix_spawn

$(BUILD_DIR)/tests/test_posix_spawn: tests/test_posix_spawn.c $(POSIX_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(POSIX_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $< $(POSIX_LIBRARY) $(LIBRARY) -lutil -Wl,--wrap=execve,--wrap=setenv,--wrap=setsid,--wrap=calloc,--wrap=malloc,--wrap=strdup,--wrap=openpty,--wrap=pipe2,--wrap=fork -o $@

test: test-grid-resize test-grid-resize-win32
test-grid-resize: $(BUILD_DIR)/tests/test_grid_resize
	$(BUILD_DIR)/tests/test_grid_resize

$(BUILD_DIR)/tests/test_grid_resize: tests/test_grid_resize.c $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $< $(UI_LIBRARY) $(LIBRARY) -o $@

test-grid-resize-win32: $(MINGW_AMD64_BUILD_DIR)/tests/test_grid_resize.exe $(MINGW_I686_BUILD_DIR)/tests/test_grid_resize.exe
	$(WINE_RUN) $(abspath $(MINGW_AMD64_BUILD_DIR)/tests/test_grid_resize.exe)
	$(WINE_RUN) $(abspath $(MINGW_I686_BUILD_DIR)/tests/test_grid_resize.exe)

$(MINGW_AMD64_BUILD_DIR)/tests/test_grid_resize.exe: tests/test_grid_resize.c $(MINGW_AMD64_UI_LIBRARY) $(MINGW_AMD64_LIBRARY) | $(MINGW_AMD64_BUILD_DIR)/tests
	$(MINGW_AMD64_CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $< $(MINGW_AMD64_UI_LIBRARY) $(MINGW_AMD64_LIBRARY) -o $@

$(MINGW_I686_BUILD_DIR)/tests/test_grid_resize.exe: tests/test_grid_resize.c $(MINGW_I686_UI_LIBRARY) $(MINGW_I686_LIBRARY) | $(MINGW_I686_BUILD_DIR)/tests
	$(MINGW_I686_CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $< $(MINGW_I686_UI_LIBRARY) $(MINGW_I686_LIBRARY) -o $@

.PHONY: test-input-config
test: test-input-config
test-input-config: $(BUILD_DIR)/tests/test_terminal_input_config
	$(BUILD_DIR)/tests/test_terminal_input_config

$(BUILD_DIR)/tests/test_terminal_input_config: tests/test_terminal_input_config.c $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $< $(LIBRARY) -Wl,--wrap=malloc -o $@

.PHONY: test-input-config-win32
test: test-input-config-win32
test-input-config-win32: $(MINGW_AMD64_BUILD_DIR)/tests/test_terminal_input_config.exe $(MINGW_I686_BUILD_DIR)/tests/test_terminal_input_config.exe
	$(WINE_RUN) $(abspath $(MINGW_AMD64_BUILD_DIR)/tests/test_terminal_input_config.exe)
	$(WINE_RUN) $(abspath $(MINGW_I686_BUILD_DIR)/tests/test_terminal_input_config.exe)

$(MINGW_AMD64_BUILD_DIR)/tests/test_terminal_input_config.exe: tests/test_terminal_input_config.c $(MINGW_AMD64_LIBRARY) | $(MINGW_AMD64_BUILD_DIR)/tests
	$(MINGW_AMD64_CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $< $(MINGW_AMD64_LIBRARY) -Wl,--wrap=malloc -o $@

$(MINGW_I686_BUILD_DIR)/tests/test_terminal_input_config.exe: tests/test_terminal_input_config.c $(MINGW_I686_LIBRARY) | $(MINGW_I686_BUILD_DIR)/tests
	$(MINGW_I686_CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $< $(MINGW_I686_LIBRARY) -Wl,--wrap=malloc -o $@

.PHONY: test-regex
test: test-regex
test-regex: $(BUILD_DIR)/tests/test_regex $(BUILD_DIR)/tests/test_regex_captures $(BUILD_DIR)/boxrig-regex
	$(BUILD_DIR)/tests/test_regex
	$(BUILD_DIR)/tests/test_regex_captures
	sh tests/check_regex_cli.sh $(BUILD_DIR)/boxrig-regex $(BUILD_DIR)/tests/regex-cli

$(BUILD_DIR)/tests/test_regex: tests/test_regex.c $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $< $(LIBRARY) -lm -o $@

$(BUILD_DIR)/tests/test_regex_captures: tests/test_regex_captures.c $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $< $(LIBRARY) -lm -o $@

.PHONY: boxrig-regex
native: boxrig-regex
boxrig-regex: $(BUILD_DIR)/boxrig-regex

$(BUILD_DIR)/boxrig-regex: examples/core/regex.c $(LIBRARY) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $< $(LIBRARY) -o $@

$(CORE_OBJECTS) $(UI_OBJECTS) $(SESSION_OBJECTS) $(POSIX_OBJECTS) $(X11_OBJECTS) $(GTK3_OBJECTS) $(GTK4_OBJECTS) $(WAYLAND_OBJECTS): $(CORE_PUBLIC_HEADERS)
$(MINGW_AMD64_CORE_OBJECTS) $(MINGW_I686_CORE_OBJECTS) $(MINGW_AMD64_UI_OBJECTS) $(MINGW_I686_UI_OBJECTS) $(MINGW_AMD64_SESSION_OBJECTS) $(MINGW_I686_SESSION_OBJECTS) $(MINGW_AMD64_WIN32_OBJECTS) $(MINGW_I686_WIN32_OBJECTS): $(CORE_PUBLIC_HEADERS)

.PHONY: test-regex-win32
test: test-regex-win32
test-regex-win32: $(MINGW_AMD64_BUILD_DIR)/tests/test_regex.exe $(MINGW_I686_BUILD_DIR)/tests/test_regex.exe $(MINGW_AMD64_BUILD_DIR)/tests/test_regex_captures.exe $(MINGW_I686_BUILD_DIR)/tests/test_regex_captures.exe
	$(WINE_RUN) $(abspath $(MINGW_AMD64_BUILD_DIR)/tests/test_regex.exe)
	$(WINE_RUN) $(abspath $(MINGW_I686_BUILD_DIR)/tests/test_regex.exe)
	$(WINE_RUN) $(abspath $(MINGW_AMD64_BUILD_DIR)/tests/test_regex_captures.exe)
	$(WINE_RUN) $(abspath $(MINGW_I686_BUILD_DIR)/tests/test_regex_captures.exe)

$(MINGW_AMD64_BUILD_DIR)/tests/test_regex.exe: tests/test_regex.c $(MINGW_AMD64_LIBRARY) | $(MINGW_AMD64_BUILD_DIR)/tests
	$(MINGW_AMD64_CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $< $(MINGW_AMD64_LIBRARY) -o $@

$(MINGW_I686_BUILD_DIR)/tests/test_regex.exe: tests/test_regex.c $(MINGW_I686_LIBRARY) | $(MINGW_I686_BUILD_DIR)/tests
	$(MINGW_I686_CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $< $(MINGW_I686_LIBRARY) -o $@

$(MINGW_AMD64_BUILD_DIR)/tests/test_regex_captures.exe: tests/test_regex_captures.c $(MINGW_AMD64_LIBRARY) | $(MINGW_AMD64_BUILD_DIR)/tests
	$(MINGW_AMD64_CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $< $(MINGW_AMD64_LIBRARY) -o $@

$(MINGW_I686_BUILD_DIR)/tests/test_regex_captures.exe: tests/test_regex_captures.c $(MINGW_I686_LIBRARY) | $(MINGW_I686_BUILD_DIR)/tests
	$(MINGW_I686_CC) $(CPPFLAGS) $(PUBLIC_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $< $(MINGW_I686_LIBRARY) -o $@

.PHONY: test-accessible-snapshot
test: test-accessible-snapshot
test-accessible-snapshot: $(BUILD_DIR)/tests/test_accessible_snapshot
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $<

$(BUILD_DIR)/tests/test_accessible_snapshot: tests/test_accessible_snapshot.c $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) $< $(UI_LIBRARY) $(LIBRARY) -lm -o $@

.PHONY: test-gtk3-accessible
test: test-gtk3-accessible
test-gtk3-accessible: $(BUILD_DIR)/tests/test_gtk3_accessible
	$(XVFB_RUN) -a env GDK_BACKEND=x11 NO_AT_BRIDGE=1 GSETTINGS_BACKEND=memory GIO_USE_VFS=local ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $<

$(BUILD_DIR)/tests/test_gtk3_accessible: tests/gtk3/test_accessible_text.c $(GTK3_LIBRARY) $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(POSIX_CPPFLAGS) $(GTK3_CPPFLAGS) $(CFLAGS) $(GTK3_PROJECT_CFLAGS) $(SANITIZERS) -Wl,--wrap=boxrig_ui_accessible_snapshot_create $< $(GTK3_LIBRARY) $(UI_LIBRARY) $(LIBRARY) $(GTK3_LIBS) -lm -o $@

.PHONY: test-gtk3-atspi
test: test-gtk3-atspi
test-gtk3-atspi: $(BUILD_DIR)/tests/test_atspi_text $(BUILD_DIR)/tests/gtk3_accessibility_driver
	sh tests/gtk/run_accessibility.sh $(BUILD_DIR)/tests/test_atspi_text $(abspath $(BUILD_DIR)/tests/gtk3_accessibility_driver)

$(BUILD_DIR)/tests/gtk3_accessibility_driver: tests/gtk/accessibility_driver.c $(GTK3_LIBRARY) $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(POSIX_CPPFLAGS) $(GTK3_CPPFLAGS) $(CFLAGS) $(GTK3_PROJECT_CFLAGS) $(SANITIZERS) $< $(GTK3_LIBRARY) $(UI_LIBRARY) $(LIBRARY) $(GTK3_LIBS) -lm -o $@

$(BUILD_DIR)/tests/test_atspi_text: tests/gtk/test_atspi_text.c | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(POSIX_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) $< $$(pkg-config --cflags --libs atspi-2 gobject-2.0) -o $@

.PHONY: test-gtk3-orca
test: test-gtk3-orca
test-gtk3-orca: $(BUILD_DIR)/tests/test_atspi_text $(BUILD_DIR)/tests/gtk3_accessibility_driver
	sh tests/gtk/run_accessibility.sh bash tests/gtk/run_orca.sh $(abspath $(BUILD_DIR)/tests/test_atspi_text) $(abspath $(BUILD_DIR)/tests/gtk3_accessibility_driver) $(abspath $(BUILD_DIR)/test-results/accessibility/gtk3)

.PHONY: test-gtk4-accessible test-gtk4-atspi test-gtk4-orca
test: test-gtk4-accessible test-gtk4-atspi test-gtk4-orca
test-gtk4-accessible: $(BUILD_DIR)/tests/test_gtk4_accessible
	$(XVFB_RUN) -a env GDK_BACKEND=x11 GTK_A11Y=none GSK_RENDERER=cairo GSETTINGS_BACKEND=memory GIO_USE_VFS=local ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $<

$(BUILD_DIR)/tests/test_gtk4_accessible: tests/gtk4/test_accessible_text.c $(GTK4_LIBRARY) $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(POSIX_CPPFLAGS) $(GTK4_CPPFLAGS) $(CFLAGS) $(GTK4_PROJECT_CFLAGS) $(SANITIZERS) -Wl,--wrap=boxrig_ui_accessible_snapshot_create $< $(GTK4_LIBRARY) $(UI_LIBRARY) $(LIBRARY) $(GTK4_LIBS) -lm \
		-Wl,--wrap=gtk_accessible_text_update_contents -Wl,--wrap=gtk_accessible_text_update_caret_position \
		-Wl,--wrap=gtk_accessible_text_update_selection_bound -o $@

test-gtk4-atspi: $(BUILD_DIR)/tests/test_atspi_text $(BUILD_DIR)/tests/gtk4_accessibility_driver
	sh tests/gtk/run_accessibility.sh $(BUILD_DIR)/tests/test_atspi_text $(abspath $(BUILD_DIR)/tests/gtk4_accessibility_driver)

$(BUILD_DIR)/tests/gtk4_accessibility_driver: tests/gtk/accessibility_driver.c $(GTK4_LIBRARY) $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(POSIX_CPPFLAGS) $(GTK4_CPPFLAGS) $(CFLAGS) $(GTK4_PROJECT_CFLAGS) $(SANITIZERS) $< $(GTK4_LIBRARY) $(UI_LIBRARY) $(LIBRARY) $(GTK4_LIBS) -lm -o $@

test-gtk4-orca: $(BUILD_DIR)/tests/test_atspi_text $(BUILD_DIR)/tests/gtk4_accessibility_driver
	sh tests/gtk/run_accessibility.sh bash tests/gtk/run_orca.sh $(abspath $(BUILD_DIR)/tests/test_atspi_text) $(abspath $(BUILD_DIR)/tests/gtk4_accessibility_driver) $(abspath $(BUILD_DIR)/test-results/accessibility/gtk4)

$(BUILD_DIR)/tests/fcitx5_server: tests/ime/fcitx5_server.c | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(IBUS_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $< $(IBUS_LIBS) -o $@

$(BUILD_DIR)/tests/test_fcitx5: tests/ime/test_fcitx5.c $(IME_SOURCES) $(IME_HEADERS) $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(FRONTEND_CPPFLAGS) $(IBUS_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) $< $(IME_SOURCES) $(UI_LIBRARY) $(LIBRARY) $(IBUS_LIBS) $(SANITIZERS) -o $@

.PHONY: test-fcitx5
test-fcitx5: $(BUILD_DIR)/tests/fcitx5_server $(BUILD_DIR)/tests/test_fcitx5
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 dbus-run-session --config-file=tests/ime/session.conf -- bash tests/ime/run_fcitx5.sh $(BUILD_DIR)/tests/fcitx5_server $(BUILD_DIR)/tests/test_fcitx5

test: test-fcitx5

$(BUILD_DIR)/tests/fcitx4_server: tests/ime/fcitx4_server.c | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(IBUS_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $< $(IBUS_LIBS) -o $@

$(BUILD_DIR)/tests/test_fcitx4: tests/ime/test_fcitx4.c $(IME_SOURCES) $(IME_HEADERS) $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(FRONTEND_CPPFLAGS) $(IBUS_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) $< $(IME_SOURCES) $(UI_LIBRARY) $(LIBRARY) $(IBUS_LIBS) $(SANITIZERS) -o $@

.PHONY: test-fcitx4
test-fcitx4: $(BUILD_DIR)/tests/fcitx4_server $(BUILD_DIR)/tests/test_fcitx4
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 dbus-run-session --config-file=tests/ime/session.conf -- bash tests/ime/run_fcitx4.sh $(BUILD_DIR)/tests/fcitx4_server $(BUILD_DIR)/tests/test_fcitx4

test: test-fcitx4

.PHONY: test-fcitx4-panels-core
test-fcitx4-panels-core: $(BUILD_DIR)/tests/fcitx4_server $(BUILD_DIR)/tests/test_fcitx4
	BOXRIG_TEST_FCITX4_PANELS=1 ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 dbus-run-session --config-file=tests/ime/session.conf -- bash tests/ime/run_fcitx4.sh $(BUILD_DIR)/tests/fcitx4_server $(BUILD_DIR)/tests/test_fcitx4

test: test-fcitx4-panels-core

$(BUILD_DIR)/tests/test_fcitx4_focus: tests/ime/test_fcitx4_focus.c $(IME_SOURCES) $(IME_HEADERS) $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(FRONTEND_CPPFLAGS) $(IBUS_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) $< $(IME_SOURCES) $(UI_LIBRARY) $(LIBRARY) $(IBUS_LIBS) $(SANITIZERS) -o $@

.PHONY: test-fcitx4-focus
test-fcitx4-focus: $(BUILD_DIR)/tests/fcitx4_server $(BUILD_DIR)/tests/test_fcitx4_focus
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 dbus-run-session --config-file=tests/ime/session.conf -- bash tests/ime/run_fcitx4.sh $(BUILD_DIR)/tests/fcitx4_server $(BUILD_DIR)/tests/test_fcitx4_focus

test: test-fcitx4-focus

$(BUILD_DIR)/tests/test_fcitx4_address: tests/ime/test_fcitx4_address.c $(IME_SOURCES) $(IME_HEADERS) $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(FRONTEND_CPPFLAGS) $(POSIX_CPPFLAGS) $(IBUS_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) $< $(IME_SOURCES) $(UI_LIBRARY) $(LIBRARY) $(IBUS_LIBS) $(SANITIZERS) -o $@

.PHONY: test-fcitx4-address
test-fcitx4-address: $(BUILD_DIR)/tests/fcitx4_server $(BUILD_DIR)/tests/test_fcitx4_address
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 dbus-run-session --config-file=tests/ime/session.conf -- bash tests/ime/run_fcitx4_address.sh $(BUILD_DIR)/tests/fcitx4_server $(BUILD_DIR)/tests/test_fcitx4_address
	BOXRIG_TEST_FCITX4_DISPLAY_NUMBER=0 ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 dbus-run-session --config-file=tests/ime/session.conf -- bash tests/ime/run_fcitx4_address.sh $(BUILD_DIR)/tests/fcitx4_server $(BUILD_DIR)/tests/test_fcitx4_address no-display

test: test-fcitx4-address

$(BUILD_DIR)/tests/test_gtk3_fcitx4: tests/gtk/test_fcitx4_widget.c $(IME_SOURCES) $(IME_HEADERS) $(GTK3_LIBRARY) $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(FRONTEND_CPPFLAGS) $(POSIX_CPPFLAGS) $(GTK3_CPPFLAGS) $(CFLAGS) $(GTK3_PROJECT_CFLAGS) $(SANITIZERS) -DBOXRIG_TEST_GTK3=1 $< $(IME_SOURCES) $(GTK3_LIBRARY) $(UI_LIBRARY) $(LIBRARY) $(GTK3_LIBS) $(SANITIZERS) -o $@

$(BUILD_DIR)/tests/test_gtk4_fcitx4: tests/gtk/test_fcitx4_widget.c $(IME_SOURCES) $(IME_HEADERS) $(GTK4_LIBRARY) $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(FRONTEND_CPPFLAGS) $(POSIX_CPPFLAGS) $(GTK4_CPPFLAGS) $(CFLAGS) $(GTK4_PROJECT_CFLAGS) $(SANITIZERS) $< $(IME_SOURCES) $(GTK4_LIBRARY) $(UI_LIBRARY) $(LIBRARY) $(GTK4_LIBS) $(SANITIZERS) -Wl,--wrap=gtk_event_controller_key_new,--wrap=gtk_event_controller_focus_new -o $@

.PHONY: test-fcitx4-panels-gtk
test-fcitx4-panels-gtk: $(BUILD_DIR)/tests/test_gtk3_fcitx4 $(BUILD_DIR)/tests/test_gtk4_fcitx4 $(BUILD_DIR)/tests/fcitx4_server
	$(XVFB_RUN) -a env BOXRIG_TEST_FCITX4_PANELS=1 GDK_BACKEND=x11 GTK_IM_MODULE=gtk-im-context-simple GTK_A11Y=none NO_AT_BRIDGE=1 GIO_USE_VFS=local ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 dbus-run-session --config-file=tests/ime/session.conf -- bash tests/ime/run_fcitx4.sh --display $(BUILD_DIR)/tests/fcitx4_server $(BUILD_DIR)/tests/test_gtk3_fcitx4
	@for gtk in 3 4; do \
		$(XVFB_RUN) -a env BOXRIG_TEST_FCITX4_PANELS=1 BOXRIG_TEST_WESTON_BACKEND=x11 GTK_IM_MODULE=gtk-im-context-simple GTK_A11Y=none GIO_USE_VFS=local LIBGL_ALWAYS_SOFTWARE=1 dbus-run-session --config-file=tests/ime/session.conf -- bash tests/gtk/run_gl_wayland.sh bash tests/ime/run_fcitx4.sh --display $(BUILD_DIR)/tests/fcitx4_server $(BUILD_DIR)/tests/test_gtk$${gtk}_fcitx4 || exit $$?; \
	done

test: test-fcitx4-panels-gtk

.PHONY: test-fcitx4-scale
test-fcitx4-scale:
	GDK_SCALE=2 $(MAKE) test-fcitx4-panels-gtk

test: test-fcitx4-scale

.PHONY: test-fcitx4-compat-real
test-fcitx4-compat-real: $(BUILD_DIR)/tests/test_gtk3_fcitx4
	$(XVFB_RUN) -a env -u FCITX_DBUS_ADDRESS GDK_BACKEND=x11 GTK_IM_MODULE=gtk-im-context-simple GTK_A11Y=none NO_AT_BRIDGE=1 GIO_USE_VFS=local BOXRIG_TEST_FCITX4_COMPAT=1 BOXRIG_TEST_FCITX4_PANELS=1 ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 dbus-run-session --config-file=tests/ime/session.conf -- bash tests/ime/run_fcitx5_real.sh --display $<
	$(XVFB_RUN) -a env -u FCITX_DBUS_ADDRESS -u GTK_IM_MODULE GDK_BACKEND=x11 GTK_A11Y=none NO_AT_BRIDGE=1 GIO_USE_VFS=local IBUS_ADDRESS= XMODIFIERS= FCITX_X11_USE_CLIENT_SIDE_UI=True BOXRIG_TEST_IME_AUTO=1 BOXRIG_TEST_FCITX4_COMPAT=1 BOXRIG_TEST_FCITX4_PANELS=1 ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 dbus-run-session --config-file=tests/ime/session.conf -- bash tests/ime/run_fcitx5_real.sh --display $<

test: test-fcitx4-compat-real

.PHONY: test-fcitx4-panels-real
test-fcitx4-panels-real: $(BUILD_DIR)/tests/test_gtk3_fcitx4 $(BUILD_DIR)/tests/test_gtk4_fcitx4 $(WAYLAND_CONTEXT_TEST_BINARY)
	$(XVFB_RUN) -a env IBUS_ADDRESS= XMODIFIERS= BOXRIG_TEST_FCITX4=1 BOXRIG_TEST_FCITX4_PANELS=1 GDK_BACKEND=x11 GIO_USE_VFS=local ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 dbus-run-session --config-file=tests/ime/session.conf -- bash tests/ime/run_fcitx4_real.sh $(WAYLAND_CONTEXT_TEST_BINARY) "$(TEST_FONT_FILE)"
	$(XVFB_RUN) -a env IBUS_ADDRESS= XMODIFIERS= BOXRIG_TEST_IME_AUTO=1 BOXRIG_TEST_FCITX4=1 BOXRIG_TEST_FCITX4_PANELS=1 GDK_BACKEND=x11 GIO_USE_VFS=local ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 dbus-run-session --config-file=tests/ime/session.conf -- bash tests/ime/run_fcitx4_real.sh $(WAYLAND_CONTEXT_TEST_BINARY) "$(TEST_FONT_FILE)"
	$(XVFB_RUN) -a env BOXRIG_TEST_FCITX4_PANELS=1 GDK_BACKEND=x11 GTK_IM_MODULE=gtk-im-context-simple GTK_A11Y=none NO_AT_BRIDGE=1 GIO_USE_VFS=local ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 dbus-run-session --config-file=tests/ime/session.conf -- bash tests/ime/run_fcitx4_real.sh $(BUILD_DIR)/tests/test_gtk3_fcitx4
	@for gtk in 3 4; do \
		$(XVFB_RUN) -a env BOXRIG_TEST_FCITX4_PANELS=1 BOXRIG_TEST_WESTON_BACKEND=x11 GTK_IM_MODULE=gtk-im-context-simple GTK_A11Y=none GIO_USE_VFS=local LIBGL_ALWAYS_SOFTWARE=1 dbus-run-session --config-file=tests/ime/session.conf -- bash tests/gtk/run_gl_wayland.sh bash tests/ime/run_fcitx4_real.sh $(BUILD_DIR)/tests/test_gtk$${gtk}_fcitx4 || exit $$?; \
	done

.PHONY: test-gtk-fcitx4
test-gtk-fcitx4: $(BUILD_DIR)/tests/fcitx4_server $(BUILD_DIR)/tests/test_gtk3_fcitx4
	$(XVFB_RUN) -a env GDK_BACKEND=x11 GTK_IM_MODULE=gtk-im-context-simple GTK_A11Y=none NO_AT_BRIDGE=1 GIO_USE_VFS=local ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 dbus-run-session --config-file=tests/ime/session.conf -- bash tests/ime/run_fcitx4.sh --display $(BUILD_DIR)/tests/fcitx4_server $(BUILD_DIR)/tests/test_gtk3_fcitx4

test: test-gtk-fcitx4

.PHONY: test-fcitx4-real
test-fcitx4-real: $(BUILD_DIR)/tests/test_gtk3_fcitx4
	$(XVFB_RUN) -a env GDK_BACKEND=x11 GTK_IM_MODULE=gtk-im-context-simple GTK_A11Y=none NO_AT_BRIDGE=1 GIO_USE_VFS=local ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 dbus-run-session --config-file=tests/ime/session.conf -- bash tests/ime/run_fcitx4_real.sh $(BUILD_DIR)/tests/test_gtk3_fcitx4
	$(XVFB_RUN) -a env BOXRIG_TEST_FCITX4_NO_FOCUS_COMMIT=1 GDK_BACKEND=x11 GTK_IM_MODULE=gtk-im-context-simple GTK_A11Y=none NO_AT_BRIDGE=1 GIO_USE_VFS=local ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 dbus-run-session --config-file=tests/ime/session.conf -- bash tests/ime/run_fcitx4_real.sh $(BUILD_DIR)/tests/test_gtk3_fcitx4
	$(XVFB_RUN) -a env BOXRIG_TEST_FCITX4_EXPLICIT_ADDRESS=1 GDK_BACKEND=x11 GTK_IM_MODULE=gtk-im-context-simple GTK_A11Y=none NO_AT_BRIDGE=1 GIO_USE_VFS=local ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 dbus-run-session --config-file=tests/ime/session.conf -- bash tests/ime/run_fcitx4_real.sh $(BUILD_DIR)/tests/test_gtk3_fcitx4

$(BUILD_DIR)/tests/test_gtk3_fcitx: tests/gtk/test_fcitx_widget.c $(IME_HEADERS) $(GTK3_LIBRARY) $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(FRONTEND_CPPFLAGS) $(POSIX_CPPFLAGS) $(GTK3_CPPFLAGS) $(CFLAGS) $(GTK3_PROJECT_CFLAGS) $(SANITIZERS) -DBOXRIG_TEST_GTK3=1 $< $(IME_SOURCES) $(GTK3_LIBRARY) $(UI_LIBRARY) $(LIBRARY) $(GTK3_LIBS) $(SANITIZERS) -o $@

$(BUILD_DIR)/tests/test_gtk4_fcitx: tests/gtk/test_fcitx_widget.c $(IME_HEADERS) $(GTK4_LIBRARY) $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(FRONTEND_CPPFLAGS) $(POSIX_CPPFLAGS) $(GTK4_CPPFLAGS) $(CFLAGS) $(GTK4_PROJECT_CFLAGS) $(SANITIZERS) $< $(IME_SOURCES) $(GTK4_LIBRARY) $(UI_LIBRARY) $(LIBRARY) $(GTK4_LIBS) $(SANITIZERS) -Wl,--wrap=gtk_event_controller_key_new,--wrap=gtk_event_controller_focus_new -o $@

.PHONY: test-gtk-fcitx5
test-gtk-fcitx5: $(BUILD_DIR)/tests/fcitx5_server $(BUILD_DIR)/tests/test_gtk3_fcitx $(BUILD_DIR)/tests/test_gtk4_fcitx
	@for gtk in 3 4; do \
		$(XVFB_RUN) -a env GDK_BACKEND=x11 GTK_IM_MODULE=gtk-im-context-simple GTK_A11Y=none NO_AT_BRIDGE=1 GIO_USE_VFS=local LIBGL_ALWAYS_SOFTWARE=1 ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 dbus-run-session --config-file=tests/ime/session.conf -- bash tests/ime/run_fcitx5.sh $(BUILD_DIR)/tests/fcitx5_server $(BUILD_DIR)/tests/test_gtk$${gtk}_fcitx || exit $$?; \
	done
	@for gtk in 3 4; do \
		$(XVFB_RUN) -a env BOXRIG_TEST_WESTON_BACKEND=x11 GTK_IM_MODULE=gtk-im-context-simple GTK_A11Y=none GIO_USE_VFS=local LIBGL_ALWAYS_SOFTWARE=1 dbus-run-session --config-file=tests/ime/session.conf -- bash tests/gtk/run_gl_wayland.sh bash tests/ime/run_fcitx5.sh $(BUILD_DIR)/tests/fcitx5_server $(BUILD_DIR)/tests/test_gtk$${gtk}_fcitx || exit $$?; \
	done

test: test-gtk-fcitx5

.PHONY: test-wayland-fcitx5 test-fcitx5-real
test-wayland-fcitx5: $(WAYLAND_CONTEXT_TEST_BINARY) $(BUILD_DIR)/tests/fcitx5_server
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 BOXRIG_TEST_FCITX5=1 dbus-run-session --config-file=tests/ime/session.conf -- bash tests/ime/run_fcitx5.sh $(BUILD_DIR)/tests/fcitx5_server $(WAYLAND_CONTEXT_TEST_BINARY) "$(TEST_FONT_FILE)"

test-fcitx5-real: $(WAYLAND_CONTEXT_TEST_BINARY) $(BUILD_DIR)/tests/test_gtk3_fcitx $(BUILD_DIR)/tests/test_gtk4_fcitx
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 BOXRIG_TEST_FCITX5=1 dbus-run-session --config-file=tests/ime/session.conf -- bash tests/ime/run_fcitx5_real.sh $(WAYLAND_CONTEXT_TEST_BINARY) "$(TEST_FONT_FILE)"
	$(XVFB_RUN) -a env GDK_BACKEND=x11 GTK_IM_MODULE=gtk-im-context-simple GTK_A11Y=none NO_AT_BRIDGE=1 GIO_USE_VFS=local ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 dbus-run-session --config-file=tests/ime/session.conf -- bash tests/ime/run_fcitx5_real.sh --display $(BUILD_DIR)/tests/test_gtk3_fcitx
	@for gtk in 3 4; do \
		$(XVFB_RUN) -a env BOXRIG_TEST_WESTON_BACKEND=x11 GTK_IM_MODULE=gtk-im-context-simple GTK_A11Y=none GIO_USE_VFS=local LIBGL_ALWAYS_SOFTWARE=1 dbus-run-session --config-file=tests/ime/session.conf -- bash tests/gtk/run_gl_wayland.sh bash tests/ime/run_fcitx5_real.sh --display $(BUILD_DIR)/tests/test_gtk$${gtk}_fcitx || exit $$?; \
	done

test: test-wayland-fcitx5 test-fcitx5-real

.PHONY: test-fcitx5-panels
test-fcitx5-panels: $(BUILD_DIR)/tests/fcitx5_server $(WAYLAND_CONTEXT_TEST_BINARY) $(BUILD_DIR)/tests/test_gtk3_fcitx $(BUILD_DIR)/tests/test_gtk4_fcitx
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 BOXRIG_TEST_FCITX5=1 BOXRIG_TEST_FCITX_CANDIDATES=1 dbus-run-session --config-file=tests/ime/session.conf -- bash tests/ime/run_fcitx5.sh $(BUILD_DIR)/tests/fcitx5_server $(WAYLAND_CONTEXT_TEST_BINARY) "$(TEST_FONT_FILE)"
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 BOXRIG_TEST_FCITX5=1 BOXRIG_TEST_FCITX_CANDIDATES=1 dbus-run-session --config-file=tests/ime/session.conf -- bash tests/ime/run_fcitx5_real.sh $(WAYLAND_CONTEXT_TEST_BINARY) "$(TEST_FONT_FILE)"
	$(XVFB_RUN) -a env GDK_BACKEND=x11 GTK_IM_MODULE=gtk-im-context-simple GTK_A11Y=none NO_AT_BRIDGE=1 GIO_USE_VFS=local BOXRIG_TEST_FCITX_CANDIDATES=1 ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 dbus-run-session --config-file=tests/ime/session.conf -- bash tests/ime/run_fcitx5.sh $(BUILD_DIR)/tests/fcitx5_server $(BUILD_DIR)/tests/test_gtk3_fcitx
	$(XVFB_RUN) -a env GDK_BACKEND=x11 GTK_IM_MODULE=gtk-im-context-simple GTK_A11Y=none NO_AT_BRIDGE=1 GIO_USE_VFS=local BOXRIG_TEST_FCITX_CANDIDATES=1 FCITX_X11_USE_CLIENT_SIDE_UI=True ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 dbus-run-session --config-file=tests/ime/session.conf -- bash tests/ime/run_fcitx5_real.sh --display $(BUILD_DIR)/tests/test_gtk3_fcitx
	$(XVFB_RUN) -a env -u FCITX_X11_USE_CLIENT_SIDE_UI GDK_BACKEND=x11 GTK_IM_MODULE=gtk-im-context-simple GTK_A11Y=none NO_AT_BRIDGE=1 GIO_USE_VFS=local BOXRIG_TEST_FCITX_CANDIDATES=1 BOXRIG_TEST_FCITX_SUPPRESSED=1 ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 dbus-run-session --config-file=tests/ime/session.conf -- bash tests/ime/run_fcitx5_real.sh --display $(BUILD_DIR)/tests/test_gtk3_fcitx
	@for gtk in 3 4; do \
		$(XVFB_RUN) -a env BOXRIG_TEST_WESTON_BACKEND=x11 GTK_IM_MODULE=gtk-im-context-simple GTK_A11Y=none GIO_USE_VFS=local LIBGL_ALWAYS_SOFTWARE=1 BOXRIG_TEST_FCITX_CANDIDATES=1 dbus-run-session --config-file=tests/ime/session.conf -- bash tests/gtk/run_gl_wayland.sh bash tests/ime/run_fcitx5.sh $(BUILD_DIR)/tests/fcitx5_server $(BUILD_DIR)/tests/test_gtk$${gtk}_fcitx || exit $$?; \
		$(XVFB_RUN) -a env BOXRIG_TEST_WESTON_BACKEND=x11 GTK_IM_MODULE=gtk-im-context-simple GTK_A11Y=none GIO_USE_VFS=local LIBGL_ALWAYS_SOFTWARE=1 BOXRIG_TEST_FCITX_CANDIDATES=1 dbus-run-session --config-file=tests/ime/session.conf -- bash tests/gtk/run_gl_wayland.sh bash tests/ime/run_fcitx5_real.sh --display $(BUILD_DIR)/tests/test_gtk$${gtk}_fcitx || exit $$?; \
	done

test: test-fcitx5-panels

.PHONY: test-fcitx5-scale
test-fcitx5-scale: $(BUILD_DIR)/tests/fcitx5_server $(BUILD_DIR)/tests/test_gtk3_fcitx $(BUILD_DIR)/tests/test_gtk4_fcitx
	$(XVFB_RUN) -a env GDK_BACKEND=x11 GDK_SCALE=2 GTK_IM_MODULE=gtk-im-context-simple GTK_A11Y=none NO_AT_BRIDGE=1 GIO_USE_VFS=local ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 dbus-run-session --config-file=tests/ime/session.conf -- bash tests/ime/run_fcitx5.sh $(BUILD_DIR)/tests/fcitx5_server $(BUILD_DIR)/tests/test_gtk3_fcitx
	@for gtk in 3 4; do \
		$(XVFB_RUN) -a env BOXRIG_TEST_WESTON_BACKEND=x11 GDK_SCALE=2 GTK_IM_MODULE=gtk-im-context-simple GTK_A11Y=none GIO_USE_VFS=local LIBGL_ALWAYS_SOFTWARE=1 dbus-run-session --config-file=tests/ime/session.conf -- bash tests/gtk/run_gl_wayland.sh bash tests/ime/run_fcitx5.sh $(BUILD_DIR)/tests/fcitx5_server $(BUILD_DIR)/tests/test_gtk$${gtk}_fcitx || exit $$?; \
	done

test: test-fcitx5-scale

$(BUILD_DIR)/tests/test_ime_discovery: tests/ime/test_discovery.c $(IME_SOURCES) $(IME_HEADERS) $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(FRONTEND_CPPFLAGS) $(IBUS_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) $< $(IME_SOURCES) $(UI_LIBRARY) $(LIBRARY) $(IBUS_LIBS) $(SANITIZERS) -o $@

.PHONY: test-ime-discovery
test-ime-discovery: $(BUILD_DIR)/tests/test_ime_discovery $(BUILD_DIR)/tests/fcitx5_server $(BUILD_DIR)/tests/wayland_ibus_engine
	GIO_USE_VFS=local ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 dbus-run-session --config-file=tests/ime/session.conf -- $<
	GIO_USE_VFS=local ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 BOXRIG_TEST_FCITX5=1 dbus-run-session --config-file=tests/ime/session.conf -- bash tests/ime/run_fcitx5.sh $(BUILD_DIR)/tests/fcitx5_server $<
	GIO_USE_VFS=local ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 BOXRIG_TEST_FCITX5=1 BOXRIG_TEST_IME_BOTH=1 dbus-run-session --config-file=tests/ime/session.conf -- node tests/wayland/run_ibus_context.mjs tests/ime/run_discovery_with_fcitx.sh "$(TEST_FONT_FILE)" "$(BUILD_DIR)/tests/wayland_ibus_engine" "$(BUILD_DIR)/test-results/ime-discovery" "$(BUILD_DIR)/tests/fcitx5_server" "$(BUILD_DIR)/tests/test_ime_discovery"

test: test-ime-discovery

.PHONY: test-fcitx4-discovery
test-fcitx4-discovery: $(BUILD_DIR)/tests/test_ime_discovery $(BUILD_DIR)/tests/fcitx4_server $(BUILD_DIR)/tests/fcitx5_server $(BUILD_DIR)/tests/wayland_ibus_engine
	GIO_USE_VFS=local BOXRIG_TEST_FCITX4=1 ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 dbus-run-session --config-file=tests/ime/session.conf -- bash tests/ime/run_fcitx4.sh $(BUILD_DIR)/tests/fcitx4_server $<
	GIO_USE_VFS=local BOXRIG_TEST_FCITX4=1 BOXRIG_TEST_FCITX5=1 ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 dbus-run-session --config-file=tests/ime/session.conf -- bash tests/ime/run_fcitx4.sh $(BUILD_DIR)/tests/fcitx4_server bash tests/ime/run_fcitx5.sh $(BUILD_DIR)/tests/fcitx5_server $<
	GIO_USE_VFS=local BOXRIG_TEST_FCITX4=1 BOXRIG_TEST_IME_BOTH=1 ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 dbus-run-session --config-file=tests/ime/session.conf -- node tests/wayland/run_ibus_context.mjs tests/ime/run_discovery_with_fcitx4.sh "$(TEST_FONT_FILE)" "$(BUILD_DIR)/tests/wayland_ibus_engine" "$(BUILD_DIR)/test-results/fcitx4/discovery-ibus" "$(BUILD_DIR)/tests/fcitx4_server" "$(BUILD_DIR)/tests/test_ime_discovery"
	GIO_USE_VFS=local BOXRIG_TEST_FCITX4=1 BOXRIG_TEST_FCITX5=1 BOXRIG_TEST_IME_BOTH=1 ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 dbus-run-session --config-file=tests/ime/session.conf -- node tests/wayland/run_ibus_context.mjs tests/ime/run_discovery_with_fcitx.sh "$(TEST_FONT_FILE)" "$(BUILD_DIR)/tests/wayland_ibus_engine" "$(BUILD_DIR)/test-results/fcitx4/discovery-all" "$(BUILD_DIR)/tests/fcitx5_server" /bin/bash tests/ime/run_fcitx4.sh "$(BUILD_DIR)/tests/fcitx4_server" "$(BUILD_DIR)/tests/test_ime_discovery"
test: test-fcitx4-discovery

.PHONY: test-fcitx4-auto
test-fcitx4-auto: $(BUILD_DIR)/tests/test_fcitx4 $(BUILD_DIR)/tests/test_gtk3_fcitx4 $(BUILD_DIR)/tests/fcitx4_server
	IBUS_ADDRESS= XMODIFIERS= BOXRIG_TEST_IME_AUTO=1 ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 dbus-run-session --config-file=tests/ime/session.conf -- bash tests/ime/run_fcitx4.sh $(BUILD_DIR)/tests/fcitx4_server $(BUILD_DIR)/tests/test_fcitx4
	$(XVFB_RUN) -a env -u GTK_IM_MODULE IBUS_ADDRESS= XMODIFIERS= BOXRIG_TEST_IME_AUTO=1 GDK_BACKEND=x11 GTK_A11Y=none NO_AT_BRIDGE=1 ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 dbus-run-session --config-file=tests/ime/session.conf -- bash tests/ime/run_fcitx4.sh --display $(BUILD_DIR)/tests/fcitx4_server $(BUILD_DIR)/tests/test_gtk3_fcitx4

test: test-fcitx4-auto

.PHONY: test-fcitx4-auto-real
test-fcitx4-auto-real: $(BUILD_DIR)/tests/test_gtk3_fcitx4
	$(XVFB_RUN) -a env -u GTK_IM_MODULE IBUS_ADDRESS= XMODIFIERS= BOXRIG_TEST_IME_AUTO=1 GDK_BACKEND=x11 GTK_A11Y=none NO_AT_BRIDGE=1 GIO_USE_VFS=local ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 dbus-run-session --config-file=tests/ime/session.conf -- bash tests/ime/run_fcitx4_real.sh $<

.PHONY: test-fcitx4-native
test-fcitx4-native: $(WAYLAND_CONTEXT_TEST_BINARY) $(BUILD_DIR)/tests/fcitx4_server
	IBUS_ADDRESS= XMODIFIERS= BOXRIG_TEST_FCITX4=1 ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 dbus-run-session --config-file=tests/ime/session.conf -- bash tests/ime/run_fcitx4.sh $(BUILD_DIR)/tests/fcitx4_server $< "$(TEST_FONT_FILE)"
	IBUS_ADDRESS= XMODIFIERS= BOXRIG_TEST_FCITX4=1 BOXRIG_TEST_IME_AUTO=1 ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 dbus-run-session --config-file=tests/ime/session.conf -- bash tests/ime/run_fcitx4.sh $(BUILD_DIR)/tests/fcitx4_server $< "$(TEST_FONT_FILE)"

test: test-fcitx4-native

.PHONY: test-fcitx4-panels-native
test-fcitx4-panels-native: $(WAYLAND_CONTEXT_TEST_BINARY) $(BUILD_DIR)/tests/fcitx4_server
	IBUS_ADDRESS= XMODIFIERS= BOXRIG_TEST_FCITX4_PANELS=1 BOXRIG_TEST_FCITX4=1 ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 dbus-run-session --config-file=tests/ime/session.conf -- bash tests/ime/run_fcitx4.sh $(BUILD_DIR)/tests/fcitx4_server $(WAYLAND_CONTEXT_TEST_BINARY) "$(TEST_FONT_FILE)"
	IBUS_ADDRESS= XMODIFIERS= BOXRIG_TEST_IME_AUTO=1 BOXRIG_TEST_FCITX4_PANELS=1 BOXRIG_TEST_FCITX4=1 ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 dbus-run-session --config-file=tests/ime/session.conf -- bash tests/ime/run_fcitx4.sh $(BUILD_DIR)/tests/fcitx4_server $(WAYLAND_CONTEXT_TEST_BINARY) "$(TEST_FONT_FILE)"

test: test-fcitx4-panels-native

.PHONY: test-fcitx5-auto-core
test-fcitx5-auto-core: $(BUILD_DIR)/tests/test_fcitx5 $(BUILD_DIR)/tests/fcitx5_server
	GIO_USE_VFS=local IBUS_ADDRESS= XMODIFIERS= BOXRIG_TEST_IME_AUTO=1 ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 dbus-run-session --config-file=tests/ime/session.conf -- bash tests/ime/run_fcitx5.sh $(BUILD_DIR)/tests/fcitx5_server $<

test: test-fcitx5-auto-core

.PHONY: test-fcitx5-auto
test-fcitx5-auto: $(BUILD_DIR)/tests/fcitx5_server $(WAYLAND_CONTEXT_TEST_BINARY) $(BUILD_DIR)/tests/test_gtk3_fcitx $(BUILD_DIR)/tests/test_gtk4_fcitx
	GIO_USE_VFS=local IBUS_ADDRESS= XMODIFIERS= BOXRIG_TEST_IME_AUTO=1 BOXRIG_TEST_FCITX5=1 BOXRIG_TEST_FCITX_CANDIDATES=1 ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 dbus-run-session --config-file=tests/ime/session.conf -- bash tests/ime/run_fcitx5.sh $(BUILD_DIR)/tests/fcitx5_server $(WAYLAND_CONTEXT_TEST_BINARY) "$(TEST_FONT_FILE)"
	$(XVFB_RUN) -a env -u GTK_IM_MODULE GDK_BACKEND=x11 GIO_USE_VFS=local GTK_A11Y=none NO_AT_BRIDGE=1 IBUS_ADDRESS= XMODIFIERS= BOXRIG_TEST_IME_AUTO=1 BOXRIG_TEST_FCITX_CANDIDATES=1 ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 dbus-run-session --config-file=tests/ime/session.conf -- bash tests/ime/run_fcitx5.sh $(BUILD_DIR)/tests/fcitx5_server $(BUILD_DIR)/tests/test_gtk3_fcitx
	@for gtk in 3 4; do \
		$(XVFB_RUN) -a env -u GTK_IM_MODULE BOXRIG_TEST_WESTON_BACKEND=x11 GTK_A11Y=none GIO_USE_VFS=local LIBGL_ALWAYS_SOFTWARE=1 IBUS_ADDRESS= XMODIFIERS= BOXRIG_TEST_IME_AUTO=1 BOXRIG_TEST_FCITX_CANDIDATES=1 dbus-run-session --config-file=tests/ime/session.conf -- bash tests/gtk/run_gl_wayland.sh bash tests/ime/run_fcitx5.sh $(BUILD_DIR)/tests/fcitx5_server $(BUILD_DIR)/tests/test_gtk$${gtk}_fcitx || exit $$?; \
	done
	@for gtk in 3 4; do for module in gtk-im-context-simple fcitx; do \
		$(XVFB_RUN) -a env BOXRIG_TEST_WESTON_BACKEND=x11 GTK_IM_MODULE=$$module GTK_A11Y=none GIO_USE_VFS=local LIBGL_ALWAYS_SOFTWARE=1 IBUS_ADDRESS= XMODIFIERS=@im=fcitx BOXRIG_TEST_IME_AUTO=1 BOXRIG_TEST_FCITX_CANDIDATES=1 dbus-run-session --config-file=tests/ime/session.conf -- bash tests/gtk/run_gl_wayland.sh bash tests/ime/run_fcitx5.sh $(BUILD_DIR)/tests/fcitx5_server $(BUILD_DIR)/tests/test_gtk$${gtk}_fcitx || exit $$?; \
	done; done
	@for gtk in 3 4; do \
		$(XVFB_RUN) -a env -u GTK_IM_MODULE BOXRIG_TEST_WESTON_BACKEND=x11 BOXRIG_TEST_GTK_MODULE=fcitx GTK_A11Y=none GIO_USE_VFS=local LIBGL_ALWAYS_SOFTWARE=1 IBUS_ADDRESS= XMODIFIERS=@im=ibus BOXRIG_TEST_IME_AUTO=1 BOXRIG_TEST_FCITX_CANDIDATES=1 dbus-run-session --config-file=tests/ime/session.conf -- bash tests/gtk/run_gl_wayland.sh bash tests/ime/run_fcitx5.sh $(BUILD_DIR)/tests/fcitx5_server $(BUILD_DIR)/tests/test_gtk$${gtk}_fcitx || exit $$?; \
	done

test: test-fcitx5-auto

.PHONY: test-fcitx5-auto-real
test-fcitx5-auto-real: $(WAYLAND_CONTEXT_TEST_BINARY) $(BUILD_DIR)/tests/test_gtk3_fcitx $(BUILD_DIR)/tests/test_gtk4_fcitx
	GIO_USE_VFS=local IBUS_ADDRESS= XMODIFIERS= BOXRIG_TEST_IME_AUTO=1 BOXRIG_TEST_FCITX5=1 BOXRIG_TEST_FCITX_CANDIDATES=1 ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 dbus-run-session --config-file=tests/ime/session.conf -- bash tests/ime/run_fcitx5_real.sh $(WAYLAND_CONTEXT_TEST_BINARY) "$(TEST_FONT_FILE)"
	$(XVFB_RUN) -a env -u GTK_IM_MODULE GDK_BACKEND=x11 GTK_A11Y=none NO_AT_BRIDGE=1 GIO_USE_VFS=local IBUS_ADDRESS= XMODIFIERS= BOXRIG_TEST_IME_AUTO=1 BOXRIG_TEST_FCITX_CANDIDATES=1 FCITX_X11_USE_CLIENT_SIDE_UI=True ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 dbus-run-session --config-file=tests/ime/session.conf -- bash tests/ime/run_fcitx5_real.sh --display $(BUILD_DIR)/tests/test_gtk3_fcitx
	@for gtk in 3 4; do \
		$(XVFB_RUN) -a env -u GTK_IM_MODULE BOXRIG_TEST_WESTON_BACKEND=x11 GTK_A11Y=none GIO_USE_VFS=local LIBGL_ALWAYS_SOFTWARE=1 IBUS_ADDRESS= XMODIFIERS= BOXRIG_TEST_IME_AUTO=1 BOXRIG_TEST_FCITX_CANDIDATES=1 dbus-run-session --config-file=tests/ime/session.conf -- bash tests/gtk/run_gl_wayland.sh bash tests/ime/run_fcitx5_real.sh --display $(BUILD_DIR)/tests/test_gtk$${gtk}_fcitx || exit $$?; \
	done

test: test-fcitx5-auto-real

.PHONY: test-fcitx5-display
test-fcitx5-display: $(GTK3_EXAMPLE_BINARY) $(WAYLAND_EXAMPLE_BINARY)
	@for renderer in wayland opengl; do \
		$(XVFB_RUN) -a -s '-screen 0 1280x800x24' env BOXRIG_TEST_WESTON_BACKEND=x11 BOXRIG_TEST_WESTON_DEBUG=1 GTK_IM_MODULE=gtk-im-context-simple GTK_A11Y=none GIO_USE_VFS=local LIBGL_ALWAYS_SOFTWARE=1 IBUS_ADDRESS= XMODIFIERS=@im=fcitx dbus-run-session --config-file=tests/ime/session.conf -- bash tests/gtk/run_gl_wayland.sh bash tests/ime/run_fcitx5_real.sh --display node tests/ime/test_display.mjs $(WAYLAND_EXAMPLE_BINARY) wayland $$renderer $(BUILD_DIR)/test-results/fcitx5-display/wayland-$$renderer || exit $$?; \
	done
	$(XVFB_RUN) -a -s '-screen 0 1280x800x24' env GDK_BACKEND=x11 GTK_IM_MODULE=gtk-im-context-simple GTK_A11Y=none NO_AT_BRIDGE=1 GIO_USE_VFS=local LIBGL_ALWAYS_SOFTWARE=1 dbus-run-session --config-file=tests/ime/session.conf -- bash tests/ime/run_fcitx5_real.sh --display node tests/ime/test_display.mjs $(GTK3_EXAMPLE_BINARY) gtk3 cairo $(BUILD_DIR)/test-results/fcitx5-display/gtk3-system

test: test-fcitx5-display

.PHONY: test-fcitx4-display
test-fcitx4-display: $(GTK3_EXAMPLE_BINARY) $(WAYLAND_EXAMPLE_BINARY)
	@for renderer in wayland opengl; do \
		$(XVFB_RUN) -a -s '-screen 0 1280x800x24' env BOXRIG_TEST_FCITX4_DISPLAY=1 BOXRIG_TEST_WESTON_BACKEND=x11 BOXRIG_TEST_WESTON_DEBUG=1 GTK_IM_MODULE=gtk-im-context-simple GTK_A11Y=none GIO_USE_VFS=local LIBGL_ALWAYS_SOFTWARE=1 IBUS_ADDRESS= XMODIFIERS=@im=fcitx dbus-run-session --config-file=tests/ime/session.conf -- bash tests/gtk/run_gl_wayland.sh bash tests/ime/run_fcitx4_real.sh node tests/ime/test_display.mjs $(WAYLAND_EXAMPLE_BINARY) wayland $$renderer $(BUILD_DIR)/test-results/fcitx4-display/wayland-$$renderer || exit $$?; \
	done
	$(XVFB_RUN) -a -s '-screen 0 1280x800x24' env BOXRIG_TEST_FCITX4_DISPLAY=1 GDK_BACKEND=x11 GTK_IM_MODULE=gtk-im-context-simple GTK_A11Y=none NO_AT_BRIDGE=1 GIO_USE_VFS=local LIBGL_ALWAYS_SOFTWARE=1 dbus-run-session --config-file=tests/ime/session.conf -- bash tests/ime/run_fcitx4_real.sh node tests/ime/test_display.mjs $(GTK3_EXAMPLE_BINARY) gtk3 cairo $(BUILD_DIR)/test-results/fcitx4-display/gtk3-system
	$(XVFB_RUN) -a -s '-screen 0 1280x800x24' env BOXRIG_TEST_FCITX4_DISPLAY=1 BOXRIG_TEST_FCITX4_CLIENT=1 GDK_BACKEND=x11 GTK_IM_MODULE=gtk-im-context-simple GTK_A11Y=none NO_AT_BRIDGE=1 GIO_USE_VFS=local LIBGL_ALWAYS_SOFTWARE=1 dbus-run-session --config-file=tests/ime/session.conf -- bash tests/ime/run_fcitx4_real.sh node tests/ime/test_display.mjs $(GTK3_EXAMPLE_BINARY) gtk3 cairo $(BUILD_DIR)/test-results/fcitx4-display/gtk3-client

# Real sample GSource regression, using an isolated GLib main context.
SAMPLE_RECOGNITION_SOURCES := tests/gtk/test_sample_recognition.c \
	examples/boxrig_term/common/text_policy.c \
	examples/boxrig_term/common/font_demo.c \
	examples/boxrig_term/common/scrollbar_demo.c

$(BUILD_DIR)/tests/test_gtk3_sample_recognition: $(SAMPLE_RECOGNITION_SOURCES) \
		examples/boxrig_term/gtk3/event_source.c examples/boxrig_term/gtk3/application_internal.h \
		examples/boxrig_term/gtk3/event_source_policy.c $(GTK3_LIBRARY) $(SESSION_LIBRARY) $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(POSIX_CPPFLAGS) $(GTK3_CPPFLAGS) \
		$(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) -ffunction-sections -fdata-sections \
		$(SAMPLE_RECOGNITION_SOURCES) examples/boxrig_term/gtk3/event_source_policy.c \
		$(GTK3_LIBRARY) $(SESSION_LIBRARY) $(UI_LIBRARY) $(LIBRARY) $(GTK3_LIBS) \
		-Wl,--gc-sections,--wrap=boxrig_term_text_policy_resume $(SANITIZERS) -o $@

$(BUILD_DIR)/tests/test_gtk4_sample_recognition: $(SAMPLE_RECOGNITION_SOURCES) \
		examples/boxrig_term/gtk4/event_source.c examples/boxrig_term/gtk4/application_internal.h \
		examples/boxrig_term/gtk4/event_source_policy.c $(GTK4_LIBRARY) $(SESSION_LIBRARY) $(UI_LIBRARY) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(POSIX_CPPFLAGS) $(GTK4_CPPFLAGS) \
		$(CFLAGS) $(GTK4_PROJECT_CFLAGS) $(SANITIZERS) -ffunction-sections -fdata-sections \
		$(SAMPLE_RECOGNITION_SOURCES) examples/boxrig_term/gtk4/event_source_policy.c \
		$(GTK4_LIBRARY) $(SESSION_LIBRARY) $(UI_LIBRARY) $(LIBRARY) $(GTK4_LIBS) \
		-Wl,--gc-sections,--wrap=boxrig_term_text_policy_resume $(SANITIZERS) -o $@

.PHONY: test-gtk-sample-recognition
test: test-gtk-sample-recognition
test-gtk-sample-recognition: $(BUILD_DIR)/tests/test_gtk3_sample_recognition $(BUILD_DIR)/tests/test_gtk4_sample_recognition $(FRONTEND_SAMPLE_GTK3_PROBE_BINARY) $(FRONTEND_SAMPLE_GTK4_PROBE_BINARY)
	$(XVFB_RUN) -a env GDK_BACKEND=x11 LIBGL_ALWAYS_SOFTWARE=1 NO_AT_BRIDGE=1 GTK_A11Y=none ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(BUILD_DIR)/tests/test_gtk3_sample_recognition
	$(XVFB_RUN) -a env GDK_BACKEND=x11 LIBGL_ALWAYS_SOFTWARE=1 NO_AT_BRIDGE=1 GTK_A11Y=none GSK_RENDERER=cairo $(GTK4_TEST_IM_ENV) ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(BUILD_DIR)/tests/test_gtk4_sample_recognition
	$(XVFB_RUN) -a env GDK_BACKEND=x11 LIBGL_ALWAYS_SOFTWARE=1 NO_AT_BRIDGE=1 GTK_A11Y=none node tests/gtk/run_sample_recognition.mjs $(FRONTEND_SAMPLE_GTK3_PROBE_BINARY)
	$(XVFB_RUN) -a env GDK_BACKEND=x11 LIBGL_ALWAYS_SOFTWARE=1 NO_AT_BRIDGE=1 GTK_A11Y=none $(GTK4_TEST_IM_ENV) node tests/gtk/run_sample_recognition.mjs $(FRONTEND_SAMPLE_GTK4_PROBE_BINARY)

include tests/performance/phase_probe.mk
include tests/performance/recognition.mk
include tests/x11/transfer.mk
include tests/wayland/transfer.mk

test: test-x11-transfer

$(BUILD_DIR)/tests/performance/steady_recognizer: tests/performance/steady_recognizer.c \
		tests/measure_text_workloads.c $(TEXT_MEASUREMENT_LIBRARY) | $(BUILD_DIR)/tests/performance
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(POSIX_CPPFLAGS) $(CFLAGS) \
		$(PROJECT_CFLAGS) -I. -DBOXRIG_MEASURE_TEXT_WORKLOADS $< \
		$(TEXT_MEASUREMENT_LIBRARY) -Wl,--wrap=malloc -Wl,--wrap=calloc \
		-Wl,--wrap=realloc -Wl,--wrap=free -Wl,--wrap=memmove -o $@

$(BUILD_DIR)/tests/performance/steady_recognizer_unwrapped: tests/performance/steady_recognizer.c \
		tests/measure_text_workloads.c $(TEXT_MEASUREMENT_LIBRARY) | $(BUILD_DIR)/tests/performance
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(POSIX_CPPFLAGS) $(CFLAGS) \
		$(PROJECT_CFLAGS) -I. -DBOXRIG_MEASURE_TEXT_WORKLOADS \
		-DBOXRIG_MEASURE_UNWRAPPED $< $(TEXT_MEASUREMENT_LIBRARY) -o $@

.PHONY: test-steady-recognizer
test: test-steady-recognizer
test-steady-recognizer: $(BUILD_DIR)/tests/performance/steady_recognizer
	$< self-test

.PHONY: test-performance-phases
test: test-performance-phases
test-performance-phases: $(PHASE_STATE_TEST)
	$(PHASE_STATE_TEST)

.PHONY: test-notification-empty-body
test: test-notification-empty-body
test-notification-empty-body: $(BUILD_DIR)/tests/test_notification_empty_body
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $<

$(BUILD_DIR)/tests/test_notification_empty_body: tests/test_notification_empty_body.c src/core/notification.c $(CORE_HEADERS) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) -fno-sanitize-recover=undefined tests/test_notification_empty_body.c src/core/notification.c $(LIBRARY) $(SANITIZERS) -o $@

.PHONY: test-screen-row-rotation
test: test-screen-row-rotation
test-screen-row-rotation: $(BUILD_DIR)/tests/test_screen_row_rotation
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $<

$(BUILD_DIR)/tests/test_screen_row_rotation: tests/test_screen_row_rotation.c $(CORE_HEADERS) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) $< $(LIBRARY) $(SANITIZERS) -Wl,--wrap=calloc -Wl,--wrap=memmove -Wl,--wrap=boxrig_screen_view_cell -o $@

.PHONY: test-ascii-runs
.PHONY: test-input-profile
test: test-input-profile
test-input-profile: $(BUILD_DIR)/tests/performance/profile_input
	bash tests/performance/test_input_profile.sh $< $(BUILD_DIR)/tests/performance

$(BUILD_DIR)/tests/performance/profile_input: INPUT_PROFILE_FLAGS = \
	-Wl,--wrap=boxrig_screen_put_ascii_run -Wl,--wrap=boxrig_codec_decoder_put \
	-Wl,--wrap=boxrig_screen_put_codepoint -Wl,--wrap=utf8proc_grapheme_break_stateful \
	-Wl,--wrap=utf8proc_get_property -Wl,--wrap=utf8proc_charwidth
$(BUILD_DIR)/tests/performance/profile_input_unwrapped: INPUT_PROFILE_FLAGS = -DBOXRIG_INPUT_UNWRAPPED

$(BUILD_DIR)/tests/performance/profile_input \
$(BUILD_DIR)/tests/performance/profile_input_unwrapped: tests/performance/profile_input.c \
		tests/measure_text_workloads.c $(TEXT_MEASUREMENT_LIBRARY) | $(BUILD_DIR)/tests/performance
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(POSIX_CPPFLAGS) $(CFLAGS) \
		$(PROJECT_CFLAGS) -DBOXRIG_MEASURE_TEXT_WORKLOADS -DBOXRIG_MEASURE_UNWRAPPED \
		$< $(TEXT_MEASUREMENT_LIBRARY) -Wl,--wrap=boxrig_terminal_create \
		-Wl,--wrap=boxrig_text_workload_counters_reset \
		-Wl,--wrap=boxrig_text_workload_counters_read \
		$(INPUT_PROFILE_FLAGS) -o $@

$(BUILD_DIR)/tests/performance/interactive_workload: tests/performance/interactive_workload.c \
		tests/performance/pty_workload.c tests/performance/interval_trace.c \
		tests/performance/interval_trace.h | $(BUILD_DIR)/tests/performance
	$(CC) $(POSIX_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $< \
		tests/performance/interval_trace.c -o $@

$(BUILD_DIR)/tests/performance/interactive_probe: tests/performance/interactive_probe.c \
		| $(BUILD_DIR)/tests/performance
	$(CC) $(POSIX_CPPFLAGS) $(X11_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $< \
		$(X11_LIBS) -lXtst -o $@

test: test-ascii-runs
test-ascii-runs: $(BUILD_DIR)/tests/test_ascii_runs
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $<

$(BUILD_DIR)/tests/test_ascii_runs: tests/test_ascii_runs.c $(CORE_HEADERS) $(LIBRARY) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZERS) $< $(LIBRARY) $(SANITIZERS) -Wl,--wrap=boxrig_unicode_cluster_analyzer_accept -Wl,--wrap=malloc -o $@

$(BUILD_DIR)/tests/performance/annotation_gtk_peer: tests/performance/annotation_gtk_peer.c | $(BUILD_DIR)/tests/performance
	$(CC) $(POSIX_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $< -o $@

$(BUILD_DIR)/tests/performance/annotation_gtk_probe: tests/performance/annotation_gtk_probe.c | $(BUILD_DIR)/tests/performance
	$(CC) $(POSIX_CPPFLAGS) $(X11_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $< $(X11_LIBS) -lXtst -o $@

$(BUILD_DIR)/tests/performance/annotation_gl_vendor: tests/performance/annotation_gl_vendor.c | $(BUILD_DIR)/tests/performance
	$(CC) $(POSIX_CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $< -lX11 -lGL -o $@

$(BUILD_DIR)/tests/performance/annotation_gtk_counted: $(GTK3_EXAMPLE_SOURCES) \
		tests/performance/annotation_gtk_count.c $(GTK3_LIBRARY) $(NOTIFICATION_LIBRARY) \
		$(POSIX_LIBRARY) $(SESSION_LIBRARY) $(UI_LIBRARY) $(LIBRARY) \
		| $(BUILD_DIR)/tests/performance
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(POSIX_URI_CPPFLAGS) $(POSIX_CPPFLAGS) \
		$(GTK3_CPPFLAGS) $(CFLAGS) $(GTK3_PROJECT_CFLAGS) \
		-DBOXRIG_ANNOTATION_COUNT_LINE_CLEAR=1 \
		$(GTK3_EXAMPLE_SOURCES) tests/performance/annotation_gtk_count.c \
		$(GTK3_LIBRARY) $(NOTIFICATION_LIBRARY) $(POSIX_LIBRARY) $(SESSION_LIBRARY) \
		$(UI_LIBRARY) $(LIBRARY) $(GTK3_LIBS) -lutil $(POSIX_URI_LIBS) \
		-Wl,--wrap=boxrig_term_text_policy_create \
		-Wl,--wrap=boxrig_term_text_policy_resume \
		-Wl,--wrap=boxrig_ui_view_replace_text_annotations \
		-Wl,--wrap=boxrig_ui_view_clear_text_annotation_line \
		-Wl,--wrap=boxrig_ui_view_clear_text_annotations \
		-Wl,--wrap=boxrig_terminal_text_recognizer_event_peek -o $@

PLAN11_BASELINE_TEXT_POLICY := $(BUILD_DIR)/tests/performance/plan11-text_policy.c
PLAN11_BASELINE_GTK3_SOURCES := \
	$(filter-out examples/boxrig_term/common/text_policy.c,$(GTK3_EXAMPLE_SOURCES)) \
	$(PLAN11_BASELINE_TEXT_POLICY)

$(PLAN11_BASELINE_TEXT_POLICY): | $(BUILD_DIR)/tests/performance
	git show 2a7a682:examples/boxrig_term/common/text_policy.c > $@.tmp && mv $@.tmp $@

$(BUILD_DIR)/tests/performance/annotation_gtk_counted_baseline: $(PLAN11_BASELINE_GTK3_SOURCES) \
		tests/performance/annotation_gtk_count.c $(GTK3_LIBRARY) $(NOTIFICATION_LIBRARY) \
		$(POSIX_LIBRARY) $(SESSION_LIBRARY) artifacts/plan11/baseline/libboxrig-ui.a \
		artifacts/plan11/baseline/libboxrig.a | $(BUILD_DIR)/tests/performance
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(POSIX_URI_CPPFLAGS) $(POSIX_CPPFLAGS) \
		-Iexamples/boxrig_term/common \
		$(GTK3_CPPFLAGS) $(CFLAGS) $(GTK3_PROJECT_CFLAGS) \
		$(PLAN11_BASELINE_GTK3_SOURCES) tests/performance/annotation_gtk_count.c \
		$(GTK3_LIBRARY) $(NOTIFICATION_LIBRARY) $(POSIX_LIBRARY) $(SESSION_LIBRARY) \
		artifacts/plan11/baseline/libboxrig-ui.a artifacts/plan11/baseline/libboxrig.a \
		$(GTK3_LIBS) -lutil $(POSIX_URI_LIBS) \
		-Wl,--wrap=boxrig_term_text_policy_create \
		-Wl,--wrap=boxrig_term_text_policy_resume \
		-Wl,--wrap=boxrig_ui_view_replace_text_annotations \
		-Wl,--wrap=boxrig_ui_view_clear_text_annotations \
		-Wl,--wrap=boxrig_terminal_text_recognizer_event_peek -o $@

.PHONY: test-annotation-gtk annotation-gtk-comparison annotation-gtk-stress
test: test-annotation-gtk
test-annotation-gtk: $(GTK3_EXAMPLE_BINARY) $(BUILD_DIR)/tests/performance/annotation_gtk_peer $(BUILD_DIR)/tests/performance/annotation_gtk_probe $(BUILD_DIR)/tests/performance/annotation_gl_vendor
	bash tests/performance/run_annotation_gtk.sh $(GTK3_EXAMPLE_BINARY) $(BUILD_DIR)/tests/performance/annotation_gtk_peer $(BUILD_DIR)/tests/performance/annotation_gtk_probe cairo 0 idle none $(BUILD_DIR)/tests/performance/annotation-gtk-smoke

annotation-gtk-comparison: $(GTK3_EXAMPLE_BINARY) $(BUILD_DIR)/tests/performance/annotation_gtk_peer $(BUILD_DIR)/tests/performance/annotation_gtk_probe $(BUILD_DIR)/tests/performance/annotation_gl_vendor $(BUILD_DIR)/tests/performance/annotation_gtk_counted $(BUILD_DIR)/tests/performance/annotation_gtk_counted_baseline
	node tests/performance/run_annotation_gtk_matrix.mjs artifacts/plan11/gtk-matrix

annotation-gtk-stress: $(BUILD_DIR)/tests/performance/annotation_gtk_peer $(BUILD_DIR)/tests/performance/annotation_gtk_probe $(BUILD_DIR)/tests/performance/annotation_gl_vendor
	node tests/performance/run_annotation_gtk_stress.mjs artifacts/plan11/gtk-matrix
