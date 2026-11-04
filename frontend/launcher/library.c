/*
 * Biblioteca de jogos: consoles, árvore de pastas e cache.
 *
 * Copyright (C) 2026 Pingo OS by Retrogamer Brasil
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "library.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Ordem livre aqui: a tela inicial ordena pelo nome mostrado */
const struct console consoles[] = {
	{ "GB",       "Game Boy",          "gb zip",                  "gambatte" },
	{ "GBC",      "Game Boy Color",    "gbc gb zip",              "gambatte" },
	{ "GBA",      "Game Boy Advance",  "gba zip",                 "mgba" },
	{ "NES",      "Nintendo",          "nes fds unf unif zip",    "fceumm" },
	{ "SNES",     "Super Nintendo",    "sfc smc swc fig bs zip",  "snes9x" },
	{ "VB",       "Virtual Boy",       "vb vboy zip",             "mednafen_vb" },
	{ "POKEMINI", "Pokémon mini",      "min zip",                 "pokemini" },
	{ "MS",       "Master System",     "sms zip",                 "genesis_plus_gx" },
	{ "GG",       "Game Gear",         "gg zip",                  "genesis_plus_gx" },
	{ "MD",       "Mega Drive",        "md gen smd bin zip",      "genesis_plus_gx" },
	{ "SEGACD",   "Sega CD",           "chd cue iso m3u",         "genesis_plus_gx" },
	{ "32X",      "32X",               "32x zip",                 "picodrive" },
	{ "PCE",      "PC Engine",         "pce zip",                 "mednafen_pce_fast" },
	{ "PCECD",    "PC Engine CD",      "chd cue ccd m3u",         "mednafen_pce_fast" },
	{ "NGP",      "Neo Geo Pocket",    "ngp ngc zip",             "mednafen_ngp" },
	{ "WS",       "WonderSwan",        "ws wsc zip",              "mednafen_wswan" },
	{ "MSX",      "MSX",               "rom mx1 mx2 dsk zip",     "bluemsx" },
	{ "A2600",    "Atari 2600",        "a26 bin zip",             "stella2014" },
	{ "A7800",    "Atari 7800",        "a78 zip",                 "prosystem" },
	{ "LYNX",     "Atari Lynx",        "lnx zip",                 "handy" },
	{ "PS",       "PlayStation",       "chd cue pbp m3u",         "pcsx_rearmed" },
	{ "ARCADE",   "Arcade",            "zip",                     "fbneo" },
};
const int n_consoles = sizeof(consoles) / sizeof(consoles[0]);

/* ---- nomes ----------------------------------------------------------- */

int natural_cmp(const char *a, const char *b)
{
	while (*a && *b) {
		if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
			/* compara números pelo valor: pula zeros, depois tamanho */
			while (*a == '0')
				a++;
			while (*b == '0')
				b++;
			const char *ea = a, *eb = b;
			while (isdigit((unsigned char)*ea))
				ea++;
			while (isdigit((unsigned char)*eb))
				eb++;
			if (ea - a != eb - b)
				return (int)(ea - a) - (int)(eb - b);
			int c = strncmp(a, b, ea - a);
			if (c)
				return c;
			a = ea;
			b = eb;
			continue;
		}
		int ca = tolower((unsigned char)*a), cb = tolower((unsigned char)*b);
		if (ca != cb)
			return ca - cb;
		a++;
		b++;
	}
	return (unsigned char)*a - (unsigned char)*b;
}

char *library_clean_name(const char *fname, bool is_dir)
{
	size_t len = strlen(fname);
	char *out = malloc(len + 1);
	const char *end = fname + len;
	size_t o = 0;
	int depth = 0;

	if (!is_dir) {
		const char *dot = strrchr(fname, '.');
		if (dot && dot != fname)
			end = dot;
	}
	/* tira o que estiver entre () e [] */
	for (const char *p = fname; p < end; p++) {
		if (*p == '(' || *p == '[') {
			depth++;
			continue;
		}
		if ((*p == ')' || *p == ']') && depth > 0) {
			depth--;
			continue;
		}
		if (depth == 0)
			out[o++] = *p;
	}
	out[o] = 0;

	/* espaços: junta repetidos e tira das pontas */
	size_t w = 0;
	for (size_t r = 0; out[r]; r++) {
		if (out[r] == ' ' && (w == 0 || out[w - 1] == ' '))
			continue;
		out[w++] = out[r];
	}
	while (w > 0 && out[w - 1] == ' ')
		w--;
	out[w] = 0;

	if (w == 0) { /* nome só com tags: mostra o original */
		free(out);
		out = strndup(fname, end - fname);
	}
	return out;
}

static bool ext_ok(const struct console *c, const char *fname)
{
	const char *dot = strrchr(fname, '.');
	char ext[16];
	size_t n;

	if (!dot || dot == fname)
		return false;
	n = strlen(dot + 1);
	if (n == 0 || n >= sizeof(ext))
		return false;
	for (size_t i = 0; i <= n; i++)
		ext[i] = (char)tolower((unsigned char)dot[1 + i]);

	for (const char *p = c->exts; *p;) {
		const char *sp = strchr(p, ' ');
		size_t l = sp ? (size_t)(sp - p) : strlen(p);
		if (l == n && !strncmp(p, ext, n))
			return true;
		p += l;
		while (*p == ' ')
			p++;
	}
	return false;
}

/* ---- árvore ---------------------------------------------------------- */

static struct node *node_new(const char *fname, const char *path, int console, bool dir)
{
	struct node *n = calloc(1, sizeof(*n));
	n->fname = strdup(fname);
	n->path = strdup(path);
	n->label = library_clean_name(fname, dir);
	n->console = console;
	n->dir = dir;
	n->games = dir ? 0 : 1;
	return n;
}

static void node_add(struct node *dir, struct node *kid)
{
	dir->kids = realloc(dir->kids, sizeof(*dir->kids) * (dir->nkids + 1));
	dir->kids[dir->nkids++] = kid;
	dir->games += kid->games;
}

static int kid_cmp(const void *a, const void *b)
{
	const struct node *x = *(const struct node *const *)a, *y = *(const struct node *const *)b;
	int c = natural_cmp(x->label, y->label);
	return c ? c : natural_cmp(x->fname, y->fname);
}

void library_free(struct node *n)
{
	if (!n)
		return;
	for (int i = 0; i < n->nkids; i++)
		library_free(n->kids[i]);
	free(n->kids);
	free(n->fname);
	free(n->label);
	free(n->path);
	free(n);
}

static void scan_dir(struct node *dir, int console)
{
	DIR *d = opendir(dir->path);
	struct dirent *e;

	if (!d)
		return;
	while ((e = readdir(d))) {
		/* ocultos (inclui os "._" do macOS) */
		if (e->d_name[0] == '.')
			continue;

		char path[4096];
		snprintf(path, sizeof(path), "%s/%s", dir->path, e->d_name);

		bool is_dir = e->d_type == DT_DIR;
		if (e->d_type == DT_UNKNOWN || e->d_type == DT_LNK) {
			struct stat st;
			if (stat(path, &st) < 0)
				continue;
			is_dir = S_ISDIR(st.st_mode);
		}

		if (is_dir) {
			struct node *sub = node_new(e->d_name, path, console, true);
			scan_dir(sub, console);
			if (sub->games > 0) /* pasta sem jogos fica oculta */
				node_add(dir, sub);
			else
				library_free(sub);
		} else if (ext_ok(&consoles[console], e->d_name)) {
			node_add(dir, node_new(e->d_name, path, console, false));
		}
	}
	closedir(d);
	qsort(dir->kids, dir->nkids, sizeof(*dir->kids), kid_cmp);
}

static int console_cmp(const void *a, const void *b)
{
	const struct node *x = *(const struct node *const *)a, *y = *(const struct node *const *)b;
	return natural_cmp(consoles[x->console].name, consoles[y->console].name);
}

static struct node *root_new(void)
{
	return node_new("", ROMS_DIR, -1, true);
}

static struct node *console_node(int c)
{
	char path[512];
	snprintf(path, sizeof(path), "%s/%s", ROMS_DIR, consoles[c].code);
	struct node *n = node_new(consoles[c].code, path, c, true);
	free(n->label);
	n->label = strdup(consoles[c].name);
	return n;
}

struct node *library_scan(void)
{
	struct node *root = root_new();

	for (int c = 0; c < n_consoles; c++) {
		struct node *cn = console_node(c);
		scan_dir(cn, c);
		if (cn->games > 0)
			node_add(root, cn);
		else
			library_free(cn);
	}
	qsort(root->kids, root->nkids, sizeof(*root->kids), console_cmp);
	return root;
}

int library_find_kid(const struct node *dir, const char *path)
{
	if (!dir || !path)
		return -1;
	for (int i = 0; i < dir->nkids; i++)
		if (!strcmp(dir->kids[i]->path, path))
			return i;
	return -1;
}

struct node *library_game_node(int console, const char *path)
{
	const char *slash = strrchr(path, '/');
	return node_new(slash ? slash + 1 : path, path, console, false);
}

bool library_equal(const struct node *a, const struct node *b)
{
	if (!a || !b)
		return a == b;
	if (a->dir != b->dir || a->nkids != b->nkids || a->console != b->console ||
	    strcmp(a->fname, b->fname))
		return false;
	for (int i = 0; i < a->nkids; i++)
		if (!library_equal(a->kids[i], b->kids[i]))
			return false;
	return true;
}

/* ---- cache ----------------------------------------------------------- *
 * Uma linha por nó, em pré-ordem: "<profundidade>\t<D|F>\t<console>\t<nome>".
 * Os consoles ficam na profundidade 1. Escrito em arquivo temporário e
 * renomeado, para um desligamento no meio não deixar cache pela metade.
 */

static void save_node(FILE *f, const struct node *n, int depth)
{
	fprintf(f, "%d\t%c\t%d\t%s\n", depth, n->dir ? 'D' : 'F', n->console, n->fname);
	for (int i = 0; i < n->nkids; i++)
		save_node(f, n->kids[i], depth + 1);
}

bool library_save_cache(const struct node *root, const char *path)
{
	char tmp[4096];
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	FILE *f = fopen(tmp, "w");
	if (!f)
		return false;
	fprintf(f, "pingo-library 1\n");
	for (int i = 0; i < root->nkids; i++)
		save_node(f, root->kids[i], 1);
	if (fflush(f) || fsync(fileno(f)) || fclose(f)) {
		unlink(tmp);
		return false;
	}
	return rename(tmp, path) == 0;
}

static int count_games(struct node *n)
{
	if (!n->dir)
		return n->games = 1;
	n->games = 0;
	for (int i = 0; i < n->nkids; i++)
		n->games += count_games(n->kids[i]);
	return n->games;
}

struct node *library_load_cache(const char *path)
{
	FILE *f = fopen(path, "r");
	char line[4096];
	struct node *stack[64];

	if (!f)
		return NULL;
	if (!fgets(line, sizeof(line), f) || strcmp(line, "pingo-library 1\n")) {
		fclose(f);
		return NULL;
	}

	struct node *root = root_new();
	stack[0] = root;
	int top = 0;
	bool ok = true;

	while (fgets(line, sizeof(line), f)) {
		char *nl = strchr(line, '\n');
		if (!nl) {
			ok = false;
			break;
		}
		*nl = 0;
		int depth, console;
		char type;
		int off = 0;
		if (sscanf(line, "%d\t%c\t%d\t%n", &depth, &type, &console, &off) != 3 || !off ||
		    depth < 1 || depth > top + 1 || depth >= 64 || console < 0 || console >= n_consoles) {
			ok = false;
			break;
		}
		const char *name = line + off;
		struct node *parent = stack[depth - 1];
		struct node *n;

		if (depth == 1) {
			n = console_node(console);
		} else {
			char p[4096];
			snprintf(p, sizeof(p), "%s/%s", parent->path, name);
			n = node_new(name, p, console, type == 'D');
		}
		/* o total de jogos é refeito ao subir */
		n->games = n->dir ? 0 : 1;
		parent->kids = realloc(parent->kids, sizeof(*parent->kids) * (parent->nkids + 1));
		parent->kids[parent->nkids++] = n;
		stack[depth] = n;
		top = depth;
	}
	fclose(f);
	if (!ok) {
		library_free(root);
		return NULL;
	}

	count_games(root);
	return root;
}
