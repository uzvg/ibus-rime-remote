# ibus-rime-remote-c -- C 实现（GDBus / GLib-GIO）
#
# Arch 依赖：pacman -S glib2

CC     ?= cc
CFLAGS ?= -O2 -Wall -Wextra -std=c11

BIN := ibus-rime-remote
SRC := ibus-rime-remote.c

.PHONY: all clean

all: $(BIN)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) $(shell pkg-config --cflags gio-2.0) -o $@ $< $(shell pkg-config --libs gio-2.0)

clean:
	rm -f $(BIN)
