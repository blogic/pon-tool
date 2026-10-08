/* SPDX-License-Identifier: LGPL-2.1-only */
/* Copyright (C) 2026 John Crispin <john@phrozen.org> */
#ifndef LIBPON_PON_H
#define LIBPON_PON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <linux/pon.h>

#define PON_NL_ERR_MAX		128
#define PON_EVENT_BUFFER	(256 * 1024)
#define PON_DEV_ANY		UINT32_MAX
#define PON_SERIAL_LEN		8
#define PON_VENDOR_ID_LEN	4
#define PON_SERIAL_TEXT_LEN	12
#define PON_SERIAL_HEX_LEN	16
#define PON_SERIAL_TEXT_MAX	PON_SERIAL_HEX_LEN
#define PON_REGID_MAX		36
#define PON_KEY_LEN		16
#define PON_OMCI_PDU_MIN	10
#define PON_OMCI_PDU_MAX	1976
#define PON_OMCI_BUFFER		(1024 * 1024)

#define PON_DEV_F_IFINDEX	(1u << 0)
#define PON_DEV_F_MODE		(1u << 2)
#define PON_DEV_F_MODES_CAP	(1u << 3)
#define PON_DEV_F_MAX_TCONTS	(1u << 4)
#define PON_DEV_F_MAX_GEMS	(1u << 5)
#define PON_DEV_F_PLOAM		(1u << 6)
#define PON_DEV_F_ENABLE	(1u << 7)
#define PON_DEV_F_RESPONSE_TIME	(1u << 9)
#define PON_DEV_F_SERIAL	(1u << 10)
#define PON_DEV_F_REGISTRATION_ID (1u << 11)
#define PON_DEV_F_DISABLED	(1u << 13)

#define PON_EVENT_F_ALLOC_ID	(1u << 0)
#define PON_EVENT_F_DEPTH	(1u << 1)
#define PON_EVENT_F_IMAGE	(1u << 2)
#define PON_EVENT_F_CALLS	(1u << 3)

struct nl_sock;
struct nl_msg;

/**
 * typedef pon_nl_show - handle one reply message of a request
 * @msg: the reply
 * @arg: the argument the caller gave pon_nl_run()
 *
 * Return: NL_OK, or NL_SKIP for a reply the handler does not take.
 */
typedef int (*pon_nl_show)(struct nl_msg *msg, void *arg);
/**
 * typedef pon_nl_trace - see one netlink message cross the socket
 * @arg: the trace_arg of the client
 * @tx: true for a request sent, false for a message received
 * @type: the netlink message type
 * @seq: the sequence number
 * @flags: the netlink flags
 * @len: the length of the message with its header
 */
typedef void (*pon_nl_trace)(void *arg, bool tx, uint16_t type, uint32_t seq,
			     uint16_t flags, uint32_t len);

/**
 * struct pon_nl - a client of one netlink protocol
 * @sock: the socket, NULL while closed
 * @err: 0, or the negative errno of the last request
 * @err_msg: the text of the last refusal; empty when the kernel sent none
 * @requests: the requests pon_nl_run() sent
 * @refusals: the requests that ended in an error
 * @trace: called for every message sent and received, or NULL
 * @trace_arg: passed to @trace
 * @interrupted: the last dump saw the list change while it ran
 */
struct pon_nl {
	struct nl_sock *sock;
	int err;
	char err_msg[PON_NL_ERR_MAX];
	uint64_t requests;
	uint64_t refusals;
	pon_nl_trace trace;
	void *trace_arg;
	bool interrupted;
};

/**
 * struct pon_dev_info - a PON device as the kernel reports it
 * @dev_id: the device
 * @has: one PON_DEV_F_ flag for each member below that the kernel sent
 * @ifindex: the ifindex of the PON data network device
 * @mode: the active enum pon_mode value
 * @modes_cap: the modes the device supports, one bit per enum pon_mode
 * @max_tconts: the number of T-CONTs the device supports
 * @max_gems: the number of GEM ports gem-new can create
 * @ploam: the activation state, an enum pon_ploam_state value
 * @enable: 1 when the upstream link was last started
 * @response_time: the response time of the ONU in nanoseconds
 * @disabled: 1 when the OLT disabled the ONU, which the kernel says in O7
 *	      only
 * @serial: the serial number, PON_SERIAL_LEN bytes
 */
struct pon_dev_info {
	uint32_t dev_id;
	uint32_t has;
	uint32_t ifindex;
	uint32_t mode;
	uint32_t modes_cap;
	uint32_t max_tconts;
	uint32_t max_gems;
	uint32_t ploam;
	uint32_t enable;
	uint32_t response_time;
	uint32_t disabled;
	uint8_t serial[PON_SERIAL_LEN];
};

/**
 * struct pon_dev_config - the settings pon_dev_set() sends to a device
 * @dev_id: the device
 * @has: one PON_DEV_F_ flag for each member below that is sent
 * @mode: the enum pon_mode value to run
 * @enable: 1 starts the upstream link, 0 stops it
 * @disabled: 1 when the OLT disabled the ONU; 1 is the only value
 * @serial: the serial number, PON_SERIAL_LEN bytes
 * @registration_id: the registration ID
 * @registration_id_len: its length, at most PON_REGID_MAX
 */
struct pon_dev_config {
	uint32_t dev_id;
	uint32_t has;
	uint32_t mode;
	uint8_t enable;
	uint8_t disabled;
	uint8_t serial[PON_SERIAL_LEN];
	uint8_t registration_id[PON_REGID_MAX];
	size_t registration_id_len;
};

/**
 * struct pon_tcont - a T-CONT of a device
 * @dev_id: the device
 * @index: the T-CONT index, from 0
 * @alloc_id: the Alloc-ID bound to the T-CONT
 * @alloc_id_set: the kernel sent @alloc_id
 */
struct pon_tcont {
	uint32_t dev_id;
	uint32_t index;
	uint32_t alloc_id;
	bool alloc_id_set;
};

/**
 * struct pon_gem - a GEM port of a device
 * @dev_id: the device
 * @id: the GEM port ID
 * @dir: the direction, an enum pon_gem_dir value; 0 when absent
 * @tcont_index: the T-CONT of the upstream half, or -1 for none
 * @key_ring: the encryption key ring, an enum pon_gem_key_ring value,
 *	      or -1 for none
 * @queue: the transmit queue within the T-CONT, or -1 for none
 * @no_offload: 1 when no flow of the port may be offloaded, 0 when one
 *		may, or -1 when the kernel did not say
 *
 * A negative member is left out of the request of pon_gem_new().
 */
struct pon_gem {
	uint32_t dev_id;
	uint32_t id;
	uint32_t dir;
	int tcont_index;
	int key_ring;
	int queue;
	int no_offload;
};

/**
 * struct pon_gem_map - a classifier rule that steers frames to a GEM port
 * @dev_id: the device
 * @gem_id: the GEM port the matching frames go to
 * @tag: the tag state to match, an enum pon_gem_map_tag value
 * @vid: the outer VLAN ID to match
 * @pbit: the outer VLAN priority to match
 * @dscp: the IP DSCP to match
 *
 * A negative match field is absent and matches anything.
 */
struct pon_gem_map {
	uint32_t dev_id;
	uint32_t gem_id;
	int tag;
	int vid;
	int pbit;
	int dscp;
};

/**
 * struct pon_alarms - the alarms of a device
 * @dev_id: the device
 * @alarms: the alarms raised now, one bit per enum pon_alarm value
 */
struct pon_alarms {
	uint32_t dev_id;
	uint32_t alarms;
};

/**
 * struct pon_gem_stats - the counters of a GEM port
 * @dev_id: the device
 * @gem_id: the GEM port
 * @rx_frames: the GEM frames received
 * @rx_bytes: the bytes received
 * @tx_frames: the GEM frames sent
 * @tx_bytes: the bytes sent
 */
struct pon_gem_stats {
	uint32_t dev_id;
	uint32_t gem_id;
	uint64_t rx_frames;
	uint64_t rx_bytes;
	uint64_t tx_frames;
	uint64_t tx_bytes;
};

/**
 * struct pon_fec_stats - the FEC counters of a device
 * @dev_id: the device
 * @corrected: the codewords FEC corrected
 * @uncorrectable: the codewords FEC could not correct
 * @corrected_bytes: the bytes FEC corrected
 * @total_codewords: the codewords received
 * @seconds: the seconds with at least one uncorrectable codeword
 */
struct pon_fec_stats {
	uint32_t dev_id;
	uint64_t corrected;
	uint64_t uncorrectable;
	uint64_t corrected_bytes;
	uint64_t total_codewords;
	uint64_t seconds;
};

/**
 * struct pon_tc_stats - the counters of the TC layer of a device
 * @dev_id: the device
 * @has: bit N set when the kernel sent @counter[N]
 * @counter: the counters, indexed by the PON_A_TC_STATS_ value of each
 */
struct pon_tc_stats {
	uint32_t dev_id;
	uint64_t has;
	uint64_t counter[PON_A_TC_STATS_MAX + 1];
};

/**
 * struct pon_event - an event of a device
 * @dev_id: the device
 * @type: what happened, an enum pon_event_type value
 * @has: one PON_EVENT_F_ flag for each member below that the kernel sent
 * @alloc_id: the Alloc-ID of a tcont-alloc or a tcont-dealloc
 * @depth: the reboot depth of a reboot-req
 * @image: the image a reboot-req asks to run
 * @calls: the condition on calls in progress of a reboot-req or a
 *	   mib-reset-req, an enum pon_reboot_calls value
 */
struct pon_event {
	uint32_t dev_id;
	uint32_t type;
	uint32_t has;
	uint32_t alloc_id;
	uint32_t depth;
	uint32_t image;
	uint32_t calls;
};

struct pon;

/**
 * struct pon_notify_ops - the callbacks of the notifications
 * @dev: a device was added, deleted or changed; @cmd is the
 *	 PON_CMD_DEV_ADD_NTF, PON_CMD_DEV_DEL_NTF or PON_CMD_DEV_CHANGE_NTF
 *	 the kernel sent
 * @ploam: the activation state of a device changed
 * @event: an event of a device
 * @alarm: the alarms of a device changed
 * @tcont: a T-CONT was added, changed or deleted; @cmd is the
 *	   notification the kernel sent
 * @gem: a GEM port was added or deleted; @cmd is the notification the
 *	 kernel sent
 * @gem_map: a classifier rule was added or deleted; @cmd is the
 *	     notification the kernel sent
 *
 * A callback may be NULL; its notifications are then dropped.
 */
struct pon_notify_ops {
	void (*dev)(struct pon *pon, uint8_t cmd,
		    const struct pon_dev_info *dev);
	void (*ploam)(struct pon *pon, const struct pon_dev_info *dev);
	void (*event)(struct pon *pon, const struct pon_event *ev);
	void (*alarm)(struct pon *pon, const struct pon_alarms *alarm);
	void (*tcont)(struct pon *pon, uint8_t cmd,
		      const struct pon_tcont *tcont);
	void (*gem)(struct pon *pon, uint8_t cmd, const struct pon_gem *gem);
	void (*gem_map)(struct pon *pon, uint8_t cmd,
			const struct pon_gem_map *map);
};

/**
 * struct pon - a client of the pon family
 * @cmd: the client of the requests
 * @evt: the socket of the notifications, or NULL for a client that only
 *	 sends requests
 * @family: the ID of the pon family
 * @ops: the callbacks of the notifications, or NULL
 * @priv: the context of the caller
 * @notifications: the notifications read
 */
struct pon {
	struct pon_nl cmd;
	struct nl_sock *evt;
	int family;
	const struct pon_notify_ops *ops;
	void *priv;
	uint64_t notifications;
};

/**
 * struct pon_omci - the OMCI channel of a PON device
 * @nl: the client of the channel
 * @family: the ID of the pon family
 * @dev_id: the device
 * @tx_refused: the PDUs the kernel refused
 * @tx_err: the negative errno of the last PDU refused
 */
struct pon_omci {
	struct pon_nl nl;
	int family;
	uint32_t dev_id;
	uint64_t tx_refused;
	int tx_err;
};

/**
 * typedef pon_dev_cb - see one device of a dump
 * @arg: the argument the caller gave the dump
 * @dev: the device
 */
typedef void (*pon_dev_cb)(void *arg, const struct pon_dev_info *dev);
/**
 * typedef pon_tcont_cb - see one T-CONT of a dump
 * @arg: the argument the caller gave the dump
 * @tcont: the T-CONT
 */
typedef void (*pon_tcont_cb)(void *arg, const struct pon_tcont *tcont);
/**
 * typedef pon_gem_cb - see one GEM port of a dump
 * @arg: the argument the caller gave the dump
 * @gem: the GEM port
 */
typedef void (*pon_gem_cb)(void *arg, const struct pon_gem *gem);
/**
 * typedef pon_gem_map_cb - see one classifier rule of a dump
 * @arg: the argument the caller gave the dump
 * @map: the rule
 */
typedef void (*pon_gem_map_cb)(void *arg, const struct pon_gem_map *map);
/**
 * typedef pon_alarms_cb - see the alarms of one device of a dump
 * @arg: the argument the caller gave the dump
 * @alarm: the alarms
 */
typedef void (*pon_alarms_cb)(void *arg, const struct pon_alarms *alarm);
/**
 * typedef pon_gem_stats_cb - see the counters of one GEM port of a dump
 * @arg: the argument the caller gave the dump
 * @stats: the counters
 */
typedef void (*pon_gem_stats_cb)(void *arg, const struct pon_gem_stats *stats);

int pon_nl_open(struct pon_nl *nl, int protocol);
void pon_nl_close(struct pon_nl *nl);
int pon_nl_run(struct pon_nl *nl, struct nl_msg *msg, pon_nl_show show,
	       void *arg);
const char *pon_nl_error(const struct pon_nl *nl);

int pon_open(struct pon *pon, const struct pon_notify_ops *ops, void *priv);
void pon_close(struct pon *pon);
const char *pon_error(const struct pon *pon);
int pon_event_fd(const struct pon *pon);
int pon_event_process(struct pon *pon);

int pon_dev_dump(struct pon *pon, pon_dev_cb cb, void *arg);
int pon_dev_get(struct pon *pon, uint32_t dev_id, struct pon_dev_info *out);
int pon_dev_set(struct pon *pon, const struct pon_dev_config *cfg);
int pon_dev_msk_set(struct pon *pon, uint32_t dev_id, const uint8_t *msk);
int pon_bcast_key_set(struct pon *pon, uint32_t dev_id, uint32_t index,
		      const uint8_t *key);
int pon_bcast_key_del(struct pon *pon, uint32_t dev_id, uint32_t index);

int pon_tcont_set(struct pon *pon, uint32_t dev_id, uint32_t index,
		  uint32_t alloc_id);
int pon_tcont_del(struct pon *pon, uint32_t dev_id, uint32_t index);
int pon_tcont_get(struct pon *pon, uint32_t dev_id, uint32_t index,
		  struct pon_tcont *out);
int pon_tcont_dump(struct pon *pon, uint32_t dev_id, pon_tcont_cb cb,
		   void *arg);

int pon_gem_new(struct pon *pon, const struct pon_gem *gem);
int pon_gem_del(struct pon *pon, uint32_t dev_id, uint32_t id);
int pon_gem_get(struct pon *pon, uint32_t dev_id, uint32_t id,
		struct pon_gem *out);
int pon_gem_dump(struct pon *pon, uint32_t dev_id, pon_gem_cb cb, void *arg);

int pon_gem_map_new(struct pon *pon, const struct pon_gem_map *map);
int pon_gem_map_del(struct pon *pon, const struct pon_gem_map *map);
int pon_gem_map_dump(struct pon *pon, uint32_t dev_id, pon_gem_map_cb cb,
		     void *arg);

int pon_alarm_get(struct pon *pon, uint32_t dev_id, struct pon_alarms *out);
int pon_alarm_dump(struct pon *pon, uint32_t dev_id, pon_alarms_cb cb,
		   void *arg);

int pon_stats_get(struct pon *pon, uint32_t dev_id, uint32_t gem_id,
		  struct pon_gem_stats *out);
int pon_stats_dump(struct pon *pon, uint32_t dev_id, pon_gem_stats_cb cb,
		   void *arg);
int pon_fec_get(struct pon *pon, uint32_t dev_id, struct pon_fec_stats *out);
int pon_tc_stats_get(struct pon *pon, uint32_t dev_id,
		     struct pon_tc_stats *out);

/**
 * pon_tc_stats_has - whether the reply carried a counter of the TC layer
 * @stats: the counters pon_tc_stats_get() filled
 * @counter: a PON_A_TC_STATS_ value, the index into @stats->counter
 *
 * The kernel leaves out a counter that the device does not keep, so a
 * counter that is absent differs from one that stands at 0.
 *
 * Return: true when the reply carried the counter.
 */
static inline bool pon_tc_stats_has(const struct pon_tc_stats *stats,
				    unsigned int counter)
{
	return counter <= PON_A_TC_STATS_MAX &&
	       (stats->has & (1ull << counter));
}

int pon_omci_open(struct pon_omci *omci, uint32_t dev_id);
void pon_omci_close(struct pon_omci *omci);
int pon_omci_fd(const struct pon_omci *omci);
int pon_omci_send(struct pon_omci *omci, const uint8_t *pdu, size_t len);
int pon_omci_recv(struct pon_omci *omci, uint8_t *buf, size_t size);

int pon_gem_link_add(struct pon_nl *rtnl, const char *name, unsigned int lower,
		     uint32_t gem_id, const uint8_t *mac);
int pon_gem_link_del(struct pon_nl *rtnl, unsigned int ifindex);

const char *pon_mode_name(uint32_t mode);
const char *pon_ploam_state_name(uint32_t state);
const char *pon_ploam_state_name_mode(uint32_t state, uint32_t mode);
const char *pon_alarm_name(unsigned int bit);
const char *pon_event_type_name(uint32_t type);
const char *pon_gem_dir_name(uint32_t dir);
const char *pon_gem_key_ring_name(uint32_t ring);
const char *pon_gem_map_tag_name(uint32_t tag);
const char *pon_tc_stat_name(unsigned int counter);
int pon_mode_value(const char *text);
int pon_gem_dir_value(const char *text);
int pon_gem_key_ring_value(const char *text);
int pon_gem_map_tag_value(const char *text);
int pon_number_parse(const char *text, uint32_t min, uint32_t max,
		     uint32_t *out);
int pon_hex_parse(const char *text, uint8_t *out, size_t max, size_t *len);
int pon_serial_parse(const char *text, uint8_t *serial);
void pon_serial_format(const uint8_t *serial, char *text, size_t size);

#endif
