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
#include <errno.h>
#include <execinfo.h>
#include <signal.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <linux/input.h>
#include <math.h>
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
#include "imagem.h"
#include "controle2.h"
#include "libretro.h"
#include "osd.h"
#include "luzes.h"
#include "ra.h"
#include "textos.h"
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
	void (*reset)(void);
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
	LOAD_SYM(reset, "retro_reset");
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
/* Controle 2: bind2[f] = função do controle 2 (C2_*, numeração do painel) */
static int bind2[16];
static int bind2_default(int f);
static int button_by_name(const char *name);
static const char *button_name(int code);
static int default_bind(int f);

enum { SCALE_FIT, SCALE_INT };
static int scale_mode = SCALE_FIT;

/* vibração: o aparelho tem motor, o core pediu, e a opção do menu */
static bool rumble_hw, rumble_asked, rumble_on = true;

#define MAX_VARS 192
static struct var {
	char *key, *desc;
	char *values[32];
	int nvalues, cur;
	char *label; /* nome mostrado (lista curada); NULL: fica oculta */
	char *vlabel[32]; /* nome mostrado de cada valor (NULL: o do core) */
	bool vhide[32];   /* valores fora da lista curada */
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
	for (int i = 0; i < nvars; i++) {
		free(vars[i].label);
		vars[i].label = NULL;
		for (int k = 0; k < vars[i].nvalues; k++) {
			free(vars[i].vlabel[k]);
			vars[i].vlabel[k] = NULL;
			vars[i].vhide[k] = false;
		}
	}
	if (!f)
		return;
	while (fgets(line, sizeof(line), f) && nshown < MAX_VARS) {
		char *eq = strchr(line, '='), *nl = strchr(line, '\n');
		if (line[0] == '#' || !eq)
			continue;
		if (nl)
			*nl = 0;
		*eq = 0;
		/* "chave=Nome mostrado|padrão do Pingo|valor=Nome;valor=Nome"
		 * O padrão e a lista de valores são opcionais; com a lista, só os
		 * valores dela aparecem, com esses nomes. */
		char *def = strchr(eq + 1, '|'), *list = NULL;
		if (def) {
			*def++ = 0;
			if ((list = strchr(def, '|')))
				*list++ = 0;
			if (!*def)
				def = NULL;
		}
		for (int i = 0; i < nvars; i++) {
			struct var *v = &vars[i];
			if (strcmp(v->key, line) || v->label)
				continue;
			bool any = false;
			if (list) {
				char *copy = strdup(list), *save = NULL;
				for (int k = 0; k < v->nvalues; k++)
					v->vhide[k] = true;
				for (char *t = strtok_r(copy, ";", &save); t; t = strtok_r(NULL, ";", &save)) {
					char *e = strchr(t, '=');
					if (e)
						*e++ = 0;
					for (int k = 0; k < v->nvalues; k++)
						if (!strcmp(v->values[k], t)) {
							v->vhide[k] = false;
							v->vlabel[k] = e ? strdup(e) : NULL;
							any = true;
						}
				}
				free(copy);
				if (!any) /* nenhum valor bate (core mudou): mostra todos */
					for (int k = 0; k < v->nvalues; k++)
						v->vhide[k] = false;
			}
			v->label = strdup(eq + 1);
			shown[nshown++] = i;
			for (int k = 0; def && k < v->nvalues; k++)
				if (!strcmp(v->values[k], def))
					v->cur = k;
		}
	}
	fclose(f);
}

/* ---- filtros de tela (GB, GBC, GBA) ----------------------------------- *
 * O usuário escolhe um nome; nunca vê parâmetros. A cor do GB e do GBC vem
 * do próprio Gambatte (custo zero); a do GBA é uma tabela de 32768 cores
 * aplicada a cada quadro (240x160: nada para a CPU). O rastro (mistura com
 * o quadro anterior) e a grade LCD valem para os três.
 */
static const struct screen_console {
	const char *code;
	const char *names[3];
	int n, def;
} screen_consoles[] = {
	{ "GB",  { "Original", "DMG" },           2, 1 },
	{ "GBC", { "Original", "GBC" },           2, 0 },
	{ "GBA", { "Original", "GBA", "SP 101" }, 3, 0 },
};
static const struct screen_console *scr; /* NULL: console sem filtros */
static int screen_preset;
static bool grid_on, ghost_on;
static bool scanlines_on; /* todos os consoles */

/* Volume do jogo (reforço por software, em dB): o FBNeo reproduz a mixagem
 * das placas, uns 5-6 dB abaixo dos consoles (medido: Raiden x Shadow
 * Dancer). Padrão +6 dB no Arcade e no Neo Geo, 0 nos outros. */
static const int game_vol_db[] = { 0, 3, 6, 9 };
#define GAME_VOL_N ((int)(sizeof(game_vol_db) / sizeof(*game_vol_db)))
static int game_vol = 0;         /* índice em game_vol_db */
static float game_gain = 1.0f;

static void game_vol_apply(void)
{
	game_gain = powf(10.0f, game_vol_db[game_vol] / 20.0f);
}

static void game_vol_default(void)
{
	game_vol = (!strcmp(console_code, "ARCADE") || !strcmp(console_code, "NEOGEO")) ? 2 : 0;
	game_vol_apply();
}

static void screen_init(void)
{
	scr = NULL;
	for (size_t i = 0; i < sizeof(screen_consoles) / sizeof(*screen_consoles); i++)
		if (!strcmp(screen_consoles[i].code, console_code))
			scr = &screen_consoles[i];
	screen_preset = scr ? scr->def : 0;
	grid_on = ghost_on = false;
}

/* Nome de um valor no menu: o da lista curada, os comuns em português ou
 * o do core */
static const char *var_value_text(const struct var *v)
{
	const char *val = v->values[v->cur];
	if (v->vlabel[v->cur])
		return v->vlabel[v->cur];
	if (!strcasecmp(val, "enabled") || !strcasecmp(val, "on"))
		return S_O_ON;
	if (!strcasecmp(val, "disabled") || !strcasecmp(val, "off"))
		return S_O_OFF;
	if (!strcasecmp(val, "auto"))
		return S_O_AUTO;
	return val;
}

static void var_set_value(const char *key, const char *value)
{
	if (!strcmp(key, "pingo.scale")) {
		scale_mode = !strcmp(value, "inteira") ? SCALE_INT : SCALE_FIT;
		return;
	}
	if (!strcmp(key, "pingo.tela")) {
		for (int i = 0; scr && i < scr->n; i++)
			if (!strcmp(scr->names[i], value))
				screen_preset = i;
		return;
	}
	if (!strcmp(key, "pingo.volume")) {
		int db = atoi(value);
		for (int i = 0; i < GAME_VOL_N; i++)
			if (game_vol_db[i] == db)
				game_vol = i;
		game_vol_apply();
		return;
	}
	if (!strcmp(key, "pingo.vibracao")) {
		rumble_on = !strcmp(value, "sim");
		return;
	}
	if (!strcmp(key, "pingo.scanlines")) {
		scanlines_on = !strcmp(value, "sim");
		return;
	}
	if (!strcmp(key, "pingo.grade") || !strcmp(key, "pingo.rastro")) {
		*(key[6] == 'g' ? &grid_on : &ghost_on) = !strcmp(value, "sim");
		return;
	}
	if (!strncmp(key, "pingo.bind2.", 12)) {
		int f = atoi(key + 12);
		for (int c = 0; c < C2_N && f >= 0 && f < 16; c++)
			if (!strcmp(value, c2_keys[c]))
				bind2[f] = c;
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

/* ---- dip switches do arcade (FBNeo) ------------------------------------ *
 * Cada placa tem os seus: o FBNeo os manda como opções do core
 * ("fbneo-dipswitch-<jogo>-<nome>", descrição "[Dipswitch] Nome"). Ficam
 * num submenu próprio, com os nomes mais comuns em português. A BIOS do
 * Neo Geo fica de fora: ela é a opção "Neo Geo: versão".
 */
#define DIP_PREFIX "fbneo-dipswitch-"
static int dips[MAX_VARS], ndips;

static void dips_build(void)
{
	ndips = 0;
	for (int i = 0; i < nvars; i++)
		if (!strncmp(vars[i].key, DIP_PREFIX, strlen(DIP_PREFIX)) &&
		    strcmp(vars[i].desc, "[Dipswitch] BIOS"))
			dips[ndips++] = i;
	if (ndips)
		fprintf(stderr, "pingo-retro: %d dip switches\n", ndips);
}

static const char *const dip_words[][2] = {
	/* nomes */
	{ "Difficulty", "Dificuldade" }, { "Game Difficulty", "Dificuldade" },
	{ "Lives", "Vidas" }, { "Player Lives", "Vidas" }, { "Bonus Life", "Vida extra" },
	{ "Extend", "Vida extra" }, { "Demo Sounds", "Som na demonstração" },
	{ "Demo Sound", "Som na demonstração" }, { "Flip Screen", "Inverter a tela" },
	{ "Free Play", "Jogo grátis" }, { "Cabinet", "Gabinete" }, { "Coinage", "Fichas" },
	{ "Coin A", "Fichas (A)" }, { "Coin B", "Fichas (B)" }, { "Coin Slots", "Entradas de ficha" },
	{ "Service Mode", "Modo de serviço" }, { "Allow Continue", "Permitir continuar" },
	{ "Continue", "Continuar" }, { "Continues", "Continuações" }, { "Language", "Idioma" },
	{ "Region", "Região" }, { "Time", "Tempo" }, { "Timer", "Tempo" },
	{ "Timer Speed", "Velocidade do tempo" }, { "Game Time", "Tempo de jogo" },
	{ "Round Time", "Tempo do round" }, { "Blood", "Sangue" }, { "Violence", "Violência" },
	{ "Players", "Jogadores" }, { "Max Players", "Jogadores" },
	{ "Rounds", "Rounds" }, { "Stage Select", "Escolher fase" },
	/* valores */
	{ "Off", "Desligado" }, { "On", "Ligado" }, { "Yes", "Sim" }, { "No", "Não" },
	{ "Very Easy", "Muito fácil" }, { "Easiest", "Muito fácil" }, { "Easy", "Fácil" },
	{ "Normal", "Normal" }, { "Medium", "Médio" }, { "Hard", "Difícil" },
	{ "Harder", "Mais difícil" }, { "Very Hard", "Muito difícil" },
	{ "Hardest", "Muito difícil" }, { "Upright", "Em pé" }, { "Cocktail", "Mesa (cocktail)" },
	{ "English", "Inglês" }, { "Japanese", "Japonês" }, { "Japan", "Japão" },
	{ "World", "Mundo" }, { "Europe", "Europa" }, { "USA", "EUA" }, { "Asia", "Ásia" },
	{ "None", "Nenhum" }, { "Infinite", "Infinito" },
};

static const char *dip_text(const char *en)
{
	if (!lang_is_pt()) /* a tabela é só para o português; senão, como vem do FBNeo */
		return en;
	for (size_t i = 0; i < sizeof(dip_words) / sizeof(*dip_words); i++)
		if (!strcasecmp(dip_words[i][0], en))
			return dip_words[i][1];
	return en;
}

static const char *dip_label(const struct var *v)
{
	const char *d = v->desc;
	if (!strncmp(d, "[Dipswitch] ", 12))
		d += 12;
	return dip_text(d);
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
	len += (size_t)snprintf(buf + len, sizeof(buf) - len, "pingo.volume=%d\n",
				game_vol_db[game_vol]);
	len += (size_t)snprintf(buf + len, sizeof(buf) - len, "pingo.scanlines=%s\n",
				scanlines_on ? "sim" : "nao");
	if (rumble_asked && rumble_hw)
		len += (size_t)snprintf(buf + len, sizeof(buf) - len, "pingo.vibracao=%s\n",
					rumble_on ? "sim" : "nao");
	if (scr)
		len += (size_t)snprintf(buf + len, sizeof(buf) - len,
					"pingo.tela=%s\npingo.grade=%s\npingo.rastro=%s\n",
					scr->names[screen_preset], grid_on ? "sim" : "nao",
					ghost_on ? "sim" : "nao");
	/* botões: só os que mudaram do padrão */
	for (int f = 0; f < 16 && len < sizeof(buf); f++) {
		if (bind[f] != default_bind(f) && bind[f] >= 0)
			len += (size_t)snprintf(buf + len, sizeof(buf) - len, "pingo.bind.%d=%s\n", f,
						button_name(bind[f]));
	}
	for (int f = 0; f < 16 && len < sizeof(buf); f++)
		if (bind2[f] != bind2_default(f) && bind2[f] >= 0)
			len += (size_t)snprintf(buf + len, sizeof(buf) - len, "pingo.bind2.%d=%s\n", f,
						c2_keys[bind2[f]]);
	/* só as opções do menu: as ocultas seguem o padrão do core */
	for (int k = 0; k < nshown && len < sizeof(buf); k++) {
		struct var *v = &vars[shown[k]];
		len += (size_t)snprintf(buf + len, sizeof(buf) - len, "%s=%s\n", v->key,
					v->values[v->cur]);
	}
	/* dip switches: todos (são do jogo; no arquivo do console não atrapalham
	 * os outros jogos, porque a chave tem o nome do jogo) */
	for (int k = 0; k < ndips && len < sizeof(buf); k++) {
		struct var *v = &vars[dips[k]];
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
		const char *semi = v->value ? strchr(v->value, ';') : NULL;
		if (!semi)
			continue;
		bool dup = false;
		for (int i = 0; i < nvars && !dup; i++)
			dup = !strcmp(vars[i].key, v->key);
		if (dup)
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

/* Muda uma opção do core por baixo (as do filtro de tela, que não
 * aparecem no menu) */
static void var_force(const char *key, const char *value)
{
	for (int i = 0; i < nvars; i++)
		if (!strcmp(vars[i].key, key))
			for (int k = 0; k < vars[i].nvalues; k++)
				if (!strcmp(vars[i].values[k], value) && vars[i].cur != k) {
					vars[i].cur = k;
					vars_dirty = true;
				}
}

/* tabela de cor: índice RGB 5-5-5 -> cor final no formato da textura */
static uint32_t *color_lut;
static bool lut_on;
static void lut_build(void);

static void screen_apply(void)
{
	if (!scr)
		return;
	const char *name = scr->names[screen_preset];
	if (!strcmp(scr->code, "GB")) {
		bool dmg = !strcmp(name, "DMG");
		var_force("gambatte_gb_colorization", dmg ? "internal" : "disabled");
		if (dmg)
			var_force("gambatte_gb_internal_palette", "GB - DMG");
	} else if (!strcmp(scr->code, "GBC")) {
		var_force("gambatte_gbc_color_correction",
			  !strcmp(name, "GBC") ? "GBC only" : "disabled");
		var_force("gambatte_gbc_color_correction_mode", "accurate");
	} else {
		var_force("mgba_color_correction", "OFF");
		var_force("mgba_interframe_blending", "OFF");
	}
	var_force("gambatte_mix_frames", "disabled"); /* o rastro é nosso */
	lut_build();
}

/* Hardcore do RetroAchievements: valor proibido volta para o primeiro
 * permitido (o padrão do core vem primeiro) */
static void vars_enforce_hardcore(void)
{
	for (int i = 0; i < nvars; i++) {
		struct var *v = &vars[i];
		if (ra_setting_allowed(v->key, v->values[v->cur]))
			continue;
		for (int k = 0; k < v->nvalues; k++)
			if (ra_setting_allowed(v->key, v->values[k])) {
				fprintf(stderr, "pingo-retro: hardcore: %s=%s -> %s\n", v->key,
					v->values[v->cur], v->values[k]);
				v->cur = k;
				vars_dirty = true;
				break;
			}
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

/* Matrizes de cor de Pokefan531 e hunterk (gba-color e sp101-color dos
 * libretro slang-shaders, perfil sRGB; domínio público). Linha = cor de
 * saída, coluna = contribuição de R, G, B da entrada. */
static const float gba_matrix[3][3] = {
	{ 0.905f, 0.195f, -0.10f }, { 0.10f, 0.65f, 0.25f }, { 0.1575f, 0.1425f, 0.70f } };
static const float sp101_matrix[3][3] = {
	{ 0.96f, 0.11f, -0.07f }, { 0.0325f, 0.89f, 0.0775f }, { 0.001f, -0.03f, 1.029f } };

static void lut_build(void)
{
	const float (*m)[3] = NULL;
	float lum = 1, darken = 0;
	if (scr && !strcmp(scr->code, "GBA") && screen_preset == 1) {
		m = gba_matrix; lum = 0.91f; darken = 0.5f; /* tela sem luz: mais escura */
	} else if (scr && !strcmp(scr->code, "GBA") && screen_preset == 2) {
		m = sp101_matrix; lum = 0.935f;
	}
	lut_on = m != NULL;
	if (!lut_on)
		return;
	if (!color_lut && !(color_lut = malloc(32768 * sizeof(*color_lut)))) {
		lut_on = false;
		return;
	}
	const float g = 2.2f, out_g = darken / (g * g) + 1 / g;
	for (int i = 0; i < 32768; i++) {
		float in[3] = { powf(((i >> 10) & 31) / 31.0f, g), powf(((i >> 5) & 31) / 31.0f, g),
				powf((i & 31) / 31.0f, g) };
		int o[3];
		for (int c = 0; c < 3; c++) {
			float v = (m[c][0] * in[0] + m[c][1] * in[1] + m[c][2] * in[2]) * lum;
			v = v < 0 ? 0 : (v > 1 ? 1 : v);
			o[c] = (int)(powf(v, out_g) * 255 + 0.5f);
		}
		if (tex_format == SDL_PIXELFORMAT_RGB565)
			color_lut[i] = (o[0] >> 3) << 11 | (o[1] >> 2) << 5 | o[2] >> 3;
		else if (tex_format == SDL_PIXELFORMAT_RGB555)
			color_lut[i] = (o[0] >> 3) << 10 | (o[1] >> 3) << 5 | o[2] >> 3;
		else
			color_lut[i] = (uint32_t)o[0] << 16 | o[1] << 8 | o[2];
	}
}

/* quadro filtrado e o quadro anterior (para o rastro) */
static void *flt_buf, *prev_buf;
static int flt_w, flt_h;
static bool prev_valid;

static const void *screen_filter(const void *data, unsigned w, unsigned h, size_t *pitch)
{
	if (!scr || (!lut_on && !ghost_on))
		return data;
	if ((int)w != flt_w || (int)h != flt_h || !flt_buf) {
		free(flt_buf);
		free(prev_buf);
		flt_buf = malloc((size_t)w * h * 4);
		prev_buf = malloc((size_t)w * h * 4);
		flt_w = w;
		flt_h = h;
		prev_valid = false;
		if (!flt_buf || !prev_buf)
			return data;
	}
	if (!ghost_on)
		prev_valid = false;
	if (tex_bpp == 2) {
		bool is565 = tex_format == SDL_PIXELFORMAT_RGB565;
		uint16_t mask = is565 ? 0xF7DE : 0x7BDE; /* tira o bit baixo de cada cor */
		for (unsigned y = 0; y < h; y++) {
			const uint16_t *src = (const uint16_t *)((const char *)data + y * *pitch);
			uint16_t *dst = (uint16_t *)flt_buf + y * w, *prev = (uint16_t *)prev_buf + y * w;
			for (unsigned x = 0; x < w; x++) {
				uint16_t c = src[x];
				if (lut_on)
					c = color_lut[is565 ? (c >> 1 & 0x7C00) | (c >> 1 & 0x3E0) | (c & 31)
							    : (c & 0x7FFF)];
				if (ghost_on) {
					uint16_t p = prev_valid ? prev[x] : c;
					prev[x] = c;
					c = (uint16_t)(((c ^ p) & mask) >> 1) + (c & p);
				}
				dst[x] = c;
			}
		}
	} else {
		for (unsigned y = 0; y < h; y++) {
			const uint32_t *src = (const uint32_t *)((const char *)data + y * *pitch);
			uint32_t *dst = (uint32_t *)flt_buf + y * w, *prev = (uint32_t *)prev_buf + y * w;
			for (unsigned x = 0; x < w; x++) {
				uint32_t c = src[x] & 0xFFFFFF;
				if (lut_on)
					c = color_lut[(c >> 9 & 0x7C00) | (c >> 6 & 0x3E0) | (c >> 3 & 31)];
				if (ghost_on) {
					uint32_t p = prev_valid ? prev[x] : c;
					prev[x] = c;
					c = (((c ^ p) & 0xFEFEFE) >> 1) + (c & p);
				}
				dst[x] = c;
			}
		}
	}
	prev_valid = ghost_on;
	*pitch = (size_t)w * tex_bpp;
	return flt_buf;
}

static void video_refresh(const void *data, unsigned width, unsigned height, size_t pitch)
{
	if (!data) /* quadro duplicado (GET_CAN_DUPE) */
		return;
	data = screen_filter(data, width, height, &pitch);
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
/* Rotação pedida pelo core (SET_ROTATION), em passos de 90° no sentido
 * anti-horário: jogos verticais do arcade vêm deitados e são girados aqui.
 * A proporção informada pelo core já é a da tela girada. */
static int rotation;

/* Copia uma textura para o retângulo final, girada se for o caso */
static void blit_rotated(SDL_Texture *t, const SDL_Rect *dst)
{
	if (!rotation) {
		SDL_RenderCopy(renderer, t, NULL, dst);
		return;
	}
	SDL_Rect r = *dst;
	if (rotation & 1) { /* 90° ou 270°: antes de girar, largura e altura trocadas */
		r.w = dst->h;
		r.h = dst->w;
		r.x = dst->x + (dst->w - r.w) / 2;
		r.y = dst->y + (dst->h - r.h) / 2;
	}
	SDL_RenderCopyEx(renderer, t, NULL, &r, -90.0 * rotation, NULL, SDL_FLIP_NONE);
}

static SDL_Rect game_rect(void)
{
	int gw = rotation & 1 ? tex_h : tex_w, gh = rotation & 1 ? tex_w : tex_h;
	float a = aspect > 0 ? aspect : (float)gw / gh;
	SDL_Rect r;
	if (scale_mode == SCALE_INT) {
		/* múltiplos exatos da altura; a largura segue a proporção */
		int s = SDL_min(screen_w / gw, screen_h / gh);
		if (s < 1)
			s = 1;
		r.h = gh * s;
		r.w = (int)(r.h * a + 0.5f);
		if (r.w > screen_w)
			r.w = gw * s;
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

/* Grade LCD: textura do tamanho da imagem, branca com as bordas de cada
 * pixel do jogo mais escuras, multiplicada por cima (só na escala Inteira) */
static SDL_Texture *grid_tex;
static int grid_w, grid_h, grid_s;

static void draw_grid(const SDL_Rect *dst)
{
	int s = tex_h > 0 ? dst->h / tex_h : 0;
	if (s < 2 || dst->w != tex_w * s)
		return;
	if (!grid_tex || grid_w != dst->w || grid_h != dst->h || grid_s != s) {
		if (grid_tex)
			SDL_DestroyTexture(grid_tex);
		grid_tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
					     SDL_TEXTUREACCESS_STATIC, dst->w, dst->h);
		uint32_t *px = grid_tex ? malloc((size_t)dst->w * dst->h * 4) : NULL;
		if (!px) {
			if (grid_tex)
				SDL_DestroyTexture(grid_tex);
			grid_tex = NULL;
			return;
		}
		/* em 2x a linha ocupa metade do pixel: mais clara */
		uint32_t line = s >= 3 ? 0xB0 : 0xD8;
		for (int y = 0; y < dst->h; y++)
			for (int x = 0; x < dst->w; x++) {
				uint32_t v = (x % s == s - 1 || y % s == s - 1) ? line : 0xFF;
				px[y * dst->w + x] = 0xFF000000 | v << 16 | v << 8 | v;
			}
		SDL_UpdateTexture(grid_tex, NULL, px, dst->w * 4);
		free(px);
		SDL_SetTextureBlendMode(grid_tex, SDL_BLENDMODE_MOD);
		grid_w = dst->w;
		grid_h = dst->h;
		grid_s = s;
	}
	SDL_RenderCopy(renderer, grid_tex, NULL, dst);
}

/* Scanlines: escurece 25 % da metade de baixo de cada linha do jogo. A
 * textura tem 4 linhas por linha do jogo e é esticada com filtro linear:
 * na escala Inteira as linhas saem exatas; na Ajustada, suaves e
 * regulares. Gira junto com a imagem (jogos verticais). */
static SDL_Texture *scan_tex;
static int scan_h;

static void draw_scanlines(const SDL_Rect *dst)
{
	if (!scan_tex || scan_h != tex_h) {
		if (scan_tex)
			SDL_DestroyTexture(scan_tex);
		scan_tex = NULL;
		scan_h = tex_h;
		int h = tex_h * 4;
		uint32_t *px = malloc((size_t)h * 4);
		if (!px)
			return;
		for (int y = 0; y < h; y++) {
			uint32_t v = (y & 3) >= 2 ? 0xBF : 0xFF; /* 75 %: 25 % mais escuro */
			px[y] = 0xFF000000 | v << 16 | v << 8 | v;
		}
		scan_tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
					     SDL_TEXTUREACCESS_STATIC, 1, h);
		if (scan_tex) {
			SDL_UpdateTexture(scan_tex, NULL, px, 4);
			SDL_SetTextureBlendMode(scan_tex, SDL_BLENDMODE_MOD);
			SDL_SetTextureScaleMode(scan_tex, SDL_ScaleModeLinear);
		}
		free(px);
	}
	if (scan_tex)
		blit_rotated(scan_tex, dst);
}

static void draw_game(void)
{
	ui_logical(false); /* o jogo em coordenadas da tela */
	SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
	SDL_RenderClear(renderer);
	if (texture) {
		SDL_Rect dst = game_rect();
		blit_rotated(texture, &dst);
		if (scr && grid_on && scale_mode == SCALE_INT && !rotation)
			draw_grid(&dst);
		if (scanlines_on && tex_h > 0)
			draw_scanlines(&dst);
	}
}

/* ---- Ambilight (luzes do RG40XX) -------------------------------------- *
 * Umas 15 vezes por segundo, a imagem do jogo vira uma grade de 16x12 pontos
 * (em coordenadas da tela, já com a rotação dos jogos verticais) e cada
 * metade dá a cor de um analógico. Cor predominante, não média: os pontos
 * vão para 12 faixas de tom, com peso maior para os de cor viva e claros, e
 * fica a faixa mais forte. Tela escura ou cinza: luz fraca e neutra. O
 * pingo-leds suaviza a transição. Custo: 192 pontos a cada 4 quadros.
 */
static bool ambi_on;

static void ambi_init(void)
{
	FILE *f = fopen(LEDS_CFG, "r");
	char line[64];
	ambi_on = false;
	while (f && fgets(line, sizeof(line), f))
		if (!strncmp(line, "modo=ambilight", 14))
			ambi_on = leds_available();
	if (f)
		fclose(f);
}

static void ambi_pixel(int x, int y, float *r, float *g, float *b)
{
	const unsigned char *row = (const unsigned char *)frame_buf + (size_t)y * frame_pitch;
	if (tex_bpp == 4) {
		uint32_t c = ((const uint32_t *)row)[x];
		*r = ((c >> 16) & 255) / 255.0f;
		*g = ((c >> 8) & 255) / 255.0f;
		*b = (c & 255) / 255.0f;
	} else {
		uint16_t c = ((const uint16_t *)row)[x];
		if (tex_format == SDL_PIXELFORMAT_RGB565) {
			*r = ((c >> 11) & 31) / 31.0f;
			*g = ((c >> 5) & 63) / 63.0f;
			*b = (c & 31) / 31.0f;
		} else {
			*r = ((c >> 10) & 31) / 31.0f;
			*g = ((c >> 5) & 31) / 31.0f;
			*b = (c & 31) / 31.0f;
		}
	}
}

/* A cor de uma metade da tela (side 0 = esquerda, 1 = direita), "RRGGBB" */
static void ambi_side(int side, char out[7])
{
	float wsum[12] = { 0 }, rs[12] = { 0 }, gs[12] = { 0 }, bs[12] = { 0 };
	float ar = 0, ag = 0, ab = 0;
	int n = 0;
	for (int gy = 0; gy < 12; gy++)
		for (int gx = 0; gx < 8; gx++) {
			float u = (side * 8 + gx + 0.5f) / 16.0f, v = (gy + 0.5f) / 12.0f, x, y;
			/* da tela para a imagem do core (rotation = passos de 90° anti-horário) */
			switch (rotation & 3) {
			case 1: x = 1 - v; y = u; break;
			case 2: x = 1 - u; y = 1 - v; break;
			case 3: x = v; y = 1 - u; break;
			default: x = u; y = v; break;
			}
			float r, g, b;
			ambi_pixel((int)(x * (frame_w - 1)), (int)(y * (frame_h - 1)), &r, &g, &b);
			ar += r;
			ag += g;
			ab += b;
			n++;
			float mx = SDL_max(r, SDL_max(g, b)), mn = SDL_min(r, SDL_min(g, b));
			if (mx <= 0.02f)
				continue;
			float sat = (mx - mn) / mx, w = sat * sat * mx;
			float h; /* tom 0..1 */
			if (mx == mn)
				h = 0;
			else if (mx == r)
				h = fmodf((g - b) / (mx - mn) / 6.0f + 1.0f, 1.0f);
			else if (mx == g)
				h = ((b - r) / (mx - mn) + 2) / 6.0f;
			else
				h = ((r - g) / (mx - mn) + 4) / 6.0f;
			int bin = (int)(h * 12) % 12;
			wsum[bin] += w;
			rs[bin] += r * w;
			gs[bin] += g * w;
			bs[bin] += b * w;
		}
	int best = 0;
	float total = 0;
	for (int i = 0; i < 12; i++) {
		total += wsum[i];
		if (wsum[i] > wsum[best])
			best = i;
	}
	float r, g, b;
	if (total < 1.5f || wsum[best] < 0.6f) { /* tela escura/cinza: neutra e fraca */
		r = ar / n * 0.5f;
		g = ag / n * 0.5f;
		b = ab / n * 0.5f;
	} else { /* o tom mais forte, na intensidade máxima (o brilho é das Luzes) */
		r = rs[best] / wsum[best];
		g = gs[best] / wsum[best];
		b = bs[best] / wsum[best];
		float mx = SDL_max(r, SDL_max(g, b));
		if (mx > 0) {
			r /= mx;
			g /= mx;
			b /= mx;
		}
	}
	snprintf(out, 7, "%02X%02X%02X", (int)(r * 255 + 0.5f), (int)(g * 255 + 0.5f),
		 (int)(b * 255 + 0.5f));
}

static void ambi_frame(void)
{
	static unsigned tick;
	if (!ambi_on || !frame_buf || frame_w < 2 || frame_h < 2 || (tick++ & 3))
		return;
	char l[7], r[7];
	ambi_side(0, l);
	ambi_side(1, r);
	leds_cmd("ambi %s %s", l, r);
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

/* Nível do som que o core entrega (para o log: pico e média RMS) */
static int audio_peak;
static double audio_sq_sum;
static unsigned long audio_n;

/* Ganho com limitador suave: abaixo de 80 % do máximo é só a multiplicação;
 * acima, a curva arredonda até o máximo em vez de cortar (sem estalo). A
 * tanhf só roda nas amostras que passam do joelho, que são raras. */
static inline int16_t gain_sample(float x)
{
	x *= game_gain;
	const float knee = 0.8f * 32767.0f, room = 32767.0f - knee;
	float a = x < 0 ? -x : x;
	if (a > knee)
		a = knee + room * tanhf((a - knee) / room);
	return (int16_t)(x < 0 ? -a : a);
}

static size_t audio_batch(const int16_t *data, size_t frames)
{
	static int16_t out[8192 * 2];
	size_t o = 0;

	for (size_t i = 0; i < frames * 2; i++) {
		int v = data[i] < 0 ? -data[i] : data[i];
		if (v > audio_peak)
			audio_peak = v;
		audio_sq_sum += (double)data[i] * data[i];
	}
	audio_n += frames * 2;

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
			out[o * 2] = gain_sample((float)(in_prev[0] + (cur[0] - in_prev[0]) * t));
			out[o * 2 + 1] = gain_sample((float)(in_prev[1] + (cur[1] - in_prev[1]) * t));
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
/* Teclados: a Picade do Pingo Arcade (gpio-keys, teclas do MAME) e teclados
 * USB. As teclas viram os botões do controle do portátil (key_to_pad), então
 * o resto do código só conhece BTN_*. */
#define MAX_KEY_FDS 6
static int key_fds[MAX_KEY_FDS], n_key_fds;
static bool pad_state[16];

/* Analógicos (RETRO_DEVICE_ANALOG): [esquerdo/direito][x/y], -32767..32767.
 * O driver (rocknix-joypad) entrega -1800..1800 e já desinverte os eixos
 * de cada aparelho pelo device tree. */
static int16_t stick[2][2];
static int abs_min[ABS_RY + 1], abs_max[ABS_RY + 1];

static void stick_ranges(void)
{
	for (int code = ABS_X; code <= ABS_RY; code++) {
		struct input_absinfo ai;
		abs_min[code] = -1800;
		abs_max[code] = 1800;
		if (pad_fd >= 0 && ioctl(pad_fd, EVIOCGABS(code), &ai) >= 0 && ai.maximum > ai.minimum) {
			abs_min[code] = ai.minimum;
			abs_max[code] = ai.maximum;
		}
	}
}

/* Valor bruto -> -32767..32767, com zona morta de ~8 % (os sticks destes
 * aparelhos não voltam exatamente ao centro) */
static int16_t stick_scale(int code, int v)
{
	const int dead = 2600;
	int mid = (abs_min[code] + abs_max[code]) / 2, half = (abs_max[code] - abs_min[code]) / 2;
	long x = (long)(v - mid) * 32767 / (half ? half : 1);
	if (x > -dead && x < dead)
		return 0;
	x = x > 0 ? (x - dead) * 32767 / (32767 - dead) : (x + dead) * 32767 / (32767 - dead);
	return (int16_t)(x > 32767 ? 32767 : x < -32767 ? -32767 : x);
}


/* Nomes das funções vindos do core (SET_INPUT_DESCRIPTORS), porta 0 */
static char *desc_name[16];
static bool quit_requested, power_off_requested, suspend_requested, menu_requested;
static bool power_down;
static Uint32 power_down_at;

/* teclas apertadas neste quadro, para o menu */
static int pressed[16], npressed;
/* os mesmos apertos com o código de jogo (o remapeamento grava este: no
 * painel, as teclas mudam de função entre menu e jogo) */
static int pressed_game[16];

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
		/* leitura e escrita: a vibração é escrita no controle */
		int fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0)
			fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
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

/* Teclados: todo dispositivo com setas e Ctrl (a Picade, teclados USB) */
static bool has_key(int fd, int code)
{
	unsigned long bits[KEY_MAX / (8 * sizeof(long)) + 1] = { 0 };
	return ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(bits)), bits) >= 0 &&
	       (bits[code / (8 * sizeof(long))] >> (code % (8 * sizeof(long)))) & 1;
}

static void open_keyboards(void)
{
	DIR *dir = opendir("/dev/input");
	struct dirent *ent;
	char path[300], name[256];
	if (!dir)
		return;
	while ((ent = readdir(dir)) && n_key_fds < MAX_KEY_FDS) {
		if (strncmp(ent->d_name, "event", 5))
			continue;
		snprintf(path, sizeof(path), "/dev/input/%s", ent->d_name);
		int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0)
			continue;
		if (has_key(fd, KEY_UP) && has_key(fd, KEY_LEFTCTRL)) {
			if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) < 0)
				strcpy(name, "?");
			fprintf(stderr, "pingo-retro: teclado %s (%s)\n", name, path);
			key_fds[n_key_fds++] = fd;
		} else {
			close(fd);
		}
	}
	closedir(dir);
}

/* ---- vibração ----------------------------------------------------------- *
 * O core pede força para os motores forte e fraco (RETRO_RUMBLE_*); o
 * aparelho tem um motor só, no PWM, que o rocknix-joypad expõe como
 * FF_RUMBLE (ff-memless). Um efeito só, atualizado quando a força muda,
 * uma vez por quadro. */
static uint16_t rumble_want[2], rumble_sent[2];
static int rumble_id = -1;
static bool rumble_playing;

static void rumble_init(void)
{
	unsigned long bits[(FF_MAX + 8 * sizeof(long)) / (8 * sizeof(long))] = { 0 };
	rumble_hw = pad_fd >= 0 && ioctl(pad_fd, EVIOCGBIT(EV_FF, sizeof(bits)), bits) >= 0 &&
		    (bits[FF_RUMBLE / (8 * sizeof(long))] >> (FF_RUMBLE % (8 * sizeof(long)))) & 1;
}

static void rumble_write(uint16_t strong, uint16_t weak)
{
	if (!rumble_hw)
		return;
	if (strong || weak) {
		struct ff_effect e = { .type = FF_RUMBLE, .id = (short)rumble_id };
		e.u.rumble.strong_magnitude = strong;
		e.u.rumble.weak_magnitude = weak;
		e.replay.length = 0; /* até mandar parar */
		if (ioctl(pad_fd, EVIOCSFF, &e) < 0) {
			fprintf(stderr, "pingo-retro: vibração: %s\n", strerror(errno));
			rumble_hw = false;
			return;
		}
		rumble_id = e.id;
	}
	bool play = strong || weak;
	if (play != rumble_playing && rumble_id >= 0) {
		struct input_event ev = { .type = EV_FF, .code = (uint16_t)rumble_id, .value = play };
		if (write(pad_fd, &ev, sizeof(ev)) != sizeof(ev))
			fprintf(stderr, "pingo-retro: vibração: %s\n", strerror(errno));
		rumble_playing = play;
	}
	rumble_sent[0] = strong;
	rumble_sent[1] = weak;
}

/* Uma vez por quadro, depois do core */
static void rumble_frame(void)
{
	uint16_t s = rumble_on ? rumble_want[0] : 0, w = rumble_on ? rumble_want[1] : 0;
	if (s != rumble_sent[0] || w != rumble_sent[1])
		rumble_write(s, w);
}

/* Menu, suspensão, saída: o motor não pode ficar ligado */
static void rumble_stop(void)
{
	if (rumble_playing)
		rumble_write(0, 0);
}

static bool RETRO_CALLCONV set_rumble_state(unsigned port, enum retro_rumble_effect effect,
					     uint16_t strength)
{
	if (port != 0 || effect > RETRO_RUMBLE_WEAK)
		return false;
	rumble_want[effect] = strength;
	return rumble_hw;
}

/* Teclas -> botões do controle. Painel do bartop, fios nos bornes da Picade:
 *     verde(1)   azul(2)    lilás(3)
 *     amarelo(5) laranja(6) vermelho(4)
 * No FBNeo (retropad), jogos de 6 botões como Street Fighter: socos em cima
 * (Y X L) e chutes embaixo (B A R, da esquerda para a direita). Neo Geo
 * (A B C D = 1 2 3 4, o arco do gabinete MVS) entra por perfil.
 * Nos menus, o amarelo (5) confirma e o laranja (6) volta (e Enter/Backspace
 * de um teclado). */
static int key_to_pad(int code, bool menu)
{
	switch (code) {
	case KEY_UP:        return BTN_DPAD_UP;
	case KEY_DOWN:      return BTN_DPAD_DOWN;
	case KEY_LEFT:      return BTN_DPAD_LEFT;
	case KEY_RIGHT:     return BTN_DPAD_RIGHT;
	case KEY_LEFTCTRL:  return BTN_NORTH;                    /* 1 verde: Y */
	case KEY_LEFTALT:   return BTN_WEST;                     /* 2 azul: X */
	case KEY_SPACE:     return BTN_TL;                       /* 3 lilás: L */
	case KEY_LEFTSHIFT: return BTN_TR;                       /* 4 vermelho: R */
	case KEY_Z:         return menu ? BTN_EAST : BTN_SOUTH;  /* 5 amarelo: B */
	case KEY_X:         return menu ? BTN_SOUTH : BTN_EAST;  /* 6 laranja: A */
	case KEY_1:         return BTN_START;                    /* 1P */
	case KEY_5:         return BTN_SELECT;                   /* ficha */
	case KEY_ESC:       return BTN_MODE;                     /* menu */
	case KEY_ENTER:     return menu ? BTN_EAST : BTN_START;
	case KEY_BACKSPACE: return menu ? BTN_SOUTH : -1;
	default:            return -1;
	}
}

/* Layout libretro (Nintendo): B embaixo, A à direita, Y à esquerda, X em
 * cima, como impresso no aparelho. No device tree do H700 os códigos de X e
 * Y estão trocados em relação à posição: o botão da esquerda (Y) envia
 * BTN_NORTH e o de cima (X) envia BTN_WEST. */
#define PAD_Y_LEFT BTN_NORTH
#define PAD_X_TOP  BTN_WEST

static int map_key(int code)
{
	switch (code) {
	case BTN_SOUTH:      return RETRO_DEVICE_ID_JOYPAD_B;
	case BTN_EAST:       return RETRO_DEVICE_ID_JOYPAD_A;
	case PAD_Y_LEFT:     return RETRO_DEVICE_ID_JOYPAD_Y;
	case PAD_X_TOP:      return RETRO_DEVICE_ID_JOYPAD_X;
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
	for (int f = 0; f < 16; f++) {
		bind[f] = -1;
		bind2[f] = bind2_default(f);
	}
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
	case PAD_Y_LEFT:     return "Y";
	case PAD_X_TOP:      return "X";
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

static void pad_event(const struct input_event *e);

/* ---- controle 2 (USB) ---------------------------------------------------- *
 * Qualquer gamepad/joystick que não seja o painel. Os botões têm a mesma
 * numeração do painel (key_to_pad): 1 2 3 = Y X L, 4 = R, 5 6 = B A. */
static struct c2_dev p2 = { .fd = -1 };
static bool pad2_state[16];
static Uint32 p2_next_scan;

static const int p2_id[C2_N] = {
		[C2_UP] = RETRO_DEVICE_ID_JOYPAD_UP, [C2_DOWN] = RETRO_DEVICE_ID_JOYPAD_DOWN,
		[C2_LEFT] = RETRO_DEVICE_ID_JOYPAD_LEFT, [C2_RIGHT] = RETRO_DEVICE_ID_JOYPAD_RIGHT,
		[C2_B1] = RETRO_DEVICE_ID_JOYPAD_Y, [C2_B2] = RETRO_DEVICE_ID_JOYPAD_X,
		[C2_B3] = RETRO_DEVICE_ID_JOYPAD_L, [C2_B4] = RETRO_DEVICE_ID_JOYPAD_R,
		[C2_B5] = RETRO_DEVICE_ID_JOYPAD_B, [C2_B6] = RETRO_DEVICE_ID_JOYPAD_A,
		[C2_START] = RETRO_DEVICE_ID_JOYPAD_START, [C2_COIN] = RETRO_DEVICE_ID_JOYPAD_SELECT,
};
static int p2_last = -1; /* último aperto, para o remapeamento */

static int bind2_default(int f)
{
	for (int c = 0; c < C2_N; c++)
		if (p2_id[c] == f)
			return c;
	return -1;
}

static void p2_event(int func, bool down)
{
	if (down)
		p2_last = func;
	for (int f = 0; f < 16; f++)
		if (bind2[f] == func)
			pad2_state[f] = down;
}

static void p2_poll(void)
{
	if (p2.fd < 0) {
		/* procura um controle de vez em quando (plugado com o jogo aberto) */
		if (SDL_GetTicks() < p2_next_scan)
			return;
		p2_next_scan = SDL_GetTicks() + 2000;
		if (c2_open(&p2))
			fprintf(stderr, "pingo-retro: controle 2: %s (%04x:%04x, mapa %s)\n", p2.name,
				p2.vid, p2.pid, p2.saved ? "salvo" : "padrão");
		return;
	}
	if (!c2_poll(&p2, p2_event)) {
		fprintf(stderr, "pingo-retro: controle 2 desconectado\n");
		memset(pad2_state, 0, sizeof(pad2_state));
	}
}

/* Lê todos os dispositivos (uma vez por quadro, no jogo e no menu) */
static void poll_devices(void)
{
	ra_pump();
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
	while (pad_fd >= 0 && read(pad_fd, &ev, sizeof(ev)) == sizeof(ev))
		pad_event(&ev);
	p2_poll();
	for (int k = 0; k < n_key_fds; k++)
		while (read(key_fds[k], &ev, sizeof(ev)) == sizeof(ev)) {
			if (ev.type != EV_KEY)
				continue;
			if (ev.code == KEY_POWER) { /* botão de ligar da Picade */
				if (ev.value == 1) {
					fprintf(stderr, "pingo-retro: botão de ligar: desligar\n");
					power_off_requested = quit_requested = true;
				}
				continue;
			}
			int menu_code = key_to_pad(ev.code, true), game_code = key_to_pad(ev.code, false);
			if (game_code < 0)
				continue;
			if (game_code == BTN_MODE) {
				struct input_event m = ev;
				m.code = BTN_MODE;
				pad_event(&m);
				continue;
			}
			if (ev.value == 1 && npressed < 16) {
				pressed_game[npressed] = game_code;
				pressed[npressed++] = menu_code;
			}
			for (int f = 0; f < 16; f++)
				if (bind[f] == game_code)
					pad_state[f] = ev.value != 0;
		}
}

/* Um evento do controle (portátil) */
static void pad_event(const struct input_event *e)
{
	struct input_event ev = *e;
	{
		if (ev.type == EV_ABS && ev.code <= ABS_RY && ev.code != ABS_Z) {
			int side = ev.code >= ABS_RX, axis = ev.code == ABS_Y || ev.code == ABS_RY;
			stick[side][axis] = stick_scale(ev.code, ev.value);
			return;
		}
		if (ev.type != EV_KEY)
			return;
		if (osd_key(ev.code, ev.value))
			return;
		if (ev.code == BTN_MODE) {
			/* MENU ao soltar, sem Vol± junto */
			if (osd_menu_tapped())
				menu_requested = true;
			return;
		}
		if (ev.value == 1 && npressed < 16) {
			pressed_game[npressed] = ev.code;
			pressed[npressed++] = ev.code;
		}
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
	if (port == 1 && device == RETRO_DEVICE_JOYPAD) { /* controle 2 (USB) */
		if (id == RETRO_DEVICE_ID_JOYPAD_MASK) {
			int16_t mask = 0;
			for (unsigned i = 0; i < 16; i++)
				if (pad2_state[i])
					mask |= (int16_t)(1 << i);
			return mask;
		}
		return id < 16 && pad2_state[id];
	}
	if (port != 0)
		return 0;
	if (device == RETRO_DEVICE_ANALOG) {
		if (index <= RETRO_DEVICE_INDEX_ANALOG_RIGHT && id <= RETRO_DEVICE_ID_ANALOG_Y)
			return stick[index][id];
		return 0;
	}
	if (device != RETRO_DEVICE_JOYPAD)
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
		lut_build(); /* a tabela de cor sai no formato da textura */
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
		dips_build();
		cfg_load_all(); /* valores salvos por console/jogo */
		screen_apply();
		vars_enforce_hardcore();
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
	case RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE: {
		struct retro_rumble_interface *r = data;
		if (!r)
			return false;
		r->set_rumble_state = set_rumble_state;
		rumble_asked = true;
		return true;
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
	/* Sem estas duas respostas, o FBNeo acha que é partida em rede (netplay)
	 * na primeira vez que o save state é consultado e desliga os recordes
	 * (hiscore.dat). Aqui é sempre: vídeo e som ligados, save state normal. */
	case RETRO_ENVIRONMENT_SET_MEMORY_MAPS: /* o RetroAchievements lê a memória por aqui */
		ra_set_memory_map((const struct retro_memory_map *)data);
		return true;
	case RETRO_ENVIRONMENT_SET_ROTATION:
		if (!data)
			return false;
		rotation = (int)(*(const unsigned *)data & 3);
		return true;
	case RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE:
		if (data)
			*(int *)data = 1 | 2;
		return true;
	case RETRO_ENVIRONMENT_GET_SAVESTATE_CONTEXT:
		if (data)
			*(int *)data = RETRO_SAVESTATE_CONTEXT_NORMAL;
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
	ui_logical(true); /* o menu em coordenadas de 640x480 */
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

enum { MI_CONTINUE, MI_SAVE, MI_LOAD, MI_OPTIONS, MI_CHEEVOS, MI_DISC, MI_FLIP, MI_QUIT };
static int menu_ids[8], menu_n;

static void menu_build(void)
{
	menu_n = 0;
	menu_ids[menu_n++] = MI_CONTINUE;
	menu_ids[menu_n++] = MI_SAVE;
	menu_ids[menu_n++] = MI_LOAD;
	menu_ids[menu_n++] = MI_OPTIONS;
	if (ra_active())
		menu_ids[menu_n++] = MI_CHEEVOS;
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
	case MI_CHEEVOS:  return S_M_CHEEVOS;
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
		if (s && (id == MI_SAVE || id == MI_LOAD || id == MI_OPTIONS || id == MI_CHEEVOS))
			ui_icon_chevron(PAD_X + 252 - ROW_PAD - 16, y + ROW_H / 2, COL_SEL_FG);
	}

	/* prévia do slot */
	int px = PAD_X + 252 + 24, pw = UI_W - PAD_X - px, ph = 240;
	ui_round_rect(px, top, pw, ph, 10, 0x3A3A3A); /* moldura da prévia: cantos discretos */
	ui_round_rect(px + 2, top + 2, pw - 4, ph - 4, 8, 0x111111);
	if (slot_preview[slot]) {
		int tw, th;
		SDL_QueryTexture(slot_preview[slot], NULL, NULL, &tw, &th);
		if (rotation & 1) {
			int t = tw;
			tw = th;
			th = t;
		}
		float a = aspect > 0 ? aspect : (float)tw / th;
		int iw = pw - 16, ih = (int)(iw / a);
		if (ih > ph - 16) {
			ih = ph - 16;
			iw = (int)(ih * a);
		}
		SDL_Rect d = { px + (pw - iw) / 2, top + (ph - ih) / 2, iw, ih };
		blit_rotated(slot_preview[slot], &d);
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
	ui_screenshot_check(renderer);
	SDL_RenderPresent(renderer);
}

/* opções: escala, botões, opções do core e as duas formas de salvar */
enum { OPT_SCALE, OPT_SCANLINES, OPT_VOLUME, OPT_RUMBLE, OPT_SCREEN, OPT_GRID, OPT_GHOST, OPT_BUTTONS, OPT_DIPS, OPT_VAR,
       OPT_SAVE_GAME, OPT_SAVE_CONSOLE };
static int opt_kind[MAX_VARS + 8], opt_arg[MAX_VARS + 8], opt_n;

static void options_build(void)
{
	opt_n = 0;
	opt_kind[opt_n++] = OPT_SCALE;
	opt_kind[opt_n++] = OPT_SCANLINES;
	opt_kind[opt_n++] = OPT_VOLUME;
	if (rumble_asked && rumble_hw)
		opt_kind[opt_n++] = OPT_RUMBLE;
	if (scr) {
		opt_kind[opt_n++] = OPT_SCREEN;
		opt_kind[opt_n++] = OPT_GRID;
		opt_kind[opt_n++] = OPT_GHOST;
	}
	opt_kind[opt_n++] = OPT_BUTTONS;
	if (ndips)
		opt_kind[opt_n++] = OPT_DIPS;
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
		case OPT_VOLUME: {
			static char vb[24];
			label = S_O_GAME_VOL;
			if (game_vol_db[game_vol] == 0)
				value = S_O_GAME_VOL_NORMAL;
			else {
				snprintf(vb, sizeof(vb), "+%d dB", game_vol_db[game_vol]);
				value = vb;
			}
			break;
		}
		case OPT_RUMBLE:
			label = S_O_RUMBLE;
			value = rumble_on ? S_O_ON_F : S_O_OFF_F;
			break;
		case OPT_SCANLINES:
			label = S_O_SCANLINES;
			value = scanlines_on ? S_O_ON_FP : S_O_OFF_FP;
			break;
		case OPT_SCREEN:
			label = S_O_SCREEN;
			value = scr->names[screen_preset];
			break;
		case OPT_GRID:
			label = S_O_GRID;
			value = grid_on ? S_O_ON_F : S_O_OFF_F;
			break;
		case OPT_GHOST:
			label = S_O_GHOST;
			value = ghost_on ? S_O_ON : S_O_OFF;
			break;
		case OPT_BUTTONS:
			label = S_O_BUTTONS;
			break;
		case OPT_DIPS:
			label = S_O_DIPS;
			break;
		case OPT_VAR: {
			struct var *v = &vars[shown[opt_arg[i]]];
			label = v->label;
			value = var_value_text(v);
			break;
		}
		case OPT_SAVE_GAME:
			label = S_O_SAVE_GAME;
			break;
		default:
			label = S_O_SAVE_CONSOLE;
		}
		draw_list_row(r, i == sel, label, value);
		if (i == sel && (opt_kind[i] == OPT_BUTTONS || opt_kind[i] == OPT_DIPS))
			ui_icon_chevron(UI_W - PAD_X - ROW_PAD - 16,
					PAD_Y + BAR_H + LIST_GAP + r * (ROW_H + ROW_GAP) + ROW_H / 2,
					COL_SEL_FG);
	}

	draw_toast_or(NULL);
	int right = UI_W - PAD_X;
	right -= draw_hint(right, "B", S_BACK) + 22;
	if (opt_kind[sel] != OPT_BUTTONS && opt_kind[sel] != OPT_DIPS &&
	    opt_kind[sel] < OPT_SAVE_GAME)
		draw_hint(right, "◀ ▶", S_HINT_CHANGE);
	else
		draw_hint(right, "A", S_CONFIRM);
	osd_draw(renderer, screen_w, screen_h);
	ui_screenshot_check(renderer);
	SDL_RenderPresent(renderer);
}

static void var_step(struct var *v, int d)
{
	int k = v->cur;
	do /* pula os valores fora da lista curada e os proibidos no hardcore */
		k = (k + d + v->nvalues) % v->nvalues;
	while ((v->vhide[k] || !ra_setting_allowed(v->key, v->values[k])) && k != v->cur);
	v->cur = k;
	vars_dirty = true;
}

static void option_change(int i, int d)
{
	if (opt_kind[i] == OPT_SCALE) {
		scale_mode = scale_mode == SCALE_INT ? SCALE_FIT : SCALE_INT;
		if (scale_mode == SCALE_FIT)
			grid_on = false; /* a grade só fica certa na escala inteira */
	} else if (opt_kind[i] == OPT_VOLUME) {
		game_vol = (game_vol + d + GAME_VOL_N) % GAME_VOL_N;
		game_vol_apply();
	} else if (opt_kind[i] == OPT_RUMBLE) {
		rumble_on = !rumble_on;
	} else if (opt_kind[i] == OPT_SCANLINES) {
		scanlines_on = !scanlines_on;
	} else if (opt_kind[i] == OPT_SCREEN) {
		screen_preset = (screen_preset + d + scr->n) % scr->n;
		screen_apply();
	} else if (opt_kind[i] == OPT_GRID) {
		grid_on = !grid_on;
		if (grid_on)
			scale_mode = SCALE_INT;
	} else if (opt_kind[i] == OPT_GHOST) {
		ghost_on = !ghost_on;
	} else if (opt_kind[i] == OPT_VAR) {
		var_step(&vars[shown[opt_arg[i]]], d);
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

/* Nome do botão para a tela: no painel do Arcade, a numeração dos bornes */
static const char *button_label(int code)
{
	if (pad_fd < 0 && n_key_fds > 0)
		switch (code) {
		case BTN_NORTH:  return "1";
		case BTN_WEST:   return "2";
		case BTN_TL:     return "3";
		case BTN_TR:     return "4";
		case BTN_SOUTH:  return "5";
		case BTN_EAST:   return "6";
		case BTN_START:  return "1P";
		case BTN_SELECT: return "Ficha";
		}
	return button_name(code);
}

static const char *c2_short(int c)
{
	static const char *n[C2_N] = { "↑", "↓", "←", "→", "1", "2", "3", "4", "5", "6",
				       "Start", "Ficha" };
	return c >= 0 && c < C2_N ? n[c] : "—";
}

static int buttons_port; /* 0: painel/controle do aparelho; 1: controle 2 */

static void draw_buttons(int sel, int top_item, bool waiting)
{
	draw_dimmed_game();
	draw_bar(buttons_port ? p2.name : S_O_BUTTONS);
	int n = nbfunc + 1;
	for (int r = 0; r < ROWS && top_item + r < n; r++) {
		int i = top_item + r;
		if (i < nbfunc) {
			int f = bfunc[i];
			const char *v = (waiting && i == sel) ? S_PRESS_BUTTON
				      : buttons_port ? c2_short(bind2[f]) : button_label(bind[f]);
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
	ui_screenshot_check(renderer);
	SDL_RenderPresent(renderer);
}

/* A numa função: espera o botão do aparelho; se ele já era de outra
 * função, as duas trocam (nenhuma fica sem botão) */
static void run_buttons_port(void)
{
	int sel = 0, top = 0;
	bool waiting = false;
	buttons_build();
	for (;;) {
		poll_devices();
		if (quit_requested)
			return;
		int n = nbfunc + 1;
		if (waiting && buttons_port) {
			if (p2.fd < 0) { /* tirou o controle */
				waiting = false;
			} else if (p2_last >= 0) {
				int f = bfunc[sel], old = bind2[f], c = p2_last;
				for (int g = 0; g < 16; g++)
					if (g != f && bind2[g] == c)
						bind2[g] = old;
				bind2[f] = c;
				waiting = false;
			}
			memset(pad2_state, 0, sizeof(pad2_state));
		} else if (waiting) {
			for (int k = 0; k < npressed; k++) {
				int code = pressed_game[k];
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
					p2_last = -1;
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

/* Nome do controle 1 no Arcade: "gabinete=" em Config/arcade.cfg (o nome
 * do bartop); sem ele, "painel" */
static const char *cabinet_name(void)
{
	static char name[64];
	if (name[0])
		return name;
	snprintf(name, sizeof(name), "painel");
	FILE *f = fopen(DATA_ROOT "/Config/arcade.cfg", "r");
	char line[128];
	if (!f)
		return name;
	while (fgets(line, sizeof(line), f))
		if (!strncmp(line, "gabinete=", 9)) {
			line[strcspn(line, "\r\n")] = 0;
			if (line[9])
				snprintf(name, sizeof(name), "%s", line + 9);
		}
	fclose(f);
	return name;
}

/* Botões: com um controle 2 ligado, primeiro escolhe qual controle */
static void draw_buttons_choice(int sel)
{
	draw_dimmed_game();
	draw_bar(S_O_BUTTONS);
	char p1label[96], p2label[160];
	snprintf(p1label, sizeof(p1label), "Controle 1 (%s)", cabinet_name());
	snprintf(p2label, sizeof(p2label), "Controle 2 (%s)", p2.name);
	draw_list_row(0, sel == 0, p1label, NULL);
	draw_list_row(1, sel == 1, p2label, NULL);
	draw_toast_or(NULL);
	int right = UI_W - PAD_X;
	right -= draw_hint(right, "B", S_BACK) + 22;
	draw_hint(right, "A", S_CONFIRM);
	osd_draw(renderer, screen_w, screen_h);
	ui_screenshot_check(renderer);
	SDL_RenderPresent(renderer);
}

static void run_buttons(void)
{
	int sel = 0;
	if (p2.fd < 0) {
		buttons_port = 0;
		run_buttons_port();
		return;
	}
	for (;;) {
		poll_devices();
		if (quit_requested)
			return;
		if (p2.fd < 0) { /* tirou o controle 2: só o 1 */
			buttons_port = 0;
			run_buttons_port();
			return;
		}
		if (was_pressed(BTN_DPAD_UP) || was_pressed(BTN_DPAD_DOWN))
			sel = !sel;
		if (was_pressed(BTN_EAST)) {
			buttons_port = sel;
			run_buttons_port();
			if (quit_requested)
				return;
		} else if (was_pressed(BTN_SOUTH) || menu_requested) {
			menu_requested = false;
			return;
		}
		draw_buttons_choice(sel);
	}
}

static void dips_note(void)
{
	ui_text(F_BAR, S_DIPS_NOTE, PAD_X, UI_H - PAD_Y - BAR_H / 2, COL_DIM, 0, 330);
}

static void draw_dips(int sel, int top_item)
{
	draw_dimmed_game();
	draw_bar(S_O_DIPS);
	for (int r = 0; r < ROWS && top_item + r <= ndips; r++) {
		int i = top_item + r;
		if (i == ndips) { /* última linha: reiniciar */
			draw_list_row(r, i == sel, S_DIPS_APPLY, NULL);
			continue;
		}
		const struct var *v = &vars[dips[i]];
		draw_list_row(r, i == sel, dip_label(v), dip_text(v->values[v->cur]));
	}
	draw_toast_or(dips_note);
	int right = UI_W - PAD_X;
	right -= draw_hint(right, "B", S_BACK) + 22;
	if (sel < ndips)
		draw_hint(right, "◀ ▶", S_HINT_CHANGE);
	else
		draw_hint(right, "A", S_CONFIRM);
	osd_draw(renderer, screen_w, screen_h);
	ui_screenshot_check(renderer);
	SDL_RenderPresent(renderer);
}

/* Dip switches: mudam na hora (o FBNeo lê a cada mudança); "Aplicar
 * mudanças" salva para este jogo e reinicia a placa */
static void run_dips(void)
{
	int sel = 0, top = 0;
	int dip_start[MAX_VARS];
	for (int k = 0; k < ndips; k++)
		dip_start[k] = vars[dips[k]].cur;
	for (;;) {
		poll_devices();
		if (quit_requested)
			return;
		int n = ndips + 1;
		if (was_pressed(BTN_DPAD_UP))
			sel = (sel - 1 + n) % n;
		if (was_pressed(BTN_DPAD_DOWN))
			sel = (sel + 1) % n;
		if (sel < ndips && was_pressed(BTN_DPAD_LEFT))
			var_step(&vars[dips[sel]], -1);
		if (sel < ndips && (was_pressed(BTN_DPAD_RIGHT) || was_pressed(BTN_EAST)))
			var_step(&vars[dips[sel]], 1);
		if (sel == ndips && was_pressed(BTN_EAST)) {
			/* salva para este jogo e reinicia a placa (alguns dips só são
			 * lidos no boot); assim sair depois não perde nada */
			bool changed = false;
			for (int k = 0; k < ndips; k++)
				changed |= vars[dips[k]].cur != dip_start[k];
			if (!changed) {
				show_toast(S_DIPS_NOCHANGE);
			} else if (cfg_save(true)) {
				core.reset();
				ra_reset();
				show_toast(S_DIPS_APPLIED); /* aparece nas Opções */
				return;
			} else {
				show_toast(S_SAVE_FAILED);
			}
		}
		if (was_pressed(BTN_SOUTH) || menu_requested) {
			menu_requested = false;
			return;
		}
		if (sel < top)
			top = sel;
		if (sel >= top + ROWS)
			top = sel - ROWS + 1;
		draw_dips(sel, top);
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
			if (k != OPT_BUTTONS && k != OPT_DIPS && k < OPT_SAVE_GAME)
				option_change(sel, 1);
			else if (k == OPT_BUTTONS)
				run_buttons();
			else if (k == OPT_DIPS)
				run_dips();
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

/* ---- RetroAchievements: avisos e lista ------------------------------- */

#define RA_NOTES 4
#define RA_NOTE_MS 5000
static struct {
	char title[128], text[192], image[200];
	SDL_Texture *tex;
	Uint32 next_try;
} ra_notes[RA_NOTES];
static int ra_nnotes;
static Uint32 ra_note_start;

static void ra_note_drop(void)
{
	if (ra_notes[0].tex)
		SDL_DestroyTexture(ra_notes[0].tex);
	memmove(&ra_notes[0], &ra_notes[1], sizeof(ra_notes[0]) * (RA_NOTES - 1));
	memset(&ra_notes[RA_NOTES - 1], 0, sizeof(ra_notes[0]));
	ra_nnotes--;
	ra_note_start = SDL_GetTicks();
}

static void ra_note(const char *title, const char *text, const char *image)
{
	if (ra_nnotes == RA_NOTES) /* cheio: o mais antigo sai */
		ra_note_drop();
	int i = ra_nnotes++;
	snprintf(ra_notes[i].title, sizeof(ra_notes[i].title), "%s", title ? title : "");
	snprintf(ra_notes[i].text, sizeof(ra_notes[i].text), "%s", text ? text : "");
	snprintf(ra_notes[i].image, sizeof(ra_notes[i].image), "%s", image ? image : "");
	ra_notes[i].tex = NULL;
	ra_notes[i].next_try = 0;
	if (i == 0)
		ra_note_start = SDL_GetTicks();
}

/* Notificação em cima: caixa escura com borda branca, a imagem da conquista
 * à esquerda (quando há) e o texto ao lado; desliza de cima e volta */
static void ra_draw_note(void)
{
	if (!ra_nnotes)
		return;
	Uint32 now = SDL_GetTicks();
	if (now - ra_note_start >= RA_NOTE_MS) {
		ra_note_drop();
		if (!ra_nnotes)
			return;
	}
	typeof(ra_notes[0]) *n = &ra_notes[0];
	/* a imagem pode chegar depois (download): tenta de tempos em tempos */
	if (n->image[0] && !n->tex && now >= n->next_try) {
		n->tex = img_load(renderer, n->image, 0);
		n->next_try = now + 500;
	}
	const int img = 64, pad = 14;
	bool has_img = n->image[0] != 0;
	int text_w = SDL_max(ui_text_width(F_BAR, n->title),
			     n->text[0] ? ui_text_width(F_ITEM_SEL, n->text) : 0);
	int w = (has_img ? img + pad : 0) + text_w + 2 * pad + 6;
	w = SDL_min(w, UI_W - 2 * PAD_X);
	int h = has_img ? img + 2 * pad : (n->text[0] ? 72 : 48);
	/* entra e sai deslizando (250 ms) */
	Uint32 t = now - ra_note_start;
	int slide = 0;
	if (t < 250)
		slide = (int)((250 - t) * (h + PAD_Y) / 250);
	else if (t > RA_NOTE_MS - 250)
		slide = (int)((t - (RA_NOTE_MS - 250)) * (h + PAD_Y) / 250);
	int x = (UI_W - w) / 2, y = PAD_Y - slide;
	ui_round_rect(x, y, w, h, 16, 0xF0F0F0);           /* borda */
	ui_round_rect(x + 3, y + 3, w - 6, h - 6, 13, 0x0C0C0C); /* fundo */
	int tx = x + pad + 3;
	if (has_img) {
		SDL_Rect r = { tx, y + pad, img, img };
		if (n->tex) {
			SDL_RenderCopy(renderer, n->tex, NULL, &r);
		} else { /* ainda baixando: um quadro discreto no lugar */
			ui_round_rect(r.x, r.y, img, img, 8, 0x2A2A2A);
		}
		tx += img + pad;
	}
	int avail = x + w - pad - 3 - tx;
	if (n->text[0]) {
		ui_text(F_BAR, n->title, tx, y + h / 2 - 14, COL_DIM, 0, avail);
		ui_text(F_ITEM_SEL, n->text, tx, y + h / 2 + 12, COL_TEXT, 0, avail);
	} else {
		ui_text(F_ITEM_SEL, n->title, tx, y + h / 2, COL_TEXT, 0, avail);
	}
}

#define RA_LIST_MAX 400

/* Ordem da lista: por pontos (no RetroAchievements, a pontuação acompanha a
 * dificuldade), as mais fáceis primeiro; empate pelo nome */
static int cheevo_cmp(const void *a, const void *b)
{
	const struct ra_cheevo *x = a, *y = b;
	if (x->points != y->points)
		return x->points < y->points ? -1 : 1;
	return strcasecmp(x->title, y->title);
}

/* Caixa de texto embaixo, como a dos diálogos de RPG: quebra por palavra,
 * até max_lines linhas; o que não couber termina em "…" */
static void dialog_box(const char *text, int x, int y, int w, int max_lines, enum font_id f,
		       int line_h)
{
	int h = max_lines * line_h + 28;
	ui_round_rect(x, y, w, h, 14, 0xD8D8D8);
	ui_round_rect(x + 3, y + 3, w - 6, h - 6, 11, 0x101010);
	int inner = w - 2 * 22, ly = y + 14 + line_h / 2;
	const char *p = text;
	for (int line = 0; line < max_lines && *p; line++) {
		while (*p == ' ')
			p++;
		/* o máximo de palavras que cabe na linha */
		char buf[512];
		size_t best = 0, len = strlen(p);
		for (size_t i = 0; i <= len && i < sizeof(buf) - 1; i++) {
			if (p[i] != ' ' && p[i] != 0)
				continue;
			memcpy(buf, p, i);
			buf[i] = 0;
			if (ui_text_width(f, buf) > inner)
				break;
			best = i;
			if (!p[i])
				break;
		}
		if (best == 0) /* palavra maior que a linha: corta nela */
			best = SDL_min(len, sizeof(buf) - 1);
		bool last = line == max_lines - 1;
		memcpy(buf, p, best);
		buf[best] = 0;
		if (last && p[best]) {
			/* sobrou texto: a última linha leva o resto, cortado com "…" */
			snprintf(buf, sizeof(buf), "%s", p);
		}
		ui_text(f, buf, x + 22, ly, COL_TEXT, 0, inner);
		p += best;
		ly += line_h;
	}
}

static void run_cheevos(void)
{
	static struct ra_cheevo list[RA_LIST_MAX];
	static SDL_Texture *tex[RA_LIST_MAX];
	unsigned got, total;
	int n = ra_list(list, RA_LIST_MAX, &got, &total);
	qsort(list, n, sizeof(*list), cheevo_cmp);
	memset(tex, 0, sizeof(tex));
	Uint32 next_try = 0;
	int sel = 0, top = 0;
	char head[96];
	snprintf(head, sizeof(head), S_RA_SUMMARY, got, total);
	const int line_h = 28, box_lines = 3, box_h = box_lines * line_h + 28;
	const int box_y = UI_H - PAD_Y - BAR_H - 8 - box_h;
	const int list_y = PAD_Y + BAR_H + LIST_GAP;
	const int rows = SDL_max(1, (box_y - 8 - list_y) / (ROW_H + ROW_GAP));
	for (;;) {
		poll_devices();
		ra_idle();
		if (quit_requested)
			break;
		if (n && was_pressed(BTN_DPAD_UP))
			sel = (sel - 1 + n) % n;
		if (n && was_pressed(BTN_DPAD_DOWN))
			sel = (sel + 1) % n;
		if (n && was_pressed(BTN_DPAD_LEFT))
			sel = SDL_max(0, sel - rows);
		if (n && was_pressed(BTN_DPAD_RIGHT))
			sel = SDL_min(n - 1, sel + rows);
		if (was_pressed(BTN_SOUTH) || menu_requested) {
			menu_requested = false;
			break;
		}
		if (sel < top)
			top = sel;
		if (sel >= top + rows)
			top = sel - rows + 1;
		/* imagens das linhas visíveis (as que ainda estão baixando: de novo daqui a pouco) */
		if (SDL_GetTicks() >= next_try) {
			for (int r = 0; r < rows && top + r < n; r++)
				if (!tex[top + r])
					tex[top + r] = img_load(renderer, list[top + r].image,
								!list[top + r].unlocked);
			next_try = SDL_GetTicks() + 700;
		}

		draw_dimmed_game();
		draw_bar(S_M_CHEEVOS);
		/* "12 de 110 conquistas" logo depois do título, como "707 jogos" nas
		 * listas (à direita fica a bateria) */
		ui_text(F_BAR, head, PAD_X + ui_text_width(F_BAR_BOLD, S_M_CHEEVOS) + 10,
			PAD_Y + BAR_H / 2, COL_DIM, 0, 0);
		for (int r = 0; r < rows && top + r < n; r++) {
			const struct ra_cheevo *c = &list[top + r];
			char pts[24];
			snprintf(pts, sizeof(pts), "%s%u", c->unlocked ? "✓  " : "", c->points);
			int y = list_y + r * (ROW_H + ROW_GAP);
			bool s = top + r == sel;
			if (s)
				ui_round_rect(PAD_X, y, UI_W - 2 * PAD_X, ROW_H, ROW_RADIUS, COL_SEL_BG);
			Uint32 fg = s ? COL_SEL_FG : (c->unlocked ? COL_TEXT : COL_DIM);
			int vw = ui_text(F_COUNT, pts, UI_W - PAD_X - ROW_PAD, y + ROW_H / 2, fg, 1, 0) + 16;
			int ix = PAD_X + ROW_PAD - 4, isz = ROW_H - 10;
			SDL_Texture *t = tex[top + r];
			if (t) { /* bloqueada: em preto e branco; conquistada: colorida */
				SDL_RenderCopy(renderer, t, NULL, &(SDL_Rect){ ix, y + 5, isz, isz });
			} else {
				ui_round_rect(ix, y + 5, isz, isz, 6, s ? 0x9A9A9A : 0x2A2A2A);
			}
			int tx = ix + isz + 12;
			ui_text(s ? F_ITEM_SEL : F_ITEM, c->title, tx, y + ROW_H / 2, fg, 0,
				UI_W - PAD_X - ROW_PAD - vw - tx);
		}
		if (n)
			dialog_box(list[sel].description, PAD_X, box_y, UI_W - 2 * PAD_X, box_lines, F_ITEM,
				   line_h);
		int right = UI_W - PAD_X;
		right -= draw_hint(right, "B", S_BACK) + 22;
		if (n > rows)
			draw_hint(right, "◀ ▶", S_RA_PAGE);
		osd_draw(renderer, screen_w, screen_h);
		ui_screenshot_check(renderer);
		SDL_RenderPresent(renderer);
	}
	for (int i = 0; i < n; i++)
		if (tex[i])
			SDL_DestroyTexture(tex[i]);
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
			if (id == MI_CHEEVOS) {
				run_cheevos();
				continue;
			}
			if (id == MI_LOAD && ra_hardcore()) {
				show_toast(S_RA_NO_LOAD);
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
static void term_handler(int sig)
{
	(void)sig;
	quit_requested = true;
}

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

	/* Login do RetroAchievements pela tela do launcher (senha pela entrada) */
	if (argc == 3 && !strcmp(argv[1], "--ra-login"))
		return ra_login_cli(argv[2]);
	if (argc < 3) {
		fprintf(stderr, "uso: %s <core_libretro.so> <rom> [console] [resume]\n", argv[0]);
		return 2;
	}
	/* desligamento pelo sistema: sai como pelo menu (SRAM, recordes) */
	signal(SIGTERM, term_handler);
	signal(SIGINT, term_handler);
	signal(SIGSEGV, crash_handler);
	signal(SIGBUS, crash_handler);
	signal(SIGABRT, crash_handler);
	lang_load();
	ui_screenshot_init();
	names_from_path(argv[2], argc > 3 ? argv[3] : NULL);
	ambi_init();
	bool resume = argc > 4 && !strcmp(argv[4], "resume");
	snprintf(resume_core, sizeof(resume_core), "%s", argv[1]);
	snprintf(resume_rom, sizeof(resume_rom), "%s", argv[2]);
	snprintf(system_dir, sizeof(system_dir), "%s/Bios", DATA_ROOT);
	snprintf(save_dir, sizeof(save_dir), "%s/Saves/%s", DATA_ROOT, console_code);
	mkdir_p(save_dir);
	binds_default();
	screen_init();
	game_vol_default();
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
	/* RetroAchievements antes de carregar o jogo: no hardcore, opções
	 * proibidas (algumas só valem na carga, como a região) voltam ao padrão */
	ra_set_notify(ra_note);
	bool ra_on = ra_start(console_code, sysinfo.library_name, sysinfo.library_version);
	vars_enforce_hardcore();

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
	/* PlayStation: controle DualShock. Começa no modo digital e o jogo que
	 * conhece o DualShock liga o analógico sozinho (L1+R1+Select alterna) */
	if (!strcmp(sysinfo.library_name, "PCSX-ReARMed")) {
		void (*set_port)(unsigned, unsigned);
		*(void **)&set_port = dlsym(core.handle, "retro_set_controller_port_device");
		if (set_port)
			set_port(0, RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_ANALOG, 1));
	}
	if (ra_on)
		ra_load_game(game.path, game.data, game.size, core.get_memory_data, core.get_memory_size);
	core.get_system_av_info(&av);
	aspect = av.geometry.aspect_ratio;
	core_rate = av.timing.sample_rate;
	double fps = av.timing.fps > 0 ? av.timing.fps : 60;
	/* perto de 60 Hz: um quadro do core por vsync; senão, acumula tempo */
	bool lockstep = fps > 58.5 && fps < 61.5;
	double runs_per_sec = lockstep ? 60.0 : fps;
	audio_step = core_rate * (runs_per_sec / fps) / AUDIO_RATE;
	fprintf(stderr, "pingo-retro: %ux%u, %.2f fps, som %.0f Hz, %s%s\n", av.geometry.base_width,
		av.geometry.base_height, fps, core_rate, lockstep ? "1 quadro por vsync" : "acumulando",
		rotation ? ", tela girada" : "");

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
	stick_ranges();
	rumble_init();
	vol_fd = open_input("gpio-keys-volume");
	power_fd = open_input("axp20x-pek");
	open_keyboards();
	osd_init();

	core.set_video_refresh(video_refresh);
	core.set_audio_sample(audio_sample);
	core.set_audio_sample_batch(audio_batch);
	core.set_input_poll(input_poll);
	core.set_input_state(input_state);

	sram_load();
	slots_refresh();
	slot_pick_latest();
	if (resume && !ra_hardcore()) {
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
			rumble_stop();
			if (!run_menu())
				break;
		}
		if (suspend_requested) {
			suspend_requested = false;
			rumble_stop();
			do_suspend();
		}

		if (lockstep) {
			core.run();
			ra_frame();
		} else {
			acc += fps / 60.0;
			while (acc >= 1.0) {
				core.run();
				ra_frame();
				acc -= 1.0;
			}
		}
		rumble_frame();
		draw_game();
		ambi_frame();
		ui_logical(true); /* avisos por cima do jogo, em 640x480 */
		ra_draw_note();
		if (toast[0] && SDL_GetTicks() < toast_until) {
			int w = ui_text_width(F_BAR, toast) + 2 * ROW_PAD;
			ui_round_rect((UI_W - w) / 2, PAD_Y, w, BAR_H + 4, ROW_RADIUS, 0x000000);
			ui_text(F_BAR, toast, UI_W / 2, PAD_Y + (BAR_H + 4) / 2, COL_TEXT, 2, 0);
		}
		osd_draw(renderer, screen_w, screen_h);
		ui_screenshot_check(renderer);
		SDL_RenderPresent(renderer);

		frames++;
		Uint32 now = SDL_GetTicks();
		/* jogo do SD2 e o cartão saiu: pausa no menu e avisa */
		if (frames % 60 == 0 && !strncmp(resume_rom, "/mnt/sd2/", 9) &&
		    access(resume_rom, F_OK) != 0) {
			fprintf(stderr, "pingo-retro: SD2 removido com o jogo aberto\n");
			show_toast(S_SD2_REMOVED);
			toast_until = SDL_GetTicks() + 600000; /* fica até o cartão voltar */
			menu_requested = true;
		}
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
			/* nível em % do máximo (100 % = 32767) */
			double rms = audio_n ? sqrt(audio_sq_sum / audio_n) : 0;
			fprintf(stderr, "pingo-retro: %.1f fps, som na fila %u bytes, pico %d%%, média %.1f%%\n",
				frames * 1000.0 / (now - t0), audio_dev ? SDL_GetQueuedAudioSize(audio_dev) : 0,
				audio_peak * 100 / 32767, rms * 100 / 32767);
			audio_peak = 0;
			audio_sq_sum = 0;
			audio_n = 0;
			t0 = now;
			frames = 0;
		}
	}

	rumble_stop();
	if (power_off_requested)
		save_for_resume();
	else
		clear_resume();
	sram_save();
	if (ambi_on)
		leds_cmd("ambi-end"); /* as luzes voltam às cores escolhidas */
	ra_stop(); /* entrega o que ainda estava a caminho */
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
