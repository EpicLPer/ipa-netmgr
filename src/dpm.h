/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include <libqmi-glib.h>

#include "config.h"

typedef struct _NmDpm NmDpm;
typedef void (*NmDpmDoneFunc)(GError *error, gpointer user_data);

NmDpm *nm_dpm_new(const NmConfig *config);
void nm_dpm_free(NmDpm *dpm);

/*
 * Open the DPM control and hardware data ports over QRTR and keep the QMI
 * client for as long as the modem is up.  The callback is invoked once the
 * session is ready (error == NULL) or has failed.
 */
void nm_dpm_start(NmDpm *dpm, NmDpmDoneFunc done, gpointer user_data);

/* Close the ports and release the client (best effort). */
void nm_dpm_stop(NmDpm *dpm, NmDpmDoneFunc done, gpointer user_data);

gboolean nm_dpm_is_open(NmDpm *dpm);

/*
 * Drop the current DPM session (modem restart) without destroying the QRTR
 * bus: the bus and its main-loop watches live for the whole process, freeing
 * them while libqrtr still has pending callbacks crashes the daemon.
 */
void nm_dpm_reset(NmDpm *dpm);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(NmDpm, nm_dpm_free)
