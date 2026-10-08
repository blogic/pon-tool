// SPDX-License-Identifier: LGPL-2.1-only
/* Copyright (C) 2026 John Crispin <john@phrozen.org> */
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <netlink/netlink.h>
#include <netlink/genl/genl.h>

#include "internal.h"

/**
 * typedef one_decode - decode the reply to a do request that reads one object
 * @msg: the reply
 * @out: the object to fill
 *
 * Return: 0, or -1 when the reply is not one.
 */
typedef int (*one_decode)(struct nl_msg *msg, void *out);

/**
 * struct dump - the state of a dump
 * @dev_id: the device, or PON_DEV_ANY for every device
 * @cb: the callback of the caller, of the type of the dump
 * @arg: passed to @cb
 */
struct dump {
	uint32_t dev_id;
	void *cb;
	void *arg;
};

/**
 * struct one - the state of a do request that reads one object
 * @decode: decodes the reply into @out
 * @out: the object
 * @found: a reply decoded
 */
struct one {
	one_decode decode;
	void *out;
	bool found;
};

#define POLICY_U8	{ .type = NLA_U8, .maxlen = sizeof(uint8_t) }
#define POLICY_U32	{ .type = NLA_U32, .maxlen = sizeof(uint32_t) }
#define POLICY_UINT	{ .type = NLA_U32, .maxlen = sizeof(uint64_t) }

static const struct nla_policy dev_policy[PON_A_DEV_MAX + 1] = {
	[PON_A_DEV_ID] = POLICY_U32,
	[PON_A_DEV_IFINDEX] = POLICY_U32,
	[PON_A_DEV_MODE] = POLICY_U32,
	[PON_A_DEV_MODES_CAP] = POLICY_U32,
	[PON_A_DEV_SERIAL] = { .minlen = PON_SERIAL_LEN,
			       .maxlen = PON_SERIAL_LEN },
	[PON_A_DEV_PLOAM_STATE] = POLICY_U32,
	[PON_A_DEV_MAX_TCONTS] = POLICY_U32,
	[PON_A_DEV_MAX_GEMS] = POLICY_U32,
	[PON_A_DEV_RESPONSE_TIME] = POLICY_U32,
	[PON_A_DEV_ENABLE] = POLICY_U8,
	[PON_A_DEV_DISABLED] = { .type = NLA_FLAG },
};

static const struct nla_policy tcont_policy[PON_A_TCONT_MAX + 1] = {
	[PON_A_TCONT_DEV_ID] = POLICY_U32,
	[PON_A_TCONT_INDEX] = POLICY_U32,
	[PON_A_TCONT_ALLOC_ID] = POLICY_U32,
};

static const struct nla_policy gem_policy[PON_A_GEM_MAX + 1] = {
	[PON_A_GEM_DEV_ID] = POLICY_U32,
	[PON_A_GEM_ID] = POLICY_U32,
	[PON_A_GEM_DIR] = POLICY_U32,
	[PON_A_GEM_TCONT_INDEX] = POLICY_U32,
	[PON_A_GEM_KEY_RING] = POLICY_U32,
	[PON_A_GEM_QUEUE] = POLICY_U32,
	[PON_A_GEM_NO_OFFLOAD] = POLICY_U8,
};

static const struct nla_policy gem_map_policy[PON_A_GEM_MAP_MAX + 1] = {
	[PON_A_GEM_MAP_DEV_ID] = POLICY_U32,
	[PON_A_GEM_MAP_GEM_ID] = POLICY_U32,
	[PON_A_GEM_MAP_TAG] = POLICY_U32,
	[PON_A_GEM_MAP_VID] = POLICY_U32,
	[PON_A_GEM_MAP_PBIT] = POLICY_U32,
	[PON_A_GEM_MAP_DSCP] = POLICY_U32,
};

static const struct nla_policy alarm_policy[PON_A_ALARM_MAX + 1] = {
	[PON_A_ALARM_DEV_ID] = POLICY_U32,
	[PON_A_ALARM_ALARMS] = POLICY_U32,
};

static const struct nla_policy event_policy[PON_A_EVENT_MAX + 1] = {
	[PON_A_EVENT_DEV_ID] = POLICY_U32,
	[PON_A_EVENT_TYPE] = POLICY_U32,
	[PON_A_EVENT_ALLOC_ID] = POLICY_U32,
	[PON_A_EVENT_DEPTH] = POLICY_U32,
	[PON_A_EVENT_IMAGE] = POLICY_U32,
	[PON_A_EVENT_CALLS] = POLICY_U32,
};

static const struct nla_policy stats_policy[PON_A_GEM_STATS_MAX + 1] = {
	[PON_A_GEM_STATS_DEV_ID] = POLICY_U32,
	[PON_A_GEM_STATS_GEM_ID] = POLICY_U32,
	[PON_A_GEM_STATS_RX_FRAMES] = POLICY_UINT,
	[PON_A_GEM_STATS_RX_BYTES] = POLICY_UINT,
	[PON_A_GEM_STATS_TX_FRAMES] = POLICY_UINT,
	[PON_A_GEM_STATS_TX_BYTES] = POLICY_UINT,
};

static const struct nla_policy fec_policy[PON_A_FEC_MAX + 1] = {
	[PON_A_FEC_DEV_ID] = POLICY_U32,
	[PON_A_FEC_CORRECTED] = POLICY_UINT,
	[PON_A_FEC_UNCORRECTABLE] = POLICY_UINT,
	[PON_A_FEC_CORRECTED_BYTES] = POLICY_UINT,
	[PON_A_FEC_TOTAL_CODEWORDS] = POLICY_UINT,
	[PON_A_FEC_SECONDS] = POLICY_UINT,
};

static const struct nla_policy tc_stats_policy[PON_A_TC_STATS_MAX + 1] = {
	[PON_A_TC_STATS_DEV_ID] = POLICY_U32,
	[PON_A_TC_STATS_DEV_ID + 1 ... PON_A_TC_STATS_MAX] = POLICY_UINT,
};

/**
 * lp_msg_new - a request of the family with its generic header
 * @pon: the client
 * @cmd: the command
 * @flags: the netlink flags, NLM_F_DUMP for a dump
 *
 * Return: the message, or NULL when out of memory; pon_error() then says
 * so.
 */
struct nl_msg *lp_msg_new(struct pon *pon, int cmd, int flags)
{
	struct nl_msg *msg = nlmsg_alloc();

	if (!msg) {
		lp_nomem(&pon->cmd);
		return NULL;
	}
	if (!genlmsg_put(msg, 0, NL_AUTO_SEQ, pon->family, 0, flags, cmd,
			 PON_FAMILY_VERSION)) {
		nlmsg_free(msg);
		lp_nomem(&pon->cmd);
		return NULL;
	}
	return msg;
}

/**
 * attrs_parse - index the attributes of a message of the family
 * @msg: the message
 * @tb: the index, @max + 1 entries
 * @max: the highest attribute of the set
 * @policy: the size of each attribute of the set
 *
 * Return: 0, or a negative libnl-tiny code for a message shorter than
 * its header or an attribute of the wrong size.
 */
static int attrs_parse(struct nl_msg *msg, struct nlattr **tb, int max,
		       const struct nla_policy *policy)
{
	return genlmsg_parse(nlmsg_hdr(msg), 0, tb, max, policy);
}

/**
 * attr_uint - the value of a counter attribute
 * @attr: the attribute, or NULL
 *
 * The kernel sends a counter in 4 bytes or in 8, as its value needs.
 *
 * Return: the value, or 0 for an absent attribute.
 */
static uint64_t attr_uint(struct nlattr *attr)
{
	if (!attr)
		return 0;
	if (nla_len(attr) == sizeof(uint64_t))
		return nla_get_u64(attr);
	return nla_get_u32(attr);
}

/**
 * attr_int - the value of an optional u32 attribute
 * @attr: the attribute, or NULL
 *
 * Return: the value, or -1 for an absent attribute.
 */
static int attr_int(struct nlattr *attr)
{
	return attr ? (int)nla_get_u32(attr) : -1;
}

/**
 * dev_match - whether an object of a dump is of the device asked for
 * @dump: the dump
 * @dev_id: the device of the object
 *
 * Return: true when the dump asked for that device or for every one.
 */
static bool dev_match(const struct dump *dump, uint32_t dev_id)
{
	return dump->dev_id == PON_DEV_ANY || dump->dev_id == dev_id;
}

/**
 * dev_u32 - read an optional u32 attribute and note that it was there
 * @tb: the attributes
 * @type: the attribute
 * @flag: the flag that says the attribute was there
 * @has: the flags, @flag added when the attribute is there
 * @value: the value, written when the attribute is there
 */
static void dev_u32(struct nlattr **tb, int type, uint32_t flag, uint32_t *has,
		    uint32_t *value)
{
	if (!tb[type])
		return;
	*value = nla_get_u32(tb[type]);
	*has |= flag;
}

/**
 * dev_u8 - read an optional u8 attribute and note that it was there
 * @tb: the attributes
 * @type: the attribute
 * @flag: the flag that says the attribute was there
 * @has: the flags, @flag added when the attribute is there
 * @value: the value, written when the attribute is there
 */
static void dev_u8(struct nlattr **tb, int type, uint32_t flag, uint32_t *has,
		   uint32_t *value)
{
	if (!tb[type])
		return;
	*value = nla_get_u8(tb[type]);
	*has |= flag;
}

/**
 * lp_dev_decode - decode a device reply or notification
 * @msg: the message
 * @out: the device, overwritten; a flag of @out->has says that the kernel
 *	 sent the attribute of that member
 *
 * Return: 0, or -1 when the message lacks the device or an attribute has
 * the wrong size.
 */
int lp_dev_decode(struct nl_msg *msg, struct pon_dev_info *out)
{
	struct nlattr *tb[PON_A_DEV_MAX + 1];

	memset(out, 0, sizeof(*out));
	if (attrs_parse(msg, tb, PON_A_DEV_MAX, dev_policy) ||
	    !tb[PON_A_DEV_ID])
		return -1;
	out->dev_id = nla_get_u32(tb[PON_A_DEV_ID]);
	dev_u32(tb, PON_A_DEV_IFINDEX, PON_DEV_F_IFINDEX, &out->has,
		&out->ifindex);
	dev_u32(tb, PON_A_DEV_MODE, PON_DEV_F_MODE, &out->has, &out->mode);
	dev_u32(tb, PON_A_DEV_MODES_CAP, PON_DEV_F_MODES_CAP, &out->has,
		&out->modes_cap);
	dev_u32(tb, PON_A_DEV_MAX_TCONTS, PON_DEV_F_MAX_TCONTS, &out->has,
		&out->max_tconts);
	dev_u32(tb, PON_A_DEV_MAX_GEMS, PON_DEV_F_MAX_GEMS, &out->has,
		&out->max_gems);
	dev_u32(tb, PON_A_DEV_PLOAM_STATE, PON_DEV_F_PLOAM, &out->has,
		&out->ploam);
	dev_u32(tb, PON_A_DEV_RESPONSE_TIME, PON_DEV_F_RESPONSE_TIME, &out->has,
		&out->response_time);
	dev_u8(tb, PON_A_DEV_ENABLE, PON_DEV_F_ENABLE, &out->has, &out->enable);
	if (tb[PON_A_DEV_DISABLED]) {
		out->disabled = 1;
		out->has |= PON_DEV_F_DISABLED;
	}
	if (tb[PON_A_DEV_SERIAL] &&
	    nla_len(tb[PON_A_DEV_SERIAL]) == PON_SERIAL_LEN) {
		memcpy(out->serial, nla_data(tb[PON_A_DEV_SERIAL]),
		       PON_SERIAL_LEN);
		out->has |= PON_DEV_F_SERIAL;
	}
	return 0;
}

/**
 * lp_tcont_decode - decode a T-CONT reply or notification
 * @msg: the message
 * @out: the T-CONT, overwritten
 *
 * Return: 0, or -1 when the message lacks the device or the index, or an
 * attribute has the wrong size.
 */
int lp_tcont_decode(struct nl_msg *msg, struct pon_tcont *out)
{
	struct nlattr *tb[PON_A_TCONT_MAX + 1];

	memset(out, 0, sizeof(*out));
	if (attrs_parse(msg, tb, PON_A_TCONT_MAX, tcont_policy) ||
	    !tb[PON_A_TCONT_DEV_ID] || !tb[PON_A_TCONT_INDEX])
		return -1;
	out->dev_id = nla_get_u32(tb[PON_A_TCONT_DEV_ID]);
	out->index = nla_get_u32(tb[PON_A_TCONT_INDEX]);
	if (tb[PON_A_TCONT_ALLOC_ID]) {
		out->alloc_id = nla_get_u32(tb[PON_A_TCONT_ALLOC_ID]);
		out->alloc_id_set = true;
	}
	return 0;
}

/**
 * lp_gem_decode - decode a GEM port reply or notification
 * @msg: the message
 * @out: the port, overwritten; an absent optional attribute reads -1
 *
 * Return: 0, or -1 when the message lacks the device or the port, or an
 * attribute has the wrong size.
 */
int lp_gem_decode(struct nl_msg *msg, struct pon_gem *out)
{
	struct nlattr *tb[PON_A_GEM_MAX + 1];

	memset(out, 0, sizeof(*out));
	if (attrs_parse(msg, tb, PON_A_GEM_MAX, gem_policy) ||
	    !tb[PON_A_GEM_DEV_ID] || !tb[PON_A_GEM_ID])
		return -1;
	out->dev_id = nla_get_u32(tb[PON_A_GEM_DEV_ID]);
	out->id = nla_get_u32(tb[PON_A_GEM_ID]);
	out->dir = tb[PON_A_GEM_DIR] ? nla_get_u32(tb[PON_A_GEM_DIR]) : 0;
	out->tcont_index = attr_int(tb[PON_A_GEM_TCONT_INDEX]);
	out->key_ring = attr_int(tb[PON_A_GEM_KEY_RING]);
	out->queue = attr_int(tb[PON_A_GEM_QUEUE]);
	out->no_offload = tb[PON_A_GEM_NO_OFFLOAD] ?
			  nla_get_u8(tb[PON_A_GEM_NO_OFFLOAD]) : -1;
	return 0;
}

/**
 * lp_gem_map_decode - decode a classifier rule reply or notification
 * @msg: the message
 * @out: the rule, overwritten; an absent match field reads -1
 *
 * Return: 0, or -1 when the message lacks the device or the port, or an
 * attribute has the wrong size.
 */
int lp_gem_map_decode(struct nl_msg *msg, struct pon_gem_map *out)
{
	struct nlattr *tb[PON_A_GEM_MAP_MAX + 1];

	memset(out, 0, sizeof(*out));
	if (attrs_parse(msg, tb, PON_A_GEM_MAP_MAX, gem_map_policy) ||
	    !tb[PON_A_GEM_MAP_DEV_ID] || !tb[PON_A_GEM_MAP_GEM_ID])
		return -1;
	out->dev_id = nla_get_u32(tb[PON_A_GEM_MAP_DEV_ID]);
	out->gem_id = nla_get_u32(tb[PON_A_GEM_MAP_GEM_ID]);
	out->tag = attr_int(tb[PON_A_GEM_MAP_TAG]);
	out->vid = attr_int(tb[PON_A_GEM_MAP_VID]);
	out->pbit = attr_int(tb[PON_A_GEM_MAP_PBIT]);
	out->dscp = attr_int(tb[PON_A_GEM_MAP_DSCP]);
	return 0;
}

/**
 * lp_alarm_decode - decode an alarm reply or notification
 * @msg: the message
 * @out: the alarm bitmap, overwritten
 *
 * Return: 0, or -1 when the message lacks the device or an attribute has
 * the wrong size.
 */
int lp_alarm_decode(struct nl_msg *msg, struct pon_alarms *out)
{
	struct nlattr *tb[PON_A_ALARM_MAX + 1];

	memset(out, 0, sizeof(*out));
	if (attrs_parse(msg, tb, PON_A_ALARM_MAX, alarm_policy) ||
	    !tb[PON_A_ALARM_DEV_ID])
		return -1;
	out->dev_id = nla_get_u32(tb[PON_A_ALARM_DEV_ID]);
	out->alarms = tb[PON_A_ALARM_ALARMS] ?
		      nla_get_u32(tb[PON_A_ALARM_ALARMS]) : 0;
	return 0;
}

/**
 * lp_event_decode - decode an event notification
 * @msg: the notification
 * @out: the event, overwritten; a flag of @out->has says that the kernel
 *	 sent the attribute of that member
 *
 * Return: 0, or -1 when the notification lacks the device or the type, or
 * an attribute has the wrong size.
 */
int lp_event_decode(struct nl_msg *msg, struct pon_event *out)
{
	struct nlattr *tb[PON_A_EVENT_MAX + 1];

	memset(out, 0, sizeof(*out));
	if (attrs_parse(msg, tb, PON_A_EVENT_MAX, event_policy) ||
	    !tb[PON_A_EVENT_DEV_ID] || !tb[PON_A_EVENT_TYPE])
		return -1;
	out->dev_id = nla_get_u32(tb[PON_A_EVENT_DEV_ID]);
	out->type = nla_get_u32(tb[PON_A_EVENT_TYPE]);
	dev_u32(tb, PON_A_EVENT_ALLOC_ID, PON_EVENT_F_ALLOC_ID, &out->has,
		&out->alloc_id);
	dev_u32(tb, PON_A_EVENT_DEPTH, PON_EVENT_F_DEPTH, &out->has,
		&out->depth);
	dev_u32(tb, PON_A_EVENT_IMAGE, PON_EVENT_F_IMAGE, &out->has,
		&out->image);
	dev_u32(tb, PON_A_EVENT_CALLS, PON_EVENT_F_CALLS, &out->has,
		&out->calls);
	return 0;
}

/**
 * stats_decode - decode the counters of a GEM port
 * @msg: the reply
 * @out: the counters, overwritten
 *
 * Return: 0, or -1 when the reply lacks the device or the port, or an
 * attribute has the wrong size.
 */
static int stats_decode(struct nl_msg *msg, struct pon_gem_stats *out)
{
	struct nlattr *tb[PON_A_GEM_STATS_MAX + 1];

	memset(out, 0, sizeof(*out));
	if (attrs_parse(msg, tb, PON_A_GEM_STATS_MAX, stats_policy) ||
	    !tb[PON_A_GEM_STATS_DEV_ID] || !tb[PON_A_GEM_STATS_GEM_ID])
		return -1;
	out->dev_id = nla_get_u32(tb[PON_A_GEM_STATS_DEV_ID]);
	out->gem_id = nla_get_u32(tb[PON_A_GEM_STATS_GEM_ID]);
	out->rx_frames = attr_uint(tb[PON_A_GEM_STATS_RX_FRAMES]);
	out->rx_bytes = attr_uint(tb[PON_A_GEM_STATS_RX_BYTES]);
	out->tx_frames = attr_uint(tb[PON_A_GEM_STATS_TX_FRAMES]);
	out->tx_bytes = attr_uint(tb[PON_A_GEM_STATS_TX_BYTES]);
	return 0;
}

/**
 * fec_decode - decode the FEC counters of a device
 * @msg: the reply
 * @out: the counters, overwritten
 *
 * Return: 0, or -1 when the reply lacks the device or an attribute has the
 * wrong size.
 */
static int fec_decode(struct nl_msg *msg, struct pon_fec_stats *out)
{
	struct nlattr *tb[PON_A_FEC_MAX + 1];

	memset(out, 0, sizeof(*out));
	if (attrs_parse(msg, tb, PON_A_FEC_MAX, fec_policy) ||
	    !tb[PON_A_FEC_DEV_ID])
		return -1;
	out->dev_id = nla_get_u32(tb[PON_A_FEC_DEV_ID]);
	out->corrected = attr_uint(tb[PON_A_FEC_CORRECTED]);
	out->uncorrectable = attr_uint(tb[PON_A_FEC_UNCORRECTABLE]);
	out->corrected_bytes = attr_uint(tb[PON_A_FEC_CORRECTED_BYTES]);
	out->total_codewords = attr_uint(tb[PON_A_FEC_TOTAL_CODEWORDS]);
	out->seconds = attr_uint(tb[PON_A_FEC_SECONDS]);
	return 0;
}

/**
 * tc_stats_decode - decode the counters of the TC layer of a device
 * @msg: the reply
 * @out: the counters, overwritten
 *
 * Return: 0, or -1 when the reply lacks the device or an attribute has the
 * wrong size.
 */
static int tc_stats_decode(struct nl_msg *msg, struct pon_tc_stats *out)
{
	struct nlattr *tb[PON_A_TC_STATS_MAX + 1];
	unsigned int counter;

	memset(out, 0, sizeof(*out));
	if (attrs_parse(msg, tb, PON_A_TC_STATS_MAX, tc_stats_policy) ||
	    !tb[PON_A_TC_STATS_DEV_ID])
		return -1;
	out->dev_id = nla_get_u32(tb[PON_A_TC_STATS_DEV_ID]);
	for (counter = PON_A_TC_STATS_DEV_ID + 1;
	     counter <= PON_A_TC_STATS_MAX; counter++) {
		if (!tb[counter])
			continue;
		out->counter[counter] = attr_uint(tb[counter]);
		out->has |= 1ull << counter;
	}
	return 0;
}

/**
 * run - send a request of the family
 * @pon: the client
 * @msg: the request, or NULL when it could not be allocated; consumed
 * @show: called for every reply message, or NULL
 * @arg: passed to @show
 *
 * Return: 0, or a negative errno.
 */
static int run(struct pon *pon, struct nl_msg *msg, pon_nl_show show, void *arg)
{
	if (!msg)
		return -ENOMEM;
	return pon_nl_run(&pon->cmd, msg, show, arg);
}

/**
 * refuse - fail a request without sending it
 * @pon: the client
 * @err: the negative errno
 * @why: the text pon_error() gives
 *
 * Return: @err.
 */
static int refuse(struct pon *pon, int err, const char *why)
{
	return lp_refuse(&pon->cmd, err, why);
}

/**
 * put_failed - fail a request that does not fit in its message
 * @pon: the client
 * @msg: the request; freed
 *
 * Return: -EMSGSIZE.
 */
static int put_failed(struct pon *pon, struct nl_msg *msg)
{
	return lp_put_failed(&pon->cmd, msg);
}

/**
 * dump_run - run a dump of one device or of every device
 * @pon: the client
 * @cmd: the command of the dump
 * @dev_attr: the attribute that names the device in the request, or 0
 *	      for a dump that takes no device and lists every one
 * @dump: the device, the callback and its argument
 * @show: decodes each reply and calls back
 *
 * Return: 0, a negative errno, or -EINTR when the list changed during the
 * dump.
 */
static int dump_run(struct pon *pon, int cmd, int dev_attr, struct dump *dump,
		    pon_nl_show show)
{
	struct nl_msg *msg = lp_msg_new(pon, cmd, NLM_F_DUMP);
	int err;

	if (!msg)
		return -ENOMEM;
	if (dev_attr && dump->dev_id != PON_DEV_ANY &&
	    nla_put_u32(msg, dev_attr, dump->dev_id))
		return put_failed(pon, msg);
	err = pon_nl_run(&pon->cmd, msg, show, dump);
	if (err || !pon->cmd.interrupted)
		return err;
	return refuse(pon, -EINTR, "the list changed during the dump");
}

/**
 * one_show - decode the reply to a do request that reads one object
 * @msg: the reply
 * @arg: the struct one of the request
 *
 * Return: NL_OK, or NL_SKIP for a reply that does not decode.
 */
static int one_show(struct nl_msg *msg, void *arg)
{
	struct one *one = arg;

	if (one->decode(msg, one->out))
		return NL_SKIP;
	one->found = true;
	return NL_OK;
}

/**
 * get_run - run a do request that reads one object
 * @pon: the client
 * @msg: the request; consumed
 * @decode: decodes the reply into @out
 * @out: the object
 *
 * Return: 0, a negative errno, or -EPROTO when the kernel acknowledged the
 * request without a reply that decodes.
 */
static int get_run(struct pon *pon, struct nl_msg *msg, one_decode decode,
		   void *out)
{
	struct one one = { .decode = decode, .out = out };
	int err;

	err = run(pon, msg, one_show, &one);
	if (err || one.found)
		return err;
	return refuse(pon, -EPROTO, "the reply held no object");
}

/**
 * dev_show - decode one device of a dump and call back
 * @msg: the reply
 * @arg: the struct dump of the request
 *
 * Return: NL_OK, or NL_SKIP for a reply that does not decode or is of
 * another device.
 */
static int dev_show(struct nl_msg *msg, void *arg)
{
	struct dump *dump = arg;
	struct pon_dev_info dev;

	if (lp_dev_decode(msg, &dev) || !dev_match(dump, dev.dev_id))
		return NL_SKIP;
	((pon_dev_cb)dump->cb)(dump->arg, &dev);
	return NL_OK;
}

/**
 * dev_one - decode the reply to a get of a device
 * @msg: the reply
 * @out: the struct pon_dev_info to fill
 *
 * Return: 0, or -1 when the reply is not one.
 */
static int dev_one(struct nl_msg *msg, void *out)
{
	return lp_dev_decode(msg, out);
}

/**
 * pon_dev_dump - list the PON devices
 * @pon: the client
 * @cb: called for every device
 * @arg: passed to @cb
 *
 * Return: 0, or a negative errno; -EINTR when the list changed during the
 * dump and @cb saw a device twice or missed one.
 */
int pon_dev_dump(struct pon *pon, pon_dev_cb cb, void *arg)
{
	struct dump dump = { PON_DEV_ANY, (void *)cb, arg };

	return dump_run(pon, PON_CMD_DEV_GET, 0, &dump, dev_show);
}

/**
 * pon_dev_get - read one PON device
 * @pon: the client
 * @dev_id: the device
 * @out: the device, overwritten
 *
 * Return: 0, or a negative errno; -EPROTO when the kernel sent no reply
 * that decodes.
 */
int pon_dev_get(struct pon *pon, uint32_t dev_id, struct pon_dev_info *out)
{
	struct nl_msg *msg = lp_msg_new(pon, PON_CMD_DEV_GET, 0);

	memset(out, 0, sizeof(*out));
	if (!msg)
		return -ENOMEM;
	if (nla_put_u32(msg, PON_A_DEV_ID, dev_id))
		return put_failed(pon, msg);
	return get_run(pon, msg, dev_one, out);
}

/**
 * pon_dev_set - change the settings of a PON device
 * @pon: the client
 * @cfg: the device and the settings, each one sent only when its
 *	 PON_DEV_F_ flag is set in @cfg->has
 *
 * The kernel applies the identity and the disabled state before it moves
 * the link, so one message may carry them and enable. Enable restarts
 * activation whatever the state. The kernel takes the disabled state for a
 * link that is not enabled only. The disabled state is a flag: only 1 can
 * be sent. The kernel refuses a serial number or a mode that differs from
 * the one in use while the link is enabled.
 *
 * Return: 0, or a negative errno; -EINVAL for a registration ID longer
 * than PON_REGID_MAX or the disabled state 0, -EBUSY for the disabled
 * state, a changed serial number or a changed mode while the link is
 * enabled.
 */
int pon_dev_set(struct pon *pon, const struct pon_dev_config *cfg)
{
	struct nl_msg *msg;
	int err;

	if ((cfg->has & PON_DEV_F_REGISTRATION_ID) &&
	    cfg->registration_id_len > PON_REGID_MAX)
		return refuse(pon, -EINVAL, "the registration ID is too long");
	if ((cfg->has & PON_DEV_F_DISABLED) && cfg->disabled != 1)
		return refuse(pon, -EINVAL, "the disabled state takes 1 only");
	msg = lp_msg_new(pon, PON_CMD_DEV_SET, 0);
	if (!msg)
		return -ENOMEM;
	err = nla_put_u32(msg, PON_A_DEV_ID, cfg->dev_id);
	if (!err && (cfg->has & PON_DEV_F_MODE))
		err = nla_put_u32(msg, PON_A_DEV_MODE, cfg->mode);
	if (!err && (cfg->has & PON_DEV_F_SERIAL))
		err = nla_put(msg, PON_A_DEV_SERIAL, PON_SERIAL_LEN,
			      cfg->serial);
	if (!err && (cfg->has & PON_DEV_F_REGISTRATION_ID))
		err = nla_put(msg, PON_A_DEV_REGISTRATION_ID,
			      cfg->registration_id_len, cfg->registration_id);
	if (!err && (cfg->has & PON_DEV_F_DISABLED))
		err = nla_put_flag(msg, PON_A_DEV_DISABLED);
	if (!err && (cfg->has & PON_DEV_F_ENABLE))
		err = nla_put_u8(msg, PON_A_DEV_ENABLE, cfg->enable);
	if (err)
		return put_failed(pon, msg);
	return run(pon, msg, NULL, NULL);
}

/**
 * pon_dev_msk_set - hand the master session key to a PON device
 * @pon: the client
 * @dev_id: the device
 * @msk: the PON_KEY_LEN bytes of the key
 *
 * Sends msk-set. The PON MAC derives its integrity keys and its KEK from
 * the master session key of an OMCI authentication (ITU-T G.988 clause
 * 9.13.11). No reply carries the key back.
 *
 * Return: 0, or a negative errno; -ENETDOWN outside O5, -EOPNOTSUPP for a
 * device that takes no key.
 */
int pon_dev_msk_set(struct pon *pon, uint32_t dev_id, const uint8_t *msk)
{
	struct nl_msg *msg = lp_msg_new(pon, PON_CMD_MSK_SET, 0);

	if (!msg)
		return -ENOMEM;
	if (nla_put_u32(msg, PON_A_DEV_ID, dev_id) ||
	    nla_put(msg, PON_A_DEV_MSK, PON_KEY_LEN, msk))
		return put_failed(pon, msg);
	return run(pon, msg, NULL, NULL);
}

/**
 * bcast_key_send - send bcast-key-set with or without a key
 * @pon: the client
 * @dev_id: the device
 * @index: the key index, 1 or 2
 * @key: the PON_KEY_LEN bytes, or NULL to clear the index
 *
 * Return: 0, or a negative errno.
 */
static int bcast_key_send(struct pon *pon, uint32_t dev_id, uint32_t index,
			  const uint8_t *key)
{
	struct nl_msg *msg = lp_msg_new(pon, PON_CMD_BCAST_KEY_SET, 0);

	if (!msg)
		return -ENOMEM;
	if (nla_put_u32(msg, PON_A_DEV_ID, dev_id) ||
	    nla_put_u32(msg, PON_A_DEV_BCAST_KEY_INDEX, index) ||
	    (key && nla_put(msg, PON_A_DEV_BCAST_KEY, PON_KEY_LEN, key)))
		return put_failed(pon, msg);
	return run(pon, msg, NULL, NULL);
}

/**
 * pon_bcast_key_set - hand a broadcast key of a key index to a PON device
 * @pon: the client
 * @dev_id: the device
 * @index: the key index of the multicast GEM frames, 1 or 2
 * @key: the PON_KEY_LEN bytes of the key, encrypted with the KEK as the
 *	 OLT wrote it
 *
 * Return: 0, or a negative errno; -ENETDOWN outside O5, -EOPNOTSUPP for a
 * device that takes no broadcast key, -ERANGE for another index.
 */
int pon_bcast_key_set(struct pon *pon, uint32_t dev_id, uint32_t index,
		      const uint8_t *key)
{
	return bcast_key_send(pon, dev_id, index, key);
}

/**
 * pon_bcast_key_del - clear the broadcast key of a key index
 * @pon: the client
 * @dev_id: the device
 * @index: the key index, 1 or 2
 *
 * Return: 0, or a negative errno; -EOPNOTSUPP for a device that takes no
 * broadcast key, -ERANGE for another index.
 */
int pon_bcast_key_del(struct pon *pon, uint32_t dev_id, uint32_t index)
{
	return bcast_key_send(pon, dev_id, index, NULL);
}

/**
 * pon_tcont_set - bind a T-CONT index to an alloc-id
 * @pon: the client
 * @dev_id: the device
 * @index: the T-CONT index
 * @alloc_id: the alloc-id
 *
 * Return: 0, or a negative errno.
 */
int pon_tcont_set(struct pon *pon, uint32_t dev_id, uint32_t index,
		  uint32_t alloc_id)
{
	struct nl_msg *msg = lp_msg_new(pon, PON_CMD_TCONT_SET, 0);

	if (!msg)
		return -ENOMEM;
	if (nla_put_u32(msg, PON_A_TCONT_DEV_ID, dev_id) ||
	    nla_put_u32(msg, PON_A_TCONT_INDEX, index) ||
	    nla_put_u32(msg, PON_A_TCONT_ALLOC_ID, alloc_id))
		return put_failed(pon, msg);
	return run(pon, msg, NULL, NULL);
}

/**
 * pon_tcont_del - delete a T-CONT
 * @pon: the client
 * @dev_id: the device
 * @index: the T-CONT index
 *
 * Return: 0, or a negative errno; -ENOENT when it does not exist.
 */
int pon_tcont_del(struct pon *pon, uint32_t dev_id, uint32_t index)
{
	struct nl_msg *msg = lp_msg_new(pon, PON_CMD_TCONT_DEL, 0);

	if (!msg)
		return -ENOMEM;
	if (nla_put_u32(msg, PON_A_TCONT_DEV_ID, dev_id) ||
	    nla_put_u32(msg, PON_A_TCONT_INDEX, index))
		return put_failed(pon, msg);
	return run(pon, msg, NULL, NULL);
}

/**
 * tcont_one - decode the reply to a get of a T-CONT
 * @msg: the reply
 * @out: the struct pon_tcont to fill
 *
 * Return: 0, or -1 when the reply is not one.
 */
static int tcont_one(struct nl_msg *msg, void *out)
{
	return lp_tcont_decode(msg, out);
}

/**
 * pon_tcont_get - read one T-CONT
 * @pon: the client
 * @dev_id: the device
 * @index: the T-CONT index
 * @out: the T-CONT, overwritten
 *
 * Return: 0, or a negative errno; -EPROTO when the kernel sent no reply
 * that decodes.
 */
int pon_tcont_get(struct pon *pon, uint32_t dev_id, uint32_t index,
		  struct pon_tcont *out)
{
	struct nl_msg *msg = lp_msg_new(pon, PON_CMD_TCONT_GET, 0);

	memset(out, 0, sizeof(*out));
	if (!msg)
		return -ENOMEM;
	if (nla_put_u32(msg, PON_A_TCONT_DEV_ID, dev_id) ||
	    nla_put_u32(msg, PON_A_TCONT_INDEX, index))
		return put_failed(pon, msg);
	return get_run(pon, msg, tcont_one, out);
}

/**
 * tcont_show - decode one T-CONT of a dump and call back
 * @msg: the reply
 * @arg: the struct dump of the request
 *
 * Return: NL_OK, or NL_SKIP for a reply that does not decode or is of
 * another device.
 */
static int tcont_show(struct nl_msg *msg, void *arg)
{
	struct dump *dump = arg;
	struct pon_tcont tcont;

	if (lp_tcont_decode(msg, &tcont) || !dev_match(dump, tcont.dev_id))
		return NL_SKIP;
	((pon_tcont_cb)dump->cb)(dump->arg, &tcont);
	return NL_OK;
}

/**
 * pon_tcont_dump - list the T-CONTs
 * @pon: the client
 * @dev_id: the device, or PON_DEV_ANY
 * @cb: called for every T-CONT
 * @arg: passed to @cb
 *
 * Return: 0, or a negative errno; -ENODEV when the device does not exist,
 * -EINTR when the list changed during the dump and @cb saw a T-CONT twice
 * or missed one.
 */
int pon_tcont_dump(struct pon *pon, uint32_t dev_id, pon_tcont_cb cb, void *arg)
{
	struct dump dump = { dev_id, (void *)cb, arg };

	return dump_run(pon, PON_CMD_TCONT_GET, PON_A_TCONT_DEV_ID, &dump,
			tcont_show);
}

/**
 * pon_gem_new - create a GEM port
 * @pon: the client
 * @gem: the port; a negative @tcont_index, @key_ring, @queue or @no_offload
 *	 is left out of the request
 *
 * An identical re-add succeeds.
 *
 * Return: 0, or a negative errno.
 */
int pon_gem_new(struct pon *pon, const struct pon_gem *gem)
{
	struct nl_msg *msg = lp_msg_new(pon, PON_CMD_GEM_NEW, 0);
	int err;

	if (!msg)
		return -ENOMEM;
	err = nla_put_u32(msg, PON_A_GEM_DEV_ID, gem->dev_id);
	if (!err)
		err = nla_put_u32(msg, PON_A_GEM_ID, gem->id);
	if (!err)
		err = nla_put_u32(msg, PON_A_GEM_DIR, gem->dir);
	if (!err && gem->tcont_index >= 0)
		err = nla_put_u32(msg, PON_A_GEM_TCONT_INDEX, gem->tcont_index);
	if (!err && gem->key_ring >= 0)
		err = nla_put_u32(msg, PON_A_GEM_KEY_RING, gem->key_ring);
	if (!err && gem->queue >= 0)
		err = nla_put_u32(msg, PON_A_GEM_QUEUE, gem->queue);
	if (!err && gem->no_offload >= 0)
		err = nla_put_u8(msg, PON_A_GEM_NO_OFFLOAD, gem->no_offload);
	if (err)
		return put_failed(pon, msg);
	return run(pon, msg, NULL, NULL);
}

/**
 * pon_gem_del - delete a GEM port
 * @pon: the client
 * @dev_id: the device
 * @id: the GEM port
 *
 * Return: 0, or a negative errno.
 */
int pon_gem_del(struct pon *pon, uint32_t dev_id, uint32_t id)
{
	struct nl_msg *msg = lp_msg_new(pon, PON_CMD_GEM_DEL, 0);

	if (!msg)
		return -ENOMEM;
	if (nla_put_u32(msg, PON_A_GEM_DEV_ID, dev_id) ||
	    nla_put_u32(msg, PON_A_GEM_ID, id))
		return put_failed(pon, msg);
	return run(pon, msg, NULL, NULL);
}

/**
 * gem_one - decode the reply to a get of a GEM port
 * @msg: the reply
 * @out: the struct pon_gem to fill
 *
 * Return: 0, or -1 when the reply is not one.
 */
static int gem_one(struct nl_msg *msg, void *out)
{
	return lp_gem_decode(msg, out);
}

/**
 * pon_gem_get - read one GEM port
 * @pon: the client
 * @dev_id: the device
 * @id: the GEM port
 * @out: the port, overwritten
 *
 * Return: 0, or a negative errno; -EPROTO when the kernel sent no reply
 * that decodes.
 */
int pon_gem_get(struct pon *pon, uint32_t dev_id, uint32_t id,
		struct pon_gem *out)
{
	struct nl_msg *msg = lp_msg_new(pon, PON_CMD_GEM_GET, 0);

	memset(out, 0, sizeof(*out));
	if (!msg)
		return -ENOMEM;
	if (nla_put_u32(msg, PON_A_GEM_DEV_ID, dev_id) ||
	    nla_put_u32(msg, PON_A_GEM_ID, id))
		return put_failed(pon, msg);
	return get_run(pon, msg, gem_one, out);
}

/**
 * gem_show - decode one GEM port of a dump and call back
 * @msg: the reply
 * @arg: the struct dump of the request
 *
 * Return: NL_OK, or NL_SKIP for a reply that does not decode or is of
 * another device.
 */
static int gem_show(struct nl_msg *msg, void *arg)
{
	struct dump *dump = arg;
	struct pon_gem gem;

	if (lp_gem_decode(msg, &gem) || !dev_match(dump, gem.dev_id))
		return NL_SKIP;
	((pon_gem_cb)dump->cb)(dump->arg, &gem);
	return NL_OK;
}

/**
 * pon_gem_dump - list the GEM ports
 * @pon: the client
 * @dev_id: the device, or PON_DEV_ANY
 * @cb: called for every port
 * @arg: passed to @cb
 *
 * Return: 0, or a negative errno; -ENODEV when the device does not exist,
 * -EINTR when the list changed during the dump and @cb saw a port twice or
 * missed one.
 */
int pon_gem_dump(struct pon *pon, uint32_t dev_id, pon_gem_cb cb, void *arg)
{
	struct dump dump = { dev_id, (void *)cb, arg };

	return dump_run(pon, PON_CMD_GEM_GET, PON_A_GEM_DEV_ID, &dump,
			gem_show);
}

/**
 * gem_map_request - send a classifier rule
 * @pon: the client
 * @cmd: PON_CMD_GEM_MAP_NEW or PON_CMD_GEM_MAP_DEL
 * @map: the rule; a negative match field is left out
 *
 * Return: 0, or a negative errno.
 */
static int gem_map_request(struct pon *pon, int cmd,
			   const struct pon_gem_map *map)
{
	struct nl_msg *msg = lp_msg_new(pon, cmd, 0);
	int err;

	if (!msg)
		return -ENOMEM;
	err = nla_put_u32(msg, PON_A_GEM_MAP_DEV_ID, map->dev_id);
	if (!err)
		err = nla_put_u32(msg, PON_A_GEM_MAP_GEM_ID, map->gem_id);
	if (!err && map->tag >= 0)
		err = nla_put_u32(msg, PON_A_GEM_MAP_TAG, map->tag);
	if (!err && map->vid >= 0)
		err = nla_put_u32(msg, PON_A_GEM_MAP_VID, map->vid);
	if (!err && map->pbit >= 0)
		err = nla_put_u32(msg, PON_A_GEM_MAP_PBIT, map->pbit);
	if (!err && map->dscp >= 0)
		err = nla_put_u32(msg, PON_A_GEM_MAP_DSCP, map->dscp);
	if (err)
		return put_failed(pon, msg);
	return run(pon, msg, NULL, NULL);
}

/**
 * pon_gem_map_new - add a classifier rule
 * @pon: the client
 * @map: the rule; a negative match field is left out and matches anything
 *
 * An identical rule is accepted silently.
 *
 * Return: 0, or a negative errno.
 */
int pon_gem_map_new(struct pon *pon, const struct pon_gem_map *map)
{
	return gem_map_request(pon, PON_CMD_GEM_MAP_NEW, map);
}

/**
 * pon_gem_map_del - delete a classifier rule
 * @pon: the client
 * @map: the rule, as it was added
 *
 * Return: 0, or a negative errno; -ENOENT when it does not exist.
 */
int pon_gem_map_del(struct pon *pon, const struct pon_gem_map *map)
{
	return gem_map_request(pon, PON_CMD_GEM_MAP_DEL, map);
}

/**
 * gem_map_show - decode one classifier rule of a dump and call back
 * @msg: the reply
 * @arg: the struct dump of the request
 *
 * Return: NL_OK, or NL_SKIP for a reply that does not decode or is of
 * another device.
 */
static int gem_map_show(struct nl_msg *msg, void *arg)
{
	struct dump *dump = arg;
	struct pon_gem_map map;

	if (lp_gem_map_decode(msg, &map) || !dev_match(dump, map.dev_id))
		return NL_SKIP;
	((pon_gem_map_cb)dump->cb)(dump->arg, &map);
	return NL_OK;
}

/**
 * pon_gem_map_dump - list the classifier rules
 * @pon: the client
 * @dev_id: the device, or PON_DEV_ANY
 * @cb: called for every rule
 * @arg: passed to @cb
 *
 * Return: 0, or a negative errno; -ENODEV when the device does not exist,
 * -EINTR when the list changed during the dump and @cb saw a rule twice or
 * missed one.
 */
int pon_gem_map_dump(struct pon *pon, uint32_t dev_id, pon_gem_map_cb cb,
		     void *arg)
{
	struct dump dump = { dev_id, (void *)cb, arg };

	return dump_run(pon, PON_CMD_GEM_MAP_GET, PON_A_GEM_MAP_DEV_ID, &dump,
			gem_map_show);
}

/**
 * alarm_one - decode the reply to a get of the alarms of a device
 * @msg: the reply
 * @out: the struct pon_alarms to fill
 *
 * Return: 0, or -1 when the reply is not one.
 */
static int alarm_one(struct nl_msg *msg, void *out)
{
	return lp_alarm_decode(msg, out);
}

/**
 * pon_alarm_get - read the alarm bitmap of a device
 * @pon: the client
 * @dev_id: the device
 * @out: the bitmap, one bit per enum pon_alarm, overwritten
 *
 * Return: 0, or a negative errno; -EPROTO when the kernel sent no reply
 * that decodes.
 */
int pon_alarm_get(struct pon *pon, uint32_t dev_id, struct pon_alarms *out)
{
	struct nl_msg *msg = lp_msg_new(pon, PON_CMD_ALARM_GET, 0);

	memset(out, 0, sizeof(*out));
	if (!msg)
		return -ENOMEM;
	if (nla_put_u32(msg, PON_A_ALARM_DEV_ID, dev_id))
		return put_failed(pon, msg);
	return get_run(pon, msg, alarm_one, out);
}

/**
 * alarm_show - decode one alarm bitmap of a dump and call back
 * @msg: the reply
 * @arg: the struct dump of the request
 *
 * Return: NL_OK, or NL_SKIP for a reply that does not decode or is of
 * another device.
 */
static int alarm_show(struct nl_msg *msg, void *arg)
{
	struct dump *dump = arg;
	struct pon_alarms alarms;

	if (lp_alarm_decode(msg, &alarms) || !dev_match(dump, alarms.dev_id))
		return NL_SKIP;
	((pon_alarms_cb)dump->cb)(dump->arg, &alarms);
	return NL_OK;
}

/**
 * pon_alarm_dump - list the alarm bitmaps
 * @pon: the client
 * @dev_id: the device, or PON_DEV_ANY
 * @cb: called for every device
 * @arg: passed to @cb
 *
 * The kernel lists every device; the library passes on those of @dev_id.
 *
 * Return: 0, or a negative errno; -EINTR when the list changed during the
 * dump and @cb saw a device twice or missed one.
 */
int pon_alarm_dump(struct pon *pon, uint32_t dev_id, pon_alarms_cb cb,
		   void *arg)
{
	struct dump dump = { dev_id, (void *)cb, arg };

	return dump_run(pon, PON_CMD_ALARM_GET, 0, &dump, alarm_show);
}

/**
 * stats_one - decode the reply to a get of the counters of a GEM port
 * @msg: the reply
 * @out: the struct pon_gem_stats to fill
 *
 * Return: 0, or -1 when the reply is not one.
 */
static int stats_one(struct nl_msg *msg, void *out)
{
	return stats_decode(msg, out);
}

/**
 * pon_stats_get - read the counters of one GEM port
 * @pon: the client
 * @dev_id: the device
 * @gem_id: the GEM port
 * @out: the counters, overwritten
 *
 * Use this rather than the dump for a known port: the dump aborts whole
 * when the driver refuses one port.
 *
 * Return: 0, or a negative errno; -EPROTO when the kernel sent no reply
 * that decodes.
 */
int pon_stats_get(struct pon *pon, uint32_t dev_id, uint32_t gem_id,
		  struct pon_gem_stats *out)
{
	struct nl_msg *msg = lp_msg_new(pon, PON_CMD_GEM_STATS_GET, 0);

	memset(out, 0, sizeof(*out));
	if (!msg)
		return -ENOMEM;
	if (nla_put_u32(msg, PON_A_GEM_STATS_DEV_ID, dev_id) ||
	    nla_put_u32(msg, PON_A_GEM_STATS_GEM_ID, gem_id))
		return put_failed(pon, msg);
	return get_run(pon, msg, stats_one, out);
}

/**
 * stats_show - decode one set of GEM port counters of a dump and call back
 * @msg: the reply
 * @arg: the struct dump of the request
 *
 * Return: NL_OK, or NL_SKIP for a reply that does not decode or is of
 * another device.
 */
static int stats_show(struct nl_msg *msg, void *arg)
{
	struct dump *dump = arg;
	struct pon_gem_stats stats;

	if (stats_decode(msg, &stats) || !dev_match(dump, stats.dev_id))
		return NL_SKIP;
	((pon_gem_stats_cb)dump->cb)(dump->arg, &stats);
	return NL_OK;
}

/**
 * pon_stats_dump - list the counters of every GEM port
 * @pon: the client
 * @dev_id: the device, or PON_DEV_ANY
 * @cb: called for every port
 * @arg: passed to @cb
 *
 * Return: 0, or a negative errno; -ENODEV when the device does not exist,
 * -EINTR when the list changed during the dump and @cb saw a port twice or
 * missed one.
 */
int pon_stats_dump(struct pon *pon, uint32_t dev_id, pon_gem_stats_cb cb,
		   void *arg)
{
	struct dump dump = { dev_id, (void *)cb, arg };

	return dump_run(pon, PON_CMD_GEM_STATS_GET, PON_A_GEM_STATS_DEV_ID,
			&dump, stats_show);
}

/**
 * fec_one - decode the reply to a get of the FEC counters of a device
 * @msg: the reply
 * @out: the struct pon_fec_stats to fill
 *
 * Return: 0, or -1 when the reply is not one.
 */
static int fec_one(struct nl_msg *msg, void *out)
{
	return fec_decode(msg, out);
}

/**
 * pon_fec_get - read the FEC counters of a device
 * @pon: the client
 * @dev_id: the device
 * @out: the counters, overwritten
 *
 * Return: 0, or a negative errno; -EPROTO when the kernel sent no reply
 * that decodes.
 */
int pon_fec_get(struct pon *pon, uint32_t dev_id, struct pon_fec_stats *out)
{
	struct nl_msg *msg = lp_msg_new(pon, PON_CMD_FEC_GET, 0);

	memset(out, 0, sizeof(*out));
	if (!msg)
		return -ENOMEM;
	if (nla_put_u32(msg, PON_A_FEC_DEV_ID, dev_id))
		return put_failed(pon, msg);
	return get_run(pon, msg, fec_one, out);
}

/**
 * tc_stats_one - decode the reply to a get of the counters of the TC layer
 * @msg: the reply
 * @out: the struct pon_tc_stats to fill
 *
 * Return: 0, or -1 when the reply is not one.
 */
static int tc_stats_one(struct nl_msg *msg, void *out)
{
	return tc_stats_decode(msg, out);
}

/**
 * pon_tc_stats_get - read the counters of the TC layer of a device
 * @pon: the client
 * @dev_id: the device
 * @out: the counters, overwritten; @out->counter is indexed by the
 *	 PON_A_TC_STATS_ value of the counter
 *
 * The kernel leaves out a counter that the device does not keep.
 * pon_tc_stats_has() tells whether the reply carried a counter.
 *
 * Return: 0, or a negative errno; -EPROTO when the kernel sent no reply
 * that decodes.
 */
int pon_tc_stats_get(struct pon *pon, uint32_t dev_id, struct pon_tc_stats *out)
{
	struct nl_msg *msg = lp_msg_new(pon, PON_CMD_TC_STATS_GET, 0);

	memset(out, 0, sizeof(*out));
	if (!msg)
		return -ENOMEM;
	if (nla_put_u32(msg, PON_A_TC_STATS_DEV_ID, dev_id))
		return put_failed(pon, msg);
	return get_run(pon, msg, tc_stats_one, out);
}
