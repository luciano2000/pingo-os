/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * pingo-leds: luzes RGB dos analógicos (Anbernic RG40XX-H/V).
 *
 * O microcontrolador das luzes fica na /dev/ttyS2 (115200 8N1). Pacote:
 *   modo (1), brilho (0-255), 16 cores RGB, soma de verificação (& 0xFF)
 * LEDs 0-7 = analógico direito, 8-15 = esquerdo (conferido no aparelho), no
 * sentido anti-horário, um a cada 45°: o 0 do direito fica às 3h e o 0 do
 * esquerdo (LED 8) às 12h.
 * A cada pacote aceito ele responde "XXH_002". A luz "rgb:kbd_backlight"
 * liga a energia (e fica sempre ligada: ver power()).
 * Resumo em /run/pingo/leds.state (pacotes, respostas, perdas), a cada 5 s.
 *
 * Configuração: /mnt/sd1/Config/luzes.cfg (escrita pela tela Luzes):
 *   modo=fixo|respirar|arcoiris|bateria|ambilight|desligado
 *   esquerda=RRGGBB  direita=RRGGBB  brilho=0-100
 *
 * Comandos pela FIFO /run/pingo/leds (uma linha cada):
 *   reload                  relê a configuração
 *   off | on                tela apagada/suspensão e de volta
 *   ambi RRGGBB RRGGBB      cores do jogo (esquerda, direita) no Ambilight
 *   ambi-end                o jogo fechou
 *   preview RRGGBB RRGGBB B cores e brilho na tela Luzes, antes de salvar
 */
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define TTY      "/dev/ttyS2"
#define POWER    "/sys/class/leds/rgb:kbd_backlight/brightness"
#define CFG      "/mnt/sd1/Config/luzes.cfg"
#define FIFO     "/run/pingo/leds"
#define BAT      "/sys/class/power_supply/battery/capacity"
#define NLEDS    16
/* Intervalo entre pacotes. Rajadas (um pacote a cada 5 ms, quando cada cor
 * do jogo disparava um envio) travavam o microcontrolador até o aparelho ser
 * desligado. 40 ms = 25 por segundo, com folga. */
#define TICK_MS  40

enum { M_OFF, M_FIXED, M_BREATHE, M_RAINBOW, M_BATTERY, M_AMBI };
static const char *mode_names[] = { "desligado", "fixo", "respirar", "arcoiris", "bateria",
				    "ambilight" };

struct rgb {
	float r, g, b;
};

static int mode = M_FIXED, bright = 60; /* brilho em % */
static struct rgb left = { 1, 0.45f, 0 }, right = { 1, 0.45f, 0 }; /* laranja do Pingo */
static bool screen_off, preview, ambi_active;
static unsigned n_sent, n_rx; /* pacotes enviados, bytes de resposta */
static struct rgb pv_left, pv_right, ambi_l, ambi_r, cur_l, cur_r;
static int pv_bright;
static int tty = -1;
static volatile sig_atomic_t quit;

static struct rgb hex(const char *s)
{
	unsigned v = (unsigned)strtoul(s, NULL, 16);
	return (struct rgb){ ((v >> 16) & 255) / 255.0f, ((v >> 8) & 255) / 255.0f, (v & 255) / 255.0f };
}

static void load_cfg(void)
{
	FILE *f = fopen(CFG, "r");
	char line[128];
	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = 0;
		char *eq = strchr(line, '=');
		if (!eq)
			continue;
		*eq++ = 0;
		if (!strcmp(line, "modo")) {
			for (int i = 0; i < (int)(sizeof(mode_names) / sizeof(*mode_names)); i++)
				if (!strcmp(eq, mode_names[i]))
					mode = i;
		} else if (!strcmp(line, "esquerda")) {
			left = hex(eq);
		} else if (!strcmp(line, "direita")) {
			right = hex(eq);
		} else if (!strcmp(line, "brilho")) {
			bright = atoi(eq);
			if (bright < 0)
				bright = 0;
			if (bright > 100)
				bright = 100;
		}
	}
	fclose(f);
}

static int force_frames; /* depois de religar a energia: manda sempre */

/* Energia das luzes: só liga, nunca desliga. O pino (rgb:kbd_backlight) não
 * corta a energia do microcontrolador por inteiro, e a serial continua
 * ligada nele: desligado assim, ele ficava meio alimentado e travava até o
 * aparelho ser desligado de verdade. Para apagar, vai a cor preta. */
static void power(bool on)
{
	force_frames = 30; /* ~1 s mandando sempre */
	int fd = open(POWER, O_WRONLY);
	if (fd >= 0) {
		if (write(fd, on ? "1" : "0", 1) < 0)
			perror("pingo-leds: energia");
		close(fd);
	}
}

static bool tty_open(void)
{
	tty = open(TTY, O_RDWR | O_NOCTTY);
	if (tty < 0)
		return false;
	struct termios t = { 0 };
	t.c_cflag = CS8 | CREAD | CLOCAL;
	cfsetispeed(&t, B115200);
	cfsetospeed(&t, B115200);
	tcsetattr(tty, TCSANOW, &t);
	return true;
}

static unsigned char to8(float v)
{
	if (v < 0)
		v = 0;
	if (v > 1)
		v = 1;
	return (unsigned char)(v * 255 + 0.5f);
}

/* Ângulo de cada LED na volta do analógico, em voltas (0 = 12h, sentido
 * horário): direito começa às 3h, esquerdo às 12h; os dois vão no sentido
 * anti-horário, um a cada 45° */
static float led_angle(int i)
{
	float start = i < 8 ? 0.25f : 0.0f;
	float a = start - (i % 8) / 8.0f;
	return a - floorf(a);
}

/* Reenvio: um pacote perdido (o microcontrolador ainda ligando depois da
 * suspensão, por exemplo) deixaria as luzes paradas até a cor mudar. Então o
 * estado vai de novo a cada meio segundo, mesmo igual. */
static struct timespec last_sent;

static long ms_since(const struct timespec *a)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (now.tv_sec - a->tv_sec) * 1000 + (now.tv_nsec - a->tv_nsec) / 1000000;
}

/* Manda uma cor por LED com o brilho dado (0-100) */
/* O microcontrolador responde "XXH_002" a cada pacote aceito. Quando ele
 * reinicia (queda de energia no jogo, com a bateria fraca), as respostas
 * param, e uma enxurrada de pacotes nessa hora o deixava preso
 * (provavelmente no modo de gravação do próprio chip) até o aparelho ser
 * desligado. Então: 4 pacotes sem resposta, e só vai um pacote de teste a
 * cada meio segundo (o ritmo com que ele sempre se recuperou), até ele
 * responder de novo. Um chip que nunca respondeu (outro firmware) recebe
 * normalmente. */
static int pending; /* pacotes sem resposta */
static unsigned n_silences;

static bool chip_lost(void)
{
	return n_rx && pending >= 4;
}

static struct timespec t_start;

static void chip_answer(int lines)
{
	if (chip_lost() && lines)
		force_frames = 10; /* voltou: manda já, sem esperar mudança */
	pending = 0;
}

static void send_leds(const struct rgb *c, int pct)
{
	static unsigned char last[2 + NLEDS * 3];
	static bool sent;
	if (sent && ms_since(&last_sent) < TICK_MS - 5)
		return; /* nunca em rajada */
	bool lost = chip_lost();
	if (lost && ms_since(&last_sent) < 500)
		return; /* chip sem responder: só um teste a cada meio segundo */
	unsigned char pkt[2 + NLEDS * 3 + 1];
	pkt[0] = 1;
	pkt[1] = (unsigned char)(pct * 255 / 100);
	for (int i = 0; i < NLEDS; i++) {
		pkt[2 + i * 3] = to8(c[i].r);
		pkt[3 + i * 3] = to8(c[i].g);
		pkt[4 + i * 3] = to8(c[i].b);
	}
	if (lost)
		; /* teste: vai mesmo sem mudança */
	else if (force_frames > 0)
		force_frames--;
	else if (sent && !memcmp(last, pkt, sizeof(last)) && ms_since(&last_sent) < 500)
		return; /* nada mudou há pouco: a serial fica quieta */
	clock_gettime(CLOCK_MONOTONIC, &last_sent);
	memcpy(last, pkt, sizeof(last));
	sent = true;
	unsigned sum = 0;
	for (size_t i = 0; i < sizeof(pkt) - 1; i++)
		sum += pkt[i];
	pkt[sizeof(pkt) - 1] = (unsigned char)(sum & 0xFF);
	if (write(tty, pkt, sizeof(pkt)) < 0)
		perror("pingo-leds: serial");
	n_sent++;
	if (pending == 3)
		n_silences++; /* este é o quarto sem resposta */
	if (pending < 1000)
		pending++;
}

/* As duas cores: 0-7 direita, 8-15 esquerda */
static void send(struct rgb l, struct rgb r, int pct)
{
	struct rgb c[NLEDS];
	for (int i = 0; i < NLEDS; i++)
		c[i] = i < 8 ? r : l;
	send_leds(c, pct);
}

static struct rgb hsv(float h, float s, float v)
{
	h = fmodf(h, 1.0f) * 6;
	int i = (int)h;
	float f = h - i, p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
	switch (i % 6) {
	case 0: return (struct rgb){ v, t, p };
	case 1: return (struct rgb){ q, v, p };
	case 2: return (struct rgb){ p, v, t };
	case 3: return (struct rgb){ p, q, v };
	case 4: return (struct rgb){ t, p, v };
	default: return (struct rgb){ v, p, q };
	}
}

static struct rgb mix(struct rgb a, struct rgb b, float k)
{
	return (struct rgb){ a.r + (b.r - a.r) * k, a.g + (b.g - a.g) * k, a.b + (b.b - a.b) * k };
}

static struct rgb scale(struct rgb a, float k)
{
	return (struct rgb){ a.r * k, a.g * k, a.b * k };
}

static int battery(void)
{
	FILE *f = fopen(BAT, "r");
	int v = -1;
	if (f) {
		if (fscanf(f, "%d", &v) != 1)
			v = -1;
		fclose(f);
	}
	return v;
}

static void command(char *line)
{
	char a[16], b[16];
	int br;
	if (!strcmp(line, "reload")) {
		load_cfg();
		preview = false;
	} else if (!strcmp(line, "off")) {
		/* na hora, sem a transição: a suspensão vem logo em seguida */
		screen_off = true;
		cur_l = cur_r = (struct rgb){ 0, 0, 0 }; /* o próximo pacote (até 40 ms) já vai preto */
	} else if (!strcmp(line, "on")) {
		screen_off = false;
		force_frames = 30;
	} else if (sscanf(line, "ambi %15s %15s", a, b) == 2) {
		ambi_l = hex(a);
		ambi_r = hex(b);
		ambi_active = true;
	} else if (!strcmp(line, "ambi-end")) {
		ambi_active = false;
	} else if (sscanf(line, "preview %15s %15s %d", a, b, &br) == 3) {
		pv_left = hex(a);
		pv_right = hex(b);
		pv_bright = br;
		preview = true;
	}
}

/* Resumo para diagnóstico (/run/pingo/leds.state, a cada 5 s, na memória) */
static void dump_state(void)
{
	FILE *f = fopen("/run/pingo/leds.state.tmp", "w");
	if (!f)
		return;
	fprintf(f, "modo=%s\npacotes=%u\nrespostas=%u\nchip_sem_resposta=%d\nchip_perdas=%u\n",
		mode_names[mode], n_sent, n_rx / 9, pending, n_silences);
	fclose(f);
	rename("/run/pingo/leds.state.tmp", "/run/pingo/leds.state");
}

static void on_term(int s)
{
	(void)s;
	quit = 1;
}

int main(void)
{
	signal(SIGTERM, on_term);
	signal(SIGINT, on_term);
	if (!tty_open()) {
		fprintf(stderr, "pingo-leds: sem %s\n", TTY);
		return 1;
	}
	mkdir("/run/pingo", 0755);
	unlink(FIFO);
	if (mkfifo(FIFO, 0666) < 0) {
		perror("pingo-leds: fifo");
		return 1;
	}
	/* O_RDWR: a FIFO nunca "fecha" quando quem escreve sai */
	int fifo = open(FIFO, O_RDWR | O_NONBLOCK);
	load_cfg();
	power(true);

	char buf[512];
	size_t len = 0;
	struct timespec t0;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	t_start = t0;
	int last_bat = -1, bat_tick = 0;

	long next_tick = 0;
	while (!quit) {
		struct pollfd pp[2] = { { fifo, POLLIN, 0 }, { tty, POLLIN, 0 } };
		/* Relógio fixo: um pacote a cada TICK_MS, no máximo. Comandos e
		 * respostas do chip só acordam o laço para serem lidos. */
		long now_ms = ms_since(&t_start);
		long wait = next_tick - now_ms;
		int pr = poll(pp, 2, wait < 0 ? 0 : wait > TICK_MS ? TICK_MS : (int)wait);
		if (pr > 0 && (pp[1].revents & POLLIN)) {
			/* "XXH_002" do microcontrolador: a resposta ao último pacote */
			char rb[256];
			ssize_t rn = read(tty, rb, sizeof(rb));
			if (rn > 0) {
				int lines = 0;
				for (ssize_t i = 0; i < rn; i++)
					lines += rb[i] == '\n';
				if (lines)
					chip_answer(lines);
				n_rx += (unsigned)rn;
			}
		}
		if (pr > 0 && (pp[0].revents & POLLIN)) {
			ssize_t n = read(fifo, buf + len, sizeof(buf) - 1 - len);
			if (n > 0) {
				len += (size_t)n;
				buf[len] = 0;
				char *nl;
				while ((nl = strchr(buf, '\n'))) {
					*nl = 0;
					command(buf);
					memmove(buf, nl + 1, len - (size_t)(nl + 1 - buf) + 1);
					len -= (size_t)(nl + 1 - buf);
				}
				if (len >= sizeof(buf) - 1)
					len = 0; /* linha comprida demais: descarta */
			}
		}
		now_ms = ms_since(&t_start);
		if (now_ms < next_tick)
			continue;
		next_tick = now_ms + TICK_MS;
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		static time_t last_dump;
		if (now.tv_sec - last_dump >= 5) {
			last_dump = now.tv_sec;
			dump_state();
		}
		float t = (now.tv_sec - t0.tv_sec) + (now.tv_nsec - t0.tv_nsec) / 1e9f;

		struct rgb want_l = left, want_r = right;
		int pct = bright;
		if (screen_off || mode == M_OFF) {
			want_l = want_r = (struct rgb){ 0, 0, 0 };
		} else if (preview) {
			want_l = pv_left;
			want_r = pv_right;
			pct = pv_bright;
		} else if (mode == M_BREATHE) {
			float k = 0.15f + 0.85f * (0.5f - 0.5f * cosf(t * 2 * (float)M_PI / 4.0f));
			want_l = scale(left, k);
			want_r = scale(right, k);
		} else if (mode == M_RAINBOW && !screen_off && !preview) {
			/* cada LED com o matiz da sua posição, girando uma volta a cada 6 s */
			struct rgb c[NLEDS];
			for (int i = 0; i < NLEDS; i++)
				c[i] = hsv(led_angle(i) + t / 6.0f, 1, 1);
			send_leds(c, pct);
			continue;
		} else if (mode == M_BATTERY) {
			if (bat_tick-- <= 0) {
				last_bat = battery();
				bat_tick = 30 * 10;
			}
			struct rgb c = last_bat >= 60 ? (struct rgb){ 0.1f, 1, 0.2f }
				     : last_bat >= 40 ? (struct rgb){ 1, 0.8f, 0 }
				     : last_bat >= 20 ? (struct rgb){ 1, 0.35f, 0 }
							: (struct rgb){ 1, 0, 0 };
			want_l = want_r = c;
		} else if (mode == M_AMBI && ambi_active) {
			want_l = ambi_l;
			want_r = ambi_r;
		}
		/* Transição suave (sem piscar a cada quadro no Ambilight) */
		float k = (mode == M_AMBI && !preview) ? 0.25f : 0.5f;
		cur_l = mix(cur_l, want_l, k);
		cur_r = mix(cur_r, want_r, k);
		send(cur_l, cur_r, pct);
	}
	usleep(TICK_MS * 1000); /* o intervalo vale também para o último pacote */
	send((struct rgb){ 0, 0, 0 }, (struct rgb){ 0, 0, 0 }, 0);
	unlink(FIFO);
	return 0;
}
