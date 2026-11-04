/*
 * Volume e brilho (Vol± e MENU+Vol±) com sobreposição na tela.
 *
 * Volume: controle "Line Out" do codec (0-31, vale para alto-falante e
 * fone), em 20 passos; o passo 0 desliga a saída. Brilho: backlight PWM
 * em 10 passos com curva perceptual e mínimo acima de zero.
 * Os valores ficam em /mnt/sd1/Config/sistema.cfg.
 *
 * Copyright (C) 2026 Pingo OS by Retrogamer Brasil
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "osd.h"

#include <dirent.h>
#include <linux/input.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <alsa/asoundlib.h>

#define CFG_FILE   "/mnt/sd1/Config/sistema.cfg"
#define MIXER_CTL  "Line Out"
#define SHOW_MS    1500

/* % do máximo do backlight em cada passo (1..10) */
static const int bright_pct[BRIGHT_STEPS + 1] = { 0, 4, 8, 14, 22, 32, 44, 58, 72, 86, 100 };

static int volume = 13, bright = 6;
static bool menu_down, menu_combo, menu_tap;
static enum { SHOW_NONE, SHOW_VOLUME, SHOW_BRIGHT } shown;
static Uint32 shown_until;
static bool was_visible, changed;

/* ---- hardware -------------------------------------------------------- */

static void apply_volume(void)
{
	snd_mixer_t *m;
	snd_mixer_selem_id_t *sid;

	if (snd_mixer_open(&m, 0) < 0)
		return;
	if (snd_mixer_attach(m, "default") < 0 || snd_mixer_selem_register(m, NULL, NULL) < 0 ||
	    snd_mixer_load(m) < 0) {
		snd_mixer_close(m);
		return;
	}
	snd_mixer_selem_id_alloca(&sid);
	snd_mixer_selem_id_set_name(sid, MIXER_CTL);
	snd_mixer_elem_t *e = snd_mixer_find_selem(m, sid);
	if (e) {
		long lo, hi;
		snd_mixer_selem_get_playback_volume_range(e, &lo, &hi);
		long v = lo + (hi - lo) * volume / VOLUME_STEPS;
		snd_mixer_selem_set_playback_volume_all(e, v);
		if (snd_mixer_selem_has_playback_switch(e))
			snd_mixer_selem_set_playback_switch_all(e, volume > 0);
	}
	snd_mixer_close(m);
}

static char backlight[256];

static void find_backlight(void)
{
	DIR *d = opendir("/sys/class/backlight");
	struct dirent *e;
	if (!d)
		return;
	while ((e = readdir(d)))
		if (e->d_name[0] != '.') {
			snprintf(backlight, sizeof(backlight), "/sys/class/backlight/%s", e->d_name);
			break;
		}
	closedir(d);
}

static void apply_bright(void)
{
	char path[300];
	FILE *f;
	int max = 0;

	if (!backlight[0])
		return;
	snprintf(path, sizeof(path), "%s/max_brightness", backlight);
	if ((f = fopen(path, "r"))) {
		if (fscanf(f, "%d", &max) != 1)
			max = 0;
		fclose(f);
	}
	if (max <= 0)
		return;
	int v = max * bright_pct[bright] / 100;
	if (v < 1)
		v = 1;
	snprintf(path, sizeof(path), "%s/brightness", backlight);
	if ((f = fopen(path, "w"))) {
		fprintf(f, "%d\n", v);
		fclose(f);
	}
}

void osd_screen(bool on)
{
	char path[300];
	FILE *f;

	if (!backlight[0])
		find_backlight();
	snprintf(path, sizeof(path), "%s/bl_power", backlight);
	if ((f = fopen(path, "w"))) {
		fprintf(f, "%d\n", on ? 0 : 4); /* FB_BLANK_UNBLANK / POWERDOWN */
		fclose(f);
	}
}

/* ---- configuração ---------------------------------------------------- */

static void cfg_load(void)
{
	FILE *f = fopen(CFG_FILE, "r");
	char line[128];
	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		int v;
		if (sscanf(line, "volume=%d", &v) == 1 && v >= 0 && v <= VOLUME_STEPS)
			volume = v;
		else if (sscanf(line, "brilho=%d", &v) == 1 && v >= 1 && v <= BRIGHT_STEPS)
			bright = v;
	}
	fclose(f);
}

static void cfg_save(void)
{
	FILE *f = fopen(CFG_FILE ".tmp", "w");
	if (!f)
		return;
	fprintf(f, "volume=%d\nbrilho=%d\n", volume, bright);
	fflush(f);
	fsync(fileno(f));
	fclose(f);
	rename(CFG_FILE ".tmp", CFG_FILE);
}

void osd_init(void)
{
	find_backlight();
	osd_screen(true); /* nunca começa com a tela apagada */
	cfg_load();
	apply_volume();
	apply_bright();
}

/* ---- teclas ---------------------------------------------------------- */

bool osd_key(int code, int value)
{
	if (code == BTN_MODE) {
		if (value == 1) {
			menu_down = true;
			menu_combo = false;
		} else if (value == 0 && menu_down) {
			menu_down = false;
			if (!menu_combo)
				menu_tap = true;
		}
		return false; /* quem chamou decide o que fazer com o MENU */
	}
	if (code != KEY_VOLUMEUP && code != KEY_VOLUMEDOWN)
		return false;
	if (value == 0)
		return true;

	int d = code == KEY_VOLUMEUP ? 1 : -1;
	if (menu_down) {
		menu_combo = true;
		bright += d;
		if (bright < 1)
			bright = 1;
		if (bright > BRIGHT_STEPS)
			bright = BRIGHT_STEPS;
		apply_bright();
		shown = SHOW_BRIGHT;
	} else {
		volume += d;
		if (volume < 0)
			volume = 0;
		if (volume > VOLUME_STEPS)
			volume = VOLUME_STEPS;
		apply_volume();
		shown = SHOW_VOLUME;
	}
	cfg_save();
	shown_until = SDL_GetTicks() + SHOW_MS;
	changed = true;
	return true;
}

bool osd_menu_tapped(void)
{
	bool t = menu_tap;
	menu_tap = false;
	return t;
}

bool osd_visible(void)
{
	bool v = shown != SHOW_NONE && SDL_GetTicks() < shown_until;
	if (!v)
		shown = SHOW_NONE;
	if (v != was_visible) {
		was_visible = v;
		changed = true;
	}
	return v;
}

bool osd_changed(void)
{
	osd_visible();
	bool c = changed;
	changed = false;
	return c;
}

/* ---- desenho --------------------------------------------------------- */

typedef bool (*shape_fn)(float x, float y);

static SDL_Texture *raster(SDL_Renderer *ren, int w, int h, shape_fn inside)
{
	SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
	Uint32 *px = s->pixels;
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			int hits = 0;
			for (int sy = 0; sy < 4; sy++)
				for (int sx = 0; sx < 4; sx++)
					hits += inside(x + (sx + 0.5f) / 4, y + (sy + 0.5f) / 4);
			px[y * (s->pitch / 4) + x] = ((Uint32)(hits * 255 / 16) << 24) | 0xFFFFFF;
		}
	SDL_Texture *t = SDL_CreateTextureFromSurface(ren, s);
	SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
	SDL_FreeSurface(s);
	return t;
}

#define PILL_W 44
#define PILL_H 220
#define TRACK_W 8

static float sq(float v) { return v * v; }

/* meia-cápsula: pontas da barra (largura PILL_W) */
static bool in_cap(float x, float y)
{
	float r = PILL_W / 2.0f;
	return sq(x - r) + sq(y - r) <= r * r;
}

static bool in_track_cap(float x, float y)
{
	float r = TRACK_W / 2.0f;
	return sq(x - r) + sq(y - r) <= r * r;
}

/* Alto-falante 22x22: corpo, cone e duas ondas */
static bool in_speaker(float x, float y)
{
	if (x >= 2 && x <= 7 && y >= 8 && y <= 14)
		return true;
	if (x >= 7 && x <= 12 && fabsf(y - 11) <= 3 + (x - 7) * 1.0f)
		return true;
	float d = sqrtf(sq(x - 12) + sq(y - 11));
	if (x > 13 && fabsf(d - 4.5f) <= 0.9f && fabsf(y - 11) <= 4)
		return true;
	return x > 13 && fabsf(d - 8) <= 0.9f && fabsf(y - 11) <= 6.5f;
}

/* Alto-falante sem as ondas (volume zero) */
static bool in_speaker_mute(float x, float y)
{
	if (x >= 2 && x <= 7 && y >= 8 && y <= 14)
		return true;
	if (x >= 7 && x <= 12 && fabsf(y - 11) <= 3 + (x - 7) * 1.0f)
		return true;
	/* um "x" ao lado */
	float u = x - 17, v = y - 11;
	return fabsf(u) <= 3.5f && fabsf(v) <= 3.5f && (fabsf(u - v) <= 1.1f || fabsf(u + v) <= 1.1f);
}

/* Sol 22x22: disco e 8 raios */
static bool in_sun(float x, float y)
{
	float dx = x - 11, dy = y - 11, d = sqrtf(dx * dx + dy * dy);
	if (d <= 4.5f)
		return true;
	if (d < 6.5f || d > 10)
		return false;
	float a = atan2f(dy, dx) / (float)M_PI * 4; /* 8 setores */
	float frac = a - floorf(a + 0.5f);
	return fabsf(frac) * d * (float)M_PI / 4 <= 0.9f;
}

static SDL_Renderer *tex_ren;
static SDL_Texture *t_cap, *t_track, *t_speaker, *t_mute, *t_sun;

void osd_quit(void)
{
	SDL_Texture **all[] = { &t_cap, &t_track, &t_speaker, &t_mute, &t_sun };
	for (unsigned i = 0; i < sizeof(all) / sizeof(all[0]); i++)
		if (*all[i]) {
			SDL_DestroyTexture(*all[i]);
			*all[i] = NULL;
		}
	tex_ren = NULL;
}

static void make_textures(SDL_Renderer *ren)
{
	if (tex_ren == ren)
		return;
	osd_quit();
	tex_ren = ren;
	t_cap = raster(ren, PILL_W, PILL_W, in_cap);
	t_track = raster(ren, TRACK_W, TRACK_W, in_track_cap);
	t_speaker = raster(ren, 22, 22, in_speaker);
	t_mute = raster(ren, 22, 22, in_speaker_mute);
	t_sun = raster(ren, 22, 22, in_sun);
}

/* Cápsula vertical (pontas redondas) a partir da textura de um disco */
static void capsule(SDL_Renderer *ren, SDL_Texture *disc, int d, int x, int y, int h,
		    Uint8 r, Uint8 g, Uint8 b, Uint8 a)
{
	int rad = d / 2;
	if (h < d)
		h = d;
	SDL_SetTextureColorMod(disc, r, g, b);
	SDL_SetTextureAlphaMod(disc, a);
	SDL_Rect top_src = { 0, 0, d, rad }, bot_src = { 0, rad, d, d - rad };
	SDL_Rect top = { x, y, d, rad }, bot = { x, y + h - (d - rad), d, d - rad };
	SDL_RenderCopy(ren, disc, &top_src, &top);
	SDL_RenderCopy(ren, disc, &bot_src, &bot);
	SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
	SDL_SetRenderDrawColor(ren, r, g, b, a);
	SDL_Rect mid = { x, y + rad, d, h - d };
	SDL_RenderFillRect(ren, &mid);
}

void osd_draw(SDL_Renderer *ren, int w, int h)
{
	if (!osd_visible())
		return;
	make_textures(ren);

	int x = w - 20 - PILL_W, y = (h - PILL_H) / 2;
	/* fundo preto quase opaco: legível por cima de qualquer jogo */
	capsule(ren, t_cap, PILL_W, x, y, PILL_H, 0, 0, 0, 225);

	int level = shown == SHOW_BRIGHT ? bright : volume;
	int steps = shown == SHOW_BRIGHT ? BRIGHT_STEPS : VOLUME_STEPS;
	int tx = x + (PILL_W - TRACK_W) / 2, ty = y + 18, th = PILL_H - 18 - 52;
	capsule(ren, t_track, TRACK_W, tx, ty, th, 0x8C, 0x8C, 0x8C, 110);
	if (level > 0) {
		int fh = TRACK_W + (th - TRACK_W) * level / steps;
		capsule(ren, t_track, TRACK_W, tx, ty + th - fh, fh, 0xE8, 0xE8, 0xE8, 255);
	}

	SDL_Texture *icon = shown == SHOW_BRIGHT ? t_sun : (volume == 0 ? t_mute : t_speaker);
	SDL_Rect dst = { x + (PILL_W - 22) / 2, y + PILL_H - 22 - 15, 22, 22 };
	SDL_SetTextureColorMod(icon, 0xE8, 0xE8, 0xE8);
	SDL_SetTextureAlphaMod(icon, 255);
	SDL_RenderCopy(ren, icon, NULL, &dst);
}
