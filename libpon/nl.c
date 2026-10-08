// SPDX-License-Identifier: LGPL-2.1-only
/* Copyright (C) 2026 John Crispin <john@phrozen.org> */
#include <sys/socket.h>

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <linux/netlink.h>

#include <netlink/netlink.h>
#include <netlink/attr.h>
#include <netlink/msg.h>

#include "internal.h"

#define PON_NL_RECV_MAX	4096

/**
 * struct nl_run - the state of one request
 * @nl: the client
 * @show: called for every reply message, or NULL
 * @arg: passed to @show
 * @seq: the sequence number of the request
 * @finished: the message that ends the request arrived
 */
struct nl_run {
	struct pon_nl *nl;
	pon_nl_show show;
	void *arg;
	uint32_t seq;
	bool finished;
};

static const int lp_errnos[] = {
	[NLE_FAILURE] = EIO,
	[NLE_INTR] = EINTR,
	[NLE_BAD_SOCK] = EBADF,
	[NLE_AGAIN] = EAGAIN,
	[NLE_NOMEM] = ENOBUFS,
	[NLE_EXIST] = EEXIST,
	[NLE_INVAL] = EINVAL,
	[NLE_RANGE] = ERANGE,
	[NLE_MSGSIZE] = EMSGSIZE,
	[NLE_OPNOTSUPP] = EOPNOTSUPP,
	[NLE_AF_NOSUPPORT] = EAFNOSUPPORT,
	[NLE_OBJ_NOTFOUND] = ENOENT,
	[NLE_MSG_OVERFLOW] = ENOBUFS,
	[NLE_MSG_TRUNC] = EMSGSIZE,
	[NLE_NOADDR] = EADDRNOTAVAIL,
	[NLE_BUSY] = EBUSY,
	[NLE_PROTO_MISMATCH] = EPROTONOSUPPORT,
	[NLE_NOACCESS] = EACCES,
	[NLE_PERM] = EPERM,
	[NLE_NODEV] = ENODEV,
	[NLE_DUMP_INTR] = EINTR,
};

/**
 * lp_errno - the errno of a libnl-tiny error code
 * @err: the negative NLE_ code a libnl-tiny call returned
 *
 * libnl-tiny folds ENOBUFS and ENOMEM into NLE_NOMEM. On a receive that
 * is the overrun of the socket buffer, so it maps to ENOBUFS.
 *
 * Return: the negative errno, -EIO for a code without one.
 */
int lp_errno(int err)
{
	unsigned int code = -err;

	if (code < ARRAY_SIZE(lp_errnos) && lp_errnos[code])
		return -lp_errnos[code];
	return -EIO;
}

/**
 * lp_refuse - fail a request without sending it
 * @nl: the client
 * @err: the negative errno
 * @why: the text pon_nl_error() gives
 *
 * Return: @err.
 */
int lp_refuse(struct pon_nl *nl, int err, const char *why)
{
	nl->err = err;
	snprintf(nl->err_msg, PON_NL_ERR_MAX, "%s", why);
	return err;
}

/**
 * lp_nomem - fail a request whose message could not be allocated
 * @nl: the client
 *
 * Return: -ENOMEM.
 */
int lp_nomem(struct pon_nl *nl)
{
	return lp_refuse(nl, -ENOMEM, strerror(ENOMEM));
}

/**
 * request_free - wipe the payload of a request and free it
 * @msg: the request
 *
 * A request can carry a key. Its bytes do not stay in freed memory.
 */
static void request_free(struct nl_msg *msg)
{
	struct nlmsghdr *nlh = nlmsg_hdr(msg);

	explicit_bzero(nlmsg_data(nlh), nlmsg_len(nlh));
	nlmsg_free(msg);
}

/**
 * lp_put_failed - fail a request that does not fit in its message
 * @nl: the client
 * @msg: the request; freed
 *
 * Return: -EMSGSIZE.
 */
int lp_put_failed(struct pon_nl *nl, struct nl_msg *msg)
{
	request_free(msg);
	return lp_refuse(nl, -EMSGSIZE,
			 "the request does not fit in a message");
}

/**
 * trace - hand one message to the trace hook of the client
 * @nl: the client
 * @tx: true for a request sent, false for a message received
 * @nlh: the message
 */
static void trace(struct pon_nl *nl, bool tx, const struct nlmsghdr *nlh)
{
	if (nl->trace)
		nl->trace(nl->trace_arg, tx, nlh->nlmsg_type, nlh->nlmsg_seq,
			  nlh->nlmsg_flags, nlh->nlmsg_len);
}

/**
 * msg_in - trace a message received and skip one of another request
 * @msg: the message
 * @arg: the struct nl_run of the request
 *
 * A dump that the kernel marked as interrupted sets the interrupted flag
 * of the client.
 *
 * Return: NL_OK, or NL_SKIP for a message with another sequence number.
 */
static int msg_in(struct nl_msg *msg, void *arg)
{
	struct nl_run *run = arg;
	struct nlmsghdr *nlh = nlmsg_hdr(msg);

	trace(run->nl, false, nlh);
	if (nlh->nlmsg_seq != run->seq)
		return NL_SKIP;
	if (nlh->nlmsg_flags & NLM_F_DUMP_INTR)
		run->nl->interrupted = true;
	return NL_OK;
}

/**
 * lp_seq_ok - take a message whatever its sequence number
 * @msg: the message
 * @arg: unused
 *
 * Replaces the sequence check of libnl-tiny. A request matches its answers
 * in msg_in() and error_cb() itself. A notification carries sequence 0,
 * which the check of libnl-tiny drops before the valid handler runs.
 *
 * Return: NL_OK.
 */
int lp_seq_ok(struct nl_msg *msg, void *arg)
{
	return NL_OK;
}

/**
 * valid_cb - hand one reply message to the handler of the request
 * @msg: the reply
 * @arg: the struct nl_run of the request
 *
 * Return: what the handler returns, or NL_OK without a handler.
 */
static int valid_cb(struct nl_msg *msg, void *arg)
{
	struct nl_run *run = arg;

	return run->show ? run->show(msg, run->arg) : NL_OK;
}

/**
 * ack_cb - end a request at its acknowledgment
 * @msg: the acknowledgment
 * @arg: the struct nl_run of the request
 *
 * Return: NL_STOP.
 */
static int ack_cb(struct nl_msg *msg, void *arg)
{
	struct nl_run *run = arg;

	run->finished = true;
	return NL_STOP;
}

/**
 * extack_text - copy the text of an extended acknowledgment
 * @nl: the client
 * @tlv: the attributes after the error
 * @len: their length
 */
static void extack_text(struct pon_nl *nl, char *tlv, int len)
{
	struct nlattr *tb[NLMSGERR_ATTR_MAX + 1];
	struct nlattr *text;

	if (len <= 0 || nla_parse(tb, NLMSGERR_ATTR_MAX, (struct nlattr *)tlv,
				  len, NULL))
		return;
	text = tb[NLMSGERR_ATTR_MSG];
	if (!text)
		return;
	strncpy(nl->err_msg, nla_data(text), PON_NL_ERR_MAX - 1);
	nl->err_msg[PON_NL_ERR_MAX - 1] = '\0';
}

/**
 * finish_cb - end a dump at its done message
 * @msg: the done message
 * @arg: the struct nl_run of the dump
 *
 * The done message of a dump carries the dump's own errno. With
 * NLM_F_ACK_TLVS the kernel's text follows it.
 *
 * Return: NL_STOP.
 */
static int finish_cb(struct nl_msg *msg, void *arg)
{
	struct nl_run *run = arg;
	struct nlmsghdr *nlh = nlmsg_hdr(msg);
	int skip = NLMSG_ALIGN(sizeof(int));
	int error;

	if (nlmsg_len(nlh) >= (int)sizeof(error)) {
		memcpy(&error, nlmsg_data(nlh), sizeof(error));
		if (error < 0)
			run->nl->err = error;
		if (error < 0 && (nlh->nlmsg_flags & NLM_F_ACK_TLVS))
			extack_text(run->nl, (char *)nlmsg_data(nlh) + skip,
				    nlmsg_len(nlh) - skip);
	}
	run->finished = true;
	return NL_STOP;
}

/**
 * error_cb - end a request at its error or acknowledgment
 * @nla: the sender
 * @nlerr: the error
 * @arg: the struct nl_run of the request
 *
 * The text follows the header of the request in a capped error, and the
 * whole request in an uncapped one.
 *
 * Return: NL_STOP, or NL_SKIP for the answer to another request.
 */
static int error_cb(struct sockaddr_nl *nla, struct nlmsgerr *nlerr, void *arg)
{
	struct nlmsghdr *nlh =
		(struct nlmsghdr *)((char *)nlerr - NLMSG_HDRLEN);
	struct nl_run *run = arg;
	struct pon_nl *nl = run->nl;
	char *tlv;

	if (nlh->nlmsg_seq != run->seq)
		return NL_SKIP;
	run->finished = true;
	nl->err = nlerr->error;
	nl->err_msg[0] = '\0';

	if (!(nlh->nlmsg_flags & NLM_F_ACK_TLVS))
		return NL_STOP;

	if (nlh->nlmsg_flags & NLM_F_CAPPED)
		tlv = (char *)nlerr + sizeof(*nlerr);
	else
		tlv = (char *)&nlerr->msg + NLMSG_ALIGN(nlerr->msg.nlmsg_len);
	extack_text(nl, tlv, (char *)nlh + nlh->nlmsg_len - tlv);
	return NL_STOP;
}

/**
 * pon_nl_open - connect a socket of one netlink protocol
 * @nl: the client
 * @protocol: NETLINK_GENERIC, NETLINK_ROUTE and so on
 *
 * Extended acknowledgments are requested in the capped form: an error
 * echoes the header of the request, not its attributes, so a refused key
 * does not come back. Each datagram is peeked at before it is read, so
 * that one larger than the first receive buffer is read whole and not
 * lost. The trace hook is cleared; set it after this call.
 *
 * Return: 0, or a negative errno; -EPROTONOSUPPORT for a protocol the
 * kernel does not have.
 */
int pon_nl_open(struct pon_nl *nl, int protocol)
{
	int on = 1, err;

	memset(nl, 0, sizeof(*nl));
	nl->sock = nl_socket_alloc();
	if (!nl->sock)
		return -ENOMEM;
	err = nl_connect(nl->sock, protocol);
	if (err) {
		nl_socket_free(nl->sock);
		nl->sock = NULL;
		return lp_errno(err);
	}
	setsockopt(nl_socket_get_fd(nl->sock), SOL_NETLINK, NETLINK_EXT_ACK,
		   &on, sizeof(on));
	setsockopt(nl_socket_get_fd(nl->sock), SOL_NETLINK, NETLINK_CAP_ACK,
		   &on, sizeof(on));
	nl_socket_enable_msg_peek(nl->sock);
	return 0;
}

/**
 * pon_nl_close - release the socket
 * @nl: the client
 */
void pon_nl_close(struct pon_nl *nl)
{
	if (nl->sock)
		nl_socket_free(nl->sock);
	nl->sock = NULL;
}

/**
 * pon_nl_run - send one request and collect the answer
 * @nl: the client
 * @msg: the request; consumed
 * @show: called for every reply message, or NULL
 * @arg: passed to @show
 *
 * Reads until the message that ends this request: the done message of
 * a dump, or the acknowledgment or error of a do request, matched by
 * sequence number. A reply to a do request arrives in its own datagram
 * before the acknowledgment. Anything with another sequence number is
 * left over from an earlier request and skipped.
 *
 * The kernel marks a dump when the list changed between two parts of it.
 * @show then saw an object twice or missed one. @nl->interrupted says so;
 * the return value does not.
 *
 * Return: 0, the kernel's negative errno, or -EIO for a transport failure.
 */
int pon_nl_run(struct pon_nl *nl, struct nl_msg *msg, pon_nl_show show,
	       void *arg)
{
	struct nl_run run = { .nl = nl, .show = show, .arg = arg };
	struct nl_cb *cb;
	unsigned int rounds = 0;
	int err;

	nl->err = 0;
	nl->err_msg[0] = '\0';
	nl->interrupted = false;
	nl->requests++;

	cb = nl_cb_alloc(NL_CB_DEFAULT);
	if (!cb) {
		request_free(msg);
		return lp_nomem(nl);
	}
	nl_cb_set(cb, NL_CB_MSG_IN, NL_CB_CUSTOM, msg_in, &run);
	nl_cb_set(cb, NL_CB_SEQ_CHECK, NL_CB_CUSTOM, lp_seq_ok, NULL);
	nl_cb_set(cb, NL_CB_VALID, NL_CB_CUSTOM, valid_cb, &run);
	nl_cb_set(cb, NL_CB_ACK, NL_CB_CUSTOM, ack_cb, &run);
	nl_cb_set(cb, NL_CB_FINISH, NL_CB_CUSTOM, finish_cb, &run);
	nl_cb_err(cb, NL_CB_CUSTOM, error_cb, &run);

	err = nl_send_auto_complete(nl->sock, msg);
	run.seq = nlmsg_hdr(msg)->nlmsg_seq;
	trace(nl, true, nlmsg_hdr(msg));
	request_free(msg);
	if (err < 0) {
		nl_cb_put(cb);
		nl->err = -EIO;
		return -EIO;
	}

	while (!run.finished) {
		err = nl_recvmsgs(nl->sock, cb);
		if (err < 0)
			break;
		if (++rounds > PON_NL_RECV_MAX) {
			err = -NLE_FAILURE;
			break;
		}
	}
	nl_cb_put(cb);
	if (nl->err < 0) {
		nl->refusals++;
		return nl->err;
	}
	if (err < 0) {
		nl->err = -EIO;
		snprintf(nl->err_msg, PON_NL_ERR_MAX, "%s", nl_geterror(err));
		return -EIO;
	}
	return 0;
}

/**
 * pon_nl_error - the text of the last refusal
 * @nl: the client
 *
 * Return: the kernel's extack message when it sent one, else strerror.
 */
const char *pon_nl_error(const struct pon_nl *nl)
{
	if (nl->err_msg[0])
		return nl->err_msg;
	return strerror(-nl->err);
}
