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

# CFLAGS += -fno-omit-frame-pointer -fsanitize=address
# LDLIBS += -fsanitize=address

SD_CFLAGS := $(shell pkg-config --cflags libsystemd)
SD_LDLIBS := $(shell pkg-config --libs libsystemd)

all: qemu-bridge-helper qemu-bridge-helper-server

install: qemu-bridge-helper qemu-bridge-helper-server qemu-bridge-helper@.service installdirs
	$(INSTALL_PROGRAM) qemu-bridge-helper $(DESTDIR)$(bindir)/qemu-bridge-helper
	$(INSTALL_PROGRAM) qemu-bridge-helper-server $(DESTDIR)$(libexecdir)/qemu-bridge-helper-server
	$(INSTALL_DATA) qemu-bridge-helper@.service $(DESTDIR)$(systemddir)/qemu-bridge-helper@.service
	$(INSTALL_DATA) qemu-bridge-helper.socket $(DESTDIR)$(systemddir)/qemu-bridge-helper.socket

installdirs:
	mkdir -p $(DESTDIR)$(bindir)
	mkdir -p $(DESTDIR)$(libexecdir)
	mkdir -p $(DESTDIR)$(systemddir)

qemu-bridge-helper@.service: qemu-bridge-helper@.service.in
	$(SED) 's:@libexecdir@:$(libexecdir):g' $< >$@

qemu-bridge-helper: qemu-bridge-helper.o
	$(CC) $(LDFLAGS) -o $@ $^ $(SD_LDLIBS) $(LDLIBS)

qemu-bridge-helper-server: qemu-bridge-helper-server.o
	$(CC) $(LDFLAGS) -o $@ $^ $(SD_LDLIBS) $(LDLIBS)

qemu-bridge-helper.o: qemu-bridge-helper.c qemu-bridge-helper.h
	$(CC) $(CFLAGS) $(SD_CFLAGS) -c -o $@ $<

qemu-bridge-helper-server.o: qemu-bridge-helper-server.c qemu-bridge-helper.h
	$(CC) $(CFLAGS) $(SD_CFLAGS) -c -o $@ $<

.PHONY: all install installdirs
