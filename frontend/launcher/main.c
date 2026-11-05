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

/* caminho relativo à pasta do console ("MD/1 US/Jogo.md" -> "1 US/Jogo.md");
 * fora dela (.m3u gerado no cache), o caminho completo */
static const char *rel_path(const struct node *game)
{
	char prefix[1100];
	const char *root = library_console_root(lib, game->console);
	if (!root)
		return game->path;
	snprintf(prefix, sizeof(prefix), "%s/", root);
	if (strncmp(game->path, prefix, strlen(prefix)))
		return game->path;
	return game->path + strlen(prefix);
}

static void recents_load(void)
{
	FILE *f = fopen(RECENTS_FILE, "r");
	char line[4096];

	library_free(recents);
	recents = calloc(1, sizeof(*recents));
	recents->fname = strdup(RECENTS_PATH);
	recents->path = strdup(RECENTS_PATH);
	recents->label = strdup(S_RECENTS);
	recents->dir = true;
	recents->console = -1;
	if (!f)
		return;
	while (fgets(line, sizeof(line), f) && recents->nkids < RECENTS_MAX) {
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
		const char *root = library_console_root(lib, c);
		if (tab[1] == '/')
			snprintf(path, sizeof(path), "%s", tab + 1);
		else if (root)
			snprintf(path, sizeof(path), "%s/%s", root, tab + 1);
		else
			continue; /* console sem jogos agora */
		if (stat(path, &st) < 0) /* jogo apagado ou renomeado */
			continue;
		struct node *g = library_game_node(c, path);
		recents->kids = realloc(recents->kids, sizeof(*recents->kids) * (recents->nkids + 1));
		recents->kids[recents->nkids++] = g;
	}
	recents->games = recents->nkids;
	fclose(f);
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

/* ---- Início ---------------------------------------------------------- */

static void home_build(void)
{
	free(home.kids);
	memset(&home, 0, sizeof(home));
	home.dir = true;
	home.console = -1;
	home.path = (char *)"pingo:inicio";
	home.label = (char *)"";
	int n = (lib ? lib->nkids : 0) + 2;
	home.kids = calloc(n, sizeof(*home.kids));
	if (recents && recents->nkids > 0)
		home.kids[home.nkids++] = recents;
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
	home_build();
	restore_paths(paths, n);
	if (pending_console >= 0) { /* acabou de trocar a origem: abre o console de novo */
		depth = 0;
		for (int i = 0; i < home.nkids; i++)
			if (home.kids[i]->console == pending_console && home.kids[i] != recents) {
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
		x -= ui_text(F_BAR, pct, x, y, COL_DIM, 1, 0);
		x -= 10 + 26;
		ui_icon_battery(x, y, status.battery, status.charging, COL_DIM);
	}
	if (status.wifi) {
		x -= 10 + 18;
		ui_icon_wifi(x, y, COL_DIM);
	}
}

/* Dica do rodapé ("A abrir"), da direita para a esquerda */
static int draw_hint(int right, const char *button, const char *text)
{
	int y = UI_H - PAD_Y - BAR_H / 2;
	int w = ui_text(F_BAR, text, right, y, COL_DIM, 1, 0);
	int sp = ui_text_width(F_BAR, " ");
	ui_text(F_BAR_BOLD, button, right - w - sp, y, COL_TEXT, 1, 0);
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

	ui_clear();

	/* barra superior */
	if (at_home) {
		ui_text(F_BAR, status.clock, PAD_X, bar_y, COL_DIM, 0, 0);
	} else {
		char cnt[32];
		count_text(l->dir, cnt, sizeof(cnt));
		int cw = ui_text_width(F_BAR, cnt);
		int w = ui_text(F_BAR_BOLD, l->dir->label, PAD_X, bar_y, COL_TEXT, 0, 380 - cw);
		ui_text(F_BAR, cnt, PAD_X + w + 10, bar_y, COL_DIM, 0, 0);
	}
	draw_status_right();

	/* lista */
	int n = l->dir ? l->dir->nkids : 0;
	if (n == 0) {
		ui_text(F_TITLE, have_roms ? S_EMPTY_TITLE : S_NO_CARD, UI_W / 2, UI_H / 2 - 16,
			COL_TEXT, 2, UI_W - 2 * PAD_X);
		if (have_roms)
			ui_text(F_BAR, S_EMPTY_HINT, UI_W / 2, UI_H / 2 + 18, COL_DIM, 2,
				UI_W - 2 * PAD_X);
	}
	for (int row = 0; row < ROWS && l->top + row < n; row++) {
		int i = l->top + row;
		struct node *it = l->dir->kids[i];
		bool sel = i == l->sel;
		int y = list_y(row), x = PAD_X, w = UI_W - 2 * PAD_X;
		Uint32 fg = sel ? COL_SEL_FG : COL_TEXT;

		if (sel)
			ui_round_rect(x, y, w, ROW_H, ROW_RADIUS, COL_SEL_BG);

		int right = 0;
		if (!at_home && it->discs > 1) { /* "3 discos", como no mockup */
			char d[32];
			snprintf(d, sizeof(d), S_DISCS_N, it->discs);
			right = ui_text(F_SMALL, d, x + w - ROW_PAD, y + ROW_H / 2,
					sel ? COL_SEL_FG : COL_DIM, 1, 0) + 12;
		}
		if (at_home) { /* contagem à direita */
			char cnt[16];
			if (it->missing)
				snprintf(cnt, sizeof(cnt), "—");
			else
				snprintf(cnt, sizeof(cnt), "%d", it->games);
			right = ui_text(F_COUNT, cnt, x + w - ROW_PAD, y + ROW_H / 2,
					sel ? COL_SEL_FG : COL_DIM, 1, 0) + 12;
		}
		ui_text(sel ? F_ITEM_SEL : F_ITEM, it->label, x + ROW_PAD, y + ROW_H / 2, fg, 0,
			w - 2 * ROW_PAD - right);
	}

	/* rodapé */
	int right = UI_W - PAD_X;
	if (toast[0] && SDL_GetTicks() < toast_until) {
		ui_text(F_BAR, toast, PAD_X, foot_y, COL_TEXT, 0, UI_W - 2 * PAD_X);
	} else {
		toast[0] = 0;
		if (!at_home && n > 0) {
			char pos[32];
			snprintf(pos, sizeof(pos), "%d / %d", l->sel + 1, n);
			ui_text(F_BAR, pos, PAD_X, foot_y, COL_DIM, 0, 0);
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
			if (l->dir != recents && l->dir->games > 1)
				draw_hint(right, "X", S_RANDOM_HINT);
		}
	}

	osd_draw(ui_renderer(), UI_W, UI_H);
	ui_present();
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
	show_toast(to == 2 ? S_READING_SD2 : S_READING_SD1);
	start_scan();
}

/* MENU dentro de um console: "Origem: SD1 / SD2" */
static void console_menu(int c)
{
	int cur_src = cur()->dir && cur()->dir->source ? cur()->dir->source : 1;
	int to = origin[c] ? origin[c] : cur_src;
	for (;;) {
		ui_clear();
		int bar_y = PAD_Y + BAR_H / 2;
		ui_text(F_BAR_BOLD, consoles[c].name, PAD_X, bar_y, COL_TEXT, 0, 420);
		int y = PAD_Y + BAR_H + LIST_GAP;
		ui_round_rect(PAD_X, y, UI_W - 2 * PAD_X, ROW_H, ROW_RADIUS, COL_SEL_BG);
		ui_text(F_ITEM_SEL, S_ORIGIN, PAD_X + ROW_PAD, y + ROW_H / 2, COL_SEL_FG, 0, 0);
		ui_text(F_COUNT, to == 2 ? "SD2" : "SD1", UI_W - PAD_X - ROW_PAD, y + ROW_H / 2,
			COL_SEL_FG, 1, 0);
		if (to == 2 && !sd2_present)
			ui_text(F_BAR, S_SD2_MISSING, PAD_X + ROW_PAD, y + ROW_H + 24, COL_DIM, 0,
				UI_W - 2 * PAD_X);
		int right = UI_W - PAD_X;
		right -= draw_hint(right, "B", S_BACK) + 22;
		draw_hint(right, "◀ ▶", S_HINT_CHANGE);
		osd_draw(ui_renderer(), UI_W, UI_H);
		ui_present();

		int k = wait_key();
		if (k == BTN_DPAD_LEFT || k == BTN_DPAD_RIGHT || k == BTN_EAST)
			to = to == 2 ? 1 : 2;
		else if (k == BTN_SOUTH || k == BTN_MODE)
			break;
	}
	if (to != cur_src || origin[c])
		change_origin(c, to); /* a escolha fica fixada */
	dirty = true;
}

/* Console aponta para o SD2 e o SD2 não está: avisa e oferece o SD1 */
static void sd2_missing_screen(int c)
{
	for (;;) {
		ui_clear();
		ui_text(F_BIG, S_SD2_MISSING, UI_W / 2, UI_H / 2 - 30, COL_TEXT, 2, UI_W - 2 * PAD_X);
		ui_text_wrap(F_BODY, S_SD2_MISSING_TXT, UI_W / 2, UI_H / 2 + 4, 520, 31, COL_DIM);
		int right = UI_W - PAD_X;
		right -= draw_hint(right, "B", S_BACK) + 22;
		draw_hint(right, "A", S_USE_SD1);
		ui_present();
		int k = wait_key();
		if (k == BTN_EAST) {
			change_origin(c, 1);
			break;
		}
		if (k == BTN_SOUTH || k == BTN_MODE)
			break;
	}
	dirty = true;
}

static bool open_selected(void)
{
	struct node *it = selected();
	if (!it)
		return false;
	if (it->missing) {
		sd2_missing_screen(it->console);
		return false;
	}
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
static void home_menu(void)
{
	if (!(sd2_present && !sd2_ready)) {
		show_toast(S_NO_OPTIONS);
		return;
	}
	for (;;) {
		ui_clear();
		ui_text(F_BAR_BOLD, S_HOME_OPTIONS, PAD_X, PAD_Y + BAR_H / 2, COL_TEXT, 0, 0);
		int y = PAD_Y + BAR_H + LIST_GAP;
		ui_round_rect(PAD_X, y, UI_W - 2 * PAD_X, ROW_H, ROW_RADIUS, COL_SEL_BG);
		ui_text(F_ITEM_SEL, S_PREPARE_SD2, PAD_X + ROW_PAD, y + ROW_H / 2, COL_SEL_FG, 0, 0);
		int right = UI_W - PAD_X;
		right -= draw_hint(right, "B", S_BACK) + 22;
		draw_hint(right, "A", S_CONFIRM);
		ui_present();
		int k = wait_key();
		if (k == BTN_EAST) {
			sd2_prepare_now();
			break;
		}
		if (k == BTN_SOUTH || k == BTN_MODE)
			break;
	}
	dirty = true;
}

/* ---- principal -------------------------------------------------------- */

int main(int argc, char **argv)
{
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
