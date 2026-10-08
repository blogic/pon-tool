// SPDX-License-Identifier: LGPL-2.1-only
/* Copyright (C) 2026 John Crispin <john@phrozen.org> */
#include <stdint.h>
#include <string.h>

#include <netlink/netlink.h>
#include <netlink/attr.h>
#include <netlink/msg.h>
#include <netlink/genl/genl.h>

#include "internal.h"
#include "test.h"

#define T_FAMILY	0x7000

/**
 * t_msg - a message of the family as the kernel sends it
 * @cmd: the command
 *
 * Return: the message, or NULL when out of memory.
 */
static struct nl_msg *t_msg(int cmd)
{
	struct nl_msg *msg = nlmsg_alloc();

	if (msg)
		genlmsg_put(msg, 0, 0, T_FAMILY, 0, 0, cmd, PON_FAMILY_VERSION);
	return msg;
}

/**
 * decode_dev_test - a device reply sets a flag for each attribute it has
 */
static void decode_dev_test(void)
{
	static const uint8_t serial[PON_SERIAL_LEN] = {
		'A', 'D', 'T', 'N', 0x12, 0x34, 0x56, 0x78
	};
	struct nl_msg *msg = t_msg(PON_CMD_DEV_GET);
	struct pon_dev_info dev;

	nla_put_u32(msg, PON_A_DEV_ID, 3);
	nla_put_u32(msg, PON_A_DEV_PLOAM_STATE, PON_PLOAM_STATE_O5);
	nla_put_u8(msg, PON_A_DEV_ENABLE, 1);
	nla_put(msg, PON_A_DEV_SERIAL, PON_SERIAL_LEN, serial);
	CHECK_EQ(lp_dev_decode(msg, &dev), 0);
	CHECK_EQ(dev.dev_id, 3);
	CHECK_EQ(dev.has, PON_DEV_F_PLOAM | PON_DEV_F_ENABLE |
		 PON_DEV_F_SERIAL);
	CHECK_EQ(dev.ploam, PON_PLOAM_STATE_O5);
	CHECK_EQ(dev.enable, 1);
	CHECK(!memcmp(dev.serial, serial, PON_SERIAL_LEN));
	nlmsg_free(msg);

	msg = t_msg(PON_CMD_DEV_GET);
	nla_put_u32(msg, PON_A_DEV_IFINDEX, 7);
	CHECK_EQ(lp_dev_decode(msg, &dev), -1);
	nlmsg_free(msg);

	msg = t_msg(PON_CMD_DEV_GET);
	nla_put_u32(msg, PON_A_DEV_ID, 3);
	nla_put_u32(msg, PON_A_DEV_PLOAM_STATE, PON_PLOAM_STATE_O7);
	nla_put_flag(msg, PON_A_DEV_DISABLED);
	CHECK_EQ(lp_dev_decode(msg, &dev), 0);
	CHECK_EQ(dev.has, PON_DEV_F_PLOAM | PON_DEV_F_DISABLED);
	CHECK_EQ(dev.disabled, 1);
	nlmsg_free(msg);
}

/**
 * decode_gem_test - an absent optional attribute of a GEM port reads -1
 */
static void decode_gem_test(void)
{
	struct nl_msg *msg = t_msg(PON_CMD_GEM_GET);
	struct pon_gem gem;

	nla_put_u32(msg, PON_A_GEM_DEV_ID, 0);
	nla_put_u32(msg, PON_A_GEM_ID, 1100);
	nla_put_u32(msg, PON_A_GEM_DIR, PON_GEM_DIR_BIDIR);
	nla_put_u32(msg, PON_A_GEM_TCONT_INDEX, 2);
	CHECK_EQ(lp_gem_decode(msg, &gem), 0);
	CHECK_EQ(gem.id, 1100);
	CHECK_EQ(gem.dir, PON_GEM_DIR_BIDIR);
	CHECK_EQ(gem.tcont_index, 2);
	CHECK_EQ(gem.key_ring, -1);
	CHECK_EQ(gem.queue, -1);
	CHECK_EQ(gem.no_offload, -1);
	nlmsg_free(msg);
}

/**
 * decode_event_test - each value of an event has its own member and flag
 */
static void decode_event_test(void)
{
	struct nl_msg *msg = t_msg(PON_CMD_EVENT_NTF);
	struct pon_event event;

	nla_put_u32(msg, PON_A_EVENT_DEV_ID, 0);
	nla_put_u32(msg, PON_A_EVENT_TYPE, PON_EVENT_TYPE_REBOOT_REQ);
	nla_put_u32(msg, PON_A_EVENT_DEPTH, 1);
	nla_put_u32(msg, PON_A_EVENT_IMAGE, 0);
	nla_put_u32(msg, PON_A_EVENT_CALLS, PON_REBOOT_CALLS_NO_CALLS);
	CHECK_EQ(lp_event_decode(msg, &event), 0);
	CHECK_EQ(event.has, PON_EVENT_F_DEPTH | PON_EVENT_F_IMAGE |
		 PON_EVENT_F_CALLS);
	CHECK_EQ(event.depth, 1);
	CHECK_EQ(event.image, 0);
	CHECK_EQ(event.calls, PON_REBOOT_CALLS_NO_CALLS);
	nlmsg_free(msg);

	msg = t_msg(PON_CMD_EVENT_NTF);
	nla_put_u32(msg, PON_A_EVENT_DEV_ID, 0);
	nla_put_u32(msg, PON_A_EVENT_TYPE, PON_EVENT_TYPE_MIB_RESET_REQ);
	CHECK_EQ(lp_event_decode(msg, &event), 0);
	CHECK_EQ(event.has, 0);
	nlmsg_free(msg);

	msg = t_msg(PON_CMD_EVENT_NTF);
	nla_put_u32(msg, PON_A_EVENT_DEV_ID, 0);
	nla_put_u32(msg, PON_A_EVENT_TYPE, PON_EVENT_TYPE_MIB_RESET_REQ);
	nla_put_u32(msg, PON_A_EVENT_CALLS,
		    PON_REBOOT_CALLS_NO_EMERGENCY_CALLS);
	CHECK_EQ(lp_event_decode(msg, &event), 0);
	CHECK_EQ(event.has, PON_EVENT_F_CALLS);
	CHECK_EQ(event.calls, PON_REBOOT_CALLS_NO_EMERGENCY_CALLS);
	nlmsg_free(msg);

	msg = t_msg(PON_CMD_EVENT_NTF);
	nla_put_u32(msg, PON_A_EVENT_DEV_ID, 0);
	nla_put_u32(msg, PON_A_EVENT_TYPE, PON_EVENT_TYPE_TCONT_ALLOC);
	nla_put_u32(msg, PON_A_EVENT_ALLOC_ID, 1024);
	CHECK_EQ(lp_event_decode(msg, &event), 0);
	CHECK_EQ(event.has, PON_EVENT_F_ALLOC_ID);
	CHECK_EQ(event.alloc_id, 1024);
	nlmsg_free(msg);
}

/**
 * decode_size_test - an attribute shorter or longer than its type is refused
 */
static void decode_size_test(void)
{
	struct nl_msg *msg = t_msg(PON_CMD_DEV_GET);
	struct pon_dev_info dev;
	uint16_t half = 3;
	uint64_t wide = 7;

	nla_put(msg, PON_A_DEV_ID, sizeof(half), &half);
	CHECK_EQ(lp_dev_decode(msg, &dev), -1);
	nlmsg_free(msg);

	msg = t_msg(PON_CMD_DEV_GET);
	nla_put_u32(msg, PON_A_DEV_ID, 3);
	nla_put_u32(msg, PON_A_DEV_ENABLE, 1);
	CHECK_EQ(lp_dev_decode(msg, &dev), -1);
	nlmsg_free(msg);

	msg = t_msg(PON_CMD_DEV_GET);
	nla_put_u32(msg, PON_A_DEV_ID, 3);
	nla_put(msg, PON_A_DEV_MODE, sizeof(wide), &wide);
	CHECK_EQ(lp_dev_decode(msg, &dev), -1);
	nlmsg_free(msg);

	msg = t_msg(PON_CMD_DEV_GET);
	nla_put_u32(msg, PON_A_DEV_ID, 3);
	nla_put(msg, PON_A_DEV_SERIAL, 4, &wide);
	CHECK_EQ(lp_dev_decode(msg, &dev), -1);
	nlmsg_free(msg);

	msg = t_msg(PON_CMD_DEV_GET);
	nla_put_u32(msg, PON_A_DEV_ID, 3);
	nla_put_u8(msg, PON_A_DEV_DISABLED, 0);
	CHECK_EQ(lp_dev_decode(msg, &dev), -1);
	nlmsg_free(msg);

	msg = t_msg(PON_CMD_EVENT_NTF);
	nla_put_u32(msg, PON_A_EVENT_DEV_ID, 0);
	nla_put(msg, PON_A_EVENT_TYPE, sizeof(half), &half);
	CHECK_EQ(lp_event_decode(msg, &(struct pon_event){ 0 }), -1);
	nlmsg_free(msg);
}

/**
 * main - run the cases of the decoders
 *
 * Return: 0 when every check passed, else 1.
 */
int main(void)
{
	decode_dev_test();
	decode_gem_test();
	decode_event_test();
	decode_size_test();
	return t_report("test-decode");
}
