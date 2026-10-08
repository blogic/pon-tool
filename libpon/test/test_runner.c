// SPDX-License-Identifier: LGPL-2.1-only
/* Copyright (C) 2026 John Crispin <john@phrozen.org> */
#include <sys/socket.h>

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <linux/genetlink.h>
#include <linux/netlink.h>

#include <netlink/netlink.h>
#include <netlink/attr.h>
#include <netlink/msg.h>
#include <netlink/socket.h>
#include <netlink/genl/genl.h>

#include "libpon/pon.h"
#include "test.h"
#include "fake.h"

#define T_FAMILY	0x7000
#define T_LARGE		20000
#define T_GROUP		GENL_ID_CTRL
#define T_OVERRUN_EXTRA	16

static struct t_fake fake;
static struct pon pon;
static unsigned int t_objects;
static unsigned int t_events;
static struct pon_event t_event;
static uint8_t t_key[PON_KEY_LEN];
static unsigned int t_keys_freed;

void __real_nlmsg_free(struct nl_msg *msg);

/**
 * __wrap_nlmsg_free - count the messages freed with t_key in them
 * @msg: the message, or NULL
 *
 * The runner links with --wrap=nlmsg_free, so every free of a message
 * passes here before libnl-tiny frees it.
 */
void __wrap_nlmsg_free(struct nl_msg *msg)
{
	struct nlmsghdr *nlh = msg ? nlmsg_hdr(msg) : NULL;

	if (nlh && memmem(nlh, nlh->nlmsg_len, t_key, sizeof(t_key)))
		t_keys_freed++;
	__real_nlmsg_free(msg);
}

/**
 * t_setup -connect a client to a socket that plays the kernel
 *
 * Return: 0, or -1 when the sockets cannot be opened.
 */
static int t_setup(void)
{
	memset(&pon, 0, sizeof(pon));
	if (pon_nl_open(&pon.cmd, NETLINK_GENERIC))
		return -1;
	pon.family = T_FAMILY;
	return t_fake_open(&fake, pon.cmd.sock, T_FAMILY, PON_FAMILY_VERSION);
}

/**
 * t_event_cb - keep the last event and count the events
 * @client: the client
 * @event: the event
 */
static void t_event_cb(struct pon *client, const struct pon_event *event)
{
	t_event = *event;
	t_events++;
}

static const struct pon_notify_ops t_ops = {
	.event = t_event_cb,
};

/**
 * genl_ctrl_resolve - resolve every family to the fake's
 * @sock: the socket of the request, unused
 * @name: the name of the family, unused
 *
 * This replaces the function of libnl-tiny in this test, so that
 * pon_open() opens its notification socket without a pon family.
 *
 * Return: T_FAMILY.
 */
int genl_ctrl_resolve(struct nl_sock *sock, const char *name)
{
	return T_FAMILY;
}

/**
 * genl_ctrl_resolve_grp - resolve every multicast group to T_GROUP
 * @sock: the socket of the request, unused
 * @family: the name of the family, unused
 * @name: the name of the group, unused
 *
 * This replaces the function of libnl-tiny in this test. T_GROUP is the
 * group of the nlctrl family, which every kernel has, so the client can
 * join it.
 *
 * Return: T_GROUP.
 */
int genl_ctrl_resolve_grp(struct nl_sock *sock, const char *family,
			  const char *name)
{
	return T_GROUP;
}

/**
 * t_events_open - give the client a socket for notifications
 *
 * The socket is set up as the library sets up its own; the fake sends
 * the notifications to it directly.
 *
 * Return: 0, or -1 when the socket cannot be opened.
 */
static int t_events_open(void)
{
	pon.ops = &t_ops;
	pon.evt = nl_socket_alloc();
	if (!pon.evt || nl_connect(pon.evt, NETLINK_GENERIC))
		return -1;
	nl_socket_set_nonblocking(pon.evt);
	nl_socket_enable_msg_peek(pon.evt);
	return 0;
}

/**
 * t_event_send - send an event notification to the client
 * @type: the enum pon_event_type value
 * @alloc_id: the alloc-id, or -1 to leave it out
 * @depth: the reboot depth, or -1 to leave it out
 * @image: the image, or -1 to leave it out
 */
static void t_event_send(uint32_t type, int alloc_id, int depth, int image)
{
	struct nl_msg *msg = nlmsg_alloc();

	genlmsg_put(msg, 0, 0, T_FAMILY, 0, 0, PON_CMD_EVENT_NTF,
		    PON_FAMILY_VERSION);
	nla_put_u32(msg, PON_A_EVENT_DEV_ID, 0);
	nla_put_u32(msg, PON_A_EVENT_TYPE, type);
	if (alloc_id >= 0)
		nla_put_u32(msg, PON_A_EVENT_ALLOC_ID, alloc_id);
	if (depth >= 0)
		nla_put_u32(msg, PON_A_EVENT_DEPTH, depth);
	if (image >= 0)
		nla_put_u32(msg, PON_A_EVENT_IMAGE, image);
	t_fake_send(&fake, pon.evt, msg);
}

/**
 * t_event_large - an event notification larger than the first receive
 * buffer
 * @alloc_id: the alloc-id of the event
 *
 * Return: the message.
 */
static struct nl_msg *t_event_large(uint32_t alloc_id)
{
	static uint8_t pad[T_LARGE];
	struct nl_msg *msg = nlmsg_alloc_size(2 * T_LARGE);

	genlmsg_put(msg, 0, 0, T_FAMILY, 0, 0, PON_CMD_EVENT_NTF,
		    PON_FAMILY_VERSION);
	nla_put_u32(msg, PON_A_EVENT_DEV_ID, 0);
	nla_put_u32(msg, PON_A_EVENT_TYPE, PON_EVENT_TYPE_TCONT_ALLOC);
	nla_put_u32(msg, PON_A_EVENT_ALLOC_ID, alloc_id);
	nla_put(msg, PON_A_EVENT_MAX + 1, sizeof(pad), pad);
	return msg;
}

/**
 * t_tcont_reply - queue one T-CONT for the client's next request
 * @flags: the netlink flags
 * @index: the T-CONT index
 */
static void t_tcont_reply(int flags, uint32_t index)
{
	struct nl_msg *msg = t_fake_reply(&fake, PON_CMD_TCONT_GET, flags);

	nla_put_u32(msg, PON_A_TCONT_DEV_ID, 0);
	nla_put_u32(msg, PON_A_TCONT_INDEX, index);
	nla_put_u32(msg, PON_A_TCONT_ALLOC_ID, 1024 + index);
	t_fake_send(&fake, pon.cmd.sock, msg);
}

/**
 * t_tcont_cb - count the T-CONTs of a dump
 * @arg: unused
 * @tcont: the T-CONT
 */
static void t_tcont_cb(void *arg, const struct pon_tcont *tcont)
{
	t_objects++;
}

/**
 * runner_get_test - a do request returns the reply and the acknowledgment
 */
static void runner_get_test(void)
{
	struct pon_tcont tcont;

	t_tcont_reply(0, 2);
	t_fake_ack(&fake, 0, NULL);
	CHECK_EQ(pon_tcont_get(&pon, 0, 2, &tcont), 0);
	CHECK_EQ(tcont.index, 2);
	CHECK_EQ(tcont.alloc_id, 1026);
	CHECK(tcont.alloc_id_set);
	CHECK_EQ(t_fake_drain(&fake), 1);
}

/**
 * runner_counters_test - a counter comes as 4 bytes or as 8
 */
static void runner_counters_test(void)
{
	struct nl_msg *msg = t_fake_reply(&fake, PON_CMD_GEM_STATS_GET, 0);
	struct pon_gem_stats stats;

	nla_put_u32(msg, PON_A_GEM_STATS_DEV_ID, 0);
	nla_put_u32(msg, PON_A_GEM_STATS_GEM_ID, 1100);
	nla_put_u32(msg, PON_A_GEM_STATS_RX_FRAMES, 7);
	nla_put_u64(msg, PON_A_GEM_STATS_RX_BYTES, 1ull << 40);
	t_fake_send(&fake, pon.cmd.sock, msg);
	t_fake_ack(&fake, 0, NULL);
	CHECK_EQ(pon_stats_get(&pon, 0, 1100, &stats), 0);
	CHECK_EQ(stats.rx_frames, 7);
	CHECK_EQ(stats.rx_bytes, 1ull << 40);
	t_fake_drain(&fake);
}

/**
 * runner_large_test - a reply larger than the first receive buffer is read
 */
static void runner_large_test(void)
{
	static uint8_t pad[T_LARGE];
	struct nl_msg *msg = nlmsg_alloc_size(2 * T_LARGE);
	struct pon_tcont tcont;

	genlmsg_put(msg, 0, t_fake_seq(&fake), T_FAMILY, 0, 0,
		    PON_CMD_TCONT_GET, PON_FAMILY_VERSION);
	nla_put_u32(msg, PON_A_TCONT_DEV_ID, 0);
	nla_put_u32(msg, PON_A_TCONT_INDEX, 5);
	nla_put(msg, PON_A_TCONT_MAX + 1, sizeof(pad), pad);
	t_fake_send(&fake, pon.cmd.sock, msg);
	t_fake_ack(&fake, 0, NULL);
	CHECK_EQ(pon_tcont_get(&pon, 0, 5, &tcont), 0);
	CHECK_EQ(tcont.index, 5);
	t_fake_drain(&fake);
}

/**
 * runner_get_empty_test - a do request without a reply that decodes fails
 */
static void runner_get_empty_test(void)
{
	struct pon_dev_info dev;
	struct pon_tcont tcont;
	struct nl_msg *msg;

	t_fake_ack(&fake, 0, NULL);
	CHECK_EQ(pon_dev_get(&pon, 0, &dev), -EPROTO);
	t_fake_drain(&fake);

	msg = t_fake_reply(&fake, PON_CMD_TCONT_GET, 0);
	nla_put_u32(msg, PON_A_TCONT_DEV_ID, 0);
	t_fake_send(&fake, pon.cmd.sock, msg);
	t_fake_ack(&fake, 0, NULL);
	CHECK_EQ(pon_tcont_get(&pon, 0, 2, &tcont), -EPROTO);
	CHECK_STR(pon_error(&pon), "the reply held no object");
	t_fake_drain(&fake);
}

/**
 * runner_stale_test - an answer to an earlier request is skipped
 */
static void runner_stale_test(void)
{
	struct nl_msg *msg = nlmsg_alloc_simple(NLMSG_ERROR, 0);
	struct nlmsgerr err = { .error = -EBUSY };
	struct pon_tcont tcont;

	nlmsg_hdr(msg)->nlmsg_seq = t_fake_seq(&fake) - 1;
	err.msg.nlmsg_len = sizeof(struct nlmsghdr);
	err.msg.nlmsg_seq = t_fake_seq(&fake) - 1;
	nlmsg_append(msg, &err, sizeof(err), NLMSG_ALIGNTO);
	t_fake_send(&fake, pon.cmd.sock, msg);
	t_tcont_reply(0, 3);
	t_fake_ack(&fake, 0, NULL);
	CHECK_EQ(pon_tcont_get(&pon, 0, 3, &tcont), 0);
	CHECK_EQ(tcont.index, 3);
	t_fake_drain(&fake);
}

/**
 * runner_refusal_test - a refusal returns its errno and its text
 */
static void runner_refusal_test(void)
{
	struct pon_dev_config cfg = { .dev_id = 0 };

	t_fake_ack(&fake, -EBUSY, "the link is enabled");
	CHECK_EQ(pon_dev_set(&pon, &cfg), -EBUSY);
	CHECK_STR(pon_error(&pon), "the link is enabled");
	CHECK_EQ(pon.cmd.refusals, 1);
	t_fake_drain(&fake);
}

/**
 * runner_dev_set_length_test - an identity longer than its field is refused
 *
 * An acknowledgment waits on the client, so that a request that goes out
 * returns 0 instead of blocking.
 */
static void runner_dev_set_length_test(void)
{
	struct pon_dev_config cfg = {
		.has = PON_DEV_F_REGISTRATION_ID,
		.registration_id_len = PON_REGID_MAX + 1,
	};

	t_fake_ack(&fake, 0, NULL);
	CHECK_EQ(pon_dev_set(&pon, &cfg), -EINVAL);
	CHECK_EQ(t_fake_drain(&fake), 0);
	t_fake_recv(pon.cmd.sock);
}

/**
 * runner_dev_set_disabled_test - only the disabled state 1 goes out
 */
static void runner_dev_set_disabled_test(void)
{
	struct pon_dev_config cfg = {
		.has = PON_DEV_F_DISABLED,
		.disabled = 0,
	};

	t_fake_ack(&fake, 0, NULL);
	CHECK_EQ(pon_dev_set(&pon, &cfg), -EINVAL);
	CHECK_EQ(t_fake_drain(&fake), 0);
	t_fake_recv(pon.cmd.sock);

	cfg.disabled = 1;
	t_fake_ack(&fake, 0, NULL);
	CHECK_EQ(pon_dev_set(&pon, &cfg), 0);
	CHECK_EQ(t_fake_drain(&fake), 1);
}

/**
 * runner_keys_test - the key calls send their command with the key
 *
 * msk-set carries the device and the key; bcast-key-set carries the key
 * index and the key, or no key for a clear. A refusal comes back with
 * its text. No request is freed with the key in it.
 */
static void runner_keys_test(void)
{
	uint8_t *key = t_key;
	struct nlattr *tb[PON_A_DEV_MAX + 1];

	memset(key, 0x5a, PON_KEY_LEN);
	t_keys_freed = 0;
	t_fake_ack(&fake, 0, NULL);
	CHECK_EQ(pon_dev_msk_set(&pon, 3, key), 0);
	CHECK_EQ(t_fake_request(&fake, tb, PON_A_DEV_MAX), PON_CMD_MSK_SET);
	CHECK(tb[PON_A_DEV_ID] && nla_get_u32(tb[PON_A_DEV_ID]) == 3);
	CHECK(tb[PON_A_DEV_MSK] && nla_len(tb[PON_A_DEV_MSK]) == PON_KEY_LEN);
	CHECK(tb[PON_A_DEV_MSK] &&
	      !memcmp(nla_data(tb[PON_A_DEV_MSK]), key, PON_KEY_LEN));

	t_fake_ack(&fake, 0, NULL);
	CHECK_EQ(pon_bcast_key_set(&pon, 3, 2, key), 0);
	CHECK_EQ(t_fake_request(&fake, tb, PON_A_DEV_MAX),
		 PON_CMD_BCAST_KEY_SET);
	CHECK(tb[PON_A_DEV_BCAST_KEY_INDEX] &&
	      nla_get_u32(tb[PON_A_DEV_BCAST_KEY_INDEX]) == 2);
	CHECK(tb[PON_A_DEV_BCAST_KEY] &&
	      nla_len(tb[PON_A_DEV_BCAST_KEY]) == PON_KEY_LEN);

	t_fake_ack(&fake, 0, NULL);
	CHECK_EQ(pon_bcast_key_del(&pon, 3, 1), 0);
	CHECK_EQ(t_fake_request(&fake, tb, PON_A_DEV_MAX),
		 PON_CMD_BCAST_KEY_SET);
	CHECK(tb[PON_A_DEV_BCAST_KEY_INDEX] &&
	      nla_get_u32(tb[PON_A_DEV_BCAST_KEY_INDEX]) == 1);
	CHECK(!tb[PON_A_DEV_BCAST_KEY]);

	t_fake_ack(&fake, -ENETDOWN, "a master session key is taken in O5 only");
	CHECK_EQ(pon_dev_msk_set(&pon, 3, key), -ENETDOWN);
	CHECK_STR(pon_error(&pon), "a master session key is taken in O5 only");
	t_fake_drain(&fake);
	CHECK_EQ(t_keys_freed, 0);
}

/**
 * runner_cap_ack_test - the client asks for capped acknowledgments
 */
static void runner_cap_ack_test(void)
{
	socklen_t len = sizeof(int);
	int on = 0;

	CHECK_EQ(getsockopt(nl_socket_get_fd(pon.cmd.sock), SOL_NETLINK,
			    NETLINK_CAP_ACK, &on, &len), 0);
	CHECK_EQ(on, 1);
}

/**
 * runner_dump_test - a dump calls back for every object up to its end
 */
static void runner_dump_test(void)
{
	t_objects = 0;
	t_tcont_reply(NLM_F_MULTI, 0);
	t_tcont_reply(NLM_F_MULTI, 1);
	t_fake_done(&fake, 0, 0, NULL);
	CHECK_EQ(pon_tcont_dump(&pon, PON_DEV_ANY, t_tcont_cb, NULL), 0);
	CHECK_EQ(t_objects, 2);
	t_fake_drain(&fake);

	t_objects = 0;
	t_tcont_reply(NLM_F_MULTI, 0);
	t_fake_done(&fake, 0, NLM_F_DUMP_INTR, NULL);
	CHECK_EQ(pon_tcont_dump(&pon, 0, t_tcont_cb, NULL), -EINTR);
	t_fake_drain(&fake);
}

/**
 * t_dev_cb - count the devices of a dump
 * @arg: unused
 * @dev: the device
 */
static void t_dev_cb(void *arg, const struct pon_dev_info *dev)
{
	t_objects++;
}

/**
 * t_alarm_cb - count the alarm bitmaps of a dump
 * @arg: unused
 * @alarms: the bitmap
 */
static void t_alarm_cb(void *arg, const struct pon_alarms *alarms)
{
	t_objects++;
}

/**
 * runner_dump_end_test - every dump reports a changed list and the text of
 * its end
 */
static void runner_dump_end_test(void)
{
	t_fake_done(&fake, 0, NLM_F_DUMP_INTR, NULL);
	CHECK_EQ(pon_dev_dump(&pon, t_dev_cb, NULL), -EINTR);
	t_fake_drain(&fake);

	t_fake_done(&fake, 0, NLM_F_DUMP_INTR, NULL);
	CHECK_EQ(pon_alarm_dump(&pon, PON_DEV_ANY, t_alarm_cb, NULL), -EINTR);
	t_fake_drain(&fake);

	t_fake_done(&fake, -ENODEV, 0, "no PON device 7");
	CHECK_EQ(pon_tcont_dump(&pon, 7, t_tcont_cb, NULL), -ENODEV);
	CHECK_STR(pon_error(&pon), "no PON device 7");
	t_fake_drain(&fake);
}

/**
 * runner_events_test - one call handles every notification that waits
 */
static void runner_events_test(void)
{
	t_events = 0;
	t_event_send(PON_EVENT_TYPE_TCONT_ALLOC, 1024, -1, -1);
	t_event_send(PON_EVENT_TYPE_TCONT_DEALLOC, 1025, -1, -1);
	CHECK_EQ(pon_event_process(&pon), 0);
	CHECK_EQ(t_events, 2);
	CHECK_EQ(t_event.type, PON_EVENT_TYPE_TCONT_DEALLOC);
	CHECK_EQ(t_event.alloc_id, 1025);
	CHECK_EQ(pon_event_process(&pon), 0);
	CHECK_EQ(t_events, 2);
}

/**
 * runner_events_large_test - a notification larger than the first receive
 * buffer reaches the socket pon_open() opens whole
 */
static void runner_events_large_test(void)
{
	struct pon listener;

	CHECK_EQ(pon_open(&listener, &t_ops, NULL), 0);
	if (!listener.evt)
		return;
	t_events = 0;
	t_fake_send(&fake, listener.evt, t_event_large(1030));
	CHECK_EQ(pon_event_process(&listener), 0);
	CHECK_EQ(t_events, 1);
	CHECK_EQ(t_event.alloc_id, 1030);
	pon_close(&listener);
}

/**
 * runner_events_overrun_test - an overrun of the notification socket
 * returns -ENOBUFS once; the notifications that fit are read after it
 *
 * The fake sends to T_GROUP, which the socket of pon_open() joined: the
 * kernel marks an overrun only for a multicast.
 */
static void runner_events_overrun_test(void)
{
	struct pon listener;
	socklen_t len;
	unsigned int i, count;
	int rcvbuf = 0;
	struct nl_msg *msg;

	CHECK_EQ(pon_open(&listener, &t_ops, NULL), 0);
	if (!listener.evt)
		return;
	len = sizeof(rcvbuf);
	getsockopt(pon_event_fd(&listener), SOL_SOCKET, SO_RCVBUF, &rcvbuf,
		   &len);
	count = rcvbuf / T_LARGE + T_OVERRUN_EXTRA;
	nl_socket_set_peer_port(fake.kernel, 0);
	nl_socket_set_peer_groups(fake.kernel, 1u << (T_GROUP - 1));
	for (i = 0; i < count; i++) {
		msg = t_event_large(1024 + i);
		nl_send(fake.kernel, msg);
		nlmsg_free(msg);
	}
	nl_socket_set_peer_groups(fake.kernel, 0);
	t_events = 0;
	CHECK_EQ(pon_event_process(&listener), -ENOBUFS);
	CHECK_EQ(pon_event_process(&listener), 0);
	CHECK(t_events > 0 && t_events < count);
	pon_close(&listener);
}

/**
 * main - run the cases that need a socket in the role of the kernel
 *
 * Return: 0 when every check passed, else 1.
 */
int main(void)
{
	if (t_setup() || t_events_open()) {
		fprintf(stderr, "test-runner: no netlink sockets\n");
		return 1;
	}
	runner_get_test();
	runner_get_empty_test();
	runner_large_test();
	runner_counters_test();
	runner_refusal_test();
	runner_stale_test();
	runner_dev_set_length_test();
	runner_dev_set_disabled_test();
	runner_keys_test();
	runner_cap_ack_test();
	runner_dump_test();
	runner_dump_end_test();
	runner_events_test();
	runner_events_large_test();
	runner_events_overrun_test();
	pon_close(&pon);
	t_fake_close(&fake);
	return t_report("test-runner");
}
