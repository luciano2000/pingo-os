/*
 * Biblioteca de jogos: consoles, árvore de pastas e cache.
 *
 * Copyright (C) 2026 Pingo OS by Retrogamer Brasil
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef PINGO_LIBRARY_H
#define PINGO_LIBRARY_H

#include <stdbool.h>

#define ROMS_DIR "/mnt/sd1/Roms"
#define CORES_DIR "/usr/lib/libretro"

struct console {
	const char *code; /* nome da pasta em Roms/ */
	const char *name; /* nome mostrado */
	const char *exts; /* extensões aceitas, separadas por espaço */
	const char *core; /* core libretro, sem o sufixo _libretro.so */
};

extern const struct console consoles[];
extern const int n_consoles;

struct node {
	char *fname;  /* nome do arquivo ou pasta */
	char *label;  /* nome limpo, mostrado na lista */
	char *path;   /* caminho completo */
	int console;  /* índice em consoles[] */
	bool dir;
	int games;    /* jogos nesta pasta e abaixo (1 para arquivo) */
	struct node **kids;
	int nkids;
};

/* Raiz: um filho por console que tem jogos, na ordem do nome mostrado */
struct node *library_scan(void);
struct node *library_load_cache(const char *path);
bool library_save_cache(const struct node *root, const char *path);
bool library_equal(const struct node *a, const struct node *b);
void library_free(struct node *n);

/* Procura um filho pelo caminho completo; -1 se não houver */
int library_find_kid(const struct node *dir, const char *path);

/* Jogo avulso (usado pelos Recentes) */
struct node *library_game_node(int console, const char *path);

/* Nome limpo: sem extensão e sem "(USA)", "[!]" etc. Devolve malloc. */
char *library_clean_name(const char *fname, bool is_dir);

/* Comparação natural sem distinguir maiúsculas ("2" antes de "10") */
int natural_cmp(const char *a, const char *b);

#endif
