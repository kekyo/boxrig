# An explicit elder-terms checkout and its built SDK are required. This makefile
# only reads that checkout/build and puts all diagnostic outputs under OUTPUT.
ELDER_SOURCE ?=
ELDER_BUILD ?=
OUTPUT ?= build/osc8-resolve
OSC8_CXXFLAGS ?= -O0 -g
SDK := $(ELDER_BUILD)/boxrig/sdk
DRIVER := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))/osc8_resolve_cost.cpp
SOURCES := $(DRIVER) $(ELDER_SOURCE)/elder-terms-terminal/src/terminal-links.cpp $(ELDER_SOURCE)/elder-terms-terminal/src/terminal-hyperlink-action.cpp
FLAGS := -std=c++20 -Wall -Wextra -Wpedantic -Werror $(OSC8_CXXFLAGS) \
	-I$(ELDER_SOURCE)/elder-terms-terminal/src -I$(ELDER_SOURCE)/shared/include \
	-I$(ELDER_SOURCE)/deps/cardio/include -I$(SDK)/include \
	-DCARDIO_SHARED_LIB=1 -DCARDIO_WITH_GLIB=1 -DCARDIO_WITH_GIO=1 -DCARDIO_WITH_LINUX_IO_URING=1 \
	$(shell pkg-config --cflags gtk+-3.0 gio-2.0)
LIBS := -L$(ELDER_BUILD)/shared -L$(ELDER_BUILD) -L$(SDK)/lib \
	-Wl,-rpath,$(abspath $(ELDER_BUILD)/shared):$(abspath $(ELDER_BUILD)):$(abspath $(SDK)/lib) \
	-lelder-terms -lboxrig-ui -lboxrig -lcardio $(shell pkg-config --libs gtk+-3.0 gio-2.0)

.PHONY: all check validate
all: $(OUTPUT)/plain $(OUTPUT)/observed

validate:
	@test -n "$(ELDER_SOURCE)" -a -n "$(ELDER_BUILD)" || { echo 'Set ELDER_SOURCE and ELDER_BUILD to one matching checkout/build.' >&2; exit 2; }

$(OUTPUT):
	mkdir -p "$@"

$(OUTPUT)/plain: $(SOURCES) | validate $(OUTPUT)
	$(CXX) $(FLAGS) $(SOURCES) $(LIBS) -o "$@"

$(OUTPUT)/observed: $(SOURCES) | validate $(OUTPUT)
	$(CXX) $(FLAGS) -DBOXRIG_OBSERVE_OSC8 $(SOURCES) $(LIBS) \
		-Wl,--wrap=boxrig_regex_matcher_create -Wl,--wrap=boxrig_regex_matcher_resume -o "$@"

check: all
	! $(OUTPUT)/plain invalid > $(OUTPUT)/invalid.log 2>&1
	$(OUTPUT)/plain default > $(OUTPUT)/default.csv
	$(OUTPUT)/plain custom > $(OUTPUT)/custom.csv
	$(OUTPUT)/observed default > $(OUTPUT)/default-observed.csv
	$(OUTPUT)/observed custom > $(OUTPUT)/custom-observed.csv
