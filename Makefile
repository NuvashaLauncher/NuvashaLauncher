# Copyright (C) 2026 Flia
# SPDX-License-Identifier: GPL-3.0-only
#
# Hand-written Makefile for AML, Another Minecraft Launcher.
# Release objects live in build/, sanitizer objects in build-debug/.

CC         := gcc
PKG_CONFIG ?= pkg-config
PREFIX     ?= /usr/local

# Libraries AML links against. Never add an X11 package here: AML is
# Wayland only and must not link anything X11 related.
# gio-unix-2.0 is the Linux part of GIO itself (no display involved) and
# provides the output stream that writes a download into its temporary
# file descriptor.
PKGS := gtk4 libadwaita-1 json-glib-1.0 libsoup-3.0 libarchive gio-unix-2.0

# pkg-config prints -I paths; use -isystem so warnings are only about our
# own code and not about the GTK/GLib headers themselves.
PKG_CFLAGS := $(shell $(PKG_CONFIG) --cflags $(PKGS) | \
	sed -e 's/ -I/ -isystem /g' -e 's/^-I/-isystem /')
PKG_LIBS := $(shell $(PKG_CONFIG) --libs $(PKGS))
CPPFLAGS += -DAML_LOCALE_DIR=\"$(PREFIX)/share/locale\"
LOCALES := es_AR es_ES pt_BR fr de it ja ko zh_CN ru pl uk tr nl
LOCALE_MOS := $(LOCALES:%=build/locale/%/LC_MESSAGES/aml.mo)

CFLAGS   := -std=c17 -Wall -Wextra -Wpedantic -MMD -MP
RELFLAGS := -O2
DBGFLAGS := -g -O0 -fsanitize=address,undefined

BIN    := build/AML
DBGBIN := build-debug/AML

SRCS    := $(wildcard src/*.c)
RELOBJS := $(patsubst src/%.c,build/%.o,$(SRCS))
DBGOBJS := $(patsubst src/%.c,build-debug/%.o,$(SRCS))
RELDEPS := $(RELOBJS:.o=.d)
DBGDEPS := $(DBGOBJS:.o=.d)

# Tests link the src files they need; src/main.c is excluded because it
# owns main().
TESTLIBSRCS := $(filter-out src/main.c,$(SRCS))
TESTLIBOBJS := $(patsubst src/%.c,build/%.o,$(TESTLIBSRCS))
TESTLIBDBGOBJS := $(patsubst src/%.c,build-debug/%.o,$(TESTLIBSRCS))
TESTSRCS    := $(wildcard tests/test-*.c)
TESTBINS    := $(patsubst tests/%.c,build/tests/%,$(TESTSRCS))
TESTDEPS    := $(addsuffix .d,$(TESTBINS))
TESTDBGBINS := $(patsubst tests/%.c,build-debug/tests/%,$(TESTSRCS))
TESTDBGDEPS := $(addsuffix .d,$(TESTDBGBINS))

.PHONY: all debug test test-debug test-lsan test-valgrind static-analysis clean install uninstall

all: $(if $(SRCS),$(BIN)) $(LOCALE_MOS)

debug: $(if $(SRCS),$(DBGBIN))

$(BIN): $(RELOBJS) | build
	$(CC) $(RELFLAGS) -o $@ $(RELOBJS) $(PKG_LIBS)

$(DBGBIN): $(DBGOBJS) | build-debug
	$(CC) $(DBGFLAGS) -o $@ $(DBGOBJS) $(PKG_LIBS)

build/%.o: src/%.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(RELFLAGS) $(PKG_CFLAGS) -c -o $@ $<

build-debug/%.o: src/%.c | build-debug
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DBGFLAGS) $(PKG_CFLAGS) -c -o $@ $<

build/tests/%: tests/%.c $(TESTLIBOBJS) | build/tests
	$(CC) $(CFLAGS) $(RELFLAGS) -Isrc $(PKG_CFLAGS) \
		-DAML_DATA_DIR=\"$(CURDIR)/data\" -DAML_BIN=\"$(CURDIR)/$(BIN)\" \
		-DAML_TEST_FIXTURES=\"$(CURDIR)/tests/fixtures\" \
		-DAML_SRC_DIR=\"$(CURDIR)/src\" -DAML_PO_DIR=\"$(CURDIR)/po\" \
		-o $@ $< $(TESTLIBOBJS) $(PKG_LIBS)

# Tests never write to the real data directory and never touch the
# network. Only the icon name audit opens a display, and it skips itself
# when there is none; the test that runs the launcher runs it with DISPLAY
# and WAYLAND_DISPLAY removed.
test: $(if $(SRCS),$(BIN)) $(TESTBINS)
	@set -e; for t in $(TESTBINS); do echo "RUN  $$t"; ./$$t; done; \
	echo "PASS $(words $(TESTBINS)) test(s)"

# Same tests, but every test and every src object is built with
# AddressSanitizer and UndefinedBehaviorSanitizer into build-debug/.
build-debug/tests/%: tests/%.c $(TESTLIBDBGOBJS) | build-debug/tests
	$(CC) $(CFLAGS) $(DBGFLAGS) -Isrc $(PKG_CFLAGS) \
		-DAML_DATA_DIR=\"$(CURDIR)/data\" -DAML_BIN=\"$(CURDIR)/$(BIN)\" \
		-DAML_TEST_FIXTURES=\"$(CURDIR)/tests/fixtures\" \
		-DAML_SRC_DIR=\"$(CURDIR)/src\" -DAML_PO_DIR=\"$(CURDIR)/po\" \
		-o $@ $< $(TESTLIBDBGOBJS) $(PKG_LIBS)

# The Wayland guard test runs the release launcher, so test-debug builds
# both binaries.
test-debug: $(if $(SRCS),$(DBGBIN) $(BIN)) $(TESTDBGBINS)
	@set -e; for t in $(TESTDBGBINS); do echo "RUN  $$t"; ./$$t; done; \
	echo "PASS $(words $(TESTDBGBINS)) test(s)"

# LeakSanitizer is included with the Linux AddressSanitizer runtime. Keep it
# explicit so CI does not silently lose leak checking when sanitizer defaults
# change. The options are set per test rather than around a recursive make,
# so the suite runs once and every test is the one being leak checked.
test-lsan: $(if $(SRCS),$(DBGBIN) $(BIN)) $(TESTDBGBINS)
	@set -e; for t in $(TESTDBGBINS); do echo "RUN  $$t"; \
	  ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
	  LSAN_OPTIONS=detect_leaks=1:exitcode=1 \
	  ./$$t; done; \
	echo "PASS $(words $(TESTDBGBINS)) test(s)"

# Optional local target for environments that provide Valgrind. The caches
# the toolkit and the graphics driver keep for the life of the process are
# listed in tools/aml.valgrind.supp.
test-valgrind: $(if $(SRCS),$(BIN)) $(TESTBINS)
	@set -e; for t in $(TESTBINS); do echo "VALGRIND $$t"; \
	  G_SLICE=always-malloc G_DEBUG=gc-friendly \
	  valgrind --quiet --leak-check=full --suppressions=tools/aml.valgrind.supp \
	    --errors-for-leak-kinds=definite,indirect \
	    --error-exitcode=1 ./$$t; done

static-analysis:
	@set -e; for source in src/*.c; do \
	  $(CC) $(CPPFLAGS) -std=c17 -Wall -Wextra -Wpedantic -fanalyzer -fsyntax-only \
	    $(PKG_CFLAGS) -Isrc "$$source"; done
	@if command -v clang-tidy >/dev/null; then \
	  clang-tidy src/*.c --warnings-as-errors='*' \
	    -- $(CPPFLAGS) -std=c17 $(PKG_CFLAGS) -Isrc; \
	else echo "clang-tidy not installed; skipping"; fi
	@if command -v cppcheck >/dev/null; then \
	  cppcheck --enable=warning,style,performance,portability --error-exitcode=1 \
	    --inline-suppr --suppress=missingIncludeSystem --suppress=unknownMacro \
	    --suppress=syntaxError -Isrc src; \
	else echo "cppcheck not installed; skipping"; fi

install: $(if $(SRCS),$(BIN)) $(LOCALE_MOS)
	install -Dm755 $(BIN) $(DESTDIR)$(PREFIX)/bin/AML
	install -Dm644 data/io.gitlab.anotherminecraftlauncher.AML.desktop \
		$(DESTDIR)$(PREFIX)/share/applications/io.gitlab.anotherminecraftlauncher.AML.desktop
	@for lang in $(LOCALES); do \
		install -Dm644 build/locale/$$lang/LC_MESSAGES/aml.mo \
			$(DESTDIR)$(PREFIX)/share/locale/$$lang/LC_MESSAGES/aml.mo; \
	done

# The same list the install loop uses, so the catalogs go back out with
# the rest of the launcher.
uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/AML
	rm -f $(DESTDIR)$(PREFIX)/share/applications/io.gitlab.anotherminecraftlauncher.AML.desktop
	@for lang in $(LOCALES); do \
		rm -f $(DESTDIR)$(PREFIX)/share/locale/$$lang/LC_MESSAGES/aml.mo; \
	done

clean:
	rm -rf build build-debug

build build-debug build/tests build-debug/tests:
	mkdir -p $@

build/locale/%/LC_MESSAGES/aml.mo: po/%.po
	mkdir -p $(dir $@)
	msgfmt -o $@ $<

-include $(RELDEPS) $(DBGDEPS) $(TESTDEPS) $(TESTDBGDEPS)
