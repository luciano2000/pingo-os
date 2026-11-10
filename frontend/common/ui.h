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
#define ROW_RADIUS  (ROW_H / 2) /* pílula */
#define ROWS        8

enum font_id {
	F_BAR,       /* 15 px, 500 */
	F_BAR_BOLD,  /* 15 px, 600 */
	F_ITEM,      /* 24 px, Quicksand 600 */
	F_ITEM_SEL,  /* 24 px, Quicksand 700 */
	F_COUNT,     /* 16 px, 500 */
	F_SMALL,     /* 14 px, 500 */
	F_TITLE,     /* 23 px, 600 (mensagens) */
	F_BODY,      /* 21 px, 400 (textos do assistente e mensagens) */
	F_BIG,       /* 28 px, 600 (títulos do assistente e mensagens) */
	F_COUNT_N
};

/* Cria a janela e o renderer (launcher) */
bool ui_init(void);
void ui_quit(void);

/* Usa um renderer que já existe (pingo-retro): carrega fontes e ícones */
bool ui_attach(SDL_Renderer *renderer);
void ui_detach(void);

/* Bolinha de 10 px dos slots: cheia ou só o contorno */
void ui_dot(int x, int y_center, bool filled, Uint32 rgb);

/* Ícone "›" do item selecionado (menu do jogo) */
void ui_icon_chevron(int x, int y_center, Uint32 rgb);

SDL_Renderer *ui_renderer(void);
void ui_clear(void);
void ui_clear_rgb(Uint32 rgb);
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

/* Print da tela (SIGUSR1, pelo SSH): salva o que acabou de ser desenhado
 * em /mnt/sd1/Screenshots antes de exibir. ui_screenshot_check() vai logo
 * antes de cada SDL_RenderPresent. */
void ui_screenshot_init(void);
void ui_screenshot_check(SDL_Renderer *ren);
int ui_screenshot_pending(void); /* o launcher redesenha para o print sair */

#endif
