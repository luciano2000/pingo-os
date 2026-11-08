/* SPDX-License-Identifier: GPL-2.0-only */
/* RetroAchievements no pingo-retro (rcheevos + libcurl). Sem PINGO_RA
 * (build no PC sem as bibliotecas), tudo vira função vazia. */
#ifndef PINGO_RA_H
#define PINGO_RA_H

#include <stdbool.h>
#include <stddef.h>

#include "libretro.h"

/* Conta salva pela tela do launcher (usuário e token, nunca a senha) */
#define RA_FILE "/mnt/sd1/.system/retroachievements.txt"

/* Aviso na tela: o pingo-retro desenha (título, texto e, se houver, a
 * imagem da conquista: PNG no cache, que pode chegar depois do aviso) */
typedef void (*ra_notify_fn)(const char *title, const char *text, const char *image);

struct ra_cheevo {
	char image[160]; /* PNG no cache (pode ainda não existir) */
	char title[96];
	char description[160];
	unsigned points;
	bool unlocked;
};

#ifdef PINGO_RA
void ra_set_notify(ra_notify_fn fn);
bool ra_start(const char *console_code, const char *core_name, const char *core_version);
void ra_set_memory_map(const struct retro_memory_map *mmap);
void ra_load_game(const char *path, const void *data, size_t size,
		  void *(*get_mem)(unsigned), size_t (*get_size)(unsigned));
void ra_frame(void);  /* depois de cada core.run() */
void ra_pump(void);   /* respostas da internet */
void ra_idle(void);   /* com o jogo pausado (menu) */
void ra_reset(void);
void ra_stop(void);
bool ra_active(void); /* conta e jogo reconhecido */
bool ra_hardcore(void);
bool ra_setting_allowed(const char *key, const char *value); /* hardcore */
int ra_list(struct ra_cheevo *out, int max, unsigned *unlocked, unsigned *total);
int ra_login_cli(const char *user); /* --ra-login: senha pela entrada padrão */
#else
static inline void ra_set_notify(ra_notify_fn fn) { (void)fn; }
static inline bool ra_start(const char *c, const char *n, const char *v)
{ (void)c; (void)n; (void)v; return false; }
static inline void ra_set_memory_map(const struct retro_memory_map *m) { (void)m; }
static inline void ra_load_game(const char *p, const void *d, size_t s, void *(*a)(unsigned),
				size_t (*b)(unsigned)) { (void)p; (void)d; (void)s; (void)a; (void)b; }
static inline void ra_frame(void) {}
static inline void ra_pump(void) {}
static inline void ra_idle(void) {}
static inline void ra_reset(void) {}
static inline void ra_stop(void) {}
static inline bool ra_active(void) { return false; }
static inline bool ra_hardcore(void) { return false; }
static inline bool ra_setting_allowed(const char *k, const char *v) { (void)k; (void)v; return true; }
static inline int ra_list(struct ra_cheevo *o, int m, unsigned *u, unsigned *t)
{ (void)o; (void)m; *u = *t = 0; return 0; }
static inline int ra_login_cli(const char *u) { (void)u; return 3; }
#endif

#endif
