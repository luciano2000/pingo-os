/* SPDX-License-Identifier: GPL-2.0-only */
/* Conversa com o pingo-leds (luzes RGB dos analógicos). Sem o serviço (um
 * aparelho sem as luzes), tudo vira nada. */
#ifndef PINGO_LUZES_H
#define PINGO_LUZES_H

#include <stdbool.h>

#define LEDS_FIFO "/run/pingo/leds"
#define LEDS_CFG  "/mnt/sd1/Config/luzes.cfg"

bool leds_available(void);
void leds_cmd(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif
