// SPDX-License-Identifier: LGPL-2.1-only
/* Copyright (C) 2026 John Crispin <john@phrozen.org> */
#include <errno.h>

#include <linux/if_link.h>
#include <linux/pon_link.h>
#include <linux/rtnetlink.h>

#include <netlink/netlink.h>
#include <netlink/msg.h>
#include <netlink/attr.h>

#include "internal.h"

/**
 * link_msg - a route netlink request for a network device
 * @type: the message type
 * @flags: the netlink flags besides NLM_F_REQUEST
 * @ifindex: the device, or 0 for none
 *
 * Return: the message, or NULL when out of memory.
 */
static struct nl_msg *link_msg(int type, int flags, unsigned int ifindex)
{
	struct nl_msg *msg = nlmsg_alloc_simple(type, NLM_F_REQUEST | flags);
	struct ifinfomsg ifi = {
		.ifi_family = AF_UNSPEC,
		.ifi_index = ifindex,
	};

	if (!msg)
		return NULL;
	if (nlmsg_append(msg, &ifi, sizeof(ifi), NLMSG_ALIGNTO) < 0) {
		nlmsg_free(msg);
		return NULL;
	}
	return msg;
}

/**
 * pon_gem_link_add - create the network device of a GEM port
 * @rtnl: a client of NETLINK_ROUTE
 * @name: the device name
 * @lower: the ifindex of the PON data interface
 * @gem_id: the GEM port, which must exist already
 * @mac: the hardware address, or NULL for the one of @lower
 *
 * The address of a GEM network device is fixed at creation.
 *
 * Return: 0, or a negative errno; -EMSGSIZE for a name too long for the
 * message.
 */
int pon_gem_link_add(struct pon_nl *rtnl, const char *name, unsigned int lower,
		     uint32_t gem_id, const uint8_t *mac)
{
	struct nl_msg *msg = link_msg(RTM_NEWLINK,
				      NLM_F_CREATE | NLM_F_EXCL, 0);
	struct nlattr *info, *data;

	if (!msg)
		return lp_nomem(rtnl);
	if (nla_put_string(msg, IFLA_IFNAME, name) ||
	    nla_put_u32(msg, IFLA_LINK, lower) ||
	    (mac && nla_put(msg, IFLA_ADDRESS, 6, mac)))
		goto full;
	info = nla_nest_start(msg, IFLA_LINKINFO);
	if (!info || nla_put_string(msg, IFLA_INFO_KIND, "gem"))
		goto full;
	data = nla_nest_start(msg, IFLA_INFO_DATA);
	if (!data || nla_put_u32(msg, IFLA_GEM_ID, gem_id))
		goto full;
	nla_nest_end(msg, data);
	nla_nest_end(msg, info);
	return pon_nl_run(rtnl, msg, NULL, NULL);

full:
	return lp_put_failed(rtnl, msg);
}

/**
 * pon_gem_link_del - delete a network device
 * @rtnl: a client of NETLINK_ROUTE
 * @ifindex: the device
 *
 * Return: 0, or a negative errno.
 */
int pon_gem_link_del(struct pon_nl *rtnl, unsigned int ifindex)
{
	struct nl_msg *msg = link_msg(RTM_DELLINK, 0, ifindex);

	if (!msg)
		return lp_nomem(rtnl);
	return pon_nl_run(rtnl, msg, NULL, NULL);
}
