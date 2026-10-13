CC ?= cc
BOXRIG_ROOT ?= ../../..
ROOT := $(abspath $(BOXRIG_ROOT))

CPPFLAGS += -I$(ROOT)/include $(shell pkg-config --cflags x11)
CFLAGS ?= -O2 -g
CFLAGS += -std=c99 -Wall -Wextra -Wpedantic -Werror
LDLIBS += $(shell pkg-config --libs x11) -lutil

LIBBOXRIG := $(ROOT)/build/libboxrig-posix.a \
             $(ROOT)/build/libboxrig-session.a \
             $(ROOT)/build/libboxrig.a
UI_LIBBOXRIG := $(ROOT)/build/libboxrig-x11.a \
                $(ROOT)/build/libboxrig-ui.a
UI_PACKAGES := x11 xcomposite xrender xfixes 'pango >= 1.50' pangocairo pangoft2 fontconfig dbus-1
UI_CPPFLAGS = $(shell pkg-config --cflags $(UI_PACKAGES))
UI_LDLIBS = $(shell pkg-config --libs $(UI_PACKAGES))
UI_TEST := $(ROOT)/build/tests/test_simple_ui_term
UI_GUIDE_TEST := $(ROOT)/build/tests/test_ui_guide
UI_GUIDE_BUILD := $(ROOT)/build/simple-ui-guide

.PHONY: all clean libraries ui-libraries check

all: simple_core_term simple_ui_term

# Build the public archives with the repository's normal rules.
libraries:
	$(MAKE) -C $(ROOT) build/libboxrig-posix.a build/libboxrig-session.a build/libboxrig.a

ui-libraries:
	$(MAKE) -C $(ROOT) build/libboxrig-x11.a build/libboxrig-ui.a

$(LIBBOXRIG): libraries
$(UI_LIBBOXRIG): ui-libraries

simple_core_term: simple_core_term.c $(LIBBOXRIG)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $< $(LIBBOXRIG) $(LDLIBS) -o $@

simple_ui_term: simple_ui_term.c $(UI_LIBBOXRIG) $(LIBBOXRIG)
	$(CC) $(CPPFLAGS) $(UI_CPPFLAGS) $(CFLAGS) $(LDFLAGS) $< $(UI_LIBBOXRIG) $(LIBBOXRIG) $(UI_LDLIBS) $(LDLIBS) -o $@

$(UI_TEST): $(ROOT)/tests/x11/test_simple_ui_term.c
	mkdir -p $(@D)
	$(CC) $(CFLAGS) $(LDFLAGS) $< $(shell pkg-config --cflags --libs x11) -o $@

$(UI_GUIDE_TEST): $(ROOT)/tests/test_ui_guide.c
	$(MAKE) -C $(ROOT) build/tests/test_ui_guide

check: all $(UI_TEST) $(UI_GUIDE_TEST)
	xvfb-run -a $(UI_TEST) --pty ./simple_ui_term
	xvfb-run -a $(UI_TEST) --close ./simple_ui_term
	$(RM) -r $(UI_GUIDE_BUILD)
	mkdir -p $(UI_GUIDE_BUILD)
	$(UI_GUIDE_TEST) step-by-step-ui.md $(UI_GUIDE_BUILD) Makefile hello_ui_term.c editable_document.c
	$(MAKE) -C $(UI_GUIDE_BUILD) BOXRIG_ROOT=$(ROOT)
	xvfb-run -a $(UI_TEST) --hello $(UI_GUIDE_BUILD)/hello_ui_term
	$(MAKE) -C $(UI_GUIDE_BUILD) BOXRIG_ROOT=$(ROOT) editable_document
	test "$$($(UI_GUIDE_BUILD)/editable_document)" = "前か語後 revision=2 cursor=9 pty=0"
	test "$$($(UI_GUIDE_BUILD)/editable_document --reject)" = "前かな後 revision=1 cursor=9 pty=0"

clean:
	$(RM) simple_core_term simple_ui_term
