/* SPDX-License-Identifier: GPL-2.0-only */
#include "luzes.h"

#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

bool leds_available(void)
{
	struct stat st;
	return stat(LEDS_FIFO, &st) == 0 && S_ISFIFO(st.st_mode);
}

/* Uma linha para o serviço. Sem bloquear: se ninguém lê, não espera. */
void leds_cmd(const char *fmt, ...)
{
	char line[128];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(line, sizeof(line) - 1, fmt, ap);
	va_end(ap);
	if (n <= 0 || n >= (int)sizeof(line) - 1)
		return;
	line[n++] = '\n';
	int fd = open(LEDS_FIFO, O_WRONLY | O_NONBLOCK);
	if (fd < 0)
		return;
	if (write(fd, line, (size_t)n) < 0) {
		/* fila cheia: a próxima atualização resolve */
	}
	close(fd);
}
