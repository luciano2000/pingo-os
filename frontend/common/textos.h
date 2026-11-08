/* SPDX-License-Identifier: GPL-2.0-only */
/* Textos da interface: cada S_NOME é um ponteiro, em português por padrão,
 * trocado pelo arquivo do idioma escolhido (lang_load). */
#ifndef PINGO_TEXTOS_H
#define PINGO_TEXTOS_H

#include <stdbool.h>

#define STR(id, text) extern const char *id;
#include "textos_pt.h"
#undef STR

/* Idiomas: o embutido (pt) e os arquivos <código>.txt em
 * /usr/share/pingo/idiomas (da imagem) e /mnt/sd1/Idiomas (da comunidade,
 * vale mais). A escolha fica em /mnt/sd1/Config/idioma.txt. */
#define LANG_MAX 32
struct lang {
	char code[16];
	char name[48];
};

void lang_load(void);                   /* aplica o idioma salvo */
int lang_list(struct lang *out, int max); /* pt primeiro */
const char *lang_current(void);         /* código: "pt", "en"... */
bool lang_set(const char *code);        /* salva e aplica */
bool lang_is_pt(void);

#endif
