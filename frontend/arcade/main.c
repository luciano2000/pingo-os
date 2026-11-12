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
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

#include "imagem.h"
#include "controle2.h"
#include "fbcopy.h"

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
static char cabinet[64]; /* nome do bartop (o pingo-retro mostra no Controle 1) */

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
			} else if (!strncmp(line, "gabinete=", 9)) {
				snprintf(cabinet, sizeof(cabinet), "%s", line + 9);
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
	if (cabinet[0])
		fprintf(f, "gabinete=%s\n", cabinet);
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

/* Mensagem no centro; fica também no fb0, que é o que aparece entre um
 * programa e outro (galeria -> jogo, desligando) */
static void message_screen(const char *msg)
{
	SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
	SDL_RenderClear(ren);
	text(F_BIG, msg, W / 2, H / 2, COL_TEXT, 2, 0);
	fb_copy_from(ren);
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
enum { A_NONE, A_UP, A_DOWN, A_LEFT, A_RIGHT, A_OK, A_BACK, A_MENU, A_POWER, A_LIST, A_FAV, A_START };

static int key_action(int code)
{
	switch (code) {
	case KEY_UP:        return A_UP;
	case KEY_DOWN:      return A_DOWN;
	case KEY_LEFT:      return A_LEFT;
	case KEY_RIGHT:     return A_RIGHT;
	case KEY_Z:         /* botão 5 (amarelo) */
	case KEY_ENTER:     return A_OK;
	case KEY_1:         return A_START; /* 1P: joga; no teclado da tela, OK */
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

enum { M_LIST, M_WIFI, M_PAD2, M_OFF, M_BACK, M_N };

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
	const char *items[M_N] = { lname, "Wi-Fi", "Controle 2", "Desligar", "Voltar" };
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

/* ---- controle 2: mapear (menu → Controle 2) ---------------------------- */

static struct c2_dev p2 = { .fd = -1 };
static int p2_step;            /* função sendo pedida; C2_N = terminou */
static Uint32 p2_ignore_until, p2_done_at;

static void pad2_start(void)
{
	if (p2.fd < 0)
		c2_open(&p2);
	p2_step = 0;
	p2_done_at = 0;
	p2_ignore_until = SDL_GetTicks() + 300;
}

/* true quando o assistente terminou (ou não há controle e o usuário saiu) */
static bool pad2_frame(int a)
{
	Uint32 now = SDL_GetTicks();
	if (p2.fd < 0) {
		if (now >= p2_ignore_until) { /* procura de novo (plugado agora) */
			c2_open(&p2);
			p2_ignore_until = now + 1000;
		}
		return a == A_BACK || a == A_MENU;
	}
	if (p2_step >= C2_N)
		return now - p2_done_at > 1200;
	if (a == A_MENU) { /* cancela: volta o mapa salvo */
		c2_close(&p2);
		return true;
	}
	struct c2_bind b;
	bool got = c2_capture(&p2, &b);
	if (p2.fd < 0)
		return false; /* tirou da USB no meio */
	if (now < p2_ignore_until)
		return false;
	if (a == A_BACK) { /* laranja do painel: pula esta função */
		memset(&p2.map[p2_step], 0, sizeof(p2.map[p2_step]));
		got = true;
	} else if (got) {
		/* o mesmo botão em duas funções: a anterior perde */
		for (int i = 0; i < p2_step; i++)
			if (p2.map[i].type == b.type && p2.map[i].code == b.code && p2.map[i].sign == b.sign)
				memset(&p2.map[i], 0, sizeof(p2.map[i]));
		p2.map[p2_step] = b;
	}
	if (got) {
		p2_step++;
		p2_ignore_until = now + 350;
		if (p2_step >= C2_N) {
			c2_save(&p2);
			p2_done_at = now;
		}
	}
	return false;
}

static void draw_pad2(void)
{
	draw_gallery();
	fill(0, 0, W, H, 0x000000, 225);
	int x = PX(LIST_X), y0 = PX(LIST_Y);
	text(F_TITLE, "Controle 2", x, PX(56), COL_TEXT, 0, 0);
	int hx = W - PX(48), hy = H - PX(44);
	if (p2.fd < 0) {
		text(F_TITLE, "Ligue um controle na USB", W / 2, H / 2 - PX(20), COL_TEXT, 2, 0);
		text(F_INFO, "Ele vira o player 2 nos jogos", W / 2, H / 2 + PX(24), COL_DIM, 2, 0);
		draw_hint(&hx, hy, COR_LARANJA, false, "voltar");
		return;
	}
	text(F_INFO, p2.name, W - PX(48), PX(56), COL_DIM, 1, 0);
	if (p2_step >= C2_N) {
		text(F_TITLE, "Pronto: mapa salvo", W / 2, H / 2, COL_TEXT, 2, 0);
		return;
	}
	/* a lista das funções, com o que já foi mapeado */
	int row = PX(ROW_H) * 3 / 4, ltop = p2_step - 5 < 0 ? 0 : p2_step - 5;
	if (ltop > C2_N - 12)
		ltop = C2_N - 12 < 0 ? 0 : C2_N - 12;
	for (int r = 0; r < 12 && ltop + r < C2_N; r++) {
		int i = ltop + r, y = y0 + r * (row + PX(ROW_GAP));
		bool s = i == p2_step;
		char d[48];
		if (s)
			pill(x, y, PX(LIST_W) + PX(240), row, COL_SEL_BG);
		text(F_INFO, c2_labels[i], x + PX(22), y + row / 2, s ? COL_SEL_FG : COL_TEXT, 0, 0);
		if (s)
			snprintf(d, sizeof(d), "aperte no controle…");
		else if (i < p2_step)
			c2_describe(&p2.map[i], d, sizeof(d));
		else
			d[0] = 0;
		text(F_INFO, d, x + PX(LIST_W) + PX(218), y + row / 2, s ? 0x444444 : COL_DIM, 1, 0);
	}
	draw_hint(&hx, hy, COR_PRETO, true, "cancelar");
	draw_hint(&hx, hy, COR_LARANJA, false, "pular");
}

/* ---- Wi-Fi (menu → Wi-Fi), pelo pingo-wifi ------------------------------ *
 * Os comandos demoram (procurar redes ~4 s, conectar até 30 s): rodam em
 * outro processo e a tela continua respondendo. A senha vai pela entrada
 * padrão do pingo-wifi, nunca pela linha de comando. */

struct job { pid_t pid; int fd; char out[8192]; int len, status; bool done; };

static bool job_start(struct job *j, const char *const argv[], const char *input)
{
	int out[2], in[2];
	memset(j, 0, sizeof(*j));
	j->pid = -1;
	if (pipe(out) < 0)
		return false;
	if (pipe(in) < 0) {
		close(out[0]);
		close(out[1]);
		return false;
	}
	pid_t pid = fork();
	if (pid == 0) {
		dup2(in[0], 0);
		dup2(out[1], 1);
		close(in[1]);
		close(out[0]);
		execv(argv[0], (char *const *)argv);
		_exit(127);
	}
	close(in[0]);
	close(out[1]);
	if (pid < 0) {
		close(in[1]);
		close(out[0]);
		return false;
	}
	if (input && *input && write(in[1], input, strlen(input)) < 0)
		perror("pingo-arcade: senha");
	if (input && write(in[1], "\n", 1) < 0)
		perror("pingo-arcade: senha");
	close(in[1]);
	fcntl(out[0], F_SETFL, O_NONBLOCK);
	j->pid = pid;
	j->fd = out[0];
	return true;
}

/* true quando terminou */
static bool job_poll(struct job *j)
{
	if (j->done || j->pid < 0)
		return true;
	ssize_t r;
	while (j->len < (int)sizeof(j->out) - 1 &&
	       (r = read(j->fd, j->out + j->len, sizeof(j->out) - 1 - j->len)) > 0)
		j->len += (int)r;
	j->out[j->len] = 0;
	int st;
	if (waitpid(j->pid, &st, WNOHANG) == j->pid) {
		while (j->len < (int)sizeof(j->out) - 1 &&
		       (r = read(j->fd, j->out + j->len, sizeof(j->out) - 1 - j->len)) > 0)
			j->len += (int)r;
		j->out[j->len] = 0;
		close(j->fd);
		j->status = WIFEXITED(st) ? WEXITSTATUS(st) : 1;
		j->done = true;
	}
	return j->done;
}

#define WIFI "/usr/sbin/pingo-wifi"
#define MAX_NETS 32
static struct net { int sig; char type[8]; bool saved; char ssid[64]; } nets[MAX_NETS];
static int nnets;
static bool wifi_on;
static char wifi_ssid[64], wifi_ip[32];
static struct job wjob;
static enum { WJ_NONE, WJ_STATUS, WJ_SCAN, WJ_TOGGLE, WJ_CONNECT, WJ_FORGET } wjob_kind;
static int wsel, wtop;
static char wmsg[128];
static Uint32 wmsg_until;
static char wtarget[64]; /* rede sendo conectada */

static void wifi_run(int kind, const char *const argv[], const char *input)
{
	if (job_start(&wjob, argv, input))
		wjob_kind = kind;
}

static void wifi_status(void)
{
	const char *a[] = { WIFI, "status", NULL };
	wifi_run(WJ_STATUS, a, NULL);
}

static void wifi_scan(void)
{
	const char *a[] = { WIFI, "scan", NULL };
	wifi_run(WJ_SCAN, a, NULL);
}

static void wifi_toast(const char *m)
{
	snprintf(wmsg, sizeof(wmsg), "%s", m);
	wmsg_until = SDL_GetTicks() + 3500;
}

static void wifi_parse_status(const char *o)
{
	wifi_on = strstr(o, "estado ligado") != NULL;
	wifi_ssid[0] = wifi_ip[0] = 0;
	const char *p;
	if ((p = strstr(o, "\nrede ")))
		sscanf(p + 6, "%63[^\n]", wifi_ssid);
	if ((p = strstr(o, "\nip ")))
		sscanf(p + 4, "%31[^\n]", wifi_ip);
}

static void wifi_parse_scan(char *o)
{
	nnets = 0;
	for (char *line = strtok(o, "\n"); line && nnets < MAX_NETS; line = strtok(NULL, "\n")) {
		struct net *n = &nets[nnets];
		char saved[4];
		if (sscanf(line, "%d\t%7[^\t]\t%3[^\t]\t%63[^\n]", &n->sig, n->type, saved, n->ssid) == 4) {
			n->saved = saved[0] == '1';
			nnets++;
		}
	}
}

/* Chamado a cada quadro na tela de Wi-Fi: termina os comandos em andamento */
static void wifi_tick(void)
{
	if (wjob_kind == WJ_NONE || !job_poll(&wjob))
		return;
	int kind = wjob_kind;
	wjob_kind = WJ_NONE;
	switch (kind) {
	case WJ_STATUS:
		wifi_parse_status(wjob.out);
		if (wifi_on)
			wifi_scan();
		else
			nnets = 0;
		break;
	case WJ_SCAN:
		wifi_parse_scan(wjob.out);
		break;
	case WJ_TOGGLE:
		wifi_status();
		break;
	case WJ_CONNECT:
		if (wjob.status == 0) {
			char m[128];
			snprintf(m, sizeof(m), "Conectado a %s", wtarget);
			wifi_toast(m);
		} else if (wjob.status == 3) {
			wifi_toast("A senha precisa ter de 8 a 63 caracteres");
		} else {
			wifi_toast("Não conectou: senha errada ou rede longe");
		}
		wifi_status();
		break;
	case WJ_FORGET:
		wifi_toast("Rede esquecida");
		wifi_status();
		break;
	}
}

static void wifi_open(void)
{
	wsel = wtop = 0;
	nnets = 0;
	wmsg[0] = 0;
	wifi_status();
}

/* ---- teclado na tela ------------------------------------------------------ */

static const char *const kbd_rows[3][4] = {
	{ "1234567890", "qwertyuiop", "asdfghjkl@", "zxcvbnm.-_" },
	{ "1234567890", "QWERTYUIOP", "ASDFGHJKL@", "ZXCVBNM.-_" },
	{ "!#$%&*()+=", "?/\\:;'\"<>,", "[]{}~^`|", "" },
};
static const char *const kbd_mode_name[3] = { "abc", "ABC", "#+=" };
enum { K_MODE, K_SPACE, K_DEL, K_OK, K_SPECIALS };
static const char *const kbd_special[K_SPECIALS] = { "abc", "espaço", "apagar", "OK" };
static int kmode, krow, kcol;
static char kbuf[64];

static int kbd_row_len(int r)
{
	if (r == 4)
		return K_SPECIALS;
	return (int)strlen(kbd_rows[kmode][r]);
}

static void kbd_open(void)
{
	kmode = krow = kcol = 0;
	kbuf[0] = 0;
}

static void kbd_backspace(void)
{
	size_t n = strlen(kbuf);
	if (n)
		kbuf[n - 1] = 0;
}

static void kbd_type(char c)
{
	size_t n = strlen(kbuf);
	if (n < sizeof(kbuf) - 1) {
		kbuf[n] = c;
		kbuf[n + 1] = 0;
	}
}

static void kbd_fix_col(void)
{
	while (kbd_row_len(krow) == 0)
		krow = krow == 4 ? 0 : krow + 1;
	if (kcol >= kbd_row_len(krow))
		kcol = kbd_row_len(krow) - 1;
}

/* 1 = OK, -1 = cancelou, 0 = continua */
static int kbd_frame(int a)
{
	switch (a) {
	case A_UP:    krow = (krow + 4) % 5; kbd_fix_col(); break;
	case A_DOWN:  krow = (krow + 1) % 5; kbd_fix_col(); break;
	case A_LEFT:  kcol = (kcol - 1 + kbd_row_len(krow)) % kbd_row_len(krow); break;
	case A_RIGHT: kcol = (kcol + 1) % kbd_row_len(krow); break;
	case A_LIST:  kmode = (kmode + 1) % 3; kbd_fix_col(); break;
	case A_BACK:  kbd_backspace(); break;
	case A_MENU:  return -1;
	case A_START: return 1;
	case A_OK:
		if (krow < 4) {
			kbd_type(kbd_rows[kmode][krow][kcol]);
		} else if (kcol == K_MODE) {
			kmode = (kmode + 1) % 3;
			kbd_fix_col();
		} else if (kcol == K_SPACE) {
			kbd_type(' ');
		} else if (kcol == K_DEL) {
			kbd_backspace();
		} else {
			return 1;
		}
		break;
	}
	return 0;
}

static void draw_kbd(const char *title)
{
	SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
	SDL_RenderClear(ren);
	int x0 = PX(LIST_X);
	text(F_TITLE, title, x0, PX(56), COL_TEXT, 0, W - 2 * x0);
	/* o campo, com o texto à mostra (a tela do bartop é só sua) */
	int fy = PX(110), fh = PX(64);
	pill(x0, fy, W - 2 * x0, fh, 0x222222);
	char shown[80];
	snprintf(shown, sizeof(shown), "%s|", kbuf);
	text(F_LIST, shown, x0 + PX(28), fy + fh / 2, COL_TEXT, 0, W - 2 * x0 - PX(56));
	/* teclas */
	int kw = PX(96), kh = PX(72), gap = PX(10), ky = PX(220);
	for (int r = 0; r < 5; r++) {
		int n = kbd_row_len(r);
		if (!n)
			continue;
		int w = r == 4 ? PX(200) : kw;
		int row_w = n * w + (n - 1) * gap, x = (W - row_w) / 2, y = ky + r * (kh + gap);
		for (int c = 0; c < n; c++) {
			bool s = r == krow && c == kcol;
			int kx = x + c * (w + gap);
			char label[16];
			if (r == 4)
				snprintf(label, sizeof(label), "%s",
					 c == K_MODE ? kbd_mode_name[(kmode + 1) % 3] : kbd_special[c]);
			else
				snprintf(label, sizeof(label), "%c", kbd_rows[kmode][r][c]);
			pill(kx, y, w, kh, s ? COL_SEL_BG : 0x2A2A2A);
			text(r == 4 ? F_INFO : F_LIST_SEL, label, kx + w / 2, y + kh / 2,
			     s ? COL_SEL_FG : COL_TEXT, 2, 0);
		}
	}
	int hx = W - PX(48), hy = H - PX(44);
	draw_hint(&hx, hy, COR_PRETO, true, "cancelar");
	draw_hint(&hx, hy, COR_PRETO, false, "1P conectar");
	draw_hint(&hx, hy, COR_LILAS, false, "abc/ABC/#+=");
	draw_hint(&hx, hy, COR_LARANJA, false, "apagar");
	draw_hint(&hx, hy, COR_AMARELO, false, "digitar");
}

/* Linhas da tela de Wi-Fi: 0 = ligar/desligar, depois as redes */
static int wifi_rows(void) { return 1 + (wifi_on ? nnets : 0); }

static void draw_wifi(void)
{
	draw_gallery();
	fill(0, 0, W, H, 0x000000, 225);
	int x = PX(LIST_X), y0 = PX(LIST_Y);
	text(F_TITLE, "Wi-Fi", x, PX(56), COL_TEXT, 0, 0);
	char st[160];
	if (!wifi_on)
		snprintf(st, sizeof(st), "desligado");
	else if (wifi_ssid[0])
		snprintf(st, sizeof(st), "%s · %s", wifi_ssid, wifi_ip[0] ? wifi_ip : "sem endereço");
	else
		snprintf(st, sizeof(st), "não conectado");
	text(F_INFO, st, W - PX(48), PX(56), COL_DIM, 1, 0);
	int n = wifi_rows();
	for (int r = 0; r < ROWS && wtop + r < n; r++) {
		int i = wtop + r, y = y0 + r * (PX(ROW_H) + PX(ROW_GAP));
		bool s = i == wsel;
		int rw = W - 2 * x;
		if (s)
			pill(x, y, rw, PX(ROW_H), COL_SEL_BG);
		Uint32 fg = s ? COL_SEL_FG : COL_TEXT, dim = s ? 0x444444 : COL_DIM;
		if (i == 0) {
			text(s ? F_LIST_SEL : F_LIST, wifi_on ? "Wi-Fi: ligado" : "Wi-Fi: desligado",
			     x + PX(22), y + PX(ROW_H) / 2, fg, 0, 0);
			continue;
		}
		const struct net *nt = &nets[i - 1];
		bool cur = wifi_ssid[0] && !strcmp(nt->ssid, wifi_ssid);
		char info[64];
		snprintf(info, sizeof(info), "%s%s%s", cur ? "conectado · " : nt->saved ? "salva · " : "",
			 !strcmp(nt->type, "aberta") ? "aberta" : "senha",
			 nt->sig >= -60 ? " · ●●●" : nt->sig >= -72 ? " · ●●○" : " · ●○○");
		int iw = text(F_INFO, info, x + rw - PX(22), y + PX(ROW_H) / 2, dim, 1, 0);
		text(s ? F_LIST_SEL : F_LIST, nt->ssid, x + PX(22), y + PX(ROW_H) / 2, fg, 0,
		     rw - iw - PX(66));
	}
	if (wjob_kind == WJ_SCAN || wjob_kind == WJ_STATUS)
		text(F_INFO, "Procurando redes…", x, y0 + n * (PX(ROW_H) + PX(ROW_GAP)) + PX(24), COL_DIM, 0, 0);
	if (wjob_kind == WJ_CONNECT) {
		char m[128];
		snprintf(m, sizeof(m), "Conectando a %s…", wtarget);
		fill(0, 0, W, H, 0x000000, 180);
		text(F_BIG, m, W / 2, H / 2, COL_TEXT, 2, W - PX(96));
	} else if (wmsg[0] && SDL_GetTicks() < wmsg_until) {
		text(F_TITLE, wmsg, x, H - PX(96), COL_TEXT, 0, W - 2 * x);
	}
	int hx = W - PX(48), hy = H - PX(44);
	draw_hint(&hx, hy, COR_LARANJA, false, "voltar");
	if (wsel > 0 && nets[wsel - 1].saved)
		draw_hint(&hx, hy, COR_VERMELHO, false, "esquecer");
	draw_hint(&hx, hy, COR_LILAS, false, "procurar");
	draw_hint(&hx, hy, COR_AMARELO, false, wsel == 0 ? (wifi_on ? "desligar" : "ligar") : "conectar");
}

/* 1 = sair da tela de Wi-Fi */
static int wifi_frame(int a, bool *typing)
{
	wifi_tick();
	if (wjob_kind == WJ_CONNECT || wjob_kind == WJ_TOGGLE || wjob_kind == WJ_FORGET)
		return 0; /* espera o comando */
	int n = wifi_rows();
	if (wsel >= n)
		wsel = n - 1;
	if (a == A_UP && wsel > 0)
		wsel--;
	else if (a == A_DOWN && wsel < n - 1)
		wsel++;
	else if (a == A_BACK || a == A_MENU)
		return 1;
	else if (a == A_LIST && wifi_on && wjob_kind == WJ_NONE)
		wifi_scan();
	else if (a == A_FAV && wsel > 0 && nets[wsel - 1].saved) {
		const char *f[] = { WIFI, "forget", nets[wsel - 1].ssid, NULL };
		wifi_run(WJ_FORGET, f, NULL);
	} else if (a == A_OK) {
		if (wsel == 0) {
			const char *t[] = { WIFI, wifi_on ? "off" : "on", NULL };
			if (wjob_kind != WJ_NONE) { /* procurando: para e troca */
				kill(wjob.pid, SIGTERM);
				job_poll(&wjob);
				wjob_kind = WJ_NONE;
			}
			wifi_run(WJ_TOGGLE, t, NULL);
		} else {
			const struct net *nt = &nets[wsel - 1];
			snprintf(wtarget, sizeof(wtarget), "%s", nt->ssid);
			if (nt->saved || !strcmp(nt->type, "aberta")) {
				const char *c[] = { WIFI, "connect", wtarget, NULL };
				wifi_run(WJ_CONNECT, c, "");
			} else {
				kbd_open();
				*typing = true;
			}
		}
	}
	if (wsel < wtop)
		wtop = wsel;
	if (wsel >= wtop + ROWS)
		wtop = wsel - ROWS + 1;
	return 0;
}

static void wifi_connect_typed(void)
{
	const char *c[] = { WIFI, "connect", wtarget, NULL };
	wifi_run(WJ_CONNECT, c, kbuf);
	memset(kbuf, 0, sizeof(kbuf)); /* a senha não fica na memória */
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

	enum { V_GALLERY, V_MENU, V_LISTS, V_PAD2, V_WIFI, V_KBD } mode = V_GALLERY;
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
		if (mode == V_KBD) {
			int k = kbd_frame(a);
			if (k > 0) {
				wifi_connect_typed();
				mode = V_WIFI;
			} else if (k < 0) {
				memset(kbuf, 0, sizeof(kbuf));
				mode = V_WIFI;
			}
			wifi_tick();
		} else if (mode == V_WIFI) {
			bool typing = false;
			if (wifi_frame(a, &typing))
				mode = V_MENU;
			else if (typing)
				mode = V_KBD;
		} else if (mode == V_PAD2) {
			if (pad2_frame(a))
				mode = V_MENU;
		} else if (mode == V_LISTS) {
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
				} else if (msel == M_WIFI) {
					wifi_open();
					mode = V_WIFI;
				} else if (msel == M_PAD2) {
					pad2_start();
					mode = V_PAD2;
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
			} else if ((a == A_OK || a == A_START) && nview) {
				rc = launch();
				message_screen("Carregando…");
				break;
			}
		}
		/* a imagem só carrega quando a seleção para um pouco */
		if (nview && snap_for != view[sel] && SDL_GetTicks() - sel_changed_at > 120)
			load_media(view[sel]);
		if (mode == V_KBD) {
			char t[96];
			snprintf(t, sizeof(t), "Senha de %s", wtarget);
			draw_kbd(t);
		} else if (mode == V_WIFI)
			draw_wifi();
		else if (mode == V_PAD2)
			draw_pad2();
		else if (mode == V_LISTS)
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
