/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include <glib.h>
#include <libqmi-glib.h>

#include "config.h"

typedef struct _NmModem NmModem;

typedef void (*NmModemReadyFunc)(gpointer user_data);
typedef void (*NmModemLostFunc)(gpointer user_data);
typedef void (*NmModemStoppedFunc)(gpointer user_data);

NmModem *nm_modem_new(const NmConfig *config);
void nm_modem_free(NmModem *modem);

void nm_modem_set_ready(NmModem *modem, NmModemReadyFunc fn, gpointer user_data);
void nm_modem_set_lost(NmModem *modem, NmModemLostFunc fn, gpointer user_data);
void nm_modem_set_stopped(NmModem *modem, NmModemStoppedFunc fn,
			  gpointer user_data);

/*
 * Watch the modem's remoteproc, start it when down and run the firmware quirks
 * once it is running (DPM session, WDA data format, SIM application and
 * subscription, NAS dual-standby, DMS online, radio cycle, network selection,
 * SMS bearer).  The ready callback fires when the modem is registered; the
 * lost callback fires when it disappears again (the caller must tear its data
 * path down).
 */
void nm_modem_start(NmModem *modem);

/* Release all QMI clients and the DPM session, then call the stopped callback. */
void nm_modem_stop(NmModem *modem);

gboolean nm_modem_is_ready(NmModem *modem);

/* Channel that serves UIM/WDS (valid while ready): used as the preferred one. */
const gchar *nm_modem_get_control_channel(NmModem *modem);

/* Open QMI device of the control channel (valid while ready).  All services
 * (UIM/NAS/DMS/WDA/WDS) share it: this firmware hangs up the endpoint when the
 * same QMI channel is opened more than once at a time. */
QmiDevice *nm_modem_get_device(NmModem *modem);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(NmModem, nm_modem_free)
