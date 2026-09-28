/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "at.h"
#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <gio/gio.h>
#include <poll.h>
#include <string.h>
#include <unistd.h>

struct _NmAt {
	gchar *path;
	gint fd;
	GString *response;
};

NmAt *nm_at_new(const gchar *path)
{
	NmAt *at = g_new0(NmAt, 1);

	at->path = g_strdup(path);
	at->fd = -1;
	at->response = g_string_new(NULL);

	return at;
}

void nm_at_free(NmAt *at)
{
	if (!at)
		return;
	if (at->fd >= 0)
		close(at->fd);
	g_string_free(at->response, TRUE);
	g_free(at->path);
	g_free(at);
}

static void at_close(NmAt *at)
{
	if (at->fd >= 0) {
		close(at->fd);
		at->fd = -1;
	}
}

static gboolean at_open(NmAt *at, GError **error)
{
	if (at->fd >= 0)
		return TRUE;

	at->fd = open(at->path, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
	if (at->fd < 0) {
		g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno),
			    "could not open %s: %s", at->path, g_strerror(errno));
		return FALSE;
	}

	return TRUE;
}

/* Drop anything the port sent before we write (unsolicited notifications). */
static void at_drain(NmAt *at)
{
	gchar buf[512];

	while (at->fd >= 0 && read(at->fd, buf, sizeof(buf)) > 0)
		;
}

static gboolean at_write(NmAt *at, const gchar *command, GError **error)
{
	g_autofree gchar *line = g_strdup_printf("%s\r", command);
	gsize length = strlen(line);
	gsize written = 0;

	while (written < length) {
		ssize_t n = write(at->fd, line + written, length - written);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				struct pollfd pfd = { .fd = at->fd, .events = POLLOUT };

				poll(&pfd, 1, 100);
				continue;
			}
			g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno),
				    "could not write to %s: %s", at->path,
				    g_strerror(errno));
			at_close(at);
			return FALSE;
		}
		written += n;
	}

	return TRUE;
}

static gboolean at_has_final_result(const gchar *response)
{
	return strstr(response, "\r\nOK\r\n") != NULL ||
	       strstr(response, "\r\nERROR\r\n") != NULL ||
	       strstr(response, "\r\n+CME ERROR") != NULL ||
	       strstr(response, "\r\n+CMS ERROR") != NULL ||
	       strstr(response, "\r\nNO CARRIER") != NULL;
}

gboolean nm_at_command(NmAt *at, const gchar *command, guint timeout_ms,
		       gchar **response, GError **error)
{
	gint64 deadline;

	if (!at_open(at, error))
		return FALSE;

	at_drain(at);
	if (!at_write(at, command, error))
		return FALSE;

	g_string_truncate(at->response, 0);
	deadline = g_get_monotonic_time() + ((gint64)timeout_ms * 1000);

	for (;;) {
		struct pollfd pfd = { .fd = at->fd, .events = POLLIN };
		gint64 remaining = deadline - g_get_monotonic_time();
		gchar buf[1024];
		ssize_t n;
		int ret;

		if (remaining <= 0) {
			g_set_error(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
				    "'%s' did not answer within %u ms", command,
				    timeout_ms);
			return FALSE;
		}

		ret = poll(&pfd, 1, (gint)(remaining / 1000) + 1);
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno),
				    "poll on %s failed: %s", at->path,
				    g_strerror(errno));
			at_close(at);
			return FALSE;
		}
		if (ret == 0)
			continue;

		n = read(at->fd, buf, sizeof(buf));
		if (n > 0) {
			g_string_append_len(at->response, buf, n);
			if (at_has_final_result(at->response->str)) {
				if (response)
					*response = g_strdup(at->response->str);
				return TRUE;
			}
			continue;
		}
		if (n == 0) {
			g_set_error(error, G_IO_ERROR, G_IO_ERROR_CLOSED,
				    "%s closed while waiting for '%s'", at->path,
				    command);
			at_close(at);
			return FALSE;
		}
		if (errno == EINTR || errno == EAGAIN)
			continue;

		g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno),
			    "could not read from %s: %s", at->path,
			    g_strerror(errno));
		at_close(at);
		return FALSE;
	}
}
