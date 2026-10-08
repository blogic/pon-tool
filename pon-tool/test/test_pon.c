// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (C) 2026 John Crispin <john@phrozen.org> */
#define main pon_tool_main
#include "../pon.c"
#undef main

#include <unistd.h>

#include "test.h"
#include "fake.h"

#define T_FAMILY	0x7000
#define T_OUT_MAX	4096
#define T_ARGS(...)	(sizeof((char *[]){ __VA_ARGS__ }) / sizeof(char *)), \
			((char *[]){ __VA_ARGS__ })

static const uint8_t t_serial[PON_SERIAL_LEN] = {
	'A', 'D', 'T', 'N', 0, 0, 0, 1
};

static struct t_fake fake;
static char t_out[T_OUT_MAX];
static FILE *t_out_file;
static int t_out_saved = -1;
static int t_out_fd;

/**
 * t_capture_start - send one output stream of the tool into a file
 * @fd: STDOUT_FILENO or STDERR_FILENO
 */
static void t_capture_start(int fd)
{
	fflush(stdout);
	fflush(stderr);
	t_out_fd = fd;
	t_out_file = tmpfile();
	t_out_saved = dup(fd);
	dup2(fileno(t_out_file), fd);
}

/**
 * t_out_start - send the standard output of the tool into a file
 */
static void t_out_start(void)
{
	t_capture_start(STDOUT_FILENO);
}

/**
 * t_err_start - send the standard error of the tool into a file
 */
static void t_err_start(void)
{
	t_capture_start(STDERR_FILENO);
}

/**
 * t_out_end - give the stream back and read what the tool wrote to it
 *
 * Return: the text the tool wrote since t_out_start() or t_err_start().
 */
static const char *t_out_end(void)
{
	size_t len;

	fflush(stdout);
	fflush(stderr);
	dup2(t_out_saved, t_out_fd);
	close(t_out_saved);
	rewind(t_out_file);
	len = fread(t_out, 1, sizeof(t_out) - 1, t_out_file);
	t_out[len] = '\0';
	fclose(t_out_file);
	return t_out;
}

/**
 * t_dev_reply - queue a device as the board reports it
 * @flags: the netlink flags, NLM_F_MULTI for a part of a dump
 */
static void t_dev_reply(int flags)
{
	struct nl_msg *msg = t_fake_reply(&fake, PON_CMD_DEV_GET, flags);

	nla_put_u32(msg, PON_A_DEV_ID, 1);
	nla_put_u32(msg, PON_A_DEV_IFINDEX, 9);
	nla_put_u32(msg, PON_A_DEV_MODE, PON_MODE_XGS_PON);
	nla_put_u32(msg, PON_A_DEV_MODES_CAP, 1u << PON_MODE_XGS_PON);
	nla_put(msg, PON_A_DEV_SERIAL, sizeof(t_serial), t_serial);
	nla_put_u32(msg, PON_A_DEV_PLOAM_STATE, PON_PLOAM_STATE_O5);
	nla_put_u32(msg, PON_A_DEV_MAX_TCONTS, 32);
	nla_put_u32(msg, PON_A_DEV_MAX_GEMS, 255);
	nla_put_u8(msg, PON_A_DEV_ENABLE, 1);
	nla_put_u32(msg, PON_A_DEV_RESPONSE_TIME, 35005);
	t_fake_send(&fake, pon.cmd.sock, msg);
}

/**
 * t_refused - run a command that must be refused before a request
 * @err: the result of the command
 *
 * The acknowledgment queued before the command answers a request that
 * goes out anyway, so that the command returns instead of blocking.
 */
static void t_refused(int err)
{
	CHECK_EQ(err, -EINVAL);
	CHECK_EQ(t_fake_drain(&fake), 0);
	t_fake_recv(pon.cmd.sock);
}

/**
 * pon_gem_new_test - pon gem new takes the T-CONT as a keyword, where the
 * direction needs one
 */
static void pon_gem_new_test(void)
{
	struct nlattr *tb[PON_A_GEM_MAX + 1] = { 0 };

	t_fake_ack(&fake, 0, NULL);
	CHECK_EQ(gem_cmd(T_ARGS("gem", "new", "0", "1100", "bidir", "tcont",
				"2", "key-ring", "unicast")), 0);
	CHECK_EQ(t_fake_request(&fake, tb, PON_A_GEM_MAX), PON_CMD_GEM_NEW);
	CHECK(tb[PON_A_GEM_DIR] &&
	      nla_get_u32(tb[PON_A_GEM_DIR]) == PON_GEM_DIR_BIDIR);
	CHECK(tb[PON_A_GEM_TCONT_INDEX] &&
	      nla_get_u32(tb[PON_A_GEM_TCONT_INDEX]) == 2);
	CHECK(tb[PON_A_GEM_KEY_RING] &&
	      nla_get_u32(tb[PON_A_GEM_KEY_RING]) == PON_GEM_KEY_RING_UNICAST);

	t_fake_ack(&fake, 0, NULL);
	t_refused(gem_cmd(T_ARGS("gem", "new", "0", "1100", "3", "2",
				 "key-ring", "unicast")));
	t_fake_ack(&fake, 0, NULL);
	t_refused(gem_cmd(T_ARGS("gem", "new", "0", "1100", "bidir", "tcont")));
	t_fake_ack(&fake, 0, NULL);
	t_refused(gem_cmd(T_ARGS("gem", "new", "0", "1100", "0")));
	t_fake_ack(&fake, 0, NULL);
	t_refused(gem_cmd(T_ARGS("gem", "new", "0", "1100", "bidir", "queue",
				 "x")));
	t_fake_ack(&fake, 0, NULL);
	t_refused(gem_cmd(T_ARGS("gem", "new", "0", "1100", "bidir")));
	t_fake_ack(&fake, 0, NULL);
	t_refused(gem_cmd(T_ARGS("gem", "new", "0", "1100", "upstream",
				 "queue", "1")));
	t_fake_ack(&fake, 0, NULL);
	t_refused(gem_cmd(T_ARGS("gem", "new", "0", "1100", "downstream",
				 "tcont", "2")));

	t_fake_ack(&fake, 0, NULL);
	CHECK_EQ(gem_cmd(T_ARGS("gem", "new", "0", "1101", "downstream")), 0);
	CHECK_EQ(t_fake_request(&fake, tb, PON_A_GEM_MAX), PON_CMD_GEM_NEW);
	CHECK(!tb[PON_A_GEM_TCONT_INDEX]);
}

/**
 * pon_numbers_test - a number with junk is refused before a request
 */
static void pon_numbers_test(void)
{
	t_fake_ack(&fake, 0, NULL);
	t_refused(gem_cmd(T_ARGS("gem", "map", "add", "0", "1100", "8")));
	t_fake_ack(&fake, 0, NULL);
	t_refused(gem_cmd(T_ARGS("gem", "map", "add", "0", "1100", "x")));
	t_fake_ack(&fake, 0, NULL);
	t_refused(dev_cmd(T_ARGS("dev", "7x")));
	t_fake_ack(&fake, 0, NULL);
	t_refused(tconts_cmd(T_ARGS("tconts", "0", "-1")));
	t_fake_ack(&fake, 0, NULL);
	t_refused(tcont_cmd(T_ARGS("tcont", "set", "0", "1", "1024x")));
}

/**
 * pon_arguments_test - extra, missing and refused arguments are refused
 */
static void pon_arguments_test(void)
{
	struct nlattr *tb[PON_A_TCONT_MAX + 1] = { 0 };

	t_fake_ack(&fake, 0, NULL);
	t_refused(dev_cmd(T_ARGS("dev", "set")));
	t_fake_ack(&fake, 0, NULL);
	t_refused(dev_cmd(T_ARGS("dev", "set", "0", "disabled", "0")));
	t_fake_ack(&fake, 0, NULL);
	t_refused(tcont_cmd(T_ARGS("tcont", "set", "0", "1", "1024", "9")));
	t_fake_ack(&fake, 0, NULL);
	t_refused(tcont_cmd(T_ARGS("tcont", "del", "0", "1", "9")));
	t_fake_ack(&fake, 0, NULL);
	t_refused(gem_cmd(T_ARGS("gem", "del", "0", "1100", "9")));

	t_fake_ack(&fake, 0, NULL);
	CHECK_EQ(tcont_cmd(T_ARGS("tcont", "set", "0", "1", "1024")), 0);
	CHECK_EQ(t_fake_request(&fake, tb, PON_A_TCONT_MAX),
		 PON_CMD_TCONT_SET);
	CHECK(tb[PON_A_TCONT_ALLOC_ID] &&
	      nla_get_u32(tb[PON_A_TCONT_ALLOC_ID]) == 1024);
}

/**
 * t_seed_reply - queue the two dumps of the start of pon monitor
 * @alarms: the alarm bitmap of device 1
 *
 * The second dump goes out with the next sequence number, so its answer
 * carries that number.
 */
static void t_seed_reply(uint32_t alarms)
{
	struct nl_msg *msg;

	t_dev_reply(NLM_F_MULTI);
	t_fake_done(&fake, 0, 0, NULL);
	fake.client->s_seq_next++;
	msg = t_fake_reply(&fake, PON_CMD_ALARM_GET, NLM_F_MULTI);
	nla_put_u32(msg, PON_A_ALARM_DEV_ID, 1);
	nla_put_u32(msg, PON_A_ALARM_ALARMS, alarms);
	t_fake_send(&fake, pon.cmd.sock, msg);
	t_fake_done(&fake, 0, 0, NULL);
	fake.client->s_seq_next--;
}

/**
 * pon_dev_test - pon dev names the capacities of a device as limits
 *
 * The device id stays the second word and the pair "ploam O5" stays, as
 * the lab scripts read them.
 */
static void pon_dev_test(void)
{
	t_dev_reply(0);
	t_fake_ack(&fake, 0, NULL);
	t_out_start();
	CHECK_EQ(dev_cmd(T_ARGS("dev", "1")), 0);
	CHECK_STR(t_out_end(),
		  "dev 1 ifindex 9 mode xgs-pon ploam O5 max-tconts 32 max-gems 255 modes xgs-pon serial ADTN00000001 response-time 35005 enable 1\n");
	CHECK_EQ(t_fake_drain(&fake), 1);
}

/**
 * pon_monitor_ploam_test - pon monitor names a state in the mode of its device
 *
 * A notification of the activation state carries no mode. The monitor
 * takes the mode from the dump at its start, so the Serial Number state of
 * an XGS-PON device is O2-3, as pon dev and omcid name it. A device the
 * monitor does not know keeps the name without a mode.
 */
static void pon_monitor_ploam_test(void)
{
	struct pon_dev_info dev = {
		.dev_id = 1,
		.has = PON_DEV_F_PLOAM,
		.ploam = PON_PLOAM_STATE_O2,
	};

	t_seed_reply(0);
	CHECK_EQ(monitor_seed(), 0);
	CHECK_EQ(t_fake_drain(&fake), 2);

	t_out_start();
	ntf_ploam(&pon, &dev);
	dev.dev_id = 2;
	ntf_ploam(&pon, &dev);
	CHECK_STR(t_out_end(),
		  "ploam dev 1 state O2-3\n"
		  "ploam dev 2 state O2\n");
}

/**
 * pon_monitor_alarm_test - pon monitor prints each alarm raised or cleared
 *
 * The notification carries the whole bitmap. The monitor compares it with
 * the bitmap of the dump at its start and of the notification before: a
 * clear prints "cleared", a bitmap that did not change prints nothing.
 */
static void pon_monitor_alarm_test(void)
{
	struct pon_alarms alarms = { .dev_id = 1 };

	memset(monitor_devs, 0, sizeof(monitor_devs));
	t_seed_reply(1u << PON_ALARM_LODS);
	CHECK_EQ(monitor_seed(), 0);
	CHECK_EQ(t_fake_drain(&fake), 2);

	t_out_start();
	ntf_alarm(&pon, &alarms);
	ntf_alarm(&pon, &alarms);
	alarms.alarms = 1u << PON_ALARM_LODS;
	ntf_alarm(&pon, &alarms);
	alarms.dev_id = 3;
	ntf_alarm(&pon, &alarms);
	CHECK_STR(t_out_end(),
		  "alarm dev 1 cleared lods\n"
		  "alarm dev 1 raised lods\n"
		  "alarm dev 3 raised lods\n");
}

/**
 * pon_monitor_lost_test - pon monitor reads the alarms again after a loss
 *
 * A lost clear leaves the kept bitmap raised. After the loss the monitor
 * reads the bitmaps again, so the next raise prints its line.
 */
static void pon_monitor_lost_test(void)
{
	struct pon_alarms alarms = {
		.dev_id = 1,
		.alarms = 1u << PON_ALARM_LODS,
	};
	struct nl_sock *evt = pon.evt;

	monitor_dev_find(1, true)->alarms = 1u << PON_ALARM_LODS;
	pon.evt = NULL;
	t_seed_reply(0);
	monitor_events();
	pon.evt = evt;
	CHECK_EQ(t_fake_drain(&fake), 2);

	t_out_start();
	ntf_alarm(&pon, &alarms);
	CHECK_STR(t_out_end(), "alarm dev 1 raised lods\n");
}

/**
 * pon_monitor_dev_test - pon monitor names a device notification
 *
 * The line starts as the T-CONT and GEM port lines do, then shows the
 * device as pon dev does.
 */
static void pon_monitor_dev_test(void)
{
	struct pon_dev_info dev = {
		.dev_id = 2,
		.has = PON_DEV_F_PLOAM,
		.ploam = PON_PLOAM_STATE_O1,
	};

	t_out_start();
	ntf_dev(&pon, PON_CMD_DEV_ADD_NTF, &dev);
	ntf_dev(&pon, PON_CMD_DEV_CHANGE_NTF, &dev);
	ntf_dev(&pon, PON_CMD_DEV_DEL_NTF, &dev);
	CHECK_STR(t_out_end(),
		  "dev added: dev 2 ifindex 0 mode ? ploam O1\n"
		  "dev changed: dev 2 ifindex 0 mode ? ploam O1\n"
		  "dev removed: dev 2 ifindex 0 mode ? ploam O1\n");
}

/**
 * pon_stats_test - pon stats names each counter of a GEM port
 */
static void pon_stats_test(void)
{
	struct nl_msg *msg = t_fake_reply(&fake, PON_CMD_GEM_STATS_GET, 0);

	nla_put_u32(msg, PON_A_GEM_STATS_DEV_ID, 1);
	nla_put_u32(msg, PON_A_GEM_STATS_GEM_ID, 2306);
	nla_put_u32(msg, PON_A_GEM_STATS_RX_FRAMES, 45620);
	nla_put_u32(msg, PON_A_GEM_STATS_RX_BYTES, 67633461);
	nla_put_u32(msg, PON_A_GEM_STATS_TX_FRAMES, 16708);
	nla_put_u32(msg, PON_A_GEM_STATS_TX_BYTES, 1571420);
	t_fake_send(&fake, pon.cmd.sock, msg);
	t_fake_ack(&fake, 0, NULL);
	t_out_start();
	CHECK_EQ(stats_cmd(T_ARGS("stats", "1", "2306")), 0);
	CHECK_STR(t_out_end(),
		  "dev 1 gem 2306 rx-frames 45620 rx-bytes 67633461 tx-frames 16708 tx-bytes 1571420\n");
	CHECK_EQ(t_fake_drain(&fake), 1);
}

/**
 * pon_fec_test - pon fec names each counter as the family does
 */
static void pon_fec_test(void)
{
	struct nl_msg *msg = t_fake_reply(&fake, PON_CMD_FEC_GET, 0);

	nla_put_u32(msg, PON_A_FEC_DEV_ID, 1);
	nla_put_u32(msg, PON_A_FEC_CORRECTED, 18107);
	nla_put_u32(msg, PON_A_FEC_UNCORRECTABLE, 6704);
	nla_put_u32(msg, PON_A_FEC_CORRECTED_BYTES, 40252);
	nla_put_u64(msg, PON_A_FEC_TOTAL_CODEWORDS, 10500231032ull);
	nla_put_u32(msg, PON_A_FEC_SECONDS, 6);
	t_fake_send(&fake, pon.cmd.sock, msg);
	t_fake_ack(&fake, 0, NULL);
	t_out_start();
	CHECK_EQ(fec_cmd(T_ARGS("fec", "1")), 0);
	CHECK_STR(t_out_end(),
		  "dev 1 fec corrected 18107 uncorrectable 6704 corrected-bytes 40252 total-codewords 10500231032 seconds 6\n");
	CHECK_EQ(t_fake_drain(&fake), 1);
}

/**
 * pon_help_test - pon help prints the usage text on stdout and succeeds
 *
 * The three forms run before the tool opens the family, so they work
 * where the family is not registered, as in this test.
 */
static void pon_help_test(void)
{
	const char *text;

	t_out_start();
	CHECK_EQ(pon_tool_main(T_ARGS("pon", "help")), 0);
	CHECK_EQ(pon_tool_main(T_ARGS("pon", "-h")), 0);
	CHECK_EQ(pon_tool_main(T_ARGS("pon", "--help")), 0);
	text = t_out_end();
	CHECK(!strncmp(text, "usage: pon dev", strlen("usage: pon dev")));
	CHECK(strstr(text, "pon gem | gem maps"));
	CHECK_EQ(strlen(text) % 3, 0);
	CHECK(!strncmp(text + strlen(text) / 3, "usage:", strlen("usage:")));
}

/**
 * pon_extra_arguments_test - pon maps and pon monitor refuse an argument
 */
static void pon_extra_arguments_test(void)
{
	t_fake_ack(&fake, 0, NULL);
	t_refused(maps_cmd(T_ARGS("maps", "1")));
	t_fake_ack(&fake, 0, NULL);
	t_refused(monitor_run(T_ARGS("monitor", "1")));
}

/**
 * pon_reasons_test - a refused command line says why before the usage
 *
 * A wrong number of arguments and an unknown verb each print one line on
 * stderr; none sends a request.
 */
static void pon_reasons_test(void)
{
	t_fake_ack(&fake, 0, NULL);
	t_err_start();
	CHECK_EQ(tconts_cmd(T_ARGS("tconts", "1")), -EINVAL);
	CHECK_EQ(fec_cmd(T_ARGS("fec")), -EINVAL);
	CHECK_EQ(gem_cmd(T_ARGS("gem", "new", "1", "2306")), -EINVAL);
	CHECK_EQ(gem_cmd(T_ARGS("gem", "foo")), -EINVAL);
	CHECK_EQ(gem_cmd(T_ARGS("gem", "map", "add", "1")), -EINVAL);
	CHECK_EQ(gem_cmd(T_ARGS("gem", "map", "add", "1", "2306", "0",
				 "vid", "6")), -EINVAL);
	CHECK_EQ(gem_cmd(T_ARGS("gem", "netdev", "show")), -EINVAL);
	CHECK_EQ(gem_cmd(T_ARGS("gem", "mapper", "add")), -EINVAL);
	CHECK_EQ(tcont_cmd(T_ARGS("tcont", "foo", "1", "0")), -EINVAL);
	CHECK_EQ(tcont_cmd(T_ARGS("tcont", "set", "1", "0")), -EINVAL);
	CHECK_EQ(omci_cmd(T_ARGS("omci", "send", "1")), -EINVAL);
	CHECK_EQ(dev_cmd(T_ARGS("dev", "1", "2")), -EINVAL);
	CHECK_STR(t_out_end(),
		  "wrong number of arguments for tconts\n"
		  "wrong number of arguments for fec\n"
		  "wrong number of arguments for gem new\n"
		  "unknown command: gem foo\n"
		  "wrong number of arguments for gem map add\n"
		  "wrong number of arguments for gem map add\n"
		  "unknown command: gem netdev show\n"
		  "wrong number of arguments for gem mapper add\n"
		  "unknown command: tcont foo\n"
		  "wrong number of arguments for tcont set\n"
		  "wrong number of arguments for omci send\n"
		  "wrong number of arguments for dev\n");
	CHECK_EQ(t_fake_drain(&fake), 0);
	t_fake_recv(pon.cmd.sock);
}

/**
 * pon_gem_netdev_del_test - pon gem netdev del and pon gem mapper del
 * refuse a device of another kind
 *
 * A dummy device in the network namespace of the test stays after the
 * command.
 */
static void pon_gem_netdev_del_test(void)
{
	CHECK_EQ(system("ip link add t-dummy0 type dummy"), 0);
	t_err_start();
	CHECK_EQ(gem_netdev_cmd(T_ARGS("netdev", "del", "t-dummy0")),
		 ERR_REPORTED);
	CHECK_STR(t_out_end(), "not a GEM network device: t-dummy0\n");
	CHECK(if_nametoindex("t-dummy0"));
	t_err_start();
	CHECK_EQ(gem_mapper_cmd(T_ARGS("mapper", "del", "t-dummy0")),
		 ERR_REPORTED);
	CHECK_STR(t_out_end(),
		  "not a gem-mapper network device: t-dummy0\n");
	CHECK(if_nametoindex("t-dummy0"));
}

/**
 * t_tcont_reply - queue a T-CONT of device 1
 * @flags: the netlink flags, NLM_F_MULTI for a part of a dump
 * @index: the T-CONT index
 * @alloc_id: its Alloc-ID
 */
static void t_tcont_reply(int flags, uint32_t index, uint32_t alloc_id)
{
	struct nl_msg *msg = t_fake_reply(&fake, PON_CMD_TCONT_GET, flags);

	nla_put_u32(msg, PON_A_TCONT_DEV_ID, 1);
	nla_put_u32(msg, PON_A_TCONT_INDEX, index);
	nla_put_u32(msg, PON_A_TCONT_ALLOC_ID, alloc_id);
	t_fake_send(&fake, pon.cmd.sock, msg);
}

/**
 * t_gem_reply - queue one of the two GEM ports of the lab board
 * @flags: the netlink flags, NLM_F_MULTI for a part of a dump
 * @flood: GEM 4095, downstream and not offloaded, instead of GEM 2306
 */
static void t_gem_reply(int flags, bool flood)
{
	struct nl_msg *msg = t_fake_reply(&fake, PON_CMD_GEM_GET, flags);

	nla_put_u32(msg, PON_A_GEM_DEV_ID, 1);
	nla_put_u32(msg, PON_A_GEM_ID, flood ? 4095 : 2306);
	nla_put_u32(msg, PON_A_GEM_DIR,
		    flood ? PON_GEM_DIR_DOWNSTREAM : PON_GEM_DIR_BIDIR);
	if (!flood)
		nla_put_u32(msg, PON_A_GEM_TCONT_INDEX, 2);
	nla_put_u32(msg, PON_A_GEM_KEY_RING,
		    flood ? PON_GEM_KEY_RING_NONE :
			    PON_GEM_KEY_RING_UNICAST_DOWNSTREAM);
	nla_put_u32(msg, PON_A_GEM_QUEUE, 0);
	nla_put_u8(msg, PON_A_GEM_NO_OFFLOAD, flood);
	t_fake_send(&fake, pon.cmd.sock, msg);
}

/**
 * t_map_reply - queue a mapper rule of device 1
 * @flags: the netlink flags, NLM_F_MULTI for a part of a dump
 * @gem_id: the GEM port
 * @pbit: the P-bit
 * @ifindex: the gem-mapper network device, or 0 for the data interface
 */
static void t_map_reply(int flags, uint32_t gem_id, uint32_t pbit,
			uint32_t ifindex)
{
	struct nl_msg *msg = t_fake_reply(&fake, PON_CMD_GEM_MAP_GET, flags);

	nla_put_u32(msg, PON_A_GEM_MAP_DEV_ID, 1);
	nla_put_u32(msg, PON_A_GEM_MAP_GEM_ID, gem_id);
	nla_put_u32(msg, PON_A_GEM_MAP_PBIT, pbit);
	if (ifindex)
		nla_put_u32(msg, PON_A_GEM_MAP_IFINDEX, ifindex);
	t_fake_send(&fake, pon.cmd.sock, msg);
}

/**
 * t_alarm_reply - queue the alarms of device 1
 * @flags: the netlink flags, NLM_F_MULTI for a part of a dump
 * @alarms: the bitmap
 */
static void t_alarm_reply(int flags, uint32_t alarms)
{
	struct nl_msg *msg = t_fake_reply(&fake, PON_CMD_ALARM_GET, flags);

	nla_put_u32(msg, PON_A_ALARM_DEV_ID, 1);
	nla_put_u32(msg, PON_A_ALARM_ALARMS, alarms);
	t_fake_send(&fake, pon.cmd.sock, msg);
}

/**
 * t_tc_stats_reply - queue three counters of the TC layer of device 1
 */
static void t_tc_stats_reply(void)
{
	struct nl_msg *msg = t_fake_reply(&fake, PON_CMD_TC_STATS_GET, 0);

	struct nlattr *nest;

	nla_put_u32(msg, PON_A_TC_STATS_DEV_ID, 1);
	nest = nla_nest_start(msg, PON_A_TC_STATS_PLOAM_DS);
	nla_put_u32(msg, PON_A_PLOAM_DS_STATS_RX_DEACTIVATE_ONU_ID, 0);
	nla_put_u32(msg, PON_A_PLOAM_DS_STATS_RX_ASSIGN_ONU_ID, 4);
	nla_nest_end(msg, nest);
	nest = nla_nest_start(msg, PON_A_TC_STATS_PLOAM_US);
	nla_put_u32(msg, PON_A_PLOAM_US_STATS_TX_SERIAL_NUMBER_ONU, 4);
	nla_nest_end(msg, nest);
	t_fake_send(&fake, pon.cmd.sock, msg);
}

/**
 * t_omci_stats_reply - queue the counters of the OMCI channel of device 1
 */
static void t_omci_stats_reply(void)
{
	struct nl_msg *msg = t_fake_reply(&fake, PON_CMD_OMCI_STATS_GET, 0);

	nla_put_u32(msg, PON_A_OMCI_STATS_DEV_ID, 1);
	nla_put_u32(msg, PON_A_OMCI_STATS_RX, 9);
	nla_put_u32(msg, PON_A_OMCI_STATS_RX_DROPPED, 1);
	nla_put_u32(msg, PON_A_OMCI_STATS_RX_ERRORS, 0);
	nla_put_u32(msg, PON_A_OMCI_STATS_TX, 8);
	nla_put_u32(msg, PON_A_OMCI_STATS_TX_ERRORS, 2);
	t_fake_send(&fake, pon.cmd.sock, msg);
}

/**
 * pon_text_test - the text lines of the lists and the monitor
 *
 * The lab scripts and the recipes read these lines: the device id as the
 * second word, the counter names of tc-stats with their value in the
 * fifth field, the words of the monitor.
 */
static void pon_text_test(void)
{
	struct pon_event ev = {
		.dev_id = 1,
		.type = PON_EVENT_TYPE_TCONT_ALLOC,
		.has = PON_EVENT_F_ALLOC_ID,
		.alloc_id = 1027,
	};
	struct pon_tcont tcont = {
		.dev_id = 1, .index = 0, .alloc_id = 1155, .alloc_id_set = true,
	};
	struct pon_gem gem = {
		.dev_id = 1, .id = 2306, .dir = PON_GEM_DIR_BIDIR,
		.tcont_index = 2,
		.key_ring = PON_GEM_KEY_RING_UNICAST_DOWNSTREAM,
		.queue = 0, .no_offload = 0,
	};
	struct pon_gem_map map = {
		.dev_id = 1, .gem_id = 2306, .pbit = 6,
	};

	t_tcont_reply(NLM_F_MULTI, 0, 1155);
	t_tcont_reply(NLM_F_MULTI, 6, 1795);
	t_fake_done(&fake, 0, 0, NULL);
	t_out_start();
	CHECK_EQ(tconts_cmd(T_ARGS("tconts")), 0);
	CHECK_STR(t_out_end(),
		  "dev 1 tcont 0 alloc-id 1155\n"
		  "dev 1 tcont 6 alloc-id 1795\n");

	t_gem_reply(NLM_F_MULTI, false);
	t_gem_reply(NLM_F_MULTI, true);
	t_fake_done(&fake, 0, 0, NULL);
	t_out_start();
	CHECK_EQ(gems_cmd(T_ARGS("gems")), 0);
	CHECK_STR(t_out_end(),
		  "dev 1 gem 2306 dir bidir tcont 2 key-ring unicast-downstream queue 0\n"
		  "dev 1 gem 4095 dir downstream key-ring none queue 0 no-offload\n");

	t_map_reply(NLM_F_MULTI, 2306, 6, 0);
	t_map_reply(NLM_F_MULTI, 2308, 0, 9);
	t_fake_done(&fake, 0, 0, NULL);
	t_out_start();
	CHECK_EQ(maps_cmd(T_ARGS("maps")), 0);
	CHECK_STR(t_out_end(),
		  "dev 1 gem 2306 pbit 6\n"
		  "dev 1 gem 2308 pbit 0 mapper 9\n");

	t_alarm_reply(NLM_F_MULTI, 0);
	t_fake_done(&fake, 0, 0, NULL);
	t_out_start();
	CHECK_EQ(alarms_cmd(T_ARGS("alarms")), 0);
	CHECK_STR(t_out_end(), "dev 1 alarms none\n");

	t_alarm_reply(0, 1u << PON_ALARM_LODS);
	t_fake_ack(&fake, 0, NULL);
	t_out_start();
	CHECK_EQ(alarms_cmd(T_ARGS("alarms", "1")), 0);
	CHECK_STR(t_out_end(), "dev 1 alarms lods\n");

	t_tc_stats_reply();
	t_fake_ack(&fake, 0, NULL);
	t_out_start();
	CHECK_EQ(tc_stats_cmd(T_ARGS("tc-stats", "1")), 0);
	CHECK_STR(t_out_end(),
		  "dev 1 tc ploam-ds rx-deactivate-onu-id 0\n"
		  "dev 1 tc ploam-ds rx-assign-onu-id 4\n"
		  "dev 1 tc ploam-us tx-serial-number-onu 4\n");

	t_tc_stats_reply();
	t_fake_ack(&fake, 0, NULL);
	t_out_start();
	CHECK_EQ(tc_stats_cmd(T_ARGS("tc-stats", "1", "ploam-ds", "ploam-us")),
		 0);
	t_out_end();
	CHECK_EQ(tc_stats_cmd(T_ARGS("tc-stats", "1", "nonsense")), -EINVAL);

	t_omci_stats_reply();
	t_fake_ack(&fake, 0, NULL);
	t_out_start();
	CHECK_EQ(omci_stats_cmd(T_ARGS("omci-stats", "1")), 0);
	CHECK_STR(t_out_end(),
		  "dev 1 omci rx 9 rx-dropped 1 rx-errors 0 tx 8 tx-errors 2\n");
	CHECK_EQ(t_fake_drain(&fake), 8);

	t_out_start();
	ntf_event(&pon, &ev);
	ntf_tcont(&pon, PON_CMD_TCONT_ADD_NTF, &tcont);
	ntf_gem(&pon, PON_CMD_GEM_DEL_NTF, &gem);
	ntf_gem_map(&pon, PON_CMD_GEM_MAP_ADD_NTF, &map);
	CHECK_STR(t_out_end(),
		  "event dev 1 tcont-alloc alloc-id 1027\n"
		  "tcont added: dev 1 tcont 0 alloc-id 1155\n"
		  "gem removed: dev 1 gem 2306 dir bidir tcont 2 key-ring unicast-downstream queue 0\n"
		  "rule added: dev 1 gem 2306 pbit 6\n");
}

/**
 * pon_json_test - pon -j gives the objects under the names of the family
 *
 * A list is one object with one array, also for one object. A name of the
 * family takes an underscore for its dash; what the kernel did not send is
 * left out. The monitor gives one object per notification.
 */
static void pon_json_test(void)
{
	struct pon_dev_info dev = {
		.dev_id = 1,
		.has = PON_DEV_F_PLOAM,
		.ploam = PON_PLOAM_STATE_O2,
	};
	struct pon_event ev = {
		.dev_id = 1,
		.type = PON_EVENT_TYPE_TCONT_ALLOC,
		.has = PON_EVENT_F_ALLOC_ID,
		.alloc_id = 1027,
	};
	struct pon_tcont tcont = {
		.dev_id = 1, .index = 0, .alloc_id = 1155, .alloc_id_set = true,
	};
	struct pon_alarms alarms = { .dev_id = 1 };

	json = true;
	t_dev_reply(0);
	t_fake_ack(&fake, 0, NULL);
	t_out_start();
	CHECK_EQ(dev_cmd(T_ARGS("dev", "1")), 0);
	CHECK_STR(t_out_end(),
		  "{\"dev\":[{\"id\":1,\"ifindex\":9,\"mode\":\"xgs-pon\",\"ploam_state\":\"O5\",\"max_tconts\":32,\"max_gems\":255,\"modes_cap\":[\"xgs-pon\"],\"serial\":\"ADTN00000001\",\"response_time\":35005,\"enable\":1}]}\n");

	t_gem_reply(NLM_F_MULTI, false);
	t_gem_reply(NLM_F_MULTI, true);
	t_fake_done(&fake, 0, 0, NULL);
	t_out_start();
	CHECK_EQ(gem_cmd(T_ARGS("gem")), 0);
	CHECK_STR(t_out_end(),
		  "{\"gem\":[{\"dev_id\":1,\"id\":2306,\"dir\":\"bidir\",\"tcont_index\":2,\"key_ring\":\"unicast-downstream\",\"queue\":0,\"no_offload\":0},{\"dev_id\":1,\"id\":4095,\"dir\":\"downstream\",\"key_ring\":\"none\",\"queue\":0,\"no_offload\":1}]}\n");

	t_map_reply(NLM_F_MULTI, 2306, 6, 0);
	t_map_reply(NLM_F_MULTI, 2308, 0, 9);
	t_fake_done(&fake, 0, 0, NULL);
	t_out_start();
	CHECK_EQ(maps_cmd(T_ARGS("maps")), 0);
	CHECK_STR(t_out_end(),
		  "{\"gem_map\":[{\"dev_id\":1,\"gem_id\":2306,\"pbit\":6},{\"dev_id\":1,\"gem_id\":2308,\"pbit\":0,\"ifindex\":9}]}\n");

	t_alarm_reply(0, 1u << PON_ALARM_LODS);
	t_fake_ack(&fake, 0, NULL);
	t_out_start();
	CHECK_EQ(alarms_cmd(T_ARGS("alarms", "1")), 0);
	CHECK_STR(t_out_end(),
		  "{\"alarm\":[{\"dev_id\":1,\"alarms\":[\"lods\"]}]}\n");

	t_tc_stats_reply();
	t_fake_ack(&fake, 0, NULL);
	t_out_start();
	CHECK_EQ(tc_stats_cmd(T_ARGS("tc-stats", "1")), 0);
	CHECK_STR(t_out_end(),
		  "{\"tc_stats\":[{\"dev_id\":1,\"ploam_ds\":{\"rx_deactivate_onu_id\":0,\"rx_assign_onu_id\":4},\"ploam_us\":{\"tx_serial_number_onu\":4}}]}\n");

	t_omci_stats_reply();
	t_fake_ack(&fake, 0, NULL);
	t_out_start();
	CHECK_EQ(omci_stats_cmd(T_ARGS("omci-stats", "1")), 0);
	CHECK_STR(t_out_end(),
		  "{\"omci_stats\":[{\"dev_id\":1,\"rx\":9,\"rx_dropped\":1,\"rx_errors\":0,\"tx\":8,\"tx_errors\":2}]}\n");
	CHECK_EQ(t_fake_drain(&fake), 6);

	t_out_start();
	ntf_ploam(&pon, &dev);
	ntf_alarm(&pon, &alarms);
	ntf_tcont(&pon, PON_CMD_TCONT_ADD_NTF, &tcont);
	ntf_event(&pon, &ev);
	CHECK_STR(t_out_end(),
		  "{\"notification\":\"ploam-ntf\",\"dev\":{\"id\":1,\"ploam_state\":\"O2-3\"}}\n"
		  "{\"notification\":\"alarm-ntf\",\"alarm\":{\"dev_id\":1,\"alarms\":[]}}\n"
		  "{\"notification\":\"tcont-add-ntf\",\"tcont\":{\"dev_id\":1,\"index\":0,\"alloc_id\":1155}}\n"
		  "{\"notification\":\"event-ntf\",\"event\":{\"dev_id\":1,\"type\":\"tcont-alloc\",\"alloc_id\":1027}}\n");

	t_err_start();
	CHECK_EQ(pon_tool_main(T_ARGS("pon", "-j", "omci", "listen", "1")), 2);
	CHECK(!strncmp(t_out_end(), "pon omci has no JSON form\n",
		       strlen("pon omci has no JSON form\n")));
	json = false;
}

/**
 * main - run the cases of the command line against a fake family
 *
 * Return: 0 when every check passed, else 1.
 */
int main(void)
{
	if (pon_nl_open(&pon.cmd, NETLINK_GENERIC))
		return 1;
	pon.family = T_FAMILY;
	if (t_fake_open(&fake, pon.cmd.sock, T_FAMILY, PON_FAMILY_VERSION))
		return 1;
	pon_gem_new_test();
	pon_numbers_test();
	pon_arguments_test();
	pon_dev_test();
	pon_monitor_ploam_test();
	pon_monitor_alarm_test();
	pon_monitor_lost_test();
	pon_monitor_dev_test();
	pon_stats_test();
	pon_fec_test();
	pon_text_test();
	pon_json_test();
	pon_extra_arguments_test();
	pon_reasons_test();
	pon_gem_netdev_del_test();
	pon_help_test();
	t_fake_close(&fake);
	pon_close(&pon);
	return t_report("test-pon");
}
