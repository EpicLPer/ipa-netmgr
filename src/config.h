/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include <glib.h>

/*
 * Runtime configuration for ipa-netmgr.  Values default to the measured
 * MSM8994 / MPSS.BO.2.5 platform setup; see data/ipa-netmgr.conf.example.
 */
typedef struct {
	/* "linux": daemon owns the data call; "android": RIL owns it */
	gchar *mode;

	/* Modem lifecycle: the daemon starts the remoteproc and runs the
	 * firmware quirks itself (no helper scripts). */
	gboolean manage_modem;
	gchar *modem_state;       /* /sys/class/remoteproc/remoteproc0/state */

	/* QRTR node that serves the DPM/IPA services */
	guint32 qrtr_node;

	/* DPM session (held open for the modem's uptime) */
	gboolean dpm_enabled;
	gchar *control_port;      /* DATA5_CNTL */
	guint32 control_ep_type;  /* QmiDataEndpointType: 4 = embedded */
	guint32 control_iface;
	guint32 hw_ep_type;
	guint32 hw_iface;
	guint32 hw_rx;            /* modem RX endpoint -> IPA TX */
	guint32 hw_tx;            /* modem TX endpoint -> IPA RX */

	/* WDA data format */
	gboolean wda_enabled;
	gchar **qmi_devices;      /* NULL-terminated candidate QMI channels */
	guint32 wda_ep_type;
	guint32 wda_ep_iface;

	/* Firmware quirks (UIM / NAS / DMS / AT), applied before ModemManager */
	gchar *at_device;         /* /dev/wwan0at0 */
	gchar *home_plmn;         /* 23203 */
	gchar *apn;               /* profile 1 APN */
	guint32 registration_timeout_ms;       /* automatic network selection */
	guint32 registration_manual_ms;        /* explicit home PLMN */

	/* Data call (mode = linux: the daemon owns the call) */
	gboolean call_enabled;
	guint32 profile;          /* 3GPP profile index (carries the APN) */
	guint32 mux_id;
	guint32 subscription;     /* 1 = primary, 2 = secondary */

	/* rmnet mux link created for the call */
	gchar *base_link;         /* rmnet_ipa0 (the IPA data interface) */
	gchar *link_name;         /* printf pattern, %u = mux id */
	guint32 prefix_len;

	/* Link recovery (ModemManager resets the IPA interface on init) */
	guint32 link_max_recreates;
	guint32 link_recreate_ms;

	/* misc */
	gchar *log_level;
	guint32 retry_min_ms;     /* bring-up retry backoff, first step */
	guint32 retry_max_ms;
} NmConfig;

NmConfig *nm_config_load(const gchar *path, GError **error);
void nm_config_free(NmConfig *config);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(NmConfig, nm_config_free)
