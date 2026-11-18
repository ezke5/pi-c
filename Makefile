# SPDX-License-Identifier: GPL-3.0-or-later
CC := clang
CFLAGS ?= -std=c23 -O2 -Wall -Wextra -Werror -pedantic -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE
CPPFLAGS ?= -Iinclude
LDLIBS ?= -lcurl -lcrypto -larchive -ldl

LIB_SOURCES = src/conf.c src/sandbox.c src/db.c src/dag.c src/archive.c src/hooks.c src/net.c src/build.c src/index.c
LIB_OBJECTS = $(LIB_SOURCES:.c=.o)

.PHONY: all clean test install

PREFIX ?= /usr
DESTDIR ?=
SCDOC ?= scdoc

all: pi

man/pi.1: man/pi.1.scd
	@if command -v $(SCDOC) >/dev/null 2>&1; then $(SCDOC) < $< > $@; else :; fi

man/anrix.conf.5: man/anrix.conf.5.scd
	@if command -v $(SCDOC) >/dev/null 2>&1; then $(SCDOC) < $< > $@; else :; fi

man/PKGBUILD-anrix.5: man/PKGBUILD-anrix.5.scd
	@if command -v $(SCDOC) >/dev/null 2>&1; then $(SCDOC) < $< > $@; else :; fi

install: pi man/pi.1 man/anrix.conf.5 man/PKGBUILD-anrix.5
	install -Dm755 pi $(DESTDIR)$(PREFIX)/bin/pi
	@if test -s man/pi.1; then install -Dm644 man/pi.1 $(DESTDIR)$(PREFIX)/share/man/man1/pi.1; fi
	@if test -s man/anrix.conf.5; then install -Dm644 man/anrix.conf.5 $(DESTDIR)$(PREFIX)/share/man/man5/anrix.conf.5; fi
	@if test -s man/PKGBUILD-anrix.5; then install -Dm644 man/PKGBUILD-anrix.5 $(DESTDIR)$(PREFIX)/share/man/man5/PKGBUILD-anrix.5; fi
	install -Dm644 completions/pi.bash $(DESTDIR)$(PREFIX)/share/bash-completion/completions/pi
	install -Dm644 completions/_pi $(DESTDIR)$(PREFIX)/share/zsh/site-functions/_pi
	install -Dm644 completions/pi.fish $(DESTDIR)$(PREFIX)/share/fish/vendor_completions.d/pi.fish

pi: src/main.o $(LIB_OBJECTS)
	$(CC) $(CFLAGS) $^ $(LDLIBS) -o $@

test_conf: tests/test_conf.o src/conf.o
	$(CC) $(CFLAGS) $^ $(LDLIBS) -o $@

test_phase2: tests/test_phase2.o src/db.o src/hooks.o
	$(CC) $(CFLAGS) $^ $(LDLIBS) -o $@

test_phase3: tests/test_phase3.o src/dag.o
	$(CC) $(CFLAGS) $^ $(LDLIBS) -o $@

test_v1_e2e: tests/test_v1_e2e.o src/db.o src/hooks.o
	$(CC) $(CFLAGS) $^ $(LDLIBS) -o $@

test_index: tests/test_index.o src/index.o
	$(CC) $(CFLAGS) $^ -lcrypto -o $@

test_pkgbuild: tests/test_pkgbuild.o src/build.o src/net.o src/sandbox.o src/archive.o src/conf.o
	$(CC) $(CFLAGS) $^ $(LDLIBS) -o $@

test: test_conf test_phase2 test_phase3 test_v1_e2e test_index test_pkgbuild
	./test_conf
	./test_phase2
	./test_phase3
	./test_v1_e2e
	./test_index
	./test_pkgbuild

clean:
	rm -f pi test_conf test_phase2 test_phase3 test_v1_e2e test_index test_pkgbuild src/*.o tests/*.o man/pi.1 man/anrix.conf.5 man/PKGBUILD-anrix.5
