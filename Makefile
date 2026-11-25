# SPDX-License-Identifier: MIT
#
# Distro builds pass their own CFLAGS/CPPFLAGS/LDFLAGS; the language level and
# warnings below are always added. WERROR=1 turns warnings into errors (CI).
CC       ?= cc
CFLAGS   ?= -O2 -g
CPPFLAGS ?=
LDFLAGS  ?=
WERROR   ?= 0
PREFIX   ?= /usr
DESTDIR  ?=
PYTHON   ?= python3

# Install layout. Distros override these as needed (NixOS: PREFIX=$out, and
# MODPROBE/KILL pointing into the store).
BINDIR         ?= $(PREFIX)/bin
LIBDIR         ?= $(PREFIX)/lib
PYLIBDIR       ?= $(LIBDIR)/globetrot
UNITDIR        ?= $(LIBDIR)/systemd/system
USERUNITDIR    ?= $(LIBDIR)/systemd/user
MODULESLOADDIR ?= $(LIBDIR)/modules-load.d
LIBINPUTDIR    ?= $(PREFIX)/share/libinput
DOCDIR         ?= $(PREFIX)/share/doc/globetrot
LICENSEDIR     ?= $(PREFIX)/share/licenses/globetrot
SYSCONFDIR     ?= /etc
# Where the default config is installed; differs from SYSCONFDIR only when /etc
# is managed elsewhere (NixOS ships it in the store and links it via a module).
ETCDIR         ?= $(SYSCONFDIR)
MODPROBE       ?= /sbin/modprobe
KILL           ?= /bin/kill

WARN_CFLAGS = -std=c11 -Wall -Wextra -Wpedantic
ifeq ($(WERROR),1)
WARN_CFLAGS += -Werror
endif
SANFLAGS = -fsanitize=address,undefined -fno-omit-frame-pointer -g

ALL_CFLAGS = $(WARN_CFLAGS) $(CPPFLAGS) $(CFLAGS)

CORE_SRCS   = src/keycodes.c src/globe_sm.c src/globe_config.c
CORE_HDRS   = src/keycodes.h src/globe_sm.h src/globe_config.h
DAEMON_SRCS = $(CORE_SRCS) src/main_linux.c
PY_MODULES  = __init__ ini mru backends session ctl

UNAME_S := $(shell uname -s)

.PHONY: all daemon test test-c test-py install dist clean

ifeq ($(UNAME_S),Linux)
all: daemon
else
all: test
endif

daemon: build/globetrotd

build:
	mkdir -p build

build/globetrotd: $(DAEMON_SRCS) $(CORE_HDRS) | build
	$(CC) $(ALL_CFLAGS) $(LDFLAGS) -o $@ $(DAEMON_SRCS)

# Tests always build with sanitizers and without distro hardening flags that
# conflict with them (e.g. _FORTIFY_SOURCE with ASan).
build/test_globe_sm: tests/test_globe_sm.c $(CORE_SRCS) $(CORE_HDRS) | build
	$(CC) $(WARN_CFLAGS) -O1 $(SANFLAGS) -o $@ tests/test_globe_sm.c $(CORE_SRCS)

build/test_config: tests/test_config.c $(CORE_SRCS) $(CORE_HDRS) | build
	$(CC) $(WARN_CFLAGS) -O1 $(SANFLAGS) -o $@ tests/test_config.c $(CORE_SRCS)

test-c: build/test_globe_sm build/test_config
	./build/test_globe_sm
	./build/test_config

test-py:
	$(PYTHON) -m unittest discover -s tests -p 'test_*.py' -v

test: test-c test-py

SUBST = sed -e 's|@BINDIR@|$(BINDIR)|g' -e 's|@PYLIBDIR@|$(PYLIBDIR)|g' -e 's|@DOCDIR@|$(DOCDIR)|g' \
	-e 's|@SYSCONFDIR@|$(SYSCONFDIR)|g' -e 's|@MODPROBE@|$(MODPROBE)|g' -e 's|@KILL@|$(KILL)|g'

install: daemon
	install -Dm755 build/globetrotd "$(DESTDIR)$(BINDIR)/globetrotd"
	install -d "$(DESTDIR)$(UNITDIR)" "$(DESTDIR)$(USERUNITDIR)"
	$(SUBST) bin/globetrot > build/globetrot && install -Dm755 build/globetrot "$(DESTDIR)$(BINDIR)/globetrot"
	$(SUBST) bin/globetrot-session > build/globetrot-session && install -Dm755 build/globetrot-session "$(DESTDIR)$(BINDIR)/globetrot-session"
	for m in $(PY_MODULES); do \
		install -Dm644 python/globetrot/$$m.py "$(DESTDIR)$(PYLIBDIR)/globetrot/$$m.py" || exit 1; \
	done
	$(SUBST) systemd/globetrotd.service.in > build/globetrotd.service
	$(SUBST) systemd/globetrot-session.service.in > build/globetrot-session.service
	install -Dm644 build/globetrotd.service "$(DESTDIR)$(UNITDIR)/globetrotd.service"
	install -Dm644 build/globetrot-session.service "$(DESTDIR)$(USERUNITDIR)/globetrot-session.service"
	install -Dm644 modules-load/globetrot.conf "$(DESTDIR)$(MODULESLOADDIR)/globetrot.conf"
	install -Dm644 libinput/60-globetrot.quirks "$(DESTDIR)$(LIBINPUTDIR)/60-globetrot.quirks"
	install -Dm644 config/globetrot.conf "$(DESTDIR)$(ETCDIR)/globetrot.conf"
	install -Dm644 config/globetrot.conf "$(DESTDIR)$(DOCDIR)/globetrot.conf"
	install -Dm644 docs/ARCHITECTURE.md "$(DESTDIR)$(DOCDIR)/ARCHITECTURE.md"
	install -Dm644 README.md "$(DESTDIR)$(DOCDIR)/README.md"
	install -Dm644 xkb/globetrot-hyper.txt "$(DESTDIR)$(DOCDIR)/globetrot-hyper.txt"
	install -Dm644 LICENSE "$(DESTDIR)$(LICENSEDIR)/LICENSE"

VERSION := $(shell sed -n 's/^__version__ = "\(.*\)"/\1/p' python/globetrot/__init__.py)
DIST_FILES = LICENSE Makefile README.md install.sh flake.nix bin config debian docs libinput modules-load nix packaging python src systemd tests xkb

# Release tarball for packaging (no build output, caches or macOS metadata).
dist:
	mkdir -p build
	COPYFILE_DISABLE=1 tar --exclude='__pycache__' --exclude='*.pyc' --exclude='._*' --exclude='.DS_Store' \
		--no-xattrs --no-mac-metadata --uid 0 --gid 0 --uname root --gname root \
		-czf build/globetrot-$(VERSION).tar.gz -s ',^,globetrot-$(VERSION)/,' $(DIST_FILES) 2>/dev/null || \
	COPYFILE_DISABLE=1 tar --exclude='__pycache__' --exclude='*.pyc' --exclude='._*' --exclude='.DS_Store' \
		--owner=0 --group=0 --transform 's,^,globetrot-$(VERSION)/,' -czf build/globetrot-$(VERSION).tar.gz $(DIST_FILES)
	@echo build/globetrot-$(VERSION).tar.gz

clean:
	rm -rf build
