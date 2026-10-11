// SPDX-License-Identifier: Unlicense
#include "sgc-log.h"

#include <stdio.h>

static sgc_log_handler_t log_handler;

void sgc_set_log_handler(sgc_log_handler_t handler)
{
	log_handler = handler;
}

void sgc_log(int level, const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	if (log_handler) {
		log_handler(level, fmt, args);
	} else {
		fputs("[sgc] ", stderr);
		vfprintf(stderr, fmt, args);
		fputc('\n', stderr);
	}
	va_end(args);
}
