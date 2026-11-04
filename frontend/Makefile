# pingo-retro: frontend libretro mínimo (Fase 0)
CC ?= gcc
PKG_CONFIG ?= pkg-config
CFLAGS ?= -O2
override CFLAGS += -Wall -Wextra -Wno-unused-parameter -Iinclude $(shell $(PKG_CONFIG) --cflags sdl2)
override LDLIBS += $(shell $(PKG_CONFIG) --libs sdl2) -ldl

pingo-retro: pingo-retro.c include/libretro.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

install: pingo-retro
	install -D -m 0755 pingo-retro $(DESTDIR)/usr/bin/pingo-retro

clean:
	rm -f pingo-retro

.PHONY: install clean
