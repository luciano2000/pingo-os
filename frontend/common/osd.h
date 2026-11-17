/*
 * Volume e brilho (Vol± e MENU+Vol±) com sobreposição na tela.
 * Usado pelo launcher e pelo pingo-retro.
 *
 * Copyright (C) 2026 Pingo OS by Retrogamer Brasil
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef PINGO_OSD_H
#define PINGO_OSD_H

#include <stdbool.h>
#include <SDL2/SDL.h>

#define VOLUME_STEPS 20
#define BRIGHT_STEPS 10

/* Lê os valores salvos e aplica no hardware */
void osd_init(void);

/* Passa os eventos de tecla (EV_KEY). Trata Vol± e acompanha o MENU
 * (BTN_MODE). Devolve true se o evento foi consumido. */
bool osd_key(int code, int value);

/* MENU está apertado (para combinações como MENU + cima/baixo) */
bool osd_menu_held(void);

/* Sobe/desce um passo do volume e mostra a sobreposição. Com o MENU
 * apertado, conta como combinação (soltar o MENU não abre o menu). */
void osd_volume_step(int d);

/* Ganho a aplicar no som quando o codec não tem controle de volume
 * (1.0 quando o volume é no hardware) */
float osd_soft_gain(void);

/* MENU foi solto sem ter sido usado com Vol±: é um toque de MENU de verdade */
bool osd_menu_tapped(void);

/* A sobreposição está na tela (ou acabou de sumir): precisa redesenhar */
bool osd_visible(void);
bool osd_changed(void);

/* Desenha por cima do que já está no renderer (coordenadas lógicas w x h) */
void osd_draw(SDL_Renderer *ren, int w, int h);

/* Liga/desliga a luz da tela (proteção de tela), sem mexer no brilho */
void osd_screen(bool on);

/* Libera as texturas (antes de destruir o renderer) */
void osd_quit(void);

#endif
