CC = cc
CFLAGS = -Wall -Wextra -std=c99 -pedantic
LDFLAGS =
LDLIBS =

CFLAGS += $(shell pkg-config --cflags libsystemd)
LDLIBS += $(shell pkg-config --libs libsystemd)

qemu-bridge-helper: qemu-bridge-helper.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)
