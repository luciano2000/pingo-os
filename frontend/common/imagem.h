/* SPDX-License-Identifier: GPL-2.0-only */
/* PNG -> textura (stb_image, domínio público/MIT) */
#ifndef PINGO_IMAGEM_H
#define PINGO_IMAGEM_H

#include <SDL2/SDL.h>

/* NULL se o arquivo não existe ou não é um PNG válido */
SDL_Texture *img_load(SDL_Renderer *ren, const char *path);

#endif
