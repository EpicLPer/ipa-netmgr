/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include <libqmi-glib.h>

#include "config.h"

typedef void (*NmWdaDoneFunc)(GError *error, gpointer user_data);

/*
 * Apply the QMAP data format on an already open QMI device (the modem's
 * control channel).  The format persists on the modem; the client is released
 * again afterwards.  Keeping every service on one QmiDevice matters on this
 * firmware: multiple simultaneous opens of the same rpmsg QMI channel make
 * the modem hang up the endpoint.
 */
void nm_wda_apply(QmiDevice *device, const NmConfig *config,
		  NmWdaDoneFunc done, gpointer user_data);
