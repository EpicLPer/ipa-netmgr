/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include <glib.h>

typedef enum {
	NM_LOG_ERROR = 0,
	NM_LOG_WARN,
	NM_LOG_INFO,
	NM_LOG_DEBUG,
} NmLogLevel;

void nm_log_set_level(NmLogLevel level);
void nm_log(NmLogLevel level, const gchar *fmt, ...) G_GNUC_PRINTF(2, 3);

#define nm_error(...) nm_log(NM_LOG_ERROR, __VA_ARGS__)
#define nm_warn(...)  nm_log(NM_LOG_WARN, __VA_ARGS__)
#define nm_info(...)  nm_log(NM_LOG_INFO, __VA_ARGS__)
#define nm_debug(...) nm_log(NM_LOG_DEBUG, __VA_ARGS__)
