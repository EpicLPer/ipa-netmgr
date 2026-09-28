/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include <glib.h>

#include "config.h"

typedef struct _NmLink NmLink;
typedef void (*NmLinkLostFunc)(gpointer user_data);

NmLink *nm_link_new(const NmConfig *config);
void nm_link_free(NmLink *link);

/* Name of the mux link (the configured pattern with the mux id filled in). */
const gchar *nm_link_get_name(NmLink *link);

/*
 * Create the rmnet mux link if it does not exist, set its MTU and bring it up.
 * The base interface (rmnet_ipa0) is brought up as well.
 */
gboolean nm_link_ensure(NmLink *link, GError **error);

/*
 * Assign the IPv4 address from the WDS call, the default route via the call's
 * gateway and the resolver configuration (best effort; Linux mode only).
 */
gboolean nm_link_configure(NmLink *link, const gchar *ip, const gchar *gw,
			   const gchar *dns, GError **error);

void nm_link_remove(NmLink *link);

/*
 * Watch the link for deletion (ModemManager resets the IPA data interface
 * while it initializes the modem; the link then has to be recreated).
 */
gboolean nm_link_watch(NmLink *link, NmLinkLostFunc lost, gpointer user_data,
		       GError **error);
