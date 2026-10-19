# Copyright (C) 2026 Flia
# SPDX-License-Identifier: GPL-3.0-only
#
# Hand-written Makefile for nuvasha, Nuvasha Launcher.
# Release objects live in build/, sanitizer objects in build-debug/.

CC         := gcc
PKG_CONFIG ?= pkg-config
PREFIX     ?= /usr/local
VERSION    := $(strip $(shell cat VERSION))

# Libraries nuvasha links against. GTK loads the available display backend at
# runtime, so nuvasha does not link to a window-system backend directly.
# gio-unix-2.0 is the Linux part of GIO itself (no display involved) and
# provides the output stream that writes a download into its temporary
# file descriptor.
PKGS := gtk4 libadwaita-1 json-glib-1.0 libsoup-3.0 libarchive gio-unix-2.0 epoxy

# pkg-config prints -I paths; use -isystem so warnings are only about our
# own code and not about the GTK/GLib headers themselves.
PKG_CFLAGS := $(shell $(PKG_CONFIG) --cflags $(PKGS) | \
	sed -e 's/ -I/ -isystem /g' -e 's/^-I/-isystem /')
PKG_LIBS := $(shell $(PKG_CONFIG) --libs $(PKGS)) -lm
CPPFLAGS += -Dnuvasha_LOCALE_DIR=\"$(PREFIX)/share/locale\"
CPPFLAGS += -Dnuvasha_VERSION=\"$(VERSION)\"
CPPFLAGS += -Dnuvasha_DATA_DIR=\"$(PREFIX)/share/nuvasha\"
CPPFLAGS += -Dnuvasha_SOURCE_DATA_DIR=\"$(CURDIR)/data\"
LOCALES := es_AR es_ES pt_BR pt_PT fr de it ja ko zh_CN zh_TW ru pl uk tr nl cs hu sv da fi nb ro vi id ar
LOCALE_MOS := $(LOCALES:%=build/locale/%/LC_MESSAGES/nuvasha.mo)

CFLAGS   := -std=c17 -Wall -Wextra -Wpedantic -MMD -MP
RELFLAGS := -O2
DBGFLAGS := -g -O0 -fsanitize=address,undefined -fno-sanitize-recover=all

BIN    := build/NuvashaLauncher
DBGBIN := build-debug/NuvashaLauncher

SRCS    := $(filter-out $(wildcard src/*-part.c),$(wildcard src/*.c))
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

.PHONY: all fast debug test test-debug test-lsan static-analysis pot clean install uninstall

all: $(if $(SRCS),$(BIN)) $(LOCALE_MOS)

# Convenience target for local development. Keep parallelism opt-in so normal
# `make` remains predictable for packaging and CI.
fast:
	+$(MAKE) -j$$(nproc) all

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
		-Dnuvasha_DATA_DIR=\"$(CURDIR)/data\" -Dnuvasha_BIN=\"$(CURDIR)/$(BIN)\" \
		-Dnuvasha_TEST_FIXTURES=\"$(CURDIR)/tests/fixtures\" \
		-Dnuvasha_SRC_DIR=\"$(CURDIR)/src\" -Dnuvasha_PO_DIR=\"$(CURDIR)/po\" \
		-o $@ $< $(TESTLIBOBJS) $(PKG_LIBS)

# Tests never write to the real data directory and never touch the
# network. Only the icon name audit opens a display, and it skips itself
# when there is none; the test that runs the launcher runs it with DISPLAY
# and display variables removed.
test: $(if $(SRCS),$(BIN)) $(TESTBINS)
	@pass=0; skip=0; for t in $(TESTBINS); do echo "RUN  $$t"; ./$$t; status=$$?; \
		if test $$status -eq 77; then skip=$$((skip + 1)); \
		elif test $$status -ne 0; then exit $$status; else pass=$$((pass + 1)); fi; done; \
		echo "PASS $$pass test(s), SKIP $$skip test(s)"

# Same tests, but every test and every src object is built with
# AddressSanitizer and UndefinedBehaviorSanitizer into build-debug/.
build-debug/tests/%: tests/%.c $(TESTLIBDBGOBJS) | build-debug/tests
	$(CC) $(CFLAGS) $(DBGFLAGS) -Isrc $(PKG_CFLAGS) \
		-Dnuvasha_DATA_DIR=\"$(CURDIR)/data\" -Dnuvasha_BIN=\"$(CURDIR)/$(BIN)\" \
		-Dnuvasha_TEST_FIXTURES=\"$(CURDIR)/tests/fixtures\" \
		-Dnuvasha_SRC_DIR=\"$(CURDIR)/src\" -Dnuvasha_PO_DIR=\"$(CURDIR)/po\" \
		-o $@ $< $(TESTLIBDBGOBJS) $(PKG_LIBS)

# The display guard test runs the release launcher, so test-debug builds both
# binaries.
test-debug: $(if $(SRCS),$(DBGBIN) $(BIN)) $(TESTDBGBINS)
	@pass=0; skip=0; for t in $(TESTDBGBINS); do echo "RUN  $$t"; ./$$t; status=$$?; \
		if test $$status -eq 77; then skip=$$((skip + 1)); \
		elif test $$status -ne 0; then exit $$status; else pass=$$((pass + 1)); fi; done; \
		echo "PASS $$pass test(s), SKIP $$skip test(s)"

# LeakSanitizer is included with the Linux AddressSanitizer runtime. Keep it
# explicit so CI does not silently lose leak checking when sanitizer defaults
# change. The options are set per test rather than around a recursive make,
# so the suite runs once and every test is the one being leak checked.
test-lsan: $(if $(SRCS),$(DBGBIN) $(BIN)) $(TESTDBGBINS)
	@pass=0; skip=0; for t in $(TESTDBGBINS); do echo "RUN  $$t"; \
	  ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
	  LSAN_OPTIONS=detect_leaks=1:exitcode=1 \
	  ./$$t; status=$$?; \
		if test $$status -eq 77; then skip=$$((skip + 1)); \
		elif test $$status -ne 0; then exit $$status; else pass=$$((pass + 1)); fi; done; \
		echo "PASS $$pass test(s), SKIP $$skip test(s)"

static-analysis:
	@set -e; for source in src/*.c; do \
	    case "$$source" in *-part.c) continue;; esac; \
	    $(CC) $(CPPFLAGS) -std=c17 -Wall -Wextra -Wpedantic -fanalyzer -c -o /dev/null \
	    $(PKG_CFLAGS) -Isrc "$$source"; done

pot:
	xgettext --from-code=UTF-8 --language=C --keyword=_ --keyword=N_ \
		--files-from=po/POTFILES.in --output=po/nuvasha.pot

install: $(if $(SRCS),$(BIN)) $(LOCALE_MOS)
	install -Dm755 $(BIN) "$(DESTDIR)$(PREFIX)/bin/NuvashaLauncher"
	install -Dm644 data/pack.png "$(DESTDIR)$(PREFIX)/share/nuvasha/pack.png"
	@for skin in data/skins/defaults/*.png; do \
		install -Dm644 "$$skin" "$(DESTDIR)$(PREFIX)/share/nuvasha/skins/defaults/$$(basename "$$skin")"; \
	done
	install -Dm644 data/com.github.nuvashalauncher.nuvasha.desktop \
		"$(DESTDIR)$(PREFIX)/share/applications/com.github.nuvashalauncher.nuvasha.desktop"
	install -Dm644 data/icons/scalable/apps/com.github.nuvashalauncher.nuvasha.svg \
		"$(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps/com.github.nuvashalauncher.nuvasha.svg"
	install -Dm644 data/com.github.nuvashalauncher.nuvasha.metainfo.xml \
		"$(DESTDIR)$(PREFIX)/share/metainfo/com.github.nuvashalauncher.nuvasha.metainfo.xml"
	@for lang in $(LOCALES); do \
		install -Dm644 build/locale/$$lang/LC_MESSAGES/nuvasha.mo \
			"$(DESTDIR)$(PREFIX)/share/locale/$$lang/LC_MESSAGES/nuvasha.mo"; \
	done

# The same list the install loop uses, so the catalogs go back out with
# the rest of the launcher.
uninstall:
	rm -f "$(DESTDIR)$(PREFIX)/bin/NuvashaLauncher"
	rm -f "$(DESTDIR)$(PREFIX)/share/nuvasha/pack.png"
	rm -rf "$(DESTDIR)$(PREFIX)/share/nuvasha/skins/defaults"
	rm -f "$(DESTDIR)$(PREFIX)/share/applications/com.github.nuvashalauncher.nuvasha.desktop"
	rm -f "$(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps/com.github.nuvashalauncher.nuvasha.svg"
	rm -f "$(DESTDIR)$(PREFIX)/share/metainfo/com.github.nuvashalauncher.nuvasha.metainfo.xml"
	@for lang in $(LOCALES); do \
		rm -f "$(DESTDIR)$(PREFIX)/share/locale/$$lang/LC_MESSAGES/nuvasha.mo"; \
	done

clean:
	rm -rf build build-debug

build build-debug build/tests build-debug/tests:
	mkdir -p $@

build/locale/%/LC_MESSAGES/nuvasha.mo: po/%.po
	mkdir -p $(dir $@)
	msgfmt -o $@ $<

-include $(RELDEPS) $(DBGDEPS) $(TESTDEPS) $(TESTDBGDEPS)
