# SPDX-License-Identifier: MIT-0
# SPDX-FileCopyrightText: 2026 Skye Soss
CC = cc
CFLAGS = -Wall -Wextra -std=c99 -pedantic
LDFLAGS =
LDLIBS =

CFLAGS += $(shell pkg-config --cflags libsystemd)
LDLIBS += $(shell pkg-config --libs libsystemd)

CFLAGS += -fno-omit-frame-pointer -fsanitize=address
LDLIBS += -fsanitize=address

all: qemu-bridge-helper qemu-bridge-helper-server

qemu-bridge-helper: qemu-bridge-helper.c qemu-bridge-helper.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

qemu-bridge-helper-server: qemu-bridge-helper-server.c qemu-bridge-helper.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

.PHONY: all
