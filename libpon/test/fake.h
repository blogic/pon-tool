/* SPDX-License-Identifier: LGPL-2.1-only */
/* Copyright (C) 2026 John Crispin <john@phrozen.org> */
#ifndef LIBPON_TEST_FAKE_H
#define LIBPON_TEST_FAKE_H

#include <sys/socket.h>

#include <stdint.h>
#include <stdio.h>

#include <linux/netlink.h>

#include <netlink/netlink.h>
#include <netlink/attr.h>
#include <netlink/msg.h>
#include <netlink/socket.h>
#include <netlink/genl/genl.h>

#define T_FAKE_RECV_MAX		65536
#define T_FAKE_REQUEST_LEN	64

/**
 * struct t_fake - a socket that plays the kernel for a client
 * @kernel: the socket of the fake
 * @client: the socket of the client, connected
 * @family: the generic netlink family the fake answers as
 * @version: the version of the family
 */
struct t_fake {
	struct nl_sock *kernel;
	struct nl_sock *client;
	int family;
	uint8_t version;
};

/**
 * t_fake_open - let a socket play the kernel for a client
 * @fake: the fake, overwritten
 * @client: the socket of the client, connected
 * @family: the generic netlink family the fake answers as
 * @version: the version of the family
 *
 * The client sends its requests to @fake->kernel instead of the kernel.
 * A test queues the answers on the client before the request. Sending to
 * another socket needs CAP_NET_ADMIN in the network namespace.
 *
 * Return: 0, or -1 when the socket cannot be opened.
 */
static inline int t_fake_open(struct t_fake *fake, struct nl_sock *client,
			      int family, uint8_t version)
{
	fake->client = client;
	fake->family = family;
	fake->version = version;
	fake->kernel = nl_socket_alloc();
	if (!fake->kernel || nl_connect(fake->kernel, NETLINK_GENERIC))
		return -1;
	nl_socket_set_peer_port(client, nl_socket_get_local_port(fake->kernel));
	return 0;
}

/**
 * t_fake_close - release the socket of the fake
 * @fake: the fake
 */
static inline void t_fake_close(struct t_fake *fake)
{
	nl_socket_free(fake->kernel);
}

/**
 * t_fake_recv - drop what waits on a socket
 * @sock: the socket
 *
 * Return: the number of datagrams dropped.
 */
static inline int t_fake_recv(struct nl_sock *sock)
{
	static uint8_t buf[T_FAKE_RECV_MAX];
	int count = 0;

	while (recv(nl_socket_get_fd(sock), buf, sizeof(buf),
		    MSG_DONTWAIT) > 0)
		count++;
	return count;
}

/**
 * t_fake_drain - drop the requests the client sent to the fake
 * @fake: the fake
 *
 * Return: the number of requests dropped.
 */
static inline int t_fake_drain(struct t_fake *fake)
{
	return t_fake_recv(fake->kernel);
}

/**
 * t_fake_request - take the next request the client sent to the fake
 * @fake: the fake
 * @tb: the index of its attributes, @max + 1 entries
 * @max: the highest attribute of the set
 *
 * Return: the command of the request, or -1 when none waits or it does
 * not parse.
 */
static inline int t_fake_request(struct t_fake *fake, struct nlattr **tb,
				 int max)
{
	static uint8_t buf[T_FAKE_RECV_MAX];
	struct nlmsghdr *nlh = (struct nlmsghdr *)buf;
	struct genlmsghdr *gnlh;
	ssize_t len;

	len = recv(nl_socket_get_fd(fake->kernel), buf, sizeof(buf),
		   MSG_DONTWAIT);
	if (len < (ssize_t)NLMSG_LENGTH(GENL_HDRLEN) ||
	    !genlmsg_valid_hdr(nlh, 0))
		return -1;
	gnlh = nlmsg_data(nlh);
	if (nla_parse(tb, max, genlmsg_attrdata(gnlh, 0),
		      genlmsg_attrlen(gnlh, 0), NULL))
		return -1;
	return gnlh->cmd;
}

/**
 * t_fake_send - send a message from the fake to a socket of the client
 * @fake: the fake
 * @to: the socket of the client
 * @msg: the message; consumed
 */
static inline void t_fake_send(struct t_fake *fake, struct nl_sock *to,
			       struct nl_msg *msg)
{
	nl_socket_set_peer_port(fake->kernel, nl_socket_get_local_port(to));
	if (nl_send(fake->kernel, msg) < 0)
		fprintf(stderr, "cannot send to the client\n");
	nlmsg_free(msg);
}

/**
 * t_fake_seq - the sequence number of the client's next request
 * @fake: the fake
 *
 * Return: the number.
 */
static inline uint32_t t_fake_seq(struct t_fake *fake)
{
	return fake->client->s_seq_next;
}

/**
 * t_fake_reply - a reply of the family to the client's next request
 * @fake: the fake
 * @cmd: the command
 * @flags: the netlink flags, NLM_F_MULTI for a part of a dump
 *
 * Return: the message.
 */
static inline struct nl_msg *t_fake_reply(struct t_fake *fake, int cmd,
					  int flags)
{
	struct nl_msg *msg = nlmsg_alloc();

	genlmsg_put(msg, 0, t_fake_seq(fake), fake->family, 0, flags, cmd,
		    fake->version);
	return msg;
}

/**
 * t_fake_ack - queue the acknowledgment of the client's next request
 * @fake: the fake
 * @error: 0, or the negative errno of a refusal
 * @text: the extack text, or NULL
 *
 * The acknowledgment has the capped form the kernel sends to a client
 * with NETLINK_CAP_ACK: the header of the request with its length, not
 * its attributes.
 */
static inline void t_fake_ack(struct t_fake *fake, int error, const char *text)
{
	struct nl_msg *msg = nlmsg_alloc_simple(NLMSG_ERROR,
						NLM_F_CAPPED |
						(text ? NLM_F_ACK_TLVS : 0));
	struct nlmsgerr err = {
		.error = error,
		.msg = {
			.nlmsg_len = T_FAKE_REQUEST_LEN,
			.nlmsg_seq = t_fake_seq(fake),
		},
	};

	nlmsg_hdr(msg)->nlmsg_seq = t_fake_seq(fake);
	nlmsg_append(msg, &err, sizeof(err), NLMSG_ALIGNTO);
	if (text)
		nla_put_string(msg, NLMSGERR_ATTR_MSG, text);
	t_fake_send(fake, fake->client, msg);
}

/**
 * t_fake_done - queue the end of the dump the client asks for next
 * @fake: the fake
 * @error: 0, or the negative errno that ended the dump
 * @flags: extra netlink flags, NLM_F_DUMP_INTR for a changed list
 * @text: the extack text, or NULL
 */
static inline void t_fake_done(struct t_fake *fake, int error, int flags,
			       const char *text)
{
	struct nl_msg *msg;

	flags |= NLM_F_MULTI;
	if (text)
		flags |= NLM_F_ACK_TLVS;
	msg = nlmsg_alloc_simple(NLMSG_DONE, flags);
	nlmsg_hdr(msg)->nlmsg_seq = t_fake_seq(fake);
	nlmsg_append(msg, &error, sizeof(error), NLMSG_ALIGNTO);
	if (text)
		nla_put_string(msg, NLMSGERR_ATTR_MSG, text);
	t_fake_send(fake, fake->client, msg);
}

#endif
