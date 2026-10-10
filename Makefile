# ibus-rime-remote-c -- C 实现（GDBus / GLib-GIO）
#
# Arch 依赖：pacman -S glib2

CC     ?= cc
CFLAGS ?= -O2 -Wall -Wextra -std=c11

BIN := ibus-rime-remote
SRC := ibus-rime-remote.c

# 安装位置：默认 ~/.local/bin，可用 make install BINDIR=/somewhere/bin 覆盖。
BINDIR ?= $(HOME)/.local/bin

.PHONY: all clean install uninstall

all: $(BIN)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) $(shell pkg-config --cflags gio-2.0) -o $@ $< $(shell pkg-config --libs gio-2.0)

install: $(BIN)
	install -Dm755 $(BIN) $(DESTDIR)$(BINDIR)/$(BIN)

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(BIN)

clean:
	rm -f $(BIN)
