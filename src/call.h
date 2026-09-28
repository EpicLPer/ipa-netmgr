/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include <glib.h>
#include <libqmi-glib.h>

#include "config.h"

typedef struct _NmCall NmCall;
typedef void (*NmCallDoneFunc)(GError *error, gpointer user_data);

NmCall *nm_call_new(const NmConfig *config);
void nm_call_free(NmCall *call);

/*
 * Start a WDS call on the modem's control channel.  The QmiDevice is shared
 * with the modem lifecycle (one device per channel: this firmware hangs up an
 * endpoint that is opened twice), and the WDS client is kept allocated for the
 * call's lifetime.  The callback reports the address, gateway and DNS obtained
 * from the call.
 */
void nm_call_start(NmCall *call, QmiDevice *device, NmCallDoneFunc done,
		   gpointer user_data);

/* Stop the network and release the WDS client (does not touch the device). */
void nm_call_stop(NmCall *call, NmCallDoneFunc done, gpointer user_data);

/*
 * Abandon the current call (modem lost).  Any operation still in flight
 * becomes stale and is dropped by its callback; the object stays valid for
 * the process lifetime so no callback ever touches freed memory.
 */
void nm_call_abort(NmCall *call);

gboolean nm_call_is_active(NmCall *call);

const gchar *nm_call_get_ipv4(NmCall *call);
const gchar *nm_call_get_gateway(NmCall *call);
const gchar *nm_call_get_dns(NmCall *call);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(NmCall, nm_call_free)
