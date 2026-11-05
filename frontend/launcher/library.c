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
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

/* Ordem livre aqui: a tela inicial ordena pelo nome mostrado */
const struct console consoles[] = {
	{ "GB",       "Game Boy",          "gb zip",                  "gambatte" },
	{ "GBC",      "Game Boy Color",    "gbc gb zip",              "gambatte" },
	{ "GBA",      "Game Boy Advance",  "gba zip",                 "mgba" },
	{ "NES",      "Nintendo",          "nes fds unf unif zip",    "fceumm" },
	{ "FDS",      "Famicom Disk System", "fds zip",               "fceumm" },
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
	{ "NEOGEO",   "Neo Geo",           "zip",                     "fbneo" },
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

/* ---- nomes do arcade (DAT do FBNeo) ---------------------------------- *
 * /usr/share/pingo/arcade/fbneo.tsv: "zip<TAB>descrição<TAB>bios", em ordem
 * de nome (gerado no build a partir do DAT desta versão do FBNeo).
 */

#ifndef ARCADE_NAMES
#define ARCADE_NAMES "/usr/share/pingo/arcade/fbneo.tsv"
#endif

static struct arcade_name { char *zip, *desc; bool bios; } *arcade;
static size_t narcade;
static bool arcade_loaded;

static void arcade_load(void)
{
	FILE *f;
	char line[1024];
	size_t cap = 0;

	arcade_loaded = true;
	if (!(f = fopen(ARCADE_NAMES, "r")))
		return;
	while (fgets(line, sizeof(line), f)) {
		char *t1 = strchr(line, '\t');
		char *t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
		if (!t2)
			continue;
		*t1 = *t2 = 0;
		if (narcade == cap) {
			cap = cap ? cap * 2 : 8192;
			arcade = realloc(arcade, cap * sizeof(*arcade));
		}
		arcade[narcade++] = (struct arcade_name){ strdup(line), strdup(t1 + 1), t2[1] == '1' };
	}
	fclose(f);
}

static int arcade_cmp(const void *key, const void *el)
{
	return strcmp(key, ((const struct arcade_name *)el)->zip);
}

/* NULL se o zip não está no DAT */
static const struct arcade_name *arcade_find(const char *fname)
{
	char key[256];
	if (!arcade_loaded)
		arcade_load();
	snprintf(key, sizeof(key), "%s", fname);
	char *dot = strrchr(key, '.');
	if (dot)
		*dot = 0;
	return narcade ? bsearch(key, arcade, narcade, sizeof(*arcade), arcade_cmp) : NULL;
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

static void group_discs(struct node *dir, int console);

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
			struct node *g = node_new(e->d_name, path, console, false);
			if (!strcmp(consoles[console].core, "fbneo")) {
				const struct arcade_name *a = arcade_find(e->d_name);
				if (a && a->bios) { /* BIOS (neogeo.zip...): fica na pasta, fora da lista */
					library_free(g);
					continue;
				}
				if (a) {
					free(g->label);
					g->label = library_clean_name(a->desc, true);
				}
			}
			node_add(dir, g);
		}
	}
	closedir(d);
	if (strstr(consoles[console].exts, "m3u"))
		group_discs(dir, console);
	qsort(dir->kids, dir->nkids, sizeof(*dir->kids), kid_cmp);
}

/* ---- jogos de vários discos ------------------------------------------ */

static void kid_remove(struct node *dir, int i)
{
	dir->games -= dir->kids[i]->games;
	library_free(dir->kids[i]);
	memmove(&dir->kids[i], &dir->kids[i + 1], sizeof(*dir->kids) * (dir->nkids - i - 1));
	dir->nkids--;
}

static bool is_ext(const char *name, const char *ext)
{
	const char *dot = strrchr(name, '.');
	return dot && !strcasecmp(dot + 1, ext);
}

/* "(Disc 2)", "(Disco 2)", "(CD2)": número do disco e onde a tag começa/termina */
static int disc_tag(const char *name, const char **start, const char **end)
{
	for (const char *p = strchr(name, '('); p; p = strchr(p + 1, '(')) {
		const char *q = p + 1;
		if (!strncasecmp(q, "disc ", 5))
			q += 5;
		else if (!strncasecmp(q, "disco ", 6))
			q += 6;
		else if (!strncasecmp(q, "cd", 2))
			q += 2;
		else
			continue;
		if (!isdigit((unsigned char)*q))
			continue;
		int n = atoi(q);
		while (isdigit((unsigned char)*q))
			q++;
		if (*q == ' ' && !strncasecmp(q, " of ", 4)) { /* "(Disc 1 of 2)" */
			q += 4;
			while (isdigit((unsigned char)*q))
				q++;
		}
		if (*q != ')')
			continue;
		*start = p;
		*end = q + 1;
		return n;
	}
	return 0;
}

/* Base do jogo sem a tag do disco e sem a extensão */
static void disc_key(const char *name, char *out, size_t n)
{
	const char *a, *b;
	char tmp[1024];
	if (disc_tag(name, &a, &b))
		snprintf(tmp, sizeof(tmp), "%.*s%s", (int)(a - name), name, b);
	else
		snprintf(tmp, sizeof(tmp), "%s", name);
	char *dot = strrchr(tmp, '.');
	if (dot)
		*dot = 0;
	/* espaços que sobraram onde estava a tag */
	size_t w = 0;
	for (size_t r = 0; tmp[r]; r++) {
		if (tmp[r] == ' ' && (w == 0 || tmp[w - 1] == ' '))
			continue;
		tmp[w++] = tmp[r];
	}
	while (w > 0 && tmp[w - 1] == ' ')
		w--;
	tmp[w] = 0;
	snprintf(out, n, "%s", tmp);
}

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

struct disc { int n; struct node *node; };

static int disc_cmp(const void *a, const void *b)
{
	return ((const struct disc *)a)->n - ((const struct disc *)b)->n;
}

/* Um item por jogo: .m3u existente esconde os discos que cita; sem .m3u,
 * arquivos "(Disc N)" com a mesma base viram um .m3u gerado no cache. */
static void group_discs(struct node *dir, int console)
{
	/* 1) .m3u do usuário */
	for (int i = 0; i < dir->nkids; i++) {
		struct node *m = dir->kids[i];
		if (m->dir || !is_ext(m->fname, "m3u"))
			continue;
		FILE *f = fopen(m->path, "r");
		char line[4096];
		if (!f)
			continue;
		while (fgets(line, sizeof(line), f)) {
			line[strcspn(line, "\r\n")] = 0;
			if (!line[0] || line[0] == '#')
				continue;
			const char *b = strrchr(line, '/');
			b = b ? b + 1 : line;
			m->discs++;
			for (int k = 0; k < dir->nkids; k++)
				if (k != i && !dir->kids[k]->dir && !strcmp(dir->kids[k]->fname, b)) {
					kid_remove(dir, k);
					if (k < i)
						i--;
					break;
				}
		}
		fclose(f);
	}

	/* 2) grupos "(Disc N)" sem .m3u */
	for (int i = 0; i < dir->nkids; i++) {
		struct node *a = dir->kids[i];
		const char *s, *e;
		if (a->dir || a->discs || is_ext(a->fname, "m3u") || !disc_tag(a->fname, &s, &e))
			continue;
		char key[1024];
		disc_key(a->fname, key, sizeof(key));
		struct disc group[16];
		int ng = 0;
		for (int k = i; k < dir->nkids && ng < 16; k++) {
			struct node *b = dir->kids[k];
			char kb[1024];
			if (b->dir || is_ext(b->fname, "m3u"))
				continue;
			disc_key(b->fname, kb, sizeof(kb));
			int n = disc_tag(b->fname, &s, &e);
			if (n && !strcmp(kb, key))
				group[ng++] = (struct disc){ n, b };
		}
		if (ng < 2)
			continue;
		qsort(group, ng, sizeof(group[0]), disc_cmp);

		char mdir[4096], mpath[4600];
		snprintf(mdir, sizeof(mdir), "%s/%s", M3U_DIR, consoles[console].code);
		mkdir_p(mdir);
		snprintf(mpath, sizeof(mpath), "%s/%s.m3u", mdir, key);
		FILE *f = fopen(mpath, "w");
		if (!f)
			continue;
		for (int g = 0; g < ng; g++)
			fprintf(f, "%s\n", group[g].node->path);
		fclose(f);

		char fname[1100];
		snprintf(fname, sizeof(fname), "%s.m3u", key);
		struct node *m = node_new(fname, mpath, console, false);
		m->discs = ng;
		/* tira os discos e põe o item do jogo */
		for (int g = 0; g < ng; g++)
			for (int k = 0; k < dir->nkids; k++)
				if (dir->kids[k] == group[g].node) {
					kid_remove(dir, k);
					break;
				}
		node_add(dir, m);
		i = -1; /* recomeça: a lista mudou */
	}
}

/* Pasta de console: o código ("NES") com um número opcional na frente para
 * escolher a ordem, como no MinUI: "01 - NES", "01 NES", "01) NES", "01.NES".
 * Devolve o console (ou -1) e o número (-1 sem número). */
int library_console_folder(const char *name, int *order)
{
	const char *p = name;
	*order = -1;
	if (isdigit((unsigned char)*p)) {
		const char *q = p;
		while (isdigit((unsigned char)*q))
			q++;
		const char *r = q;
		while (*r == ' ' || *r == '-' || *r == '.' || *r == ')' || *r == '_')
			r++;
		if (r > q && *r) { /* precisa de separador: "32X" não é "3" + "2X" */
			*order = atoi(p);
			p = r;
		}
	}
	for (int c = 0; c < n_consoles; c++)
		if (!strcasecmp(p, consoles[c].code))
			return c;
	*order = -1;
	return -1;
}

/* Ordem do Início: primeiro as pastas numeradas (pelo número), depois as
 * outras pelo nome mostrado */
static int console_cmp(const void *a, const void *b)
{
	const struct node *x = *(const struct node *const *)a, *y = *(const struct node *const *)b;
	int ox, oy;
	library_console_folder(x->fname, &ox);
	library_console_folder(y->fname, &oy);
	if ((ox >= 0) != (oy >= 0))
		return ox >= 0 ? -1 : 1;
	if (ox >= 0 && ox != oy)
		return ox - oy;
	return natural_cmp(consoles[x->console].name, consoles[y->console].name);
}

static struct node *root_new(void)
{
	return node_new("", ROMS_DIR, -1, true);
}

/* folder: o nome da pasta em <roms>/ (com ou sem número na frente) */
static struct node *console_node(int c, const char *roms, const char *folder, int source)
{
	char path[1024];
	snprintf(path, sizeof(path), "%s/%s", roms, folder);
	struct node *n = node_new(folder, path, c, true);
	free(n->label);
	n->label = strdup(consoles[c].name);
	n->source = source;
	return n;
}

/* Uma pasta por console em <roms> (duas do mesmo console, ex. "NES" e
 * "01 - NES": fica a numerada, ou a que tem jogos) */
static void scan_root(const char *roms, int source, struct node *seen[64])
{
	DIR *d = opendir(roms);
	struct dirent *e;

	while (d && (e = readdir(d))) {
		int order, c = library_console_folder(e->d_name, &order);
		if (c < 0 || c >= 64)
			continue;
		struct node *cn = console_node(c, roms, e->d_name, source);
		scan_dir(cn, c);
		if (seen[c]) {
			int o_old;
			library_console_folder(seen[c]->fname, &o_old);
			bool better = (order >= 0 && o_old < 0) || (seen[c]->games == 0 && cn->games > 0);
			if (!better) {
				library_free(cn);
				continue;
			}
			library_free(seen[c]);
		}
		seen[c] = cn;
	}
	if (d)
		closedir(d);
}

struct node *library_scan(const int *origin, bool sd2)
{
	struct node *root = root_new();
	struct node *s1[64] = { 0 }, *s2[64] = { 0 };
	bool want2 = false;

	/* origem 0 (não escolhida): SD1 se lá tiver jogos do console, senão SD2 */
	for (int c = 0; c < n_consoles; c++)
		want2 |= !origin || origin[c] != 1;
	scan_root(ROMS_DIR, 1, s1);
	if (want2 && sd2)
		scan_root(ROMS_DIR_SD2, 2, s2);

	for (int c = 0; c < n_consoles && c < 64; c++) {
		struct node *pick = NULL;
		int o = origin ? origin[c] : 0;
		if (o == 0)
			o = (s1[c] && s1[c]->games > 0) || !sd2 ? 1 : 2;
		if (o == 2 && origin && origin[c] == 0 && !sd2)
			o = 1;
		if (o == 2) {
			if (sd2) {
				pick = s2[c];
				s2[c] = NULL;
			} else { /* SD2 fora: o console continua no Início, avisando */
				pick = console_node(c, ROMS_DIR_SD2, consoles[c].code, 2);
				pick->missing = true;
			}
		} else {
			pick = s1[c];
			s1[c] = NULL;
		}
		if (pick && (pick->games > 0 || pick->missing))
			node_add(root, pick);
		else
			library_free(pick);
		library_free(s1[c]);
		library_free(s2[c]);
	}
	qsort(root->kids, root->nkids, sizeof(*root->kids), console_cmp);
	return root;
}

const char *library_console_root(const struct node *root, int console)
{
	for (int i = 0; root && i < root->nkids; i++)
		if (root->kids[i]->console == console)
			return root->kids[i]->path;
	return NULL;
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
	    a->source != b->source || a->missing != b->missing || strcmp(a->fname, b->fname) ||
	    strcmp(a->path, b->path))
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
	/* .m3u gerado: o caminho fica no cache (não está na pasta do jogo) */
	bool virt = !n->dir && !strncmp(n->path, M3U_DIR, strlen(M3U_DIR));
	char type = n->dir ? 'D' : (virt ? 'M' : 'F');
	if (depth == 1) /* pasta de console: "C" + origem (ou "X": SD2 ausente) + caminho */
		type = n->missing ? 'X' : 'C';
	fprintf(f, "%d\t%c\t%d\t%d\t%s", depth, type, n->console,
		depth == 1 ? n->source : n->discs, n->fname);
	if (virt || depth == 1)
		fprintf(f, "\t%s", n->path);
	fputc('\n', f);
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
	fprintf(f, "pingo-library 5\n");
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
	if (!fgets(line, sizeof(line), f) || strcmp(line, "pingo-library 5\n")) {
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
		int depth, console, discs;
		char type;
		int off = 0;
		if (sscanf(line, "%d\t%c\t%d\t%d\t%n", &depth, &type, &console, &discs, &off) != 4 || !off ||
		    depth < 1 || depth > top + 1 || depth >= 64 || console < 0 || console >= n_consoles) {
			ok = false;
			break;
		}
		char *name = line + off;
		struct node *parent = stack[depth - 1];
		struct node *n;

		if (depth == 1) { /* "pasta\tcaminho"; discs = origem */
			char *tab = strchr(name, '\t');
			if (!tab) {
				ok = false;
				break;
			}
			*tab = 0;
			n = node_new(name, tab + 1, console, true);
			free(n->label);
			n->label = strdup(consoles[console].name);
			n->source = discs;
			n->missing = type == 'X';
			discs = 0;
		} else if (type == 'M') { /* .m3u gerado: "nome\tcaminho" */
			char *tab = strchr(name, '\t');
			if (!tab) {
				ok = false;
				break;
			}
			*tab = 0;
			n = node_new(name, tab + 1, console, false);
		} else {
			char p[4096];
			snprintf(p, sizeof(p), "%s/%s", parent->path, name);
			n = node_new(name, p, console, type == 'D');
			if (!n->dir && !strcmp(consoles[console].core, "fbneo")) {
				const struct arcade_name *a = arcade_find(name);
				if (a) {
					free(n->label);
					n->label = library_clean_name(a->desc, true);
				}
			}
		}
		n->discs = discs;
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
