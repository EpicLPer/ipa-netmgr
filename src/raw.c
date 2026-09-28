/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Raw QMI requests for messages libqmi does not implement.  The QMI framing is
 * handled by libqmi (qmi_message_new() + qmi_message_add_raw_tlv() +
 * qmi_device_command_full()), so only the TLV payloads are hand-built from the
 * measured wire format; no protocol code is duplicated.
 */
#include "raw.h"
#include "log.h"

typedef struct {
	NmQmiRawDoneFunc done;
	gpointer user_data;
} RawCall;

static guint16 next_transaction = 1;

static void raw_call_done(GObject *source, GAsyncResult *res, gpointer user_data)
{
	QmiDevice *device = QMI_DEVICE(source);
	RawCall *ctx = user_data;
	g_autoptr(GError) error = NULL;
	g_autoptr(QmiMessage) response = NULL;

	response = qmi_device_command_full_finish(device, res, &error);

	if (ctx->done)
		ctx->done(response, error, ctx->user_data);
	g_free(ctx);
}

void nm_qmi_raw_call(QmiDevice *device, QmiService service, guint8 cid,
		     guint16 message_id, const NmQmiRawTlv *tlvs, guint n_tlvs,
		     guint timeout, NmQmiRawDoneFunc done, gpointer user_data)
{
	g_autoptr(QmiMessage) message = NULL;
	RawCall *ctx;

	message = qmi_message_new(service, cid, next_transaction++, message_id);
	for (guint i = 0; i < n_tlvs; i++) {
		g_autoptr(GError) error = NULL;

		if (qmi_message_add_raw_tlv(message, tlvs[i].type, tlvs[i].data,
					    tlvs[i].length, &error))
			continue;

		nm_warn("could not build raw QMI message 0x%04x: %s", message_id,
			error->message);
		if (done)
			done(NULL, error, user_data);
		return;
	}

	ctx = g_new0(RawCall, 1);
	ctx->done = done;
	ctx->user_data = user_data;
	qmi_device_command_full(device, message, NULL, timeout, NULL,
				raw_call_done, ctx);
}

gboolean nm_qmi_raw_get_result(QmiMessage *response, guint16 *qmi_error,
			       GError **error)
{
	const guint8 *tlv;
	guint16 length;
	guint16 result;
	guint16 code;

	tlv = qmi_message_get_raw_tlv(response, 0x02, &length);
	if (!tlv || length < 4) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
			    "QMI response has no result TLV");
		return FALSE;
	}

	result = tlv[0] | ((guint16)tlv[1] << 8);
	code = tlv[2] | ((guint16)tlv[3] << 8);

	if (qmi_error)
		*qmi_error = code;

	if (result != 0) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
			    "QMI protocol error %u", code);
		return FALSE;
	}

	return TRUE;
}
