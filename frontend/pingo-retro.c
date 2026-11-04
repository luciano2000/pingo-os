/*
 * pingo-retro: frontend libretro mínimo do Pingo OS (Fase 0).
 *
 * Uso: pingo-retro <core_libretro.so> <rom>
 *
 * Vídeo pelo SDL2 (KMS/DRM + GLES, escala inteira centralizada), som pelo
 * SDL2 (ALSA) e controles lidos direto do evdev do "H700 Gamepad".
 * O botão MENU (BTN_MODE) sai.
 *
 * Copyright (C) 2026 Pingo OS by Retrogamer Brasil
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <SDL2/SDL.h>

#include "libretro.h"
#include "osd.h"

#define GAMEPAD_NAME "H700 Gamepad"

/* ---- core ------------------------------------------------------------ */

static struct {
	void *handle;
	void (*init)(void);
	void (*deinit)(void);
	unsigned (*api_version)(void);
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
	LOAD_SYM(api_version, "retro_api_version");
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
	return true;
}

/* ---- vídeo ----------------------------------------------------------- */

static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Texture *texture;
static int tex_w, tex_h;
static Uint32 tex_format = SDL_PIXELFORMAT_RGB565;
static int screen_w, screen_h;

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
}

/* Quadro atual (escala inteira, centralizada) + sobreposição de volume/brilho */
static void render_frame(void)
{
	SDL_RenderClear(renderer);
	if (texture) {
		int scale = SDL_min(screen_w / tex_w, screen_h / tex_h);
		if (scale < 1)
			scale = 1;
		SDL_Rect dst = { (screen_w - tex_w * scale) / 2, (screen_h - tex_h * scale) / 2,
				 tex_w * scale, tex_h * scale };
		SDL_RenderCopy(renderer, texture, NULL, &dst);
	}
	osd_draw(renderer, screen_w, screen_h);
	SDL_RenderPresent(renderer);
}

/* ---- som ------------------------------------------------------------- */

static SDL_AudioDeviceID audio_dev;
static Uint32 audio_max_queued; /* bytes: acima disso descarta, para não acumular atraso */

static size_t audio_batch(const int16_t *data, size_t frames)
{
	if (audio_dev && SDL_GetQueuedAudioSize(audio_dev) < audio_max_queued)
		SDL_QueueAudio(audio_dev, data, (Uint32)(frames * 2 * sizeof(int16_t)));
	return frames;
}

static void audio_sample(int16_t left, int16_t right)
{
	int16_t frame[2] = { left, right };
	audio_batch(frame, 1);
}

/* ---- controles (evdev) ----------------------------------------------- */

static int pad_fd = -1, vol_fd = -1;
static bool pad_state[16];
static bool quit_requested;

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
		int fd = open(path, O_RDONLY | O_NONBLOCK);
		if (fd < 0)
			continue;
		if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) >= 0 && !strcmp(name, want)) {
			closedir(dir);
			fprintf(stderr, "pingo-retro: controle em %s\n", path);
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

static void input_poll(void)
{
	struct input_event ev;

	while (vol_fd >= 0 && read(vol_fd, &ev, sizeof(ev)) == sizeof(ev))
		if (ev.type == EV_KEY)
			osd_key(ev.code, ev.value);
	if (pad_fd < 0)
		return;
	while (read(pad_fd, &ev, sizeof(ev)) == sizeof(ev)) {
		if (ev.type != EV_KEY)
			continue;
		if (osd_key(ev.code, ev.value))
			continue;
		if (ev.code == BTN_MODE) {
			/* MENU ao soltar, sem Vol± junto. Até o menu do jogo existir
			 * (fatia 3), ele sai do jogo. */
			if (osd_menu_tapped())
				quit_requested = true;
			continue;
		}
		int id = map_key(ev.code);
		if (id >= 0)
			pad_state[id] = ev.value != 0;
	}
}

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

/* Raiz somente leitura: saves e BIOS vão para /tmp na Fase 0 */
static const char *data_dir = "/tmp";

static bool environment(unsigned cmd, void *data)
{
	switch (cmd) {
	case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
		((struct retro_log_callback *)data)->log = core_log;
		return true;
	case RETRO_ENVIRONMENT_GET_CAN_DUPE:
		*(bool *)data = true;
		return true;
	case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
		enum retro_pixel_format fmt = *(enum retro_pixel_format *)data;
		switch (fmt) {
		case RETRO_PIXEL_FORMAT_RGB565:   tex_format = SDL_PIXELFORMAT_RGB565; break;
		case RETRO_PIXEL_FORMAT_XRGB8888: tex_format = SDL_PIXELFORMAT_RGB888; break;
		case RETRO_PIXEL_FORMAT_0RGB1555: tex_format = SDL_PIXELFORMAT_RGB555; break;
		default: return false;
		}
		return true;
	}
	case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
	case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
		*(const char **)data = data_dir;
		return true;
	case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
		*(bool *)data = false;
		return true;
	default:
		return false;
	}
}

/* ---- principal ------------------------------------------------------- */

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

int main(int argc, char **argv)
{
	struct retro_system_info sysinfo = { 0 };
	struct retro_system_av_info av = { 0 };
	struct retro_game_info game = { 0 };

	if (argc != 3) {
		fprintf(stderr, "uso: %s <core_libretro.so> <rom>\n", argv[0]);
		return 2;
	}
	if (!core_open(argv[1]))
		return 1;

	core.set_environment(environment);
	core.init();
	core.get_system_info(&sysinfo);
	fprintf(stderr, "pingo-retro: core %s %s\n", sysinfo.library_name, sysinfo.library_version);

	game.path = argv[2];
	if (!sysinfo.need_fullpath) {
		game.data = read_file(argv[2], &game.size);
		if (!game.data) {
			fprintf(stderr, "pingo-retro: não consegui ler %s\n", argv[2]);
			return 1;
		}
	}

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
	SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
	{
		SDL_RendererInfo ri;
		SDL_GetRendererInfo(renderer, &ri);
		fprintf(stderr, "pingo-retro: vídeo %s (%s) %dx%d\n", SDL_GetCurrentVideoDriver(),
			ri.name, screen_w, screen_h);
	}

	if (!core.load_game(&game)) {
		fprintf(stderr, "pingo-retro: o core recusou a ROM\n");
		return 1;
	}
	core.get_system_av_info(&av);
	fprintf(stderr, "pingo-retro: %ux%u, %.2f fps, som %.0f Hz\n", av.geometry.base_width,
		av.geometry.base_height, av.timing.fps, av.timing.sample_rate);

	SDL_AudioSpec want = { 0 }, have;
	want.freq = (int)(av.timing.sample_rate + 0.5);
	want.format = AUDIO_S16SYS;
	want.channels = 2;
	want.samples = 1024;
	audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
	if (audio_dev) {
		audio_max_queued = (Uint32)want.freq * 4 / 10; /* ~100 ms */
		SDL_PauseAudioDevice(audio_dev, 0);
	} else {
		fprintf(stderr, "pingo-retro: sem som: %s\n", SDL_GetError());
	}

	pad_fd = open_input(GAMEPAD_NAME);
	vol_fd = open_input("gpio-keys-volume");
	osd_init();
	if (pad_fd < 0)
		fprintf(stderr, "pingo-retro: \"%s\" não encontrado, sem controle\n", GAMEPAD_NAME);

	core.set_video_refresh(video_refresh);
	core.set_audio_sample(audio_sample);
	core.set_audio_sample_batch(audio_batch);
	core.set_input_poll(input_poll);
	core.set_input_state(input_state);

	/* Um retro_run por vsync (60 Hz). O Game Boy roda a 59,73 Hz; a
	 * diferença (~0,5 %) fica no som, que descarta o excesso. */
	Uint32 t0 = SDL_GetTicks(), frames = 0;
	while (!quit_requested) {
		SDL_Event e;
		while (SDL_PollEvent(&e))
			if (e.type == SDL_QUIT)
				quit_requested = true;

		core.run();
		render_frame();

		frames++;
		Uint32 now = SDL_GetTicks();
		if (now - t0 >= 5000) {
			fprintf(stderr, "pingo-retro: %.1f fps, som na fila %u bytes\n",
				frames * 1000.0 / (now - t0),
				audio_dev ? SDL_GetQueuedAudioSize(audio_dev) : 0);
			t0 = now;
			frames = 0;
		}
	}

	core.unload_game();
	core.deinit();
	if (audio_dev)
		SDL_CloseAudioDevice(audio_dev);
	osd_quit();
	SDL_Quit();
	free((void *)game.data);
	return 0;
}
