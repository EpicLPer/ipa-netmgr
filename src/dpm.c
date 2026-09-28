/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * DPM session: the modem only publishes its DATA5_CNTL control channel
 * (and with it the main QMI services) while a Data Port Mapper client holds
 * the port open, and the data endpoint mapping lives there too.  The DPM
 * service is only reachable over QRTR on this firmware.
 *
 * The QRTR bus lives for the whole process: libqrtr keeps main-loop watches on
 * it, and freeing the bus while those are pending crashes.  Each modem uptime
 * gets a new session, tracked with a generation counter so callbacks from a
 * session that ended in a modem restart cannot touch the next one.
 */
#include "dpm.h"
#include "log.h"

#include <libqrtr-glib.h>

struct _NmDpm {
	const NmConfig *config;
	QrtrBus *bus;
	guint generation;
	gboolean session_open;
	QrtrNode *node;
	QmiDevice *device;
	QmiClient *client;
	NmDpmDoneFunc done;
	gpointer user_data;
};

typedef struct {
	NmDpm *dpm;
	guint generation;
} DpmOp;

NmDpm *nm_dpm_new(const NmConfig *config)
{
	NmDpm *dpm = g_new0(NmDpm, 1);

	dpm->config = config;

	return dpm;
}

gboolean nm_dpm_is_open(NmDpm *dpm)
{
	return dpm->session_open;
}

static DpmOp *dpm_op_new(NmDpm *dpm)
{
	DpmOp *op = g_new0(DpmOp, 1);

	op->dpm = dpm;
	op->generation = dpm->generation;

	return op;
}

static gboolean dpm_op_stale(DpmOp *op)
{
	return op->dpm->generation != op->generation;
}

/* Terminal step of a chain: reports the result (unless the operation belongs
 * to an aborted session) and frees the operation. */
static void dpm_report(DpmOp *op, GError *error)
{
	NmDpm *dpm = op->dpm;

	if (!dpm_op_stale(op)) {
		NmDpmDoneFunc done = dpm->done;
		gpointer user_data = dpm->user_data;

		dpm->done = NULL;
		dpm->user_data = NULL;

		if (done)
			done(error, user_data);
	}

	g_free(op);
}

static void dpm_open_port_ready(QmiClientDpm *client, GAsyncResult *res,
				gpointer user_data)
{
	DpmOp *op = user_data;
	NmDpm *dpm = op->dpm;
	g_autoptr(QmiMessageDpmOpenPortOutput) output = NULL;
	g_autoptr(GError) error = NULL;

	if (dpm_op_stale(op)) {
		dpm_report(op, NULL);
		return;
	}

	output = qmi_client_dpm_open_port_finish(client, res, &error);
	if (!output) {
		nm_error("DPM open port failed: %s", error->message);
		dpm_report(op, error);
		return;
	}
	if (!qmi_message_dpm_open_port_output_get_result(output, &error)) {
		nm_error("DPM open port rejected: %s", error->message);
		dpm_report(op, error);
		return;
	}

	dpm->session_open = TRUE;
	nm_info("DPM ports open (control %s ep %u/%u, data ep %u/%u rx %u tx %u)",
		dpm->config->control_port, dpm->config->control_ep_type,
		dpm->config->control_iface, dpm->config->hw_ep_type,
		dpm->config->hw_iface, dpm->config->hw_rx, dpm->config->hw_tx);

	dpm_report(op, NULL);
}

static void dpm_open_port(DpmOp *op)
{
	NmDpm *dpm = op->dpm;
	g_autoptr(QmiMessageDpmOpenPortInput) input = NULL;
	g_autoptr(GArray) control_ports = NULL;
	g_autoptr(GArray) hw_ports = NULL;
	QmiMessageDpmOpenPortInputControlPortsElement control_port = { 0 };
	QmiMessageDpmOpenPortInputHardwareDataPortsElement hw_port = { 0 };
	g_autoptr(GError) error = NULL;

	control_ports = g_array_new(FALSE, FALSE,
			sizeof(QmiMessageDpmOpenPortInputControlPortsElement));
	control_port.port_name = g_strdup(dpm->config->control_port);
	control_port.endpoint_type = dpm->config->control_ep_type;
	control_port.interface_number = dpm->config->control_iface;
	g_array_append_val(control_ports, control_port);

	hw_ports = g_array_new(FALSE, FALSE,
			sizeof(QmiMessageDpmOpenPortInputHardwareDataPortsElement));
	hw_port.endpoint_type = dpm->config->hw_ep_type;
	hw_port.interface_number = dpm->config->hw_iface;
	hw_port.rx_endpoint_number = dpm->config->hw_rx;
	hw_port.tx_endpoint_number = dpm->config->hw_tx;
	g_array_append_val(hw_ports, hw_port);

	input = qmi_message_dpm_open_port_input_new();
	if (!qmi_message_dpm_open_port_input_set_control_ports(input,
			control_ports, &error) ||
	    !qmi_message_dpm_open_port_input_set_hardware_data_ports(input,
			hw_ports, &error)) {
		nm_error("could not build DPM open request: %s", error->message);
		dpm_report(op, error);
		return;
	}

	qmi_client_dpm_open_port(QMI_CLIENT_DPM(dpm->client), input, 30, NULL,
				 (GAsyncReadyCallback)dpm_open_port_ready, op);
}

static void dpm_client_ready(QmiDevice *device, GAsyncResult *res,
			     gpointer user_data)
{
	DpmOp *op = user_data;
	NmDpm *dpm = op->dpm;
	g_autoptr(GError) error = NULL;

	if (dpm_op_stale(op)) {
		dpm_report(op, NULL);
		return;
	}

	dpm->client = qmi_device_allocate_client_finish(device, res, &error);
	if (!dpm->client) {
		nm_error("could not allocate DPM client: %s", error->message);
		dpm_report(op, error);
		return;
	}

	nm_debug("DPM client allocated");
	dpm_open_port(op);
}

static void dpm_device_ready(QmiDevice *device, GAsyncResult *res,
			     gpointer user_data)
{
	DpmOp *op = user_data;
	NmDpm *dpm = op->dpm;
	g_autoptr(GError) error = NULL;

	if (dpm_op_stale(op)) {
		dpm_report(op, NULL);
		return;
	}

	if (!qmi_device_open_finish(device, res, &error)) {
		nm_error("could not open QRTR QMI device: %s", error->message);
		dpm_report(op, error);
		return;
	}

	nm_debug("QRTR QMI device open (node %u)", dpm->config->qrtr_node);
	qmi_device_allocate_client(dpm->device, QMI_SERVICE_DPM, QMI_CID_NONE,
				   10, NULL,
				   (GAsyncReadyCallback)dpm_client_ready, op);
}

static void dpm_device_new_ready(GObject *source, GAsyncResult *res,
				 gpointer user_data)
{
	DpmOp *op = user_data;
	NmDpm *dpm = op->dpm;
	g_autoptr(GError) error = NULL;

	if (dpm_op_stale(op)) {
		dpm_report(op, NULL);
		return;
	}

	dpm->device = qmi_device_new_from_node_finish(res, &error);
	if (!dpm->device) {
		nm_error("could not create QMI device for QRTR node: %s",
			 error->message);
		dpm_report(op, error);
		return;
	}

	qmi_device_open(dpm->device, QMI_DEVICE_OPEN_FLAGS_NONE, 10, NULL,
			(GAsyncReadyCallback)dpm_device_ready, op);
}

static void dpm_node_wait_ready(GObject *source, GAsyncResult *res,
				gpointer user_data)
{
	DpmOp *op = user_data;
	NmDpm *dpm = op->dpm;
	g_autoptr(GError) error = NULL;

	if (dpm_op_stale(op)) {
		dpm_report(op, NULL);
		return;
	}

	dpm->node = qrtr_bus_wait_for_node_finish(dpm->bus, res, &error);
	if (!dpm->node) {
		nm_error("QRTR node %u did not appear: %s",
			 dpm->config->qrtr_node, error->message);
		dpm_report(op, error);
		return;
	}

	qmi_device_new_from_node(dpm->node, NULL,
				 (GAsyncReadyCallback)dpm_device_new_ready, op);
}

static void dpm_bus_ready(GObject *source, GAsyncResult *res,
			  gpointer user_data)
{
	DpmOp *op = user_data;
	NmDpm *dpm = op->dpm;
	g_autoptr(GError) error = NULL;
	QrtrNode *node;

	if (dpm_op_stale(op)) {
		dpm_report(op, NULL);
		return;
	}

	if (!dpm->bus) {
		dpm->bus = qrtr_bus_new_finish(res, &error);
		if (!dpm->bus) {
			nm_error("could not open QRTR bus: %s", error->message);
			dpm_report(op, error);
			return;
		}
	}

	node = qrtr_bus_peek_node(dpm->bus, dpm->config->qrtr_node);
	if (node && QRTR_IS_NODE(node)) {
		dpm->node = g_object_ref(node);
		qmi_device_new_from_node(node, NULL,
					 (GAsyncReadyCallback)dpm_device_new_ready,
					 op);
		return;
	}

	nm_debug("waiting for QRTR node %u", dpm->config->qrtr_node);
	qrtr_bus_wait_for_node(dpm->bus, dpm->config->qrtr_node, 5000, NULL,
			       (GAsyncReadyCallback)dpm_node_wait_ready, op);
}

void nm_dpm_start(NmDpm *dpm, NmDpmDoneFunc done, gpointer user_data)
{
	DpmOp *op;

	/* A new session replaces whatever is left from the previous modem uptime */
	dpm->generation++;
	g_clear_object(&dpm->client);
	g_clear_object(&dpm->device);
	g_clear_object(&dpm->node);
	dpm->session_open = FALSE;

	dpm->done = done;
	dpm->user_data = user_data;

	op = dpm_op_new(dpm);
	if (!dpm->bus) {
		qrtr_bus_new(1000, NULL, (GAsyncReadyCallback)dpm_bus_ready, op);
		return;
	}

	dpm_bus_ready(G_OBJECT(dpm->bus), NULL, op);
}

/* ------------------------------------------------------------------ */
/* Teardown                                                            */

static void dpm_release_client_ready(QmiDevice *device, GAsyncResult *res,
				     gpointer user_data)
{
	DpmOp *op = user_data;
	NmDpm *dpm = op->dpm;
	g_autoptr(GError) error = NULL;

	if (!qmi_device_release_client_finish(device, res, &error))
		nm_warn("could not release DPM client: %s", error->message);
	else
		nm_debug("DPM client released");

	g_clear_object(&dpm->client);
	dpm_report(op, NULL);
}

static void dpm_close_port_ready(QmiClientDpm *client, GAsyncResult *res,
				 gpointer user_data)
{
	DpmOp *op = user_data;
	NmDpm *dpm = op->dpm;
	g_autoptr(QmiMessageDpmClosePortOutput) output = NULL;
	g_autoptr(GError) error = NULL;

	if (dpm_op_stale(op)) {
		dpm_report(op, NULL);
		return;
	}

	output = qmi_client_dpm_close_port_finish(client, res, &error);
	if (!output || !qmi_message_dpm_close_port_output_get_result(output, &error))
		nm_warn("DPM close port failed: %s",
			error ? error->message : "unknown error");
	else
		nm_debug("DPM ports closed");

	dpm->session_open = FALSE;
	qmi_device_release_client(dpm->device, dpm->client,
				  QMI_DEVICE_RELEASE_CLIENT_FLAGS_RELEASE_CID,
				  5, NULL,
				  (GAsyncReadyCallback)dpm_release_client_ready,
				  op);
}

void nm_dpm_stop(NmDpm *dpm, NmDpmDoneFunc done, gpointer user_data)
{
	DpmOp *op;

	dpm->done = done;
	dpm->user_data = user_data;

	if (dpm->session_open && dpm->client && dpm->device) {
		op = dpm_op_new(dpm);
		/* Close Port carries no input; the second argument is unused */
		qmi_client_dpm_close_port(QMI_CLIENT_DPM(dpm->client), NULL, 5,
					  NULL,
					  (GAsyncReadyCallback)dpm_close_port_ready,
					  op);
		return;
	}

	dpm->session_open = FALSE;
	if (dpm->done) {
		NmDpmDoneFunc report = dpm->done;
		gpointer data = dpm->user_data;

		dpm->done = NULL;
		dpm->user_data = NULL;
		report(NULL, data);
	}
}

/*
 * Abandon the current session (modem restart).  The QRTR bus is kept: libqrtr
 * has main-loop watches on it and freeing it while pending crashes.
 */
void nm_dpm_reset(NmDpm *dpm)
{
	if (!dpm)
		return;

	dpm->generation++;
	g_clear_object(&dpm->client);
	g_clear_object(&dpm->device);
	g_clear_object(&dpm->node);
	dpm->session_open = FALSE;
	dpm->done = NULL;
	dpm->user_data = NULL;
}

void nm_dpm_free(NmDpm *dpm)
{
	if (!dpm)
		return;
	g_clear_object(&dpm->client);
	g_clear_object(&dpm->device);
	g_clear_object(&dpm->node);
	g_clear_object(&dpm->bus);
	g_free(dpm);
}
