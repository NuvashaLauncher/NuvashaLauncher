# Copyright (C) 2026 Flia
# SPDX-License-Identifier: GPL-3.0-only
#
# Hand-written Makefile for nuvashalauncher.
# Release objects live in build/, sanitizer objects in build-debug/.

CC         := gcc
AR         ?= ar
PKG_CONFIG ?= pkg-config
PREFIX     ?= /usr/local
VERSION    := $(strip $(shell cat VERSION))
FLATPAK    ?= 0

# Libraries Nuvasha Launcher links against. GTK loads the available display backend at
# runtime, so Nuvasha Launcher does not link to a window-system backend directly.
# gio-unix-2.0 is the Linux part of GIO itself (no display involved) and
# provides the output stream that writes a download into its temporary
# file descriptor.
PKGS := gtk4 libadwaita-1 json-glib-1.0 libsoup-3.0 libarchive gio-unix-2.0 epoxy libsecret-1

# Goals that compile nothing and so must keep working on hosts without the
# development packages. test-flatpak-runtime only drives the flatpak CLI to
# smoke-test an already-installed bundle; clean only deletes build output.
NO_BUILD_GOALS := test-flatpak-runtime check-potfiles clean
ifneq ($(filter-out $(NO_BUILD_GOALS),$(MAKECMDGOALS)),)
NEEDS_BUILD := 1
else ifeq ($(MAKECMDGOALS),)
# No goal named: the default goal builds the launcher.
NEEDS_BUILD := 1
endif

ifeq ($(NEEDS_BUILD),1)
# pkg-config prints -I paths; use -isystem so warnings are only about our
# own code and not about the GTK/GLib headers themselves.
PKG_CFLAGS := $(shell $(PKG_CONFIG) --cflags $(PKGS) | \
	sed -e 's/ -I/ -isystem /g' -e 's/^-I/-isystem /')
PKG_LIBS := $(shell $(PKG_CONFIG) --libs $(PKGS)) -lm
ifeq ($(strip $(PKG_CFLAGS)),)
$(error pkg-config could not find the required development packages: $(PKGS))
endif
else
# This goal compiles nothing, so skip pkg-config entirely: probing it here
# would only print "not found" noise on hosts without the dev packages.
PKG_CFLAGS :=
PKG_LIBS    :=
endif
CPPFLAGS += -DNUVASHALAUNCHER_LOCALE_DIR=\"$(PREFIX)/share/locale\"
CPPFLAGS += -DNUVASHALAUNCHER_VERSION=\"$(VERSION)\"
CPPFLAGS += -DNUVASHALAUNCHER_DATA_DIR=\"$(PREFIX)/share/nuvashalauncher\"
CPPFLAGS += -DNUVASHALAUNCHER_SOURCE_DATA_DIR=\"$(CURDIR)/data\"
CPPFLAGS += -DNUVASHALAUNCHER_BUILD_SCHEMA_DIR=\"$(CURDIR)/build/schemas\"
ifeq ($(FLATPAK),1)
CPPFLAGS += -DNUVASHALAUNCHER_FLATPAK
endif
LOCALES := es_AR es_ES pt_BR pt_PT fr de it ja ko zh_CN zh_TW ru pl uk tr nl cs hu sv da fi nb ro vi id ar
LOCALE_MOS := $(LOCALES:%=build/locale/%/LC_MESSAGES/nuvashalauncher.mo)
SCHEMA_XML := data/com.github.NuvashaLauncher.NuvashaLauncher.gschema.xml
SCHEMA_COMPILED := build/schemas/gschemas.compiled

CFLAGS     ?= -std=c17 -Wall -Wextra -Wpedantic -MMD -MP
RELFLAGS   := -O2 -D_FORTIFY_SOURCE=2 -fstack-protector-strong -fPIE
RELLDFLAGS := -pie -Wl,-z,relro,-z,now
DBGFLAGS   := -g -O1 -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=all

BIN    := build/nuvashalauncher
DBGBIN := build-debug/nuvashalauncher

SRCS    := $(wildcard src/*.c)
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
TESTLIBA    := build/libnuvashalauncher.a
TESTLIBDBGA := build-debug/libnuvashalauncher.a
TESTSRCS    := $(wildcard tests/test-*.c)
TESTBINS    := $(patsubst tests/%.c,build/tests/%,$(TESTSRCS))
TESTDEPS    := $(addsuffix .d,$(TESTBINS))
TESTDBGBINS := $(patsubst tests/%.c,build-debug/tests/%,$(TESTSRCS))
TESTDBGDEPS := $(addsuffix .d,$(TESTDBGBINS))

.PHONY: all fast debug test test-display test-flatpak-runtime test-debug test-lsan static-analysis pot check-potfiles clean install uninstall

all: $(if $(SRCS),$(BIN)) $(LOCALE_MOS) $(SCHEMA_COMPILED)

# Convenience target for local development. Keep parallelism opt-in so normal
# `make` remains predictable for packaging and CI.
fast:
	+$(MAKE) -j$$(nproc) all

debug: $(if $(SRCS),$(DBGBIN))

$(BIN): $(RELOBJS) | build
	$(CC) $(RELFLAGS) $(RELLDFLAGS) -o $@ $(RELOBJS) $(PKG_LIBS)

$(DBGBIN): $(DBGOBJS) | build-debug
	$(CC) $(DBGFLAGS) -o $@ $(DBGOBJS) $(PKG_LIBS)

build/%.o: src/%.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(RELFLAGS) $(PKG_CFLAGS) -Wno-overlength-strings -c -o $@ $<

$(RESOURCE_C): data/nuvashalauncher.gresource.xml data/nuvashalauncher.css $(wildcard data/ui/*.ui) | build
	glib-compile-resources --generate-source --target=$@ --sourcedir=data $<

$(RESOURCE_O): $(RESOURCE_C) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(RELFLAGS) $(PKG_CFLAGS) -Wno-overlength-strings -c -o $@ $<

build-debug/%.o: src/%.c | build-debug
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DBGFLAGS) $(PKG_CFLAGS) -Wno-overlength-strings -c -o $@ $<

$(RESOURCE_DBG_O): $(RESOURCE_C) | build-debug
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DBGFLAGS) $(PKG_CFLAGS) -Wno-overlength-strings -c -o $@ $(RESOURCE_C)

$(TESTLIBA): $(TESTLIBOBJS) | build
	rm -f $@
	$(AR) rcs $@ $(TESTLIBOBJS)

build/tests/%: tests/%.c $(TESTLIBA) $(RESOURCE_O) | build/tests
	$(CC) $(CFLAGS) $(RELFLAGS) $(RELLDFLAGS) -Wl,--as-needed -Isrc $(PKG_CFLAGS) \
		-DNUVASHALAUNCHER_DATA_DIR=\"$(CURDIR)/data\" -DNUVASHALAUNCHER_BIN=\"$(CURDIR)/$(BIN)\" \
		-DNUVASHALAUNCHER_BUILD_SCHEMA_DIR=\"$(CURDIR)/build/schemas\" \
		-DNUVASHALAUNCHER_TEST_FIXTURES=\"$(CURDIR)/tests/fixtures\" \
		-DNUVASHALAUNCHER_SRC_DIR=\"$(CURDIR)/src\" -DNUVASHALAUNCHER_PO_DIR=\"$(CURDIR)/po\" \
		-o $@ $< $(RESOURCE_O) $(TESTLIBA) $(PKG_LIBS)

TESTRUNNER  := build/test-runner

$(TESTRUNNER): tests/runner.c | build
	$(CC) -std=c17 -Wall -Wextra -Wpedantic -O2 -pthread -o $@ $<

define run_test_suite
	@$(2) ./$(TESTRUNNER) --schema-dir "$(CURDIR)/build/schemas" $(1)
endef

# Tests never write to the real data directory and never touch the network.
# Display-backed tests use invisible windows so they can exercise GTK without
# covering the developer's desktop; they skip when no display is available.
test: $(if $(SRCS),$(BIN)) $(TESTRUNNER) $(TESTBINS) $(SCHEMA_COMPILED)
	$(call run_test_suite,$(TESTBINS),)

# Run the same suite with an existing Wayland session. Display-backed GTK
# coverage is intentionally skipped when no Wayland compositor is available.
test-display:
	@if test -n "$$WAYLAND_DISPLAY" && test -n "$$XDG_RUNTIME_DIR" && \
	   test -S "$$XDG_RUNTIME_DIR/$$WAYLAND_DISPLAY"; then \
		echo "RUN test-display: existing Wayland session"; \
		GDK_BACKEND=wayland $(MAKE) test; \
	else \
		echo "SKIP test-display: no Wayland session"; exit 77; \
	fi

# Smoke-test an installed Flatpak without launching Minecraft. The test checks
# the packaged command, sandbox data directory, and host-data isolation. It is
# deliberately local: automated tests must not contact Mojang services.
test-flatpak-runtime:
	@if ! command -v flatpak >/dev/null 2>&1; then \
		echo "SKIP test-flatpak-runtime: flatpak is unavailable"; exit 77; \
	fi
	@if ! flatpak info com.github.NuvashaLauncher.NuvashaLauncher >/dev/null 2>&1; then \
		echo "SKIP test-flatpak-runtime: application is not installed"; exit 77; \
	fi
	@set -eu; \
	version=$$(flatpak run com.github.NuvashaLauncher.NuvashaLauncher --version); \
	test "$$version" = "Nuvasha Launcher $(VERSION)"; \
	flatpak run com.github.NuvashaLauncher.NuvashaLauncher --list-instances >/dev/null; \
	flatpak run com.github.NuvashaLauncher.NuvashaLauncher --list-accounts >/dev/null; \
	flatpak run --command=sh com.github.NuvashaLauncher.NuvashaLauncher -c '\
		set -eu; \
		test -w "$$XDG_DATA_HOME"; \
		probe="$$XDG_DATA_HOME/.runtime-smoke"; \
		printf flatpak-runtime > "$$probe"; \
		test "$$(cat "$$probe")" = flatpak-runtime; \
		rm -f "$$probe"; \
		! test -e "$$HOME/.local/share/nuvashalauncher/accounts.json"'; \
	echo "PASS Flatpak runtime smoke test"

# Same tests, but every test and every src object is built with
# AddressSanitizer and UndefinedBehaviorSanitizer into build-debug/.
$(TESTLIBDBGA): $(TESTLIBDBGOBJS) | build-debug
	rm -f $@
	$(AR) rcs $@ $(TESTLIBDBGOBJS)

build-debug/tests/%: tests/%.c $(TESTLIBDBGA) $(RESOURCE_DBG_O) | build-debug/tests
	$(CC) $(CFLAGS) $(DBGFLAGS) -Wl,--as-needed -Isrc $(PKG_CFLAGS) \
		-DNUVASHALAUNCHER_DATA_DIR=\"$(CURDIR)/data\" -DNUVASHALAUNCHER_BIN=\"$(CURDIR)/$(BIN)\" \
		-DNUVASHALAUNCHER_BUILD_SCHEMA_DIR=\"$(CURDIR)/build/schemas\" \
		-DNUVASHALAUNCHER_TEST_FIXTURES=\"$(CURDIR)/tests/fixtures\" \
		-DNUVASHALAUNCHER_SRC_DIR=\"$(CURDIR)/src\" -DNUVASHALAUNCHER_PO_DIR=\"$(CURDIR)/po\" \
		-o $@ $< $(RESOURCE_DBG_O) $(TESTLIBDBGA) $(PKG_LIBS)

# The display guard test runs the release launcher, so test-debug builds both
# binaries.
test-debug: $(if $(SRCS),$(DBGBIN) $(BIN)) $(TESTRUNNER) $(TESTDBGBINS) $(SCHEMA_COMPILED)
	$(call run_test_suite,$(TESTDBGBINS),ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 LSAN_OPTIONS=detect_leaks=1:exitcode=1:suppressions="$(CURDIR)/tests/gtk-lsan.supp")

# LeakSanitizer is included with the Linux AddressSanitizer runtime and is
# enabled explicitly in test-debug above. Keep test-lsan as an alias so
# existing scripts and CI invocations continue to work.
test-lsan: test-debug

ANALYZER_STAMPS := $(patsubst src/%.c,build/.analyzer/%.stamp,$(SRCS))

build/.analyzer:
	@mkdir -p $@

build/.analyzer/%.stamp: src/%.c $(SCHEMA_COMPILED) | build/.analyzer
	$(CC) $(CPPFLAGS) -std=c17 -Wall -Wextra -Wpedantic -fanalyzer -c -o /dev/null \
		$(PKG_CFLAGS) -Isrc $<
	@touch $@

static-analysis: $(ANALYZER_STAMPS)

pot:
	xgettext --from-code=UTF-8 --keyword=_ --keyword=N_ \
		--files-from=po/POTFILES.in --output=po/nuvashalauncher.pot

check-potfiles:
	@set -e; missing=0; \
	for f in src/*.c; do \
		if ! grep -Fxq "$$f" po/POTFILES.in; then \
			echo "Missing from po/POTFILES.in: $$f"; missing=1; \
		fi; \
	done; \
	for f in data/ui/*.ui; do \
		if grep -q 'translatable="yes"' "$$f" && ! grep -Fxq "$$f" po/POTFILES.in; then \
			echo "Missing translatable UI file from po/POTFILES.in: $$f"; missing=1; \
		fi; \
	done; \
	while IFS= read -r line; do \
		case "$$line" in ""|\#*) continue;; esac; \
		if ! test -f "$$line"; then \
			echo "Nonexistent file in po/POTFILES.in: $$line"; missing=1; \
		fi; \
	done < po/POTFILES.in; \
	test $$missing -eq 0 && echo "PASS po/POTFILES.in is up to date"

install: $(if $(SRCS),$(BIN)) $(LOCALE_MOS) $(SCHEMA_COMPILED)
	install -Dm755 $(BIN) "$(DESTDIR)$(PREFIX)/bin/nuvashalauncher"
	install -Dm644 data/pack.png "$(DESTDIR)$(PREFIX)/share/nuvashalauncher/pack.png"
	install -Dm644 data/nuvashalauncher.css "$(DESTDIR)$(PREFIX)/share/nuvashalauncher/nuvashalauncher.css"
	@for skin in data/skins/defaults/*.png; do \
		install -Dm644 "$$skin" "$(DESTDIR)$(PREFIX)/share/nuvashalauncher/skins/defaults/$$(basename "$$skin")"; \
	done
	install -Dm644 data/com.github.NuvashaLauncher.NuvashaLauncher.desktop \
		"$(DESTDIR)$(PREFIX)/share/applications/com.github.NuvashaLauncher.NuvashaLauncher.desktop"
	install -Dm644 data/nuvashalauncher.bash \
		"$(DESTDIR)$(PREFIX)/share/bash-completion/completions/nuvashalauncher"
	install -Dm644 data/com.github.NuvashaLauncher.NuvashaLauncher.xml \
		"$(DESTDIR)$(PREFIX)/share/mime/packages/com.github.NuvashaLauncher.NuvashaLauncher.xml"
	install -Dm644 data/icons/scalable/apps/com.github.NuvashaLauncher.NuvashaLauncher.svg \
		"$(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps/com.github.NuvashaLauncher.NuvashaLauncher.svg"
	install -Dm644 data/icons/scalable/actions/export-symbolic.svg \
		"$(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/actions/export-symbolic.svg"
	install -Dm644 data/icons/scalable/actions/external-link-symbolic.svg \
		"$(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/actions/external-link-symbolic.svg"
	install -Dm644 data/com.github.NuvashaLauncher.NuvashaLauncher.metainfo.xml \
		"$(DESTDIR)$(PREFIX)/share/metainfo/com.github.NuvashaLauncher.NuvashaLauncher.metainfo.xml"
	install -Dm644 $(SCHEMA_XML) \
		"$(DESTDIR)$(PREFIX)/share/glib-2.0/schemas/com.github.NuvashaLauncher.NuvashaLauncher.gschema.xml"
	@for lang in $(LOCALES); do \
		install -Dm644 build/locale/$$lang/LC_MESSAGES/nuvashalauncher.mo \
			"$(DESTDIR)$(PREFIX)/share/locale/$$lang/LC_MESSAGES/nuvashalauncher.mo"; \
	done
	@schema_dir="$(DESTDIR)$(PREFIX)/share/glib-2.0/schemas"; \
	if command -v glib-compile-schemas >/dev/null 2>&1; then \
		glib-compile-schemas "$$schema_dir"; \
	fi
	@desktop_dir="$(DESTDIR)$(PREFIX)/share/applications"; \
	if command -v update-desktop-database >/dev/null 2>&1; then \
		update-desktop-database "$$desktop_dir"; \
	fi
	@mime_dir="$(DESTDIR)$(PREFIX)/share/mime"; \
	if command -v update-mime-database >/dev/null 2>&1; then \
		update-mime-database "$$mime_dir"; \
	fi
	@icon_dir="$(DESTDIR)$(PREFIX)/share/icons/hicolor"; \
	if command -v gtk-update-icon-cache >/dev/null 2>&1 && test -f "$$icon_dir/index.theme"; then \
		gtk-update-icon-cache -f -t "$$icon_dir"; \
	fi

# The same list the install loop uses, so the catalogs go back out with
# the rest of the launcher.
uninstall:
	rm -f "$(DESTDIR)$(PREFIX)/bin/nuvashalauncher"
	rm -f "$(DESTDIR)$(PREFIX)/share/nuvashalauncher/pack.png"
	rm -f "$(DESTDIR)$(PREFIX)/share/nuvashalauncher/nuvashalauncher.css"
	rm -rf "$(DESTDIR)$(PREFIX)/share/nuvashalauncher/skins/defaults"
	rm -f "$(DESTDIR)$(PREFIX)/share/applications/com.github.NuvashaLauncher.NuvashaLauncher.desktop"
	rm -f "$(DESTDIR)$(PREFIX)/share/bash-completion/completions/nuvashalauncher"
	rm -f "$(DESTDIR)$(PREFIX)/share/mime/packages/com.github.NuvashaLauncher.NuvashaLauncher.xml"
	rm -f "$(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps/com.github.NuvashaLauncher.NuvashaLauncher.svg"
	rm -f "$(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/actions/export-symbolic.svg"
	rm -f "$(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/actions/external-link-symbolic.svg"
	rm -f "$(DESTDIR)$(PREFIX)/share/metainfo/com.github.NuvashaLauncher.NuvashaLauncher.metainfo.xml"
	rm -f "$(DESTDIR)$(PREFIX)/share/glib-2.0/schemas/com.github.NuvashaLauncher.NuvashaLauncher.gschema.xml"
	@for lang in $(LOCALES); do \
		rm -f "$(DESTDIR)$(PREFIX)/share/locale/$$lang/LC_MESSAGES/nuvashalauncher.mo"; \
	done
	@schema_dir="$(DESTDIR)$(PREFIX)/share/glib-2.0/schemas"; \
	if test -d "$$schema_dir" && command -v glib-compile-schemas >/dev/null 2>&1; then \
		glib-compile-schemas "$$schema_dir"; \
	fi
	@desktop_dir="$(DESTDIR)$(PREFIX)/share/applications"; \
	if test -d "$$desktop_dir" && command -v update-desktop-database >/dev/null 2>&1; then \
		update-desktop-database "$$desktop_dir"; \
	fi
	@mime_dir="$(DESTDIR)$(PREFIX)/share/mime"; \
	if test -d "$$mime_dir" && command -v update-mime-database >/dev/null 2>&1; then \
		update-mime-database "$$mime_dir"; \
	fi
	@icon_dir="$(DESTDIR)$(PREFIX)/share/icons/hicolor"; \
	if test -f "$$icon_dir/index.theme" && command -v gtk-update-icon-cache >/dev/null 2>&1; then \
		gtk-update-icon-cache -f -t "$$icon_dir"; \
	fi

clean:
	rm -rf build build-debug *.flatpak *.rpm *.deb *.tar.gz

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
