/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Modem lifecycle and firmware quirks.
 *
 * The modem firmware (MPSS.BO.2.5) needs several steps that are not automatic
 * before it serves telephony and data:
 *
 *   1. start the remoteproc (the Q6 auth handshake can fail and is retried)
 *   2. open the DPM ports (the modem publishes DATA5_CNTL only on request)
 *   3. set the WDA data format (raw-ip + QMAP for the mainline rmnet driver)
 *   4. select the card application and publish the subscription
 *      (halt_subscription=1 in the firmware configuration)
 *   5. enable the dual-standby subscription (NAS 0x4B) and put DMS online
 *   6. radio cycle, network selection and the SMS bearer (AT): the CFUN cycle
 *      lets the SM task read EF_SMSP; PS SMS submission does not work here
 *
 * Every step is idempotent, so the whole sequence re-runs whenever the modem
 * restarts.  This module replaces the former bringup.sh / recover.sh helpers:
 * it starts the modem itself, watches it, and re-runs everything on loss.
 */
#include "modem.h"
#include "log.h"
#include "raw.h"
#include "wda.h"
#include "dpm.h"
#include "at.h"

#include <errno.h>
#include <fcntl.h>
#include <gio/gio.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>

#define POLL_INTERVAL_MS         1000
#define PORT_WAIT_TIMEOUT_MS     20000
#define PORT_POLL_MS             250
#define CARD_WAIT_TIMEOUT_MS     20000
#define CARD_POLL_MS             1000
#define CFUN_OFF_SETTLE_MS       1500
#define CFUN_ON_SETTLE_MS        2000
#define REGISTRATION_POLL_MS     1000

typedef enum {
	NM_MODEM_WAIT = 0,   /* remoteproc not running: start it */
	NM_MODEM_BRINGUP,    /* running: quirks in progress */
	NM_MODEM_READY,      /* registered and serving */
} NmModemState;

struct _NmModem {
	const NmConfig *config;

	NmModemReadyFunc ready_fn;
	gpointer ready_data;
	NmModemLostFunc lost_fn;
	gpointer lost_data;
	NmModemStoppedFunc stopped_fn;
	gpointer stopped_data;

	NmModemState state;
	gboolean shutting_down;
	guint generation;
	guint poll_id;
	gchar *modem_state;   /* resolved rproc state file */

	/* remoteproc start attempts */
	guint start_attempts;
	gint64 next_start_us;

	/* bring-up round */
	gboolean bringup_active;
	guint retry_count;
	gint64 next_retry_us;
	guint wait_id;
	gint64 wait_deadline;
	guint probe_index;
	gchar **probe_order;

	/* QMI control channel */
	NmDpm *dpm;
	QmiDevice *device;
	QmiClient *uim;
	QmiClient *nas;
	QmiClient *dms;
	gchar *control;
	NmAt *at;

	/* card / radio polling */
	guint card_id;
	gboolean card_session_sent;
	gint64 card_deadline;
	guint radio_id;
	guint at_step;
	gint64 at_deadline;
};

/* ------------------------------------------------------------------ helpers */

static void device_removed(QmiDevice *device, gpointer user_data);
static void bringup_card_begin(NmModem *modem);
static void card_done(QmiClientUim *client, GAsyncResult *res,
		      gpointer user_data);
static void bringup_dpm_done(GError *error, gpointer user_data);
static void modem_ready(NmModem *modem);
static void bringup_rat_preference(NmModem *modem);
static gboolean card_publish_cb(gpointer user_data);

static gboolean modem_is_running(NmModem *modem)
{
	g_autofree gchar *contents = NULL;

	if (!g_file_get_contents(modem->modem_state, &contents, NULL, NULL))
		return FALSE;

	return g_str_has_prefix(contents, "running");
}

static gboolean modem_write_state(const gchar *path, const gchar *value,
				  GError **error)
{
	gint fd;
	gsize length = strlen(value);
	gsize written = 0;
	ssize_t n;

	fd = open(path, O_WRONLY | O_CLOEXEC);
	if (fd < 0) {
		g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno),
			    "could not open %s: %s", path, g_strerror(errno));
		return FALSE;
	}

	while (written < length) {
		n = write(fd, value + written, length - written);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno),
				    "could not write to %s: %s", path,
				    g_strerror(errno));
			close(fd);
			return FALSE;
		}
		written += n;
	}

	close(fd);
	return TRUE;
}

static void remove_source(guint *id)
{
	if (*id) {
		g_source_remove(*id);
		*id = 0;
	}
}

static void bringup_cleanup_clients(NmModem *modem)
{
	if (modem->device) {
		if (modem->uim) {
			qmi_device_release_client(modem->device, modem->uim,
						  QMI_DEVICE_RELEASE_CLIENT_FLAGS_RELEASE_CID,
						  5, NULL, NULL, NULL);
			g_clear_object(&modem->uim);
		}
		if (modem->nas) {
			qmi_device_release_client(modem->device, modem->nas,
						  QMI_DEVICE_RELEASE_CLIENT_FLAGS_RELEASE_CID,
						  5, NULL, NULL, NULL);
			g_clear_object(&modem->nas);
		}
		if (modem->dms) {
			qmi_device_release_client(modem->device, modem->dms,
						  QMI_DEVICE_RELEASE_CLIENT_FLAGS_RELEASE_CID,
						  5, NULL, NULL, NULL);
			g_clear_object(&modem->dms);
		}
		g_clear_object(&modem->device);
	}
}

static void bringup_fail(NmModem *modem, guint delay_ms, const gchar *format,
			 ...)
{
	g_autofree gchar *message = NULL;
	va_list args;

	va_start(args, format);
	message = g_strdup_vprintf(format, args);
	va_end(args);

	nm_warn("modem bring-up: %s (retrying in %u ms)", message, delay_ms);

	remove_source(&modem->wait_id);
	remove_source(&modem->card_id);
	remove_source(&modem->radio_id);
	bringup_cleanup_clients(modem);
	modem->generation++;
	modem->bringup_active = FALSE;
	modem->card_session_sent = FALSE;

	/* retry_min_ms/retry_max_ms bound the per-step delays */
	if (delay_ms < modem->config->retry_min_ms)
		delay_ms = modem->config->retry_min_ms;
	if (delay_ms > modem->config->retry_max_ms)
		delay_ms = modem->config->retry_max_ms;
	modem->next_retry_us = g_get_monotonic_time() + ((gint64)delay_ms * 1000);
}

static gchar *bytes_to_hex(const guint8 *bytes, gsize length)
{
	GString *string = g_string_sized_new(length * 2);

	for (gsize i = 0; i < length; i++)
		g_string_append_printf(string, "%02X", bytes[i]);

	return g_string_free(string, FALSE);
}

/* ------------------------------------------------------------------- probing */

typedef struct {
	NmModem *modem;
	guint generation;
	gchar *path;
	QmiDevice *device;
	QmiClient *client;
} Probe;

static void probe_free(Probe *probe)
{
	g_clear_object(&probe->client);
	g_clear_object(&probe->device);
	g_free(probe->path);
	g_free(probe);
}

static void bringup_probe_next(NmModem *modem);
static void bringup_wda(NmModem *modem);

static void probe_failed(Probe *probe)
{
	NmModem *modem = probe->modem;

	probe_free(probe);
	modem->probe_index++;
	bringup_probe_next(modem);
}

static void probe_client_ready(QmiDevice *device, GAsyncResult *res,
			       gpointer user_data)
{
	Probe *probe = user_data;
	NmModem *modem = probe->modem;
	g_autoptr(GError) error = NULL;

	probe->client = qmi_device_allocate_client_finish(device, res, &error);
	if (!probe->client) {
		nm_debug("%s does not serve UIM: %s", probe->path, error->message);
		probe_failed(probe);
		return;
	}

	if (!modem->bringup_active || modem->shutting_down ||
	    probe->generation != modem->generation || modem->device) {
		probe_free(probe);
		return;
	}

	modem->device = g_steal_pointer(&probe->device);
	modem->uim = g_steal_pointer(&probe->client);
	modem->control = g_strdup(probe->path);
	probe_free(probe);

	nm_info("control channel: %s", modem->control);
	/* In ModemManager mode the daemon releases the channel after bring-up
	 * and must not react to proxy/transport blips: the rproc poll detects
	 * real modem restarts (a re-run of the quirks would reset the SIM
	 * session under MM). */
	if (modem->config->call_enabled)
		g_signal_connect(modem->device, QMI_DEVICE_SIGNAL_REMOVED,
				 G_CALLBACK(device_removed), modem);

	/* ModemManager sets the data format itself when it owns the data path */
	if (modem->config->call_enabled)
		bringup_wda(modem);
	else
		bringup_card_begin(modem);
}

static void probe_open_ready(QmiDevice *device, GAsyncResult *res,
			     gpointer user_data)
{
	Probe *probe = user_data;
	g_autoptr(GError) error = NULL;

	if (!qmi_device_open_finish(device, res, &error)) {
		nm_debug("%s could not be opened: %s", probe->path,
			 error->message);
		probe_failed(probe);
		return;
	}

	qmi_device_allocate_client(probe->device, QMI_SERVICE_UIM, QMI_CID_NONE,
				   10, NULL,
				   (GAsyncReadyCallback)probe_client_ready,
				   probe);
}

static void probe_device_new_ready(GObject *source, GAsyncResult *res,
				   gpointer user_data)
{
	Probe *probe = user_data;
	g_autoptr(GError) error = NULL;

	probe->device = qmi_device_new_finish(res, &error);
	if (!probe->device) {
		nm_debug("%s is not usable: %s", probe->path, error->message);
		probe_failed(probe);
		return;
	}

	/* Through qmi-proxy: several processes (ModemManager, tools) may then
	 * share this rpmsg channel; a second direct opener would steal
	 * responses from the shared WWAN rx queue. */
	qmi_device_open(probe->device,
			QMI_DEVICE_OPEN_FLAGS_PROXY | QMI_DEVICE_OPEN_FLAGS_VERSION_INFO,
			10, NULL,
			(GAsyncReadyCallback)probe_open_ready, probe);
}

static void bringup_probe_next(NmModem *modem)
{
	const gchar *path;
	Probe *probe;

	if (!modem->bringup_active || modem->shutting_down)
		return;

	path = modem->probe_order ? modem->probe_order[modem->probe_index] : NULL;
	if (!path) {
		bringup_fail(modem, 2000, "no QMI channel serves UIM");
		return;
	}

	probe = g_new0(Probe, 1);
	probe->modem = modem;
	probe->generation = modem->generation;
	probe->path = g_strdup(path);

	{
		g_autoptr(GFile) file = g_file_new_for_path(path);

		qmi_device_new(file, NULL,
			       (GAsyncReadyCallback)probe_device_new_ready, probe);
	}
}

static void bringup_probe_begin(NmModem *modem)
{
	GPtrArray *order = g_ptr_array_new();

	/* Last known control channel first (the services can re-home) */
	if (modem->control && g_file_test(modem->control, G_FILE_TEST_EXISTS))
		g_ptr_array_add(order, g_strdup(modem->control));

	for (guint i = 0; modem->config->qmi_devices[i]; i++) {
		if (!g_file_test(modem->config->qmi_devices[i], G_FILE_TEST_EXISTS))
			continue;
		if (modem->control &&
		    !g_strcmp0(modem->config->qmi_devices[i], modem->control))
			continue;
		g_ptr_array_add(order, g_strdup(modem->config->qmi_devices[i]));
	}
	g_ptr_array_add(order, NULL);

	g_strfreev(modem->probe_order);
	modem->probe_order = (gchar **)g_ptr_array_free(order, FALSE);
	modem->probe_index = 0;

	bringup_probe_next(modem);
}

/* ------------------------------------------------------------- bring-up flow */

static void bringup_probe_begin(NmModem *modem);
static gboolean ports_poll(gpointer user_data);

static gboolean ports_present(NmModem *modem)
{
	if (!g_file_test(modem->config->at_device, G_FILE_TEST_EXISTS))
		return FALSE;

	for (guint i = 0; modem->config->qmi_devices[i]; i++)
		if (g_file_test(modem->config->qmi_devices[i], G_FILE_TEST_EXISTS))
			return TRUE;

	return FALSE;
}

static void bringup_wait_ports(NmModem *modem)
{
	modem->wait_deadline = g_get_monotonic_time() +
			       ((gint64)PORT_WAIT_TIMEOUT_MS * 1000);
	modem->wait_id = g_timeout_add(PORT_POLL_MS, ports_poll, modem);
}

static gboolean ports_poll(gpointer user_data)
{
	NmModem *modem = user_data;

	modem->wait_id = 0;
	if (!modem->bringup_active || modem->shutting_down)
		return G_SOURCE_REMOVE;

	if (ports_present(modem)) {
		bringup_probe_begin(modem);
		return G_SOURCE_REMOVE;
	}

	if (g_get_monotonic_time() >= modem->wait_deadline) {
		bringup_fail(modem, 2000, "QMI/AT ports did not appear");
		return G_SOURCE_REMOVE;
	}

	modem->wait_id = g_timeout_add(PORT_POLL_MS, ports_poll, modem);
	return G_SOURCE_REMOVE;
}

static void bringup_start(NmModem *modem)
{
	if (modem->bringup_active || modem->shutting_down)
		return;

	modem->bringup_active = TRUE;
	modem->retry_count++;
	nm_info("modem bring-up started (round %u)", modem->retry_count);

	if (!modem->dpm)
		modem->dpm = nm_dpm_new(modem->config);

	if (nm_dpm_is_open(modem->dpm)) {
		bringup_wait_ports(modem);
		return;
	}

	nm_dpm_start(modem->dpm, bringup_dpm_done, modem);
}

static void bringup_dpm_done(GError *error, gpointer user_data)
{
	NmModem *modem = user_data;

	if (!modem->bringup_active || modem->shutting_down)
		return;

	if (error) {
		bringup_fail(modem, 3000, "DPM session failed: %s", error->message);
		return;
	}

	bringup_wait_ports(modem);
}

static void bringup_wda_done(GError *error, gpointer user_data);

static void bringup_wda(NmModem *modem)
{
	nm_wda_apply(modem->device, modem->config, bringup_wda_done, modem);
}

static void bringup_wda_done(GError *error, gpointer user_data)
{
	NmModem *modem = user_data;

	if (!modem->bringup_active || modem->shutting_down)
		return;

	/* The data format persists on the modem; when ModemManager already holds
	 * the WDA service (daemon restart while MM runs) the allocation fails
	 * with 'Internal'.  Continue: a call that needs the format will fail and
	 * the bring-up round is retried then. */
	if (error)
		nm_warn("WDA data format: %s (continuing)", error->message);
	else
		nm_debug("WDA data format applied");

	bringup_card_begin(modem);
}

/* ----------------------------------------------------------------- SIM card */

static void bringup_card_begin(NmModem *modem);
static void bringup_subscription_ok(NmModem *modem);
static void card_request(NmModem *modem);

static gboolean card_poll_cb(gpointer user_data)
{
	NmModem *modem = user_data;

	modem->card_id = 0;
	if (modem->bringup_active && !modem->shutting_down)
		card_request(modem);

	return G_SOURCE_REMOVE;
}

static void card_schedule(NmModem *modem, guint delay_ms)
{
	remove_source(&modem->card_id);
	modem->card_id = g_timeout_add(delay_ms, card_poll_cb, modem);
}

static void card_request(NmModem *modem)
{
	modem->card_id = 0;
	if (!modem->bringup_active || modem->shutting_down)
		return;

	/* Get Card Status has an empty input; libqmi takes NULL for it. */
	qmi_client_uim_get_card_status(QMI_CLIENT_UIM(modem->uim), NULL, 10,
				       NULL, (GAsyncReadyCallback)card_done,
				       modem);
}

static QmiMessageUimGetCardStatusOutputCardStatusCardsElementApplicationsElementV2 *
card_find_application(QmiMessageUimGetCardStatusOutput *output,
		      gboolean *card_present)
{
	QmiMessageUimGetCardStatusOutputCardStatusCardsElementApplicationsElementV2 *fallback = NULL;
	GArray *cards = NULL;
	guint16 index_1, index_2, index_3, index_4;

	*card_present = FALSE;

	if (!qmi_message_uim_get_card_status_output_get_card_status(output,
			&index_1, &index_2, &index_3, &index_4, &cards, NULL) ||
	    !cards)
		return NULL;

	for (guint c = 0; c < cards->len; c++) {
		QmiMessageUimGetCardStatusOutputCardStatusCardsElement *card;

		card = &g_array_index(cards,
			QmiMessageUimGetCardStatusOutputCardStatusCardsElement, c);
		if (card->card_state != QMI_UIM_CARD_STATE_PRESENT ||
		    !card->applications)
			continue;

		*card_present = TRUE;
		fallback = NULL;
		for (guint a = 0; a < card->applications->len; a++) {
			QmiMessageUimGetCardStatusOutputCardStatusCardsElementApplicationsElementV2 *app;

			app = &g_array_index(card->applications,
				QmiMessageUimGetCardStatusOutputCardStatusCardsElementApplicationsElementV2, a);
			if (app->type == QMI_UIM_CARD_APPLICATION_TYPE_USIM)
				return app;
			if (!fallback)
				fallback = app;
		}
		if (fallback)
			return fallback;
	}

	return NULL;
}

static void select_application_done(QmiClientUim *client, GAsyncResult *res,
				    gpointer user_data)
{
	NmModem *modem = user_data;
	g_autoptr(QmiMessageUimChangeProvisioningSessionOutput) output = NULL;
	g_autoptr(GError) error = NULL;

	if (!modem->bringup_active || modem->shutting_down)
		return;

	output = qmi_client_uim_change_provisioning_session_finish(client, res,
								   &error);
	if (!output ||
	    !qmi_message_uim_change_provisioning_session_output_get_result(output,
									   &error)) {
		bringup_fail(modem, 2000, "could not select the SIM application: %s",
			     error->message);
		return;
	}

	nm_info("SIM application selected");
	modem->card_session_sent = TRUE;

	/* Give the new provisioning session a moment to settle before
	 * publishing the subscription: sending SUBSCRIPTION_OK immediately
	 * after the session change makes the modem acknowledge it (result 0)
	 * but leaves the application in check-personalization-state
	 * (measured), so the publication is delayed slightly. */
	modem->card_id = g_timeout_add(1500, (GSourceFunc)card_publish_cb, modem);
}

static gboolean card_publish_cb(gpointer user_data)
{
	NmModem *modem = user_data;

	modem->card_id = 0;
	if (!modem->bringup_active || modem->shutting_down)
		return G_SOURCE_REMOVE;

	bringup_subscription_ok(modem);
	return G_SOURCE_REMOVE;
}

static void select_application(NmModem *modem, const guint8 *aid, gsize length)
{
	g_autoptr(QmiMessageUimChangeProvisioningSessionInput) input = NULL;
	g_autoptr(GArray) aid_array = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *aid_hex = NULL;

	aid_hex = bytes_to_hex(aid, length);
	nm_info("selecting the SIM application (AID %s)", aid_hex);

	aid_array = g_array_sized_new(FALSE, FALSE, sizeof(guint8), length);
	g_array_append_vals(aid_array, aid, length);

	input = qmi_message_uim_change_provisioning_session_input_new();
	if (!qmi_message_uim_change_provisioning_session_input_set_application_information(
			input, 1, aid_array, &error) ||
	    !qmi_message_uim_change_provisioning_session_input_set_session_change(
			input, QMI_UIM_SESSION_TYPE_PRIMARY_GW_PROVISIONING, TRUE,
			&error)) {
		bringup_fail(modem, 2000, "could not build the provisioning request: %s",
			     error->message);
		return;
	}

	qmi_client_uim_change_provisioning_session(QMI_CLIENT_UIM(modem->uim),
						   input, 10, NULL,
						   (GAsyncReadyCallback)select_application_done,
						   modem);
}

static void card_done(QmiClientUim *client, GAsyncResult *res,
		      gpointer user_data)
{
	NmModem *modem = user_data;
	g_autoptr(QmiMessageUimGetCardStatusOutput) output = NULL;
	g_autoptr(GError) error = NULL;
	QmiMessageUimGetCardStatusOutputCardStatusCardsElementApplicationsElementV2 *app;
	gboolean card_present = FALSE;

	if (!modem->bringup_active || modem->shutting_down)
		return;

	output = qmi_client_uim_get_card_status_finish(client, res, &error);
	if (!output ||
	    !qmi_message_uim_get_card_status_output_get_result(output, &error)) {
		if (g_get_monotonic_time() < modem->card_deadline)
			card_schedule(modem, CARD_POLL_MS);
		else
			bringup_fail(modem, 2000, "SIM card status failed: %s",
				     error->message);
		return;
	}

	app = card_find_application(output, &card_present);
	if (!app) {
		if (g_get_monotonic_time() < modem->card_deadline) {
			card_schedule(modem, CARD_POLL_MS);
			return;
		}
		bringup_fail(modem, 2000, card_present
			     ? "SIM application did not appear"
			     : "SIM card did not appear");
		return;
	}

	if (app->state == QMI_UIM_CARD_APPLICATION_STATE_READY) {
		nm_info("SIM application ready");
		bringup_subscription_ok(modem);
		return;
	}

	if (!modem->card_session_sent &&
	    app->application_identifier_value &&
	    app->application_identifier_value->len) {
		select_application(modem,
				   (const guint8 *)app->application_identifier_value->data,
				   app->application_identifier_value->len);
		return;
	}

	/* No session to select (or already selected): publish the subscription,
	 * which is what makes the application leave the personalization check. */
	bringup_subscription_ok(modem);
}

static void bringup_card_begin(NmModem *modem)
{
	modem->card_deadline = g_get_monotonic_time() +
			       ((gint64)CARD_WAIT_TIMEOUT_MS * 1000);
	modem->card_session_sent = FALSE;
	card_request(modem);
}

/* --------------------------------------------------------- subscription/NAS */

static void bringup_clients(NmModem *modem);
static void bringup_nas_dual_standby(NmModem *modem);
static void bringup_dms_online(NmModem *modem);
static void bringup_radio(NmModem *modem);

static void subscription_done(QmiMessage *response, GError *error,
			      gpointer user_data)
{
	NmModem *modem = user_data;
	g_autoptr(GError) local_error = NULL;
	guint16 qmi_error = 0;

	if (!modem->bringup_active || modem->shutting_down)
		return;

	if (!response ||
	    !nm_qmi_raw_get_result(response, &qmi_error, &local_error)) {
		if (qmi_error == 3) {
			nm_debug("subscription was already published");
		} else {
			bringup_fail(modem, 1000, "SUBSCRIPTION_OK failed: %s",
				     local_error ? local_error->message :
				     (error ? error->message : "no response"));
			return;
		}
	} else {
		nm_info("SIM subscription published");
	}

	bringup_clients(modem);
}

static void bringup_subscription_ok(NmModem *modem)
{
	static const guint8 session_information[2] = { 0x00, 0x00 };
	static const guint8 ok_for_subscription = 0x01;
	NmQmiRawTlv tlvs[] = {
		{ 0x01, session_information, sizeof(session_information) },
		{ 0x02, &ok_for_subscription, 1 },
	};

	/* UIM SUBSCRIPTION_OK (0x0040): primary GW session, no AID, ok */
	nm_qmi_raw_call(modem->device, QMI_SERVICE_UIM,
			qmi_client_get_cid(modem->uim), 0x0040, tlvs,
			G_N_ELEMENTS(tlvs), 6, subscription_done, modem);
}

static void dms_client_done(QmiDevice *device, GAsyncResult *res,
			    gpointer user_data)
{
	NmModem *modem = user_data;
	g_autoptr(GError) error = NULL;

	if (!modem->bringup_active || modem->shutting_down)
		return;

	modem->dms = qmi_device_allocate_client_finish(device, res, &error);
	if (!modem->dms) {
		bringup_fail(modem, 2000, "could not allocate DMS client: %s",
			     error->message);
		return;
	}

	bringup_nas_dual_standby(modem);
}

static void nas_client_done(QmiDevice *device, GAsyncResult *res,
			    gpointer user_data)
{
	NmModem *modem = user_data;
	g_autoptr(GError) error = NULL;

	if (!modem->bringup_active || modem->shutting_down)
		return;

	modem->nas = qmi_device_allocate_client_finish(device, res, &error);
	if (!modem->nas) {
		bringup_fail(modem, 2000, "could not allocate NAS client: %s",
			     error->message);
		return;
	}

	if (!modem->config->call_enabled) {
		bringup_nas_dual_standby(modem);
		return;
	}

	qmi_device_allocate_client(modem->device, QMI_SERVICE_DMS, QMI_CID_NONE,
				   10, NULL,
				   (GAsyncReadyCallback)dms_client_done, modem);
}

static void bringup_clients(NmModem *modem)
{
	qmi_device_allocate_client(modem->device, QMI_SERVICE_NAS, QMI_CID_NONE,
				   10, NULL,
				   (GAsyncReadyCallback)nas_client_done, modem);
}

static void nas_dual_done(QmiMessage *response, GError *error,
			  gpointer user_data)
{
	NmModem *modem = user_data;
	g_autoptr(GError) local_error = NULL;

	if (!modem->bringup_active || modem->shutting_down)
		return;

	if (!response || !nm_qmi_raw_get_result(response, NULL, &local_error))
		nm_warn("dual-standby preference failed: %s",
			local_error ? local_error->message :
			(error ? error->message : "no response"));
	else
		nm_debug("dual-standby preference set");

	bringup_rat_preference(modem);
}

/*
 * The modem's stored mode preference can be left at a single RAT (measured:
 * 'gsm, umts' after some experiments, which registers 2G only).  Set the full
 * supported set explicitly so the modem can use LTE; the network selection
 * preference (automatic) is left untouched.
 */
static void rat_preference_done(QmiClientNas *client, GAsyncResult *res,
				gpointer user_data)
{
	NmModem *modem = user_data;
	g_autoptr(QmiMessageNasSetSystemSelectionPreferenceOutput) output = NULL;
	g_autoptr(GError) error = NULL;

	if (!modem->bringup_active || modem->shutting_down)
		return;

	output = qmi_client_nas_set_system_selection_preference_finish(client,
								       res, &error);
	if (!output ||
	    !qmi_message_nas_set_system_selection_preference_output_get_result(output,
									       &error))
		nm_warn("could not set the RAT mode preference: %s", error->message);
	else
		nm_debug("RAT mode preference set (GSM/UMTS/LTE)");

	/* In ModemManager mode the core quirks are done: registration, DMS
	 * online and the data format belong to MM from here on. */
	if (!modem->config->call_enabled) {
		modem_ready(modem);
		return;
	}

	bringup_dms_online(modem);
}

static void bringup_rat_preference(NmModem *modem)
{
	g_autoptr(QmiMessageNasSetSystemSelectionPreferenceInput) input = NULL;
	g_autoptr(GError) error = NULL;

	input = qmi_message_nas_set_system_selection_preference_input_new();
	if (!qmi_message_nas_set_system_selection_preference_input_set_mode_preference(
			input,
			QMI_NAS_RAT_MODE_PREFERENCE_GSM |
			QMI_NAS_RAT_MODE_PREFERENCE_UMTS |
			QMI_NAS_RAT_MODE_PREFERENCE_LTE, &error)) {
		nm_warn("could not build the RAT mode preference request: %s",
			error->message);
		/* Not fatal: continue the bring-up */
		if (!modem->config->call_enabled)
			modem_ready(modem);
		else
			bringup_dms_online(modem);
		return;
	}

	qmi_client_nas_set_system_selection_preference(QMI_CLIENT_NAS(modem->nas),
						       input, 10, NULL,
						       (GAsyncReadyCallback)rat_preference_done,
						       modem);
}

static void bringup_nas_dual_standby(NmModem *modem)
{
	static const guint8 dual_standby[8] = { 0x01, 0, 0, 0, 0, 0, 0, 0 };
	NmQmiRawTlv tlv = { 0x14, dual_standby, sizeof(dual_standby) };

	/* NAS DUAL_STANDBY_PREF (0x004B): activate the primary subscription */
	nm_qmi_raw_call(modem->device, QMI_SERVICE_NAS,
			qmi_client_get_cid(modem->nas), 0x004B, &tlv, 1, 6,
			nas_dual_done, modem);
}

static void dms_online_done(QmiClientDms *client, GAsyncResult *res,
			    gpointer user_data)
{
	NmModem *modem = user_data;
	g_autoptr(QmiMessageDmsSetOperatingModeOutput) output = NULL;
	g_autoptr(GError) error = NULL;

	if (!modem->bringup_active || modem->shutting_down)
		return;

	output = qmi_client_dms_set_operating_mode_finish(client, res, &error);
	if (!output ||
	    !qmi_message_dms_set_operating_mode_output_get_result(output, &error)) {
		bringup_fail(modem, 2000, "DMS online failed: %s", error->message);
		return;
	}

	nm_debug("DMS online");
	bringup_radio(modem);
}

static void bringup_dms_online(NmModem *modem)
{
	g_autoptr(QmiMessageDmsSetOperatingModeInput) input = NULL;
	g_autoptr(GError) error = NULL;

	input = qmi_message_dms_set_operating_mode_input_new();
	if (!qmi_message_dms_set_operating_mode_input_set_mode(input,
			QMI_DMS_OPERATING_MODE_ONLINE, &error)) {
		bringup_fail(modem, 2000, "could not build the DMS request: %s",
			     error->message);
		return;
	}

	qmi_client_dms_set_operating_mode(QMI_CLIENT_DMS(modem->dms), input, 10,
					  NULL,
					  (GAsyncReadyCallback)dms_online_done,
					  modem);
}

/* ------------------------------------------------------------- AT / radio */

static void modem_ready(NmModem *modem);
static gboolean radio_step(gpointer user_data);

static guint schedule_radio(NmModem *modem, guint delay_ms)
{
	remove_source(&modem->radio_id);
	modem->radio_id = g_timeout_add(delay_ms ? delay_ms : 1, radio_step,
					modem);
	return modem->radio_id;
}

static gboolean radio_step(gpointer user_data)
{
	NmModem *modem = user_data;
	g_autofree gchar *response = NULL;
	g_autoptr(GError) error = NULL;
	const gchar *command = NULL;

	modem->radio_id = 0;
	if (!modem->bringup_active || modem->shutting_down)
		return G_SOURCE_REMOVE;

	switch (modem->at_step) {
	case 0:
		command = "AT+CFUN=0";
		if (!nm_at_command(modem->at, command, 3000, NULL, &error))
			nm_debug("%s: %s", command, error->message);
		modem->at_step = 1;
		schedule_radio(modem, CFUN_OFF_SETTLE_MS);
		break;

	case 1:
		command = "AT+CFUN=1";
		if (!nm_at_command(modem->at, command, 5000, NULL, &error))
			nm_debug("%s: %s", command, error->message);
		modem->at_step = 2;
		schedule_radio(modem, CFUN_ON_SETTLE_MS);
		break;

	case 2:
		command = "AT+COPS=0";
		if (!nm_at_command(modem->at, command, 5000, NULL, &error))
			nm_debug("%s: %s", command, error->message);
		modem->at_step = 3;
		modem->at_deadline = g_get_monotonic_time() +
			((gint64)modem->config->registration_timeout_ms * 1000);
		schedule_radio(modem, REGISTRATION_POLL_MS);
		break;

	case 3:
	case 4:
		if (!nm_at_command(modem->at, "AT+CREG?", 2000, &response, &error)) {
			bringup_fail(modem, 1000, "AT+CREG? failed: %s",
				     error->message);
			return G_SOURCE_REMOVE;
		}
		if (strstr(response, "+CREG: 0,1")) {
			nm_info("network registered");
			modem->at_step = 5;
			schedule_radio(modem, 1);
			break;
		}
		if (g_get_monotonic_time() >= modem->at_deadline) {
			if (modem->at_step == 3) {
				g_autofree gchar *cops = NULL;

				cops = g_strdup_printf("AT+COPS=1,2,\"%s\"",
						       modem->config->home_plmn);
				nm_info("automatic selection did not register, selecting %s",
					modem->config->home_plmn);
				if (!nm_at_command(modem->at, cops, 5000, NULL, &error))
					nm_debug("%s: %s", cops, error->message);
				modem->at_step = 4;
				modem->at_deadline = g_get_monotonic_time() +
					((gint64)modem->config->registration_manual_ms * 1000);
				schedule_radio(modem, REGISTRATION_POLL_MS);
				break;
			}
			bringup_fail(modem, 5000, "not registered");
			return G_SOURCE_REMOVE;
		}
		schedule_radio(modem, REGISTRATION_POLL_MS);
		break;

	case 5:
		if (!nm_at_command(modem->at, "AT+CGSMS=1", 3000, NULL, &error))
			nm_warn("AT+CGSMS=1 failed: %s", error->message);
		else
			nm_debug("SMS bearer: CS preferred");

		modem_ready(modem);
		break;

	default:
		break;
	}

	return G_SOURCE_REMOVE;
}

static void bringup_radio(NmModem *modem)
{
	if (!modem->at)
		modem->at = nm_at_new(modem->config->at_device);

	modem->at_step = 0;
	schedule_radio(modem, 1);
}

/* ------------------------------------------------------------- lifecycle */

static void modem_ready(NmModem *modem)
{
	/* The bring-up clients are not needed once the modem serves: releasing
	 * them keeps the QMI channels quiet for the other users (ModemManager
	 * allocates its own clients on the same channels). */
	if (modem->device) {
		if (modem->uim) {
			qmi_device_release_client(modem->device, modem->uim,
						  QMI_DEVICE_RELEASE_CLIENT_FLAGS_RELEASE_CID,
						  5, NULL, NULL, NULL);
			g_clear_object(&modem->uim);
		}
		if (modem->nas) {
			qmi_device_release_client(modem->device, modem->nas,
						  QMI_DEVICE_RELEASE_CLIENT_FLAGS_RELEASE_CID,
						  5, NULL, NULL, NULL);
			g_clear_object(&modem->nas);
		}
		if (modem->dms) {
			qmi_device_release_client(modem->device, modem->dms,
						  QMI_DEVICE_RELEASE_CLIENT_FLAGS_RELEASE_CID,
						  5, NULL, NULL, NULL);
			g_clear_object(&modem->dms);
		}
	}

	/* The AT port is not needed after the bring-up; other users (e.g.
	 * ModemManager) can use it without competing for responses. */
	g_clear_pointer(&modem->at, nm_at_free);


	modem->bringup_active = FALSE;
	modem->retry_count = 0;
	modem->state = NM_MODEM_READY;
	nm_info("modem is ready (registered)");
	if (modem->ready_fn)
		modem->ready_fn(modem->ready_data);
}

static void modem_loss(NmModem *modem)
{
	if (modem->shutting_down)
		return;
	if (modem->state == NM_MODEM_WAIT && !modem->bringup_active)
		return;

	nm_warn("modem went down");

	remove_source(&modem->wait_id);
	remove_source(&modem->card_id);
	remove_source(&modem->radio_id);
	/* The caller tears its data path down first, then the clients go. */
	if (modem->state == NM_MODEM_READY && modem->lost_fn)
		modem->lost_fn(modem->lost_data);

	bringup_cleanup_clients(modem);
	g_clear_pointer(&modem->control, g_free);
	if (modem->dpm)
		nm_dpm_reset(modem->dpm);

	modem->generation++;
	modem->bringup_active = FALSE;
	modem->card_session_sent = FALSE;
	modem->state = NM_MODEM_WAIT;
	modem->start_attempts = 0;
	modem->next_start_us = g_get_monotonic_time() + 1000000;
}

static void device_removed(QmiDevice *device, gpointer user_data)
{
	NmModem *modem = user_data;

	nm_warn("QMI control channel was removed");
	modem_loss(modem);
}

static void modem_try_start(NmModem *modem)
{
	g_autoptr(GError) error = NULL;

	nm_info("starting the modem (attempt %u)", modem->start_attempts + 1);

	/* The kernel boots the firmware synchronously in this write (the Q6
	 * auth handshake takes up to ~10 s).  This is the one place where
	 * blocking the main loop is acceptable: nothing else can be done
	 * before the modem is up. */
	if (!modem_write_state(modem->modem_state, "start", &error))
		nm_warn("could not start the modem: %s", error->message);

	modem->start_attempts++;
	modem->next_start_us = g_get_monotonic_time() +
		((gint64)MIN(2000 + 1000 * modem->start_attempts, 10000) * 1000);
}

static gboolean modem_poll(gpointer user_data)
{
	NmModem *modem = user_data;

	if (modem->shutting_down)
		return G_SOURCE_REMOVE;

	if (modem_is_running(modem)) {
		if (modem->state == NM_MODEM_WAIT) {
			nm_info("modem is running");
			modem->state = NM_MODEM_BRINGUP;
			modem->retry_count = 0;
			modem->next_retry_us = 0;
		}
		if (modem->state == NM_MODEM_BRINGUP &&
		    !modem->bringup_active &&
		    g_get_monotonic_time() >= modem->next_retry_us)
			bringup_start(modem);
		return G_SOURCE_CONTINUE;
	}

	if (modem->state != NM_MODEM_WAIT)
		modem_loss(modem);

	if (g_get_monotonic_time() >= modem->next_start_us)
		modem_try_start(modem);

	return G_SOURCE_CONTINUE;
}

/* --------------------------------------------------------------- public API */

/*
 * The remoteproc index is not stable: find the modem (fc880000) by its device
 * name instead of assuming remoteproc0.  Falls back to the configured path.
 */
static gchar *modem_resolve_state_path(const gchar *configured)
{
	GDir *dir;
	const gchar *entry;

	dir = g_dir_open("/sys/class/remoteproc", 0, NULL);
	if (!dir)
		return g_strdup(configured);

	while ((entry = g_dir_read_name(dir))) {
		g_autofree gchar *name_path = NULL;
		g_autofree gchar *name = NULL;

		name_path = g_strdup_printf("/sys/class/remoteproc/%s/name", entry);
		if (!g_file_get_contents(name_path, &name, NULL, NULL))
			continue;
		g_strstrip(name);
		if (strstr(name, "fc880000")) {
			gchar *state;

			state = g_strdup_printf("/sys/class/remoteproc/%s/state",
						entry);
			g_dir_close(dir);
			return state;
		}
	}

	g_dir_close(dir);
	return g_strdup(configured);
}

NmModem *nm_modem_new(const NmConfig *config)
{
	NmModem *modem = g_new0(NmModem, 1);

	modem->config = config;
	modem->at = nm_at_new(config->at_device);
	modem->modem_state = modem_resolve_state_path(config->modem_state);

	return modem;
}

static void modem_finish_stop(NmModem *modem)
{
	g_clear_object(&modem->device);
	g_clear_pointer(&modem->control, g_free);
	if (modem->stopped_fn)
		modem->stopped_fn(modem->stopped_data);
}

static void modem_dpm_stopped(GError *error, gpointer user_data)
{
	NmModem *modem = user_data;

	if (error)
		nm_warn("closing the DPM session failed: %s", error->message);
	modem_finish_stop(modem);
}

void nm_modem_stop(NmModem *modem)
{
	if (modem->shutting_down)
		return;
	modem->shutting_down = TRUE;

	remove_source(&modem->poll_id);
	remove_source(&modem->wait_id);
	remove_source(&modem->card_id);
	remove_source(&modem->radio_id);

	/* Release the rpmsg clients; the awaited DPM close below gives these
	 * requests time to leave the transport. */
	if (modem->device && qmi_device_is_open(modem->device)) {
		if (modem->uim)
			qmi_device_release_client(modem->device, modem->uim,
						  QMI_DEVICE_RELEASE_CLIENT_FLAGS_RELEASE_CID,
						  5, NULL, NULL, NULL);
		if (modem->nas)
			qmi_device_release_client(modem->device, modem->nas,
						  QMI_DEVICE_RELEASE_CLIENT_FLAGS_RELEASE_CID,
						  5, NULL, NULL, NULL);
		if (modem->dms)
			qmi_device_release_client(modem->device, modem->dms,
						  QMI_DEVICE_RELEASE_CLIENT_FLAGS_RELEASE_CID,
						  5, NULL, NULL, NULL);
	}
	g_clear_object(&modem->uim);
	g_clear_object(&modem->nas);
	g_clear_object(&modem->dms);

	if (modem->dpm && nm_dpm_is_open(modem->dpm)) {
		nm_dpm_stop(modem->dpm, modem_dpm_stopped, modem);
		return;
	}

	modem_finish_stop(modem);
}

void nm_modem_free(NmModem *modem)
{
	if (!modem)
		return;

	remove_source(&modem->poll_id);
	remove_source(&modem->wait_id);
	remove_source(&modem->card_id);
	remove_source(&modem->radio_id);
	g_clear_object(&modem->uim);
	g_clear_object(&modem->nas);
	g_clear_object(&modem->dms);
	g_clear_object(&modem->device);
	if (modem->dpm)
		nm_dpm_free(modem->dpm);
	if (modem->at)
		nm_at_free(modem->at);
	g_strfreev(modem->probe_order);
	g_free(modem->control);
	g_free(modem->modem_state);
	g_free(modem);
}

void nm_modem_set_ready(NmModem *modem, NmModemReadyFunc fn, gpointer user_data)
{
	modem->ready_fn = fn;
	modem->ready_data = user_data;
}

void nm_modem_set_lost(NmModem *modem, NmModemLostFunc fn, gpointer user_data)
{
	modem->lost_fn = fn;
	modem->lost_data = user_data;
}

void nm_modem_set_stopped(NmModem *modem, NmModemStoppedFunc fn,
			  gpointer user_data)
{
	modem->stopped_fn = fn;
	modem->stopped_data = user_data;
}

void nm_modem_start(NmModem *modem)
{
	modem_poll(modem);
	modem->poll_id = g_timeout_add(POLL_INTERVAL_MS, modem_poll, modem);
}

gboolean nm_modem_is_ready(NmModem *modem)
{
	return modem->state == NM_MODEM_READY;
}

const gchar *nm_modem_get_control_channel(NmModem *modem)
{
	return modem->control;
}

QmiDevice *nm_modem_get_device(NmModem *modem)
{
	return modem->device;
}
