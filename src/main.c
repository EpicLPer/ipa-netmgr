/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * ipa-netmgr - data-path manager for Qualcomm IPA v2.x modems on mainline
 * Linux (MSM8994 / MPSS.BO.2.5, IPA v2.0 over BAM).
 *
 * The daemon owns the modem lifecycle (start, firmware quirks, recovery) and,
 * in linux mode, the WDS data call and the rmnet mux link.  See
 * notes/ipa-netmgr-spec.md in the main repository.
 */
#include <glib-unix.h>
#include <gio/gio.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "call.h"
#include "config.h"
#include "link.h"
#include "log.h"
#include "modem.h"

#define DEFAULT_CONFIG "/etc/ipa-netmgr.conf"

typedef struct {
	NmConfig *config;
	NmModem *modem;
	NmCall *call;
	NmLink *link;
	GMainLoop *loop;
	gboolean shutting_down;
	gboolean ready_notified;
	gboolean link_recreating;
	guint link_recreates;
	guint link_recreate_id;
	guint call_start_id;
	guint call_retry_id;
	guint call_retries;
	int exit_code;
} NmApp;

/* --------------------------------------------------------------- sd_notify */

/* Type=notify support without a libsystemd dependency. */
static void notify_ready(void)
{
	const gchar *path = g_getenv("NOTIFY_SOCKET");
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	int fd;

	if (!path || !*path)
		return;
	if (strlen(path) >= sizeof(addr.sun_path))
		return;

	strcpy(addr.sun_path, path);
	if (addr.sun_path[0] == '@')
		addr.sun_path[0] = '\0';

	fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return;
	if (sendto(fd, "READY=1\n", 8, 0, (struct sockaddr *)&addr,
		   sizeof(addr)) < 0)
		nm_debug("could not send READY to systemd: %s", g_strerror(errno));
	close(fd);
}

/* ------------------------------------------------------------------ shutdown */

static void app_shutdown_continue(NmApp *app)
{
	if (app->link_recreate_id) {
		g_source_remove(app->link_recreate_id);
		app->link_recreate_id = 0;
	}

	if (app->link) {
		nm_link_remove(app->link);
		g_clear_pointer(&app->link, nm_link_free);
	}

	if (app->modem) {
		nm_modem_stop(app->modem);
		return; /* modem_stopped() quits the loop */
	}

	g_main_loop_quit(app->loop);
}

static void modem_stopped(gpointer user_data)
{
	NmApp *app = user_data;

	g_main_loop_quit(app->loop);
}

static void call_stopped(GError *error, gpointer user_data)
{
	NmApp *app = user_data;

	if (error)
		nm_warn("stopping the data call failed: %s", error->message);
	app_shutdown_continue(app);
}

/*
 * Single shutdown path: every exit (signal or error) stops the call, removes
 * the link and releases the modem clients and the DPM session before the main
 * loop ends.  Abandoned QMI clients leak client IDs on this modem.
 */
static void app_shutdown(NmApp *app, int code)
{
	if (code && !app->exit_code)
		app->exit_code = code;
	if (app->shutting_down)
		return;
	app->shutting_down = TRUE;

	if (app->call && nm_call_is_active(app->call)) {
		nm_call_stop(app->call, call_stopped, app);
		return;
	}

	app_shutdown_continue(app);
}

static gboolean signal_handler(gpointer user_data)
{
	NmApp *app = user_data;

	nm_info("signal received, shutting down");
	app_shutdown(app, 0);

	return G_SOURCE_REMOVE;
}

/* ------------------------------------------------------------------ link */

static gboolean link_recreate_cb(gpointer user_data)
{
	NmApp *app = user_data;
	g_autoptr(GError) error = NULL;

	app->link_recreate_id = 0;
	if (app->shutting_down || !app->link || !app->call) {
		app->link_recreating = FALSE;
		return G_SOURCE_REMOVE;
	}

	if (nm_link_ensure(app->link, &error) &&
	    (!nm_call_get_ipv4(app->call) ||
	     nm_link_configure(app->link, nm_call_get_ipv4(app->call),
			       nm_call_get_gateway(app->call),
			       nm_call_get_dns(app->call), &error))) {
		app->link_recreating = FALSE;
		app->link_recreates = 0;
		nm_info("%s recreated", nm_link_get_name(app->link));
		return G_SOURCE_REMOVE;
	}

	nm_warn("could not recreate %s: %s", nm_link_get_name(app->link),
		error->message);
	if (++app->link_recreates >= app->config->link_max_recreates) {
		nm_error("%s could not be recreated, giving up",
			 nm_link_get_name(app->link));
		app_shutdown(app, 1);
		return G_SOURCE_REMOVE;
	}
	app->link_recreate_id = g_timeout_add(app->config->link_recreate_ms,
					      link_recreate_cb, app);

	return G_SOURCE_REMOVE;
}

static void link_lost(gpointer user_data)
{
	NmApp *app = user_data;

	if (app->shutting_down || app->link_recreating)
		return;

	if (app->link_recreates >= app->config->link_max_recreates) {
		nm_error("%s keeps disappearing, giving up",
			 nm_link_get_name(app->link));
		app_shutdown(app, 1);
		return;
	}

	/* ModemManager deletes mux links while it initializes the modem; the
	 * link is recreated after it has settled (bounded, not a restart loop). */
	app->link_recreating = TRUE;
	nm_warn("%s disappeared, recreating", nm_link_get_name(app->link));
	app->link_recreate_id = g_timeout_add(app->config->link_recreate_ms,
					      link_recreate_cb, app);
}

/* ------------------------------------------------------------------- call */

#define CALL_RETRY_MAX 3
#define CALL_RETRY_MS  5000

static void call_done(GError *error, gpointer user_data);
static gboolean call_start_cb(gpointer user_data);

static gboolean call_retry_cb(gpointer user_data)
{
	NmApp *app = user_data;

	app->call_retry_id = 0;
	if (app->shutting_down || !nm_modem_is_ready(app->modem))
		return G_SOURCE_REMOVE;

	nm_call_start(app->call, nm_modem_get_device(app->modem), call_done, app);
	return G_SOURCE_REMOVE;
}

static void call_done(GError *error, gpointer user_data)
{
	NmApp *app = user_data;
	g_autoptr(GError) link_error = NULL;

	if (error) {
		/* The network can reject a call right after registration; retry a
		 * few times before giving up (systemd would restart anyway). */
		if (++app->call_retries <= CALL_RETRY_MAX) {
			nm_warn("data call failed: %s (retry %u/%u in %u s)",
				error->message, app->call_retries, CALL_RETRY_MAX,
				CALL_RETRY_MS / 1000);
			app->call_retry_id = g_timeout_add(CALL_RETRY_MS,
							   call_retry_cb, app);
			return;
		}
		nm_error("data call failed: %s", error->message);
		app_shutdown(app, 1);
		return;
	}

	app->call_retries = 0;

	if (!app->link) {
		app->link = nm_link_new(app->config);
		if (!nm_link_ensure(app->link, &link_error)) {
			nm_error("could not create %s: %s",
				 nm_link_get_name(app->link),
				 link_error->message);
			app_shutdown(app, 1);
			return;
		}
		nm_link_watch(app->link, link_lost, app, &link_error);
	}

	if (!nm_link_configure(app->link, nm_call_get_ipv4(app->call),
			       nm_call_get_gateway(app->call),
			       nm_call_get_dns(app->call), &link_error)) {
		nm_error("could not set up %s: %s", nm_link_get_name(app->link),
			 link_error->message);
		app_shutdown(app, 1);
		return;
	}

	nm_info("ipa-netmgr ready: %s has %s (gw %s)", nm_link_get_name(app->link),
		nm_call_get_ipv4(app->call), nm_call_get_gateway(app->call));
}

/* ------------------------------------------------------------- modem hooks */

static void modem_lost(gpointer user_data)
{
	NmApp *app = user_data;

	if (app->shutting_down)
		return;

	nm_warn("modem lost, tearing the data path down");

	if (app->call_start_id) {
		g_source_remove(app->call_start_id);
		app->call_start_id = 0;
	}
	if (app->call_retry_id) {
		g_source_remove(app->call_retry_id);
		app->call_retry_id = 0;
	}
	if (app->link_recreate_id) {
		g_source_remove(app->link_recreate_id);
		app->link_recreate_id = 0;
	}

	if (app->link) {
		nm_link_remove(app->link);
		g_clear_pointer(&app->link, nm_link_free);
	}
	/* The transport is gone: abandon the call chain (stale callbacks become
	 * no-ops) and let the modem module bring the modem back. */
	nm_call_abort(app->call);
	app->link_recreating = FALSE;
	app->link_recreates = 0;
}

static gboolean call_start_cb(gpointer user_data)
{
	NmApp *app = user_data;

	app->call_start_id = 0;
	if (app->shutting_down || !nm_modem_is_ready(app->modem))
		return G_SOURCE_REMOVE;

	nm_call_start(app->call, nm_modem_get_device(app->modem), call_done, app);
	return G_SOURCE_REMOVE;
}

static void modem_ready_cb(gpointer user_data)
{
	NmApp *app = user_data;

	if (app->shutting_down)
		return;

	if (!g_strcmp0(app->config->mode, "linux") && app->config->call_enabled) {
		g_autoptr(GError) link_error = NULL;

		/* The mux link must exist before ModemManager probes the device:
		 * MM needs a net port to create the modem object, and rmnet_ipa0
		 * is ignored (its IPA data-format path wedges the modem).  The
		 * address is configured once the call is up. */
		if (!app->link) {
			app->link = nm_link_new(app->config);
			if (!nm_link_ensure(app->link, &link_error)) {
				nm_warn("could not create %s: %s",
					nm_link_get_name(app->link),
					link_error->message);
				g_clear_pointer(&app->link, nm_link_free);
			} else {
				nm_link_watch(app->link, link_lost, app,
					      &link_error);
			}
		}
	}

	if (!app->ready_notified) {
		app->ready_notified = TRUE;
		notify_ready();
	}

	if (!g_strcmp0(app->config->mode, "linux") && app->config->call_enabled) {
		if (!app->call)
			app->call = nm_call_new(app->config);
		app->call_retries = 0;
		/* Give the just-registered modem a moment before the first call
		 * (the network rejects calls made too early with CallFailed). */
		app->call_start_id = g_timeout_add(2000, call_start_cb, app);
		return;
	}

	nm_info("ipa-netmgr ready");
}

/* ------------------------------------------------------------------ main */

static NmLogLevel log_level_from_string(const gchar *level)
{
	if (!g_strcmp0(level, "debug"))
		return NM_LOG_DEBUG;
	if (!g_strcmp0(level, "warn"))
		return NM_LOG_WARN;
	if (!g_strcmp0(level, "error"))
		return NM_LOG_ERROR;
	return NM_LOG_INFO;
}

int main(int argc, char *argv[])
{
	g_autoptr(NmConfig) config = NULL;
	g_autoptr(GError) error = NULL;
	const gchar *config_path = DEFAULT_CONFIG;
	GMainLoop *loop;
	NmApp *app;
	int exit_code;
	int opt;

	while ((opt = getopt(argc, argv, "c:vh")) != -1) {
		switch (opt) {
		case 'c':
			config_path = optarg;
			break;
		case 'v':
			nm_log_set_level(NM_LOG_DEBUG);
			break;
		case 'h':
			fprintf(stderr, "usage: %s [-c config] [-v]\n", argv[0]);
			return 0;
		default:
			return 1;
		}
	}

	config = nm_config_load(config_path, &error);
	if (!config) {
		nm_error("could not load %s: %s", config_path, error->message);
		return 1;
	}
	nm_log_set_level(log_level_from_string(config->log_level));

	nm_info("starting (mode %s, config %s)", config->mode, config_path);

	loop = g_main_loop_new(NULL, FALSE);

	app = g_new0(NmApp, 1);
	app->config = config;
	app->loop = loop;

	g_unix_signal_add(SIGTERM, signal_handler, app);
	g_unix_signal_add(SIGINT, signal_handler, app);

	app->modem = nm_modem_new(config);
	nm_modem_set_ready(app->modem, modem_ready_cb, app);
	nm_modem_set_lost(app->modem, modem_lost, app);
	nm_modem_set_stopped(app->modem, modem_stopped, app);

	if (config->manage_modem)
		nm_modem_start(app->modem);
	else
		modem_ready_cb(app);

	g_main_loop_run(loop);

	exit_code = app->exit_code;

	nm_modem_free(app->modem);
	if (app->link)
		nm_link_free(app->link);
	if (app->call)
		nm_call_free(app->call);
	g_free(app);
	g_main_loop_unref(loop);

	return exit_code;
}
