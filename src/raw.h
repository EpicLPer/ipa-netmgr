/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include <libqmi-glib.h>

/* One TLV of a raw QMI request. */
typedef struct {
	guint8 type;
	const guint8 *data;
	gsize length;
} NmQmiRawTlv;

/*
 * Called with the response message (borrowed; valid only inside the callback)
 * or with an error.  On success the caller parses TLVs with
 * nm_qmi_raw_get_result()/qmi_message_get_raw_tlv().
 */
typedef void (*NmQmiRawDoneFunc)(QmiMessage *response, GError *error,
				 gpointer user_data);

/*
 * Send one raw QMI request (message id + TLVs) on an open QmiDevice using an
 * already allocated client of @service.  Used for requests libqmi does not
 * expose: UIM SUBSCRIPTION_OK (0x0040) and NAS DUAL_STANDBY_PREF (0x004B).
 */
void nm_qmi_raw_call(QmiDevice *device, QmiService service, guint8 cid,
		     guint16 message_id, const NmQmiRawTlv *tlvs, guint n_tlvs,
		     guint timeout, NmQmiRawDoneFunc done, gpointer user_data);

/* Parse the mandatory result TLV (0x02), setting @qmi_error if not required. */
gboolean nm_qmi_raw_get_result(QmiMessage *response, guint16 *qmi_error,
			       GError **error);
