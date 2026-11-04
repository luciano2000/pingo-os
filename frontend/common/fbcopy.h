/*
 * Copia a tela atual para o framebuffer do sistema (/dev/fb0). Entre um
 * programa e outro (launcher -> jogo, desligando) é o fb0 que aparece; sem
 * isto, ele ainda mostra o logo do boot.
 *
 * Copyright (C) 2026 Pingo OS by Retrogamer Brasil
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef PINGO_FBCOPY_H
#define PINGO_FBCOPY_H

#include <stdbool.h>
#include <SDL2/SDL.h>

/* Lê o quadro já desenhado (antes do present) e grava no fb0 */
void fb_copy_from(SDL_Renderer *ren);
/* Deixa o fb0 preto */
void fb_clear(void);

#endif
