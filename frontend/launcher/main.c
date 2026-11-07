/*
 * pingo-launcher: tela inicial e listas de jogos do Pingo OS.
 *
 * Desenha só quando algo muda (botão, relógio, bateria, nova varredura),
 * para gastar pouca bateria. Para abrir um jogo, grava o comando em
 * /run/pingo/next e sai: o pingo-shell roda o jogo e depois reabre o
 * launcher, que volta para o mesmo lugar (/run/pingo/launcher.state).
 *
 * Copyright (C) 2026 Pingo OS by Retrogamer Brasil
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <SDL2/SDL.h>

#include "library.h"
#include "fbcopy.h"
#include "osd.h"
#include "strings_pt.h"
#include "ui.h"

#define RUN_DIR       "/run/pingo"
#define NEXT_FILE     RUN_DIR "/next"
#define STATE_FILE    RUN_DIR "/launcher.state"
#define CACHE_FILE    "/mnt/sd1/.system/cache/launcher.cache"
#define RECENTS_FILE  "/mnt/sd1/Config/recentes.txt"
#define RECENTS_MAX   30
#define RECENTS_PATH  "pingo:recentes"
#define FAV_FILE      "/mnt/sd1/Config/favoritos.txt"
#define FAV_MAX       1000
#define FAV_PATH      "pingo:favoritos"

/* entrada (evdev), usada também pelas telas modais */
enum { DEV_PAD, DEV_POWER, DEV_VOLUME, DEV_N };
static struct pollfd fds[DEV_N];
static void suspend(void);
static void shutdown_now(void);

#define POWER_HOLD_MS 2000
static bool power_down;
static Uint32 power_down_at;

static int pending_console = -1; /* entrar neste console quando a lista nova chegar */

/* ---- estado da navegação --------------------------------------------- */

struct level {
	struct node *dir;
	int sel, top;
};

static struct level stack[64];
static int depth;            /* stack[depth] é a tela atual */
static struct node *lib;     /* árvore da biblioteca (dona dos nós) */
static struct node *recents; /* "Recentes" (dono dos nós dele) */
static struct node *favorites; /* "Favoritos" (idem), em ordem alfabética */
static struct node home;     /* Início: aponta para recents e lib->kids */
static struct node random_item; /* "Aleatório" no Início */
#define RANDOM_PATH "pingo:aleatorio"
static bool dirty = true;
static bool have_roms;

static char toast[128];
static Uint32 toast_until;

static void show_toast(const char *msg)
{
	snprintf(toast, sizeof(toast), "%s", msg);
	toast_until = SDL_GetTicks() + 2500;
	dirty = true;
}

/* ---- Recentes -------------------------------------------------------- */

static int console_by_code(const char *code)
{
	for (int i = 0; i < n_consoles; i++)
		if (!strcmp(consoles[i].code, code))
			return i;
	return -1;
}

/* caminho relativo à pasta do console ("MD/1 US/Jogo.md" -> "1 US/Jogo.md"),
 * em qualquer um dos dois cartões; fora delas (.m3u gerado), o completo */
static const char *rel_path(const struct node *game)
{
	const struct node *cn = library_console(lib, game->console);
	const char *roots[2] = { cn ? cn->path : NULL, cn ? cn->path2 : NULL };
	for (int i = 0; i < 2; i++) {
		size_t n = roots[i] ? strlen(roots[i]) : 0;
		if (n && !strncmp(game->path, roots[i], n) && game->path[n] == '/')
			return game->path + n + 1;
	}
	return game->path;
}

/* Lista de jogos num arquivo "CONSOLE<tab>caminho relativo" (Recentes e
 * Favoritos). Jogos que não estão mais em nenhum dos cartões ficam de fora. */
static struct node *game_list_load(const char *file, int max, const char *label,
				   const char *vpath)
{
	FILE *f = fopen(file, "r");
	char line[4096];

	struct node *list = calloc(1, sizeof(*list));
	list->fname = strdup(vpath);
	list->path = strdup(vpath);
	list->label = strdup(label);
	list->dir = true;
	list->console = -1;
	if (!f)
		return list;
	while (fgets(line, sizeof(line), f) && list->nkids < max) {
		char *tab = strchr(line, '\t'), *nl = strchr(line, '\n');
		if (!tab)
			continue;
		*tab = 0;
		if (nl)
			*nl = 0;
		int c = console_by_code(line);
		if (c < 0)
			continue;
		char path[4096];
		struct stat st;
		const struct node *cn = library_console(lib, c);
		bool found = false;
		if (tab[1] == '/') {
			snprintf(path, sizeof(path), "%s", tab + 1);
			found = stat(path, &st) == 0;
		} else if (cn) { /* o jogo pode estar em qualquer um dos dois cartões */
			const char *roots[2] = { cn->path, cn->path2 };
			for (int r = 0; r < 2 && !found; r++) {
				if (!roots[r])
					continue;
				snprintf(path, sizeof(path), "%s/%s", roots[r], tab + 1);
				found = stat(path, &st) == 0;
			}
		}
		if (!found) /* jogo apagado, renomeado ou no cartão que saiu */
			continue;
		struct node *g = library_game_node(c, path);
		list->kids = realloc(list->kids, sizeof(*list->kids) * (list->nkids + 1));
		list->kids[list->nkids++] = g;
	}
	list->games = list->nkids;
	fclose(f);
	return list;
}

static void recents_load(void)
{
	library_free(recents);
	recents = game_list_load(RECENTS_FILE, RECENTS_MAX, S_RECENTS, RECENTS_PATH);
}

static int label_cmp(const void *a, const void *b)
{
	const struct node *x = *(const struct node *const *)a, *y = *(const struct node *const *)b;
	return natural_cmp(x->label, y->label);
}

static void favorites_load(void)
{
	library_free(favorites);
	favorites = game_list_load(FAV_FILE, FAV_MAX, S_FAVORITES, FAV_PATH);
	qsort(favorites->kids, favorites->nkids, sizeof(*favorites->kids), label_cmp);
}

static void recents_add(const struct node *game)
{
	char tmp[] = RECENTS_FILE ".tmp";
	FILE *f = fopen(tmp, "w");
	if (!f)
		return;
	fprintf(f, "%s\t%s\n", consoles[game->console].code, rel_path(game));
	int n = 1;
	for (int i = 0; recents && i < recents->nkids && n < RECENTS_MAX; i++) {
		struct node *r = recents->kids[i];
		if (!strcmp(r->path, game->path))
			continue;
		fprintf(f, "%s\t%s\n", consoles[r->console].code, rel_path(r));
		n++;
	}
	fflush(f);
	fsync(fileno(f));
	fclose(f);
	rename(tmp, RECENTS_FILE);
}

static int favorite_index(const struct node *game)
{
	for (int i = 0; favorites && i < favorites->nkids; i++)
		if (!strcmp(favorites->kids[i]->path, game->path))
			return i;
	return -1;
}

static void favorites_save(void)
{
	char tmp[] = FAV_FILE ".tmp";
	FILE *f = fopen(tmp, "w");
	if (!f)
		return;
	for (int i = 0; i < favorites->nkids; i++)
		fprintf(f, "%s\t%s\n", consoles[favorites->kids[i]->console].code,
			rel_path(favorites->kids[i]));
	fflush(f);
	fsync(fileno(f));
	fclose(f);
	rename(tmp, FAV_FILE);
}

static void home_build(void);

/* Y num jogo: entra ou sai dos Favoritos */
static void favorite_toggle(struct node *game)
{
	if (!game || game->dir || game->console < 0 || !favorites)
		return;
	int i = favorite_index(game);
	if (i >= 0) {
		library_free(favorites->kids[i]);
		memmove(&favorites->kids[i], &favorites->kids[i + 1],
			sizeof(*favorites->kids) * (favorites->nkids - i - 1));
		favorites->nkids--;
		show_toast(S_FAV_REMOVED);
	} else {
		favorites->kids = realloc(favorites->kids,
					  sizeof(*favorites->kids) * (favorites->nkids + 1));
		favorites->kids[favorites->nkids++] = library_game_node(game->console, game->path);
		qsort(favorites->kids, favorites->nkids, sizeof(*favorites->kids), label_cmp);
		show_toast(S_FAV_ADDED);
	}
	favorites->games = favorites->nkids;
	favorites_save();
}

/* ---- Início ---------------------------------------------------------- */

static void home_build(void)
{
	free(home.kids);
	memset(&home, 0, sizeof(home));
	home.dir = true;
	home.console = -1;
	home.path = (char *)"pingo:inicio";
	home.label = (char *)"";
	int n = (lib ? lib->nkids : 0) + 3;
	home.kids = calloc(n, sizeof(*home.kids));
	if (recents && recents->nkids > 0)
		home.kids[home.nkids++] = recents;
	if (favorites && favorites->nkids > 0)
		home.kids[home.nkids++] = favorites;
	if (lib && lib->games > 0) {
		random_item = (struct node){ 0 };
		random_item.fname = random_item.path = (char *)RANDOM_PATH;
		random_item.label = (char *)S_RANDOM;
		random_item.console = -1;
		random_item.games = lib->games;
		home.kids[home.nkids++] = &random_item;
	}
	for (int i = 0; lib && i < lib->nkids; i++)
		home.kids[home.nkids++] = lib->kids[i];
}

static struct level *cur(void)
{
	return &stack[depth];
}

static void clamp_level(struct level *l)
{
	int n = l->dir ? l->dir->nkids : 0;
	if (l->sel >= n)
		l->sel = n - 1;
	if (l->sel < 0)
		l->sel = 0;
	if (l->sel < l->top)
		l->top = l->sel;
	if (l->sel >= l->top + ROWS)
		l->top = l->sel - ROWS + 1;
	if (l->top > n - ROWS)
		l->top = n - ROWS;
	if (l->top < 0)
		l->top = 0;
}

static struct node *selected(void)
{
	struct level *l = cur();
	if (!l->dir || l->sel >= l->dir->nkids)
		return NULL;
	return l->dir->kids[l->sel];
}

/* ---- estado entre execuções (volta para o mesmo lugar) ---------------- *
 * Uma linha por nível: o caminho da pasta aberta e o do item selecionado.
 */

static void state_save(void)
{
	FILE *f = fopen(STATE_FILE, "w");
	if (!f)
		return;
	for (int i = 0; i <= depth; i++) {
		struct level *l = &stack[i];
		const char *sel = (l->dir && l->sel < l->dir->nkids) ? l->dir->kids[l->sel]->path : "";
		fprintf(f, "%s\t%s\n", l->dir ? l->dir->path : "", sel);
	}
	fclose(f);
}

/* Refaz a pilha a partir dos caminhos, numa árvore nova (estado salvo ou
 * nova varredura). Para no primeiro nível que não existir mais. */
static void restore_paths(char paths[][2][4096], int n)
{
	depth = 0;
	stack[0] = (struct level){ &home, 0, 0 };
	for (int i = 0; i < n; i++) {
		if (i > 0) {
			struct node *parent = stack[depth].dir;
			int k = library_find_kid(parent, paths[i][0]);
			if (k < 0 || !parent->kids[k]->dir)
				break;
			depth++;
			stack[depth] = (struct level){ parent->kids[k], 0, 0 };
		}
		struct level *l = &stack[depth];
		int s = library_find_kid(l->dir, paths[i][1]);
		l->sel = s < 0 ? 0 : s;
		/* item selecionado mais ou menos no meio da tela */
		l->top = l->sel - ROWS / 2;
		clamp_level(l);
	}
}

static void state_load(void)
{
	static char paths[64][2][4096];
	FILE *f = fopen(STATE_FILE, "r");
	char line[4096];
	int n = 0;

	if (!f)
		return;
	while (n < 64 && fgets(line, sizeof(line), f)) {
		char *tab = strchr(line, '\t'), *nl = strchr(line, '\n');
		if (!tab)
			continue;
		*tab = 0;
		if (nl)
			*nl = 0;
		snprintf(paths[n][0], sizeof(paths[n][0]), "%s", line);
		snprintf(paths[n][1], sizeof(paths[n][1]), "%s", tab + 1);
		n++;
	}
	fclose(f);
	if (n > 0)
		restore_paths(paths, n);
}

/* Nova árvore da varredura: troca mantendo a posição */
static void swap_library(struct node *fresh)
{
	static char paths[64][2][4096];
	int n = depth + 1;

	for (int i = 0; i < n; i++) {
		struct level *l = &stack[i];
		snprintf(paths[i][0], sizeof(paths[i][0]), "%s", l->dir ? l->dir->path : "");
		snprintf(paths[i][1], sizeof(paths[i][1]), "%s",
			 (l->dir && l->sel < l->dir->nkids) ? l->dir->kids[l->sel]->path : "");
	}
	struct node *old = lib;
	lib = fresh;
	recents_load();
	favorites_load();
	home_build();
	restore_paths(paths, n);
	if (pending_console >= 0) { /* acabou de trocar a origem: abre o console de novo */
		depth = 0;
		for (int i = 0; i < home.nkids; i++)
			if (home.kids[i]->console == pending_console && home.kids[i] != recents &&
			    home.kids[i] != favorites) {
				stack[0].sel = i;
				clamp_level(&stack[0]);
				stack[++depth] = (struct level){ home.kids[i], 0, 0 };
				break;
			}
		pending_console = -1;
	}
	library_free(old);
	dirty = true;
}

static bool read_word(const char *path, const char *word);

/* ---- origem dos jogos (SD1/SD2) por console --------------------------- *
 * Config/origem.cfg: "PS=sd2". Padrão: SD1. */

#define ORIGIN_FILE "/mnt/sd1/Config/origem.cfg"
#define SD2_STATE   RUN_DIR "/sd2"
#define SD2_CHANGED RUN_DIR "/sd2.changed"

static int origin[64];
static bool sd2_present;

static void origin_load(void)
{
	FILE *f = fopen(ORIGIN_FILE, "r");
	char line[128];
	for (int c = 0; c < 64; c++)
		origin[c] = 0; /* não escolhida */
	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		char *eq = strchr(line, '=');
		if (!eq)
			continue;
		*eq = 0;
		int c = console_by_code(line);
		if (c >= 0 && c < 64)
			origin[c] = !strncmp(eq + 1, "sd2", 3) ? 2 : (!strncmp(eq + 1, "sd1", 3) ? 1 : 0);
	}
	fclose(f);
}

static void origin_save(void)
{
	FILE *f = fopen(ORIGIN_FILE ".tmp", "w");
	if (!f)
		return;
	for (int c = 0; c < n_consoles && c < 64; c++)
		if (origin[c])
			fprintf(f, "%s=%s\n", consoles[c].code, origin[c] == 2 ? "sd2" : "sd1");
	fflush(f);
	fsync(fileno(f));
	fclose(f);
	rename(ORIGIN_FILE ".tmp", ORIGIN_FILE);
}

static char sd2_serial[64];
static bool sd2_ready;

/* "ok <partição> <série> <preparado>" ou "none" */
static void sd2_read(void)
{
	FILE *f = fopen(SD2_STATE, "r");
	char ok[8] = "", part[128] = "", serial[64] = "";
	int ready = 0;
	sd2_present = false;
	sd2_serial[0] = 0;
	sd2_ready = false;
	if (!f)
		return;
	if (fscanf(f, "%7s %127s %63s %d", ok, part, serial, &ready) >= 1 && !strcmp(ok, "ok")) {
		sd2_present = true;
		snprintf(sd2_serial, sizeof(sd2_serial), "%s", serial);
		sd2_ready = ready == 1;
	}
	fclose(f);
}

/* Cartões em que o usuário respondeu "agora não" (por número de série) */
#define SD2_IGNORED "/mnt/sd1/Config/sd2-ignorados.txt"

static bool sd2_ignored(void)
{
	FILE *f = fopen(SD2_IGNORED, "r");
	char line[96];
	bool found = false;
	if (!f)
		return false;
	while (!found && fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\n")] = 0;
		found = !strcmp(line, sd2_serial);
	}
	fclose(f);
	return found;
}

static void sd2_ignore(void)
{
	FILE *f = fopen(SD2_IGNORED, "a");
	if (f) {
		fprintf(f, "%s\n", sd2_serial);
		fflush(f);
		fsync(fileno(f));
		fclose(f);
	}
}

/* ---- varredura em segundo plano -------------------------------------- */

static SDL_atomic_t scan_done;
static struct node *scan_result;
static bool scanning, scan_again;

static int scan_thread(void *arg)
{
	(void)arg;
	scan_result = library_scan(origin, sd2_present);
	SDL_AtomicSet(&scan_done, 1);
	return 0;
}

static void start_scan(void)
{
	if (scanning) {
		scan_again = true; /* a lista vai mudar de novo: refaz quando esta terminar */
		return;
	}
	scanning = true;
	SDL_AtomicSet(&scan_done, 0);
	SDL_Thread *t = SDL_CreateThread(scan_thread, "scan", NULL);
	if (t)
		SDL_DetachThread(t);
	else
		scanning = false;
}

/* ---- status: relógio, bateria e Wi-Fi --------------------------------- */

static struct {
	char clock[8];
	int battery;
	bool charging, wifi;
} status;

static int read_int(const char *path, int def)
{
	FILE *f = fopen(path, "r");
	int v = def;
	if (f) {
		if (fscanf(f, "%d", &v) != 1)
			v = def;
		fclose(f);
	}
	return v;
}

static bool read_word(const char *path, const char *word)
{
	char buf[32] = "";
	FILE *f = fopen(path, "r");
	if (f) {
		if (!fgets(buf, sizeof(buf), f))
			buf[0] = 0;
		fclose(f);
	}
	return !strncmp(buf, word, strlen(word));
}

static void status_update(void)
{
	time_t t = time(NULL);
	struct tm tm;
	char clock[8];
	localtime_r(&t, &tm);
	strftime(clock, sizeof(clock), "%H:%M", &tm);

	int bat = read_int("/sys/class/power_supply/battery/capacity", -1);
	bool chg = read_word("/sys/class/power_supply/battery/status", "Charging");
	bool wifi = read_word("/sys/class/net/wlan0/operstate", "up");

	if (strcmp(clock, status.clock) || bat != status.battery || chg != status.charging ||
	    wifi != status.wifi) {
		snprintf(status.clock, sizeof(status.clock), "%s", clock);
		status.battery = bat;
		status.charging = chg;
		status.wifi = wifi;
		dirty = true;
	}
}

/* ---- desenho ---------------------------------------------------------- */

/* Cores da tela: o Início é escuro; as listas de jogos, claras (fundo
 * claro, destaque preto), como no Analogue Pocket */
static struct theme {
	Uint32 bg, text, dim, sel_bg, sel_fg;
} th_dark = { COL_BG, COL_TEXT, COL_DIM, COL_SEL_BG, COL_SEL_FG },
  th_light = { 0xF2F2F2, 0x141414, 0x6A6A6A, 0x141414, 0xF2F2F2 }, th;

#define LIST_INDENT 12 /* margem esquerda extra das listas */
#define SEL_INSET_LIGHT 3 /* destaque um pouco mais baixo nas listas de jogos */

static int list_y(int row)
{
	return PAD_Y + BAR_H + LIST_GAP + row * (ROW_H + ROW_GAP);
}

static void draw_status_right(void)
{
	int y = PAD_Y + BAR_H / 2, x = UI_W - PAD_X;
	char pct[16];

	if (status.battery >= 0) {
		snprintf(pct, sizeof(pct), "%d%%", status.battery);
		x -= ui_text(F_BAR, pct, x, y, th.dim, 1, 0);
		x -= 10 + 26;
		ui_icon_battery(x, y, status.battery, status.charging, th.dim);
	}
	if (status.wifi) {
		x -= 10 + 18;
		ui_icon_wifi(x, y, th.dim);
	}
}

/* Dica do rodapé ("A abrir"), da direita para a esquerda */
static int draw_hint(int right, const char *button, const char *text)
{
	int y = UI_H - PAD_Y - BAR_H / 2;
	int w = ui_text(F_BAR, text, right, y, th.dim, 1, 0);
	int sp = ui_text_width(F_BAR, " ");
	ui_text(F_BAR_BOLD, button, right - w - sp, y, th.text, 1, 0);
	return w + sp + ui_text_width(F_BAR_BOLD, button);
}

static void count_text(const struct node *dir, char *buf, size_t n)
{
	bool has_dirs = false;
	for (int i = 0; i < dir->nkids; i++)
		has_dirs |= dir->kids[i]->dir;
	if (has_dirs)
		snprintf(buf, n, dir->nkids == 1 ? S_ITEMS_1 : S_ITEMS_N, dir->nkids);
	else
		snprintf(buf, n, dir->nkids == 1 ? S_GAMES_1 : S_GAMES_N, dir->nkids);
}

static void draw(void)
{
	struct level *l = cur();
	bool at_home = depth == 0;
	int bar_y = PAD_Y + BAR_H / 2, foot_y = UI_H - PAD_Y - BAR_H / 2;

	th = at_home ? th_dark : th_light;
	ui_clear_rgb(th.bg);

	/* barra superior */
	if (at_home) {
		ui_text(F_BAR, status.clock, PAD_X, bar_y, th.dim, 0, 0);
	} else {
		char cnt[32];
		count_text(l->dir, cnt, sizeof(cnt));
		int cw = ui_text_width(F_BAR, cnt);
		int w = ui_text(F_BAR_BOLD, l->dir->label, PAD_X, bar_y, th.text, 0, 380 - cw);
		ui_text(F_BAR, cnt, PAD_X + w + 10, bar_y, th.dim, 0, 0);
	}
	draw_status_right();

	/* lista */
	int n = l->dir ? l->dir->nkids : 0;
	if (n == 0) {
		ui_text(F_TITLE, have_roms ? S_EMPTY_TITLE : S_NO_CARD, UI_W / 2, UI_H / 2 - 16,
			th.text, 2, UI_W - 2 * PAD_X);
		if (have_roms)
			ui_text(F_BAR, S_EMPTY_HINT,
				UI_W / 2, UI_H / 2 + 18, th.dim, 2, UI_W - 2 * PAD_X);
	}
	for (int row = 0; row < ROWS && l->top + row < n; row++) {
		int i = l->top + row;
		struct node *it = l->dir->kids[i];
		bool sel = i == l->sel;
		int y = list_y(row), x = PAD_X + LIST_INDENT, w = UI_W - 2 * PAD_X - LIST_INDENT;
		Uint32 fg = sel ? th.sel_fg : th.text;

		if (sel) {
			int in = at_home ? 0 : SEL_INSET_LIGHT;
			ui_round_rect(x, y + in, w, ROW_H - 2 * in, ROW_RADIUS, th.sel_bg);
		}

		int right = 0;
		if (!at_home && it->discs > 1) { /* "3 discos", como no mockup */
			char d[32];
			snprintf(d, sizeof(d), S_DISCS_N, it->discs);
			right = ui_text(F_SMALL, d, x + w - ROW_PAD, y + ROW_H / 2,
					sel ? th.sel_fg : th.dim, 1, 0) + 12;
		}
		if (!at_home && !it->dir && l->dir != favorites && favorite_index(it) >= 0) {
			/* ★ nos favoritos (na lista Favoritos todos são) */
			right += ui_text(F_COUNT, "★", x + w - ROW_PAD - right, y + ROW_H / 2,
					 sel ? th.sel_fg : th.dim, 1, 0) + 10;
		}
		if (at_home) { /* contagem à direita */
			char cnt[16];
			snprintf(cnt, sizeof(cnt), "%d", it->games);
			right = ui_text(F_COUNT, cnt, x + w - ROW_PAD, y + ROW_H / 2,
					sel ? th.sel_fg : th.dim, 1, 0) + 12;
		}
		ui_text(sel ? F_ITEM_SEL : F_ITEM, it->label, x + ROW_PAD, y + ROW_H / 2, fg, 0,
			w - 2 * ROW_PAD - right);
	}

	/* rodapé */
	int right = UI_W - PAD_X;
	if (toast[0] && SDL_GetTicks() < toast_until) {
		ui_text(F_BAR, toast, PAD_X, foot_y, th.text, 0, UI_W - 2 * PAD_X);
	} else {
		toast[0] = 0;
		if (!at_home && n > 0) {
			char pos[32];
			snprintf(pos, sizeof(pos), "%d / %d", l->sel + 1, n);
			ui_text(F_BAR, pos, PAD_X, foot_y, th.dim, 0, 0);
		}
		if (at_home) {
			right -= draw_hint(right, "MENU", S_OPTIONS) + 22;
			if (n > 0)
				draw_hint(right, "A", S_OPEN);
		} else {
			right -= draw_hint(right, "B", S_BACK) + 22;
			struct node *s = selected();
			if (s)
				right -= draw_hint(right, "A", s->dir ? S_OPEN : S_PLAY) + 22;
			if (s && !s->dir)
				right -= draw_hint(right, "Y", favorite_index(s) >= 0 ? S_FAV_HINT_OFF
										   : S_FAV_HINT) + 22;
			if (l->dir != recents && l->dir->games > 1)
				draw_hint(right, "X", S_RANDOM_HINT);
		}
	}

	osd_draw(ui_renderer(), UI_W, UI_H);
	ui_present();
	th = th_dark; /* as outras telas são escuras */
	dirty = false;
}

/* ---- ações ------------------------------------------------------------ */

static void message_screen(const char *title, const char *hint);

static bool launch(struct node *game)
{
	char core[512];
	struct stat st;

	snprintf(core, sizeof(core), "%s/%s_libretro.so", CORES_DIR, consoles[game->console].core);
	if (stat(core, &st) < 0) {
		show_toast(S_NO_CORE);
		return false;
	}
	if (stat(game->path, &st) < 0) {
		show_toast(S_MISSING_GAME);
		return false;
	}

	recents_add(game);
	/* "Carregando…" fica no fb0 até o jogo abrir (senão aparece o logo) */
	message_screen(S_LOADING, game->label);
	mkdir(RUN_DIR, 0755);
	FILE *f = fopen(NEXT_FILE ".tmp", "w");
	if (!f)
		return false;
	fprintf(f, "%s\n%s\n%s\n", core, game->path, consoles[game->console].code);
	fclose(f);
	rename(NEXT_FILE ".tmp", NEXT_FILE);
	return true;
}

static void move(int delta, bool wrap)
{
	struct level *l = cur();
	int n = l->dir ? l->dir->nkids : 0;
	if (n == 0)
		return;
	int s = l->sel + delta;
	if (wrap) {
		if (s < 0)
			s = n - 1;
		else if (s >= n)
			s = 0;
	}
	l->sel = s;
	clamp_level(l);
	dirty = true;
}

/* Grupo da letra inicial: A-Z (acentos contam como a letra base), '#'
 * para números e símbolos. Usado pelo L1/R1. */
static int letter_group(const char *label)
{
	const unsigned char *p = (const unsigned char *)label;
	int c = p[0];

	if (c == 0xC3 && p[1]) { /* Latin-1 em UTF-8: À-ÿ */
		static const char map[] = "AAAAAAACEEEEIIIIDNOOOOO#OUUUUYTSAAAAAAACEEEEIIIIDNOOOOO#OUUUUYTY";
		int i = p[1] - 0x80;
		c = (i >= 0 && i < 64) ? map[i] : '#';
	}
	c = toupper(c);
	return (c >= 'A' && c <= 'Z') ? c : '#';
}

/* R1: primeiro item da próxima letra; L1: primeiro da letra anterior */
static void jump_letter(int dir)
{
	struct level *l = cur();
	int n = l->dir ? l->dir->nkids : 0;
	if (n == 0)
		return;
	struct node **k = l->dir->kids;
	int s = l->sel, g = letter_group(k[s]->label);

	if (dir > 0) {
		while (s < n && letter_group(k[s]->label) == g)
			s++;
		if (s >= n)
			return;
	} else {
		/* início do grupo atual; se já estiver nele, vai para o anterior */
		while (s > 0 && letter_group(k[s - 1]->label) == g)
			s--;
		if (s == l->sel) {
			if (s == 0)
				return;
			s--;
			g = letter_group(k[s]->label);
			while (s > 0 && letter_group(k[s - 1]->label) == g)
				s--;
		}
	}
	l->sel = s;
	clamp_level(l);
	dirty = true;
}

/* Sorteia um jogo dentro de dir (e das subpastas); cada jogo tem a mesma chance */
static struct node *random_game(struct node *dir)
{
	if (!dir || dir->games <= 0)
		return NULL;
	int k = rand() % dir->games;
	for (;;) {
		if (!dir->dir)
			return dir;
		struct node *next = NULL;
		for (int i = 0; i < dir->nkids; i++) {
			if (k < dir->kids[i]->games) {
				next = dir->kids[i];
				break;
			}
			k -= dir->kids[i]->games;
		}
		if (!next)
			return NULL;
		dir = next;
	}
}

static bool open_random(struct node *dir)
{
	struct node *g = random_game(dir);
	if (!g)
		return false;
	fprintf(stderr, "pingo-launcher: aleatório: %s\n", g->path);
	return launch(g);
}

/* Lê o próximo botão apertado (com power: toque suspende, segurar desliga) */
static int wait_key(void)
{
	struct input_event ev;
	for (;;) {
		poll(fds, DEV_N, power_down ? 50 : 1000);
		while (fds[DEV_PAD].fd >= 0 && read(fds[DEV_PAD].fd, &ev, sizeof(ev)) == sizeof(ev)) {
			if (ev.type != EV_KEY)
				continue;
			if (osd_key(ev.code, ev.value))
				return -1; /* volume: redesenha */
			if (ev.code == BTN_MODE) {
				if (osd_menu_tapped())
					return BTN_MODE;
				continue;
			}
			if (ev.value == 1)
				return ev.code;
		}
		while (fds[DEV_VOLUME].fd >= 0 &&
		       read(fds[DEV_VOLUME].fd, &ev, sizeof(ev)) == sizeof(ev))
			if (ev.type == EV_KEY && osd_key(ev.code, ev.value))
				return -1;
		while (fds[DEV_POWER].fd >= 0 &&
		       read(fds[DEV_POWER].fd, &ev, sizeof(ev)) == sizeof(ev)) {
			if (ev.type != EV_KEY || ev.code != KEY_POWER)
				continue;
			if (ev.value == 1) {
				power_down = true;
				power_down_at = SDL_GetTicks();
			} else if (ev.value == 0 && power_down) {
				power_down = false;
				suspend();
				return -1;
			}
		}
		if (power_down && SDL_GetTicks() - power_down_at >= POWER_HOLD_MS)
			shutdown_now();
		if (osd_visible() || osd_changed())
			return -1;
	}
}

static void change_origin(int c, int to)
{
	if (origin[c] == to)
		return;
	origin[c] = to;
	origin_save();
	pending_console = c;
	show_toast(S_READING);
	start_scan();
}

static const char *origin_name(int o)
{
	return o == 1 ? "SD1" : (o == 2 ? "SD2" : S_BOTH);
}

/* MENU dentro de um console: "Origem: Ambos / SD1 / SD2". Um cartão sem
 * jogos deste console é recusado, com aviso. */
static void console_menu(int c)
{
	const struct node *cn = library_console(lib, c);
	int g1 = cn ? cn->games1 : 0, g2 = cn && sd2_present ? cn->games2 : 0;
	int to = origin[c];
	char warn[160] = "";
	if (!sd2_present)
		to = 1;
	else if ((to == 1 && g1 == 0) || (to == 2 && g2 == 0))
		to = 0; /* a escolha salva aponta para um cartão sem jogos */

	for (;;) {
		ui_clear();
		ui_text(F_BAR_BOLD, consoles[c].name, PAD_X, PAD_Y + BAR_H / 2, COL_TEXT, 0, 420);
		int y = PAD_Y + BAR_H + LIST_GAP;
		ui_round_rect(PAD_X, y, UI_W - 2 * PAD_X, ROW_H, ROW_RADIUS, COL_SEL_BG);
		ui_text(F_ITEM_SEL, S_ORIGIN, PAD_X + ROW_PAD, y + ROW_H / 2, COL_SEL_FG, 0, 0);
		ui_text(F_COUNT, origin_name(to), UI_W - PAD_X - ROW_PAD, y + ROW_H / 2, COL_SEL_FG,
			1, 0);
		char info[96];
		snprintf(info, sizeof(info), S_ORIGIN_COUNTS, g1, g2);
		ui_text(F_BAR, sd2_present ? info : S_ONLY_SD1, PAD_X + ROW_PAD, y + ROW_H + 24,
			COL_DIM, 0, UI_W - 2 * PAD_X);
		if (warn[0])
			ui_text(F_BAR, warn, PAD_X, UI_H - PAD_Y - BAR_H / 2, COL_TEXT, 0, 380);
		int right = UI_W - PAD_X;
		right -= draw_hint(right, "B", S_BACK) + 22;
		draw_hint(right, "◀ ▶", S_HINT_CHANGE);
		osd_draw(ui_renderer(), UI_W, UI_H);
		ui_present();

		int k = wait_key();
		int d = (k == BTN_DPAD_LEFT) ? -1 : (k == BTN_DPAD_RIGHT || k == BTN_EAST) ? 1 : 0;
		if (d && !sd2_present) {
			snprintf(warn, sizeof(warn), "%s", S_ONLY_SD1);
		} else if (d) {
			int next = (to + d + 3) % 3; /* 0 ambos, 1 SD1, 2 SD2 */
			warn[0] = 0;
			if ((next == 1 && g1 == 0) || (next == 2 && g2 == 0)) {
				/* pula o cartão vazio e avisa */
				snprintf(warn, sizeof(warn), S_NO_GAMES_ON, consoles[c].name,
					 next == 1 ? "SD1" : "SD2");
				next = (next + d + 3) % 3;
				if ((next == 1 && g1 == 0) || (next == 2 && g2 == 0))
					next = 0;
			}
			to = next;
		} else if (k == BTN_SOUTH || k == BTN_MODE) {
			break;
		}
	}
	if (sd2_present)
		change_origin(c, to);
	dirty = true;
}

static bool open_selected(void)
{
	struct node *it = selected();
	if (!it)
		return false;

	if (it == &random_item)
		return open_random(lib);
	if (it->dir) {
		if (depth + 1 >= (int)(sizeof(stack) / sizeof(stack[0])))
			return false;
		stack[++depth] = (struct level){ it, 0, 0 };
		dirty = true;
		return false;
	}
	return launch(it);
}

static void go_back(void)
{
	if (depth > 0) {
		depth--;
		dirty = true;
	}
}

/* ---- entrada (evdev) -------------------------------------------------- */


static int open_by_name(const char *want)
{
	DIR *d = opendir("/dev/input");
	struct dirent *e;
	char path[300], name[256];

	if (!d)
		return -1;
	while ((e = readdir(d))) {
		if (strncmp(e->d_name, "event", 5))
			continue;
		snprintf(path, sizeof(path), "/dev/input/%s", e->d_name);
		int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0)
			continue;
		if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) >= 0 && !strcmp(name, want)) {
			closedir(d);
			return fd;
		}
		close(fd);
	}
	closedir(d);
	return -1;
}

static void drain(int fd)
{
	struct input_event ev;
	while (fd >= 0 && read(fd, &ev, sizeof(ev)) == sizeof(ev))
		;
}

/* Repetição ao segurar: 300 ms até começar, depois a cada 60 ms */
static int held = -1;
static Uint32 held_next;

static bool repeatable(int code)
{
	return code == BTN_DPAD_UP || code == BTN_DPAD_DOWN || code == BTN_DPAD_LEFT ||
	       code == BTN_DPAD_RIGHT || code == BTN_TL || code == BTN_TR;
}

/* devolve true quando é para sair e abrir um jogo */
static bool press(int code)
{
	switch (code) {
	case BTN_DPAD_UP:    move(-1, true); break;
	case BTN_DPAD_DOWN:  move(1, true); break;
	case BTN_DPAD_LEFT:  move(-ROWS, false); break;
	case BTN_DPAD_RIGHT: move(ROWS, false); break;
	case BTN_TL:         jump_letter(-1); break;
	case BTN_TR:         jump_letter(1); break;
	case BTN_EAST:       return open_selected(); /* A */
	case BTN_SOUTH:      go_back(); break;       /* B */
	case BTN_WEST: /* X (em cima; no device tree do H700 o X é BTN_WEST): aleatório da pasta */
		if (depth > 0 && cur()->dir != recents)
			return open_random(cur()->dir);
		break;
	case BTN_NORTH: /* Y (à esquerda; no device tree é BTN_NORTH): favorito */
		if (depth > 0) {
			struct level *l = cur();
			favorite_toggle(selected());
			if (l->dir == favorites) /* saiu da lista em que estamos */
				clamp_level(l);
			home_build();
			dirty = true;
		}
		break;
	}
	return false;
}


/* Proteção de tela (só no launcher): apaga a luz depois de um tempo parado */
#define SCREEN_OFF_MS (2 * 60 * 1000)
static Uint32 last_input;
static bool screen_off;

static void screen_wake(void)
{
	last_input = SDL_GetTicks();
	if (screen_off) {
		screen_off = false;
		osd_screen(true);
		dirty = true;
	}
}

/* Mensagem de tela cheia. Também vai para o fb0, para continuar na tela
 * depois que o launcher fechar (abrindo um jogo, desligando). */
static void message_screen(const char *title, const char *hint)
{
	for (int i = 0; i < 2; i++) { /* dois quadros: ver draw() */
		ui_clear();
		ui_text(F_BIG, title, UI_W / 2, UI_H / 2 - (hint ? 18 : 0), COL_TEXT, 2,
			UI_W - 2 * PAD_X);
		if (hint)
			ui_text(F_BODY, hint, UI_W / 2, UI_H / 2 + 24, COL_DIM, 2, UI_W - 2 * PAD_X);
		if (i == 1)
			fb_copy_from(ui_renderer());
		ui_present();
	}
}

static void shutdown_now(void)
{
	state_save();
	message_screen(S_SHUTDOWN, NULL);
	sync();
	if (system("/sbin/poweroff") < 0)
		perror("poweroff");
	for (;;)
		pause(); /* o init encerra o launcher */
}

static void suspend(void)
{
	/* pingo-suspend devolve quando o aparelho acorda; 10: ficou suspenso
	 * tempo demais ou a bateria ficou fraca, então desliga */
	int rc = system("/usr/bin/pingo-suspend");
	if (rc != -1 && WIFEXITED(rc) && WEXITSTATUS(rc) == 10)
		shutdown_now();
	drain(fds[DEV_PAD].fd);
	drain(fds[DEV_POWER].fd);
	drain(fds[DEV_VOLUME].fd);
	held = -1;
	screen_wake();
	status.clock[0] = 0; /* força atualizar */
	status_update();
	dirty = true;
}

/* ---- retomar o último jogo ------------------------------------------- */

#define RESUME_FILE "/mnt/sd1/.system/resume"

static bool resume_last_game(void)
{
	char core[4096], rom[4096], code[64];
	struct stat st;
	FILE *f = fopen(RESUME_FILE, "r");
	if (!f)
		return false;
	bool ok = fgets(core, sizeof(core), f) && fgets(rom, sizeof(rom), f) &&
		  fgets(code, sizeof(code), f);
	fclose(f);
	if (!ok)
		return false;
	core[strcspn(core, "\n")] = rom[strcspn(rom, "\n")] = code[strcspn(code, "\n")] = 0;
	if (stat(core, &st) < 0 || stat(rom, &st) < 0) {
		unlink(RESUME_FILE);
		return false;
	}
	const char *slash = strrchr(rom, '/');
	char *label = library_clean_name(slash ? slash + 1 : rom, false);
	message_screen(S_LOADING, label);
	free(label);

	FILE *n = fopen(NEXT_FILE ".tmp", "w");
	if (!n)
		return false;
	fprintf(n, "%s\n%s\n%s\nresume\n", core, rom, code);
	fclose(n);
	rename(NEXT_FILE ".tmp", NEXT_FILE);
	fprintf(stderr, "pingo-launcher: retomando %s\n", rom);
	return true;
}

/* ---- assistente do primeiro boot ------------------------------------- *
 * O boot (pingo-storage) deixa em /run/pingo/storage "setup <GB>" quando o
 * cartão é novo. Nada é formatado sem o usuário apertar A.
 */

#define STORAGE_STATE RUN_DIR "/storage"

static void read_storage_state(char *kind, size_t kn, char *arg, size_t an)
{
	FILE *f = fopen(STORAGE_STATE, "r");
	char line[128] = "";
	kind[0] = arg[0] = 0;
	if (!f)
		return;
	if (fgets(line, sizeof(line), f)) {
		char *nl = strchr(line, '\n');
		if (nl)
			*nl = 0;
		char *sp = strchr(line, ' ');
		if (sp) {
			*sp = 0;
			snprintf(arg, an, "%s", sp + 1);
		}
		snprintf(kind, kn, "%s", line);
	}
	fclose(f);
}

static void setup_screen(const char *title, const char *text, const char *hint_a, const char *hint_b)
{
	for (int i = 0; i < 2; i++) { /* dois quadros: ver draw() */
		ui_clear();
		int y = 145;
		ui_text(F_BIG, title, UI_W / 2, y, COL_TEXT, 2, UI_W - 2 * PAD_X);
		if (text)
			ui_text_wrap(F_BODY, text, UI_W / 2, y + 36, 520, 31, COL_DIM);
		int right = UI_W - PAD_X;
		if (hint_b)
			right -= draw_hint(right, "B", hint_b) + 22;
		if (hint_a)
			draw_hint(right, "A", hint_a);
		ui_present();
	}
}

/* Espera A ou B (power: toque suspende, segurar desliga) */
static int wait_ab(void)
{
	struct input_event ev;
	for (;;) {
		poll(fds, DEV_N, power_down ? 50 : 1000);
		while (fds[DEV_PAD].fd >= 0 && read(fds[DEV_PAD].fd, &ev, sizeof(ev)) == sizeof(ev))
			if (ev.type == EV_KEY && ev.value == 1 &&
			    (ev.code == BTN_EAST || ev.code == BTN_SOUTH))
				return ev.code;
		while (fds[DEV_POWER].fd >= 0 &&
		       read(fds[DEV_POWER].fd, &ev, sizeof(ev)) == sizeof(ev)) {
			if (ev.type != EV_KEY || ev.code != KEY_POWER)
				continue;
			if (ev.value == 1) {
				power_down = true;
				power_down_at = SDL_GetTicks();
			} else if (ev.value == 0 && power_down) {
				power_down = false;
				suspend();
				return -1; /* redesenha a tela */
			}
		}
		if (power_down && SDL_GetTicks() - power_down_at >= POWER_HOLD_MS)
			shutdown_now();
	}
}

/* true: a ROMS ficou pronta */
static bool setup_wizard(const char *gb)
{
	char text[256];
	snprintf(text, sizeof(text), S_SETUP_TEXT, gb[0] ? gb : "?");

	for (;;) {
		setup_screen(S_WELCOME, text, S_INSTALL, S_POWER_OFF);
		int b = wait_ab();
		if (b == BTN_SOUTH)
			shutdown_now();
		if (b == BTN_EAST)
			break;
	}

	message_screen(S_PREPARING, S_DONT_TURN_OFF);
	int rc = system("/usr/sbin/pingo-storage format");
	if (rc != 0) {
		fprintf(stderr, "pingo-launcher: pingo-storage format falhou (%d)\n", rc);
		for (;;) {
			setup_screen(S_SETUP_FAIL, S_SETUP_FAIL_TXT, NULL, S_POWER_OFF);
			if (wait_ab() == BTN_SOUTH)
				shutdown_now();
		}
	}

	do
		setup_screen(S_SETUP_DONE, S_SETUP_DONE_TXT, S_CONTINUE, NULL);
	while (wait_ab() != BTN_EAST);
	return true;
}

/* ---- SD2 novo ---------------------------------------------------------- */

static void sd2_prepare_now(void)
{
	message_screen(S_SD2_PREPARING, NULL);
	int rc = system("/usr/sbin/pingo-storage sd2-prepare");
	sd2_read();
	if (rc != 0 || !sd2_ready) {
		show_toast(S_SD2_FAIL);
		return;
	}
	do
		setup_screen(S_SD2_READY, S_SD2_READY_TXT, S_CONTINUE, NULL);
	while (wait_ab() != BTN_EAST);
	start_scan();
}

/* Cartão sem Roms/ no slot 2: pergunta uma vez (por cartão) */
static void sd2_maybe_ask(void)
{
	if (!sd2_present || sd2_ready || !sd2_serial[0] || sd2_ignored())
		return;
	for (;;) {
		setup_screen(S_SD2_NEW, S_SD2_NEW_TXT, S_PREPARE, S_NOT_NOW);
		int b = wait_ab();
		if (b == BTN_EAST) {
			sd2_prepare_now();
			break;
		}
		if (b == BTN_SOUTH) {
			sd2_ignore();
			break;
		}
	}
	dirty = true;
}

/* MENU no Início */
/* ---- Wi-Fi ------------------------------------------------------------ *
 * A tela conversa com o /usr/sbin/pingo-wifi (wpa_supplicant por baixo).
 * A senha vai pela entrada padrão do script, nunca pela linha de comando, e
 * só a chave derivada dela é gravada.
 */
static const char *wifi_cmd(void)
{
	const char *e = getenv("PINGO_WIFI"); /* para testar sem regravar o sistema */
	return e && *e ? e : "/usr/sbin/pingo-wifi";
}

/* Roda o pingo-wifi; devolve o código de saída (-1: não rodou) */
static int wifi_run(const char *a1, const char *a2, const char *input, char *out, size_t outn)
{
	int in_p[2], out_p[2];
	if (pipe(in_p) < 0)
		return -1;
	if (pipe(out_p) < 0) {
		close(in_p[0]);
		close(in_p[1]);
		return -1;
	}
	pid_t pid = fork();
	if (pid == 0) {
		dup2(in_p[0], 0);
		if (out) {
			dup2(out_p[1], 1);
		} else { /* sem resposta: a saída vai para /dev/null */
			int dn = open("/dev/null", O_WRONLY);
			if (dn >= 0)
				dup2(dn, 1);
		}
		/* Nada além de 0, 1 e 2: o wpa_supplicant e o udhcpc ficam rodando
		 * depois do script e, herdando a ponta do cano, o launcher nunca
		 * veria o fim da resposta (e herdariam também as fontes, a tela...) */
		for (int fd = 3; fd < 1024; fd++)
			close(fd);
		const char *cmd = wifi_cmd();
		execl(cmd, cmd, a1, a2, (char *)NULL);
		_exit(127);
	}
	close(in_p[0]);
	close(out_p[1]);
	if (pid < 0) {
		close(in_p[1]);
		close(out_p[0]);
		return -1;
	}
	if (input) {
		ssize_t w = write(in_p[1], input, strlen(input));
		w += write(in_p[1], "\n", 1);
		(void)w;
	}
	close(in_p[1]);
	size_t len = 0;
	ssize_t r;
	char tmp[512];
	while ((r = read(out_p[0], tmp, sizeof(tmp))) > 0)
		if (out && len + 1 < outn) {
			size_t n = SDL_min((size_t)r, outn - 1 - len);
			memcpy(out + len, tmp, n);
			len += n;
		}
	if (out && outn)
		out[len] = 0;
	close(out_p[0]);
	int st;
	if (waitpid(pid, &st, 0) < 0)
		return -1;
	return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

#define WIFI_MAX_NETS 40
static struct wifi_net {
	char ssid[64];
	int signal; /* dBm */
	bool open, wep, saved;
} wifi_nets[WIFI_MAX_NETS];
static int wifi_nnets;
static bool wifi_on;
static char wifi_ssid[64], wifi_ip[32];

static void wifi_status_read(void)
{
	char out[512];
	wifi_on = false;
	wifi_ssid[0] = wifi_ip[0] = 0;
	if (wifi_run("status", NULL, NULL, out, sizeof(out)) < 0)
		return;
	for (char *l = strtok(out, "\n"); l; l = strtok(NULL, "\n")) {
		if (!strcmp(l, "estado ligado"))
			wifi_on = true;
		else if (!strncmp(l, "rede ", 5))
			snprintf(wifi_ssid, sizeof(wifi_ssid), "%s", l + 5);
		else if (!strncmp(l, "ip ", 3))
			snprintf(wifi_ip, sizeof(wifi_ip), "%s", l + 3);
	}
}

static void wifi_scan(void)
{
	static char out[8192];
	wifi_nnets = 0;
	if (!wifi_on || wifi_run("scan", NULL, NULL, out, sizeof(out)) != 0)
		return;
	char *save = NULL;
	for (char *l = strtok_r(out, "\n", &save); l && wifi_nnets < WIFI_MAX_NETS;
	     l = strtok_r(NULL, "\n", &save)) {
		/* sinal<tab>tipo<tab>salva<tab>nome */
		char *f[4] = { l, NULL, NULL, NULL };
		for (int i = 1; i < 4; i++) {
			char *t = strchr(f[i - 1], '\t');
			if (!t)
				break;
			*t = 0;
			f[i] = t + 1;
		}
		if (!f[3] || !*f[3])
			continue;
		struct wifi_net *n = &wifi_nets[wifi_nnets++];
		snprintf(n->ssid, sizeof(n->ssid), "%s", f[3]);
		n->signal = atoi(f[0]);
		n->open = !strcmp(f[1], "aberta");
		n->wep = !strcmp(f[1], "wep");
		n->saved = !strcmp(f[2], "1");
	}
}

/* Sinal em 4 barrinhas, como no ícone da barra */
static void draw_signal(int right, int y_center, int dbm, Uint32 rgb)
{
	int bars = dbm >= -55 ? 4 : dbm >= -65 ? 3 : dbm >= -75 ? 2 : 1;
	for (int i = 0; i < 4; i++) {
		int h = 5 + i * 4, x = right - (4 - i) * 6;
		Uint32 c = i < bars ? rgb : 0x4A4A4A;
		ui_rect(x, y_center + 8 - h, 4, h, c);
	}
}

/* ---- teclado na tela ---- */
static const char *const kb_pages[3][4] = {
	{ "1234567890", "qwertyuiop", "asdfghjkl-", "zxcvbnm_.@" },
	{ "1234567890", "QWERTYUIOP", "ASDFGHJKL-", "ZXCVBNM_.@" },
	{ "!@#$%^&*()", "-_=+[]{}\\|", ";:'\",.<>/?", "`~" },
};
enum { KB_SHIFT, KB_SYMS, KB_SPACE, KB_DEL, KB_OK, KB_NSPECIAL };
static const int kb_special_w[KB_NSPECIAL] = { 84, 84, 228, 84, 96 };

#define KB_X     PAD_X
#define KB_KW    54
#define KB_KH    42
#define KB_GAP   6
#define KB_Y     160

static int kb_row_len(int page, int r)
{
	return r < 4 ? (int)strlen(kb_pages[page][r]) : KB_NSPECIAL;
}

static void kb_draw(const char *title, const char *text, int page, int r, int c, const char *err)
{
	ui_clear();
	ui_text(F_BAR_BOLD, title, PAD_X, PAD_Y + BAR_H / 2, COL_TEXT, 0, UI_W - 2 * PAD_X);
	/* campo */
	int fy = PAD_Y + BAR_H + LIST_GAP;
	ui_round_rect(PAD_X, fy, UI_W - 2 * PAD_X, 52, 12, 0x2A2A2A);
	char shown[160];
	size_t n = strlen(text);
	/* mostra o fim do texto quando não cabe */
	const char *start = text;
	while (ui_text_width(F_ITEM, start) > UI_W - 2 * PAD_X - 40 && *start)
		start++;
	snprintf(shown, sizeof(shown), "%s|", start);
	ui_text(F_ITEM, n ? shown : "|", PAD_X + ROW_PAD, fy + 26, COL_TEXT, 0, 0);
	if (err)
		ui_text(F_BAR, err, PAD_X, fy + 52 + 22, COL_DIM, 0, UI_W - 2 * PAD_X);
	/* teclas */
	for (int rr = 0; rr < 5; rr++) {
		int y = KB_Y + rr * (KB_KH + KB_GAP), x = KB_X;
		for (int cc = 0; cc < kb_row_len(page, rr); cc++) {
			int w = rr < 4 ? KB_KW : kb_special_w[cc];
			bool sel = rr == r && cc == c;
			Uint32 bg = sel ? COL_SEL_BG : 0x2A2A2A, fg = sel ? COL_SEL_FG : COL_TEXT;
			ui_round_rect(x, y, w, KB_KH, 10, bg);
			char label[16];
			if (rr < 4) {
				label[0] = kb_pages[page][rr][cc];
				label[1] = 0;
				ui_text(F_ITEM, label, x + w / 2, y + KB_KH / 2, fg, 2, 0);
			} else {
				const char *names[] = { page == 1 ? S_KB_LOWER : S_KB_UPPER,
							page == 2 ? S_KB_LETTERS : S_KB_SYMBOLS, S_KB_SPACE,
							S_KB_DELETE, S_KB_OK };
				ui_text(F_BAR_BOLD, names[cc], x + w / 2, y + KB_KH / 2, fg, 2, w - 8);
			}
			x += w + KB_GAP;
		}
	}
	int right = UI_W - PAD_X;
	right -= draw_hint(right, "START", S_KB_OK) + 22;
	right -= draw_hint(right, "B", S_KB_DELETE) + 22;
	right -= draw_hint(right, "Y", S_KB_UPPER) + 22;
	draw_hint(right, "MENU", S_CANCEL);
	osd_draw(ui_renderer(), UI_W, UI_H);
	ui_present();
}

/* coluna equivalente ao trocar entre a fileira das teclas especiais e as outras */
static int kb_map_col(int page, int from_r, int c, int to_r)
{
	int x = KB_X;
	if (from_r == 4) {
		for (int i = 0; i < c; i++)
			x += kb_special_w[i] + KB_GAP;
		x += kb_special_w[c] / 2;
	} else {
		x += c * (KB_KW + KB_GAP) + KB_KW / 2;
	}
	int best = 0, bx = KB_X;
	for (int i = 0; i < kb_row_len(page, to_r); i++) {
		int w = to_r < 4 ? KB_KW : kb_special_w[i];
		if (x >= bx)
			best = i;
		bx += w + KB_GAP;
	}
	return best;
}

/* Teclado: devolve true com o texto em buf (OK), false se cancelou */
static bool keyboard(const char *title, char *buf, size_t max, int min_len, const char *min_msg)
{
	int page = 0, r = 1, c = 0;
	const char *err = NULL;
	for (;;) {
		kb_draw(title, buf, page, r, c, err);
		int k = wait_key();
		size_t n = strlen(buf);
		bool type = false, ok = false;
		switch (k) {
		case BTN_DPAD_UP:
			if (r > 0) {
				int nr = r - 1;
				c = (r == 4) ? kb_map_col(page, r, c, nr) : SDL_min(c, kb_row_len(page, nr) - 1);
				r = nr;
			}
			break;
		case BTN_DPAD_DOWN:
			if (r < 4) {
				int nr = r + 1;
				c = (nr == 4) ? kb_map_col(page, r, c, nr) : SDL_min(c, kb_row_len(page, nr) - 1);
				r = nr;
			}
			break;
		case BTN_DPAD_LEFT:
			c = (c - 1 + kb_row_len(page, r)) % kb_row_len(page, r);
			break;
		case BTN_DPAD_RIGHT:
			c = (c + 1) % kb_row_len(page, r);
			break;
		case BTN_EAST: /* A: a tecla selecionada */
			type = true;
			break;
		case BTN_SOUTH: /* B: apagar */
			if (n)
				buf[n - 1] = 0;
			break;
		case BTN_NORTH: /* Y: maiúsculas */
			page = page == 1 ? 0 : 1;
			break;
		case BTN_WEST: /* X: espaço */
			if (n + 1 < max) {
				buf[n] = ' ';
				buf[n + 1] = 0;
			}
			break;
		case BTN_SELECT:
			page = page == 2 ? 0 : 2;
			break;
		case BTN_START:
			ok = true;
			break;
		case BTN_MODE:
			return false;
		}
		if (type && r < 4) {
			if (n + 1 < max) {
				buf[n] = kb_pages[page][r][c];
				buf[n + 1] = 0;
			}
		} else if (type) {
			switch (c) {
			case KB_SHIFT: page = page == 1 ? 0 : 1; break;
			case KB_SYMS: page = page == 2 ? 0 : 2; break;
			case KB_SPACE:
				if (n + 1 < max) {
					buf[n] = ' ';
					buf[n + 1] = 0;
				}
				break;
			case KB_DEL:
				if (n)
					buf[n - 1] = 0;
				break;
			case KB_OK: ok = true; break;
			}
		}
		/* trocar de página pode encurtar a fileira */
		if (c >= kb_row_len(page, r))
			c = kb_row_len(page, r) - 1;
		err = NULL;
		if (ok) {
			if ((int)strlen(buf) >= min_len)
				return true;
			err = min_msg;
		}
	}
}

static void wifi_busy(const char *title, const char *hint)
{
	message_screen(title, hint);
}

static void wifi_refresh(bool rescan)
{
	wifi_busy(rescan ? S_WIFI_SCANNING : S_WIFI_UPDATING, NULL);
	wifi_status_read();
	if (rescan)
		wifi_scan();
	status_update();
}

static void wifi_connect(const struct wifi_net *n)
{
	char pass[72] = "", title[128], hint[128];
	if (!strcmp(n->ssid, wifi_ssid) && wifi_ip[0]) {
		show_toast(S_WIFI_ALREADY);
		return;
	}
	if (n->wep) {
		show_toast(S_WIFI_WEP);
		return;
	}
	if (!n->open && !n->saved) {
		snprintf(title, sizeof(title), S_WIFI_PASSWORD_FOR, n->ssid);
		if (!keyboard(title, pass, 64, 8, S_WIFI_PASS_SHORT))
			return;
	}
	snprintf(hint, sizeof(hint), "%s", n->ssid);
	wifi_busy(S_WIFI_CONNECTING, hint);
	int rc = wifi_run("connect", n->ssid, pass, NULL, 0);
	memset(pass, 0, sizeof(pass));
	wifi_status_read();
	for (int i = 0; i < wifi_nnets; i++) /* atualiza "salva" sem procurar de novo */
		if (!strcmp(wifi_nets[i].ssid, n->ssid))
			wifi_nets[i].saved |= rc == 0; /* só pede senha de rede não salva */
	status_update();
	char msg[160];
	if (rc == 0)
		snprintf(msg, sizeof(msg), S_WIFI_CONNECTED_TO, n->ssid);
	else
		snprintf(msg, sizeof(msg), "%s", rc == 3 ? S_WIFI_PASS_BAD : S_WIFI_FAILED);
	show_toast(msg);
}

static void wifi_screen(void)
{
	int sel = 0, top = 0;
	wifi_refresh(true);
	for (;;) {
		int n = 1 + wifi_nnets; /* 0: liga/desliga; depois as redes */
		if (sel >= n)
			sel = n - 1;
		const int net_rows = 6;
		if (sel > 0 && sel - 1 < top)
			top = sel - 1;
		if (sel > 0 && sel - 1 >= top + net_rows)
			top = sel - net_rows;

		ui_clear();
		ui_text(F_BAR_BOLD, S_WIFI, PAD_X, PAD_Y + BAR_H / 2, COL_TEXT, 0, 0);
		draw_status_right();
		int y = PAD_Y + BAR_H + LIST_GAP;
		bool s0 = sel == 0;
		if (s0)
			ui_round_rect(PAD_X, y, UI_W - 2 * PAD_X, ROW_H, ROW_RADIUS, COL_SEL_BG);
		ui_text(s0 ? F_ITEM_SEL : F_ITEM, S_WIFI, PAD_X + ROW_PAD, y + ROW_H / 2,
			s0 ? COL_SEL_FG : COL_TEXT, 0, 0);
		ui_text(F_COUNT, wifi_on ? S_O_ON : S_O_OFF, UI_W - PAD_X - ROW_PAD, y + ROW_H / 2,
			s0 ? COL_SEL_FG : COL_DIM, 1, 0);
		y += ROW_H + 12;
		char info[160];
		if (!wifi_on)
			snprintf(info, sizeof(info), "%s", S_WIFI_OFF_INFO);
		else if (wifi_ssid[0])
			snprintf(info, sizeof(info), S_WIFI_CONNECTED_INFO, wifi_ssid,
				 wifi_ip[0] ? wifi_ip : "…");
		else
			snprintf(info, sizeof(info), "%s", wifi_nnets ? S_WIFI_NOT_CONNECTED : S_WIFI_NONE);
		ui_text(F_BAR, info, PAD_X + ROW_PAD, y + 8, COL_DIM, 0, UI_W - 2 * PAD_X - 2 * ROW_PAD);
		y += 30;
		for (int r = 0; r < net_rows && top + r < wifi_nnets; r++) {
			const struct wifi_net *w = &wifi_nets[top + r];
			bool s = sel == top + r + 1;
			int ry = y + r * (ROW_H + ROW_GAP);
			if (s)
				ui_round_rect(PAD_X, ry, UI_W - 2 * PAD_X, ROW_H, ROW_RADIUS, COL_SEL_BG);
			Uint32 fg = s ? COL_SEL_FG : COL_TEXT, dim = s ? COL_SEL_FG : COL_DIM;
			int right = UI_W - PAD_X - ROW_PAD;
			draw_signal(right, ry + ROW_H / 2, w->signal, dim);
			right -= 24 + 12;
			const char *tag = !strcmp(w->ssid, wifi_ssid) ? S_WIFI_TAG_CONNECTED
					: w->saved ? S_WIFI_TAG_SAVED : w->open ? S_WIFI_TAG_OPEN : NULL;
			if (tag)
				right -= ui_text(F_SMALL, tag, right, ry + ROW_H / 2, dim, 1, 0) + 12;
			ui_text(s ? F_ITEM_SEL : F_ITEM, w->ssid, PAD_X + ROW_PAD, ry + ROW_H / 2, fg, 0,
				right - PAD_X - 2 * ROW_PAD);
		}
		/* rodapé */
		int right = UI_W - PAD_X;
		if (toast[0] && SDL_GetTicks() < toast_until) {
			ui_text(F_BAR, toast, PAD_X, UI_H - PAD_Y - BAR_H / 2, COL_TEXT, 0, UI_W - 2 * PAD_X);
		} else {
			toast[0] = 0;
			right -= draw_hint(right, "B", S_BACK) + 22;
			right -= draw_hint(right, "A", sel == 0 ? S_HINT_CHANGE : S_WIFI_CONNECT) + 22;
			if (wifi_on)
				right -= draw_hint(right, "X", S_WIFI_SEARCH) + 22;
			if (sel > 0 && wifi_nets[sel - 1].saved)
				draw_hint(right, "Y", S_WIFI_FORGET);
		}
		osd_draw(ui_renderer(), UI_W, UI_H);
		ui_present();

		int k = wait_key();
		if (k == BTN_SOUTH || k == BTN_MODE)
			break;
		if (k == BTN_DPAD_UP)
			sel = (sel - 1 + n) % n;
		else if (k == BTN_DPAD_DOWN)
			sel = (sel + 1) % n;
		else if (k == BTN_WEST && wifi_on)
			wifi_refresh(true);
		else if (k == BTN_EAST || (sel == 0 && (k == BTN_DPAD_LEFT || k == BTN_DPAD_RIGHT))) {
			if (sel == 0) {
				wifi_busy(wifi_on ? S_WIFI_TURNING_OFF : S_WIFI_TURNING_ON, NULL);
				wifi_run(wifi_on ? "off" : "on", NULL, NULL, NULL, 0);
				if (!wifi_on)
					sleep(3); /* dá tempo de associar à rede salva */
				wifi_refresh(true);
			} else {
				wifi_connect(&wifi_nets[sel - 1]);
			}
		} else if (k == BTN_NORTH && sel > 0 && wifi_nets[sel - 1].saved) {
			struct wifi_net *w = &wifi_nets[sel - 1];
			wifi_run("forget", w->ssid, NULL, NULL, 0);
			w->saved = false;
			wifi_status_read();
			status_update();
			show_toast(S_WIFI_FORGOTTEN);
		}
	}
	dirty = true;
}

/* MENU no Início: Wi-Fi e, com um cartão novo no slot 2, prepará-lo */
static void home_menu(void)
{
	int sel = 0;
	for (;;) {
		bool sd2_item = sd2_present && !sd2_ready;
		int n = sd2_item ? 2 : 1;
		if (sel >= n)
			sel = n - 1;
		ui_clear();
		ui_text(F_BAR_BOLD, S_HOME_OPTIONS, PAD_X, PAD_Y + BAR_H / 2, COL_TEXT, 0, 0);
		for (int i = 0; i < n; i++) {
			int y = PAD_Y + BAR_H + LIST_GAP + i * (ROW_H + ROW_GAP);
			bool s = i == sel;
			if (s)
				ui_round_rect(PAD_X, y, UI_W - 2 * PAD_X, ROW_H, ROW_RADIUS, COL_SEL_BG);
			ui_text(s ? F_ITEM_SEL : F_ITEM, i == 0 ? S_WIFI : S_PREPARE_SD2, PAD_X + ROW_PAD,
				y + ROW_H / 2, s ? COL_SEL_FG : COL_TEXT, 0, 0);
			if (i == 0 && s)
				ui_icon_chevron(UI_W - PAD_X - ROW_PAD - 16, y + ROW_H / 2, COL_SEL_FG);
		}
		int right = UI_W - PAD_X;
		right -= draw_hint(right, "B", S_BACK) + 22;
		draw_hint(right, "A", S_CONFIRM);
		osd_draw(ui_renderer(), UI_W, UI_H);
		ui_present();
		int k = wait_key();
		if (k == BTN_DPAD_UP)
			sel = (sel - 1 + n) % n;
		else if (k == BTN_DPAD_DOWN)
			sel = (sel + 1) % n;
		else if (k == BTN_EAST) {
			if (sel == 0) {
				wifi_screen();
			} else {
				sd2_prepare_now();
				break;
			}
		} else if (k == BTN_SOUTH || k == BTN_MODE)
			break;
	}
	dirty = true;
}

/* ---- principal -------------------------------------------------------- */

int main(int argc, char **argv)
{
	th = th_dark;
	struct stat st;

	/* Mensagem de tela cheia até receber SIGTERM (usado no boot) */
	if (argc >= 3 && !strcmp(argv[1], "--message")) {
		if (!ui_init())
			return 1;
		message_screen(argv[2], argc >= 4 ? argv[3] : NULL);
		ui_quit();
		return 0;
	}

	if (!ui_init())
		return 1;
	srand((unsigned)time(NULL) ^ (unsigned)getpid());
	fds[DEV_PAD].fd = open_by_name("H700 Gamepad");
	fds[DEV_POWER].fd = open_by_name("axp20x-pek");
	fds[DEV_VOLUME].fd = open_by_name("gpio-keys-volume");
	for (int i = 0; i < DEV_N; i++)
		fds[i].events = POLLIN;

	char skind[32], sarg[64];
	read_storage_state(skind, sizeof(skind), sarg, sizeof(sarg));
	if (!strcmp(skind, "setup"))
		setup_wizard(sarg);
	else if (!strcmp(skind, "unknown"))
		show_toast(S_UNKNOWN_PART);

	osd_init(); /* depois da ROMS: os valores ficam em Config/ */

	/* Primeira abertura depois do boot: retoma o jogo que estava aberto
	 * (desligou pelo power, bateria acabou suspenso...) */
	if (access(RUN_DIR "/booted", F_OK) != 0) {
		mkdir(RUN_DIR, 0755);
		FILE *b = fopen(RUN_DIR "/booted", "w");
		if (b)
			fclose(b);
		if (resume_last_game())
			return 0;
	}
	have_roms = stat(ROMS_DIR, &st) == 0;
	origin_load();
	sd2_read();

	lib = have_roms ? library_load_cache(CACHE_FILE) : NULL;
	bool from_cache = lib != NULL;
	if (!lib)
		lib = have_roms ? library_scan(origin, sd2_present) : NULL;
	if (lib && !from_cache)
		library_save_cache(lib, CACHE_FILE);
	recents_load(); /* depois da biblioteca: precisa das pastas dos consoles */
	favorites_load();
	home_build();
	stack[0] = (struct level){ &home, 0, 0 };
	state_load();


	/* a lista veio do cache: confere o cartão em segundo plano */
	if (from_cache)
		start_scan();
	time_t sd2_mtime = stat(SD2_CHANGED, &st) == 0 ? st.st_mtime : 0;

	status_update();
	Uint32 last_status = SDL_GetTicks();
	bool quit = false;

	/* O KMS/DRM do SDL nem sempre mostra o primeiro quadro (a tela ficava
	 * preta ao voltar de um jogo até o próximo botão): desenha dois. */
	draw();
	dirty = true;

	sd2_maybe_ask();
	last_input = SDL_GetTicks();
	while (!quit) {
		if (dirty && !screen_off)
			draw();

		int timeout = 1000;
		if (held >= 0) {
			int left = (int)(held_next - SDL_GetTicks());
			timeout = left < 0 ? 0 : left;
		}
		if (toast[0])
			timeout = timeout > 200 ? 200 : timeout;
		if (osd_visible())
			timeout = timeout > 100 ? 100 : timeout;
		if (power_down)
			timeout = timeout > 50 ? 50 : timeout;
		if (scanning)
			timeout = timeout > 100 ? 100 : timeout;

		poll(fds, DEV_N, timeout);

		struct input_event ev;
		while (!quit && fds[DEV_PAD].fd >= 0 &&
		       read(fds[DEV_PAD].fd, &ev, sizeof(ev)) == sizeof(ev)) {
			if (ev.type != EV_KEY)
				continue;
			if (screen_off) {
				/* o primeiro toque só acende a tela */
				if (ev.value == 1)
					screen_wake();
				if (ev.code == BTN_MODE)
					osd_key(ev.code, ev.value); /* mantém o estado do MENU */
				osd_menu_tapped();
				continue;
			}
			last_input = SDL_GetTicks();
			if (osd_key(ev.code, ev.value))
				continue;
			if (ev.code == BTN_MODE) {
				/* MENU age ao soltar (MENU+Vol± é brilho). Dentro de um console:
				 * origem SD1/SD2. No Início ainda não há opções. */
				if (osd_menu_tapped()) {
					held = -1;
					if (depth == 0)
						home_menu();
					else if (cur()->dir->console >= 0 && cur()->dir != recents)
						console_menu(cur()->dir->console);
				}
				continue;
			}
			if (ev.value == 1) {
				quit = press(ev.code);
				if (repeatable(ev.code)) {
					held = ev.code;
					held_next = SDL_GetTicks() + 300;
				}
			} else if (ev.value == 0 && ev.code == held) {
				held = -1;
			}
		}
		while (!quit && fds[DEV_POWER].fd >= 0 &&
		       read(fds[DEV_POWER].fd, &ev, sizeof(ev)) == sizeof(ev)) {
			if (ev.type != EV_KEY || ev.code != KEY_POWER)
				continue;
			if (ev.value == 1) {
				power_down = true;
				power_down_at = SDL_GetTicks();
				last_input = power_down_at;
				fprintf(stderr, "pingo-launcher: power apertado\n");
			} else if (ev.value == 0 && power_down) {
				fprintf(stderr, "pingo-launcher: power solto após %u ms: suspender\n",
					SDL_GetTicks() - power_down_at);
				power_down = false;
				suspend(); /* toque curto: suspende ao soltar */
			}
		}
		if (power_down && SDL_GetTicks() - power_down_at >= POWER_HOLD_MS) {
			fprintf(stderr, "pingo-launcher: power segurado %d ms: desligar\n", POWER_HOLD_MS);
			shutdown_now();
		}

		while (!quit && fds[DEV_VOLUME].fd >= 0 &&
		       read(fds[DEV_VOLUME].fd, &ev, sizeof(ev)) == sizeof(ev)) {
			if (ev.type != EV_KEY)
				continue;
			if (screen_off) {
				if (ev.value == 1)
					screen_wake();
				continue;
			}
			last_input = SDL_GetTicks();
			osd_key(ev.code, ev.value);
		}

		if (!screen_off && SDL_GetTicks() - last_input >= SCREEN_OFF_MS) {
			screen_off = true;
			held = -1;
			osd_screen(false);
		}
		if (osd_changed())
			dirty = true;

		if (!quit && held >= 0 && SDL_GetTicks() >= held_next) {
			press(held);
			held_next = SDL_GetTicks() + 60;
		}

		if (SDL_AtomicGet(&scan_done) == 1) {
			SDL_AtomicSet(&scan_done, 2);
			scanning = false;
			if (!library_equal(scan_result, lib)) {
				library_save_cache(scan_result, CACHE_FILE);
				swap_library(scan_result);
			} else {
				library_free(scan_result);
				if (pending_console >= 0) {
					pending_console = -1;
					dirty = true;
				}
			}
			scan_result = NULL;
			if (scan_again) {
				scan_again = false;
				start_scan();
			}
		}

		if (toast[0] && SDL_GetTicks() >= toast_until)
			dirty = true;

		if (SDL_GetTicks() - last_status >= 1000) {
			status_update();
			last_status = SDL_GetTicks();
			/* SD2 inserido ou tirado: refaz a lista */
			if (stat(SD2_CHANGED, &st) == 0 && st.st_mtime != sd2_mtime) {
				sd2_mtime = st.st_mtime;
				sd2_read();
				fprintf(stderr, "pingo-launcher: SD2 %s\n", sd2_present ? "presente" : "ausente");
				start_scan();
				sd2_maybe_ask();
			}
		}
	}

	state_save();
	ui_quit();
	return 0;
}
