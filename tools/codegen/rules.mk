# Architecture-independent generation, also callable without the native UI SDKs.
BUILD_DIR ?= build
WAYLAND_PROTOCOLS_VERSION := 1.48
# Pinned submodule release: https://gitlab.freedesktop.org/wayland/wayland-protocols/-/tags/1.48
WAYLAND_PROTOCOLS_DIR := deps/wayland-protocols
WAYLAND_VIEWPORTER_XML := $(WAYLAND_PROTOCOLS_DIR)/stable/viewporter/viewporter.xml
WAYLAND_FRACTIONAL_SCALE_XML := $(WAYLAND_PROTOCOLS_DIR)/staging/fractional-scale/fractional-scale-v1.xml
WAYLAND_XDG_SHELL_XML := $(WAYLAND_PROTOCOLS_DIR)/stable/xdg-shell/xdg-shell.xml
WAYLAND_XDG_DECORATION_XML := $(WAYLAND_PROTOCOLS_DIR)/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml
WAYLAND_TEXT_INPUT_XML := $(WAYLAND_PROTOCOLS_DIR)/unstable/text-input/text-input-unstable-v3.xml
WAYLAND_PRIMARY_SELECTION_XML := $(WAYLAND_PROTOCOLS_DIR)/unstable/primary-selection/primary-selection-unstable-v1.xml
WAYLAND_PROTOCOL_XMLS := $(WAYLAND_VIEWPORTER_XML) $(WAYLAND_FRACTIONAL_SCALE_XML) \
	$(WAYLAND_XDG_SHELL_XML) $(WAYLAND_XDG_DECORATION_XML) \
	$(WAYLAND_TEXT_INPUT_XML) $(WAYLAND_PRIMARY_SELECTION_XML)
BACKGROUND_WAYLAND_XML := $(WAYLAND_PROTOCOLS_DIR)/staging/ext-background-effect/ext-background-effect-v1.xml
WAYLAND_LICENSE_XMLS := $(WAYLAND_PROTOCOL_XMLS) $(BACKGROUND_WAYLAND_XML)
WAYLAND_LICENSE_NOTICES := $(BUILD_DIR)/generated/licenses/Wayland-protocols-NOTICES.md
CODEGEN_DIR := $(BUILD_DIR)/tools/codegen
CODEGEN_TOOLS := $(addprefix $(CODEGEN_DIR)/,generate-wayland-notices.mjs generate-shift-jis.mjs generate-linux-metadata.mjs)

.PHONY: codegen-tools package-codegen
codegen-tools: $(CODEGEN_TOOLS)
package-codegen: codegen-tools $(WAYLAND_LICENSE_NOTICES)
	BOXRIG_GTK_OPENGL=1 node $(CODEGEN_DIR)/generate-linux-metadata.mjs "$(BUILD_DIR)/generated/pkgconfig" "$(or $(BOXRIG_VERSION),0.0.0)" /usr /usr/lib /usr/include /usr/lib/pkgconfig boxrig boxrig-ui boxrig-session boxrig-posix boxrig-gtk3 boxrig-gtk4

$(CODEGEN_TOOLS) &: $(wildcard tools/codegen/src/*.ts) tools/codegen/vite.config.ts tools/codegen/tsconfig.json tools/codegen/package.json tools/codegen/package-lock.json
	@test -d tools/codegen/node_modules || { echo 'Run npm ci --prefix tools/codegen to install code generation dependencies.' >&2; exit 1; }
	BOXRIG_CODEGEN_OUT_DIR="$(abspath $(CODEGEN_DIR))" npm --prefix tools/codegen run build

# Generate distribution notices through the TypeScript/Vite/funcity toolchain.
$(WAYLAND_LICENSE_NOTICES): $(WAYLAND_LICENSE_XMLS) $(CODEGEN_DIR)/generate-wayland-notices.mjs
	mkdir -p $(@D)
	node $(CODEGEN_DIR)/generate-wayland-notices.mjs "$@" "$(WAYLAND_PROTOCOLS_VERSION)" $(WAYLAND_LICENSE_XMLS)
