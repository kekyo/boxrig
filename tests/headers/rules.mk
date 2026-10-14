# Public C headers must preserve C linkage in ordinary C++20 consumers.
HEADER_NATIVE_LIBS := $(WAYLAND_LIBRARY) $(X11_LIBRARY) \
	$(OPENGL_RENDERER_LIBRARY) $(POSIX_LIBRARY) $(SESSION_LIBRARY) \
	$(UI_LIBRARY) $(LIBRARY)
HEADER_NATIVE_LDLIBS := $(WAYLAND_LIBS) $(X11_LIBS) $(OPENGL_RENDERER_LIBS)
HEADER_CONSUMER_DIR := $(BUILD_DIR)/tests/headers

.PHONY: test-public-headers test-public-headers-native test-public-headers-win32
test: test-public-headers
test-public-headers: test-public-headers-native test-public-headers-win32

test-public-headers-native: $(HEADER_NATIVE_LIBS) $(GTK3_LIBRARY) $(GTK4_LIBRARY)
	sh tests/headers/check.sh "$(CC)" "$(CXX)" native
	mkdir -p $(HEADER_CONSUMER_DIR)
	@set -e; for language in c c++; do \
		if [ "$$language" = c ]; then compiler='$(CC)'; standard=c99; else compiler='$(CXX)'; standard=c++20; fi; \
		for frontend in native gtk3 gtk4; do \
			case $$frontend in \
				native) flags=''; libs='$(HEADER_NATIVE_LIBS) $(HEADER_NATIVE_LDLIBS)' ;; \
				gtk3) flags='-DBOXRIG_HEADER_GTK3 $(GTK3_CPPFLAGS)'; libs='$(GTK3_LIBRARY) $(SESSION_LIBRARY) $(UI_LIBRARY) $(LIBRARY) $(GTK3_LIBS)' ;; \
				gtk4) flags='-DBOXRIG_HEADER_GTK4 -Wno-pedantic $(GTK4_CPPFLAGS)'; libs='$(GTK4_LIBRARY) $(SESSION_LIBRARY) $(UI_LIBRARY) $(LIBRARY) $(GTK4_LIBS)' ;; \
			esac; \
			$$compiler -x $$language -std=$$standard $(WARNINGS) $(PUBLIC_CPPFLAGS) $$flags \
				-c tests/headers/consumer.c -o $(HEADER_CONSUMER_DIR)/$$frontend-$$standard.o; \
			$$compiler $(HEADER_CONSUMER_DIR)/$$frontend-$$standard.o $$libs \
				-o $(HEADER_CONSUMER_DIR)/$$frontend-$$standard; \
			$(HEADER_CONSUMER_DIR)/$$frontend-$$standard; \
		done; \
	done

test-public-headers-win32: $(MINGW_AMD64_LIBRARY) $(MINGW_AMD64_UI_LIBRARY) \
	$(MINGW_AMD64_SESSION_LIBRARY) $(MINGW_AMD64_WIN32_LIBRARY) \
	$(MINGW_I686_LIBRARY) $(MINGW_I686_UI_LIBRARY) \
	$(MINGW_I686_SESSION_LIBRARY) $(MINGW_I686_WIN32_LIBRARY)
	mkdir -p $(HEADER_CONSUMER_DIR)
	@set -e; for arch in amd64 i686; do \
		case $$arch in \
			amd64) cc='$(MINGW_AMD64_CC)'; cxx='$(MINGW_AMD64_CXX)'; directory='$(MINGW_AMD64_BUILD_DIR)' ;; \
			i686) cc='$(MINGW_I686_CC)'; cxx='$(MINGW_I686_CXX)'; directory='$(MINGW_I686_BUILD_DIR)' ;; \
		esac; \
		sh tests/headers/check.sh "$$cc" "$$cxx" win32; \
		for language in c c++; do \
			if [ "$$language" = c ]; then compiler=$$cc; standard=c99; else compiler=$$cxx; standard=c++20; fi; \
			$$compiler -x $$language -std=$$standard $(WARNINGS) $(PUBLIC_CPPFLAGS) $(MINGW_WIN32_CPPFLAGS) \
				-c tests/headers/consumer.c -o $(HEADER_CONSUMER_DIR)/$$arch-$$standard.o; \
			$$compiler -static $(HEADER_CONSUMER_DIR)/$$arch-$$standard.o \
				$$directory/libboxrig-win32.a $$directory/libboxrig-session.a \
				$$directory/libboxrig-ui.a $$directory/libboxrig.a \
				$(WIN32_RENDERER_LIBS) $(WIN32_HOST_LIBS) -lm \
				-o $(HEADER_CONSUMER_DIR)/$$arch-$$standard.exe; \
			$(WINE_RUN) $(abspath $(HEADER_CONSUMER_DIR))/$$arch-$$standard.exe; \
		done; \
	done
