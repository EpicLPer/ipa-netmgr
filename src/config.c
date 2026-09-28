/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "config.h"
#include "log.h"

#include <gio/gio.h>

#include <string.h>

#define DEFAULT_QMI_DEVICES "/dev/wwan0qmi1,/dev/wwan0qmi0"

static gchar *trim(gchar *s)
{
	while (g_ascii_isspace(*s))
		s++;
	if (*s == '\0')
		return s;
	gchar *end = s + strlen(s) - 1;
	while (end > s && g_ascii_isspace(*end))
		*end-- = '\0';
	return s;
}

static gboolean parse_bool(const gchar *value, gboolean *out)
{
	if (!g_ascii_strcasecmp(value, "1") || !g_ascii_strcasecmp(value, "yes") ||
	    !g_ascii_strcasecmp(value, "true") || !g_ascii_strcasecmp(value, "on")) {
		*out = TRUE;
		return TRUE;
	}
	if (!g_ascii_strcasecmp(value, "0") || !g_ascii_strcasecmp(value, "no") ||
	    !g_ascii_strcasecmp(value, "false") || !g_ascii_strcasecmp(value, "off")) {
		*out = FALSE;
		return TRUE;
	}
	return FALSE;
}

static gboolean parse_u32(const gchar *value, guint32 *out, GError **error)
{
	gchar *end = NULL;
	guint64 v = g_ascii_strtoull(value, &end, 0);

	if (end == value || (end && *end != '\0') || v > G_MAXUINT32) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
			    "invalid number '%s'", value);
		return FALSE;
	}
	*out = (guint32)v;
	return TRUE;
}

static void set_string(gchar **field, const gchar *value)
{
	g_free(*field);
	*field = g_strdup(value);
}

static void set_devices(gchar ***field, const gchar *value)
{
	GStrv devices = g_strsplit(value, ",", -1);

	for (guint i = 0; devices[i]; i++) {
		gchar *t = trim(devices[i]);

		if (t != devices[i])
			memmove(devices[i], t, strlen(t) + 1);
	}

	g_strfreev(*field);
	*field = devices;
}

static void config_apply(NmConfig *c, const gchar *key, const gchar *value,
			 GError **error)
{
	if (!g_strcmp0(key, "mode")) {
		set_string(&c->mode, value);
	} else if (!g_strcmp0(key, "manage_modem")) {
		parse_bool(value, &c->manage_modem);
	} else if (!g_strcmp0(key, "modem_state")) {
		set_string(&c->modem_state, value);
	} else if (!g_strcmp0(key, "qrtr_node")) {
		parse_u32(value, &c->qrtr_node, error);
	} else if (!g_strcmp0(key, "dpm_enabled")) {
		parse_bool(value, &c->dpm_enabled);
	} else if (!g_strcmp0(key, "control_port")) {
		set_string(&c->control_port, value);
	} else if (!g_strcmp0(key, "control_ep_type")) {
		parse_u32(value, &c->control_ep_type, error);
	} else if (!g_strcmp0(key, "control_iface")) {
		parse_u32(value, &c->control_iface, error);
	} else if (!g_strcmp0(key, "hw_ep_type")) {
		parse_u32(value, &c->hw_ep_type, error);
	} else if (!g_strcmp0(key, "hw_iface")) {
		parse_u32(value, &c->hw_iface, error);
	} else if (!g_strcmp0(key, "hw_rx")) {
		parse_u32(value, &c->hw_rx, error);
	} else if (!g_strcmp0(key, "hw_tx")) {
		parse_u32(value, &c->hw_tx, error);
	} else if (!g_strcmp0(key, "wda_enabled")) {
		parse_bool(value, &c->wda_enabled);
	} else if (!g_strcmp0(key, "qmi_devices")) {
		set_devices(&c->qmi_devices, value);
	} else if (!g_strcmp0(key, "wda_ep_type")) {
		parse_u32(value, &c->wda_ep_type, error);
	} else if (!g_strcmp0(key, "wda_ep_iface")) {
		parse_u32(value, &c->wda_ep_iface, error);
	} else if (!g_strcmp0(key, "at_device")) {
		set_string(&c->at_device, value);
	} else if (!g_strcmp0(key, "home_plmn")) {
		set_string(&c->home_plmn, value);
	} else if (!g_strcmp0(key, "apn")) {
		set_string(&c->apn, value);
	} else if (!g_strcmp0(key, "registration_timeout_ms")) {
		parse_u32(value, &c->registration_timeout_ms, error);
	} else if (!g_strcmp0(key, "registration_manual_ms")) {
		parse_u32(value, &c->registration_manual_ms, error);
	} else if (!g_strcmp0(key, "call_enabled")) {
		parse_bool(value, &c->call_enabled);
	} else if (!g_strcmp0(key, "profile")) {
		parse_u32(value, &c->profile, error);
	} else if (!g_strcmp0(key, "mux_id")) {
		parse_u32(value, &c->mux_id, error);
	} else if (!g_strcmp0(key, "subscription")) {
		parse_u32(value, &c->subscription, error);
	} else if (!g_strcmp0(key, "base_link")) {
		set_string(&c->base_link, value);
	} else if (!g_strcmp0(key, "link_name")) {
		set_string(&c->link_name, value);
	} else if (!g_strcmp0(key, "prefix_len")) {
		parse_u32(value, &c->prefix_len, error);
	} else if (!g_strcmp0(key, "link_max_recreates")) {
		parse_u32(value, &c->link_max_recreates, error);
	} else if (!g_strcmp0(key, "link_recreate_ms")) {
		parse_u32(value, &c->link_recreate_ms, error);
	} else if (!g_strcmp0(key, "log_level")) {
		set_string(&c->log_level, value);
	} else if (!g_strcmp0(key, "retry_min_ms")) {
		parse_u32(value, &c->retry_min_ms, error);
	} else if (!g_strcmp0(key, "retry_max_ms")) {
		parse_u32(value, &c->retry_max_ms, error);
	} else {
		nm_warn("unknown configuration key '%s'", key);
	}
}

NmConfig *nm_config_load(const gchar *path, GError **error)
{
	g_autoptr(NmConfig) c = g_new0(NmConfig, 1);
	g_autofree gchar *contents = NULL;
	g_auto(GStrv) lines = NULL;

	/* Measured MSM8994 / MPSS.BO.2.5 defaults */
	c->mode = g_strdup("linux");
	c->manage_modem = TRUE;
	c->modem_state = g_strdup("/sys/class/remoteproc/remoteproc0/state");
	c->qrtr_node = 0;
	c->dpm_enabled = TRUE;
	c->control_port = g_strdup("DATA5_CNTL");
	c->control_ep_type = 4;	/* embedded */
	c->control_iface = 1;
	c->hw_ep_type = 4;
	c->hw_iface = 1;
	c->hw_rx = 4;
	c->hw_tx = 5;
	c->wda_enabled = TRUE;
	c->qmi_devices = g_strsplit(DEFAULT_QMI_DEVICES, ",", -1);
	c->wda_ep_type = 4;
	c->wda_ep_iface = 1;
	c->at_device = g_strdup("/dev/wwan0at0");
	c->home_plmn = g_strdup("23203");
	c->apn = g_strdup("internet.t-mobile.at");
	c->registration_timeout_ms = 15000;
	c->registration_manual_ms = 30000;
	c->call_enabled = TRUE;
	c->profile = 1;
	c->mux_id = 1;
	c->subscription = 1;
	c->base_link = g_strdup("rmnet_ipa0");
	c->link_name = g_strdup("rmnet_data%u");
	c->prefix_len = 24;
	c->link_max_recreates = 5;
	c->link_recreate_ms = 5000;
	c->log_level = g_strdup("info");
	c->retry_min_ms = 1000;
	c->retry_max_ms = 30000;

	if (!g_file_get_contents(path, &contents, NULL, error))
		return NULL;

	lines = g_strsplit(contents, "\n", -1);
	for (guint i = 0; lines[i]; i++) {
		gchar *line = trim(lines[i]);

		if (*line == '\0' || *line == '#')
			continue;

		gchar *eq = strchr(line, '=');
		if (!eq) {
			nm_warn("%s:%u: ignoring line without '='", path, i + 1);
			continue;
		}
		*eq = '\0';
		gchar *key = trim(line);
		gchar *value = trim(eq + 1);

		config_apply(c, key, value, error);
		if (error && *error)
			return NULL;
	}

	return g_steal_pointer(&c);
}

void nm_config_free(NmConfig *config)
{
	if (!config)
		return;
	g_free(config->mode);
	g_free(config->modem_state);
	g_free(config->control_port);
	g_strfreev(config->qmi_devices);
	g_free(config->at_device);
	g_free(config->home_plmn);
	g_free(config->apn);
	g_free(config->base_link);
	g_free(config->link_name);
	g_free(config->log_level);
	g_free(config);
}
