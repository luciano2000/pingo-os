/*
 * Copia a tela atual para o framebuffer do sistema (/dev/fb0).
 *
 * Copyright (C) 2026 Pingo OS by Retrogamer Brasil
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "fbcopy.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int read_sys(const char *file, int *a, int *b)
{
	char path[128];
	snprintf(path, sizeof(path), "/sys/class/graphics/fb0/%s", file);
	FILE *f = fopen(path, "r");
	int n = 0;
	if (f) {
		n = b ? fscanf(f, "%d,%d", a, b) : fscanf(f, "%d", a);
		fclose(f);
	}
	return n;
}

/* Geometria do fb0: 32 bits por pixel (H700) ou 16, RGB565 (vc4 da Raspberry) */
static int fb_bpp;
static bool fb_geometry(int *w, int *h, int *stride)
{
	int bpp = 0;
	if (read_sys("virtual_size", w, h) != 2 || read_sys("bits_per_pixel", &bpp, NULL) != 1 ||
	    read_sys("stride", stride, NULL) != 1)
		return false;
	fb_bpp = bpp;
	return (bpp == 32 || bpp == 16) && *w > 0 && *h > 0 && *stride >= *w * (bpp / 8);
}

void fb_copy_from(SDL_Renderer *ren)
{
	int w, h, stride, rw, rh;
	if (!fb_geometry(&w, &h, &stride) || SDL_GetRendererOutputSize(ren, &rw, &rh) < 0)
		return;
	if (rw < w)
		w = rw;
	if (rh < h)
		h = rh;

	/* XRGB8888 little-endian no fb0 = ARGB8888 do SDL; em 16 bits, RGB565 */
	Uint8 *px = malloc((size_t)stride * h);
	if (!px)
		return;
	SDL_Rect r = { 0, 0, w, h };
	Uint32 fmt = fb_bpp == 16 ? SDL_PIXELFORMAT_RGB565 : SDL_PIXELFORMAT_ARGB8888;
	if (SDL_RenderReadPixels(ren, &r, fmt, px, stride) == 0) {
		int fd = open("/dev/fb0", O_WRONLY);
		if (fd >= 0) {
			if (write(fd, px, (size_t)stride * h) < 0)
				perror("fb0");
			close(fd);
		}
	}
	free(px);
}

void fb_clear(void)
{
	int w, h, stride;
	if (!fb_geometry(&w, &h, &stride))
		return;
	Uint8 *z = calloc(1, (size_t)stride * h);
	int fd = open("/dev/fb0", O_WRONLY);
	if (z && fd >= 0 && write(fd, z, (size_t)stride * h) < 0)
		perror("fb0");
	if (fd >= 0)
		close(fd);
	free(z);
}
