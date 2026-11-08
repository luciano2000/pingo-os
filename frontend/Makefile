# Frontend do Pingo OS: pingo-retro (jogos) e pingo-launcher (listas)
CC ?= gcc
PKG_CONFIG ?= pkg-config
CFLAGS ?= -O2
override CFLAGS += -Wall -Wextra -Wno-unused-parameter -Iinclude -Icommon
SDL_CFLAGS = $(shell $(PKG_CONFIG) --cflags sdl2)
SDL_LIBS = $(shell $(PKG_CONFIG) --libs sdl2)
TTF_LIBS = $(shell $(PKG_CONFIG) --libs SDL2_ttf)
ALSA_LIBS = $(shell $(PKG_CONFIG) --libs alsa)
ZIP_CFLAGS = $(shell $(PKG_CONFIG) --cflags libzip)
ZIP_LIBS = $(shell $(PKG_CONFIG) --libs libzip)

COMMON_SRC = common/osd.c common/fbcopy.c common/ui.c common/textos.c
COMMON_HDR = common/osd.h common/fbcopy.h common/ui.h common/textos.h common/textos_pt.h
LAUNCHER_SRC = launcher/main.c launcher/library.c
LAUNCHER_HDR = launcher/library.h

all: pingo-retro pingo-launcher

pingo-retro: pingo-retro.c include/libretro.h $(COMMON_SRC) $(COMMON_HDR)
	$(CC) $(CFLAGS) $(SDL_CFLAGS) $(ZIP_CFLAGS) $(LDFLAGS) -o $@ pingo-retro.c $(COMMON_SRC) \
		$(TTF_LIBS) $(SDL_LIBS) $(ALSA_LIBS) $(ZIP_LIBS) -ldl -lm

pingo-launcher: $(LAUNCHER_SRC) $(LAUNCHER_HDR) $(COMMON_SRC) $(COMMON_HDR)
	$(CC) $(CFLAGS) $(SDL_CFLAGS) $(LDFLAGS) -o $@ $(LAUNCHER_SRC) $(COMMON_SRC) \
		$(TTF_LIBS) $(SDL_LIBS) $(ALSA_LIBS) -lm

install: all
	install -D -m 0755 pingo-retro $(DESTDIR)/usr/bin/pingo-retro
	install -D -m 0755 pingo-launcher $(DESTDIR)/usr/bin/pingo-launcher
	install -D -m 0755 pingo-shell $(DESTDIR)/usr/bin/pingo-shell
	for f in idiomas/*.txt; do \
		install -D -m 0644 $$f $(DESTDIR)/usr/share/pingo/idiomas/$$(basename $$f) || exit 1; \
	done
	for f in cores/*.opts; do \
		install -D -m 0644 $$f $(DESTDIR)/usr/share/pingo/cores/$$(basename $$f) || exit 1; \
	done

clean:
	rm -f pingo-retro pingo-launcher

.PHONY: all install clean
