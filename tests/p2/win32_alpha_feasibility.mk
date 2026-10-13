# Standalone native API probe; deliberately does not link any boxrig archive.
ALPHA_PROBE_DIR ?= build/tests/win32-alpha-feasibility
ALPHA_PROBE_SOURCE := tests/p2/win32_alpha_feasibility.cpp
ALPHA_PROBE_FLAGS := -std=c++20 -Wall -Wextra -Werror -O2 -municode -static -fno-exceptions -fno-rtti
ALPHA_PROBE_LIBS := -lopengl32 -lgdi32 -luser32 -lole32

.PHONY: all
all: $(ALPHA_PROBE_DIR)/amd64.exe $(ALPHA_PROBE_DIR)/i686.exe

$(ALPHA_PROBE_DIR)/amd64.exe: $(ALPHA_PROBE_SOURCE) tests/p2/win32_alpha_feasibility.mk $(ALPHA_PROBE_DIR)/amd64-manifest.o
	mkdir -p $(@D)
	x86_64-w64-mingw32-g++ $(ALPHA_PROBE_FLAGS) $< $(ALPHA_PROBE_DIR)/amd64-manifest.o $(ALPHA_PROBE_LIBS) -o $@

$(ALPHA_PROBE_DIR)/i686.exe: $(ALPHA_PROBE_SOURCE) tests/p2/win32_alpha_feasibility.mk $(ALPHA_PROBE_DIR)/i686-manifest.o
	mkdir -p $(@D)
	i686-w64-mingw32-g++ $(ALPHA_PROBE_FLAGS) $< $(ALPHA_PROBE_DIR)/i686-manifest.o $(ALPHA_PROBE_LIBS) -o $@

$(ALPHA_PROBE_DIR)/amd64-manifest.o: tests/p2/win32_transparency.rc tests/p2/win32_transparency.manifest
	mkdir -p $(@D)
	x86_64-w64-mingw32-windres -I . $< -o $@

$(ALPHA_PROBE_DIR)/i686-manifest.o: tests/p2/win32_transparency.rc tests/p2/win32_transparency.manifest
	mkdir -p $(@D)
	i686-w64-mingw32-windres -I . $< -o $@
