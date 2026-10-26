# Copyright (C) 2026 Flia
# SPDX-License-Identifier: GPL-3.0-only
#
# Hand-written Makefile for nuvashalauncher.
# Release objects live in build/, sanitizer objects in build-debug/.

CC         := gcc
PKG_CONFIG ?= pkg-config
PREFIX     ?= /usr/local
VERSION    := $(strip $(shell cat VERSION))
FLATPAK    ?= 0

# Libraries nuvashalauncher links against. GTK loads the available display backend at
# runtime, so nuvashalauncher does not link to a window-system backend directly.
# gio-unix-2.0 is the Linux part of GIO itself (no display involved) and
# provides the output stream that writes a download into its temporary
# file descriptor.
PKGS := gtk4 libadwaita-1 json-glib-1.0 libsoup-3.0 libarchive gio-unix-2.0 epoxy

# pkg-config prints -I paths; use -isystem so warnings are only about our
# own code and not about the GTK/GLib headers themselves.
PKG_CFLAGS := $(shell $(PKG_CONFIG) --cflags $(PKGS) | \
	sed -e 's/ -I/ -isystem /g' -e 's/^-I/-isystem /')
PKG_LIBS := $(shell $(PKG_CONFIG) --libs $(PKGS)) -lm
ifeq ($(strip $(PKG_CFLAGS)),)
$(error pkg-config could not find the required development packages: $(PKGS))
endif
CPPFLAGS += -Dnuvashalauncher_LOCALE_DIR=\"$(PREFIX)/share/locale\"
CPPFLAGS += -Dnuvashalauncher_VERSION=\"$(VERSION)\"
CPPFLAGS += -Dnuvashalauncher_DATA_DIR=\"$(PREFIX)/share/nuvashalauncher\"
CPPFLAGS += -Dnuvashalauncher_SOURCE_DATA_DIR=\"$(CURDIR)/data\"
CPPFLAGS += -Dnuvashalauncher_BUILD_SCHEMA_DIR=\"$(CURDIR)/build/schemas\"
ifeq ($(FLATPAK),1)
CPPFLAGS += -Dnuvashalauncher_FLATPAK
endif
LOCALES := es_AR es_ES pt_BR pt_PT fr de it ja ko zh_CN zh_TW ru pl uk tr nl cs hu sv da fi nb ro vi id ar
LOCALE_MOS := $(LOCALES:%=build/locale/%/LC_MESSAGES/nuvashalauncher.mo)
SCHEMA_XML := data/com.github.nuvashalauncher.nuvashalauncher.gschema.xml
SCHEMA_COMPILED := build/schemas/gschemas.compiled

CFLAGS   := -std=c17 -Wall -Wextra -Wpedantic -MMD -MP
RELFLAGS := -O2
DBGFLAGS := -g -O0 -fsanitize=address,undefined -fno-sanitize-recover=all

BIN    := build/nuvashalauncher
DBGBIN := build-debug/nuvashalauncher

SRCS    := $(filter-out $(wildcard src/*-part.c),$(wildcard src/*.c))
RELOBJS := $(patsubst src/%.c,build/%.o,$(SRCS))
DBGOBJS := $(patsubst src/%.c,build-debug/%.o,$(SRCS))
RESOURCE_C := build/nuvashalauncher-resources.c
RESOURCE_O := build/nuvashalauncher-resources.o
RESOURCE_DBG_O := build-debug/nuvashalauncher-resources.o
RELOBJS += $(RESOURCE_O)
DBGOBJS += $(RESOURCE_DBG_O)
RELDEPS := $(RELOBJS:.o=.d)
DBGDEPS := $(DBGOBJS:.o=.d)

# Tests link the src files they need; src/main.c is excluded because it
# owns main().
TESTLIBSRCS := $(filter-out src/main.c,$(SRCS))
TESTLIBOBJS := $(patsubst src/%.c,build/%.o,$(TESTLIBSRCS))
TESTLIBDBGOBJS := $(patsubst src/%.c,build-debug/%.o,$(TESTLIBSRCS))
TESTLIBOBJS += $(RESOURCE_O)
TESTLIBDBGOBJS += $(RESOURCE_DBG_O)
TESTSRCS    := $(wildcard tests/test-*.c)
TESTBINS    := $(patsubst tests/%.c,build/tests/%,$(TESTSRCS))
TESTDEPS    := $(addsuffix .d,$(TESTBINS))
TESTDBGBINS := $(patsubst tests/%.c,build-debug/tests/%,$(TESTSRCS))
TESTDBGDEPS := $(addsuffix .d,$(TESTDBGBINS))

.PHONY: all fast debug test test-display test-flatpak-runtime test-debug test-lsan static-analysis pot clean install uninstall

all: $(if $(SRCS),$(BIN)) $(LOCALE_MOS) $(SCHEMA_COMPILED)

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
	$(CC) $(CPPFLAGS) $(CFLAGS) $(RELFLAGS) $(PKG_CFLAGS) -Wno-overlength-strings -c -o $@ $<

$(RESOURCE_C): data/nuvashalauncher.gresource.xml data/nuvashalauncher.css $(wildcard data/ui/*.ui) | build
	glib-compile-resources --generate-source --target=$@ --sourcedir=data $<

$(RESOURCE_O): $(RESOURCE_C) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(RELFLAGS) $(PKG_CFLAGS) -Wno-overlength-strings -c -o $@ $<

build-debug/%.o: src/%.c | build-debug
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DBGFLAGS) $(PKG_CFLAGS) -c -o $@ $<

$(RESOURCE_DBG_O): $(RESOURCE_C) | build-debug
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DBGFLAGS) $(PKG_CFLAGS) -Wno-overlength-strings -c -o $@ $(RESOURCE_C)

build/tests/%: tests/%.c $(TESTLIBOBJS) | build/tests
	$(CC) $(CFLAGS) $(RELFLAGS) -Isrc $(PKG_CFLAGS) \
		-Dnuvashalauncher_DATA_DIR=\"$(CURDIR)/data\" -Dnuvashalauncher_BIN=\"$(CURDIR)/$(BIN)\" \
		-Dnuvashalauncher_BUILD_SCHEMA_DIR=\"$(CURDIR)/build/schemas\" \
		-Dnuvashalauncher_TEST_FIXTURES=\"$(CURDIR)/tests/fixtures\" \
		-Dnuvashalauncher_SRC_DIR=\"$(CURDIR)/src\" -Dnuvashalauncher_PO_DIR=\"$(CURDIR)/po\" \
		-o $@ $< $(TESTLIBOBJS) $(PKG_LIBS)

# Tests never write to the real data directory and never touch the network.
# Display-backed tests use invisible windows so they can exercise GTK without
# covering the developer's desktop; they skip when no display is available.
test: $(if $(SRCS),$(BIN)) $(TESTBINS) $(SCHEMA_COMPILED)
	@set -e; test_tmp=$$(mktemp -d "$(CURDIR)/build/test-tmp-XXXXXX"); \
	trap 'rm -rf "$$test_tmp"' EXIT HUP INT TERM; \
	pass=0; skip=0; for t in $(TESTBINS); do echo "RUN  $$t"; if TMPDIR="$$test_tmp" GSETTINGS_SCHEMA_DIR="$(CURDIR)/build/schemas" GSETTINGS_BACKEND=memory ./$$t; then status=0; else status=$$?; fi; \
		if test $$status -eq 77; then skip=$$((skip + 1)); \
		elif test $$status -ne 0; then exit $$status; else pass=$$((pass + 1)); fi; done; \
		echo "PASS $$pass test(s), SKIP $$skip test(s)"

# Run the suite with an existing Wayland session when available. Fall back to
# Xvfb so display-backed coverage still runs in headless CI.
test-display:
	@if test -n "$$WAYLAND_DISPLAY" && test -n "$$XDG_RUNTIME_DIR" && \
	   test -S "$$XDG_RUNTIME_DIR/$$WAYLAND_DISPLAY"; then \
		echo "RUN test-display: existing Wayland session"; \
		GDK_BACKEND=wayland $(MAKE) test; \
	elif command -v xvfb-run >/dev/null 2>&1; then \
		echo "RUN test-display: Xvfb fallback"; \
	if command -v dbus-run-session >/dev/null 2>&1; then \
		dbus-run-session -- xvfb-run --auto-servernum env GDK_BACKEND=x11 $(MAKE) test; \
	else \
		xvfb-run --auto-servernum env GDK_BACKEND=x11 $(MAKE) test; \
	fi; \
	else \
		echo "SKIP test-display: no Wayland session or Xvfb"; exit 77; \
	fi

# Smoke-test an installed Flatpak without launching Minecraft. The test checks
# the packaged command, sandbox data directory, host-data isolation, and the
# network permission used by the launcher for Mojang metadata.
test-flatpak-runtime:
	@if ! command -v flatpak >/dev/null 2>&1; then \
		echo "SKIP test-flatpak-runtime: flatpak is unavailable"; exit 77; \
	fi
	@if ! flatpak info com.github.nuvashalauncher.nuvashalauncher >/dev/null 2>&1; then \
		echo "SKIP test-flatpak-runtime: application is not installed"; exit 77; \
	fi
	@set -eu; \
	version=$$(flatpak run com.github.nuvashalauncher.nuvashalauncher --version); \
	test "$$version" = "nuvashalauncher $(VERSION)"; \
	flatpak run com.github.nuvashalauncher.nuvashalauncher --list-instances >/dev/null; \
	flatpak run com.github.nuvashalauncher.nuvashalauncher --list-accounts >/dev/null; \
	flatpak run --command=sh com.github.nuvashalauncher.nuvashalauncher -c '\
		set -eu; \
		test -w "$$XDG_DATA_HOME"; \
		probe="$$XDG_DATA_HOME/.runtime-smoke"; \
		printf flatpak-runtime > "$$probe"; \
		test "$$(cat "$$probe")" = flatpak-runtime; \
		rm -f "$$probe"; \
		! test -e "$$HOME/.local/share/nuvashalauncher/accounts.json"; \
		curl --fail --silent --show-error --max-time 10 \
			https://piston-meta.mojang.com/mc/game/version_manifest_v2.json \
			-o /dev/null'; \
	echo "PASS Flatpak runtime smoke test"

# Same tests, but every test and every src object is built with
# AddressSanitizer and UndefinedBehaviorSanitizer into build-debug/.
build-debug/tests/%: tests/%.c $(TESTLIBDBGOBJS) | build-debug/tests
	$(CC) $(CFLAGS) $(DBGFLAGS) -Isrc $(PKG_CFLAGS) \
		-Dnuvashalauncher_DATA_DIR=\"$(CURDIR)/data\" -Dnuvashalauncher_BIN=\"$(CURDIR)/$(BIN)\" \
		-Dnuvashalauncher_BUILD_SCHEMA_DIR=\"$(CURDIR)/build/schemas\" \
		-Dnuvashalauncher_TEST_FIXTURES=\"$(CURDIR)/tests/fixtures\" \
		-Dnuvashalauncher_SRC_DIR=\"$(CURDIR)/src\" -Dnuvashalauncher_PO_DIR=\"$(CURDIR)/po\" \
		-o $@ $< $(TESTLIBDBGOBJS) $(PKG_LIBS)

# The display guard test runs the release launcher, so test-debug builds both
# binaries.
test-debug: $(if $(SRCS),$(DBGBIN) $(BIN)) $(TESTDBGBINS) $(SCHEMA_COMPILED)
	@set -e; test_tmp=$$(mktemp -d "$(CURDIR)/build/test-tmp-XXXXXX"); \
	trap 'rm -rf "$$test_tmp"' EXIT HUP INT TERM; \
	pass=0; skip=0; for t in $(TESTDBGBINS); do echo "RUN  $$t"; if TMPDIR="$$test_tmp" GSETTINGS_SCHEMA_DIR="$(CURDIR)/build/schemas" GSETTINGS_BACKEND=memory ./$$t; then status=0; else status=$$?; fi; \
		if test $$status -eq 77; then skip=$$((skip + 1)); \
		elif test $$status -ne 0; then exit $$status; else pass=$$((pass + 1)); fi; done; \
		echo "PASS $$pass test(s), SKIP $$skip test(s)"

# LeakSanitizer is included with the Linux AddressSanitizer runtime. Keep it
# explicit so CI does not silently lose leak checking when sanitizer defaults
# change. The options are set per test rather than around a recursive make,
# so the suite runs once and every test is the one being leak checked.
test-lsan: $(if $(SRCS),$(DBGBIN) $(BIN)) $(TESTDBGBINS) $(SCHEMA_COMPILED)
	@set -e; test_tmp=$$(mktemp -d "$(CURDIR)/build/test-tmp-XXXXXX"); \
	trap 'rm -rf "$$test_tmp"' EXIT HUP INT TERM; \
	pass=0; skip=0; for t in $(TESTDBGBINS); do echo "RUN  $$t"; \
		  if ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
		     LSAN_OPTIONS=detect_leaks=1:exitcode=1 \
		     TMPDIR="$$test_tmp" \
		     GSETTINGS_SCHEMA_DIR="$(CURDIR)/build/schemas" GSETTINGS_BACKEND=memory ./$$t; then status=0; else status=$$?; fi; \
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
		--files-from=po/POTFILES.in --output=po/nuvashalauncher.pot

install: $(if $(SRCS),$(BIN)) $(LOCALE_MOS) $(SCHEMA_COMPILED)
	install -Dm755 $(BIN) "$(DESTDIR)$(PREFIX)/bin/nuvashalauncher"
	install -Dm644 data/pack.png "$(DESTDIR)$(PREFIX)/share/nuvashalauncher/pack.png"
	install -Dm644 data/nuvashalauncher.css "$(DESTDIR)$(PREFIX)/share/nuvashalauncher/nuvashalauncher.css"
	@for skin in data/skins/defaults/*.png; do \
		install -Dm644 "$$skin" "$(DESTDIR)$(PREFIX)/share/nuvashalauncher/skins/defaults/$$(basename "$$skin")"; \
	done
	install -Dm644 data/com.github.nuvashalauncher.nuvashalauncher.desktop \
		"$(DESTDIR)$(PREFIX)/share/applications/com.github.nuvashalauncher.nuvashalauncher.desktop"
	install -Dm644 data/nuvashalauncher.bash \
		"$(DESTDIR)$(PREFIX)/share/bash-completion/completions/nuvashalauncher"
	install -Dm644 data/com.github.nuvashalauncher.nuvashalauncher.xml \
		"$(DESTDIR)$(PREFIX)/share/mime/packages/com.github.nuvashalauncher.nuvashalauncher.xml"
	install -Dm644 data/icons/scalable/apps/com.github.nuvashalauncher.nuvashalauncher.svg \
		"$(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps/com.github.nuvashalauncher.nuvashalauncher.svg"
	install -Dm644 data/com.github.nuvashalauncher.nuvashalauncher.metainfo.xml \
		"$(DESTDIR)$(PREFIX)/share/metainfo/com.github.nuvashalauncher.nuvashalauncher.metainfo.xml"
	install -Dm644 $(SCHEMA_COMPILED) \
		"$(DESTDIR)$(PREFIX)/share/glib-2.0/schemas/gschemas.compiled"
	@for lang in $(LOCALES); do \
		install -Dm644 build/locale/$$lang/LC_MESSAGES/nuvashalauncher.mo \
			"$(DESTDIR)$(PREFIX)/share/locale/$$lang/LC_MESSAGES/nuvashalauncher.mo"; \
	done

# The same list the install loop uses, so the catalogs go back out with
# the rest of the launcher.
uninstall:
	rm -f "$(DESTDIR)$(PREFIX)/bin/nuvashalauncher"
	rm -f "$(DESTDIR)$(PREFIX)/share/nuvashalauncher/pack.png"
	rm -f "$(DESTDIR)$(PREFIX)/share/nuvashalauncher/nuvashalauncher.css"
	rm -rf "$(DESTDIR)$(PREFIX)/share/nuvashalauncher/skins/defaults"
	rm -f "$(DESTDIR)$(PREFIX)/share/applications/com.github.nuvashalauncher.nuvashalauncher.desktop"
	rm -f "$(DESTDIR)$(PREFIX)/share/bash-completion/completions/nuvashalauncher"
	rm -f "$(DESTDIR)$(PREFIX)/share/mime/packages/com.github.nuvashalauncher.nuvashalauncher.xml"
	rm -f "$(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps/com.github.nuvashalauncher.nuvashalauncher.svg"
	rm -f "$(DESTDIR)$(PREFIX)/share/metainfo/com.github.nuvashalauncher.nuvashalauncher.metainfo.xml"
	rm -f "$(DESTDIR)$(PREFIX)/share/glib-2.0/schemas/gschemas.compiled"
	@for lang in $(LOCALES); do \
		rm -f "$(DESTDIR)$(PREFIX)/share/locale/$$lang/LC_MESSAGES/nuvashalauncher.mo"; \
	done

clean:
	rm -rf build build-debug

build build-debug build/tests build-debug/tests:
	mkdir -p $@

$(SCHEMA_COMPILED): $(SCHEMA_XML) | build/schemas
	glib-compile-schemas --targetdir=build/schemas data

build/schemas:
	mkdir -p $@

build/locale/%/LC_MESSAGES/nuvashalauncher.mo: po/%.po
	mkdir -p $(dir $@)
	msgfmt -o $@ $<

-include $(RELDEPS) $(DBGDEPS) $(TESTDEPS) $(TESTDBGDEPS)
