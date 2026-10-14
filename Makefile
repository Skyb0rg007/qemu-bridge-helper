CC = cc
CFLAGS = -Wall -Wextra -std=c99 -pedantic
LDFLAGS =
LDLIBS =

CFLAGS += -fno-omit-frame-pointer -fsanitize=address
LDLIBS += -fsanitize=address

all: qemu-bridge-helper qemu-bridge-helper-server

# Only the server needs libsystemd, for socket activation
qemu-bridge-helper-server: CFLAGS += $(shell pkg-config --cflags libsystemd)
qemu-bridge-helper-server: LDLIBS += $(shell pkg-config --libs libsystemd)

qemu-bridge-helper: qemu-bridge-helper.c qemu-bridge-helper.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

qemu-bridge-helper-server: qemu-bridge-helper-server.c qemu-bridge-helper.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

.PHONY: all
