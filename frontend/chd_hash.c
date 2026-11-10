/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Leitor de CHD para o hash do RetroAchievements.
 *
 * O rcheevos identifica jogos de CD lendo setores pelo "cdreader"; o padrão
 * dele só entende .cue/.bin/.iso. Aqui um cdreader que abre .chd com o
 * libchdr e repassa o resto ao padrão.
 *
 * Setores: o rcheevos usa o LBA do disco (a faixa 1 começa no 0, e os
 * endereços do ISO9660 apontam direto para ele). No CHD, cada faixa ocupa
 * FRAMES quadros de 2448 bytes (2352 de setor + 96 de subcódigo),
 * arredondados para múltiplo de 4; o pregap só está no arquivo quando o
 * PGTYPE começa com 'V'.
 */
#ifdef PINGO_RA
#include "chd_hash.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <libchdr/cdrom.h>
#include <libchdr/chd.h>

struct chd_track {
	chd_file *chd;
	uint8_t *hunk;          /* um hunk decodificado */
	uint32_t hunk_bytes, frames_per_hunk, cur_hunk;
	uint32_t lba;           /* LBA do índice 01 da faixa */
	uint32_t chd_frame;     /* quadro do CHD que corresponde a esse LBA */
	uint32_t frames;        /* quadros de dados (sem o pregap) */
	uint32_t header, data;  /* bytes a pular no setor e bytes úteis */
	int audio;              /* áudio no CHD é big-endian: troca os bytes */
};

/* Handle que o rcheevos recebe: um CHD nosso ou uma faixa do leitor padrão */
struct handle {
	struct chd_track *chd;
	void *other;
};

static struct rc_hash_cdreader def;

static int is_chd(const char *path)
{
	const char *dot = strrchr(path, '.');
	return dot && !strcasecmp(dot, ".chd");
}

/* Tipo da faixa -> onde começam os dados e quantos bytes valem (o mesmo
 * critério do leitor padrão do rcheevos para .bin) */
static int track_layout(const char *type, uint32_t *header, uint32_t *data)
{
	static const struct { const char *type; uint32_t header, data; } t[] = {
		{ "MODE1_RAW", 16, 2048 },    { "MODE1/2352", 16, 2048 },
		{ "MODE1", 0, 2048 },         { "MODE1/2048", 0, 2048 },
		{ "MODE2_RAW", 24, 2048 },    { "MODE2/2352", 24, 2048 },
		{ "MODE2_FORM1", 0, 2048 },   { "MODE2/2048", 0, 2048 },
		{ "MODE2_FORM2", 0, 2324 },   { "MODE2/2324", 0, 2324 },
		{ "MODE2_FORM_MIX", 8, 2048 }, { "MODE2/2336", 8, 2048 },
		{ "MODE2", 8, 2048 },         { "AUDIO", 0, 2352 },
	};
	for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++)
		if (!strcmp(type, t[i].type)) {
			*header = t[i].header;
			*data = t[i].data;
			return 1;
		}
	return 0;
}

static struct chd_track *chd_open_track(const char *path, uint32_t want)
{
	chd_file *chd;
	if (chd_open(path, CHD_OPEN_READ, NULL, &chd) != CHDERR_NONE)
		return NULL;
	const chd_header *h = chd_get_header(chd);
	if (!h || h->hunkbytes < CD_FRAME_SIZE || h->hunkbytes % CD_FRAME_SIZE) {
		chd_close(chd);
		return NULL;
	}

	struct chd_track best = { 0 }, cur;
	int found = 0, ntracks = 0;
	uint32_t lba = 0, chd_frame = 0;
	for (uint32_t i = 0;; i++) {
		char meta[256], type[32], sub[32], pgtype[32] = "", pgsub[32];
		int num = 0, frames = 0, pregap = 0, postgap = 0;
		uint32_t len = 0;
		if (chd_get_metadata(chd, CDROM_TRACK_METADATA2_TAG, i, meta, sizeof(meta) - 1, &len,
				     NULL, NULL) == CHDERR_NONE) {
			meta[len < sizeof(meta) ? len : sizeof(meta) - 1] = 0;
			if (sscanf(meta, CDROM_TRACK_METADATA2_FORMAT, &num, type, sub, &frames, &pregap,
				   pgtype, pgsub, &postgap) != 8)
				break;
		} else if (chd_get_metadata(chd, CDROM_TRACK_METADATA_TAG, i, meta, sizeof(meta) - 1,
					    &len, NULL, NULL) == CHDERR_NONE) {
			meta[len < sizeof(meta) ? len : sizeof(meta) - 1] = 0;
			if (sscanf(meta, CDROM_TRACK_METADATA_FORMAT, &num, type, sub, &frames) != 4)
				break;
		} else {
			break; /* sem mais faixas (ou GD-ROM, que não usamos) */
		}
		if (frames <= 0)
			break;
		ntracks++;
		/* pregap gravado no arquivo: faz parte dos FRAMES */
		uint32_t stored_pregap = pgtype[0] == 'V' ? (uint32_t)pregap : 0;
		if (i > 0)
			lba += pregap; /* a faixa 1 começa no LBA 0 */
		memset(&cur, 0, sizeof(cur));
		cur.lba = lba;
		cur.chd_frame = chd_frame + stored_pregap;
		cur.frames = frames - stored_pregap;
		cur.audio = !strcmp(type, "AUDIO");
		if (!track_layout(type, &cur.header, &cur.data)) {
			fprintf(stderr, "ra: chd: faixa %d de tipo desconhecido (%s)\n", num, type);
			cur.data = 0;
		}

		int pick = 0;
		if (want == RC_HASH_CDTRACK_FIRST_DATA)
			pick = !found && !cur.audio && cur.data;
		else if (want == RC_HASH_CDTRACK_LAST)
			pick = cur.data != 0;
		else if (want == RC_HASH_CDTRACK_LARGEST)
			pick = cur.data && (!found || cur.frames > best.frames);
		else
			pick = (uint32_t)num == want && cur.data;
		if (pick) {
			best = cur;
			found = 1;
		}

		lba += cur.frames + postgap;
		chd_frame += (frames + CD_TRACK_PADDING - 1) / CD_TRACK_PADDING * CD_TRACK_PADDING;
	}
	/* RC_HASH_CDTRACK_FIRST_OF_SECOND_SESSION (e outros): o CHD não guarda
	 * as sessões; nenhum console nosso precisa disso */
	if (!found) {
		if (!ntracks)
			fprintf(stderr, "ra: chd: %s não é um CD\n", path);
		chd_close(chd);
		return NULL;
	}

	struct chd_track *t = malloc(sizeof(*t));
	uint8_t *hunk = malloc(h->hunkbytes);
	if (!t || !hunk) {
		free(t);
		free(hunk);
		chd_close(chd);
		return NULL;
	}
	*t = best;
	t->chd = chd;
	t->hunk = hunk;
	t->hunk_bytes = h->hunkbytes;
	t->frames_per_hunk = h->hunkbytes / CD_FRAME_SIZE;
	t->cur_hunk = (uint32_t)-1;
	return t;
}

static size_t track_read(struct chd_track *t, uint32_t sector, uint8_t *out, size_t want)
{
	size_t total = 0;
	if (sector < t->lba)
		return 0;
	while (want > 0) {
		uint32_t rel = sector - t->lba;
		if (rel >= t->frames)
			break;
		uint32_t frame = t->chd_frame + rel, hunk = frame / t->frames_per_hunk;
		if (hunk != t->cur_hunk) {
			if (chd_read(t->chd, hunk, t->hunk) != CHDERR_NONE) {
				t->cur_hunk = (uint32_t)-1;
				break;
			}
			t->cur_hunk = hunk;
		}
		const uint8_t *s = t->hunk + (frame % t->frames_per_hunk) * CD_FRAME_SIZE + t->header;
		size_t n = want < t->data ? want : t->data;
		if (t->audio) {
			for (size_t i = 0; i + 1 < n; i += 2) {
				out[i] = s[i + 1];
				out[i + 1] = s[i];
			}
		} else {
			memcpy(out, s, n);
		}
		out += n;
		total += n;
		want -= n;
		sector++;
	}
	return total;
}

/* ---- callbacks do rcheevos -------------------------------------------- */

static void *RC_CCONV open_track_iterator(const char *path, uint32_t track,
					  const struct rc_hash_iterator *it)
{
	struct handle *hd = calloc(1, sizeof(*hd));
	if (!hd)
		return NULL;
	if (track == 0) /* compatibilidade: 0 era "a maior" */
		track = RC_HASH_CDTRACK_LARGEST;
	if (is_chd(path))
		hd->chd = chd_open_track(path, track);
	else if (def.open_track_iterator)
		hd->other = def.open_track_iterator(path, track, it);
	if (!hd->chd && !hd->other) {
		free(hd);
		return NULL;
	}
	return hd;
}

static void *RC_CCONV open_track(const char *path, uint32_t track)
{
	return open_track_iterator(path, track, NULL);
}

static size_t RC_CCONV read_sector(void *h, uint32_t sector, void *buffer, size_t bytes)
{
	struct handle *hd = h;
	if (!hd)
		return 0;
	if (hd->chd)
		return track_read(hd->chd, sector, buffer, bytes);
	return def.read_sector(hd->other, sector, buffer, bytes);
}

static void RC_CCONV close_track(void *h)
{
	struct handle *hd = h;
	if (!hd)
		return;
	if (hd->chd) {
		chd_close(hd->chd->chd);
		free(hd->chd->hunk);
		free(hd->chd);
	} else if (hd->other) {
		def.close_track(hd->other);
	}
	free(hd);
}

static uint32_t RC_CCONV first_track_sector(void *h)
{
	struct handle *hd = h;
	if (!hd)
		return 0;
	if (hd->chd)
		return hd->chd->lba;
	return def.first_track_sector(hd->other);
}

void chd_hash_cdreader(struct rc_hash_cdreader *r)
{
	rc_hash_get_default_cdreader(&def);
	r->open_track = open_track;
	r->open_track_iterator = open_track_iterator;
	r->read_sector = read_sector;
	r->close_track = close_track;
	r->first_track_sector = first_track_sector;
}

#endif
