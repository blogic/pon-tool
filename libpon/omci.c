// SPDX-License-Identifier: LGPL-2.1-only
/* Copyright (C) 2026 John Crispin <john@phrozen.org> */
#include <sys/socket.h>

#include <errno.h>
#include <string.h>

#include <linux/genetlink.h>
#include <linux/netlink.h>

#include <netlink/netlink.h>
#include <netlink/attr.h>
#include <netlink/msg.h>
#include <netlink/genl/genl.h>
#include <netlink/genl/ctrl.h>

#include "internal.h"

#define PON_OMCI_RECV_BUFFER	8192

static const struct nla_policy omci_policy[PON_A_OMCI_MAX + 1] = {
	[PON_A_OMCI_DEV_ID] = { .type = NLA_U32, .maxlen = sizeof(uint32_t) },
	[PON_A_OMCI_PDU] = { .maxlen = PON_OMCI_PDU_MAX },
};

/**
 * omci_msg_new - a request of the OMCI channel with its device
 * @omci: the channel
 * @cmd: the command
 *
 * Return: the message, or NULL when out of memory.
 */
static struct nl_msg *omci_msg_new(struct pon_omci *omci, int cmd)
{
	struct nl_msg *msg = nlmsg_alloc();

	if (!msg)
		return NULL;
	if (!genlmsg_put(msg, 0, NL_AUTO_SEQ, omci->family, 0, 0, cmd,
			 PON_FAMILY_VERSION) ||
	    nla_put_u32(msg, PON_A_OMCI_DEV_ID, omci->dev_id)) {
		nlmsg_free(msg);
		return NULL;
	}
	return msg;
}

/**
 * pon_omci_open - own the OMCI channel of a PON device
 * @omci: the channel
 * @dev_id: the device
 *
 * Opens a socket of its own and registers it for the OMCI PDUs of
 * @dev_id. The socket then receives every PDU of the device and is the
 * only one that may send. The kernel ends the registration when the
 * socket closes. The socket is non-blocking once this returns.
 *
 * Return: 0, -ENODEV when the family is not registered, -EBUSY when
 * another socket owns the channel, or another negative errno.
 */
int pon_omci_open(struct pon_omci *omci, uint32_t dev_id)
{
	struct nl_msg *msg;
	int err;

	memset(omci, 0, sizeof(*omci));
	omci->dev_id = dev_id;

	err = pon_nl_open(&omci->nl, NETLINK_GENERIC);
	if (err)
		return err;

	omci->family = genl_ctrl_resolve(omci->nl.sock, PON_FAMILY_NAME);
	if (omci->family < 0) {
		err = -ENODEV;
		goto fail;
	}

	msg = omci_msg_new(omci, PON_CMD_OMCI_REGISTER);
	if (!msg) {
		err = -ENOMEM;
		goto fail;
	}
	err = pon_nl_run(&omci->nl, msg, NULL, NULL);
	if (err)
		goto fail;

	nl_socket_disable_auto_ack(omci->nl.sock);
	nl_socket_set_buffer_size(omci->nl.sock, PON_OMCI_BUFFER, 0);
	nl_socket_set_nonblocking(omci->nl.sock);
	return 0;

fail:
	pon_omci_close(omci);
	return err;
}

/**
 * pon_omci_close - give the OMCI channel up
 * @omci: the channel
 */
void pon_omci_close(struct pon_omci *omci)
{
	pon_nl_close(&omci->nl);
}

/**
 * pon_omci_fd - the descriptor to wait on for PDUs
 * @omci: the channel
 *
 * Return: the descriptor, or -1 when the channel is not open.
 */
int pon_omci_fd(const struct pon_omci *omci)
{
	return omci->nl.sock ? nl_socket_get_fd(omci->nl.sock) : -1;
}

/**
 * pon_omci_send - send one OMCI PDU to the OLT
 * @omci: the channel
 * @pdu: the PDU, from the transaction correlation id to the end of the
 *	 message contents, without an integrity field
 * @len: its length, PON_OMCI_PDU_MIN to PON_OMCI_PDU_MAX
 *
 * No acknowledgment is asked for. A PDU the kernel refuses comes back as
 * an error that pon_omci_recv() counts in @omci->tx_refused.
 *
 * Return: 0, -EAGAIN when the socket cannot take the request now, or
 * another negative errno.
 */
int pon_omci_send(struct pon_omci *omci, const uint8_t *pdu, size_t len)
{
	struct nl_msg *msg;
	int err;

	if (len < PON_OMCI_PDU_MIN || len > PON_OMCI_PDU_MAX)
		return -EINVAL;

	msg = omci_msg_new(omci, PON_CMD_OMCI_TX);
	if (!msg)
		return -ENOMEM;
	if (nla_put(msg, PON_A_OMCI_PDU, len, pdu)) {
		nlmsg_free(msg);
		return -EMSGSIZE;
	}

	err = nl_send_auto_complete(omci->nl.sock, msg);
	nlmsg_free(msg);
	if (err >= 0)
		return 0;
	err = lp_errno(err);
	if (err == -EAGAIN || err == -ENOBUFS)
		return -EAGAIN;
	return err;
}

/**
 * omci_error - count the error of a PDU the kernel refused
 * @omci: the channel
 * @nlh: the error message
 *
 * An error message too short for its header or with an error of 0 is not
 * counted.
 *
 * Return: 0.
 */
static int omci_error(struct pon_omci *omci, const struct nlmsghdr *nlh)
{
	const struct nlmsgerr *nlerr = NLMSG_DATA(nlh);

	if (nlh->nlmsg_len < NLMSG_LENGTH(sizeof(*nlerr)) || !nlerr->error)
		return 0;
	omci->tx_refused++;
	omci->tx_err = nlerr->error;
	return 0;
}

/**
 * omci_pdu - copy the PDU of an omci-ntf of the device
 * @omci: the channel
 * @nlh: the message
 * @buf: where the PDU goes
 * @size: the room in @buf
 *
 * Return: the length of the PDU, or 0 for a message that is not a valid
 * omci-ntf of the device or holds a PDU that does not fit.
 */
static int omci_pdu(struct pon_omci *omci, struct nlmsghdr *nlh, uint8_t *buf,
		    size_t size)
{
	struct genlmsghdr *gnlh = NLMSG_DATA(nlh);
	struct nlattr *tb[PON_A_OMCI_MAX + 1];
	int len;

	if (nlh->nlmsg_type != omci->family ||
	    nlh->nlmsg_len < NLMSG_LENGTH(GENL_HDRLEN) ||
	    gnlh->cmd != PON_CMD_OMCI_NTF)
		return 0;
	if (genlmsg_parse(nlh, 0, tb, PON_A_OMCI_MAX, omci_policy))
		return 0;
	if (!tb[PON_A_OMCI_DEV_ID] || !tb[PON_A_OMCI_PDU] ||
	    nla_get_u32(tb[PON_A_OMCI_DEV_ID]) != omci->dev_id)
		return 0;

	len = nla_len(tb[PON_A_OMCI_PDU]);
	if (len < PON_OMCI_PDU_MIN || (size_t)len > size)
		return 0;
	memcpy(buf, nla_data(tb[PON_A_OMCI_PDU]), len);
	return len;
}

/**
 * pon_omci_recv - read one OMCI PDU from the OLT
 * @omci: the channel
 * @buf: where the PDU goes
 * @size: the room in @buf, PON_OMCI_PDU_MAX for any PDU
 *
 * Reads one datagram. The kernel sends one message in each, a PDU or the
 * error of a refused pon_omci_send(). An error is counted in
 * @omci->tx_refused with its errno in @omci->tx_err. It reads as 0, as
 * does anything that is not a PDU of the device.
 *
 * Return: the length of the PDU, 0 when the datagram held none, -EAGAIN
 * when nothing is waiting, -ENOBUFS when PDUs were lost to a full socket,
 * or another negative errno.
 */
int pon_omci_recv(struct pon_omci *omci, uint8_t *buf, size_t size)
{
	uint8_t rx[PON_OMCI_RECV_BUFFER]
		__attribute__((aligned(NLMSG_ALIGNTO)));
	struct nlmsghdr *nlh = (struct nlmsghdr *)rx;
	ssize_t len;

	len = recv(pon_omci_fd(omci), rx, sizeof(rx), MSG_DONTWAIT);
	if (len < 0)
		return errno == EWOULDBLOCK ? -EAGAIN : -errno;
	if (!NLMSG_OK(nlh, (size_t)len))
		return 0;
	if (nlh->nlmsg_type == NLMSG_ERROR)
		return omci_error(omci, nlh);
	return omci_pdu(omci, nlh, buf, size);
}
