// SPDX-License-Identifier: LGPL-2.1-only
/* Copyright (C) 2026 John Crispin <john@phrozen.org> */
#include <sys/socket.h>

#include <errno.h>
#include <string.h>

#include <linux/netlink.h>

#include <netlink/netlink.h>
#include <netlink/genl/genl.h>
#include <netlink/genl/ctrl.h>
#include <netlink/genl/family.h>

#include "internal.h"

/**
 * pon_error - the text of the last refusal
 * @pon: the client
 *
 * Return: the kernel's extack message when it sent one, else strerror.
 */
const char *pon_error(const struct pon *pon)
{
	return pon_nl_error(&pon->cmd);
}

/**
 * notify_dev - call back for a device notification
 * @pon: the client
 * @msg: the notification
 * @cmd: the command of the notification
 *
 * A PLOAM notification goes to the ploam callback when it carries the
 * activation state; every other one goes to the dev callback.
 */
static void notify_dev(struct pon *pon, struct nl_msg *msg, uint8_t cmd)
{
	struct pon_dev_info dev;

	if (lp_dev_decode(msg, &dev))
		return;
	if (cmd == PON_CMD_PLOAM_NTF) {
		if ((dev.has & PON_DEV_F_PLOAM) && pon->ops->ploam)
			pon->ops->ploam(pon, &dev);
		return;
	}
	if (pon->ops->dev)
		pon->ops->dev(pon, cmd, &dev);
}

/**
 * notify_event - call back for an event notification
 * @pon: the client
 * @msg: the notification
 */
static void notify_event(struct pon *pon, struct nl_msg *msg)
{
	struct pon_event ev;

	if (!lp_event_decode(msg, &ev) && pon->ops->event)
		pon->ops->event(pon, &ev);
}

/**
 * notify_alarm - call back for an alarm notification
 * @pon: the client
 * @msg: the notification
 */
static void notify_alarm(struct pon *pon, struct nl_msg *msg)
{
	struct pon_alarms alarms;

	if (!lp_alarm_decode(msg, &alarms) && pon->ops->alarm)
		pon->ops->alarm(pon, &alarms);
}

/**
 * notify_tcont - call back for a T-CONT notification
 * @pon: the client
 * @msg: the notification
 * @cmd: the command of the notification
 */
static void notify_tcont(struct pon *pon, struct nl_msg *msg, uint8_t cmd)
{
	struct pon_tcont tcont;

	if (!lp_tcont_decode(msg, &tcont) && pon->ops->tcont)
		pon->ops->tcont(pon, cmd, &tcont);
}

/**
 * notify_gem - call back for a GEM port notification
 * @pon: the client
 * @msg: the notification
 * @cmd: the command of the notification
 */
static void notify_gem(struct pon *pon, struct nl_msg *msg, uint8_t cmd)
{
	struct pon_gem gem;

	if (!lp_gem_decode(msg, &gem) && pon->ops->gem)
		pon->ops->gem(pon, cmd, &gem);
}

/**
 * notify_gem_map - call back for a mapper rule notification
 * @pon: the client
 * @msg: the notification
 * @cmd: the command of the notification
 */
static void notify_gem_map(struct pon *pon, struct nl_msg *msg, uint8_t cmd)
{
	struct pon_gem_map map;

	if (!lp_gem_map_decode(msg, &map) && pon->ops->gem_map)
		pon->ops->gem_map(pon, cmd, &map);
}

/**
 * notify_cb - decode one notification and call back
 * @msg: the notification
 * @arg: the client
 *
 * Return: NL_OK, or NL_SKIP for a message too short for its header or of
 * another command.
 */
static int notify_cb(struct nl_msg *msg, void *arg)
{
	struct pon *pon = arg;
	struct genlmsghdr *gnlh = nlmsg_data(nlmsg_hdr(msg));

	if (!genlmsg_valid_hdr(nlmsg_hdr(msg), 0))
		return NL_SKIP;
	pon->notifications++;
	switch (gnlh->cmd) {
	case PON_CMD_DEV_ADD_NTF:
	case PON_CMD_DEV_DEL_NTF:
	case PON_CMD_DEV_CHANGE_NTF:
	case PON_CMD_PLOAM_NTF:
		notify_dev(pon, msg, gnlh->cmd);
		break;
	case PON_CMD_EVENT_NTF:
		notify_event(pon, msg);
		break;
	case PON_CMD_ALARM_NTF:
		notify_alarm(pon, msg);
		break;
	case PON_CMD_TCONT_ADD_NTF:
	case PON_CMD_TCONT_CHANGE_NTF:
	case PON_CMD_TCONT_DEL_NTF:
		notify_tcont(pon, msg, gnlh->cmd);
		break;
	case PON_CMD_GEM_ADD_NTF:
	case PON_CMD_GEM_DEL_NTF:
		notify_gem(pon, msg, gnlh->cmd);
		break;
	case PON_CMD_GEM_MAP_ADD_NTF:
	case PON_CMD_GEM_MAP_DEL_NTF:
		notify_gem_map(pon, msg, gnlh->cmd);
		break;
	default:
		return NL_SKIP;
	}
	return NL_OK;
}

/**
 * pon_event_fd - the descriptor to wait on for notifications
 * @pon: the client, opened with notification callbacks
 *
 * Return: the descriptor, or -1 when the client has no event socket.
 */
int pon_event_fd(const struct pon *pon)
{
	return pon->evt ? nl_socket_get_fd(pon->evt) : -1;
}

/**
 * event_waiting - whether a datagram waits on the notification socket
 * @sock: the socket
 *
 * This probe reports a pending overrun of the receive buffer and clears
 * it.
 *
 * Return: 1 when a datagram waits, 0 when none does, or a negative errno;
 * -ENOBUFS when notifications were lost.
 */
static int event_waiting(struct nl_sock *sock)
{
	uint8_t byte;

	for (;;) {
		if (recv(nl_socket_get_fd(sock), &byte, sizeof(byte),
			 MSG_PEEK | MSG_DONTWAIT) >= 0)
			return 1;
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			return 0;
		if (errno != EINTR)
			return -errno;
	}
}

/**
 * pon_event_process - read the pending notifications and call back for each
 * @pon: the client
 *
 * The socket is non-blocking; this reads datagram after datagram and
 * returns when nothing is left to read. A failure means notifications
 * were lost, most often by an overrun of the receive buffer. The caller
 * must then re-read the kernel's objects.
 *
 * Return: 0, or a negative errno when notifications were lost: -ENOBUFS
 * for an overrun, -EBADF for a client without an event socket.
 */
int pon_event_process(struct pon *pon)
{
	struct nl_cb *cb;
	int err;

	if (!pon->evt)
		return -EBADF;
	cb = nl_cb_alloc(NL_CB_DEFAULT);
	if (!cb)
		return -ENOMEM;
	nl_cb_set(cb, NL_CB_SEQ_CHECK, NL_CB_CUSTOM, lp_seq_ok, NULL);
	nl_cb_set(cb, NL_CB_VALID, NL_CB_CUSTOM, notify_cb, pon);
	while ((err = event_waiting(pon->evt)) > 0) {
		err = nl_recvmsgs(pon->evt, cb);
		if (err < 0 && err != -NLE_AGAIN && err != -NLE_INTR) {
			err = lp_errno(err);
			break;
		}
	}
	nl_cb_put(cb);
	return err < 0 ? err : 0;
}

/**
 * group_join - join the notification socket to a multicast group
 * @pon: the client, with its request socket open
 * @name: the group of the pon family
 *
 * Return: 0, or a negative code when the group is not found or cannot be
 * joined.
 */
static int group_join(struct pon *pon, const char *name)
{
	int grp = genl_ctrl_resolve_grp(pon->cmd.sock, PON_FAMILY_NAME, name);

	if (grp < 0)
		return -1;
	return nl_socket_add_membership(pon->evt, grp);
}

/**
 * events_open - open the socket of the notifications
 * @pon: the client, with its request socket open
 *
 * The socket joins the state and mgmt groups, does not block and peeks at
 * each datagram before it reads it, so that one larger than the first
 * receive buffer is read whole and not lost.
 *
 * Return: 0, or a negative errno.
 */
static int events_open(struct pon *pon)
{
	int err;

	pon->evt = nl_socket_alloc();
	if (!pon->evt)
		return -ENOMEM;
	err = genl_connect(pon->evt);
	if (err)
		return lp_errno(err);
	if (group_join(pon, PON_MCGRP_STATE) || group_join(pon, PON_MCGRP_MGMT))
		return -EIO;
	nl_socket_set_buffer_size(pon->evt, PON_EVENT_BUFFER, 0);
	nl_socket_set_nonblocking(pon->evt);
	nl_socket_enable_msg_peek(pon->evt);
	return 0;
}

/**
 * pon_open - connect to the pon family
 * @pon: the client
 * @ops: the notification callbacks, or NULL for a client that only sends
 *	 requests
 * @priv: the caller's context, kept in @pon->priv
 *
 * With @ops, a second socket joins the state and mgmt groups before this
 * returns, so a dump the caller runs next loses nothing that happens
 * while it runs.
 *
 * Return: 0, -ENODEV when the family is not registered, or another
 * negative errno.
 */
int pon_open(struct pon *pon, const struct pon_notify_ops *ops, void *priv)
{
	int err;

	memset(pon, 0, sizeof(*pon));
	pon->ops = ops;
	pon->priv = priv;

	err = pon_nl_open(&pon->cmd, NETLINK_GENERIC);
	if (err)
		return err;
	pon->family = genl_ctrl_resolve(pon->cmd.sock, PON_FAMILY_NAME);
	if (pon->family < 0) {
		err = -ENODEV;
		goto fail;
	}
	if (ops) {
		err = events_open(pon);
		if (err)
			goto fail;
	}
	return 0;

fail:
	pon_close(pon);
	return err;
}

/**
 * pon_close - release both sockets
 * @pon: the client
 */
void pon_close(struct pon *pon)
{
	if (pon->evt)
		nl_socket_free(pon->evt);
	pon->evt = NULL;
	pon_nl_close(&pon->cmd);
}
