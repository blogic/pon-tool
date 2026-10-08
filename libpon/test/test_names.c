// SPDX-License-Identifier: LGPL-2.1-only
/* Copyright (C) 2026 John Crispin <john@phrozen.org> */
#include <stdint.h>
#include <string.h>

#include "libpon/pon.h"
#include "test.h"

/**
 * names_enum_test - every value of the uapi enums has a name
 */
static void names_enum_test(void)
{
	unsigned int counter;

	CHECK_STR(pon_mode_name(PON_MODE_XGS_PON), "xgs-pon");
	CHECK_STR(pon_mode_name(PON_MODE_XGS_PON + 1), "?");
	CHECK_STR(pon_ploam_state_name(PON_PLOAM_STATE_O5), "O5");
	CHECK_STR(pon_alarm_name(PON_ALARM_LODS), "lods");
	CHECK_STR(pon_event_type_name(PON_EVENT_TYPE_TCONT_DEALLOC),
		  "tcont-dealloc");
	CHECK_STR(pon_gem_key_ring_name(PON_GEM_KEY_RING_UNICAST_DOWNSTREAM),
		  "unicast-downstream");
	CHECK_STR(pon_gem_map_tag_name(PON_GEM_MAP_TAG_TAGGED), "tagged");
	for (counter = PON_A_TC_STATS_DEV_ID + 1;
	     counter <= PON_A_TC_STATS_MAX; counter++)
		CHECK(strcmp(pon_tc_stat_name(counter), "?"));
	CHECK_STR(pon_tc_stat_name(PON_A_TC_STATS_MAX + 1), "?");
}

/**
 * names_ploam_mode_test - the Serial Number state is O2-3 outside G-PON
 */
static void names_ploam_mode_test(void)
{
	CHECK_STR(pon_ploam_state_name_mode(PON_PLOAM_STATE_O2, PON_MODE_GPON),
		  "O2");
	CHECK_STR(pon_ploam_state_name_mode(PON_PLOAM_STATE_O2,
					    PON_MODE_XGS_PON), "O2-3");
	CHECK_STR(pon_ploam_state_name_mode(PON_PLOAM_STATE_O2,
					    PON_MODE_XG_PON), "O2-3");
	CHECK_STR(pon_ploam_state_name_mode(PON_PLOAM_STATE_O5,
					    PON_MODE_XGS_PON), "O5");
}

/**
 * names_value_test - a value is found by its name or its number
 */
static void names_value_test(void)
{
	CHECK_EQ(pon_mode_value("xg-pon"), PON_MODE_XG_PON);
	CHECK_EQ(pon_mode_value("2"), PON_MODE_XGS_PON);
	CHECK_EQ(pon_mode_value("lte"), -1);
	CHECK_EQ(pon_gem_key_ring_value("unicast"), PON_GEM_KEY_RING_UNICAST);
	CHECK_EQ(pon_gem_map_tag_value("untagged"), PON_GEM_MAP_TAG_UNTAGGED);
	CHECK_STR(pon_gem_dir_name(PON_GEM_DIR_DOWNSTREAM), "downstream");
	CHECK_STR(pon_gem_dir_name(0), "?");
	CHECK_EQ(pon_gem_dir_value("bidir"), PON_GEM_DIR_BIDIR);
	CHECK_EQ(pon_gem_dir_value("1"), PON_GEM_DIR_UPSTREAM);
	CHECK_EQ(pon_gem_dir_value("0"), -1);
}

/**
 * names_value_refuse_test - a number with junk or out of range is refused
 */
static void names_value_refuse_test(void)
{
	uint32_t value = 5;

	CHECK_EQ(pon_mode_value("7x"), -1);
	CHECK_EQ(pon_mode_value("99"), -1);
	CHECK_EQ(pon_mode_value("4294967296"), -1);
	CHECK_EQ(pon_mode_value("-1"), -1);
	CHECK_EQ(pon_mode_value(""), -1);
	CHECK_EQ(pon_gem_map_tag_value("2"), -1);
	CHECK_EQ(pon_gem_key_ring_value("3"),
		 PON_GEM_KEY_RING_UNICAST_DOWNSTREAM);

	CHECK_EQ(pon_number_parse("4294967295", 0, UINT32_MAX, &value), 0);
	CHECK_EQ(value, UINT32_MAX);
	CHECK_EQ(pon_number_parse("4294967296", 0, UINT32_MAX, &value), -1);
	CHECK_EQ(pon_number_parse("0", 1, 1, &value), -1);
	CHECK_EQ(pon_number_parse(" 1", 1, 1, &value), -1);
	CHECK_EQ(pon_number_parse("+1", 1, 1, &value), -1);
	CHECK_EQ(value, UINT32_MAX);
}

/**
 * names_hex_test - pon_hex_parse() decodes pairs of digits
 */
static void names_hex_test(void)
{
	uint8_t out[4];
	size_t len = 0;

	CHECK_EQ(pon_hex_parse("0aFf10", out, sizeof(out), &len), 0);
	CHECK_EQ(len, 3);
	CHECK_EQ(out[0], 0x0a);
	CHECK_EQ(out[1], 0xff);
	CHECK_EQ(out[2], 0x10);
	CHECK_EQ(pon_hex_parse("abc", out, sizeof(out), &len), -1);
	CHECK_EQ(pon_hex_parse("0102030405", out, sizeof(out), &len), -1);
	CHECK_EQ(len, 3);
}

/**
 * names_hex_fail_test - a text with a bad digit leaves the output alone
 */
static void names_hex_fail_test(void)
{
	uint8_t out[8] = "HELLO";
	size_t len = 5;

	CHECK_EQ(pon_hex_parse("4142zz", out, sizeof(out), &len), -1);
	CHECK_EQ(len, 5);
	CHECK(!memcmp(out, "HELLO", 5));
}

/**
 * names_serial_test - a serial number of four letters survives the text
 */
static void names_serial_test(void)
{
	static const uint8_t serial[PON_SERIAL_LEN] = {
		'A', 'D', 'T', 'N', 0x00, 0x00, 0x00, 0x01
	};
	char text[PON_SERIAL_TEXT_LEN + 1];
	uint8_t back[PON_SERIAL_LEN];

	pon_serial_format(serial, text, sizeof(text));
	CHECK_STR(text, "ADTN00000001");
	CHECK_EQ(pon_serial_parse(text, back), 0);
	CHECK(!memcmp(back, serial, sizeof(back)));
	CHECK_EQ(pon_serial_parse("ADTN0000001", back), -1);
}

/**
 * names_serial_hex_test - a Vendor_ID without four characters is hex
 */
static void names_serial_hex_test(void)
{
	static const uint8_t nul[PON_SERIAL_LEN] = {
		'A', 0x00, 'C', 'D', 0x01, 0x02, 0x03, 0x04
	};
	static const uint8_t re_onu[PON_SERIAL_LEN] = {
		0xc1, 'B', 'C', 'D', 0x01, 0x02, 0x03, 0x04
	};
	char text[PON_SERIAL_TEXT_MAX + 1];
	uint8_t back[PON_SERIAL_LEN];

	pon_serial_format(nul, text, sizeof(text));
	CHECK_STR(text, "4100434401020304");
	CHECK_EQ(pon_serial_parse(text, back), 0);
	CHECK(!memcmp(back, nul, sizeof(back)));

	pon_serial_format(re_onu, text, sizeof(text));
	CHECK_STR(text, "C142434401020304");
	CHECK_EQ(pon_serial_parse(text, back), 0);
	CHECK(!memcmp(back, re_onu, sizeof(back)));
}

/**
 * names_serial_refuse_test - a bad text is refused and writes nothing
 */
static void names_serial_refuse_test(void)
{
	uint8_t back[PON_SERIAL_LEN];
	unsigned int i;

	memset(back, 0xee, sizeof(back));
	CHECK_EQ(pon_serial_parse("a b\x01" "00000001", back), -1);
	CHECK_EQ(pon_serial_parse("adtn00000001", back), -1);
	CHECK_EQ(pon_serial_parse("ABCDzz000000", back), -1);
	CHECK_EQ(pon_serial_parse("41424344010203zz", back), -1);
	for (i = 0; i < PON_SERIAL_LEN; i++)
		CHECK_EQ(back[i], 0xee);
}

/**
 * main - run the cases of the names and the text forms
 *
 * Return: 0 when every check passed, else 1.
 */
int main(void)
{
	names_enum_test();
	names_ploam_mode_test();
	names_value_test();
	names_value_refuse_test();
	names_hex_test();
	names_hex_fail_test();
	names_serial_test();
	names_serial_hex_test();
	names_serial_refuse_test();
	return t_report("test-names");
}
