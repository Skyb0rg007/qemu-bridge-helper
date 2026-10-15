# SPDX-License-Identifier: MIT-0
# SPDX-FileCopyrightText: 2026 Skye Soss
CC ?= cc
CFLAGS ?= -Wall -Wextra -std=c99 -pedantic
LDFLAGS ?=
LDLIBS ?=

INSTALL ?= install
INSTALL_PROGRAM ?= $(INSTALL)
INSTALL_DATA ?= $(INSTALL) -m 644
SED ?= sed

prefix ?= /usr/local
exec_prefix ?= $(prefix)
bindir ?= $(exec_prefix)/bin
libexecdir ?= $(exec_prefix)/libexec
systemddir ?= $(prefix)/lib/systemd/system

BUILDDIR ?= _build

# CFLAGS += -fno-omit-frame-pointer -fsanitize=address
# LDLIBS += -fsanitize=address

SD_CFLAGS := $(shell pkg-config --cflags libsystemd)
SD_LDLIBS := $(shell pkg-config --libs libsystemd)

all: $(BUILDDIR)/qemu-bridge-helper $(BUILDDIR)/qemu-bridge-helper-server

install: $(BUILDDIR)/qemu-bridge-helper $(BUILDDIR)/qemu-bridge-helper-server $(BUILDDIR)/qemu-bridge-helper@.service installdirs
	$(INSTALL_PROGRAM) $(BUILDDIR)/qemu-bridge-helper $(DESTDIR)$(bindir)/qemu-bridge-helper
	$(INSTALL_PROGRAM) $(BUILDDIR)/qemu-bridge-helper-server $(DESTDIR)$(libexecdir)/qemu-bridge-helper-server
	$(INSTALL_DATA) $(BUILDDIR)/qemu-bridge-helper@.service $(DESTDIR)$(systemddir)/qemu-bridge-helper@.service
	$(INSTALL_DATA) units/qemu-bridge-helper.socket $(DESTDIR)$(systemddir)/qemu-bridge-helper.socket

installdirs:
	mkdir -p $(DESTDIR)$(bindir)
	mkdir -p $(DESTDIR)$(libexecdir)
	mkdir -p $(DESTDIR)$(systemddir)

$(BUILDDIR)/qemu-bridge-helper@.service: units/qemu-bridge-helper@.service.in
	$(SED) 's:@libexecdir@:$(libexecdir):g' $< >$@

$(BUILDDIR)/qemu-bridge-helper: $(BUILDDIR)/qemu-bridge-helper.o
	$(CC) $(LDFLAGS) -o $@ $^ $(SD_LDLIBS) $(LDLIBS)

$(BUILDDIR)/qemu-bridge-helper-server: $(BUILDDIR)/qemu-bridge-helper-server.o
	$(CC) $(LDFLAGS) -o $@ $^ $(SD_LDLIBS) $(LDLIBS)

$(BUILDDIR)/qemu-bridge-helper.o: src/qemu-bridge-helper.c src/qemu-bridge-helper.h | $(BUILDDIR)
	$(CC) $(CFLAGS) $(SD_CFLAGS) -c -o $@ $<

$(BUILDDIR)/qemu-bridge-helper-server.o: src/qemu-bridge-helper-server.c src/qemu-bridge-helper.h | $(BUILDDIR)
	$(CC) $(CFLAGS) $(SD_CFLAGS) -c -o $@ $<

$(BUILDDIR):
	mkdir -p $@

.PHONY: all install installdirs
