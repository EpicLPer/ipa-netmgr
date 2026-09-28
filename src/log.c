/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <time.h>

static NmLogLevel log_level = NM_LOG_INFO;

void nm_log_set_level(NmLogLevel level)
{
	log_level = level;
}

void nm_log(NmLogLevel level, const gchar *fmt, ...)
{
	static const gchar *names[] = { "error", "warn", "info", "debug" };
	struct timespec ts;
	struct tm tm;
	gchar stamp[32];
	va_list ap;

	if (level > log_level)
		return;

	clock_gettime(CLOCK_REALTIME, &ts);
	localtime_r(&ts.tv_sec, &tm);
	strftime(stamp, sizeof(stamp), "%H:%M:%S", &tm);

	fprintf(stderr, "%s.%03ld %-5s ipa-netmgr: ", stamp, ts.tv_nsec / 1000000,
		names[level]);

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);

	fputc('\n', stderr);
}
