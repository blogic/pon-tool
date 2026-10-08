// SPDX-License-Identifier: LGPL-2.1-only
/* Copyright (C) 2026 John Crispin <john@phrozen.org> */
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "internal.h"

static const char * const mode_names[] = {
	[PON_MODE_GPON] = "gpon",
	[PON_MODE_XG_PON] = "xg-pon",
	[PON_MODE_XGS_PON] = "xgs-pon",
};

static const char * const ploam_names[] = {
	[PON_PLOAM_STATE_UNKNOWN] = "unknown",
	[PON_PLOAM_STATE_O1] = "O1",
	[PON_PLOAM_STATE_O2] = "O2",
	[PON_PLOAM_STATE_O3] = "O3",
	[PON_PLOAM_STATE_O4] = "O4",
	[PON_PLOAM_STATE_O5] = "O5",
	[PON_PLOAM_STATE_O6] = "O6",
	[PON_PLOAM_STATE_O7] = "O7",
};

static const char * const alarm_names[] = {
	[PON_ALARM_LODS] = "lods",
};

static const char * const event_names[] = {
	[PON_EVENT_TYPE_MIB_RESET_REQ] = "mib-reset-req",
	[PON_EVENT_TYPE_REBOOT_REQ] = "reboot-req",
	[PON_EVENT_TYPE_TCONT_ALLOC] = "tcont-alloc",
	[PON_EVENT_TYPE_TCONT_DEALLOC] = "tcont-dealloc",
};

static const char * const dir_names[] = {
	[PON_GEM_DIR_UPSTREAM] = "upstream",
	[PON_GEM_DIR_DOWNSTREAM] = "downstream",
	[PON_GEM_DIR_BIDIR] = "bidir",
};

static const char * const key_ring_names[] = {
	[PON_GEM_KEY_RING_NONE] = "none",
	[PON_GEM_KEY_RING_UNICAST] = "unicast",
	[PON_GEM_KEY_RING_BROADCAST] = "broadcast",
	[PON_GEM_KEY_RING_UNICAST_DOWNSTREAM] = "unicast-downstream",
};

static const char * const xgtc_stat_names[] = {
	[PON_A_XGTC_STATS_PSBD_HEC_ERRORS] = "psbd-hec-errors",
	[PON_A_XGTC_STATS_XGTC_HEC_ERRORS] = "xgtc-hec-errors",
	[PON_A_XGTC_STATS_UNKNOWN_PROFILES] = "unknown-profiles",
	[PON_A_XGTC_STATS_TX_XGEM_FRAMES] = "tx-xgem-frames",
	[PON_A_XGTC_STATS_TX_XGEM_FRAGMENTS] = "tx-xgem-fragments",
	[PON_A_XGTC_STATS_XGEM_HEC_LOST_WORDS] = "xgem-hec-lost-words",
	[PON_A_XGTC_STATS_XGEM_KEY_ERRORS] = "xgem-key-errors",
	[PON_A_XGTC_STATS_XGEM_HEC_ERRORS] = "xgem-hec-errors",
	[PON_A_XGTC_STATS_TX_XGEM_BYTES] = "tx-xgem-bytes",
	[PON_A_XGTC_STATS_RX_XGEM_BYTES] = "rx-xgem-bytes",
	[PON_A_XGTC_STATS_LODS_EVENTS] = "lods-events",
	[PON_A_XGTC_STATS_LODS_RESTORED] = "lods-restored",
	[PON_A_XGTC_STATS_LODS_REACTIVATIONS] = "lods-reactivations",
};

static const char * const ploam_ds_stat_names[] = {
	[PON_A_PLOAM_DS_STATS_PLOAM_MIC_ERRORS] = "ploam-mic-errors",
	[PON_A_PLOAM_DS_STATS_OMCI_MIC_ERRORS] = "omci-mic-errors",
	[PON_A_PLOAM_DS_STATS_RX_PLOAM] = "rx-ploam",
	[PON_A_PLOAM_DS_STATS_RX_BURST_PROFILE] = "rx-burst-profile",
	[PON_A_PLOAM_DS_STATS_RX_RANGING_TIME] = "rx-ranging-time",
	[PON_A_PLOAM_DS_STATS_RX_DEACTIVATE_ONU_ID] = "rx-deactivate-onu-id",
	[PON_A_PLOAM_DS_STATS_RX_DISABLE_SERIAL_NUMBER] = "rx-disable-serial-number",
	[PON_A_PLOAM_DS_STATS_RX_REQUEST_REGISTRATION] = "rx-request-registration",
	[PON_A_PLOAM_DS_STATS_RX_ASSIGN_ALLOC_ID] = "rx-assign-alloc-id",
	[PON_A_PLOAM_DS_STATS_RX_KEY_CONTROL] = "rx-key-control",
	[PON_A_PLOAM_DS_STATS_RX_SLEEP_ALLOW] = "rx-sleep-allow",
	[PON_A_PLOAM_DS_STATS_RX_ASSIGN_ONU_ID] = "rx-assign-onu-id",
};

static const char * const ploam_us_stat_names[] = {
	[PON_A_PLOAM_US_STATS_TX_PLOAM] = "tx-ploam",
	[PON_A_PLOAM_US_STATS_TX_SERIAL_NUMBER_ONU] = "tx-serial-number-onu",
	[PON_A_PLOAM_US_STATS_TX_REGISTRATION] = "tx-registration",
	[PON_A_PLOAM_US_STATS_TX_KEY_REPORT] = "tx-key-report",
	[PON_A_PLOAM_US_STATS_TX_ACKNOWLEDGE] = "tx-acknowledge",
	[PON_A_PLOAM_US_STATS_TX_SLEEP_REQUEST] = "tx-sleep-request",
};

static const char * const tc_group_names[] = {
	[PON_TC_STATS_GROUP_XGTC] = "xgtc",
	[PON_TC_STATS_GROUP_PLOAM_DS] = "ploam-ds",
	[PON_TC_STATS_GROUP_PLOAM_US] = "ploam-us",
};

static const struct {
	const char * const *names;
	size_t count;
} tc_stat_names[PON_TC_GROUPS] = {
	[PON_TC_STATS_GROUP_XGTC] = {
		xgtc_stat_names, ARRAY_SIZE(xgtc_stat_names)
	},
	[PON_TC_STATS_GROUP_PLOAM_DS] = {
		ploam_ds_stat_names, ARRAY_SIZE(ploam_ds_stat_names)
	},
	[PON_TC_STATS_GROUP_PLOAM_US] = {
		ploam_us_stat_names, ARRAY_SIZE(ploam_us_stat_names)
	},
};

/**
 * name_get - the name of a value of a name table
 * @names: the names, indexed by value
 * @count: the number of entries in @names
 * @value: the value
 *
 * Return: the name, or "?" for a value without one.
 */
static const char *name_get(const char * const *names, size_t count,
			    uint32_t value)
{
	return value < count && names[value] ? names[value] : "?";
}

/**
 * pon_number_parse - a decimal number within a range
 * @text: decimal digits without a sign
 * @min: the smallest value taken
 * @max: the largest value taken
 * @out: the value, written only on success
 *
 * Return: 0, or -1 when @text is not a number from @min to @max.
 */
int pon_number_parse(const char *text, uint32_t min, uint32_t max,
		     uint32_t *out)
{
	unsigned long long value;
	char *end;

	if (text[0] < '0' || text[0] > '9')
		return -1;
	errno = 0;
	value = strtoull(text, &end, 10);
	if (errno || *end || value < min || value > max)
		return -1;
	*out = value;
	return 0;
}

/**
 * value_get - a value of a name table from its name or its number
 * @names: the names, indexed by value
 * @count: the number of entries in @names
 * @text: a name of @names, or the decimal number of a named value
 *
 * Return: the value, or -1 when @text is neither.
 */
static int value_get(const char * const *names, size_t count, const char *text)
{
	uint32_t value;
	size_t i;

	for (i = 0; i < count; i++)
		if (names[i] && !strcmp(names[i], text))
			return (int)i;
	if (pon_number_parse(text, 0, count - 1, &value) || !names[value])
		return -1;
	return (int)value;
}

/**
 * pon_mode_name - the name of a PON mode
 * @mode: an enum pon_mode value
 *
 * Return: the name, or "?" for a value the family does not define.
 */
const char *pon_mode_name(uint32_t mode)
{
	return name_get(mode_names, ARRAY_SIZE(mode_names), mode);
}

/**
 * pon_ploam_state_name - the name of an activation state
 * @state: an enum pon_ploam_state value
 *
 * Return: "O1" to "O7", "unknown", or "?" for a value the family does not
 * define.
 */
const char *pon_ploam_state_name(uint32_t state)
{
	return name_get(ploam_names, ARRAY_SIZE(ploam_names), state);
}

/**
 * pon_ploam_state_name_mode - the name of an activation state in a mode
 * @state: an enum pon_ploam_state value
 * @mode: the enum pon_mode value of the device
 *
 * ITU-T G.9807.1 Table C.12.1 has one Serial Number state, O2-3, which
 * the kernel reports as O2. Only a G-PON device has an O2 of its own.
 *
 * Return: "O2-3" for O2 of a mode other than G-PON, else the name that
 * pon_ploam_state_name() gives.
 */
const char *pon_ploam_state_name_mode(uint32_t state, uint32_t mode)
{
	if (state == PON_PLOAM_STATE_O2 && mode != PON_MODE_GPON)
		return "O2-3";
	return pon_ploam_state_name(state);
}

/**
 * pon_alarm_name - the name of an alarm
 * @bit: the bit of the alarm bitmap, an enum pon_alarm value
 *
 * Return: the name, or "?" for a bit the family does not define.
 */
const char *pon_alarm_name(unsigned int bit)
{
	return name_get(alarm_names, ARRAY_SIZE(alarm_names), bit);
}

/**
 * pon_event_type_name - the name of an event type
 * @type: an enum pon_event_type value
 *
 * Return: the name, or "?" for a value the family does not define.
 */
const char *pon_event_type_name(uint32_t type)
{
	return name_get(event_names, ARRAY_SIZE(event_names), type);
}

/**
 * pon_gem_dir_name - the name of a GEM port's direction
 * @dir: an enum pon_gem_dir value
 *
 * Return: the name, or "?" for a value the family does not define.
 */
const char *pon_gem_dir_name(uint32_t dir)
{
	return name_get(dir_names, ARRAY_SIZE(dir_names), dir);
}

/**
 * pon_gem_key_ring_name - the name of a GEM port's encryption key ring
 * @ring: an enum pon_gem_key_ring value
 *
 * Return: the name, or "?" for a value the family does not define.
 */
const char *pon_gem_key_ring_name(uint32_t ring)
{
	return name_get(key_ring_names, ARRAY_SIZE(key_ring_names), ring);
}

/**
 * pon_tc_group_name - the name of a group of the counters of the TC layer
 * @group: an enum pon_tc_stats_group value
 *
 * Return: the name the netlink specification gives the group, or "?" for a
 * value that is not a group of the family.
 */
const char *pon_tc_group_name(unsigned int group)
{
	return name_get(tc_group_names, ARRAY_SIZE(tc_group_names), group);
}

/**
 * pon_tc_stat_name - the name of a counter of the TC layer
 * @group: an enum pon_tc_stats_group value
 * @counter: the attribute value of the counter within the attribute set of
 *	     @group, the index into struct pon_tc_group
 *
 * Return: the name the netlink specification gives the counter, or "?" for
 * a value that is not a counter of the group.
 */
const char *pon_tc_stat_name(unsigned int group, unsigned int counter)
{
	if (group >= PON_TC_GROUPS)
		return "?";

	return name_get(tc_stat_names[group].names, tc_stat_names[group].count,
			counter);
}

/**
 * pon_tc_group_value - a group of the counters of the TC layer from its name
 * @text: the name pon_tc_group_name() gives, or a decimal number
 *
 * Return: the value, or -1 when @text is neither.
 */
int pon_tc_group_value(const char *text)
{
	return value_get(tc_group_names, ARRAY_SIZE(tc_group_names), text);
}

/**
 * pon_mode_value - a PON mode from its name or its number
 * @text: the name pon_mode_name() gives, or a decimal number
 *
 * Return: the value, or -1 when @text is neither.
 */
int pon_mode_value(const char *text)
{
	return value_get(mode_names, ARRAY_SIZE(mode_names), text);
}

/**
 * pon_gem_dir_value - a GEM port's direction from its name or its number
 * @text: the name pon_gem_dir_name() gives, or a decimal number
 *
 * Return: the value, or -1 when @text is neither.
 */
int pon_gem_dir_value(const char *text)
{
	return value_get(dir_names, ARRAY_SIZE(dir_names), text);
}

/**
 * pon_gem_key_ring_value - an encryption key ring from its name or its number
 * @text: the name pon_gem_key_ring_name() gives, or a decimal number
 *
 * Return: the value, or -1 when @text is neither.
 */
int pon_gem_key_ring_value(const char *text)
{
	return value_get(key_ring_names, ARRAY_SIZE(key_ring_names), text);
}

/**
 * hex_nibble - the value of a hex digit
 * @digit: the character
 *
 * Return: 0 to 15, or -1 for a character that is not a hex digit.
 */
static int hex_nibble(char digit)
{
	if (digit >= '0' && digit <= '9')
		return digit - '0';
	if (digit >= 'a' && digit <= 'f')
		return digit - 'a' + 10;
	if (digit >= 'A' && digit <= 'F')
		return digit - 'A' + 10;
	return -1;
}

/**
 * pon_hex_parse - decode a string of hex digits
 * @text: an even number of hex digits, no prefix
 * @out: the bytes
 * @max: the room in @out
 * @len: the number of bytes written
 *
 * The whole text is checked first; a text that fails leaves @out and @len
 * as they were.
 *
 * Return: 0, or -1 for an odd length, a non-hex digit or too many bytes.
 */
int pon_hex_parse(const char *text, uint8_t *out, size_t max, size_t *len)
{
	size_t count = strlen(text), i;

	if (count % 2 || count / 2 > max)
		return -1;
	for (i = 0; i < count; i++) {
		if (hex_nibble(text[i]) < 0)
			return -1;
	}
	for (i = 0; i < count / 2; i++)
		out[i] = hex_nibble(text[i * 2]) << 4 |
			 hex_nibble(text[i * 2 + 1]);
	*len = count / 2;
	return 0;
}

/**
 * vendor_char - whether a Vendor_ID byte has a character in the text form
 * @byte: the byte
 *
 * ITU-T G.9807.1 clause C.11.2.6.1 makes the Vendor_ID an alphanumeric
 * label of four ASCII characters.
 *
 * Return: true for an upper case letter or a digit.
 */
static bool vendor_char(uint8_t byte)
{
	return (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9');
}

/**
 * pon_serial_parse - decode a serial number as pon_serial_format() writes it
 * @text: four vendor characters followed by eight hex digits, or sixteen
 *	  hex digits
 * @serial: the PON_SERIAL_LEN binary bytes, written only on success
 *
 * A vendor character is an upper case letter or a digit.
 *
 * Return: 0, or -1 when the text has another shape.
 */
int pon_serial_parse(const char *text, uint8_t *serial)
{
	uint8_t bytes[PON_SERIAL_LEN];
	size_t len = strlen(text), count, i;

	if (len == PON_SERIAL_HEX_LEN)
		return pon_hex_parse(text, serial, PON_SERIAL_LEN, &count);
	if (len != PON_SERIAL_TEXT_LEN)
		return -1;
	for (i = 0; i < PON_VENDOR_ID_LEN; i++) {
		if (!vendor_char(text[i]))
			return -1;
		bytes[i] = text[i];
	}
	if (pon_hex_parse(text + PON_VENDOR_ID_LEN, bytes + PON_VENDOR_ID_LEN,
			  PON_SERIAL_LEN - PON_VENDOR_ID_LEN, &count))
		return -1;
	memcpy(serial, bytes, PON_SERIAL_LEN);
	return 0;
}

/**
 * pon_serial_format - write a serial number as text
 * @serial: the PON_SERIAL_LEN binary bytes
 * @text: the buffer, at least PON_SERIAL_TEXT_MAX + 1 bytes to hold it all
 * @size: the size of @text
 *
 * The Vendor_ID is written as its four characters when each is an upper
 * case letter or a digit, the VSSN as eight hex digits. A Vendor_ID with
 * another byte, such as the 0x80 of an RE embedded ONU in ITU-T G.9807.1
 * clause C.11.2.6.1, makes the whole serial number sixteen hex digits.
 */
void pon_serial_format(const uint8_t *serial, char *text, size_t size)
{
	size_t i;

	for (i = 0; i < PON_VENDOR_ID_LEN; i++) {
		if (!vendor_char(serial[i]))
			break;
	}
	if (i < PON_VENDOR_ID_LEN) {
		snprintf(text, size, "%02X%02X%02X%02X%02X%02X%02X%02X",
			 serial[0], serial[1], serial[2], serial[3], serial[4],
			 serial[5], serial[6], serial[7]);
		return;
	}
	snprintf(text, size, "%c%c%c%c%02X%02X%02X%02X", serial[0], serial[1],
		 serial[2], serial[3], serial[4], serial[5], serial[6],
		 serial[7]);
}
