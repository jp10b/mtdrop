/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Force-included into every OWL source on Windows: the libc gaps that are not
 * tied to a single header.
 *
 * Origin: raph559/winawdl awdl/compat/owl_win.h @ 0db7031 (GPL-3.0-or-later). Changed for mtdrop: <string.h>,
 * which jedbillyb/owl's daemon/core.c uses without including (glibc brings it in through other headers). */
#pragma once
#include <stdint.h>
#include <string.h>
#include <time.h>

/* core.c registers a statistics dump on SIGUSR1. Windows has no such signal
 * and the ev shim never raises it. */
#ifndef SIGUSR1
#define SIGUSR1 30
#endif

#ifndef localtime_r
static inline struct tm *owl_localtime_r(const time_t *time, struct tm *out) {
	return localtime_s(out, time) == 0 ? out : NULL;
}
#define localtime_r owl_localtime_r
#endif
