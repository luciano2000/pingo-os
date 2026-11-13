/* SPDX-License-Identifier: GPL-2.0-only */
#include "controle2.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#define DIR_CFG "/mnt/sd1/Config/controles"

const char *const c2_keys[C2_N] = {
	"cima", "baixo", "esquerda", "direita",
	"botao1", "botao2", "botao3", "botao4", "botao5", "botao6",
	"start", "ficha",
};

const char *const c2_labels[C2_N] = {
	"Cima", "Baixo", "Esquerda", "Direita",
	"Botão 1 (soco fraco)", "Botão 2 (soco médio)", "Botão 3 (soco forte)",
	"Botão 4 (chute forte)", "Botão 5 (chute fraco)", "Botão 6 (chute médio)",
	"Start", "Ficha",
};

#define BITS(n) ((n) / (8 * sizeof(long)) + 1)
static bool bit(const unsigned long *b, int n)
{
	return (b[n / (8 * sizeof(long))] >> (n % (8 * sizeof(long)))) & 1;
}

/* Gamepad/joystick, e não um teclado (a Picade e os teclados ficam com o P1) */
static bool is_pad(int fd)
{
	unsigned long keys[BITS(KEY_MAX)] = { 0 };
	if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys) < 0)
		return false;
	if (bit(keys, KEY_LEFTCTRL) || bit(keys, KEY_A))
		return false;
	for (int c = BTN_JOYSTICK; c < BTN_JOYSTICK + 16; c++)
		if (bit(keys, c))
			return true;
	for (int c = BTN_GAMEPAD; c < BTN_GAMEPAD + 16; c++)
		if (bit(keys, c))
			return true;
	return false;
}

static void bind_set(struct c2_bind *b, char type, int code, int sign)
{
	b->type = type;
	b->code = code;
	b->sign = sign;
}

static void map_default(struct c2_dev *d)
{
	unsigned long keys[BITS(KEY_MAX)] = { 0 }, abs[BITS(ABS_MAX)] = { 0 };
	ioctl(d->fd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys);
	ioctl(d->fd, EVIOCGBIT(EV_ABS, sizeof(abs)), abs);
	memset(d->map, 0, sizeof(d->map));

	/* direções: hat, se tiver; senão o primeiro analógico; botões do
	 * direcional (BTN_DPAD_*) por cima */
	int ax = bit(abs, ABS_HAT0X) ? ABS_HAT0X : ABS_X, ay = bit(abs, ABS_HAT0Y) ? ABS_HAT0Y : ABS_Y;
	bind_set(&d->map[C2_UP], 'a', ay, -1);
	bind_set(&d->map[C2_DOWN], 'a', ay, 1);
	bind_set(&d->map[C2_LEFT], 'a', ax, -1);
	bind_set(&d->map[C2_RIGHT], 'a', ax, 1);
	if (bit(keys, BTN_DPAD_UP)) {
		bind_set(&d->map[C2_UP], 'b', BTN_DPAD_UP, 0);
		bind_set(&d->map[C2_DOWN], 'b', BTN_DPAD_DOWN, 0);
		bind_set(&d->map[C2_LEFT], 'b', BTN_DPAD_LEFT, 0);
		bind_set(&d->map[C2_RIGHT], 'b', BTN_DPAD_RIGHT, 0);
	}

	if (d->vid == 0x0583 && d->pid == 0x2060) {
		/* iBuffalo Classic (de SNES): botões 1 a 6 = B A Y X L R, os
		 * nomes do retropad (Neo Geo A B C D = B A Y X) */
		bind_set(&d->map[C2_B1], 'b', BTN_THUMB, 0);   /* B */
		bind_set(&d->map[C2_B2], 'b', BTN_TRIGGER, 0); /* A */
		bind_set(&d->map[C2_B3], 'b', BTN_TOP, 0);     /* Y */
		bind_set(&d->map[C2_B4], 'b', BTN_THUMB2, 0);  /* X */
		bind_set(&d->map[C2_B5], 'b', BTN_TOP2, 0);    /* L */
		bind_set(&d->map[C2_B6], 'b', BTN_PINKIE, 0);  /* R */
		bind_set(&d->map[C2_COIN], 'b', BTN_BASE, 0);  /* Select */
		bind_set(&d->map[C2_START], 'b', BTN_BASE2, 0);
	} else if (bit(keys, BTN_SOUTH)) {
		/* gamepad moderno (Xbox, PlayStation): a mesma ideia */
		bind_set(&d->map[C2_B1], 'b', BTN_SOUTH, 0);
		bind_set(&d->map[C2_B2], 'b', BTN_EAST, 0);
		bind_set(&d->map[C2_B3], 'b', BTN_WEST, 0);
		bind_set(&d->map[C2_B4], 'b', BTN_NORTH, 0);
		bind_set(&d->map[C2_B5], 'b', BTN_TL, 0);
		bind_set(&d->map[C2_B6], 'b', BTN_TR, 0);
		bind_set(&d->map[C2_COIN], 'b', BTN_SELECT, 0);
		bind_set(&d->map[C2_START], 'b', BTN_START, 0);
	} else {
		/* joystick genérico: os botões na ordem; os dois últimos dos 8
		 * costumam ser select e start */
		int f[] = { C2_B1, C2_B2, C2_B3, C2_B4, C2_B5, C2_B6, C2_COIN, C2_START };
		int n = 0;
		for (int c = BTN_JOYSTICK; c < BTN_JOYSTICK + 16 && n < 8; c++)
			if (bit(keys, c))
				bind_set(&d->map[f[n++]], 'b', c, 0);
	}
}

static void cfg_path(const struct c2_dev *d, char *out, int n)
{
	snprintf(out, n, DIR_CFG "/%04x-%04x.cfg", d->vid, d->pid);
}

static bool parse_bind(const char *s, struct c2_bind *b)
{
	int code;
	char sign;
	if (sscanf(s, "btn:%d", &code) == 1) {
		bind_set(b, 'b', code, 0);
		return true;
	}
	if (sscanf(s, "abs:%d%c", &code, &sign) == 2 && (sign == '+' || sign == '-')) {
		bind_set(b, 'a', code, sign == '+' ? 1 : -1);
		return true;
	}
	if (!strcmp(s, "nenhum")) {
		memset(b, 0, sizeof(*b));
		return true;
	}
	return false;
}

static void map_load(struct c2_dev *d)
{
	char path[160], line[160];
	map_default(d);
	d->saved = false;
	cfg_path(d, path, sizeof(path));
	FILE *f = fopen(path, "r");
	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = 0;
		char *eq = strchr(line, '=');
		if (line[0] == '#' || !eq)
			continue;
		*eq = 0;
		for (int i = 0; i < C2_N; i++)
			if (!strcmp(line, c2_keys[i]))
				parse_bind(eq + 1, &d->map[i]);
	}
	fclose(f);
	d->saved = true;
}

bool c2_save(struct c2_dev *d)
{
	char path[160], tmp[170];
	mkdir("/mnt/sd1/Config", 0755);
	mkdir(DIR_CFG, 0755);
	cfg_path(d, path, sizeof(path));
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	FILE *f = fopen(tmp, "w");
	if (!f)
		return false;
	fprintf(f, "# %s\n", d->name);
	for (int i = 0; i < C2_N; i++) {
		const struct c2_bind *b = &d->map[i];
		if (b->type == 'b')
			fprintf(f, "%s=btn:%d\n", c2_keys[i], b->code);
		else if (b->type == 'a')
			fprintf(f, "%s=abs:%d%c\n", c2_keys[i], b->code, b->sign > 0 ? '+' : '-');
		else
			fprintf(f, "%s=nenhum\n", c2_keys[i]);
	}
	fclose(f);
	rename(tmp, path);
	sync();
	d->saved = true;
	return true;
}

bool c2_open(struct c2_dev *d)
{
	DIR *dir = opendir("/dev/input");
	struct dirent *e;
	char path[300];
	if (!dir)
		return false;
	d->fd = -1;
	while ((e = readdir(dir))) {
		if (strncmp(e->d_name, "event", 5))
			continue;
		snprintf(path, sizeof(path), "/dev/input/%s", e->d_name);
		int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0)
			continue;
		if (!is_pad(fd)) {
			close(fd);
			continue;
		}
		struct input_id id;
		memset(d, 0, sizeof(*d));
		d->fd = fd;
		if (ioctl(fd, EVIOCGNAME(sizeof(d->name)), d->name) < 0)
			strcpy(d->name, "Controle");
		/* nomes de controle às vezes têm espaços no fim */
		for (int n = (int)strlen(d->name); n > 0 && d->name[n - 1] == ' '; n--)
			d->name[n - 1] = 0;
		if (ioctl(fd, EVIOCGID, &id) == 0) {
			d->vid = id.vendor;
			d->pid = id.product;
		}
		for (int c = 0; c <= ABS_MAX; c++) {
			struct input_absinfo ai;
			d->amin[c] = -1;
			d->amax[c] = 1;
			if (ioctl(fd, EVIOCGABS(c), &ai) == 0 && ai.maximum > ai.minimum) {
				d->amin[c] = ai.minimum;
				d->amax[c] = ai.maximum;
			}
		}
		map_load(d);
		break;
	}
	closedir(dir);
	return d->fd >= 0;
}

void c2_close(struct c2_dev *d)
{
	if (d->fd >= 0)
		close(d->fd);
	d->fd = -1;
	memset(d->down, 0, sizeof(d->down));
}

/* Eixo além de ~40 % do meio, para o lado `sign` */
static bool axis_on(const struct c2_dev *d, int code, int sign, int v)
{
	int mid = (d->amin[code] + d->amax[code]) / 2, half = (d->amax[code] - d->amin[code]) / 2;
	if (half <= 1) /* hat: -1, 0, 1 */
		return v * sign > 0;
	return (v - mid) * sign > half * 4 / 10;
}

bool c2_poll(struct c2_dev *d, void (*cb)(int func, bool down))
{
	struct input_event ev;
	ssize_t r;
	if (d->fd < 0)
		return false;
	while ((r = read(d->fd, &ev, sizeof(ev))) == sizeof(ev)) {
		for (int i = 0; i < C2_N; i++) {
			const struct c2_bind *b = &d->map[i];
			bool on;
			if (b->type == 'b' && ev.type == EV_KEY && ev.code == b->code)
				on = ev.value != 0;
			else if (b->type == 'a' && ev.type == EV_ABS && ev.code == b->code)
				on = axis_on(d, b->code, b->sign, ev.value);
			else
				continue;
			if (on != d->down[i]) {
				d->down[i] = on;
				if (cb)
					cb(i, on);
			}
		}
	}
	if (r < 0 && errno != EAGAIN) { /* tirou da USB */
		for (int i = 0; i < C2_N; i++)
			if (d->down[i] && cb)
				cb(i, false);
		c2_close(d);
		return false;
	}
	return true;
}

bool c2_capture(struct c2_dev *d, struct c2_bind *out)
{
	struct input_event ev;
	ssize_t r;
	bool got = false;
	if (d->fd < 0)
		return false;
	while ((r = read(d->fd, &ev, sizeof(ev))) == sizeof(ev)) {
		if (got)
			continue; /* esvazia o resto */
		if (ev.type == EV_KEY && ev.value == 1) {
			bind_set(out, 'b', ev.code, 0);
			got = true;
		} else if (ev.type == EV_ABS && ev.code <= ABS_MAX) {
			if (axis_on(d, ev.code, 1, ev.value)) {
				bind_set(out, 'a', ev.code, 1);
				got = true;
			} else if (axis_on(d, ev.code, -1, ev.value)) {
				bind_set(out, 'a', ev.code, -1);
				got = true;
			}
		}
	}
	if (r < 0 && errno != EAGAIN)
		c2_close(d);
	return got;
}

void c2_describe(const struct c2_bind *b, char *out, int n)
{
	if (b->type == 'b')
		snprintf(out, n, "botão %d", b->code - (b->code >= BTN_JOYSTICK ? BTN_JOYSTICK - 1 : 0));
	else if (b->type == 'a')
		snprintf(out, n, "eixo %d%s", b->code, b->sign > 0 ? "+" : "−");
	else
		snprintf(out, n, "—");
}
