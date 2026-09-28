/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include <glib.h>

typedef struct _NmAt NmAt;

/*
 * Minimal AT command helper for the modem's rpmsg AT port (/dev/wwan0at0).
 * Only the 3GPP commands the firmware quirks need are used through it:
 * CFUN, COPS, CREG, CGSMS.
 */
NmAt *nm_at_new(const gchar *path);
void nm_at_free(NmAt *at);

/*
 * Send one command and wait for its final result code (OK/ERROR/+CME/+CMS).
 * The full response (including the echo) is returned in @response when asked
 * for.  The port is opened on first use and reopened transparently if it went
 * away.
 */
gboolean nm_at_command(NmAt *at, const gchar *command, guint timeout_ms,
		       gchar **response, GError **error);
