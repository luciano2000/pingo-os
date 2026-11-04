/*
 * Desenho da interface: texto (SDL2_ttf), retângulos arredondados e ícones.
 *
 * Copyright (C) 2026 Pingo OS by Retrogamer Brasil
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef PINGO_UI_H
#define PINGO_UI_H

#include <stdbool.h>
#include <SDL2/SDL.h>

/* Paleta da especificação (seção 8) */
#define COL_BG     0x000000
#define COL_TEXT   0xE8E8E8
#define COL_DIM    0x8C8C8C
#define COL_SEL_BG 0xE8E8E8
#define COL_SEL_FG 0x000000

/* Medidas do mockup, em 640x480 */
#define UI_W        640
#define UI_H        480
#define PAD_X       20
#define PAD_Y       14
#define BAR_H       28
#define LIST_GAP    14  /* barra superior -> lista */
#define ROW_H       44
#define ROW_GAP     2
#define ROW_PAD     16
#define ROW_RADIUS  10
#define ROWS        8

enum font_id {
	F_BAR,       /* 15 px, 500 */
	F_BAR_BOLD,  /* 15 px, 600 */
	F_ITEM,      /* 23 px, 500 */
	F_ITEM_SEL,  /* 23 px, 600 */
	F_COUNT,     /* 16 px, 500 */
	F_SMALL,     /* 14 px, 500 */
	F_TITLE,     /* 23 px, 600 (mensagens) */
	F_BODY,      /* 18 px, 400 (textos do assistente) */
	F_COUNT_N
};

bool ui_init(void);
void ui_quit(void);

SDL_Renderer *ui_renderer(void);
void ui_clear(void);
void ui_present(void);

/* Texto: devolve a largura desenhada. align: 0 esquerda, 1 direita, 2 centro */
int ui_text(enum font_id f, const char *s, int x, int y_center, Uint32 rgb, int align, int max_w);
int ui_text_width(enum font_id f, const char *s);

/* Texto em várias linhas, quebrado por palavras e centralizado em x.
 * Devolve a altura ocupada. */
int ui_text_wrap(enum font_id f, const char *s, int x_center, int y_top, int max_w, int line_h,
		 Uint32 rgb);

void ui_round_rect(int x, int y, int w, int h, int r, Uint32 rgb);
void ui_rect(int x, int y, int w, int h, Uint32 rgb);

/* Ícones da barra superior; devolvem a largura */
int ui_icon_wifi(int x, int y_center, Uint32 rgb);
int ui_icon_battery(int x, int y_center, int percent, bool charging, Uint32 rgb);

#endif
