/*
 * pingo-retro: frontend libretro do Pingo OS.
 *
 * Uso: pingo-retro <core_libretro.so> <rom> [código do console]
 *
 * - Vídeo pelo SDL2 (KMS/DRM + GLES), escala Ajustada ou Inteira.
 * - Som pelo SDL2 (ALSA) a 48 kHz, reamostrado com controle dinâmico de
 *   taxa: a tela é 60 Hz e o Game Boy, por exemplo, roda a 59,73 Hz.
 * - Controles lidos direto do evdev do "H700 Gamepad".
 * - MENU (ao soltar) abre o menu do jogo: Continuar, Salvar, Carregar,
 *   Opções e Sair, com 4 slots de save state e prévia.
 * - SRAM em Saves/<console>/<jogo>.srm (nome do RetroArch), gravada sozinha
 *   quando muda, ao sair e antes de suspender.
 * - .zip de cartucho é aberto na memória (cores que pedem o caminho, como o
 *   FBNeo, recebem o .zip como está).
 * - Opções (escala e opções do core) por jogo ou por console.
 *
 * Copyright (C) 2026 Pingo OS by Retrogamer Brasil
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <dirent.h>
#include <execinfo.h>
#include <signal.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <SDL2/SDL.h>
#include <zip.h>

#include "fbcopy.h"
#include "libretro.h"
#include "osd.h"
#include "strings_pt.h"
#include "ui.h"

#define GAMEPAD_NAME "H700 Gamepad"
#define DATA_ROOT    "/mnt/sd1"
#define SLOTS        4
#define AUDIO_RATE   48000

/* ---- core ------------------------------------------------------------ */

static struct {
	void *handle;
	void (*init)(void);
	void (*deinit)(void);
	void (*get_system_info)(struct retro_system_info *);
	void (*get_system_av_info)(struct retro_system_av_info *);
	void (*set_environment)(retro_environment_t);
	void (*set_video_refresh)(retro_video_refresh_t);
	void (*set_audio_sample)(retro_audio_sample_t);
	void (*set_audio_sample_batch)(retro_audio_sample_batch_t);
	void (*set_input_poll)(retro_input_poll_t);
	void (*set_input_state)(retro_input_state_t);
	void (*run)(void);
	bool (*load_game)(const struct retro_game_info *);
	void (*unload_game)(void);
	size_t (*serialize_size)(void);
	bool (*serialize)(void *, size_t);
	bool (*unserialize)(const void *, size_t);
	void *(*get_memory_data)(unsigned);
	size_t (*get_memory_size)(unsigned);
} core;

#define LOAD_SYM(field, name)                                              \
	do {                                                                   \
		*(void **)&core.field = dlsym(core.handle, name);                  \
		if (!core.field) {                                                 \
			fprintf(stderr, "pingo-retro: símbolo %s ausente\n", name);    \
			return false;                                                  \
		}                                                                  \
	} while (0)

static bool core_open(const char *path)
{
	core.handle = dlopen(path, RTLD_LAZY);
	if (!core.handle) {
		fprintf(stderr, "pingo-retro: %s\n", dlerror());
		return false;
	}
	LOAD_SYM(init, "retro_init");
	LOAD_SYM(deinit, "retro_deinit");
	LOAD_SYM(get_system_info, "retro_get_system_info");
	LOAD_SYM(get_system_av_info, "retro_get_system_av_info");
	LOAD_SYM(set_environment, "retro_set_environment");
	LOAD_SYM(set_video_refresh, "retro_set_video_refresh");
	LOAD_SYM(set_audio_sample, "retro_set_audio_sample");
	LOAD_SYM(set_audio_sample_batch, "retro_set_audio_sample_batch");
	LOAD_SYM(set_input_poll, "retro_set_input_poll");
	LOAD_SYM(set_input_state, "retro_set_input_state");
	LOAD_SYM(run, "retro_run");
	LOAD_SYM(load_game, "retro_load_game");
	LOAD_SYM(unload_game, "retro_unload_game");
	LOAD_SYM(serialize_size, "retro_serialize_size");
	LOAD_SYM(serialize, "retro_serialize");
	LOAD_SYM(unserialize, "retro_unserialize");
	LOAD_SYM(get_memory_data, "retro_get_memory_data");
	LOAD_SYM(get_memory_size, "retro_get_memory_size");
	return true;
}

/* ---- arquivos -------------------------------------------------------- */

static char console_code[32] = "OUTROS";
static char game_base[512];   /* nome do arquivo sem extensão */
static char game_label[512];  /* nome limpo, para a tela */

static void mkdir_p(const char *path)
{
	char tmp[4096];
	snprintf(tmp, sizeof(tmp), "%s", path);
	for (char *p = tmp + 1; *p; p++)
		if (*p == '/') {
			*p = 0;
			mkdir(tmp, 0755);
			*p = '/';
		}
	mkdir(tmp, 0755);
}

/* Grava em arquivo temporário + fsync + rename: um desligamento no meio
 * nunca deixa um save pela metade */
static bool write_file(const char *path, const void *data, size_t len)
{
	char tmp[4200];
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	FILE *f = fopen(tmp, "wb");
	if (!f)
		return false;
	bool ok = fwrite(data, 1, len, f) == len && fflush(f) == 0 && fsync(fileno(f)) == 0;
	ok = fclose(f) == 0 && ok;
	if (!ok || rename(tmp, path) != 0) {
		unlink(tmp);
		return false;
	}
	return true;
}

static void *read_file(const char *path, size_t *size)
{
	FILE *f = fopen(path, "rb");
	void *buf = NULL;
	long len;

	if (!f)
		return NULL;
	if (fseek(f, 0, SEEK_END) == 0 && (len = ftell(f)) > 0 && fseek(f, 0, SEEK_SET) == 0) {
		buf = malloc(len);
		if (buf && fread(buf, 1, len, f) != (size_t)len) {
			free(buf);
			buf = NULL;
		}
		*size = len;
	}
	fclose(f);
	return buf;
}

static void data_path(char *out, size_t n, const char *dir, const char *suffix)
{
	snprintf(out, n, "%s/%s/%s/%s%s", DATA_ROOT, dir, console_code, game_base, suffix);
}

/* Nome limpo (sem extensão e sem "(USA)", "[!]"), como no launcher */
static void clean_label(const char *base, char *out, size_t n)
{
	size_t o = 0;
	int depth = 0;
	for (const char *p = base; *p && o + 1 < n; p++) {
		if (*p == '(' || *p == '[') {
			depth++;
			continue;
		}
		if ((*p == ')' || *p == ']') && depth > 0) {
			depth--;
			continue;
		}
		if (depth == 0 && !(*p == ' ' && (o == 0 || out[o - 1] == ' ')))
			out[o++] = *p;
	}
	while (o > 0 && out[o - 1] == ' ')
		o--;
	out[o] = 0;
	if (!o)
		snprintf(out, n, "%s", base);
}

/* ---- opções (escala + opções do core) -------------------------------- *
 * Padrão -> Config/<console>/console.cfg -> Config/<console>/<jogo>.cfg
 */

/* Remapeamento: bind[f] = botão do aparelho (código evdev) que aciona a
 * função f do core (RETRO_DEVICE_ID_JOYPAD_*). Padrão: o layout libretro. */
static int bind[16];
static int button_by_name(const char *name);
static const char *button_name(int code);
static int default_bind(int f);

enum { SCALE_FIT, SCALE_INT };
static int scale_mode = SCALE_FIT;

#define MAX_VARS 96
static struct var {
	char *key, *desc;
	char *values[32];
	int nvalues, cur;
	char *label; /* nome mostrado (lista curada); NULL: fica oculta */
} vars[MAX_VARS];
static int nvars;
static bool vars_dirty;

/* Opções do core mostradas no menu, na ordem da lista curada */
static int shown[MAX_VARS], nshown;
static char core_name[128];

#define CORE_OPTS_DIR "/usr/share/pingo/cores"

/* /usr/share/pingo/cores/<core>.opts: "chave=Nome mostrado". Sem o
 * arquivo, nenhuma opção do core aparece (só a Escala). */
static void vars_curate(void)
{
	char path[512], line[512];
	const char *dir = getenv("PINGO_CORE_OPTS_DIR");
	snprintf(path, sizeof(path), "%s/%s.opts", dir && *dir ? dir : CORE_OPTS_DIR, core_name);
	FILE *f = fopen(path, "r");
	nshown = 0;
	if (!f)
		return;
	while (fgets(line, sizeof(line), f) && nshown < MAX_VARS) {
		char *eq = strchr(line, '='), *nl = strchr(line, '\n');
		if (line[0] == '#' || !eq)
			continue;
		if (nl)
			*nl = 0;
		*eq = 0;
		/* "chave=Nome mostrado|valor padrão do Pingo" (o padrão é opcional) */
		char *def = strchr(eq + 1, '|');
		if (def)
			*def++ = 0;
		for (int i = 0; i < nvars; i++)
			if (!strcmp(vars[i].key, line) && !vars[i].label) {
				vars[i].label = strdup(eq + 1);
				shown[nshown++] = i;
				for (int k = 0; def && k < vars[i].nvalues; k++)
					if (!strcmp(vars[i].values[k], def))
						vars[i].cur = k;
			}
	}
	fclose(f);
}

static void var_set_value(const char *key, const char *value)
{
	if (!strcmp(key, "pingo.scale")) {
		scale_mode = !strcmp(value, "inteira") ? SCALE_INT : SCALE_FIT;
		return;
	}
	if (!strncmp(key, "pingo.bind.", 11)) {
		int f = atoi(key + 11), code = button_by_name(value);
		if (f >= 0 && f < 16 && code >= 0)
			bind[f] = code;
		return;
	}
	for (int i = 0; i < nvars; i++)
		if (!strcmp(vars[i].key, key))
			for (int k = 0; k < vars[i].nvalues; k++)
				if (!strcmp(vars[i].values[k], value)) {
					vars[i].cur = k;
					vars_dirty = true;
				}
}

static void cfg_path(char *out, size_t n, bool per_game)
{
	if (per_game)
		snprintf(out, n, "%s/Config/%s/%s.cfg", DATA_ROOT, console_code, game_base);
	else
		snprintf(out, n, "%s/Config/%s/console.cfg", DATA_ROOT, console_code);
}

static void cfg_load_file(const char *path)
{
	FILE *f = fopen(path, "r");
	char line[512];
	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		char *eq = strchr(line, '='), *nl = strchr(line, '\n');
		if (!eq || line[0] == '#')
			continue;
		if (nl)
			*nl = 0;
		*eq = 0;
		var_set_value(line, eq + 1);
	}
	fclose(f);
}

static void cfg_load_all(void)
{
	char p[4096];
	cfg_path(p, sizeof(p), false);
	cfg_load_file(p);
	cfg_path(p, sizeof(p), true);
	cfg_load_file(p);
}

static bool cfg_save(bool per_game)
{
	char p[4096], dir[4096];
	snprintf(dir, sizeof(dir), "%s/Config/%s", DATA_ROOT, console_code);
	mkdir_p(dir);
	cfg_path(p, sizeof(p), per_game);

	char buf[16384];
	size_t len = (size_t)snprintf(buf, sizeof(buf), "pingo.scale=%s\n",
				      scale_mode == SCALE_INT ? "inteira" : "ajustada");
	/* botões: só os que mudaram do padrão */
	for (int f = 0; f < 16 && len < sizeof(buf); f++) {
		if (bind[f] != default_bind(f) && bind[f] >= 0)
			len += (size_t)snprintf(buf + len, sizeof(buf) - len, "pingo.bind.%d=%s\n", f,
						button_name(bind[f]));
	}
	/* só as opções do menu: as ocultas seguem o padrão do core */
	for (int k = 0; k < nshown && len < sizeof(buf); k++) {
		struct var *v = &vars[shown[k]];
		len += (size_t)snprintf(buf + len, sizeof(buf) - len, "%s=%s\n", v->key,
					v->values[v->cur]);
	}
	if (len >= sizeof(buf))
		len = sizeof(buf) - 1;
	return write_file(p, buf, len);
}

/* Opções do core no formato antigo ("Descrição; a|b|c"): o padrão é o primeiro */
static void vars_define(const struct retro_variable *v)
{
	for (; v && v->key && nvars < MAX_VARS; v++) {
		struct var *x = &vars[nvars];
		const char *semi = strchr(v->value, ';');
		if (!semi)
			continue;
		x->key = strdup(v->key);
		x->desc = strndup(v->value, semi - v->value);
		const char *p = semi + 1;
		while (*p == ' ')
			p++;
		while (*p && x->nvalues < 32) {
			const char *bar = strchr(p, '|');
			size_t l = bar ? (size_t)(bar - p) : strlen(p);
			x->values[x->nvalues++] = strndup(p, l);
			p += l + (bar ? 1 : 0);
		}
		if (x->nvalues)
			nvars++;
	}
}

/* interface de troca de disco do core (jogos de vários discos) */
static struct retro_disk_control_callback disk;
static bool has_disk;

/* ---- vídeo ----------------------------------------------------------- */

static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Texture *texture;
static int tex_w, tex_h, screen_w, screen_h;
static Uint32 tex_format = SDL_PIXELFORMAT_RGB555;
static int tex_bpp = 2;
static float aspect;

/* cópia do último quadro (prévia dos save states) */
static void *frame_buf;
static int frame_w, frame_h, frame_pitch;

static void video_refresh(const void *data, unsigned width, unsigned height, size_t pitch)
{
	if (!data) /* quadro duplicado (GET_CAN_DUPE) */
		return;
	if (!texture || (int)width != tex_w || (int)height != tex_h) {
		if (texture)
			SDL_DestroyTexture(texture);
		texture = SDL_CreateTexture(renderer, tex_format, SDL_TEXTUREACCESS_STREAMING,
					    width, height);
		tex_w = width;
		tex_h = height;
	}
	SDL_UpdateTexture(texture, NULL, data, (int)pitch);

	if ((int)width != frame_w || (int)height != frame_h) {
		free(frame_buf);
		frame_pitch = width * tex_bpp;
		frame_buf = malloc((size_t)frame_pitch * height);
		frame_w = width;
		frame_h = height;
	}
	for (unsigned y = 0; frame_buf && y < height; y++)
		memcpy((char *)frame_buf + y * frame_pitch, (const char *)data + y * pitch, frame_pitch);
}

/* Retângulo do jogo na tela: proporção original sempre */
static SDL_Rect game_rect(void)
{
	float a = aspect > 0 ? aspect : (float)tex_w / tex_h;
	SDL_Rect r;
	if (scale_mode == SCALE_INT) {
		/* múltiplos exatos da altura; a largura segue a proporção */
		int s = SDL_min(screen_w / tex_w, screen_h / tex_h);
		if (s < 1)
			s = 1;
		r.h = tex_h * s;
		r.w = (int)(r.h * a + 0.5f);
		if (r.w > screen_w)
			r.w = tex_w * s;
	} else {
		r.w = screen_w;
		r.h = (int)(screen_w / a + 0.5f);
		if (r.h > screen_h) {
			r.h = screen_h;
			r.w = (int)(screen_h * a + 0.5f);
		}
	}
	r.x = (screen_w - r.w) / 2;
	r.y = (screen_h - r.h) / 2;
	return r;
}

static void draw_game(void)
{
	SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
	SDL_RenderClear(renderer);
	if (texture) {
		SDL_Rect dst = game_rect();
		SDL_RenderCopy(renderer, texture, NULL, &dst);
	}
}

/* ---- som: reamostragem com controle dinâmico de taxa ------------------ *
 * O core gera `core_rate` amostras por segundo de jogo. Rodamos o core no
 * ritmo da tela, então a taxa efetiva muda um pouco; a fila de som diz se
 * estamos adiantados ou atrasados e o passo da reamostragem corrige até
 * ±0,5 % (imperceptível), sem cortes nem estalos.
 */

static SDL_AudioDeviceID audio_dev;
static double core_rate, audio_step; /* amostras do core por amostra de saída */
static double audio_pos;             /* posição entre a amostra anterior e a atual */
static int16_t in_prev[2];
static Uint32 audio_target;          /* bytes na fila que queremos manter */

static size_t audio_batch(const int16_t *data, size_t frames)
{
	static int16_t out[8192 * 2];
	size_t o = 0;

	if (!audio_dev)
		return frames;

	Uint32 queued = SDL_GetQueuedAudioSize(audio_dev);
	if (queued > audio_target * 3) /* atraso grande (ex.: depois de um travamento) */
		return frames;
	double fill = (double)queued / audio_target;
	double adj = 1.0 + 0.005 * (fill - 1.0);
	if (adj < 0.995)
		adj = 0.995;
	if (adj > 1.005)
		adj = 1.005;
	double step = audio_step * adj;

	for (size_t i = 0; i < frames; i++) {
		const int16_t *cur = data + i * 2;
		while (audio_pos < 1.0 && o < 8192) {
			double t = audio_pos;
			out[o * 2] = (int16_t)(in_prev[0] + (cur[0] - in_prev[0]) * t);
			out[o * 2 + 1] = (int16_t)(in_prev[1] + (cur[1] - in_prev[1]) * t);
			o++;
			audio_pos += step;
		}
		audio_pos -= 1.0;
		in_prev[0] = cur[0];
		in_prev[1] = cur[1];
	}
	if (o)
		SDL_QueueAudio(audio_dev, out, (Uint32)(o * 4));
	return frames;
}

static void audio_sample(int16_t left, int16_t right)
{
	int16_t frame[2] = { left, right };
	audio_batch(frame, 1);
}

static void audio_pause(bool pause)
{
	if (!audio_dev)
		return;
	if (!pause)
		SDL_ClearQueuedAudio(audio_dev);
	SDL_PauseAudioDevice(audio_dev, pause);
}

/* ---- entrada (evdev) ------------------------------------------------- */

static int pad_fd = -1, vol_fd = -1, power_fd = -1;
static bool pad_state[16];


/* Nomes das funções vindos do core (SET_INPUT_DESCRIPTORS), porta 0 */
static char *desc_name[16];
static bool quit_requested, power_off_requested, suspend_requested, menu_requested;
static bool power_down;
static Uint32 power_down_at;

/* teclas apertadas neste quadro, para o menu */
static int pressed[16], npressed;

static int open_input(const char *want)
{
	DIR *dir = opendir("/dev/input");
	struct dirent *ent;
	char path[300], name[256];

	if (!dir)
		return -1;
	while ((ent = readdir(dir))) {
		if (strncmp(ent->d_name, "event", 5))
			continue;
		snprintf(path, sizeof(path), "/dev/input/%s", ent->d_name);
		int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0)
			continue;
		if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) >= 0 && !strcmp(name, want)) {
			closedir(dir);
			return fd;
		}
		close(fd);
	}
	closedir(dir);
	return -1;
}

/* Layout libretro: B embaixo, A à direita (igual aos botões do aparelho) */
static int map_key(int code)
{
	switch (code) {
	case BTN_SOUTH:      return RETRO_DEVICE_ID_JOYPAD_B;
	case BTN_EAST:       return RETRO_DEVICE_ID_JOYPAD_A;
	case BTN_WEST:       return RETRO_DEVICE_ID_JOYPAD_Y;
	case BTN_NORTH:      return RETRO_DEVICE_ID_JOYPAD_X;
	case BTN_SELECT:     return RETRO_DEVICE_ID_JOYPAD_SELECT;
	case BTN_START:      return RETRO_DEVICE_ID_JOYPAD_START;
	case BTN_DPAD_UP:    return RETRO_DEVICE_ID_JOYPAD_UP;
	case BTN_DPAD_DOWN:  return RETRO_DEVICE_ID_JOYPAD_DOWN;
	case BTN_DPAD_LEFT:  return RETRO_DEVICE_ID_JOYPAD_LEFT;
	case BTN_DPAD_RIGHT: return RETRO_DEVICE_ID_JOYPAD_RIGHT;
	case BTN_TL:         return RETRO_DEVICE_ID_JOYPAD_L;
	case BTN_TR:         return RETRO_DEVICE_ID_JOYPAD_R;
	case BTN_TL2:        return RETRO_DEVICE_ID_JOYPAD_L2;
	case BTN_TR2:        return RETRO_DEVICE_ID_JOYPAD_R2;
	case BTN_THUMBL:     return RETRO_DEVICE_ID_JOYPAD_L3;
	case BTN_THUMBR:     return RETRO_DEVICE_ID_JOYPAD_R3;
	default:             return -1;
	}
}

static const int physical_buttons[] = {
	BTN_SOUTH, BTN_EAST, BTN_WEST, BTN_NORTH, BTN_TL, BTN_TR, BTN_TL2, BTN_TR2,
	BTN_SELECT, BTN_START, BTN_THUMBL, BTN_THUMBR,
	BTN_DPAD_UP, BTN_DPAD_DOWN, BTN_DPAD_LEFT, BTN_DPAD_RIGHT,
};
#define N_PHYSICAL (int)(sizeof(physical_buttons) / sizeof(physical_buttons[0]))

static int default_bind(int f)
{
	for (int i = 0; i < N_PHYSICAL; i++)
		if (map_key(physical_buttons[i]) == f)
			return physical_buttons[i];
	return -1;
}

static void binds_default(void)
{
	for (int f = 0; f < 16; f++)
		bind[f] = -1;
	for (int i = 0; i < N_PHYSICAL; i++) {
		int f = map_key(physical_buttons[i]);
		if (f >= 0)
			bind[f] = physical_buttons[i];
	}
}

/* Nome do botão do aparelho, como está impresso nele */
static const char *button_name(int code)
{
	switch (code) {
	case BTN_SOUTH:      return "B";
	case BTN_EAST:       return "A";
	case BTN_WEST:       return "Y";
	case BTN_NORTH:      return "X";
	case BTN_TL:         return "L1";
	case BTN_TR:         return "R1";
	case BTN_TL2:        return "L2";
	case BTN_TR2:        return "R2";
	case BTN_SELECT:     return "Select";
	case BTN_START:      return "Start";
	case BTN_THUMBL:     return "L3";
	case BTN_THUMBR:     return "R3";
	case BTN_DPAD_UP:    return "↑";
	case BTN_DPAD_DOWN:  return "↓";
	case BTN_DPAD_LEFT:  return "←";
	case BTN_DPAD_RIGHT: return "→";
	default:             return "—";
	}
}

static int button_by_name(const char *name)
{
	for (int i = 0; i < N_PHYSICAL; i++)
		if (!strcmp(button_name(physical_buttons[i]), name))
			return physical_buttons[i];
	return -1;
}

/* Lê todos os dispositivos (uma vez por quadro, no jogo e no menu) */
static void poll_devices(void)
{
	struct input_event ev;

	npressed = 0;
	while (power_fd >= 0 && read(power_fd, &ev, sizeof(ev)) == sizeof(ev)) {
		if (ev.type != EV_KEY || ev.code != KEY_POWER)
			continue;
		if (ev.value == 1) {
			power_down = true;
			power_down_at = SDL_GetTicks();
		} else if (ev.value == 0 && power_down) {
			fprintf(stderr, "pingo-retro: power solto após %u ms: suspender\n",
				SDL_GetTicks() - power_down_at);
			power_down = false;
			suspend_requested = true; /* toque curto, ao soltar */
		}
	}
	if (power_down && SDL_GetTicks() - power_down_at >= 2000) {
		fprintf(stderr, "pingo-retro: power segurado 2000 ms: desligar\n");
		power_off_requested = quit_requested = true;
		power_down = false;
	}
	while (vol_fd >= 0 && read(vol_fd, &ev, sizeof(ev)) == sizeof(ev))
		if (ev.type == EV_KEY)
			osd_key(ev.code, ev.value);
	while (pad_fd >= 0 && read(pad_fd, &ev, sizeof(ev)) == sizeof(ev)) {
		if (ev.type != EV_KEY)
			continue;
		if (osd_key(ev.code, ev.value))
			continue;
		if (ev.code == BTN_MODE) {
			/* MENU ao soltar, sem Vol± junto */
			if (osd_menu_tapped())
				menu_requested = true;
			continue;
		}
		if (ev.value == 1 && npressed < 16)
			pressed[npressed++] = ev.code;
		/* um botão pode acionar uma função (remapeamento) */
		for (int f = 0; f < 16; f++)
			if (bind[f] == (int)ev.code)
				pad_state[f] = ev.value != 0;
	}
}

/* Sequência de botões apertada pelo próprio frontend ("Virar o disquete"
 * do FDS: o FCEUmm troca o lado pelos botões R e L). Cada passo: botão e
 * quantos quadros dura (botão -1: nenhum). */
static const struct { int id, frames; } fds_flip[] = {
	{ RETRO_DEVICE_ID_JOYPAD_R, 6 }, { -1, 30 },   /* tira o disquete */
	{ RETRO_DEVICE_ID_JOYPAD_L, 6 }, { -1, 30 },   /* vira o lado */
	{ RETRO_DEVICE_ID_JOYPAD_R, 6 }, { -1, 6 },    /* coloca de novo */
};
static int macro_step = -1, macro_left;

static void input_poll(void)
{
	/* os dispositivos são lidos no laço principal; aqui só a sequência */
	if (macro_step < 0)
		return;
	if (macro_left-- <= 0) {
		macro_step++;
		if (macro_step >= (int)(sizeof(fds_flip) / sizeof(fds_flip[0]))) {
			macro_step = -1;
			memset(pad_state, 0, sizeof(pad_state));
			return;
		}
		macro_left = fds_flip[macro_step].frames;
	}
	memset(pad_state, 0, sizeof(pad_state));
	if (fds_flip[macro_step].id >= 0)
		pad_state[fds_flip[macro_step].id] = true;
}

static void macro_start(void)
{
	macro_step = 0;
	macro_left = fds_flip[0].frames;
}

static bool is_fds;

static int16_t input_state(unsigned port, unsigned device, unsigned index, unsigned id)
{
	(void)index;
	if (port != 0 || device != RETRO_DEVICE_JOYPAD)
		return 0;
	if (id == RETRO_DEVICE_ID_JOYPAD_MASK) {
		int16_t mask = 0;
		for (unsigned i = 0; i < 16; i++)
			if (pad_state[i])
				mask |= (int16_t)(1 << i);
		return mask;
	}
	return id < 16 && pad_state[id];
}

/* ---- ambiente -------------------------------------------------------- */

static void core_log(enum retro_log_level level, const char *fmt, ...)
{
	static const char *names[] = { "debug", "info", "aviso", "erro" };
	va_list ap;

	if (level == RETRO_LOG_DEBUG)
		return;
	fprintf(stderr, "[core %s] ", names[level & 3]);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}

static char system_dir[512], save_dir[512];

static bool environment(unsigned cmd, void *data)
{
	switch (cmd) {
	case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
		if (!data)
			return false;
		((struct retro_log_callback *)data)->log = core_log;
		return true;
	case RETRO_ENVIRONMENT_GET_CAN_DUPE:
		*(bool *)data = true;
		return true;
	case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
		enum retro_pixel_format fmt = *(enum retro_pixel_format *)data;
		switch (fmt) {
		case RETRO_PIXEL_FORMAT_RGB565:   tex_format = SDL_PIXELFORMAT_RGB565; tex_bpp = 2; break;
		case RETRO_PIXEL_FORMAT_XRGB8888: tex_format = SDL_PIXELFORMAT_RGB888; tex_bpp = 4; break;
		case RETRO_PIXEL_FORMAT_0RGB1555: tex_format = SDL_PIXELFORMAT_RGB555; tex_bpp = 2; break;
		default: return false;
		}
		return true;
	}
	case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
		*(const char **)data = system_dir;
		return true;
	case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
		*(const char **)data = save_dir;
		return true;
	case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
		*(unsigned *)data = 0; /* opções no formato antigo (SET_VARIABLES) */
		return true;
	case RETRO_ENVIRONMENT_SET_VARIABLES:
		if (!data)
			return false;
		vars_define((const struct retro_variable *)data);
		vars_curate();
		cfg_load_all(); /* valores salvos por console/jogo */
		return true;
	case RETRO_ENVIRONMENT_GET_VARIABLE: {
		struct retro_variable *v = data;
		if (!v || !v->key)
			return false;
		for (int i = 0; i < nvars; i++)
			if (!strcmp(vars[i].key, v->key)) {
				v->value = vars[i].values[vars[i].cur];
				return true;
			}
		v->value = NULL;
		return false;
	}
	case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS: {
		const struct retro_input_descriptor *d = data;
		for (int f = 0; f < 16; f++) {
			free(desc_name[f]);
			desc_name[f] = NULL;
		}
		for (; d && d->description; d++)
			if (d->port == 0 && d->device == RETRO_DEVICE_JOYPAD && d->id < 16 &&
			    !desc_name[d->id])
				desc_name[d->id] = strdup(d->description);
		return true;
	}
	case RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE:
		/* NULL: o core está dizendo que não tem discos (Genesis Plus GX nos
		 * cartuchos) */
		has_disk = data != NULL;
		if (data)
			disk = *(const struct retro_disk_control_callback *)data;
		else
			memset(&disk, 0, sizeof(disk));
		return true;
	case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
		*(bool *)data = vars_dirty;
		vars_dirty = false;
		return true;
	default:
		return false;
	}
}

/* ---- discos (jogos de vários discos) ---------------------------------- */

static unsigned disc_count(void)
{
	return has_disk && disk.get_num_images ? disk.get_num_images() : 0;
}

static unsigned disc_index(void)
{
	return has_disk && disk.get_image_index ? disk.get_image_index() : 0;
}

/* Troca para o próximo disco: abre a tampa, troca, fecha */
static bool disc_next(void)
{
	unsigned n = disc_count();
	if (n < 2 || !disk.set_eject_state || !disk.set_image_index)
		return false;
	unsigned next = (disc_index() + 1) % n;
	return disk.set_eject_state(true) && disk.set_image_index(next) &&
	       disk.set_eject_state(false);
}

/* ---- SRAM ------------------------------------------------------------ */

static void *sram_copy;
static size_t sram_copy_size;

static void sram_load(void)
{
	void *mem = core.get_memory_data(RETRO_MEMORY_SAVE_RAM);
	size_t size = core.get_memory_size(RETRO_MEMORY_SAVE_RAM);
	char path[4096];
	size_t len = 0;

	if (!mem || !size)
		return;
	data_path(path, sizeof(path), "Saves", ".srm");
	void *buf = read_file(path, &len);
	if (buf) {
		memcpy(mem, buf, len < size ? len : size);
		free(buf);
		fprintf(stderr, "pingo-retro: SRAM carregada (%zu bytes)\n", len);
	}
	sram_copy = malloc(size);
	memcpy(sram_copy, mem, size);
	sram_copy_size = size;
}

/* Grava só se mudou desde a última gravação */
static void sram_save(void)
{
	void *mem = core.get_memory_data(RETRO_MEMORY_SAVE_RAM);
	size_t size = core.get_memory_size(RETRO_MEMORY_SAVE_RAM);
	char path[4096], dir[4096];

	if (!mem || !size || !sram_copy || size != sram_copy_size || !memcmp(mem, sram_copy, size))
		return;
	snprintf(dir, sizeof(dir), "%s/Saves/%s", DATA_ROOT, console_code);
	mkdir_p(dir);
	data_path(path, sizeof(path), "Saves", ".srm");
	if (write_file(path, mem, size)) {
		memcpy(sram_copy, mem, size);
		fprintf(stderr, "pingo-retro: SRAM gravada\n");
	}
}

/* ---- save states ----------------------------------------------------- */

static int slot;
static SDL_Texture *slot_preview[SLOTS];
static time_t slot_time[SLOTS];
static bool slot_used[SLOTS];

static void state_path(char *out, size_t n, int s, bool preview)
{
	char suffix[16];
	snprintf(suffix, sizeof(suffix), ".st%d%s", s, preview ? ".bmp" : "");
	data_path(out, n, "States", suffix);
}

static void slots_refresh(void)
{
	for (int s = 0; s < SLOTS; s++) {
		char p[4096];
		struct stat st;
		state_path(p, sizeof(p), s, false);
		slot_used[s] = stat(p, &st) == 0;
		slot_time[s] = slot_used[s] ? st.st_mtime : 0;
		if (slot_preview[s]) {
			SDL_DestroyTexture(slot_preview[s]);
			slot_preview[s] = NULL;
		}
		if (slot_used[s]) {
			state_path(p, sizeof(p), s, true);
			SDL_Surface *bmp = SDL_LoadBMP(p);
			if (bmp) {
				slot_preview[s] = SDL_CreateTextureFromSurface(renderer, bmp);
				SDL_FreeSurface(bmp);
			}
		}
	}
}

/* Slot inicial: o salvo mais recentemente */
static void slot_pick_latest(void)
{
	time_t best = 0;
	for (int s = 0; s < SLOTS; s++)
		if (slot_used[s] && slot_time[s] > best) {
			best = slot_time[s];
			slot = s;
		}
}

static bool state_save(int s)
{
	size_t size = core.serialize_size();
	char path[4096], dir[4096];
	if (!size)
		return false;
	void *buf = malloc(size);
	bool ok = buf && core.serialize(buf, size);
	snprintf(dir, sizeof(dir), "%s/States/%s", DATA_ROOT, console_code);
	mkdir_p(dir);
	state_path(path, sizeof(path), s, false);
	ok = ok && write_file(path, buf, size);
	free(buf);

	/* prévia: o quadro atual, na resolução do jogo */
	if (ok && frame_buf) {
		SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormatFrom(
			frame_buf, frame_w, frame_h, tex_bpp * 8, frame_pitch, tex_format);
		if (surf) {
			state_path(path, sizeof(path), s, true);
			SDL_SaveBMP(surf, path);
			SDL_FreeSurface(surf);
		}
	}
	sync();
	return ok;
}

static bool state_load(int s)
{
	char path[4096];
	size_t len = 0;
	state_path(path, sizeof(path), s, false);
	void *buf = read_file(path, &len);
	bool ok = buf && core.unserialize(buf, len);
	free(buf);
	return ok;
}

/* ---- retomar (energia) ----------------------------------------------- *
 * Antes de suspender ou desligar pelo power, grava um save state automático
 * (<jogo>.auto) e a marca .system/resume. No próximo boot o launcher abre o
 * jogo de novo com "resume" e ele volta exatamente de onde parou. Sair pelo
 * menu (ou acordar o aparelho) apaga a marca.
 */

#define RESUME_FILE DATA_ROOT "/.system/resume"

static char resume_core[4096], resume_rom[4096];

static bool auto_state_save(void)
{
	size_t size = core.serialize_size();
	char path[4096], dir[4096];
	if (!size)
		return false;
	void *buf = malloc(size);
	bool ok = buf && core.serialize(buf, size);
	snprintf(dir, sizeof(dir), "%s/States/%s", DATA_ROOT, console_code);
	mkdir_p(dir);
	data_path(path, sizeof(path), "States", ".auto");
	ok = ok && write_file(path, buf, size);
	free(buf);
	return ok;
}

static bool auto_state_load(void)
{
	char path[4096];
	size_t len = 0;
	data_path(path, sizeof(path), "States", ".auto");
	void *buf = read_file(path, &len);
	bool ok = buf && core.unserialize(buf, len);
	free(buf);
	return ok;
}

static void save_for_resume(void)
{
	sram_save();
	if (auto_state_save()) {
		char marker[12000];
		int n = snprintf(marker, sizeof(marker), "%s\n%s\n%s\n", resume_core, resume_rom,
				 console_code);
		write_file(RESUME_FILE, marker, (size_t)n);
	}
	sync();
}

static void clear_resume(void)
{
	unlink(RESUME_FILE);
}

/* Suspende; se o pingo-suspend pedir (bateria/24 h), desliga */
static void do_suspend(void)
{
	save_for_resume();
	audio_pause(true);
	int rc = system("/usr/bin/pingo-suspend");
	if (rc != -1 && WIFEXITED(rc) && WEXITSTATUS(rc) == 10) {
		fprintf(stderr, "pingo-retro: suspenso por muito tempo ou bateria fraca: desligar\n");
		power_off_requested = quit_requested = true; /* a marca fica: retoma no boot */
		return;
	}
	clear_resume(); /* acordou: o jogo continua da RAM */
	audio_pause(false);
}

/* ---- menu do jogo ---------------------------------------------------- */

static char toast[128];
static Uint32 toast_until;

static void show_toast(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(toast, sizeof(toast), fmt, ap);
	va_end(ap);
	toast_until = SDL_GetTicks() + 2000;
}

static int battery_percent(bool *charging)
{
	FILE *f = fopen("/sys/class/power_supply/battery/capacity", "r");
	int v = -1;
	char st[32] = "";
	if (f) {
		if (fscanf(f, "%d", &v) != 1)
			v = -1;
		fclose(f);
	}
	f = fopen("/sys/class/power_supply/battery/status", "r");
	if (f) {
		if (!fgets(st, sizeof(st), f))
			st[0] = 0;
		fclose(f);
	}
	*charging = !strncmp(st, "Charging", 8);
	return v;
}

static void draw_bar(const char *title)
{
	int y = PAD_Y + BAR_H / 2, x = UI_W - PAD_X;
	bool chg;
	int bat = battery_percent(&chg);
	ui_text(F_BAR_BOLD, title, PAD_X, y, COL_TEXT, 0, 420);
	if (bat >= 0) {
		char pct[16];
		snprintf(pct, sizeof(pct), "%d%%", bat);
		x -= ui_text(F_BAR, pct, x, y, COL_DIM, 1, 0);
		ui_icon_battery(x - 10 - 26, y, bat, chg, COL_DIM);
	}
}

static int draw_hint(int right, const char *button, const char *text)
{
	int y = UI_H - PAD_Y - BAR_H / 2;
	int w = ui_text(F_BAR, text, right, y, COL_DIM, 1, 0);
	int sp = ui_text_width(F_BAR, " ");
	ui_text(F_BAR_BOLD, button, right - w - sp, y, COL_TEXT, 1, 0);
	return w + sp + ui_text_width(F_BAR_BOLD, button);
}

/* fundo do menu: o jogo escurecido */
static void draw_dimmed_game(void)
{
	draw_game();
	SDL_SetRenderDrawColor(renderer, 0, 0, 0, 215);
	SDL_RenderFillRect(renderer, NULL);
}

static void draw_toast_or(void (*left)(void))
{
	if (toast[0] && SDL_GetTicks() < toast_until)
		ui_text(F_BAR, toast, PAD_X, UI_H - PAD_Y - BAR_H / 2, COL_TEXT, 0, 380);
	else if (left)
		left();
}

static void hint_slot(void)
{
	int y = UI_H - PAD_Y - BAR_H / 2;
	int w = ui_text(F_BAR_BOLD, "◀ ▶", PAD_X, y, COL_TEXT, 0, 0);
	ui_text(F_BAR, S_HINT_SLOT, PAD_X + w + ui_text_width(F_BAR, " "), y, COL_DIM, 0, 0);
}

enum { MI_CONTINUE, MI_SAVE, MI_LOAD, MI_OPTIONS, MI_DISC, MI_FLIP, MI_QUIT };
static int menu_ids[7], menu_n;

static void menu_build(void)
{
	menu_n = 0;
	menu_ids[menu_n++] = MI_CONTINUE;
	menu_ids[menu_n++] = MI_SAVE;
	menu_ids[menu_n++] = MI_LOAD;
	menu_ids[menu_n++] = MI_OPTIONS;
	if (disc_count() > 1)
		menu_ids[menu_n++] = MI_DISC;
	if (is_fds)
		menu_ids[menu_n++] = MI_FLIP;
	menu_ids[menu_n++] = MI_QUIT;
}

static const char *menu_label(int id, char *buf, size_t n)
{
	switch (id) {
	case MI_CONTINUE: return S_M_CONTINUE;
	case MI_SAVE:     return S_M_SAVE;
	case MI_LOAD:     return S_M_LOAD;
	case MI_OPTIONS:  return S_M_OPTIONS;
	case MI_DISC:
		snprintf(buf, n, S_M_DISC, disc_index() + 1, disc_count());
		return buf;
	case MI_FLIP:     return S_M_FLIP;
	default:          return S_M_QUIT;
	}
}

static void draw_menu(int sel)
{
	draw_dimmed_game();
	if (disc_count() > 1) { /* "Final Fantasy VII  Disco 1", como no mockup */
		char d[32];
		snprintf(d, sizeof(d), S_DISC_N, disc_index() + 1);
		draw_bar(game_label);
		int w = ui_text_width(F_BAR_BOLD, game_label);
		if (w > 420)
			w = 420;
		ui_text(F_BAR, d, PAD_X + w + 10, PAD_Y + BAR_H / 2, COL_DIM, 0, 0);
	} else {
		draw_bar(game_label);
	}

	int top = PAD_Y + BAR_H + LIST_GAP;
	char lbuf[64];
	for (int i = 0; i < menu_n; i++) {
		int y = top + i * (ROW_H + ROW_GAP), id = menu_ids[i];
		bool s = i == sel;
		if (s)
			ui_round_rect(PAD_X, y, 252, ROW_H, ROW_RADIUS, COL_SEL_BG);
		ui_text(s ? F_ITEM_SEL : F_ITEM, menu_label(id, lbuf, sizeof(lbuf)), PAD_X + ROW_PAD,
			y + ROW_H / 2, s ? COL_SEL_FG : COL_TEXT, 0, 252 - 2 * ROW_PAD - 20);
		if (s && (id == MI_SAVE || id == MI_LOAD || id == MI_OPTIONS))
			ui_icon_chevron(PAD_X + 252 - ROW_PAD - 16, y + ROW_H / 2, COL_SEL_FG);
	}

	/* prévia do slot */
	int px = PAD_X + 252 + 24, pw = UI_W - PAD_X - px, ph = 240;
	ui_round_rect(px, top, pw, ph, ROW_RADIUS, 0x3A3A3A);
	ui_round_rect(px + 2, top + 2, pw - 4, ph - 4, ROW_RADIUS - 2, 0x111111);
	if (slot_preview[slot]) {
		int tw, th;
		SDL_QueryTexture(slot_preview[slot], NULL, NULL, &tw, &th);
		float a = aspect > 0 ? aspect : (float)tw / th;
		int iw = pw - 16, ih = (int)(iw / a);
		if (ih > ph - 16) {
			ih = ph - 16;
			iw = (int)(ih * a);
		}
		SDL_Rect d = { px + (pw - iw) / 2, top + (ph - ih) / 2, iw, ih };
		SDL_RenderCopy(renderer, slot_preview[slot], NULL, &d);
	} else {
		ui_text(F_BAR, S_SLOT_EMPTY, px + pw / 2, top + ph / 2, COL_DIM, 2, 0);
	}

	char buf[64];
	int y = top + ph + 12 + 10;
	snprintf(buf, sizeof(buf), S_SLOT, slot + 1);
	ui_text(F_COUNT, buf, px, y, COL_TEXT, 0, 0);
	for (int s = 0; s < SLOTS; s++)
		ui_dot(px + pw - (SLOTS - s) * 18 + 8, y, s == slot, s == slot ? COL_TEXT : COL_DIM);
	if (slot_used[slot]) {
		struct tm t, now;
		time_t n = time(NULL);
		localtime_r(&slot_time[slot], &t);
		localtime_r(&n, &now);
		const char *fmt = S_SAVED_DATE;
		if (t.tm_year == now.tm_year && t.tm_yday == now.tm_yday)
			fmt = S_SAVED_TODAY;
		else if (t.tm_year == now.tm_year && t.tm_yday == now.tm_yday - 1)
			fmt = S_SAVED_YDAY;
		strftime(buf, sizeof(buf), fmt, &t);
		ui_text(F_SMALL, buf, px, y + 26, COL_DIM, 0, 0);
	}

	draw_toast_or(hint_slot);
	int right = UI_W - PAD_X;
	right -= draw_hint(right, "B", S_BACK) + 22;
	draw_hint(right, "A", S_CONFIRM);
	osd_draw(renderer, screen_w, screen_h);
	SDL_RenderPresent(renderer);
}

/* opções: escala, botões, opções do core e as duas formas de salvar */
enum { OPT_SCALE, OPT_BUTTONS, OPT_VAR, OPT_SAVE_GAME, OPT_SAVE_CONSOLE };
static int opt_kind[MAX_VARS + 4], opt_arg[MAX_VARS + 4], opt_n;

static void options_build(void)
{
	opt_n = 0;
	opt_kind[opt_n++] = OPT_SCALE;
	opt_kind[opt_n++] = OPT_BUTTONS;
	for (int k = 0; k < nshown; k++) {
		opt_kind[opt_n] = OPT_VAR;
		opt_arg[opt_n++] = k;
	}
	opt_kind[opt_n++] = OPT_SAVE_GAME;
	opt_kind[opt_n++] = OPT_SAVE_CONSOLE;
}

static void draw_list_row(int r, bool s, const char *label, const char *value)
{
	int y = PAD_Y + BAR_H + LIST_GAP + r * (ROW_H + ROW_GAP);
	if (s)
		ui_round_rect(PAD_X, y, UI_W - 2 * PAD_X, ROW_H, ROW_RADIUS, COL_SEL_BG);
	int vw = 0;
	if (value)
		vw = ui_text(F_COUNT, value, UI_W - PAD_X - ROW_PAD, y + ROW_H / 2,
			     s ? COL_SEL_FG : COL_DIM, 1, 200) + 16;
	ui_text(s ? F_ITEM_SEL : F_ITEM, label, PAD_X + ROW_PAD, y + ROW_H / 2,
		s ? COL_SEL_FG : COL_TEXT, 0, UI_W - 2 * PAD_X - 2 * ROW_PAD - vw);
}

static void draw_options(int sel, int top_item)
{
	draw_dimmed_game();
	draw_bar(S_M_OPTIONS);

	for (int r = 0; r < ROWS && top_item + r < opt_n; r++) {
		int i = top_item + r;
		const char *label, *value = NULL;
		switch (opt_kind[i]) {
		case OPT_SCALE:
			label = S_O_SCALE;
			value = scale_mode == SCALE_INT ? S_O_SCALE_INT : S_O_SCALE_FIT;
			break;
		case OPT_BUTTONS:
			label = S_O_BUTTONS;
			break;
		case OPT_VAR: {
			struct var *v = &vars[shown[opt_arg[i]]];
			label = v->label;
			value = v->values[v->cur];
			break;
		}
		case OPT_SAVE_GAME:
			label = S_O_SAVE_GAME;
			break;
		default:
			label = S_O_SAVE_CONSOLE;
		}
		draw_list_row(r, i == sel, label, value);
		if (i == sel && opt_kind[i] == OPT_BUTTONS)
			ui_icon_chevron(UI_W - PAD_X - ROW_PAD - 16,
					PAD_Y + BAR_H + LIST_GAP + r * (ROW_H + ROW_GAP) + ROW_H / 2,
					COL_SEL_FG);
	}

	draw_toast_or(NULL);
	int right = UI_W - PAD_X;
	right -= draw_hint(right, "B", S_BACK) + 22;
	if (opt_kind[sel] == OPT_SCALE || opt_kind[sel] == OPT_VAR)
		draw_hint(right, "◀ ▶", S_HINT_CHANGE);
	else
		draw_hint(right, "A", S_CONFIRM);
	osd_draw(renderer, screen_w, screen_h);
	SDL_RenderPresent(renderer);
}

static void option_change(int i, int d)
{
	if (opt_kind[i] == OPT_SCALE) {
		scale_mode = scale_mode == SCALE_INT ? SCALE_FIT : SCALE_INT;
	} else if (opt_kind[i] == OPT_VAR) {
		struct var *v = &vars[shown[opt_arg[i]]];
		v->cur = (v->cur + d + v->nvalues) % v->nvalues;
		vars_dirty = true;
	}
}

static bool was_pressed(int code)
{
	for (int i = 0; i < npressed; i++)
		if (pressed[i] == code)
			return true;
	return false;
}

/* Funções que aparecem na tela de botões: as que o core nomeou (menos o
 * direcional); sem nomes, os botões do layout padrão */
static int bfunc[16], nbfunc;

static void buttons_build(void)
{
	static const int order[] = {
		RETRO_DEVICE_ID_JOYPAD_A, RETRO_DEVICE_ID_JOYPAD_B, RETRO_DEVICE_ID_JOYPAD_X,
		RETRO_DEVICE_ID_JOYPAD_Y, RETRO_DEVICE_ID_JOYPAD_L, RETRO_DEVICE_ID_JOYPAD_R,
		RETRO_DEVICE_ID_JOYPAD_L2, RETRO_DEVICE_ID_JOYPAD_R2, RETRO_DEVICE_ID_JOYPAD_L3,
		RETRO_DEVICE_ID_JOYPAD_R3, RETRO_DEVICE_ID_JOYPAD_SELECT, RETRO_DEVICE_ID_JOYPAD_START,
	};
	bool named = false;
	for (int f = 0; f < 16; f++)
		named |= desc_name[f] != NULL;
	nbfunc = 0;
	for (unsigned i = 0; i < sizeof(order) / sizeof(order[0]); i++)
		if (!named || desc_name[order[i]])
			bfunc[nbfunc++] = order[i];
}

static const char *func_name(int f)
{
	static const char *def[16] = { "B", "Y", "Select", "Start", "↑", "↓", "←", "→",
				       "A", "X", "L1", "R1", "L2", "R2", "L3", "R3" };
	return desc_name[f] ? desc_name[f] : def[f];
}

static void draw_buttons(int sel, int top_item, bool waiting)
{
	draw_dimmed_game();
	draw_bar(S_O_BUTTONS);
	int n = nbfunc + 1;
	for (int r = 0; r < ROWS && top_item + r < n; r++) {
		int i = top_item + r;
		if (i < nbfunc) {
			int f = bfunc[i];
			const char *v = (waiting && i == sel) ? S_PRESS_BUTTON : button_name(bind[f]);
			draw_list_row(r, i == sel, func_name(f), v);
		} else {
			draw_list_row(r, i == sel, S_O_BUTTONS_RESET, NULL);
		}
	}
	draw_toast_or(NULL);
	int right = UI_W - PAD_X;
	if (!waiting) {
		right -= draw_hint(right, "B", S_BACK) + 22;
		draw_hint(right, "A", sel < nbfunc ? S_HINT_CHANGE : S_CONFIRM);
	}
	osd_draw(renderer, screen_w, screen_h);
	SDL_RenderPresent(renderer);
}

/* A numa função: espera o botão do aparelho; se ele já era de outra
 * função, as duas trocam (nenhuma fica sem botão) */
static void run_buttons(void)
{
	int sel = 0, top = 0;
	bool waiting = false;
	buttons_build();
	for (;;) {
		poll_devices();
		if (quit_requested)
			return;
		int n = nbfunc + 1;
		if (waiting) {
			for (int k = 0; k < npressed; k++) {
				int code = pressed[k];
				if (!strcmp(button_name(code), "—")) /* não é botão do controle */
					continue;
				int f = bfunc[sel], old = bind[f];
				for (int g = 0; g < 16; g++)
					if (g != f && bind[g] == code)
						bind[g] = old;
				bind[f] = code;
				waiting = false;
				break;
			}
			memset(pad_state, 0, sizeof(pad_state));
		} else {
			if (was_pressed(BTN_DPAD_UP))
				sel = (sel - 1 + n) % n;
			if (was_pressed(BTN_DPAD_DOWN))
				sel = (sel + 1) % n;
			if (was_pressed(BTN_EAST)) {
				if (sel < nbfunc) {
					waiting = true;
					npressed = 0;
				} else {
					binds_default();
					show_toast(S_O_BUTTONS_RESTORED);
				}
			} else if (was_pressed(BTN_SOUTH) || menu_requested) {
				menu_requested = false;
				return;
			}
		}
		if (sel < top)
			top = sel;
		if (sel >= top + ROWS)
			top = sel - ROWS + 1;
		draw_buttons(sel, top, waiting);
	}
}

/* devolve false para sair do jogo */
static bool run_options(void)
{
	int sel = 0, top = 0;
	options_build();
	for (;;) {
		poll_devices();
		if (quit_requested)
			return false;
		if (was_pressed(BTN_DPAD_UP))
			sel = (sel - 1 + opt_n) % opt_n;
		if (was_pressed(BTN_DPAD_DOWN))
			sel = (sel + 1) % opt_n;
		if (was_pressed(BTN_DPAD_LEFT))
			option_change(sel, -1);
		if (was_pressed(BTN_DPAD_RIGHT))
			option_change(sel, 1);
		if (was_pressed(BTN_EAST)) {
			int k = opt_kind[sel];
			if (k == OPT_SCALE || k == OPT_VAR)
				option_change(sel, 1);
			else if (k == OPT_BUTTONS)
				run_buttons();
			else if (cfg_save(k == OPT_SAVE_GAME))
				show_toast(k == OPT_SAVE_GAME ? S_O_SAVED_GAME : S_O_SAVED_CONS);
			else
				show_toast(S_SAVE_FAILED);
		}
		if (was_pressed(BTN_SOUTH) || menu_requested) {
			menu_requested = false;
			return true;
		}
		if (sel < top)
			top = sel;
		if (sel >= top + ROWS)
			top = sel - ROWS + 1;
		draw_options(sel, top);
	}
}

/* devolve false para sair do jogo */
static bool run_menu(void)
{
	int sel = 0;
	bool has_states = core.serialize_size() > 0;

	audio_pause(true);
	sram_save();
	slots_refresh();
	menu_build();
	toast[0] = 0;

	for (;;) {
		poll_devices();
		if (quit_requested)
			break;
		if (suspend_requested) { /* power no menu: suspende também */
			suspend_requested = false;
			do_suspend();
			audio_pause(true); /* continua no menu, sem som */
			if (quit_requested)
				break;
		}
		if (was_pressed(BTN_DPAD_UP))
			sel = (sel - 1 + menu_n) % menu_n;
		if (was_pressed(BTN_DPAD_DOWN))
			sel = (sel + 1) % menu_n;
		if (was_pressed(BTN_DPAD_LEFT))
			slot = (slot - 1 + SLOTS) % SLOTS;
		if (was_pressed(BTN_DPAD_RIGHT))
			slot = (slot + 1) % SLOTS;
		if (was_pressed(BTN_SOUTH) || menu_requested) {
			menu_requested = false;
			break;
		}
		if (was_pressed(BTN_EAST)) {
			int id = menu_ids[sel];
			if (id == MI_CONTINUE)
				break;
			if (id == MI_FLIP) {
				macro_start(); /* acontece já no jogo, quadro a quadro */
				show_toast(S_FLIPPING);
				break;
			}
			if (id == MI_DISC) {
				if (disc_next()) {
					show_toast(S_DISC_CHANGED, disc_index() + 1);
					break; /* volta ao jogo já com o disco novo */
				}
				show_toast(S_LOAD_FAILED);
				continue;
			}
			if (id == MI_QUIT) {
				quit_requested = true;
				break;
			}
			if (id == MI_OPTIONS) {
				if (!run_options())
					break;
				continue;
			}
			if (!has_states) {
				show_toast(S_NO_STATES);
			} else if (id == MI_SAVE) {
				if (state_save(slot)) {
					show_toast(S_SAVED_SLOT, slot + 1);
					slots_refresh();
					break; /* volta ao jogo, como no MinUI */
				}
				show_toast(S_SAVE_FAILED);
			} else if (id == MI_LOAD) {
				if (!slot_used[slot])
					show_toast(S_SLOT_IS_EMPTY, slot + 1);
				else if (state_load(slot))
					break;
				else
					show_toast(S_LOAD_FAILED);
			}
		}
		draw_menu(sel);
	}
	memset(pad_state, 0, sizeof(pad_state));
	audio_pause(false);
	return !quit_requested;
}

/* ---- jogo (.zip) ------------------------------------------------------ */

static bool has_ext(const char *name, const char *ext)
{
	const char *dot = strrchr(name, '.');
	return dot && !strcasecmp(dot + 1, ext);
}

#define TMP_ROM_DIR "/run/pingo/rom"
static char zip_inner_path[4096];

/* .m3u com caminhos completos: aceita nomes relativos, absolutos e o erro
 * comum de "/Nome do disco.chd" (barra no começo sem ser caminho real) */
static char m3u_fixed[4096];

static const char *fix_m3u(const char *m3u)
{
	FILE *in = fopen(m3u, "r");
	if (!in)
		return m3u;
	char dir[4096], line[4096], full[8300];
	snprintf(dir, sizeof(dir), "%s", m3u);
	char *slash = strrchr(dir, '/');
	if (slash)
		*slash = 0;
	mkdir_p(TMP_ROM_DIR);
	const char *b = strrchr(m3u, '/');
	snprintf(m3u_fixed, sizeof(m3u_fixed), "%s/%s", TMP_ROM_DIR, b ? b + 1 : m3u);
	FILE *out = fopen(m3u_fixed, "w");
	if (!out) {
		fclose(in);
		return m3u;
	}
	while (fgets(line, sizeof(line), in)) {
		line[strcspn(line, "\r\n")] = 0;
		if (!line[0] || line[0] == '#')
			continue;
		struct stat st;
		const char *name = line;
		if (line[0] == '/' && stat(line, &st) == 0) {
			snprintf(full, sizeof(full), "%s", line);
		} else {
			while (*name == '/')
				name++;
			snprintf(full, sizeof(full), "%s/%s", dir, name);
		}
		fprintf(out, "%s\n", full);
	}
	fclose(in);
	fclose(out);
	return m3u_fixed;
}

/* Maior arquivo do .zip, na memória */
static void *unzip_game(const char *path, size_t *size)
{
	int err;
	zip_t *z = zip_open(path, ZIP_RDONLY, &err);
	if (!z)
		return NULL;
	zip_int64_t n = zip_get_num_entries(z, 0), best = -1;
	zip_uint64_t best_size = 0;
	for (zip_int64_t i = 0; i < n; i++) {
		zip_stat_t st;
		if (zip_stat_index(z, i, 0, &st) || !(st.valid & ZIP_STAT_SIZE) || !st.name)
			continue;
		const char *b = strrchr(st.name, '/');
		b = b ? b + 1 : st.name;
		if (!*b || *b == '.')
			continue;
		if (st.size > best_size) {
			best_size = st.size;
			best = i;
		}
	}
	void *buf = NULL;
	if (best >= 0) {
		zip_file_t *f = zip_fopen_index(z, best, 0);
		buf = f ? malloc(best_size) : NULL;
		if (buf && zip_fread(f, buf, best_size) != (zip_int64_t)best_size) {
			free(buf);
			buf = NULL;
		}
		if (f)
			zip_fclose(f);
		if (buf) {
			/* Também em arquivo de verdade (na RAM): alguns cores abrem o
			 * caminho de novo (FCEUmm no Famicom Disk System, NeoPop) */
			const char *inner = zip_get_name(z, best, 0);
			const char *b = strrchr(inner, '/');
			mkdir_p(TMP_ROM_DIR);
			snprintf(zip_inner_path, sizeof(zip_inner_path), "%s/%s", TMP_ROM_DIR,
				 b ? b + 1 : inner);
			FILE *o = fopen(zip_inner_path, "wb");
			if (o) {
				fwrite(buf, 1, best_size, o);
				fclose(o);
			}
			*size = best_size;
		}
	}
	zip_close(z);
	return buf;
}

/* ---- diagnóstico de travamentos ------------------------------------- *
 * SIGSEGV/SIGBUS/SIGABRT: imprime a pilha e os endereços das bibliotecas no
 * log, para localizar a função no PC (addr2line no .so do build).
 */
static void crash_handler(int sig)
{
	void *frames[48];
	int n = backtrace(frames, 48);
	dprintf(2, "pingo-retro: travou (sinal %d); pilha:\n", sig);
	backtrace_symbols_fd(frames, n, 2);
	int fd = open("/proc/self/maps", O_RDONLY);
	if (fd >= 0) {
		char buf[4096];
		ssize_t r;
		dprintf(2, "pingo-retro: mapas:\n");
		while ((r = read(fd, buf, sizeof(buf))) > 0)
			if (write(2, buf, r) < 0)
				break;
		close(fd);
	}
	signal(sig, SIG_DFL);
	raise(sig);
}

/* ---- principal ------------------------------------------------------- */

static void names_from_path(const char *rom, const char *code)
{
	const char *slash = strrchr(rom, '/');
	const char *file = slash ? slash + 1 : rom;
	snprintf(game_base, sizeof(game_base), "%s", file);
	char *dot = strrchr(game_base, '.');
	if (dot && dot != game_base)
		*dot = 0;
	clean_label(game_base, game_label, sizeof(game_label));

	if (code && *code) {
		snprintf(console_code, sizeof(console_code), "%s", code);
	} else { /* .../Roms/<CONSOLE>/... */
		const char *r = strstr(rom, "/Roms/");
		if (r) {
			r += 6;
			const char *e = strchr(r, '/');
			if (e && e - r < (int)sizeof(console_code))
				snprintf(console_code, sizeof(console_code), "%.*s", (int)(e - r), r);
		}
	}
}

int main(int argc, char **argv)
{
	struct retro_system_info sysinfo = { 0 };
	struct retro_system_av_info av = { 0 };
	struct retro_game_info game = { 0 };

	if (argc < 3) {
		fprintf(stderr, "uso: %s <core_libretro.so> <rom> [console] [resume]\n", argv[0]);
		return 2;
	}
	signal(SIGSEGV, crash_handler);
	signal(SIGBUS, crash_handler);
	signal(SIGABRT, crash_handler);
	names_from_path(argv[2], argc > 3 ? argv[3] : NULL);
	bool resume = argc > 4 && !strcmp(argv[4], "resume");
	snprintf(resume_core, sizeof(resume_core), "%s", argv[1]);
	snprintf(resume_rom, sizeof(resume_rom), "%s", argv[2]);
	snprintf(system_dir, sizeof(system_dir), "%s/Bios", DATA_ROOT);
	snprintf(save_dir, sizeof(save_dir), "%s/Saves/%s", DATA_ROOT, console_code);
	mkdir_p(save_dir);
	binds_default();
	cfg_load_all(); /* escala e botões (as opções do core chegam no SET_VARIABLES) */

	{ /* ".../gambatte_libretro.so" -> "gambatte" */
		const char *b = strrchr(argv[1], '/');
		snprintf(core_name, sizeof(core_name), "%s", b ? b + 1 : argv[1]);
		char *suf = strstr(core_name, "_libretro");
		if (suf)
			*suf = 0;
	}
	if (!core_open(argv[1]))
		return 1;
	core.set_environment(environment);
	core.init();
	core.get_system_info(&sysinfo);
	fprintf(stderr, "pingo-retro: core %s %s, console %s\n", sysinfo.library_name,
		sysinfo.library_version, console_code);

	game.path = argv[2];
	if (has_ext(argv[2], "m3u"))
		game.path = fix_m3u(argv[2]);
	is_fds = has_ext(argv[2], "fds");

	/* .zip: o core que aceita .zip (FBNeo: o romset é o próprio .zip) recebe
	 * como está; os outros recebem o jogo extraído, na RAM */
	bool core_zip = sysinfo.valid_extensions && strstr(sysinfo.valid_extensions, "zip");
	void *zip_data = NULL;
	size_t zip_size = 0;
	if (has_ext(argv[2], "zip") && !core_zip) {
		zip_data = unzip_game(argv[2], &zip_size);
		if (!zip_data) {
			fprintf(stderr, "pingo-retro: não consegui abrir o .zip %s\n", argv[2]);
			return 1;
		}
		game.path = zip_inner_path;
		is_fds = has_ext(zip_inner_path, "fds");
	}
	if (!sysinfo.need_fullpath) {
		if (zip_data) {
			game.data = zip_data;
			game.size = zip_size;
		} else {
			game.data = read_file(game.path, &game.size);
		}
		if (!game.data) {
			fprintf(stderr, "pingo-retro: não consegui ler %s\n", argv[2]);
			return 1;
		}
	} else {
		free(zip_data); /* o core abre o arquivo extraído pelo caminho */
	}

	SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) < 0) {
		fprintf(stderr, "pingo-retro: SDL: %s\n", SDL_GetError());
		return 1;
	}
	SDL_ShowCursor(SDL_DISABLE);
	SDL_SetHint(SDL_HINT_RENDER_DRIVER, "opengles2");
	SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");
	window = SDL_CreateWindow("pingo-retro", 0, 0, 0, 0, SDL_WINDOW_FULLSCREEN_DESKTOP);
	renderer = window ? SDL_CreateRenderer(window, -1,
					       SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC)
			  : NULL;
	if (!renderer) {
		fprintf(stderr, "pingo-retro: vídeo: %s\n", SDL_GetError());
		return 1;
	}
	SDL_GetRendererOutputSize(renderer, &screen_w, &screen_h);
	SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
	ui_attach(renderer);

	if (!core.load_game(&game)) {
		fprintf(stderr, "pingo-retro: o core recusou a ROM\n");
		return 1;
	}
	core.get_system_av_info(&av);
	aspect = av.geometry.aspect_ratio;
	core_rate = av.timing.sample_rate;
	double fps = av.timing.fps > 0 ? av.timing.fps : 60;
	/* perto de 60 Hz: um quadro do core por vsync; senão, acumula tempo */
	bool lockstep = fps > 58.5 && fps < 61.5;
	double runs_per_sec = lockstep ? 60.0 : fps;
	audio_step = core_rate * (runs_per_sec / fps) / AUDIO_RATE;
	fprintf(stderr, "pingo-retro: %ux%u, %.2f fps, som %.0f Hz, %s\n", av.geometry.base_width,
		av.geometry.base_height, fps, core_rate, lockstep ? "1 quadro por vsync" : "acumulando");

	SDL_AudioSpec want = { 0 }, have;
	want.freq = AUDIO_RATE;
	want.format = AUDIO_S16SYS;
	want.channels = 2;
	want.samples = 512;
	audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
	if (audio_dev) {
		audio_target = AUDIO_RATE * 4 * 64 / 1000; /* ~64 ms na fila */
		SDL_PauseAudioDevice(audio_dev, 0);
	} else {
		fprintf(stderr, "pingo-retro: sem som: %s\n", SDL_GetError());
	}

	pad_fd = open_input(GAMEPAD_NAME);
	vol_fd = open_input("gpio-keys-volume");
	power_fd = open_input("axp20x-pek");
	osd_init();

	core.set_video_refresh(video_refresh);
	core.set_audio_sample(audio_sample);
	core.set_audio_sample_batch(audio_batch);
	core.set_input_poll(input_poll);
	core.set_input_state(input_state);

	sram_load();
	slots_refresh();
	slot_pick_latest();
	if (resume) {
		if (auto_state_load())
			fprintf(stderr, "pingo-retro: retomado do estado automático\n");
		clear_resume(); /* um só retorno: se travar, não entra em laço */
	}

	Uint32 t0 = SDL_GetTicks(), frames = 0, last_sram = SDL_GetTicks();
	bool low_battery_warned = false;
	double acc = 0;
	while (!quit_requested) {
		SDL_Event e;
		while (SDL_PollEvent(&e))
			if (e.type == SDL_QUIT)
				quit_requested = true;

		poll_devices();
		if (menu_requested) {
			menu_requested = false;
			if (!run_menu())
				break;
		}
		if (suspend_requested) {
			suspend_requested = false;
			do_suspend();
		}

		if (lockstep) {
			core.run();
		} else {
			acc += fps / 60.0;
			while (acc >= 1.0) {
				core.run();
				acc -= 1.0;
			}
		}
		draw_game();
		if (toast[0] && SDL_GetTicks() < toast_until) {
			int w = ui_text_width(F_BAR, toast) + 2 * ROW_PAD;
			ui_round_rect((UI_W - w) / 2, PAD_Y, w, BAR_H + 4, ROW_RADIUS, 0x000000);
			ui_text(F_BAR, toast, UI_W / 2, PAD_Y + (BAR_H + 4) / 2, COL_TEXT, 2, 0);
		}
		osd_draw(renderer, screen_w, screen_h);
		SDL_RenderPresent(renderer);

		frames++;
		Uint32 now = SDL_GetTicks();
		if (now - last_sram >= 5000) { /* SRAM: grava sozinha quando muda */
			sram_save();
			last_sram = now;
			bool chg;
			int bat = battery_percent(&chg);
			if (!chg && bat >= 0 && bat <= 3) {
				fprintf(stderr, "pingo-retro: bateria em %d%%: salvar e desligar\n", bat);
				power_off_requested = quit_requested = true;
			} else if (!chg && bat >= 0 && bat <= 10 && !low_battery_warned) {
				low_battery_warned = true;
				show_toast(S_LOW_BATTERY);
			} else if (chg || bat > 12) {
				low_battery_warned = false;
			}
		}
		if (now - t0 >= 10000) {
			fprintf(stderr, "pingo-retro: %.1f fps, som na fila %u bytes\n",
				frames * 1000.0 / (now - t0), audio_dev ? SDL_GetQueuedAudioSize(audio_dev) : 0);
			t0 = now;
			frames = 0;
		}
	}

	if (power_off_requested)
		save_for_resume();
	else
		clear_resume();
	sram_save();
	core.unload_game();
	core.deinit();
	if (audio_dev)
		SDL_CloseAudioDevice(audio_dev);
	/* Entre o jogo e o launcher aparece o fb0: preto, não o logo do boot */
	fb_clear();
	for (int s = 0; s < SLOTS; s++)
		if (slot_preview[s])
			SDL_DestroyTexture(slot_preview[s]);
	ui_detach();
	SDL_Quit();
	free((void *)game.data);
	if (zip_inner_path[0])
		unlink(zip_inner_path);
	if (m3u_fixed[0])
		unlink(m3u_fixed);
	/* 3: o pingo-shell mostra "Desligando…" e desliga */
	return power_off_requested ? 3 : 0;
}
