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

.PHONY: all debug test test-debug clean install

all: $(if $(SRCS),$(BIN))

debug: $(if $(SRCS),$(DBGBIN))

$(BIN): $(RELOBJS) | build
	$(CC) $(RELFLAGS) -o $@ $(RELOBJS) $(PKG_LIBS)

$(DBGBIN): $(DBGOBJS) | build-debug
	$(CC) $(DBGFLAGS) -o $@ $(DBGOBJS) $(PKG_LIBS)

build/%.o: src/%.c | build
	$(CC) $(CFLAGS) $(RELFLAGS) $(PKG_CFLAGS) -c -o $@ $<

build-debug/%.o: src/%.c | build-debug
	$(CC) $(CFLAGS) $(DBGFLAGS) $(PKG_CFLAGS) -c -o $@ $<

build/tests/%: tests/%.c $(TESTLIBOBJS) | build/tests
	$(CC) $(CFLAGS) $(RELFLAGS) -Isrc $(PKG_CFLAGS) \
		-DAML_DATA_DIR=\"$(CURDIR)/data\" -DAML_BIN=\"$(CURDIR)/$(BIN)\" \
		-DAML_TEST_FIXTURES=\"$(CURDIR)/tests/fixtures\" \
		-o $@ $< $(TESTLIBOBJS) $(PKG_LIBS)

# Tests never open a display: the only test that runs the launcher runs it
# with DISPLAY and WAYLAND_DISPLAY removed.
test: $(if $(SRCS),$(BIN)) $(TESTBINS)
	@set -e; for t in $(TESTBINS); do echo "RUN  $$t"; ./$$t; done; \
	echo "PASS $(words $(TESTBINS)) test(s)"

# Same tests, but every test and every src object is built with
# AddressSanitizer and UndefinedBehaviorSanitizer into build-debug/.
build-debug/tests/%: tests/%.c $(TESTLIBDBGOBJS) | build-debug/tests
	$(CC) $(CFLAGS) $(DBGFLAGS) -Isrc $(PKG_CFLAGS) \
		-DAML_DATA_DIR=\"$(CURDIR)/data\" -DAML_BIN=\"$(CURDIR)/$(BIN)\" \
		-DAML_TEST_FIXTURES=\"$(CURDIR)/tests/fixtures\" \
		-o $@ $< $(TESTLIBDBGOBJS) $(PKG_LIBS)

# The Wayland guard test runs the release launcher, so test-debug builds
# both binaries.
test-debug: $(if $(SRCS),$(DBGBIN) $(BIN)) $(TESTDBGBINS)
	@set -e; for t in $(TESTDBGBINS); do echo "RUN  $$t"; ./$$t; done; \
	echo "PASS $(words $(TESTDBGBINS)) test(s)"

install: $(if $(SRCS),$(BIN))
	install -Dm755 $(BIN) $(DESTDIR)$(PREFIX)/bin/AML
	install -Dm644 data/io.github.Flia7.AML.desktop \
		$(DESTDIR)$(PREFIX)/share/applications/io.github.Flia7.AML.desktop

clean:
	rm -rf build build-debug

build build-debug build/tests build-debug/tests:
	mkdir -p $@

-include $(RELDEPS) $(DBGDEPS) $(TESTDEPS) $(TESTDBGDEPS)
