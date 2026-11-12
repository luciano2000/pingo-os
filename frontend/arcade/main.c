/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * pingo-arcade: a galeria do Pingo Arcade (bartop, só FBNeo).
 *
 * Lista os .zip de /mnt/sd1/Roms com o nome, o ano e o fabricante do DAT do
 * FBNeo (/usr/share/pingo/arcade/arcade.tsv). A lista "Clássicos" deixa de
 * fora BIOS, homebrew, demos, hacks, bootlegs, mahjong/quiz/cassino, versões
 * de placa repetidas e clones cujo original também está no cartão.
 *
 * Painel: amarelo (5) confirma, laranja (6) volta, preto de cima = menu.
 *
 * Desenha na resolução da tela (medidas em 1280x800, escaladas). O jogo
 * escolhido vai para /run/pingo/next e o pingo-shell abre o pingo-retro.
 *
 *   pingo-arcade               galeria
 *   pingo-arcade --message T   só uma mensagem na tela (ex.: "Desligando…")
 *
 * Saída: 0 = jogo escolhido (ou nada), 3 = desligar.
 */
#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <linux/input.h>
#include <arpa/inet.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

#include "imagem.h"

#define DATA        "/mnt/sd1"
#define ROMS        DATA "/Roms"
#define MEDIA       DATA "/Midia"
#define CFG         DATA "/Config/arcade.cfg"
#define TSV         "/usr/share/pingo/arcade/arcade.tsv"
#define SHARE       "/usr/share/pingo/arcade"
#define CORE        "/usr/lib/libretro/fbneo_libretro.so"
#define FONT_INTER  "/usr/share/fonts/inter/"
#define FONT_QUICK  "/usr/share/fonts/quicksand/Quicksand.ttf"

/* Cores dos botões do painel (verde 1, azul 2, ... ; os pretos: menu e 1P) */
#define COR_AMARELO 0xF6C318  /* 5: confirmar */
#define COR_LARANJA 0xF26A1B  /* 6: voltar */
#define COR_PRETO   0x1A1A1A  /* menu */
#define COR_LILAS   0xE07AB8  /* 3: próxima lista */
#define COR_VERMELHO 0xD8232A /* 4: favorito */

#define COL_BG      0x000000
#define COL_TEXT    0xE8E8E8
#define COL_DIM     0x8C8C8C
#define COL_SEL_BG  0xE8E8E8
#define COL_SEL_FG  0x000000
#define COL_PANEL   0x111111

/* ---- jogos ------------------------------------------------------------ */

/* Gêneros do FBNeo (GBF_* dos drivers) que viram filtros */
enum { G_VSFIGHT, G_SCRFIGHT, G_VERSHOOT, G_HORSHOOT, G_RUNGUN, G_SHOOT, G_PLATFORM,
       G_MAZE, G_PUZZLE, G_BREAKOUT, G_SPORTSMISC, G_SPORTSFOOTBALL, G_RACING, G_ACTION,
       G_N };
static const char *genre_names[G_N] = {
	"VSFIGHT", "SCRFIGHT", "VERSHOOT", "HORSHOOT", "RUNGUN", "SHOOT", "PLATFORM",
	"MAZE", "PUZZLE", "BREAKOUT", "SPORTSMISC", "SPORTSFOOTBALL", "RACING", "ACTION",
};
#define G(x) (1u << G_##x)

struct game {
	char zip[32];
	char *name;   /* nome curto, para a lista */
	char *full;   /* descrição do DAT */
	char year[8];
	char *manuf;
	char parent[32];
	unsigned genres;
	int plays;
	bool neogeo, vertical, classic, fav;
};

static struct game *games;
static int ngames;
static int *view; /* índices em games[] da lista atual */
static int nview;

/* Listas: as especiais e os gêneros (só entre os Clássicos) */
enum { L_CLASSIC, L_ALL, L_FAV, L_PLAYED, L_GENRE };
static const struct { const char *name, *key; int type; unsigned genres; } lists[] = {
	{ "Clássicos",    "classicos",  L_CLASSIC, 0 },
	{ "Todos",        "todos",      L_ALL,     0 },
	{ "Favoritos",    "favoritos",  L_FAV,     0 },
	{ "Mais jogados", "jogados",    L_PLAYED,  0 },
	{ "Luta",         "luta",       L_GENRE,   G(VSFIGHT) },
	{ "Briga de rua", "briga",      L_GENRE,   G(SCRFIGHT) },
	{ "Navinha",      "navinha",    L_GENRE,   G(VERSHOOT) | G(HORSHOOT) },
	{ "Run 'n gun",   "rungun",     L_GENRE,   G(RUNGUN) },
	{ "Tiro",         "tiro",       L_GENRE,   G(SHOOT) },
	{ "Plataforma",   "plataforma", L_GENRE,   G(PLATFORM) },
	{ "Labirinto",    "labirinto",  L_GENRE,   G(MAZE) },
	{ "Puzzle",       "puzzle",     L_GENRE,   G(PUZZLE) | G(BREAKOUT) },
	{ "Esporte",      "esporte",    L_GENRE,   G(SPORTSMISC) | G(SPORTSFOOTBALL) },
	{ "Corrida",      "corrida",    L_GENRE,   G(RACING) },
	{ "Ação",         "acao",       L_GENRE,   G(ACTION) },
	{ "Em pé",        "empe",       L_GENRE,   0 }, /* verticais */
};
#define N_LISTS (int)(sizeof(lists) / sizeof(lists[0]))
static int cur_list;

struct dat_row { char *zip, *desc, *year, *manuf, *parent, *system, *kind, *genres; bool vertical; };
static struct dat_row *dat;
static int ndat;

static char *xstrdup(const char *s) { return strdup(s ? s : ""); }

static void load_dat(void)
{
	FILE *f = fopen(TSV, "r");
	if (!f) {
		fprintf(stderr, "pingo-arcade: sem %s\n", TSV);
		return;
	}
	char line[1024];
	int cap = 0;
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = 0;
		char *col[10] = { 0 };
		int n = 0;
		for (char *p = line; n < 10; n++) {
			col[n] = p;
			char *t = strchr(p, '\t');
			if (!t) { n++; break; }
			*t = 0;
			p = t + 1;
		}
		if (n < 7)
			continue;
		if (ndat == cap) {
			cap = cap ? cap * 2 : 8192;
			dat = realloc(dat, cap * sizeof(*dat));
		}
		struct dat_row *r = &dat[ndat++];
		r->zip = xstrdup(col[0]); r->desc = xstrdup(col[1]); r->year = xstrdup(col[2]);
		r->manuf = xstrdup(col[3]); r->parent = xstrdup(col[4]); r->system = xstrdup(col[5]);
		r->kind = xstrdup(col[6]);
		r->genres = xstrdup(n > 7 ? col[7] : "");
		r->vertical = n > 9 && col[9][0] == '1';
	}
	fclose(f);
}

static struct dat_row *dat_find(const char *zip)
{
	int lo = 0, hi = ndat - 1;
	while (lo <= hi) {
		int mid = (lo + hi) / 2, c = strcmp(dat[mid].zip, zip);
		if (!c)
			return &dat[mid];
		if (c < 0)
			lo = mid + 1;
		else
			hi = mid - 1;
	}
	return NULL;
}

static unsigned parse_genres(const char *s)
{
	unsigned m = 0;
	char buf[256], *tok, *save;
	snprintf(buf, sizeof(buf), "%s", s);
	for (tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save))
		for (int i = 0; i < G_N; i++)
			if (!strcmp(tok, genre_names[i]))
				m |= 1u << i;
	return m;
}

/* "Street Fighter II: The World Warrior (World 910522)" -> sem o parêntese;
 * "Art of Fighting / Ryuuko no Ken" -> só o primeiro título */
static char *short_name(const char *desc)
{
	char *s = xstrdup(desc), *p;
	if ((p = strstr(s, " (")))
		*p = 0;
	if ((p = strstr(s, " / ")))
		*p = 0;
	return s;
}

static int cmp_name(const void *a, const void *b)
{
	const struct game *x = &games[*(const int *)a], *y = &games[*(const int *)b];
	int c = strcasecmp(x->name, y->name);
	return c ? c : strcmp(x->zip, y->zip);
}

static int cmp_plays(const void *a, const void *b)
{
	const struct game *x = &games[*(const int *)a], *y = &games[*(const int *)b];
	if (x->plays != y->plays)
		return y->plays - x->plays;
	return cmp_name(a, b);
}

static struct game *find_game(const char *zip)
{
	for (int i = 0; i < ngames; i++)
		if (!strcmp(games[i].zip, zip))
			return &games[i];
	return NULL;
}

static void scan_games(void)
{
	DIR *d = opendir(ROMS);
	struct dirent *e;
	int cap = 0;
	if (!d)
		return;
	while ((e = readdir(d))) {
		size_t n = strlen(e->d_name);
		if (n < 5 || n >= sizeof(games[0].zip) + 4 || strcasecmp(e->d_name + n - 4, ".zip"))
			continue;
		char zip[32];
		snprintf(zip, sizeof(zip), "%.*s", (int)(n - 4), e->d_name);
		struct dat_row *r = dat_find(zip);
		if (!r || !strcmp(r->kind, "bios"))
			continue; /* fora do DAT desta versão, ou BIOS (neogeo.zip) */
		if (ngames == cap) {
			cap = cap ? cap * 2 : 1024;
			games = realloc(games, cap * sizeof(*games));
		}
		struct game *g = &games[ngames++];
		memset(g, 0, sizeof(*g));
		snprintf(g->zip, sizeof(g->zip), "%s", zip);
		g->full = xstrdup(r->desc);
		g->name = short_name(r->desc);
		snprintf(g->year, sizeof(g->year), "%s", r->year);
		g->manuf = xstrdup(r->manuf);
		snprintf(g->parent, sizeof(g->parent), "%s", r->parent);
		g->neogeo = !strcmp(r->system, "neogeo");
		g->classic = !strcmp(r->kind, "jogo");
		g->genres = parse_genres(r->genres);
		g->vertical = r->vertical;
	}
	closedir(d);
	/* clone com o original no cartão: só o original nos Clássicos */
	for (int i = 0; i < ngames; i++)
		if (games[i].classic && games[i].parent[0] && find_game(games[i].parent))
			games[i].classic = false;
}

static bool in_list(const struct game *g, int l)
{
	switch (lists[l].type) {
	case L_CLASSIC: return g->classic;
	case L_ALL:     return true;
	case L_FAV:     return g->fav;
	case L_PLAYED:  return g->plays > 0;
	default:
		if (!g->classic)
			return false;
		return lists[l].genres ? (g->genres & lists[l].genres) != 0 : g->vertical;
	}
}

static int list_count(int l)
{
	int n = 0;
	for (int i = 0; i < ngames; i++)
		n += in_list(&games[i], l);
	return n;
}

static void build_view(void)
{
	free(view);
	view = malloc((ngames ? ngames : 1) * sizeof(int));
	nview = 0;
	for (int i = 0; i < ngames; i++)
		if (in_list(&games[i], cur_list))
			view[nview++] = i;
	qsort(view, nview, sizeof(int), lists[cur_list].type == L_PLAYED ? cmp_plays : cmp_name);
}

/* ---- configuração, favoritos e jogados -------------------------------- */

#define FAVS  DATA "/Config/arcade-favoritos.txt"
#define PLAYS DATA "/Config/arcade-jogados.txt"
static char last_zip[32];

static void cfg_load(void)
{
	FILE *f = fopen(CFG, "r");
	char line[256];
	if (f) {
		while (fgets(line, sizeof(line), f)) {
			line[strcspn(line, "\r\n")] = 0;
			if (!strncmp(line, "lista=", 6)) {
				for (int l = 0; l < N_LISTS; l++)
					if (!strcmp(line + 6, lists[l].key))
						cur_list = l;
			} else if (!strncmp(line, "ultimo=", 7)) {
				snprintf(last_zip, sizeof(last_zip), "%s", line + 7);
			}
		}
		fclose(f);
	}
	if ((f = fopen(FAVS, "r"))) {
		while (fgets(line, sizeof(line), f)) {
			line[strcspn(line, "\r\n")] = 0;
			struct game *g = find_game(line);
			if (g)
				g->fav = true;
		}
		fclose(f);
	}
	if ((f = fopen(PLAYS, "r"))) {
		char zip[64];
		int n;
		while (fgets(line, sizeof(line), f))
			if (sscanf(line, "%63s %d", zip, &n) == 2) {
				struct game *g = find_game(zip);
				if (g)
					g->plays = n;
			}
		fclose(f);
	}
}

static void write_file(const char *path, void (*body)(FILE *))
{
	char tmp[256];
	mkdir(DATA "/Config", 0755);
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	FILE *f = fopen(tmp, "w");
	if (!f)
		return;
	body(f);
	fclose(f);
	rename(tmp, path);
	sync();
}

static const char *save_zip;
static void cfg_body(FILE *f)
{
	fprintf(f, "lista=%s\nultimo=%s\n", lists[cur_list].key, save_zip ? save_zip : "");
}
static void favs_body(FILE *f)
{
	for (int i = 0; i < ngames; i++)
		if (games[i].fav)
			fprintf(f, "%s\n", games[i].zip);
}
static void plays_body(FILE *f)
{
	for (int i = 0; i < ngames; i++)
		if (games[i].plays > 0)
			fprintf(f, "%s %d\n", games[i].zip, games[i].plays);
}

static void cfg_save(const char *zip)
{
	save_zip = zip;
	write_file(CFG, cfg_body);
}

/* ---- vídeo e texto ------------------------------------------------------ */

static SDL_Window *win;
static SDL_Renderer *ren;
static int W, H;
static float S = 1; /* escala: medidas em 1280x800 */
#define PX(v) ((int)((v) * S + 0.5f))

enum { F_LIST, F_LIST_SEL, F_INFO, F_TITLE, F_HINT, F_BIG, F_N };
static TTF_Font *font[F_N];

#define TEXT_CACHE 96
static struct { int f; char *s; SDL_Texture *t; int w, h; unsigned used; } tcache[TEXT_CACHE];
static unsigned tick;

static SDL_Texture *text_tex(int f, const char *s, int *w, int *h)
{
	int slot = 0;
	tick++;
	for (int i = 0; i < TEXT_CACHE; i++) {
		if (tcache[i].s && tcache[i].f == f && !strcmp(tcache[i].s, s)) {
			tcache[i].used = tick;
			*w = tcache[i].w;
			*h = tcache[i].h;
			return tcache[i].t;
		}
		if (tcache[i].used < tcache[slot].used)
			slot = i;
	}
	if (tcache[slot].t)
		SDL_DestroyTexture(tcache[slot].t);
	free(tcache[slot].s);
	tcache[slot].t = NULL;
	tcache[slot].s = NULL;
	SDL_Color white = { 255, 255, 255, 255 };
	SDL_Surface *surf = TTF_RenderUTF8_Blended(font[f], s, white);
	if (!surf)
		return NULL;
	tcache[slot].t = SDL_CreateTextureFromSurface(ren, surf);
	tcache[slot].w = surf->w;
	tcache[slot].h = surf->h;
	SDL_FreeSurface(surf);
	tcache[slot].f = f;
	tcache[slot].s = strdup(s);
	tcache[slot].used = tick;
	*w = tcache[slot].w;
	*h = tcache[slot].h;
	return tcache[slot].t;
}

static int text_w(int f, const char *s)
{
	int w = 0;
	if (*s)
		TTF_SizeUTF8(font[f], s, &w, NULL);
	return w;
}

/* align: 0 esquerda, 1 direita, 2 centro; y é o centro vertical */
static int text(int f, const char *s, int x, int y, Uint32 rgb, int align, int max_w)
{
	char buf[512];
	if (!s || !*s)
		return 0;
	if (max_w > 0 && text_w(f, s) > max_w) {
		size_t m = strlen(s);
		if (m >= sizeof(buf) - 4)
			m = sizeof(buf) - 5;
		for (;;) {
			do
				m--;
			while (m > 0 && ((unsigned char)s[m] & 0xC0) == 0x80);
			memcpy(buf, s, m);
			while (m > 0 && buf[m - 1] == ' ')
				m--;
			strcpy(buf + m, "…");
			if (m == 0 || text_w(f, buf) <= max_w)
				break;
		}
		s = buf;
	}
	int w, h;
	SDL_Texture *t = text_tex(f, s, &w, &h);
	if (!t)
		return 0;
	SDL_Rect d = { align == 1 ? x - w : align == 2 ? x - w / 2 : x, y - h / 2, w, h };
	SDL_SetTextureColorMod(t, (rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255);
	SDL_RenderCopy(ren, t, NULL, &d);
	return w;
}

/* Até 2 linhas, quebrando nas palavras; devolve a altura usada */
static int text_wrap(int f, const char *s, int x, int y_top, int max_w, int line_h, Uint32 rgb)
{
	char line[256];
	const char *p = s;
	int lines = 0;
	while (*p && lines < 2) {
		size_t best = 0;
		for (size_t len = 0;;) {
			const char *e = p + len;
			while (*e == ' ')
				e++;
			while (*e && *e != ' ')
				e++;
			size_t cand = (size_t)(e - p);
			if (cand >= sizeof(line))
				break;
			memcpy(line, p, cand);
			line[cand] = 0;
			if (best && text_w(f, line) > max_w)
				break;
			best = cand;
			if (!*e)
				break;
			len = cand;
		}
		if (!best)
			best = strlen(p) < sizeof(line) - 1 ? strlen(p) : sizeof(line) - 1;
		bool last = lines == 1 && p[best];
		snprintf(line, sizeof(line), "%.*s", (int)(last ? strlen(p) : best), p);
		text(f, line, x, y_top + line_h * lines + line_h / 2, rgb, 0, max_w);
		lines++;
		p += best;
		while (*p == ' ')
			p++;
	}
	return line_h * lines;
}

static void fill(int x, int y, int w, int h, Uint32 rgb, int a)
{
	SDL_Rect r = { x, y, w, h };
	SDL_SetRenderDrawColor(ren, (rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255, a);
	SDL_RenderFillRect(ren, &r);
}

/* Disco antisserrilhado (pílulas, bolinhas dos botões) */
static SDL_Texture *disc;
#define DISC 128

static void make_disc(void)
{
	SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, DISC, DISC, 32, SDL_PIXELFORMAT_ARGB8888);
	Uint32 *px = s->pixels;
	float r = DISC / 2.0f;
	for (int y = 0; y < DISC; y++)
		for (int x = 0; x < DISC; x++) {
			int hits = 0;
			for (int sy = 0; sy < 4; sy++)
				for (int sx = 0; sx < 4; sx++) {
					float fx = x + (sx + 0.5f) / 4 - r, fy = y + (sy + 0.5f) / 4 - r;
					hits += fx * fx + fy * fy <= r * r;
				}
			px[y * (s->pitch / 4) + x] = ((Uint32)(hits * 255 / 16) << 24) | 0xFFFFFF;
		}
	disc = SDL_CreateTextureFromSurface(ren, s);
	SDL_SetTextureBlendMode(disc, SDL_BLENDMODE_BLEND);
	SDL_SetTextureScaleMode(disc, SDL_ScaleModeLinear);
	SDL_FreeSurface(s);
}

static void dot(int cx, int cy, int r, Uint32 rgb)
{
	SDL_Rect d = { cx - r, cy - r, 2 * r, 2 * r };
	SDL_SetTextureColorMod(disc, (rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255);
	SDL_RenderCopy(ren, disc, NULL, &d);
}

static void pill(int x, int y, int w, int h, Uint32 rgb)
{
	int r = h / 2;
	SDL_SetTextureColorMod(disc, (rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255);
	SDL_Rect sl = { 0, 0, DISC / 2, DISC }, sr = { DISC / 2, 0, DISC / 2, DISC };
	SDL_Rect dl = { x, y, r, h }, dr = { x + w - r, y, r, h };
	SDL_RenderCopy(ren, disc, &sl, &dl);
	SDL_RenderCopy(ren, disc, &sr, &dr);
	fill(x + r, y, w - 2 * r, h, rgb, 255);
}

static bool video_init(void)
{
	SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
	if (SDL_Init(SDL_INIT_VIDEO) < 0) {
		fprintf(stderr, "pingo-arcade: SDL: %s\n", SDL_GetError());
		return false;
	}
	SDL_ShowCursor(SDL_DISABLE);
	SDL_SetHint(SDL_HINT_RENDER_DRIVER, "opengles2");
	win = SDL_CreateWindow("pingo-arcade", 0, 0, 0, 0, SDL_WINDOW_FULLSCREEN_DESKTOP);
	ren = win ? SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC) : NULL;
	if (!ren) {
		fprintf(stderr, "pingo-arcade: vídeo: %s\n", SDL_GetError());
		return false;
	}
	SDL_GetRendererOutputSize(ren, &W, &H);
	S = SDL_min(W / 1280.0f, H / 800.0f);
	SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
	if (TTF_Init() < 0)
		return false;
	static const struct { const char *file; int size; long index; } spec[F_N] = {
		[F_LIST]     = { FONT_QUICK, 30, 4L << 16 },  /* SemiBold */
		[F_LIST_SEL] = { FONT_QUICK, 30, 5L << 16 },  /* Bold */
		[F_INFO]     = { FONT_INTER "Inter-Medium.ttf", 22, 0 },
		[F_TITLE]    = { FONT_INTER "Inter-SemiBold.ttf", 28, 0 },
		[F_HINT]     = { FONT_INTER "Inter-Medium.ttf", 20, 0 },
		[F_BIG]      = { FONT_QUICK, 40, 5L << 16 },
	};
	for (int i = 0; i < F_N; i++) {
		font[i] = TTF_OpenFontIndex(spec[i].file, PX(spec[i].size), spec[i].index);
		if (!font[i]) {
			fprintf(stderr, "pingo-arcade: fonte %s: %s\n", spec[i].file, TTF_GetError());
			return false;
		}
		TTF_SetFontHinting(font[i], TTF_HINTING_LIGHT);
	}
	make_disc();
	return true;
}

static void message_screen(const char *msg)
{
	SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
	SDL_RenderClear(ren);
	text(F_BIG, msg, W / 2, H / 2, COL_TEXT, 2, 0);
	SDL_RenderPresent(ren);
}

/* ---- entrada ------------------------------------------------------------ */

#define MAX_KBD 6
static int kbd[MAX_KBD], nkbd;

static bool has_key(int fd, int code)
{
	unsigned long bits[KEY_MAX / (8 * sizeof(long)) + 1] = { 0 };
	return ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(bits)), bits) >= 0 &&
	       (bits[code / (8 * sizeof(long))] >> (code % (8 * sizeof(long)))) & 1;
}

static void open_keyboards(void)
{
	DIR *d = opendir("/dev/input");
	struct dirent *e;
	char path[300];
	if (!d)
		return;
	while ((e = readdir(d)) && nkbd < MAX_KBD) {
		if (strncmp(e->d_name, "event", 5))
			continue;
		snprintf(path, sizeof(path), "/dev/input/%s", e->d_name);
		int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0)
			continue;
		if (has_key(fd, KEY_UP) && has_key(fd, KEY_LEFTCTRL))
			kbd[nkbd++] = fd;
		else
			close(fd);
	}
	closedir(d);
}

/* Ações da galeria */
enum { A_NONE, A_UP, A_DOWN, A_LEFT, A_RIGHT, A_OK, A_BACK, A_MENU, A_POWER, A_LIST, A_FAV };

static int key_action(int code)
{
	switch (code) {
	case KEY_UP:        return A_UP;
	case KEY_DOWN:      return A_DOWN;
	case KEY_LEFT:      return A_LEFT;
	case KEY_RIGHT:     return A_RIGHT;
	case KEY_Z:         /* botão 5 (amarelo) */
	case KEY_1:         /* 1P */
	case KEY_ENTER:     return A_OK;
	case KEY_X:         /* botão 6 (laranja) */
	case KEY_BACKSPACE: return A_BACK;
	case KEY_SPACE:     return A_LIST;  /* botão 3 (lilás) */
	case KEY_LEFTSHIFT: return A_FAV;   /* botão 4 (vermelho) */
	case KEY_ESC:       return A_MENU;
	case KEY_POWER:     return A_POWER;
	default:            return A_NONE;
	}
}

/* Repetição ao segurar ▲▼ (acelera depois de um tempo) */
static int held = A_NONE;
static Uint32 held_since, held_next;

static int poll_action(void)
{
	struct input_event ev;
	int act = A_NONE;
	for (int k = 0; k < nkbd; k++)
		while (read(kbd[k], &ev, sizeof(ev)) == sizeof(ev)) {
			if (ev.type != EV_KEY)
				continue;
			int a = key_action(ev.code);
			if (a == A_NONE)
				continue;
			if (ev.value == 1) {
				act = a;
				if (a == A_UP || a == A_DOWN || a == A_LEFT || a == A_RIGHT) {
					held = a;
					held_since = SDL_GetTicks();
					held_next = held_since + 320;
				}
			} else if (ev.value == 0 && a == held) {
				held = A_NONE;
			}
		}
	Uint32 now = SDL_GetTicks();
	if (act == A_NONE && held != A_NONE && now >= held_next) {
		act = held;
		Uint32 dt = now - held_since;
		held_next = now + (held == A_LEFT || held == A_RIGHT ? 180 : dt > 2000 ? 25 : 60);
	}
	return act;
}

/* ---- imagens do jogo ------------------------------------------------- */

static SDL_Texture *logo, *coin;
static SDL_Texture *snap, *blur;
static int snap_for = -1, snap_w, snap_h;
static Uint32 sel_changed_at;

static SDL_Texture *media(const struct game *g)
{
	static const char *names[] = { "snap.png", "titulo.png", "capa.png" };
	char path[256];
	for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		snprintf(path, sizeof(path), MEDIA "/%s/%s", g->zip, names[i]);
		SDL_Texture *t = img_load(ren, path, 0);
		if (t)
			return t;
	}
	return NULL;
}

/* Fundo: a imagem reduzida a 32x24 e esticada de volta = desfocada */
static void make_blur(void)
{
	if (blur)
		SDL_DestroyTexture(blur);
	blur = NULL;
	if (!snap)
		return;
	blur = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, 32, 24);
	if (!blur)
		return;
	SDL_SetTextureScaleMode(blur, SDL_ScaleModeLinear);
	SDL_SetRenderTarget(ren, blur);
	SDL_RenderCopy(ren, snap, NULL, NULL);
	SDL_SetRenderTarget(ren, NULL);
}

static void load_media(int gi)
{
	if (snap_for == gi)
		return;
	if (snap)
		SDL_DestroyTexture(snap);
	snap = gi >= 0 ? media(&games[gi]) : NULL;
	snap_for = gi;
	if (snap)
		SDL_QueryTexture(snap, NULL, NULL, &snap_w, &snap_h);
	make_blur();
}

/* ---- desenho ------------------------------------------------------------ */

static int sel, top;
#define LIST_X   48
#define LIST_Y   112
#define LIST_W   560
#define ROW_H    54
#define ROW_GAP  4
#define ROWS     11

static char first_letter(int vi)
{
	unsigned char c = (unsigned char)games[view[vi]].name[0];
	return isalpha(c) ? (char)toupper(c) : '#';
}

static void fmt_count(char *out, size_t n, int v)
{
	if (v >= 1000)
		snprintf(out, n, "%d.%03d", v / 1000, v % 1000);
	else
		snprintf(out, n, "%d", v);
}

static void draw_hint(int *x, int y, Uint32 color, bool ring, const char *label)
{
	/* da direita para a esquerda */
	int w = text_w(F_HINT, label);
	*x -= w;
	text(F_HINT, label, *x, y, COL_TEXT, 0, 0);
	*x -= PX(10) + PX(11);
	if (ring)
		dot(*x, y, PX(11), 0x777777);
	dot(*x, y, ring ? PX(9) : PX(11), color);
	*x -= PX(11) + PX(28);
}

static void draw_gallery(void)
{
	SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
	SDL_RenderClear(ren);

	/* fundo: o screenshot do jogo, desfocado e escuro */
	if (blur) {
		SDL_RenderCopy(ren, blur, NULL, NULL);
		fill(0, 0, W, H, 0x000000, 205);
	}

	/* topo: logo e a lista */
	if (logo) {
		int lw, lh;
		SDL_QueryTexture(logo, NULL, NULL, &lw, &lh);
		int h = PX(32), w = lw * h / lh;
		SDL_Rect d = { PX(LIST_X), PX(56) - h / 2, w, h };
		SDL_RenderCopy(ren, logo, NULL, &d);
	}
	char count[32], head[64];
	fmt_count(count, sizeof(count), nview);
	snprintf(head, sizeof(head), "%s · %s", lists[cur_list].name, count);
	text(F_INFO, head, W - PX(48), PX(56), COL_DIM, 1, 0);

	if (!nview) {
		text(F_TITLE, ngames ? "Nenhum jogo nesta lista"
				     : "Copie os jogos (.zip do FBNeo) para a pasta Roms",
		     W / 2, H / 2, COL_TEXT, 2, 0);
		int hx = W - PX(48), hy = H - PX(44);
		draw_hint(&hx, hy, COR_PRETO, true, "menu");
		draw_hint(&hx, hy, COR_LILAS, false, "lista");
		return;
	}

	/* lista */
	for (int r = 0; r < ROWS && top + r < nview; r++) {
		int vi = top + r, y = PX(LIST_Y) + r * (PX(ROW_H) + PX(ROW_GAP));
		const struct game *g = &games[view[vi]];
		bool s = vi == sel;
		if (s)
			pill(PX(LIST_X), y, PX(LIST_W), PX(ROW_H), COL_SEL_BG);
		int fav_w = g->fav ? PX(26) : 0;
		text(s ? F_LIST_SEL : F_LIST, g->name, PX(LIST_X) + PX(22), y + PX(ROW_H) / 2,
		     s ? COL_SEL_FG : COL_TEXT, 0, PX(LIST_W) - PX(44) - fav_w);
		if (g->fav) /* favorito: bolinha vermelha, a cor do botão 4 */
			dot(PX(LIST_X) + PX(LIST_W) - PX(26), y + PX(ROW_H) / 2, PX(7), COR_VERMELHO);
	}

	/* painel do jogo */
	const struct game *g = &games[view[sel]];
	int px = PX(660), pw = W - px - PX(48), ph = pw * 3 / 4, py = PX(LIST_Y);
	fill(px, py, pw, ph, COL_PANEL, 255);
	if (snap) {
		float a = (float)snap_w / snap_h;
		int w = pw, h = (int)(pw / a);
		if (h > ph) {
			h = ph;
			w = (int)(ph * a);
		}
		SDL_Rect d = { px + (pw - w) / 2, py + (ph - h) / 2, w, h };
		SDL_RenderCopy(ren, snap, NULL, &d);
	} else if (coin) {
		int c = ph / 3;
		SDL_Rect d = { px + (pw - c) / 2, py + (ph - c) / 2, c, c };
		SDL_SetTextureAlphaMod(coin, 70);
		SDL_RenderCopy(ren, coin, NULL, &d);
	}
	int y = py + ph + PX(22);
	y += text_wrap(F_TITLE, g->full, px, y, pw, PX(36), COL_TEXT);
	char info[160];
	snprintf(info, sizeof(info), "%s%s%s%s", g->manuf, g->manuf[0] && g->year[0] ? " · " : "",
		 g->year, g->neogeo ? " · Neo Geo" : "");
	text(F_INFO, info, px, y + PX(18), COL_DIM, 0, pw);

	/* dicas */
	int hx = W - PX(48), hy = H - PX(44);
	draw_hint(&hx, hy, COR_PRETO, true, "menu");
	{
		const char *l = "letra";
		int w = text_w(F_HINT, l);
		hx -= w;
		text(F_HINT, l, hx, hy, COL_TEXT, 0, 0);
		hx -= PX(8);
		hx -= text(F_HINT, "◀ ▶", hx, hy, COL_TEXT, 1, 0) + PX(28);
	}
	draw_hint(&hx, hy, COR_LILAS, false, "lista");
	draw_hint(&hx, hy, COR_VERMELHO, false, g->fav ? "desfavoritar" : "favorito");
	draw_hint(&hx, hy, COR_AMARELO, false, "jogar");
}

/* ---- menu (botão preto de cima) ----------------------------------------- */

static void ip_addr(char *out, size_t n)
{
	struct ifaddrs *ifa, *i;
	snprintf(out, n, "sem rede");
	if (getifaddrs(&ifa) < 0)
		return;
	for (i = ifa; i; i = i->ifa_next)
		if (i->ifa_addr && i->ifa_addr->sa_family == AF_INET && strcmp(i->ifa_name, "lo")) {
			inet_ntop(AF_INET, &((struct sockaddr_in *)i->ifa_addr)->sin_addr, out, n);
			break;
		}
	freeifaddrs(ifa);
}

static void version(char *out, size_t n)
{
	FILE *f = fopen("/etc/os-release", "r");
	char line[128];
	snprintf(out, n, "?");
	if (!f)
		return;
	while (fgets(line, sizeof(line), f))
		if (!strncmp(line, "VERSION_ID=", 11)) {
			char *v = line + 11;
			v[strcspn(v, "\r\n")] = 0;
			if (*v == '"')
				v++;
			v[strcspn(v, "\"")] = 0;
			snprintf(out, n, "%s", v);
		}
	fclose(f);
}

enum { M_LIST, M_OFF, M_BACK, M_N };

static int counts[N_LISTS];

/* Escolha da lista, com quantos jogos cada uma tem */
static void draw_lists(int lsel, int ltop)
{
	draw_gallery();
	fill(0, 0, W, H, 0x000000, 215);
	int x = PX(LIST_X), y0 = PX(LIST_Y);
	text(F_TITLE, "Lista", x, PX(56), COL_TEXT, 0, 0);
	for (int r = 0; r < ROWS && ltop + r < N_LISTS; r++) {
		int i = ltop + r, y = y0 + r * (PX(ROW_H) + PX(ROW_GAP));
		bool s = i == lsel;
		char n[32];
		fmt_count(n, sizeof(n), counts[i]);
		if (s)
			pill(x, y, PX(LIST_W), PX(ROW_H), COL_SEL_BG);
		text(s ? F_LIST_SEL : F_LIST, lists[i].name, x + PX(22), y + PX(ROW_H) / 2,
		     s ? COL_SEL_FG : COL_TEXT, 0, 0);
		text(F_INFO, n, x + PX(LIST_W) - PX(22), y + PX(ROW_H) / 2, s ? 0x444444 : COL_DIM, 1, 0);
		if (i == cur_list && !s)
			dot(x + PX(8), y + PX(ROW_H) / 2, PX(4), COL_TEXT);
	}
	int hx = W - PX(48), hy = H - PX(44);
	draw_hint(&hx, hy, COR_LARANJA, false, "voltar");
	draw_hint(&hx, hy, COR_AMARELO, false, "escolher");
}

static void draw_menu(int msel)
{
	draw_gallery();
	fill(0, 0, W, H, 0x000000, 215);
	char lname[64];
	snprintf(lname, sizeof(lname), "Lista: %s", lists[cur_list].name);
	const char *items[M_N] = { lname, "Desligar", "Voltar" };
	int x = PX(LIST_X), y0 = PX(LIST_Y);
	text(F_TITLE, "Menu", x, PX(56), COL_TEXT, 0, 0);
	for (int i = 0; i < M_N; i++) {
		int y = y0 + i * (PX(ROW_H) + PX(ROW_GAP));
		if (i == msel)
			pill(x, y, PX(LIST_W), PX(ROW_H), COL_SEL_BG);
		text(i == msel ? F_LIST_SEL : F_LIST, items[i], x + PX(22), y + PX(ROW_H) / 2,
		     i == msel ? COL_SEL_FG : COL_TEXT, 0, 0);
	}
	char ip[64], ver[64], info[160];
	ip_addr(ip, sizeof(ip));
	version(ver, sizeof(ver));
	snprintf(info, sizeof(info), "Pingo Arcade %s · rede %s", ver, ip);
	text(F_INFO, info, x, H - PX(44), COL_DIM, 0, 0);
	int hx = W - PX(48), hy = H - PX(44);
	draw_hint(&hx, hy, COR_LARANJA, false, "voltar");
	draw_hint(&hx, hy, COR_AMARELO, false, "confirmar");
}

/* ---- laço ---------------------------------------------------------------- */

static void keep_visible(void)
{
	if (sel < top + 2)
		top = sel - 2;
	if (sel > top + ROWS - 3)
		top = sel - (ROWS - 3);
	if (top > nview - ROWS)
		top = nview - ROWS;
	if (top < 0)
		top = 0;
}

static void select_zip(const char *zip)
{
	sel = 0;
	for (int i = 0; i < nview; i++)
		if (!strcmp(games[view[i]].zip, zip)) {
			sel = i;
			break;
		}
	top = sel - ROWS / 2;
	keep_visible();
}

static void move(int to)
{
	if (!nview)
		return;
	if (to < 0)
		to = 0;
	if (to >= nview)
		to = nview - 1;
	if (to != sel) {
		sel = to;
		sel_changed_at = SDL_GetTicks();
	}
	keep_visible();
}

static void letter_jump(int dir)
{
	if (!nview)
		return;
	char cur = first_letter(sel);
	int i = sel;
	if (dir > 0) {
		while (i < nview - 1 && first_letter(i) == cur)
			i++;
	} else {
		/* início da letra atual; se já está nele, da anterior */
		while (i > 0 && first_letter(i - 1) == cur)
			i--;
		if (i == sel && i > 0) {
			char prev = first_letter(i - 1);
			i--;
			while (i > 0 && first_letter(i - 1) == prev)
				i--;
		}
	}
	move(i);
}

static void set_list(int l)
{
	char keep[32];
	snprintf(keep, sizeof(keep), "%s", nview ? games[view[sel]].zip : "");
	cur_list = l;
	build_view();
	select_zip(keep);
	sel_changed_at = SDL_GetTicks();
	cfg_save(keep);
}

static int launch(void)
{
	const struct game *g = &games[view[sel]];
	FILE *f = fopen("/run/pingo/next", "w");
	if (!f)
		return 1;
	fprintf(f, "%s\n%s/%s.zip\nARCADE\n", CORE, ROMS, g->zip);
	fclose(f);
	games[view[sel]].plays++;
	write_file(PLAYS, plays_body);
	cfg_save(g->zip);
	return 0;
}

int main(int argc, char **argv)
{
	if (!video_init())
		return 1;
	if (argc == 3 && !strcmp(argv[1], "--message")) {
		message_screen(argv[2]);
		SDL_Delay(300);
		return 0; /* a imagem fica no fb0 até desligar */
	}
	logo = img_load(ren, SHARE "/logo-horizontal.png", 0);
	coin = img_load(ren, SHARE "/moeda.png", 0);
	open_keyboards();
	load_dat();
	scan_games();
	cfg_load();
	build_view();
	select_zip(last_zip);
	mkdir("/run/pingo", 0755);
	fprintf(stderr, "pingo-arcade: %d jogos, %d na lista, %d teclados\n", ngames, nview, nkbd);

	enum { V_GALLERY, V_MENU, V_LISTS } mode = V_GALLERY;
	int msel = 0, lsel = 0, ltop = 0, rc = 0;
	for (;;) {
		SDL_Event e;
		while (SDL_PollEvent(&e))
			;
		int a = poll_action();
		if (a == A_POWER) {
			cfg_save(nview ? games[view[sel]].zip : NULL);
			message_screen("Desligando…");
			rc = 3;
			break;
		}
		if (mode == V_LISTS) {
			if (a == A_UP && lsel > 0)
				lsel--;
			else if (a == A_DOWN && lsel < N_LISTS - 1)
				lsel++;
			else if (a == A_BACK)
				mode = V_MENU;
			else if (a == A_MENU)
				mode = V_GALLERY;
			else if (a == A_OK) {
				set_list(lsel);
				mode = V_GALLERY;
			}
			if (lsel < ltop)
				ltop = lsel;
			if (lsel >= ltop + ROWS)
				ltop = lsel - ROWS + 1;
		} else if (mode == V_MENU) {
			if (a == A_UP && msel > 0)
				msel--;
			else if (a == A_DOWN && msel < M_N - 1)
				msel++;
			else if (a == A_BACK || a == A_MENU)
				mode = V_GALLERY;
			else if (a == A_OK) {
				if (msel == M_LIST) {
					for (int l = 0; l < N_LISTS; l++)
						counts[l] = list_count(l);
					lsel = cur_list;
					ltop = lsel - ROWS / 2 < 0 ? 0 : lsel - ROWS / 2;
					mode = V_LISTS;
				} else if (msel == M_OFF) {
					cfg_save(nview ? games[view[sel]].zip : NULL);
					message_screen("Desligando…");
					rc = 3;
					break;
				} else {
					mode = V_GALLERY;
				}
			}
		} else {
			if (a == A_UP)
				move(sel - 1);
			else if (a == A_DOWN)
				move(sel + 1);
			else if (a == A_LEFT)
				letter_jump(-1);
			else if (a == A_RIGHT)
				letter_jump(1);
			else if (a == A_MENU) {
				mode = V_MENU;
				msel = 0;
			} else if (a == A_LIST) {
				set_list((cur_list + 1) % N_LISTS);
			} else if (a == A_FAV && nview) {
				struct game *g = &games[view[sel]];
				g->fav = !g->fav;
				write_file(FAVS, favs_body);
				if (lists[cur_list].type == L_FAV && !g->fav) {
					int keep_sel = sel;
					build_view();
					sel = keep_sel;
					move(sel >= nview ? nview - 1 : sel);
				}
			} else if (a == A_OK && nview) {
				rc = launch();
				break;
			}
		}
		/* a imagem só carrega quando a seleção para um pouco */
		if (nview && snap_for != view[sel] && SDL_GetTicks() - sel_changed_at > 120)
			load_media(view[sel]);
		if (mode == V_LISTS)
			draw_lists(lsel, ltop);
		else if (mode == V_MENU)
			draw_menu(msel);
		else
			draw_gallery();
		SDL_RenderPresent(ren);
	}
	SDL_Quit();
	return rc;
}
