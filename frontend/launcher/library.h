/*
 * Biblioteca de jogos: consoles, árvore de pastas e cache.
 *
 * Copyright (C) 2026 Pingo OS by Retrogamer Brasil
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef PINGO_LIBRARY_H
#define PINGO_LIBRARY_H

#include <stdbool.h>

#ifndef ROMS_DIR
#define ROMS_DIR "/mnt/sd1/Roms"
#endif
#ifndef ROMS_DIR_SD2
#define ROMS_DIR_SD2 "/mnt/sd2/Roms" /* segundo cartão: só jogos */
#endif
#define CORES_DIR "/usr/lib/libretro"
#ifndef M3U_DIR
#define M3U_DIR "/mnt/sd1/.system/cache/m3u" /* .m3u gerados para jogos sem .m3u */
#endif

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
	int discs;    /* jogos de vários discos (.m3u): quantos discos */
	/* pasta de console: de onde vêm os jogos e quantos há em cada cartão */
	int source;   /* 1 = SD1, 2 = SD2, 3 = os dois */
	char *path2;  /* pasta do console no outro cartão (ou NULL) */
	int games1, games2;
	struct node **kids;
	int nkids;
};

/* Raiz: um filho por console que tem jogos, na ordem do Início.
 * origin[c] = 0 (ambos os cartões, o padrão), 1 (só SD1) ou 2 (só SD2).
 * Sem o SD2 no aparelho, tudo vem do SD1. Com os dois e origem "ambos", a
 * lista junta os jogos dos dois cartões (o mesmo arquivo aparece uma vez,
 * do SD1). Cada pasta de console guarda quantos jogos há em cada cartão. */
struct node *library_scan(const int *origin, bool sd2);
struct node *library_load_cache(const char *path);
bool library_save_cache(const struct node *root, const char *path);
bool library_equal(const struct node *a, const struct node *b);
void library_free(struct node *n);

/* Procura um filho pelo caminho completo; -1 se não houver */
int library_find_kid(const struct node *dir, const char *path);

/* Pasta de console ("01 - NES" -> NES, ordem 1); -1 se não for de console */
int library_console_folder(const char *name, int *order);

/* Pasta do console na biblioteca (NULL se ele não tem jogos) */
const struct node *library_console(const struct node *root, int console);

/* Jogo avulso (usado pelos Recentes) */
struct node *library_game_node(int console, const char *path);

/* Nome limpo: sem extensão e sem "(USA)", "[!]" etc. Devolve malloc. */
char *library_clean_name(const char *fname, bool is_dir);

/* Comparação natural sem distinguir maiúsculas ("2" antes de "10") */
int natural_cmp(const char *a, const char *b);

#endif
