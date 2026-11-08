/* SPDX-License-Identifier: GPL-2.0-only */
/* Textos da interface e troca de idioma em tempo de execução. */
#include "textos.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define STR(id, text) const char *id = text;
#include "textos_pt.h"
#undef STR

static struct entry {
	const char *key; /* sem o "S_" */
	const char **ptr;
	const char *def;
} entries[] = {
#define STR(id, text) { #id + 2, &id, text },
#include "textos_pt.h"
#undef STR
};
#define NENTRIES ((int)(sizeof(entries) / sizeof(*entries)))

#ifndef LANG_DIR_SYS
#define LANG_DIR_SYS  "/usr/share/pingo/idiomas"
#endif
#ifndef LANG_DIR_USER
#define LANG_DIR_USER "/mnt/sd1/Idiomas"
#endif
#ifndef LANG_FILE
#define LANG_FILE     "/mnt/sd1/Config/idioma.txt"
#endif

static char current[16] = "pt";
static char *loaded[512]; /* textos alocados do idioma atual */

/* Sequência das conversões de printf ("%d%s"): a tradução tem de ter as
 * mesmas, na mesma ordem, senão o texto seria lido errado (e poderia
 * derrubar o programa) */
static void conversions(const char *s, char *out, size_t n)
{
	size_t o = 0;
	for (; *s && o + 1 < n; s++) {
		if (*s != '%')
			continue;
		s++;
		if (*s == '%')
			continue;
		while (*s && strchr("-+ #0123456789.lhzjt*", *s))
			s++;
		if (!*s)
			break;
		out[o++] = *s;
	}
	out[o] = 0;
}

static void reset(void)
{
	for (int i = 0; i < NENTRIES; i++) {
		*entries[i].ptr = entries[i].def;
		free(loaded[i]);
		loaded[i] = NULL;
	}
}

static bool path_for(const char *code, char *out, size_t n)
{
	const char *dirs[2] = { LANG_DIR_USER, LANG_DIR_SYS };
	for (int d = 0; d < 2; d++) {
		snprintf(out, n, "%s/%s.txt", dirs[d], code);
		if (access(out, R_OK) == 0)
			return true;
	}
	return false;
}

/* "\n" no arquivo vira quebra de linha */
static void unescape(char *s)
{
	char *w = s;
	for (; *s; s++) {
		if (s[0] == '\\' && s[1] == 'n') {
			*w++ = '\n';
			s++;
		} else {
			*w++ = *s;
		}
	}
	*w = 0;
}

static void apply_file(const char *path)
{
	FILE *f = fopen(path, "r");
	char line[1024];
	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = 0;
		char *eq = strchr(line, '=');
		if (line[0] == '#' || !eq || eq == line)
			continue;
		*eq = 0;
		char *val = eq + 1;
		unescape(val);
		for (int i = 0; i < NENTRIES; i++) {
			if (strcmp(entries[i].key, line))
				continue;
			char a[32], b[32];
			conversions(entries[i].def, a, sizeof(a));
			conversions(val, b, sizeof(b));
			/* formato de data (strftime, "%H:%M"): qualquer campo é seguro */
			bool date_fmt = strstr(entries[i].def, "%H") != NULL;
			if ((!date_fmt && strcmp(a, b)) || !*val) {
				fprintf(stderr, "idioma: %s ignorado (%%: \"%s\" no original)\n", line, a);
				break;
			}
			free(loaded[i]);
			loaded[i] = strdup(val);
			if (loaded[i])
				*entries[i].ptr = loaded[i];
			break;
		}
	}
	fclose(f);
}

static void apply(const char *code)
{
	char path[256];
	reset();
	snprintf(current, sizeof(current), "%s", "pt");
	if (!strcmp(code, "pt") || !path_for(code, path, sizeof(path)))
		return;
	apply_file(path);
	snprintf(current, sizeof(current), "%s", code);
}

void lang_load(void)
{
	char code[16] = "pt";
	FILE *f = fopen(LANG_FILE, "r");
	if (f) {
		if (!fgets(code, sizeof(code), f))
			code[0] = 0;
		fclose(f);
		code[strcspn(code, "\r\n ")] = 0;
	}
	apply(code[0] ? code : "pt");
}

const char *lang_current(void)
{
	return current;
}

bool lang_is_pt(void)
{
	return !strcmp(current, "pt");
}

bool lang_set(const char *code)
{
	FILE *f = fopen(LANG_FILE ".tmp", "w");
	if (!f)
		return false;
	fprintf(f, "%s\n", code);
	fflush(f);
	fsync(fileno(f));
	fclose(f);
	if (rename(LANG_FILE ".tmp", LANG_FILE) != 0)
		return false;
	apply(code);
	return true;
}

/* Nome mostrado: a linha NOME= do arquivo (senão, o código) */
static void lang_name(const char *path, const char *code, char *out, size_t n)
{
	FILE *f = fopen(path, "r");
	char line[256];
	snprintf(out, n, "%s", code);
	if (!f)
		return;
	while (fgets(line, sizeof(line), f))
		if (!strncmp(line, "NOME=", 5)) {
			line[strcspn(line, "\r\n")] = 0;
			snprintf(out, n, "%s", line + 5);
			break;
		}
	fclose(f);
}

int lang_list(struct lang *out, int max)
{
	int n = 0;
	if (max <= 0)
		return 0;
	snprintf(out[n].code, sizeof(out[n].code), "pt");
	snprintf(out[n].name, sizeof(out[n].name), "Português (Brasil)");
	n++;
	const char *dirs[2] = { LANG_DIR_USER, LANG_DIR_SYS };
	for (int d = 0; d < 2; d++) {
		DIR *dir = opendir(dirs[d]);
		struct dirent *e;
		while (dir && (e = readdir(dir)) && n < max) {
			size_t len = strlen(e->d_name);
			if (len < 5 || len - 4 >= sizeof(out[0].code) || strcmp(e->d_name + len - 4, ".txt") ||
			    e->d_name[0] == '.')
				continue;
			char code[16];
			memcpy(code, e->d_name, len - 4);
			code[len - 4] = 0;
			bool dup = !strcmp(code, "pt");
			for (int i = 0; i < n && !dup; i++)
				dup = !strcmp(out[i].code, code);
			if (dup)
				continue;
			char path[512];
			snprintf(path, sizeof(path), "%s/%s", dirs[d], e->d_name);
			snprintf(out[n].code, sizeof(out[n].code), "%s", code);
			lang_name(path, code, out[n].name, sizeof(out[n].name));
			n++;
		}
		if (dir)
			closedir(dir);
	}
	return n;
}
