/* SPDX-License-Identifier: GPL-2.0-only */
/* RetroAchievements no pingo-retro.
 *
 * O rcheevos (rc_client) faz o trabalho: entra na conta, identifica o jogo
 * pelo hash, lê a memória do core a cada quadro e avisa quando uma
 * conquista é desbloqueada. Aqui ficam só a conexão (libcurl numa thread,
 * para o jogo nunca esperar a internet), a leitura da memória e os avisos.
 * As respostas da internet voltam para o rc_client no laço principal
 * (ra_pump), nunca na thread.
 */
#ifdef PINGO_RA

#include "ra.h"

#include <curl/curl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "rc_client.h"
#include "rc_consoles.h"
#include "rc_libretro.h"

#include "textos.h"

#define CA_FILE "/etc/ssl/certs/ca-certificates.crt"

static rc_client_t *client;
static ra_notify_fn notify;
static uint32_t console_id;
static bool hardcore, game_ok;
static char user[64], token[128];

static struct retro_memory_descriptor *mmap_desc;
static struct retro_memory_map mmap_copy;
static bool have_mmap;
static rc_libretro_memory_regions_t regions;
static void *(*core_mem)(unsigned);
static size_t (*core_size)(unsigned);

/* ---- conexão --------------------------------------------------------- */

struct job {
	char *url, *post, *ctype;
	rc_client_server_callback_t cb;
	void *cb_data;
	/* resposta */
	char *body;
	size_t len;
	long status;
	struct job *next;
};

static pthread_mutex_t q_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t q_cond = PTHREAD_COND_INITIALIZER;
static struct job *pending, *done;
static pthread_t worker;
static bool worker_on, sync_mode;
static char user_agent[128];

static size_t on_data(char *p, size_t sz, size_t n, void *ud)
{
	struct job *j = ud;
	size_t add = sz * n;
	char *b = realloc(j->body, j->len + add + 1);
	if (!b)
		return 0;
	memcpy(b + j->len, p, add);
	j->len += add;
	b[j->len] = 0;
	j->body = b;
	return add;
}

static void http(struct job *j)
{
	CURL *c = curl_easy_init();
	j->status = RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR;
	if (!c)
		return;
	struct curl_slist *h = NULL;
	curl_easy_setopt(c, CURLOPT_URL, j->url);
	curl_easy_setopt(c, CURLOPT_USERAGENT, user_agent);
	const char *ca = getenv("PINGO_CA_FILE"); /* para testar sem regravar o sistema */
	curl_easy_setopt(c, CURLOPT_CAINFO, ca && *ca ? ca : CA_FILE);
	curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
	curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
	curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_data);
	curl_easy_setopt(c, CURLOPT_WRITEDATA, j);
	if (j->post) {
		curl_easy_setopt(c, CURLOPT_POSTFIELDS, j->post);
		if (j->ctype) {
			char hdr[160];
			snprintf(hdr, sizeof(hdr), "Content-Type: %s", j->ctype);
			h = curl_slist_append(h, hdr);
			curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
		}
	}
	CURLcode rc = curl_easy_perform(c);
	if (rc == CURLE_OK)
		curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &j->status);
	else
		fprintf(stderr, "ra: %s\n", curl_easy_strerror(rc));
	curl_slist_free_all(h);
	curl_easy_cleanup(c);
}

static void *worker_main(void *arg)
{
	(void)arg;
	for (;;) {
		pthread_mutex_lock(&q_lock);
		while (!pending && worker_on)
			pthread_cond_wait(&q_cond, &q_lock);
		if (!worker_on && !pending) {
			pthread_mutex_unlock(&q_lock);
			return NULL;
		}
		struct job *j = pending;
		pending = j->next;
		pthread_mutex_unlock(&q_lock);

		http(j);

		pthread_mutex_lock(&q_lock);
		j->next = NULL;
		struct job **t = &done; /* mantém a ordem */
		while (*t)
			t = &(*t)->next;
		*t = j;
		pthread_mutex_unlock(&q_lock);
	}
}

static void job_finish(struct job *j)
{
	rc_api_server_response_t r = { j->body ? j->body : "", j->len, (int)j->status };
	j->cb(&r, j->cb_data);
	free(j->url);
	free(j->post);
	free(j->ctype);
	free(j->body);
	free(j);
}

static void server_call(const rc_api_request_t *req, rc_client_server_callback_t cb, void *cb_data,
			rc_client_t *cl)
{
	(void)cl;
	struct job *j = calloc(1, sizeof(*j));
	if (!j)
		return;
	j->url = strdup(req->url);
	j->post = req->post_data ? strdup(req->post_data) : NULL;
	j->ctype = req->content_type ? strdup(req->content_type) : NULL;
	j->cb = cb;
	j->cb_data = cb_data;
	if (sync_mode) { /* --ra-login: na hora */
		http(j);
		job_finish(j);
		return;
	}
	pthread_mutex_lock(&q_lock);
	struct job **t = &pending;
	while (*t)
		t = &(*t)->next;
	*t = j;
	pthread_cond_signal(&q_cond);
	pthread_mutex_unlock(&q_lock);
}

void ra_pump(void)
{
	if (!client)
		return;
	pthread_mutex_lock(&q_lock);
	struct job *list = done;
	done = NULL;
	pthread_mutex_unlock(&q_lock);
	while (list) {
		struct job *n = list->next;
		job_finish(list);
		list = n;
	}
}

/* ---- memória do core --------------------------------------------------- */

static void core_info(uint32_t id, rc_libretro_core_memory_info_t *info)
{
	info->data = core_mem ? core_mem(id) : NULL;
	info->size = core_size ? core_size(id) : 0;
}

static uint32_t read_memory(uint32_t address, uint8_t *buf, uint32_t n, rc_client_t *cl)
{
	(void)cl;
	return rc_libretro_memory_read(&regions, address, buf, n);
}

void ra_set_memory_map(const struct retro_memory_map *m)
{
	free(mmap_desc);
	mmap_desc = NULL;
	have_mmap = false;
	if (!m || !m->num_descriptors)
		return;
	mmap_desc = calloc(m->num_descriptors, sizeof(*mmap_desc));
	if (!mmap_desc)
		return;
	memcpy(mmap_desc, m->descriptors, m->num_descriptors * sizeof(*mmap_desc));
	mmap_copy.descriptors = mmap_desc;
	mmap_copy.num_descriptors = m->num_descriptors;
	have_mmap = true;
}

/* ---- conta e eventos ----------------------------------------------------- */

static void say(const char *title, const char *text)
{
	fprintf(stderr, "ra: %s%s%s\n", title, text ? ": " : "", text ? text : "");
	if (notify)
		notify(title, text);
}

static void on_event(const rc_client_event_t *e, rc_client_t *cl)
{
	(void)cl;
	char t[192];
	switch (e->type) {
	case RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED:
		snprintf(t, sizeof(t), S_RA_UNLOCKED, e->achievement->points);
		say(t, e->achievement->title);
		break;
	case RC_CLIENT_EVENT_GAME_COMPLETED:
		say(S_RA_COMPLETED, rc_client_get_game_info(client)->title);
		break;
	case RC_CLIENT_EVENT_LEADERBOARD_STARTED:
		say(S_RA_LB_STARTED, e->leaderboard->title);
		break;
	case RC_CLIENT_EVENT_LEADERBOARD_FAILED:
		say(S_RA_LB_FAILED, e->leaderboard->title);
		break;
	case RC_CLIENT_EVENT_LEADERBOARD_SUBMITTED:
		snprintf(t, sizeof(t), "%s: %s", e->leaderboard->title, e->leaderboard->tracker_value);
		say(S_RA_LB_SUBMITTED, t);
		break;
	case RC_CLIENT_EVENT_DISCONNECTED:
		say(S_RA_OFFLINE, NULL);
		break;
	case RC_CLIENT_EVENT_RECONNECTED:
		say(S_RA_ONLINE, NULL);
		break;
	case RC_CLIENT_EVENT_SERVER_ERROR:
		fprintf(stderr, "ra: erro do servidor: %s\n", e->server_error->error_message);
		break;
	default:
		break;
	}
}

static void log_msg(const char *m, const rc_client_t *cl)
{
	(void)cl;
	fprintf(stderr, "ra: %s\n", m);
}

static void read_account(void)
{
	FILE *f = fopen(RA_FILE, "r");
	char line[256];
	user[0] = token[0] = 0;
	hardcore = false;
	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = 0;
		if (!strncmp(line, "usuario=", 8))
			snprintf(user, sizeof(user), "%s", line + 8);
		else if (!strncmp(line, "token=", 6))
			snprintf(token, sizeof(token), "%s", line + 6);
		else if (!strncmp(line, "hardcore=", 9))
			hardcore = line[9] == '1';
	}
	fclose(f);
}

static uint32_t console_of(const char *code)
{
	static const struct { const char *code; uint32_t id; } map[] = {
		{ "GB", RC_CONSOLE_GAMEBOY }, { "GBC", RC_CONSOLE_GAMEBOY_COLOR },
		{ "GBA", RC_CONSOLE_GAMEBOY_ADVANCE }, { "NES", RC_CONSOLE_NINTENDO },
		{ "FDS", RC_CONSOLE_FAMICOM_DISK_SYSTEM }, { "SNES", RC_CONSOLE_SUPER_NINTENDO },
		{ "VB", RC_CONSOLE_VIRTUAL_BOY }, { "POKEMINI", RC_CONSOLE_POKEMON_MINI },
		{ "MS", RC_CONSOLE_MASTER_SYSTEM }, { "GG", RC_CONSOLE_GAME_GEAR },
		{ "MD", RC_CONSOLE_MEGA_DRIVE }, { "SEGACD", RC_CONSOLE_SEGA_CD },
		{ "32X", RC_CONSOLE_SEGA_32X }, { "PCE", RC_CONSOLE_PC_ENGINE },
		{ "PCECD", RC_CONSOLE_PC_ENGINE_CD }, { "NGP", RC_CONSOLE_NEOGEO_POCKET },
		{ "WS", RC_CONSOLE_WONDERSWAN }, { "MSX", RC_CONSOLE_MSX },
		{ "A2600", RC_CONSOLE_ATARI_2600 }, { "A7800", RC_CONSOLE_ATARI_7800 },
		{ "LYNX", RC_CONSOLE_ATARI_LYNX }, { "PS", RC_CONSOLE_PLAYSTATION },
		{ "ARCADE", RC_CONSOLE_ARCADE }, { "NEOGEO", RC_CONSOLE_ARCADE },
	};
	for (size_t i = 0; i < sizeof(map) / sizeof(*map); i++)
		if (!strcmp(map[i].code, code))
			return map[i].id;
	return 0;
}

static void make_client(void)
{
	curl_global_init(CURL_GLOBAL_DEFAULT);
	client = rc_client_create(read_memory, server_call);
	rc_client_enable_logging(client, RC_CLIENT_LOG_LEVEL_WARN, log_msg);
	rc_client_set_event_handler(client, on_event);
	char clause[64] = "";
	rc_client_get_user_agent_clause(client, clause, sizeof(clause));
	snprintf(user_agent, sizeof(user_agent), "PingoOS/0.1 (Linux) %s", clause);
}

static void on_login(int result, const char *err, rc_client_t *cl, void *ud)
{
	(void)cl;
	(void)ud;
	if (result != RC_OK) {
		fprintf(stderr, "ra: login falhou: %s\n", err ? err : "?");
		say(S_RA_LOGIN_FAILED, err);
	}
}

bool ra_start(const char *console_code)
{
	read_account();
	console_id = console_of(console_code);
	if (!user[0] || !token[0] || !console_id)
		return false;
	make_client();
	rc_client_set_hardcore_enabled(client, hardcore);
	worker_on = true;
	if (pthread_create(&worker, NULL, worker_main, NULL) != 0) {
		worker_on = false;
		rc_client_destroy(client);
		client = NULL;
		return false;
	}
	rc_client_begin_login_with_token(client, user, token, on_login, NULL);
	return true;
}

void ra_set_notify(ra_notify_fn fn)
{
	notify = fn;
}

/* ---- jogo ------------------------------------------------------------- */

static void on_game(int result, const char *err, rc_client_t *cl, void *ud)
{
	(void)cl;
	(void)ud;
	if (result == RC_NO_GAME_LOADED) {
		fprintf(stderr, "ra: jogo não reconhecido\n");
		say(S_RA_UNKNOWN, NULL);
		return;
	}
	if (result != RC_OK) {
		fprintf(stderr, "ra: não carregou o jogo: %s\n", err ? err : "?");
		return;
	}
	game_ok = true;
	rc_client_user_game_summary_t s;
	rc_client_get_user_game_summary(client, &s);
	char t[96];
	if (s.num_core_achievements) {
		snprintf(t, sizeof(t), S_RA_SUMMARY, s.num_unlocked_achievements, s.num_core_achievements);
		say(rc_client_get_game_info(client)->title, t);
	} else {
		say(rc_client_get_game_info(client)->title, S_RA_NONE);
	}
}

void ra_load_game(const char *path, const void *data, size_t size, void *(*get_mem)(unsigned),
		  size_t (*get_size)(unsigned))
{
	if (!client)
		return;
	core_mem = get_mem;
	core_size = get_size;
	if (!rc_libretro_memory_init(&regions, have_mmap ? &mmap_copy : NULL, core_info, console_id))
		fprintf(stderr, "ra: memória do core não encontrada\n");
	rc_client_begin_identify_and_load_game(client, console_id, path, data, size, on_game, NULL);
}

void ra_frame(void)
{
	if (!client)
		return;
	ra_pump();
	rc_client_do_frame(client);
}

void ra_idle(void)
{
	if (!client)
		return;
	ra_pump();
	rc_client_idle(client);
}

void ra_reset(void)
{
	if (client)
		rc_client_reset(client);
}

bool ra_active(void)
{
	return client && game_ok;
}

bool ra_hardcore(void)
{
	return client && hardcore;
}

int ra_list(struct ra_cheevo *out, int max, unsigned *unlocked, unsigned *total)
{
	*unlocked = *total = 0;
	if (!ra_active())
		return 0;
	rc_client_achievement_list_t *l =
		rc_client_create_achievement_list(client, RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE,
						  RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_LOCK_STATE);
	int n = 0;
	for (uint32_t b = 0; l && b < l->num_buckets; b++)
		for (uint32_t i = 0; i < l->buckets[b].num_achievements; i++) {
			const rc_client_achievement_t *a = l->buckets[b].achievements[i];
			(*total)++;
			if (a->unlocked)
				(*unlocked)++;
			if (n < max) {
				snprintf(out[n].title, sizeof(out[n].title), "%s", a->title);
				snprintf(out[n].description, sizeof(out[n].description), "%s", a->description);
				out[n].points = a->points;
				out[n].unlocked = a->unlocked != 0;
				n++;
			}
		}
	if (l)
		rc_client_destroy_achievement_list(l);
	return n;
}

void ra_stop(void)
{
	if (!client)
		return;
	rc_client_unload_game(client);
	pthread_mutex_lock(&q_lock);
	worker_on = false;
	pthread_cond_signal(&q_cond);
	pthread_mutex_unlock(&q_lock);
	pthread_join(worker, NULL);
	ra_pump(); /* desbloqueios que ainda estavam a caminho */
	rc_client_destroy(client);
	client = NULL;
	rc_libretro_memory_destroy(&regions);
	curl_global_cleanup();
}

/* ---- login pelo launcher: pingo-retro --ra-login <usuário> ------------------ *
 * A senha vem pela entrada padrão; no arquivo fica só o token. Saída: 0 ok,
 * 1 usuário ou senha errados, 2 sem conexão. */
static int login_result = -1;
static char login_err[256];

static void on_cli_login(int result, const char *err, rc_client_t *cl, void *ud)
{
	(void)cl;
	(void)ud;
	login_result = result;
	snprintf(login_err, sizeof(login_err), "%s", err ? err : "");
}

int ra_login_cli(const char *name)
{
	char pass[256] = "";
	if (!fgets(pass, sizeof(pass), stdin))
		pass[0] = 0;
	pass[strcspn(pass, "\r\n")] = 0;
	if (!name || !*name || !pass[0])
		return 1;
	sync_mode = true;
	make_client();
	rc_client_begin_login_with_password(client, name, pass, on_cli_login, NULL);
	memset(pass, 0, sizeof(pass));
	int rc;
	if (login_result == RC_OK) {
		const rc_client_user_t *u = rc_client_get_user_info(client);
		read_account(); /* mantém o modo hardcore escolhido */
		char tmp[] = RA_FILE ".tmp";
		FILE *f = fopen(tmp, "w");
		if (f) {
			fprintf(f, "usuario=%s\ntoken=%s\nhardcore=%d\n", u->username, u->token,
				hardcore ? 1 : 0);
			fflush(f);
			fsync(fileno(f));
			fclose(f);
			rename(tmp, RA_FILE);
		}
		printf("%s\n", u->display_name);
		rc = 0;
	} else {
		printf("%s\n", login_err);
		rc = (login_result == RC_INVALID_CREDENTIALS || login_result == RC_ACCESS_DENIED ||
		      login_result == RC_EXPIRED_TOKEN) ? 1 : 2;
	}
	rc_client_destroy(client);
	client = NULL;
	curl_global_cleanup();
	return rc;
}

#endif
