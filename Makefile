# Copyright (C) 2026 Flia
# SPDX-License-Identifier: GPL-3.0-only
#
# Hand-written Makefile for AML, Another Minecraft Launcher.
# Release objects live in build/, sanitizer objects in build-debug/.

CC         := gcc
PKG_CONFIG ?= pkg-config
PREFIX     ?= /usr/local
VERSION    := $(strip $(shell cat VERSION))

# Libraries AML links against. GTK loads the available display backend at
# runtime, so AML does not link to a window-system backend directly.
# gio-unix-2.0 is the Linux part of GIO itself (no display involved) and
# provides the output stream that writes a download into its temporary
# file descriptor.
PKGS := gtk4 libadwaita-1 json-glib-1.0 libsoup-3.0 libarchive gio-unix-2.0 epoxy

# pkg-config prints -I paths; use -isystem so warnings are only about our
# own code and not about the GTK/GLib headers themselves.
PKG_CFLAGS := $(shell $(PKG_CONFIG) --cflags $(PKGS) | \
	sed -e 's/ -I/ -isystem /g' -e 's/^-I/-isystem /')
PKG_LIBS := $(shell $(PKG_CONFIG) --libs $(PKGS)) -lm
CPPFLAGS += -DAML_LOCALE_DIR=\"$(PREFIX)/share/locale\"
CPPFLAGS += -DAML_VERSION=\"$(VERSION)\"
CPPFLAGS += -DAML_DATA_DIR=\"$(PREFIX)/share/AML\"
CPPFLAGS += -DAML_SOURCE_DATA_DIR=\"$(CURDIR)/data\"
LOCALES := es_AR es_ES pt_BR fr de it ja ko zh_CN ru pl uk tr nl
LOCALE_MOS := $(LOCALES:%=build/locale/%/LC_MESSAGES/aml.mo)

CFLAGS   := -std=c17 -Wall -Wextra -Wpedantic -MMD -MP
RELFLAGS := -O2
DBGFLAGS := -g -O0 -fsanitize=address,undefined -fno-sanitize-recover=all

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

.PHONY: all debug test test-debug test-lsan static-analysis pot clean install uninstall

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
		-DAML_DATA_DIR=\"$(CURDIR)/data\" -DAML_BIN=\"$(CURDIR)/$(BIN)\" \
		-DAML_TEST_FIXTURES=\"$(CURDIR)/tests/fixtures\" \
		-DAML_SRC_DIR=\"$(CURDIR)/src\" -DAML_PO_DIR=\"$(CURDIR)/po\" \
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
	    $(CC) $(CPPFLAGS) -std=c17 -Wall -Wextra -Wpedantic -fanalyzer -c -o /dev/null \
	    $(PKG_CFLAGS) -Isrc "$$source"; done

pot:
	xgettext --from-code=UTF-8 --language=C --keyword=_ --keyword=N_ \
		--files-from=po/POTFILES.in --output=po/aml.pot

install: $(if $(SRCS),$(BIN)) $(LOCALE_MOS)
	install -Dm755 $(BIN) "$(DESTDIR)$(PREFIX)/bin/AML"
	install -Dm644 data/skins/steve.png "$(DESTDIR)$(PREFIX)/share/AML/skins/steve.png"
	@for skin in data/skins/defaults/*.png; do \
		install -Dm644 "$$skin" "$(DESTDIR)$(PREFIX)/share/AML/skins/defaults/$$(basename "$$skin")"; \
	done
	install -Dm644 data/io.gitlab.anotherminecraftlauncher.AML.desktop \
		"$(DESTDIR)$(PREFIX)/share/applications/io.gitlab.anotherminecraftlauncher.AML.desktop"
	install -Dm644 data/icons/scalable/apps/io.gitlab.anotherminecraftlauncher.AML.svg \
		"$(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps/io.gitlab.anotherminecraftlauncher.AML.svg"
	install -Dm644 data/io.gitlab.anotherminecraftlauncher.AML.metainfo.xml \
		"$(DESTDIR)$(PREFIX)/share/metainfo/io.gitlab.anotherminecraftlauncher.AML.metainfo.xml"
	@for lang in $(LOCALES); do \
		install -Dm644 build/locale/$$lang/LC_MESSAGES/aml.mo \
			"$(DESTDIR)$(PREFIX)/share/locale/$$lang/LC_MESSAGES/aml.mo"; \
	done

# The same list the install loop uses, so the catalogs go back out with
# the rest of the launcher.
uninstall:
	rm -f "$(DESTDIR)$(PREFIX)/bin/AML"
	rm -f "$(DESTDIR)$(PREFIX)/share/AML/skins/steve.png"
	rm -rf "$(DESTDIR)$(PREFIX)/share/AML/skins/defaults"
	rm -f "$(DESTDIR)$(PREFIX)/share/applications/io.gitlab.anotherminecraftlauncher.AML.desktop"
	rm -f "$(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps/io.gitlab.anotherminecraftlauncher.AML.svg"
	rm -f "$(DESTDIR)$(PREFIX)/share/metainfo/io.gitlab.anotherminecraftlauncher.AML.metainfo.xml"
	@for lang in $(LOCALES); do \
		rm -f "$(DESTDIR)$(PREFIX)/share/locale/$$lang/LC_MESSAGES/aml.mo"; \
	done

clean:
	rm -rf build build-debug

build build-debug build/tests build-debug/tests:
	mkdir -p $@

build/locale/%/LC_MESSAGES/aml.mo: po/%.po
	mkdir -p $(dir $@)
	msgfmt -o $@ $<

-include $(RELDEPS) $(DBGDEPS) $(TESTDEPS) $(TESTDBGDEPS)
