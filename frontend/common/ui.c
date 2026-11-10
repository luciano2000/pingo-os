/*
 * Desenho da interface: texto (SDL2_ttf), retângulos arredondados e ícones.
 *
 * Os ícones e os cantos arredondados são rasterizados uma vez, em branco,
 * com antisserrilhado (4x4 amostras por pixel), e tingidos com color mod.
 *
 * Copyright (C) 2026 Pingo OS by Retrogamer Brasil
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "ui.h"
#include "osd.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL2/SDL_ttf.h>

#define FONT_DIR "/usr/share/fonts/inter"

static SDL_Window *window;
static SDL_Renderer *ren;
static TTF_Font *fonts[F_COUNT_N];

/* ---- cache de textos ------------------------------------------------- */

#define TEXT_CACHE 256

static struct text_entry {
	enum font_id font;
	char *str;
	SDL_Texture *tex;
	int w, h;
	unsigned long used;
} cache[TEXT_CACHE];
static unsigned long tick;

static struct text_entry *text_get(enum font_id f, const char *s)
{
	struct text_entry *slot = &cache[0];

	tick++;
	for (int i = 0; i < TEXT_CACHE; i++) {
		struct text_entry *e = &cache[i];
		if (e->str && e->font == f && !strcmp(e->str, s)) {
			e->used = tick;
			return e;
		}
		if (e->used < slot->used)
			slot = e;
	}

	/* substitui o menos usado */
	if (slot->tex)
		SDL_DestroyTexture(slot->tex);
	free(slot->str);
	memset(slot, 0, sizeof(*slot));

	SDL_Color white = { 255, 255, 255, 255 };
	SDL_Surface *surf = TTF_RenderUTF8_Blended(fonts[f], s, white);
	if (!surf)
		return NULL;
	slot->tex = SDL_CreateTextureFromSurface(ren, surf);
	slot->w = surf->w;
	slot->h = surf->h;
	SDL_FreeSurface(surf);
	slot->font = f;
	slot->str = strdup(s);
	slot->used = tick;
	return slot;
}

int ui_text_width(enum font_id f, const char *s)
{
	int w = 0;
	if (*s)
		TTF_SizeUTF8(fonts[f], s, &w, NULL);
	return w;
}

/* Corta no limite de caractere UTF-8 e põe reticências */
static char *ellipsize(enum font_id f, const char *s, int max_w)
{
	size_t m = strlen(s);
	char *buf = malloc(m + 4);

	while (m > 0) {
		/* recua um caractere UTF-8 */
		do
			m--;
		while (m > 0 && ((unsigned char)s[m] & 0xC0) == 0x80);
		size_t k = m;
		while (k > 0 && s[k - 1] == ' ')
			k--;
		memcpy(buf, s, k);
		strcpy(buf + k, "…");
		if (ui_text_width(f, buf) <= max_w)
			return buf;
	}
	strcpy(buf, "…");
	return buf;
}

int ui_text(enum font_id f, const char *s, int x, int y_center, Uint32 rgb, int align, int max_w)
{
	char *cut = NULL;

	if (!s || !*s)
		return 0;
	if (max_w > 0 && ui_text_width(f, s) > max_w)
		s = cut = ellipsize(f, s, max_w);

	struct text_entry *e = text_get(f, s);
	free(cut);
	if (!e || !e->tex)
		return 0;

	SDL_Rect dst = { x, y_center - e->h / 2, e->w, e->h };
	if (align == 1)
		dst.x = x - e->w;
	else if (align == 2)
		dst.x = x - e->w / 2;
	SDL_SetTextureColorMod(e->tex, (rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255);
	SDL_RenderCopy(ren, e->tex, NULL, &dst);
	return e->w;
}

int ui_text_wrap(enum font_id f, const char *s, int x_center, int y_top, int max_w, int line_h,
		 Uint32 rgb)
{
	char line[512];
	int y = y_top + line_h / 2;
	const char *p = s;

	while (*p) {
		/* junta palavras enquanto couber */
		size_t len = 0, best = 0;
		for (;;) {
			const char *w = p + len;
			while (*w == ' ')
				w++;
			const char *e = w;
			while (*e && *e != ' ')
				e++;
			size_t cand = (size_t)(e - p);
			if (cand >= sizeof(line))
				break;
			memcpy(line, p, cand);
			line[cand] = 0;
			if (best && ui_text_width(f, line) > max_w)
				break;
			best = cand;
			if (!*e)
				break;
			len = cand;
		}
		if (!best) /* palavra maior que a linha: corta */
			best = strlen(p) < sizeof(line) - 1 ? strlen(p) : sizeof(line) - 1;
		memcpy(line, p, best);
		line[best] = 0;
		ui_text(f, line, x_center, y, rgb, 2, max_w);
		y += line_h;
		p += best;
		while (*p == ' ')
			p++;
	}
	return y - line_h / 2 - y_top;
}

/* ---- rasterização com antisserrilhado -------------------------------- */

typedef bool (*shape_fn)(float x, float y, const void *arg);

static SDL_Texture *raster(int w, int h, shape_fn inside, const void *arg)
{
	SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
	Uint32 *px = s->pixels;

	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			int hits = 0;
			for (int sy = 0; sy < 4; sy++)
				for (int sx = 0; sx < 4; sx++)
					hits += inside(x + (sx + 0.5f) / 4, y + (sy + 0.5f) / 4, arg);
			Uint32 a = (Uint32)(hits * 255 / 16);
			px[y * (s->pitch / 4) + x] = (a << 24) | 0xFFFFFF;
		}
	SDL_Texture *t = SDL_CreateTextureFromSurface(ren, s);
	SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
	SDL_FreeSurface(s);
	return t;
}

static float sd_round_rect(float px, float py, float x, float y, float w, float h, float r)
{
	float cx = x + w / 2, cy = y + h / 2;
	float qx = fabsf(px - cx) - (w / 2 - r), qy = fabsf(py - cy) - (h / 2 - r);
	float ox = fmaxf(qx, 0), oy = fmaxf(qy, 0);
	return sqrtf(ox * ox + oy * oy) + fminf(fmaxf(qx, qy), 0) - r;
}

/* disco de raio r (cantos dos retângulos arredondados) */
static bool in_disc(float x, float y, const void *arg)
{
	float r = *(const float *)arg;
	return (x - r) * (x - r) + (y - r) * (y - r) <= r * r;
}

/* Arco de traço com pontas redondas, acima da corda (ícone de Wi-Fi) */
struct arc { float x0, x1, yc, r; };

static bool on_arc(float x, float y, const struct arc *a, float half)
{
	float cx = (a->x0 + a->x1) / 2, r = a->r;
	float cy = a->yc + sqrtf(r * r - (cx - a->x0) * (cx - a->x0));
	float d = sqrtf((x - cx) * (x - cx) + (y - cy) * (y - cy));
	if (fabsf(d - r) <= half && y <= a->yc)
		return true;
	float e0 = (x - a->x0) * (x - a->x0) + (y - a->yc) * (y - a->yc);
	float e1 = (x - a->x1) * (x - a->x1) + (y - a->yc) * (y - a->yc);
	return e0 <= half * half || e1 <= half * half;
}

/* Mesmo desenho do mockup (SVG 18x14) */
static bool in_wifi(float x, float y, const void *arg)
{
	static const struct arc outer = { 1, 17, 4.5f, 12 }, inner = { 4, 14, 8, 7.5f };
	(void)arg;
	if (on_arc(x, y, &outer, 0.9f) || on_arc(x, y, &inner, 0.9f))
		return true;
	return (x - 9) * (x - 9) + (y - 11.5f) * (y - 11.5f) <= 1.0f;
}

/* Contorno da bateria (SVG 26x14) sem o preenchimento */
static bool in_battery(float x, float y, const void *arg)
{
	(void)arg;
	float d = sd_round_rect(x, y, 1, 1, 21, 12, 3);
	if (fabsf(d) <= 0.8f)
		return true;
	return sd_round_rect(x, y, 23.5f, 5, 1.8f, 4, 0.9f) <= 0;
}

static SDL_Texture *corner_tex;
static float corner_r;
static SDL_Texture *wifi_tex, *battery_tex, *chevron_tex, *dot_fill_tex, *dot_ring_tex;

void ui_rect(int x, int y, int w, int h, Uint32 rgb)
{
	SDL_Rect r = { x, y, w, h };
	SDL_SetRenderDrawColor(ren, (rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255, 255);
	SDL_RenderFillRect(ren, &r);
}

/* cantos já desenhados, por raio (as telas usam poucos raios diferentes) */
static SDL_Texture *corner_cache[4];
static float corner_cache_r[4];
static int corner_next;

void ui_round_rect(int x, int y, int w, int h, int r, Uint32 rgb)
{
	if (r > h / 2)
		r = h / 2; /* pílula: cantos totalmente redondos */
	if (r > w / 2)
		r = w / 2;
	if (!corner_tex || corner_r != r) {
		corner_tex = NULL;
		for (int i = 0; i < 4; i++)
			if (corner_cache[i] && corner_cache_r[i] == r)
				corner_tex = corner_cache[i];
		if (!corner_tex) {
			int i = corner_next;
			corner_next = (corner_next + 1) % 4;
			if (corner_cache[i])
				SDL_DestroyTexture(corner_cache[i]);
			corner_cache_r[i] = (float)r;
			corner_cache[i] = raster(2 * r, 2 * r, in_disc, &corner_cache_r[i]);
			corner_tex = corner_cache[i];
		}
		corner_r = (float)r;
	}
	SDL_SetTextureColorMod(corner_tex, (rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255);

	SDL_Rect src, dst;
	for (int i = 0; i < 4; i++) {
		int sx = (i & 1) ? r : 0, sy = (i & 2) ? r : 0;
		src = (SDL_Rect){ sx, sy, r, r };
		dst = (SDL_Rect){ (i & 1) ? x + w - r : x, (i & 2) ? y + h - r : y, r, r };
		SDL_RenderCopy(ren, corner_tex, &src, &dst);
	}
	ui_rect(x + r, y, w - 2 * r, h, rgb);
	ui_rect(x, y + r, r, h - 2 * r, rgb);
	ui_rect(x + w - r, y + r, r, h - 2 * r, rgb);
}

static void draw_tex(SDL_Texture *t, int x, int y, int w, int h, Uint32 rgb)
{
	SDL_Rect dst = { x, y, w, h };
	SDL_SetTextureColorMod(t, (rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255);
	SDL_RenderCopy(ren, t, NULL, &dst);
}

/* "›" do mockup (SVG 16x16: M6 3l5 5-5 5, traço 2) */
static bool in_chevron(float x, float y, const void *arg)
{
	(void)arg;
	/* distância aos dois segmentos (6,3)-(11,8) e (11,8)-(6,13) */
	float best = 1e9f;
	const float seg[2][4] = { { 6, 3, 11, 8 }, { 11, 8, 6, 13 } };
	for (int i = 0; i < 2; i++) {
		float ax = seg[i][0], ay = seg[i][1], bx = seg[i][2], by = seg[i][3];
		float vx = bx - ax, vy = by - ay, wx = x - ax, wy = y - ay;
		float t = (wx * vx + wy * vy) / (vx * vx + vy * vy);
		t = t < 0 ? 0 : (t > 1 ? 1 : t);
		float dx = x - (ax + t * vx), dy = y - (ay + t * vy);
		float d = dx * dx + dy * dy;
		if (d < best)
			best = d;
	}
	return best <= 1.0f;
}

static bool in_dot(float x, float y, const void *arg)
{
	(void)arg;
	return (x - 5) * (x - 5) + (y - 5) * (y - 5) <= 25;
}

/* contorno de 1,5 px, como no mockup */
static bool in_ring(float x, float y, const void *arg)
{
	(void)arg;
	float d = sqrtf((x - 5) * (x - 5) + (y - 5) * (y - 5));
	return d <= 5 && d >= 3.5f;
}

void ui_dot(int x, int y_center, bool filled, Uint32 rgb)
{
	if (!dot_fill_tex) {
		dot_fill_tex = raster(10, 10, in_dot, NULL);
		dot_ring_tex = raster(10, 10, in_ring, NULL);
	}
	draw_tex(filled ? dot_fill_tex : dot_ring_tex, x, y_center - 5, 10, 10, rgb);
}

void ui_icon_chevron(int x, int y_center, Uint32 rgb)
{
	if (!chevron_tex)
		chevron_tex = raster(16, 16, in_chevron, NULL);
	draw_tex(chevron_tex, x, y_center - 8, 16, 16, rgb);
}

int ui_icon_wifi(int x, int y_center, Uint32 rgb)
{
	draw_tex(wifi_tex, x, y_center - 7, 18, 14, rgb);
	return 18;
}

/* Cor da carga: verde (60 % ou mais), amarelo (40 a 59), laranja (20 a 39)
 * e vermelho (abaixo de 20) */
static Uint32 battery_color(int percent)
{
	return percent >= 60 ? 0x3DBE5A : percent >= 40 ? 0xF2C230 : percent >= 20 ? 0xF08A24
									  : 0xE5483B;
}

int ui_icon_battery(int x, int y_center, int percent, bool charging, Uint32 rgb)
{
	int y = y_center - 7;
	if (percent < 0)
		percent = 0;
	if (percent > 100)
		percent = 100;
	Uint32 fill = battery_color(percent);
	/* carregando: o contorno também fica na cor da carga */
	draw_tex(battery_tex, x, y, 26, 14, charging ? fill : rgb);
	/* área interna do mockup: x 3,5 a 19,5; y 3,5 a 10,5 */
	int fw = (16 * percent + 50) / 100;
	if (fw < 2 && percent > 0)
		fw = 2;
	ui_rect(x + 4, y + 4, fw, 6, fill);
	return 26;
}

/* ---- ciclo ----------------------------------------------------------- */

static bool attached; /* renderer de outro programa: não destruir */

static bool load_assets(void)
{
	/* Listas em Quicksand (fonte variável: index = instância << 16, na ordem
	 * do arquivo: 1 Light, 2 Regular, 3 Medium, 4 SemiBold, 5 Bold). O resto
	 * em Inter, que lê melhor nos tamanhos pequenos. */
	static const struct { const char *file; int size; long index; } spec[F_COUNT_N] = {
		[F_BAR]      = { "Inter-Medium.ttf",   15 },
		[F_BAR_BOLD] = { "Inter-SemiBold.ttf", 15 },
		[F_ITEM]     = { "../quicksand/Quicksand.ttf", 24, 4L << 16 },
		[F_ITEM_SEL] = { "../quicksand/Quicksand.ttf", 24, 5L << 16 },
		[F_COUNT]    = { "Inter-Medium.ttf",   16 },
		[F_SMALL]    = { "Inter-Medium.ttf",   14 },
		[F_TITLE]    = { "Inter-SemiBold.ttf", 23 },
		[F_BODY]     = { "Inter-Regular.ttf",  21 },
		[F_BIG]      = { "Inter-SemiBold.ttf", 28 },
	};

	if (!TTF_WasInit() && TTF_Init() < 0) {
		fprintf(stderr, "pingo-ui: TTF: %s\n", TTF_GetError());
		return false;
	}
	/* PINGO_FONT_DIR: para testar sem regravar o sistema */
	const char *font_dir = getenv("PINGO_FONT_DIR");
	if (!font_dir || !*font_dir)
		font_dir = FONT_DIR;
	for (int i = 0; i < F_COUNT_N; i++) {
		char path[512];
		snprintf(path, sizeof(path), "%s/%s", font_dir, spec[i].file);
		fonts[i] = TTF_OpenFontIndex(path, spec[i].size, spec[i].index);
		if (!fonts[i]) {
			fprintf(stderr, "pingo-launcher: fonte %s: %s\n", path, TTF_GetError());
			return false;
		}
		TTF_SetFontHinting(fonts[i], TTF_HINTING_LIGHT);
	}

	wifi_tex = raster(18, 14, in_wifi, NULL);
	battery_tex = raster(26, 14, in_battery, NULL);
	return true;
}

bool ui_attach(SDL_Renderer *renderer)
{
	ren = renderer;
	attached = true;
	SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
	return load_assets();
}

bool ui_init(void)
{
	/* Sem isto o SDL transforma o SIGTERM num evento que o launcher não lê,
	 * e o processo não fecha (desligamento, fim da mensagem de boot) */
	SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
	if (SDL_Init(SDL_INIT_VIDEO) < 0) {
		fprintf(stderr, "pingo-launcher: SDL: %s\n", SDL_GetError());
		return false;
	}
	SDL_ShowCursor(SDL_DISABLE);
	SDL_SetHint(SDL_HINT_RENDER_DRIVER, "opengles2");
	window = SDL_CreateWindow("pingo-launcher", 0, 0, 0, 0, SDL_WINDOW_FULLSCREEN_DESKTOP);
	ren = window ? SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC)
		     : NULL;
	if (!ren) {
		fprintf(stderr, "pingo-launcher: vídeo: %s\n", SDL_GetError());
		return false;
	}
	int w, h;
	SDL_GetRendererOutputSize(ren, &w, &h);
	if (w != UI_W || h != UI_H)
		SDL_RenderSetLogicalSize(ren, UI_W, UI_H);
	SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
	return load_assets();
}

static void free_assets(void)
{
	osd_quit();
	for (int i = 0; i < TEXT_CACHE; i++) {
		if (cache[i].tex)
			SDL_DestroyTexture(cache[i].tex);
		free(cache[i].str);
	}
	memset(cache, 0, sizeof(cache));
	for (int i = 0; i < F_COUNT_N; i++)
		if (fonts[i]) {
			TTF_CloseFont(fonts[i]);
			fonts[i] = NULL;
		}
	for (int i = 0; i < 4; i++)
		if (corner_cache[i]) {
			SDL_DestroyTexture(corner_cache[i]);
			corner_cache[i] = NULL;
		}
	corner_tex = NULL; /* era um dos do cache */
	SDL_Texture **tex[] = { &wifi_tex, &battery_tex, &chevron_tex, &dot_fill_tex,
				&dot_ring_tex };
	for (unsigned i = 0; i < sizeof(tex) / sizeof(tex[0]); i++)
		if (*tex[i]) {
			SDL_DestroyTexture(*tex[i]);
			*tex[i] = NULL;
		}
	corner_r = 0;
	TTF_Quit();
}

void ui_detach(void)
{
	free_assets();
	ren = NULL;
	attached = false;
}

void ui_quit(void)
{
	free_assets();
	SDL_Quit();
}

SDL_Renderer *ui_renderer(void)
{
	return ren;
}

void ui_clear(void)
{
	ui_clear_rgb(COL_BG);
}

void ui_clear_rgb(Uint32 rgb)
{
	SDL_SetRenderDrawColor(ren, (rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255, 255);
	SDL_RenderClear(ren);
}

void ui_present(void)
{
	ui_screenshot_check(ren);
	SDL_RenderPresent(ren);
}

/* ---- print da tela ------------------------------------------------------ */

#include <signal.h>
#include <sys/stat.h>
#include <time.h>

static volatile sig_atomic_t shot_wanted;

static void on_usr1(int sig)
{
	(void)sig;
	shot_wanted = 1;
}

int ui_screenshot_pending(void)
{
	return shot_wanted;
}

void ui_screenshot_init(void)
{
	signal(SIGUSR1, on_usr1);
}

void ui_screenshot_check(SDL_Renderer *r)
{
	if (!shot_wanted || !r)
		return;
	shot_wanted = 0;
	int w, h;
	if (SDL_GetRendererOutputSize(r, &w, &h) < 0)
		return;
	SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
	if (!s)
		return;
	if (SDL_RenderReadPixels(r, NULL, SDL_PIXELFORMAT_ARGB8888, s->pixels, s->pitch) == 0) {
		char path[128], stamp[32];
		time_t t = time(NULL);
		struct tm tm;
		localtime_r(&t, &tm);
		strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);
		mkdir("/mnt/sd1/Screenshots", 0755);
		snprintf(path, sizeof(path), "/mnt/sd1/Screenshots/pingo-%s.bmp", stamp);
		if (SDL_SaveBMP(s, path) == 0)
			fprintf(stderr, "print: %s\n", path);
	}
	SDL_FreeSurface(s);
}
