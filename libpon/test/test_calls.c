// SPDX-License-Identifier: LGPL-2.1-only
/* Copyright (C) 2026 John Crispin <john@phrozen.org> */
#include <errno.h>
#include <string.h>

#include <linux/netlink.h>

#include <netlink/netlink.h>
#include <netlink/socket.h>

#include "libpon/pon.h"
#include "test.h"

#define T_PROTOCOL_NONE	99
#define T_NAME_LONG	5000

static bool t_alloc_fail;

struct nl_msg *__real_nlmsg_alloc(void);
struct nl_msg *__real_nlmsg_alloc_simple(int type, int flags);

/**
 * __wrap_nlmsg_alloc - nlmsg_alloc() of libnl-tiny, or a failure
 *
 * The link of this test redirects the calls of the library here.
 *
 * Return: NULL while t_alloc_fail is set, else the message.
 */
struct nl_msg *__wrap_nlmsg_alloc(void)
{
	return t_alloc_fail ? NULL : __real_nlmsg_alloc();
}

/**
 * __wrap_nlmsg_alloc_simple - nlmsg_alloc_simple() of libnl-tiny, or a
 * failure
 * @type: the message type
 * @flags: the netlink flags
 *
 * Return: NULL while t_alloc_fail is set, else the message.
 */
struct nl_msg *__wrap_nlmsg_alloc_simple(int type, int flags)
{
	return t_alloc_fail ? NULL : __real_nlmsg_alloc_simple(type, flags);
}

/**
 * calls_errno_test - a failure of libnl-tiny comes back as an errno
 */
static void calls_errno_test(void)
{
	static const uint8_t pdu[PON_OMCI_PDU_MIN];
	struct pon_omci omci;
	struct pon_nl client;
	struct pon pon;

	CHECK_EQ(pon_nl_open(&client, T_PROTOCOL_NONE), -EPROTONOSUPPORT);
	CHECK(!client.sock);

	memset(&pon, 0, sizeof(pon));
	CHECK_EQ(pon_event_process(&pon), -EBADF);

	memset(&omci, 0, sizeof(omci));
	omci.nl.sock = nl_socket_alloc();
	CHECK_EQ(pon_omci_send(&omci, pdu, sizeof(pdu)), -EBADF);
	nl_socket_free(omci.nl.sock);
}

/**
 * calls_put_test - a request that does not fit in its message is not sent
 */
static void calls_put_test(void)
{
	static char name[T_NAME_LONG];
	struct pon_nl rtnl;

	memset(name, 'g', sizeof(name) - 1);
	CHECK_EQ(pon_nl_open(&rtnl, NETLINK_ROUTE), 0);
	CHECK_EQ(pon_gem_link_add(&rtnl, name, 1, 1100, NULL), -EMSGSIZE);
	CHECK_STR(pon_nl_error(&rtnl), "the request does not fit in a message");
	CHECK_EQ(rtnl.requests, 0);
	pon_nl_close(&rtnl);
}

/**
 * calls_nomem_test - a request that cannot be allocated says so
 *
 * The text of the request before must not stay: pon_error() and
 * pon_nl_error() give the text of ENOMEM.
 */
static void calls_nomem_test(void)
{
	struct pon_dev_info dev;
	struct pon_nl rtnl;
	struct pon pon;

	memset(&pon, 0, sizeof(pon));
	pon.cmd.err = -EBUSY;
	snprintf(pon.cmd.err_msg, sizeof(pon.cmd.err_msg), "the link is busy");
	memset(&rtnl, 0, sizeof(rtnl));
	rtnl.err = -EBUSY;
	snprintf(rtnl.err_msg, sizeof(rtnl.err_msg), "the link is busy");

	t_alloc_fail = true;
	CHECK_EQ(pon_dev_get(&pon, 1, &dev), -ENOMEM);
	CHECK_STR(pon_error(&pon), strerror(ENOMEM));
	CHECK_EQ(pon_gem_link_del(&rtnl, 1), -ENOMEM);
	CHECK_STR(pon_nl_error(&rtnl), strerror(ENOMEM));
	t_alloc_fail = false;
}

/**
 * main - run the cases of the calls that need no peer
 *
 * Return: 0 when every check passed, else 1.
 */
int main(void)
{
	calls_errno_test();
	calls_put_test();
	calls_nomem_test();
	return t_report("test-calls");
}
