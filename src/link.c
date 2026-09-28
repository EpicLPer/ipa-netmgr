/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * rmnet mux link handling over rtnetlink (no libnl dependency).
 *
 * The IPA driver exposes one netdev (rmnet_ipa0); each QMAP mux is a separate
 * rmnet link on top of it (mainline rtnetlink kind "rmnet", data attribute
 * IFLA_RMNET_MUX_ID).  The link carries the WDS call's IPv4 address and the
 * default route.
 */
#include "link.h"
#include "log.h"

#include <glib-unix.h>
#include <gio/gio.h>

#include <errno.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/if_link.h>
#include <sys/socket.h>
#include <unistd.h>

/* Older kernel headers may not carry the rmnet attributes; the values are ABI */
#ifndef IFLA_RMNET_MUX_ID
#define IFLA_RMNET_MUX_ID 1
#endif

struct _NmLink {
	const NmConfig *config;
	gchar *name;
	NmLinkLostFunc lost;
	gpointer lost_data;
	int monitor_fd;
	guint monitor_id;
};

NmLink *nm_link_new(const NmConfig *config)
{
	NmLink *link = g_new0(NmLink, 1);

	link->config = config;
	link->monitor_fd = -1;
	link->name = g_strdup_printf(config->link_name, config->mux_id);

	return link;
}

void nm_link_free(NmLink *link)
{
	if (!link)
		return;
	if (link->monitor_id)
		g_source_remove(link->monitor_id);
	if (link->monitor_fd >= 0)
		close(link->monitor_fd);
	g_free(link->name);
	g_free(link);
}

const gchar *nm_link_get_name(NmLink *link)
{
	return link->name;
}

/* ------------------------------------------------------------------ netlink */

static int nl_open(gboolean monitor)
{
	struct sockaddr_nl addr = {
		.nl_family = AF_NETLINK,
		.nl_groups = monitor ? RTMGRP_LINK : 0,
	};
	int fd;

	fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
	if (fd < 0)
		return -1;
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(fd);
		return -1;
	}

	return fd;
}

static int addattr_l(struct nlmsghdr *n, gsize maxlen, int type,
		     const void *data, gsize alen)
{
	gsize len = RTA_LENGTH(alen);
	struct rtattr *rta;

	if (NLMSG_ALIGN(n->nlmsg_len) + RTA_ALIGN(len) > maxlen)
		return -1;
	rta = (struct rtattr *)((gchar *)n + NLMSG_ALIGN(n->nlmsg_len));
	rta->rta_type = type;
	rta->rta_len = len;
	if (alen)
		memcpy(RTA_DATA(rta), data, alen);
	n->nlmsg_len = NLMSG_ALIGN(n->nlmsg_len) + RTA_ALIGN(len);

	return 0;
}

static struct rtattr *addattr_nest(struct nlmsghdr *n, gsize maxlen, int type)
{
	struct rtattr *nest = (struct rtattr *)((gchar *)n + NLMSG_ALIGN(n->nlmsg_len));

	if (addattr_l(n, maxlen, type, NULL, 0) < 0)
		return NULL;

	return nest;
}

static void addattr_nest_end(struct nlmsghdr *n, struct rtattr *nest)
{
	nest->rta_len = (gchar *)n + n->nlmsg_len - (gchar *)nest;
}

/*
 * Send one request and wait for its ACK.  Only used for requests (never on the
 * monitoring socket), so exactly one NLMSG_ERROR reply is expected.
 */
static gboolean nl_talk(int fd, struct nlmsghdr *nlh, GError **error)
{
	struct sockaddr_nl addr = { .nl_family = AF_NETLINK };
	struct iovec iov = { .iov_base = nlh, .iov_len = nlh->nlmsg_len };
	struct msghdr msg = {
		.msg_name = &addr, .msg_namelen = sizeof(addr),
		.msg_iov = &iov, .msg_iovlen = 1,
	};

	nlh->nlmsg_flags |= NLM_F_REQUEST | NLM_F_ACK;
	nlh->nlmsg_seq = 1;

	if (sendmsg(fd, &msg, 0) < 0) {
		g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno),
			    "netlink send failed: %s", g_strerror(errno));
		return FALSE;
	}

	for (;;) {
		gchar resp[4096];
		struct iovec riov = { .iov_base = resp, .iov_len = sizeof(resp) };
		struct msghdr rmsg = {
			.msg_name = &addr, .msg_namelen = sizeof(addr),
			.msg_iov = &riov, .msg_iovlen = 1,
		};
		ssize_t n;
		struct nlmsghdr *h;

		n = recvmsg(fd, &rmsg, 0);
		if (n < 0) {
			g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno),
				    "netlink receive failed: %s", g_strerror(errno));
			return FALSE;
		}

		for (h = (struct nlmsghdr *)resp; NLMSG_OK(h, n); h = NLMSG_NEXT(h, n)) {
			if (h->nlmsg_type != NLMSG_ERROR)
				continue;

			struct nlmsgerr *e = NLMSG_DATA(h);

			if (e->error == 0)
				return TRUE;

			g_set_error(error, G_IO_ERROR, g_io_error_from_errno(-e->error),
				    "netlink request failed: %s", g_strerror(-e->error));
			return FALSE;
		}
	}
}

static gboolean link_create(NmLink *link, GError **error)
{
	gchar buf[1024] = { 0 };
	struct nlmsghdr *nlh = (struct nlmsghdr *)buf;
	struct ifinfomsg *ifi;
	struct rtattr *linkinfo, *infodata;
	guint32 base;
	guint16 mux;
	int fd;
	gboolean ok;

	nlh->nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg));
	nlh->nlmsg_type = RTM_NEWLINK;
	nlh->nlmsg_flags = NLM_F_CREATE | NLM_F_EXCL;

	ifi = NLMSG_DATA(nlh);
	ifi->ifi_family = AF_UNSPEC;

	if (addattr_l(nlh, sizeof(buf), IFLA_IFNAME, link->name,
		      strlen(link->name) + 1) < 0)
		goto too_long;

	base = if_nametoindex(link->config->base_link);
	if (!base) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
			    "base interface %s is not present",
			    link->config->base_link);
		return FALSE;
	}
	if (addattr_l(nlh, sizeof(buf), IFLA_LINK, &base, sizeof(base)) < 0)
		goto too_long;

	linkinfo = addattr_nest(nlh, sizeof(buf), IFLA_LINKINFO);
	if (!linkinfo)
		goto too_long;
	if (addattr_l(nlh, sizeof(buf), IFLA_INFO_KIND, "rmnet", 6) < 0)
		goto too_long;
	infodata = addattr_nest(nlh, sizeof(buf), IFLA_INFO_DATA);
	if (!infodata)
		goto too_long;
	mux = link->config->mux_id;
	if (addattr_l(nlh, sizeof(buf), IFLA_RMNET_MUX_ID, &mux, sizeof(mux)) < 0)
		goto too_long;
	addattr_nest_end(nlh, infodata);
	addattr_nest_end(nlh, linkinfo);

	fd = nl_open(FALSE);
	if (fd < 0) {
		g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno),
			    "could not open netlink socket: %s", g_strerror(errno));
		return FALSE;
	}
	ok = nl_talk(fd, nlh, error);
	close(fd);
	if (!ok)
		g_prefix_error(error, "create %s: ", link->name);

	return ok;

too_long:
	g_set_error(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE,
		    "netlink request for %s does not fit", link->name);
	return FALSE;
}

static gboolean link_set_up(NmLink *link, GError **error)
{
	gchar buf[256] = { 0 };
	struct nlmsghdr *nlh = (struct nlmsghdr *)buf;
	struct ifinfomsg *ifi;
	guint32 index;
	int fd;
	gboolean ok;

	index = if_nametoindex(link->name);
	if (!index) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
			    "%s is not present", link->name);
		return FALSE;
	}

	nlh->nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg));
	nlh->nlmsg_type = RTM_NEWLINK;

	ifi = NLMSG_DATA(nlh);
	ifi->ifi_family = AF_UNSPEC;
	ifi->ifi_index = index;
	ifi->ifi_flags = IFF_UP;
	ifi->ifi_change = IFF_UP;

	/* The rmnet driver derives the mux MTU (1496) from the real device;
	 * setting 1500 here is rejected with EINVAL. */

	fd = nl_open(FALSE);
	if (fd < 0) {
		g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno),
			    "could not open netlink socket: %s", g_strerror(errno));
		return FALSE;
	}
	ok = nl_talk(fd, nlh, error);
	close(fd);
	if (!ok)
		g_prefix_error(error, "bring up %s: ", link->name);

	return ok;
}

static gboolean link_add_address(NmLink *link, const gchar *ip, GError **error)
{
	gchar buf[256] = { 0 };
	struct nlmsghdr *nlh = (struct nlmsghdr *)buf;
	struct ifaddrmsg *ifa;
	struct in_addr addr;
	guint32 index;
	int fd;
	gboolean ok;

	index = if_nametoindex(link->name);
	if (!index) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
			    "%s is not present", link->name);
		return FALSE;
	}
	if (inet_pton(AF_INET, ip, &addr) != 1) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
			    "invalid IPv4 address '%s'", ip);
		return FALSE;
	}

	nlh->nlmsg_len = NLMSG_LENGTH(sizeof(struct ifaddrmsg));
	nlh->nlmsg_type = RTM_NEWADDR;
	nlh->nlmsg_flags = NLM_F_CREATE | NLM_F_REPLACE;

	ifa = NLMSG_DATA(nlh);
	ifa->ifa_family = AF_INET;
	ifa->ifa_prefixlen = link->config->prefix_len;
	ifa->ifa_scope = RT_SCOPE_UNIVERSE;
	ifa->ifa_index = index;

	if (addattr_l(nlh, sizeof(buf), IFA_LOCAL, &addr, sizeof(addr)) < 0 ||
	    addattr_l(nlh, sizeof(buf), IFA_ADDRESS, &addr, sizeof(addr)) < 0) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE,
			    "netlink request for %s does not fit", link->name);
		return FALSE;
	}

	fd = nl_open(FALSE);
	if (fd < 0) {
		g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno),
			    "could not open netlink socket: %s", g_strerror(errno));
		return FALSE;
	}
	ok = nl_talk(fd, nlh, error);
	close(fd);
	if (!ok)
		g_prefix_error(error, "add address to %s: ", link->name);

	return ok;
}

static gboolean link_add_default_route(NmLink *link, const gchar *gw,
				       GError **error)
{
	gchar buf[256] = { 0 };
	struct nlmsghdr *nlh = (struct nlmsghdr *)buf;
	struct rtmsg *rtm;
	struct in_addr addr;
	guint32 index;
	int fd;
	gboolean ok;

	index = if_nametoindex(link->name);
	if (!index) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
			    "%s is not present", link->name);
		return FALSE;
	}
	if (inet_pton(AF_INET, gw, &addr) != 1) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
			    "invalid IPv4 gateway '%s'", gw);
		return FALSE;
	}

	nlh->nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg));
	nlh->nlmsg_type = RTM_NEWROUTE;
	nlh->nlmsg_flags = NLM_F_CREATE | NLM_F_REPLACE;

	rtm = NLMSG_DATA(nlh);
	rtm->rtm_family = AF_INET;
	rtm->rtm_table = RT_TABLE_MAIN;
	rtm->rtm_protocol = RTPROT_BOOT;
	rtm->rtm_scope = RT_SCOPE_UNIVERSE;
	rtm->rtm_type = RTN_UNICAST;

	if (addattr_l(nlh, sizeof(buf), RTA_GATEWAY, &addr, sizeof(addr)) < 0 ||
	    addattr_l(nlh, sizeof(buf), RTA_OIF, &index, sizeof(index)) < 0) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE,
			    "netlink request for %s does not fit", link->name);
		return FALSE;
	}

	fd = nl_open(FALSE);
	if (fd < 0) {
		g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno),
			    "could not open netlink socket: %s", g_strerror(errno));
		return FALSE;
	}
	ok = nl_talk(fd, nlh, error);
	close(fd);
	if (!ok)
		g_prefix_error(error, "add route on %s: ", link->name);

	return ok;
}

/* Best-effort resolver configuration (Linux mode; systemd-resolved). */
static const gchar *resolvectl_path(void)
{
	static const gchar *candidates[] = {
		"/usr/sbin/resolvectl", "/usr/bin/resolvectl",
		"/sbin/resolvectl", "/bin/resolvectl",
	};

	for (guint i = 0; i < G_N_ELEMENTS(candidates); i++)
		if (g_file_test(candidates[i], G_FILE_TEST_IS_EXECUTABLE))
			return candidates[i];

	return "resolvectl";
}

static void link_set_dns(NmLink *link, const gchar *dns)
{
	const gchar *resolvectl = resolvectl_path();
	const gchar *argv_dns[] = { resolvectl, "dns", link->name, dns, NULL };
	const gchar *argv_domain[] = { resolvectl, "domain", link->name, "~.", NULL };
	g_autoptr(GError) error = NULL;

	if (!dns)
		return;

	if (!g_spawn_sync(NULL, (gchar **)argv_dns, NULL, G_SPAWN_STDOUT_TO_DEV_NULL,
			  NULL, NULL, NULL, NULL, NULL, &error))
		nm_warn("could not set DNS on %s: %s", link->name, error->message);

	g_clear_error(&error);
	if (!g_spawn_sync(NULL, (gchar **)argv_domain, NULL, G_SPAWN_STDOUT_TO_DEV_NULL,
			  NULL, NULL, NULL, NULL, NULL, &error))
		nm_debug("could not set DNS domain on %s: %s", link->name,
			 error->message);
}

/* ------------------------------------------------------------------- public */

gboolean nm_link_ensure(NmLink *link, GError **error)
{
	guint32 base;

	base = if_nametoindex(link->config->base_link);
	if (!base) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
			    "base interface %s is not present",
			    link->config->base_link);
		return FALSE;
	}

	/* The base interface must be up for the mux links to pass traffic. */
	if (!if_nametoindex(link->name)) {
		if (!link_create(link, error))
			return FALSE;
	}

	{
		gchar buf[256] = { 0 };
		struct nlmsghdr *nlh = (struct nlmsghdr *)buf;
		struct ifinfomsg *ifi;
		int fd;
		gboolean ok;

		nlh->nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg));
		nlh->nlmsg_type = RTM_NEWLINK;
		ifi = NLMSG_DATA(nlh);
		ifi->ifi_family = AF_UNSPEC;
		ifi->ifi_index = base;
		ifi->ifi_flags = IFF_UP;
		ifi->ifi_change = IFF_UP;

		fd = nl_open(FALSE);
		if (fd < 0) {
			g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno),
				    "could not open netlink socket: %s",
				    g_strerror(errno));
			return FALSE;
		}
		ok = nl_talk(fd, nlh, error);
		close(fd);
		if (!ok) {
			g_prefix_error(error, "bring up %s: ", link->config->base_link);
			return FALSE;
		}
	}

	return link_set_up(link, error);
}

gboolean nm_link_configure(NmLink *link, const gchar *ip, const gchar *gw,
			   const gchar *dns, GError **error)
{
	if (!ip || !gw) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
			    "call did not provide an address and gateway");
		return FALSE;
	}

	if (!link_add_address(link, ip, error))
		return FALSE;
	if (!link_add_default_route(link, gw, error))
		return FALSE;

	link_set_dns(link, dns);

	return TRUE;
}

void nm_link_remove(NmLink *link)
{
	gchar buf[128] = { 0 };
	struct nlmsghdr *nlh = (struct nlmsghdr *)buf;
	struct ifinfomsg *ifi;
	guint32 index;
	int fd;

	if (!link || !link->name)
		return;
	index = if_nametoindex(link->name);
	if (!index)
		return;

	nlh->nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg));
	nlh->nlmsg_type = RTM_DELLINK;
	ifi = NLMSG_DATA(nlh);
	ifi->ifi_family = AF_UNSPEC;
	ifi->ifi_index = index;

	fd = nl_open(FALSE);
	if (fd < 0)
		return;
	/* Removal on shutdown is best effort: log only. */
	{
		g_autoptr(GError) error = NULL;

		if (!nl_talk(fd, nlh, &error))
			nm_debug("could not remove %s: %s", link->name,
				 error->message);
	}
	close(fd);
}

static gboolean link_monitor_cb(gint fd, GIOCondition condition,
				gpointer user_data)
{
	NmLink *link = user_data;
	gchar buf[4096];

	for (;;) {
		ssize_t n = recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
		struct nlmsghdr *h;

		if (n < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return G_SOURCE_CONTINUE;
			nm_warn("link monitor read failed: %s", g_strerror(errno));
			return G_SOURCE_CONTINUE;
		}
		if (n == 0)
			return G_SOURCE_CONTINUE;

		for (h = (struct nlmsghdr *)buf; NLMSG_OK(h, n); h = NLMSG_NEXT(h, n)) {
			struct ifinfomsg *ifi;
			struct rtattr *rta;
			int len;
			const gchar *name = NULL;

			if (h->nlmsg_type != RTM_DELLINK &&
			    h->nlmsg_type != RTM_NEWLINK)
				continue;

			ifi = NLMSG_DATA(h);
			len = h->nlmsg_len - NLMSG_LENGTH(sizeof(*ifi));
			for (rta = IFLA_RTA(ifi); RTA_OK(rta, len); rta = RTA_NEXT(rta, len)) {
				if (rta->rta_type != IFLA_IFNAME)
					continue;
				name = RTA_DATA(rta);
			}

			if (h->nlmsg_type != RTM_DELLINK || !name)
				continue;
			if (g_strcmp0(name, link->name))
				continue;

			if (link->lost)
				link->lost(link->lost_data);
		}
	}
}

gboolean nm_link_watch(NmLink *link, NmLinkLostFunc lost, gpointer user_data,
		       GError **error)
{
	link->lost = lost;
	link->lost_data = user_data;

	if (link->monitor_id)
		return TRUE;

	link->monitor_fd = nl_open(TRUE);
	if (link->monitor_fd < 0) {
		g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno),
			    "could not open netlink monitor: %s", g_strerror(errno));
		return FALSE;
	}
	link->monitor_id = g_unix_fd_add(link->monitor_fd, G_IO_IN,
					 link_monitor_cb, link);

	return TRUE;
}
