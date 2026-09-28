/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * WDA data format.  The mainline rmnet driver speaks the 4-byte QMAP framing,
 * so the modem must be told raw-ip link layer with QMAP aggregation on both
 * directions for the embedded data endpoint before any call is started.
 */
#include "wda.h"
#include "log.h"

typedef struct {
	QmiDevice *device;
	QmiClient *client;
	const NmConfig *config;
	NmWdaDoneFunc done;
	gpointer user_data;
} WdaRequest;

static void wda_report(WdaRequest *request, GError *error)
{
	if (request->client) {
		qmi_device_release_client(request->device, request->client,
					  QMI_DEVICE_RELEASE_CLIENT_FLAGS_RELEASE_CID,
					  5, NULL, NULL, NULL);
		g_clear_object(&request->client);
	}
	g_clear_object(&request->device);
	if (request->done)
		request->done(error, request->user_data);
	g_free(request);
}

static void wda_set_ready(QmiClientWda *client, GAsyncResult *res,
			  gpointer user_data)
{
	WdaRequest *request = user_data;
	g_autoptr(QmiMessageWdaSetDataFormatOutput) output = NULL;
	g_autoptr(GError) error = NULL;

	output = qmi_client_wda_set_data_format_finish(client, res, &error);
	if (!output ||
	    !qmi_message_wda_set_data_format_output_get_result(output, &error)) {
		wda_report(request, error);
		return;
	}

	nm_info("WDA data format set (raw-ip, qmap/qmap, ep %u/%u)",
		request->config->wda_ep_type, request->config->wda_ep_iface);
	wda_report(request, NULL);
}

static void wda_client_ready(QmiDevice *device, GAsyncResult *res,
			     gpointer user_data)
{
	WdaRequest *request = user_data;
	g_autoptr(QmiMessageWdaSetDataFormatInput) input = NULL;
	g_autoptr(GError) error = NULL;

	request->client = qmi_device_allocate_client_finish(device, res, &error);
	if (!request->client) {
		wda_report(request, error);
		return;
	}

	input = qmi_message_wda_set_data_format_input_new();
	if (!qmi_message_wda_set_data_format_input_set_link_layer_protocol(input,
			QMI_WDA_LINK_LAYER_PROTOCOL_RAW_IP, &error) ||
	    !qmi_message_wda_set_data_format_input_set_uplink_data_aggregation_protocol(input,
			QMI_WDA_DATA_AGGREGATION_PROTOCOL_QMAP, &error) ||
	    !qmi_message_wda_set_data_format_input_set_downlink_data_aggregation_protocol(input,
			QMI_WDA_DATA_AGGREGATION_PROTOCOL_QMAP, &error) ||
	    !qmi_message_wda_set_data_format_input_set_endpoint_info(input,
			request->config->wda_ep_type,
			request->config->wda_ep_iface, &error)) {
		wda_report(request, error);
		return;
	}

	qmi_client_wda_set_data_format(QMI_CLIENT_WDA(request->client), input, 30,
				       NULL,
				       (GAsyncReadyCallback)wda_set_ready,
				       request);
}

void nm_wda_apply(QmiDevice *device, const NmConfig *config,
		  NmWdaDoneFunc done, gpointer user_data)
{
	WdaRequest *request = g_new0(WdaRequest, 1);

	request->device = g_object_ref(device);
	request->config = config;
	request->done = done;
	request->user_data = user_data;

	qmi_device_allocate_client(device, QMI_SERVICE_WDA, QMI_CID_NONE, 10,
				   NULL,
				   (GAsyncReadyCallback)wda_client_ready,
				   request);
}
