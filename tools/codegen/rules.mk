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
CODEGEN_TOOLS := build/tools/codegen/generate-wayland-notices.mjs build/tools/codegen/generate-shift-jis.mjs

.PHONY: codegen-tools package-codegen
codegen-tools: $(CODEGEN_TOOLS)
package-codegen: codegen-tools $(WAYLAND_LICENSE_NOTICES)

$(CODEGEN_TOOLS) &: $(wildcard tools/codegen/src/*.ts) tools/codegen/vite.config.ts tools/codegen/tsconfig.json tools/codegen/package.json tools/codegen/package-lock.json
	@test -d tools/codegen/node_modules || { echo 'Run npm ci --prefix tools/codegen to install code generation dependencies.' >&2; exit 1; }
	npm --prefix tools/codegen run build

# Use the TypeScript/Vite/funcity toolchain only for generated notices.
$(WAYLAND_LICENSE_NOTICES): $(WAYLAND_LICENSE_XMLS) build/tools/codegen/generate-wayland-notices.mjs
	mkdir -p $(@D)
	node build/tools/codegen/generate-wayland-notices.mjs "$@" "$(WAYLAND_PROTOCOLS_VERSION)" $(WAYLAND_LICENSE_XMLS)
