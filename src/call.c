/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * WDS data call (linux mode).
 *
 * On this firmware the QMI services are split: DMS/NAS/WDS/UIM live on the
 * rpmsg channels (/dev/wwan0qmiN) and DPM/IPA only on QRTR.  The call consists
 * of client-bound requests on one WDS client that is allocated on the modem's
 * already open control device:
 *
 *   1. Modify Profile         (0x002C, 3GPP profile 1: APN and PDP type)
 *   2. Set IP Family          (0x004D, IPv4)
 *   3. Bind Subscription      (0x00AF, primary)
 *   4. Bind Mux Data Port     (0x00A2, embedded endpoint, mux 1)
 *   5. Start Network          (0x0020, 3GPP profile, no APN TLV)
 *
 * The call lives with the QMI client, so the client is held open until the
 * call is stopped; releasing it (RELEASE_CID) ends the call on the modem.
 * The start response's address TLVs are not exposed by libqmi, so the address,
 * gateway and DNS are read with the typed getters of Get Current Settings on
 * the client that owns the call.  Those getters return the raw little-endian
 * TLV value (no byteswap, verified by disassembly), so ipv4_to_string() swaps
 * it back before inet_ntop().
 *
 * The NmCall object lives for the whole process: a modem restart aborts the
 * current chain (generation bump) and in-flight callbacks from the old chain
 * become no-ops instead of touching freed memory.
 */
#include "call.h"
#include "log.h"

#include <arpa/inet.h>
#include <gio/gio.h>

struct _NmCall {
	const NmConfig *config;
	guint generation;
	QmiDevice *device;
	QmiClient *client;
	gboolean active;
	guint32 packet_data_handle;
	gchar *ipv4;
	gchar *gateway;
	gchar *dns;
	NmCallDoneFunc done;
	gpointer user_data;
};

typedef struct {
	NmCall *call;
	guint generation;
} CallOp;

NmCall *nm_call_new(const NmConfig *config)
{
	NmCall *call = g_new0(NmCall, 1);

	call->config = config;

	return call;
}

void nm_call_free(NmCall *call)
{
	if (!call)
		return;
	if (call->client) {
		if (call->device && qmi_device_is_open(call->device))
			qmi_device_release_client(call->device, call->client,
						  QMI_DEVICE_RELEASE_CLIENT_FLAGS_RELEASE_CID,
						  5, NULL, NULL, NULL);
		g_clear_object(&call->client);
	}
	g_clear_object(&call->device);
	g_free(call->ipv4);
	g_free(call->gateway);
	g_free(call->dns);
	g_free(call);
}

gboolean nm_call_is_active(NmCall *call)
{
	return call && call->active;
}

const gchar *nm_call_get_ipv4(NmCall *call)
{
	return call->ipv4;
}

const gchar *nm_call_get_gateway(NmCall *call)
{
	return call->gateway;
}

const gchar *nm_call_get_dns(NmCall *call)
{
	return call->dns;
}

static CallOp *call_op_new(NmCall *call)
{
	CallOp *op = g_new0(CallOp, 1);

	op->call = call;
	op->generation = call->generation;

	return op;
}

static gboolean call_op_stale(CallOp *op)
{
	return op->call->generation != op->generation;
}

/* Terminal step: report the result (unless the chain is stale) and free it. */
static void call_report(CallOp *op, GError *error)
{
	NmCall *call = op->call;

	if (!call_op_stale(op)) {
		NmCallDoneFunc done = call->done;
		gpointer user_data = call->user_data;

		call->done = NULL;
		call->user_data = NULL;

		if (done)
			done(error, user_data);
	}

	g_free(op);
}

static void call_release_client(NmCall *call)
{
	if (call->client) {
		if (call->device && qmi_device_is_open(call->device))
			qmi_device_release_client(call->device, call->client,
						  QMI_DEVICE_RELEASE_CLIENT_FLAGS_RELEASE_CID,
						  5, NULL, NULL, NULL);
		g_clear_object(&call->client);
	}
}

/* The typed IPv4 getters return the raw 32-bit little-endian value from the
 * QMI TLV (libqmi loads the field without a byteswap), so swap it back to
 * network order before inet_ntop(). */
static gchar *ipv4_to_string(guint32 value)
{
	struct in_addr addr;
	gchar buf[INET_ADDRSTRLEN];

	addr.s_addr = htonl(value);
	if (!inet_ntop(AF_INET, &addr, buf, sizeof(buf)))
		return NULL;

	return g_strdup(buf);
}

static void call_fail(CallOp *op, GError *error, const gchar *what)
{
	NmCall *call = op->call;

	if (call_op_stale(op)) {
		call_report(op, NULL);
		return;
	}

	nm_warn("%s: %s", what, error->message);
	call_release_client(call);
	/* A retry refs the device again */
	g_clear_object(&call->device);
	call_report(op, error);
}

static void call_settings_done(QmiClientWds *client, GAsyncResult *res,
			       gpointer user_data)
{
	CallOp *op = user_data;
	NmCall *call = op->call;
	g_autoptr(QmiMessageWdsGetCurrentSettingsOutput) output = NULL;
	g_autoptr(GError) error = NULL;
	guint32 addr = 0;

	if (call_op_stale(op)) {
		call_report(op, NULL);
		return;
	}

	output = qmi_client_wds_get_current_settings_finish(client, res, &error);
	if (!output) {
		call_fail(op, error, "WDS get current settings failed");
		return;
	}
	if (!qmi_message_wds_get_current_settings_output_get_result(output, &error)) {
		call_fail(op, error, "WDS get current settings rejected");
		return;
	}

	if (qmi_message_wds_get_current_settings_output_get_ipv4_address(output, &addr, NULL))
		call->ipv4 = ipv4_to_string(addr);
	addr = 0;
	if (qmi_message_wds_get_current_settings_output_get_ipv4_gateway_address(output, &addr, NULL))
		call->gateway = ipv4_to_string(addr);
	addr = 0;
	if (qmi_message_wds_get_current_settings_output_get_primary_ipv4_dns_address(output, &addr, NULL))
		call->dns = ipv4_to_string(addr);

	if (!call->ipv4 || !call->gateway) {
		g_autoptr(GError) missing = NULL;

		g_set_error(&missing, G_IO_ERROR, G_IO_ERROR_FAILED,
			    "call has no IPv4 address/gateway");
		call_fail(op, missing, "WDS call incomplete");
		return;
	}

	call->active = TRUE;
	nm_info("data call started (ip %s gw %s dns %s)", call->ipv4,
		call->gateway, call->dns ? call->dns : "none");
	call_report(op, NULL);
}

static void call_start_done(QmiClientWds *client, GAsyncResult *res,
			    gpointer user_data)
{
	CallOp *op = user_data;
	NmCall *call = op->call;
	g_autoptr(QmiMessageWdsStartNetworkOutput) output = NULL;
	g_autoptr(QmiMessageWdsGetCurrentSettingsInput) input = NULL;
	g_autoptr(GError) error = NULL;

	if (call_op_stale(op)) {
		call_report(op, NULL);
		return;
	}

	output = qmi_client_wds_start_network_finish(client, res, &error);
	if (!output) {
		call_fail(op, error, "WDS start network failed");
		return;
	}
	if (!qmi_message_wds_start_network_output_get_result(output, &error)) {
		call_fail(op, error, "WDS start network rejected");
		return;
	}

	qmi_message_wds_start_network_output_get_packet_data_handle(output,
								    &call->packet_data_handle,
								    NULL);

	/* Read the address/gateway/DNS with Get Current Settings on the call's
	 * own client (the start response's TLVs are not exposed by libqmi); its
	 * requested-settings mask is mandatory. */
	input = qmi_message_wds_get_current_settings_input_new();
	if (!qmi_message_wds_get_current_settings_input_set_requested_settings(input,
			QMI_WDS_REQUESTED_SETTINGS_IP_ADDRESS |
			QMI_WDS_REQUESTED_SETTINGS_GATEWAY_INFO |
			QMI_WDS_REQUESTED_SETTINGS_DNS_ADDRESS, &error)) {
		call_fail(op, error, "could not build WDS settings request");
		return;
	}
	qmi_client_wds_get_current_settings(QMI_CLIENT_WDS(call->client), input,
					    10, NULL,
					    (GAsyncReadyCallback)call_settings_done,
					    op);
}

static void call_bind_mux_done(QmiClientWds *client, GAsyncResult *res,
			       gpointer user_data)
{
	CallOp *op = user_data;
	NmCall *call = op->call;
	g_autoptr(QmiMessageWdsBindMuxDataPortOutput) output = NULL;
	g_autoptr(QmiMessageWdsStartNetworkInput) input = NULL;
	g_autoptr(GError) error = NULL;

	if (call_op_stale(op)) {
		call_report(op, NULL);
		return;
	}

	output = qmi_client_wds_bind_mux_data_port_finish(client, res, &error);
	if (!output) {
		call_fail(op, error, "WDS bind mux data port failed");
		return;
	}
	if (!qmi_message_wds_bind_mux_data_port_output_get_result(output, &error)) {
		call_fail(op, error, "WDS bind mux data port rejected");
		return;
	}

	input = qmi_message_wds_start_network_input_new();
	if (!qmi_message_wds_start_network_input_set_profile_index_3gpp(input,
			call->config->profile, &error) ||
	    !qmi_message_wds_start_network_input_set_ip_family_preference(input,
			QMI_WDS_IP_FAMILY_IPV4, &error)) {
		call_fail(op, error, "could not build WDS start request");
		return;
	}

	/* The APN comes from the 3GPP profile; no APN TLV is sent. */
	qmi_client_wds_start_network(QMI_CLIENT_WDS(call->client), input, 30, NULL,
				     (GAsyncReadyCallback)call_start_done, op);
}

static void call_bind_sub_done(QmiClientWds *client, GAsyncResult *res,
			       gpointer user_data)
{
	CallOp *op = user_data;
	NmCall *call = op->call;
	g_autoptr(QmiMessageWdsBindSubscriptionOutput) output = NULL;
	g_autoptr(QmiMessageWdsBindMuxDataPortInput) input = NULL;
	g_autoptr(GError) error = NULL;

	if (call_op_stale(op)) {
		call_report(op, NULL);
		return;
	}

	output = qmi_client_wds_bind_subscription_finish(client, res, &error);
	if (!output) {
		call_fail(op, error, "WDS bind subscription failed");
		return;
	}
	if (!qmi_message_wds_bind_subscription_output_get_result(output, &error)) {
		call_fail(op, error, "WDS bind subscription rejected");
		return;
	}

	input = qmi_message_wds_bind_mux_data_port_input_new();
	if (!qmi_message_wds_bind_mux_data_port_input_set_endpoint_info(input,
			QMI_DATA_ENDPOINT_TYPE_EMBEDDED, call->config->hw_iface,
			&error) ||
	    !qmi_message_wds_bind_mux_data_port_input_set_mux_id(input,
			call->config->mux_id, &error)) {
		call_fail(op, error, "could not build WDS bind request");
		return;
	}

	qmi_client_wds_bind_mux_data_port(QMI_CLIENT_WDS(call->client), input, 10,
					  NULL,
					  (GAsyncReadyCallback)call_bind_mux_done,
					  op);
}

static void call_ip_family_done(QmiClientWds *client, GAsyncResult *res,
				gpointer user_data)
{
	CallOp *op = user_data;
	NmCall *call = op->call;
	g_autoptr(QmiMessageWdsSetIpFamilyOutput) output = NULL;
	g_autoptr(QmiMessageWdsBindSubscriptionInput) input = NULL;
	g_autoptr(GError) error = NULL;

	if (call_op_stale(op)) {
		call_report(op, NULL);
		return;
	}

	output = qmi_client_wds_set_ip_family_finish(client, res, &error);
	if (!output) {
		call_fail(op, error, "WDS set IP family failed");
		return;
	}
	if (!qmi_message_wds_set_ip_family_output_get_result(output, &error)) {
		call_fail(op, error, "WDS set IP family rejected");
		return;
	}

	input = qmi_message_wds_bind_subscription_input_new();
	if (!qmi_message_wds_bind_subscription_input_set_subscription_id(input,
			call->config->subscription == 2
				? QMI_SUBSCRIPTION_TYPE_SECONDARY
				: QMI_SUBSCRIPTION_TYPE_PRIMARY, &error)) {
		call_fail(op, error, "could not build WDS bind request");
		return;
	}

	qmi_client_wds_bind_subscription(QMI_CLIENT_WDS(call->client), input, 10,
					 NULL,
					 (GAsyncReadyCallback)call_bind_sub_done,
					 op);
}

static void call_profile_done(QmiClientWds *client, GAsyncResult *res,
			      gpointer user_data)
{
	CallOp *op = user_data;
	NmCall *call = op->call;
	g_autoptr(QmiMessageWdsModifyProfileOutput) output = NULL;
	g_autoptr(QmiMessageWdsSetIpFamilyInput) input = NULL;
	g_autoptr(GError) error = NULL;

	if (call_op_stale(op)) {
		call_report(op, NULL);
		return;
	}

	/* A rejected profile update is not fatal: the stored profile may already
	 * be correct. */
	output = qmi_client_wds_modify_profile_finish(client, res, &error);
	if (!output ||
	    !qmi_message_wds_modify_profile_output_get_result(output, &error))
		nm_warn("could not set profile %u APN '%s': %s",
			call->config->profile, call->config->apn,
			error->message);

	input = qmi_message_wds_set_ip_family_input_new();
	if (!qmi_message_wds_set_ip_family_input_set_preference(input,
			QMI_WDS_IP_FAMILY_IPV4, &error)) {
		call_fail(op, error, "could not build WDS request");
		return;
	}
	qmi_client_wds_set_ip_family(QMI_CLIENT_WDS(call->client), input, 10, NULL,
				     (GAsyncReadyCallback)call_ip_family_done,
				     op);
}

static void call_client_ready(QmiDevice *device, GAsyncResult *res,
			      gpointer user_data)
{
	CallOp *op = user_data;
	NmCall *call = op->call;
	g_autoptr(QmiMessageWdsModifyProfileInput) input = NULL;
	g_autoptr(GError) error = NULL;

	if (call_op_stale(op)) {
		call_report(op, NULL);
		return;
	}

	call->client = qmi_device_allocate_client_finish(device, res, &error);
	if (!call->client) {
		call_fail(op, error, "could not allocate WDS client");
		return;
	}

	/* The call uses the 3GPP profile (no APN TLV is sent), so keep its APN
	 * in sync with the configuration. */
	input = qmi_message_wds_modify_profile_input_new();
	if (!qmi_message_wds_modify_profile_input_set_profile_identifier(input,
			QMI_WDS_PROFILE_TYPE_3GPP, call->config->profile, &error) ||
	    !qmi_message_wds_modify_profile_input_set_apn_name(input,
			call->config->apn, &error) ||
	    !qmi_message_wds_modify_profile_input_set_pdp_type(input,
			QMI_WDS_PDP_TYPE_IPV4_OR_IPV6, &error)) {
		call_fail(op, error, "could not build the WDS profile request");
		return;
	}

	qmi_client_wds_modify_profile(QMI_CLIENT_WDS(call->client), input, 10,
				      NULL,
				      (GAsyncReadyCallback)call_profile_done,
				      op);
}

void nm_call_start(NmCall *call, QmiDevice *device, NmCallDoneFunc done,
		   gpointer user_data)
{
	CallOp *op;

	g_clear_object(&call->device);
	call->device = g_object_ref(device);
	call->active = FALSE;
	call->done = done;
	call->user_data = user_data;

	op = call_op_new(call);
	qmi_device_allocate_client(call->device, QMI_SERVICE_WDS, QMI_CID_NONE,
				   10, NULL,
				   (GAsyncReadyCallback)call_client_ready, op);
}

void nm_call_abort(NmCall *call)
{
	if (!call)
		return;

	call->generation++;
	call->active = FALSE;
	call->packet_data_handle = 0;
	call->done = NULL;
	call->user_data = NULL;

	call_release_client(call);
	g_clear_object(&call->device);
	g_clear_pointer(&call->ipv4, g_free);
	g_clear_pointer(&call->gateway, g_free);
	g_clear_pointer(&call->dns, g_free);
}

/* ------------------------------------------------------------------- stop */

static void call_stop_release_done(QmiDevice *device, GAsyncResult *res,
				   gpointer user_data)
{
	CallOp *op = user_data;
	NmCall *call = op->call;
	g_autoptr(GError) error = NULL;

	if (!qmi_device_release_client_finish(device, res, &error))
		nm_warn("could not release the WDS client: %s", error->message);

	g_clear_object(&call->client);
	call->active = FALSE;
	nm_info("data call stopped");
	call_report(op, NULL);
}

static void call_stop_network_done(QmiClientWds *client, GAsyncResult *res,
				   gpointer user_data)
{
	CallOp *op = user_data;
	NmCall *call = op->call;
	g_autoptr(QmiMessageWdsStopNetworkOutput) output = NULL;
	g_autoptr(GError) error = NULL;

	if (call_op_stale(op)) {
		call_report(op, NULL);
		return;
	}

	output = qmi_client_wds_stop_network_finish(client, res, &error);
	if (!output ||
	    !qmi_message_wds_stop_network_output_get_result(output, &error))
		nm_debug("WDS stop network: %s",
			 error ? error->message : "failed");

	qmi_device_release_client(call->device, call->client,
				  QMI_DEVICE_RELEASE_CLIENT_FLAGS_RELEASE_CID,
				  5, NULL,
				  (GAsyncReadyCallback)call_stop_release_done,
				  op);
}

void nm_call_stop(NmCall *call, NmCallDoneFunc done, gpointer user_data)
{
	CallOp *op;

	call->done = done;
	call->user_data = user_data;

	if (!call->active || !call->client || !call->device) {
		call->active = FALSE;
		op = call_op_new(call);
		call_report(op, NULL);
		return;
	}

	op = call_op_new(call);

	if (call->packet_data_handle) {
		g_autoptr(QmiMessageWdsStopNetworkInput) input = NULL;
		g_autoptr(GError) error = NULL;

		input = qmi_message_wds_stop_network_input_new();
		if (qmi_message_wds_stop_network_input_set_packet_data_handle(input,
				call->packet_data_handle, &error)) {
			qmi_client_wds_stop_network(QMI_CLIENT_WDS(call->client),
						    input, 10, NULL,
						    (GAsyncReadyCallback)call_stop_network_done,
						    op);
			return;
		}
		nm_debug("could not build WDS stop request: %s", error->message);
	}

	qmi_device_release_client(call->device, call->client,
				  QMI_DEVICE_RELEASE_CLIENT_FLAGS_RELEASE_CID,
				  5, NULL,
				  (GAsyncReadyCallback)call_stop_release_done,
				  op);
}
