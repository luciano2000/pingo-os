/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Controle do player 2 (Pingo Arcade): um controle USB qualquer.
 *
 * O player 1 é o painel (Picade, teclas); qualquer outro dispositivo com
 * botões de joystick/gamepad vira o player 2. Funções com os nomes do
 * painel: direções, botões 1 a 6 (a mesma numeração dos bornes da Picade),
 * start e ficha. O mapa fica em Config/controles/<vid>-<pid>.cfg:
 *
 *   # USB,2-axis 8-button gamepad
 *   cima=abs:1-
 *   botao1=btn:291
 *
 * "btn:<código>" é um botão (EV_KEY); "abs:<eixo>+" ou "abs:<eixo>-" é um
 * eixo passando do meio (direcional analógico ou hat). Sem arquivo, vale
 * um mapa padrão (o modelo conhecido ou o layout de gamepad do Linux).
 */
#ifndef PINGO_CONTROLE2_H
#define PINGO_CONTROLE2_H

#include <stdbool.h>
#include <linux/input.h>

enum {
	C2_UP, C2_DOWN, C2_LEFT, C2_RIGHT,
	C2_B1, C2_B2, C2_B3, C2_B4, C2_B5, C2_B6,
	C2_START, C2_COIN, C2_N
};

extern const char *const c2_keys[C2_N];   /* nomes no arquivo */
extern const char *const c2_labels[C2_N]; /* nomes na tela */

struct c2_bind { char type; int code, sign; }; /* type 'b', 'a' ou 0 */

struct c2_dev {
	int fd;
	char name[128];
	unsigned short vid, pid;
	struct c2_bind map[C2_N];
	int amin[ABS_MAX + 1], amax[ABS_MAX + 1];
	bool down[C2_N];
	bool saved; /* o mapa veio de um arquivo */
};

/* Procura um controle; true se achou (e carregou o mapa) */
bool c2_open(struct c2_dev *d);
void c2_close(struct c2_dev *d);

/* Lê os eventos; cb(função, apertado) a cada mudança. false se o controle
 * saiu (o fd é fechado) */
bool c2_poll(struct c2_dev *d, void (*cb)(int func, bool down));

/* Para o assistente: o próximo "aperto" bruto (botão ou eixo passando do
 * meio). true e o bind em *out quando há um */
bool c2_capture(struct c2_dev *d, struct c2_bind *out);

/* Descreve um bind ("botão 291", "eixo 1−") para a tela */
void c2_describe(const struct c2_bind *b, char *out, int n);

bool c2_save(struct c2_dev *d);

#endif
