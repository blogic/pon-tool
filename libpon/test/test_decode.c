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
 * decode_gem_map_test - a mapper rule needs its P-bit, its ifindex is optional
 */
static void decode_gem_map_test(void)
{
	struct nl_msg *msg = t_msg(PON_CMD_GEM_MAP_GET);
	struct pon_gem_map map;

	nla_put_u32(msg, PON_A_GEM_MAP_DEV_ID, 1);
	nla_put_u32(msg, PON_A_GEM_MAP_GEM_ID, 1100);
	nla_put_u32(msg, PON_A_GEM_MAP_PBIT, 5);
	CHECK_EQ(lp_gem_map_decode(msg, &map), 0);
	CHECK_EQ(map.gem_id, 1100);
	CHECK_EQ(map.pbit, 5);
	CHECK_EQ(map.ifindex, 0);
	nla_put_u32(msg, PON_A_GEM_MAP_IFINDEX, 9);
	CHECK_EQ(lp_gem_map_decode(msg, &map), 0);
	CHECK_EQ(map.ifindex, 9);
	nlmsg_free(msg);

	msg = t_msg(PON_CMD_GEM_MAP_GET);
	nla_put_u32(msg, PON_A_GEM_MAP_DEV_ID, 1);
	nla_put_u32(msg, PON_A_GEM_MAP_GEM_ID, 1100);
	CHECK_EQ(lp_gem_map_decode(msg, &map), -1);
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
 * decode_tc_stats_test - each group nest fills its own counters
 */
static void decode_tc_stats_test(void)
{
	struct nl_msg *msg = t_msg(PON_CMD_TC_STATS_GET);
	struct pon_tc_stats stats;
	struct nlattr *nest;

	nla_put_u32(msg, PON_A_TC_STATS_DEV_ID, 2);
	nest = nla_nest_start(msg, PON_A_TC_STATS_XGTC);
	nla_put_u64(msg, PON_A_XGTC_STATS_LODS_EVENTS, 1ull << 40);
	nla_put_u32(msg, PON_A_XGTC_STATS_PSBD_HEC_ERRORS, 0);
	nla_nest_end(msg, nest);
	nest = nla_nest_start(msg, PON_A_TC_STATS_PLOAM_US);
	nla_put_u32(msg, PON_A_PLOAM_US_STATS_TX_PLOAM, 7);
	nla_nest_end(msg, nest);
	CHECK_EQ(lp_tc_stats_decode(msg, &stats), 0);
	CHECK_EQ(stats.dev_id, 2);
	CHECK(pon_tc_stats_has(&stats, PON_TC_STATS_GROUP_XGTC,
			       PON_A_XGTC_STATS_LODS_EVENTS));
	CHECK_EQ(stats.group[PON_TC_STATS_GROUP_XGTC]
		 .counter[PON_A_XGTC_STATS_LODS_EVENTS], 1ull << 40);
	CHECK(pon_tc_stats_has(&stats, PON_TC_STATS_GROUP_XGTC,
			       PON_A_XGTC_STATS_PSBD_HEC_ERRORS));
	CHECK(!pon_tc_stats_has(&stats, PON_TC_STATS_GROUP_XGTC,
				PON_A_XGTC_STATS_XGEM_KEY_ERRORS));
	CHECK_EQ(stats.group[PON_TC_STATS_GROUP_PLOAM_DS].has, 0);
	CHECK_EQ(stats.group[PON_TC_STATS_GROUP_PLOAM_US]
		 .counter[PON_A_PLOAM_US_STATS_TX_PLOAM], 7);
	CHECK(!pon_tc_stats_has(&stats, PON_TC_GROUPS, 1));
	nlmsg_free(msg);
}

/**
 * decode_omci_stats_test - the reply fills every counter of the channel
 */
static void decode_omci_stats_test(void)
{
	struct nl_msg *msg = t_msg(PON_CMD_OMCI_STATS_GET);
	struct pon_omci_stats stats;

	nla_put_u32(msg, PON_A_OMCI_STATS_DEV_ID, 1);
	nla_put_u32(msg, PON_A_OMCI_STATS_RX, 10);
	nla_put_u32(msg, PON_A_OMCI_STATS_RX_DROPPED, 2);
	nla_put_u32(msg, PON_A_OMCI_STATS_RX_ERRORS, 3);
	nla_put_u64(msg, PON_A_OMCI_STATS_TX, 1ull << 33);
	nla_put_u32(msg, PON_A_OMCI_STATS_TX_ERRORS, 5);
	CHECK_EQ(lp_omci_stats_decode(msg, &stats), 0);
	CHECK_EQ(stats.dev_id, 1);
	CHECK_EQ(stats.rx, 10);
	CHECK_EQ(stats.rx_dropped, 2);
	CHECK_EQ(stats.rx_errors, 3);
	CHECK_EQ(stats.tx, 1ull << 33);
	CHECK_EQ(stats.tx_errors, 5);
	nlmsg_free(msg);

	msg = t_msg(PON_CMD_OMCI_STATS_GET);
	nla_put_u32(msg, PON_A_OMCI_STATS_RX, 10);
	CHECK_EQ(lp_omci_stats_decode(msg, &stats), -1);
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
	decode_gem_map_test();
	decode_event_test();
	decode_tc_stats_test();
	decode_omci_stats_test();
	decode_size_test();
	return t_report("test-decode");
}
