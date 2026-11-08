/* SPDX-License-Identifier: GPL-2.0-only */
#include "imagem.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#include "stb_image.h"
#pragma GCC diagnostic pop

SDL_Texture *img_load(SDL_Renderer *ren, const char *path, int gray)
{
	int w, h, n;
	unsigned char *px = stbi_load(path, &w, &h, &n, 4);
	if (!px)
		return NULL;
	if (gray) /* luminância (Rec. 601), um pouco mais escura */
		for (int i = 0; i < w * h; i++) {
			unsigned char *p = px + i * 4;
			int y = (p[0] * 299 + p[1] * 587 + p[2] * 114) / 1000 * 85 / 100;
			p[0] = p[1] = p[2] = (unsigned char)y;
		}
	SDL_Texture *t = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, w, h);
	if (t) {
		SDL_UpdateTexture(t, NULL, px, w * 4);
		SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
		SDL_SetTextureScaleMode(t, SDL_ScaleModeLinear);
	}
	stbi_image_free(px);
	return t;
}
