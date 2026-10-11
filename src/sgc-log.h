// SPDX-License-Identifier: Unlicense
#pragma once

#include <stdarg.h>

/* Same numeric values as libobs' LOG_* so the plugin can forward directly. */
enum sgc_log_level {
	SGC_LOG_ERROR = 100,
	SGC_LOG_WARNING = 200,
	SGC_LOG_INFO = 300,
	SGC_LOG_DEBUG = 400,
};

typedef void (*sgc_log_handler_t)(int level, const char *fmt, va_list args);

/* Defaults to stderr when no handler is installed. */
void sgc_set_log_handler(sgc_log_handler_t handler);

void sgc_log(int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
