// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (C) 2026 John Crispin <john@phrozen.org> */
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <net/if.h>

#include <linux/netlink.h>

#include <libomci-apply/apply.h>
#include <libpon/pon.h>

#define ERR_REPORTED	(-EIO)
#define ARG_INT_MAX	0x7fffffffu
#define MONITOR_DEVS	16
#define JSON_DEPTH_MAX	4

/**
 * struct monitor_dev - what pon monitor keeps of a device
 * @used: the entry holds a device
 * @mode_set: @mode holds the active mode of the device
 * @dev_id: the device
 * @mode: the active enum pon_mode value
 * @alarms: the alarms raised, one bit per enum pon_alarm value
 *
 * The mode comes from the dump at the start of the monitor and from the
 * device notifications; the alarms from the alarm dump at the start and
 * from the alarm notifications.
 */
struct monitor_dev {
	bool used;
	bool mode_set;
	uint32_t dev_id;
	uint32_t mode;
	uint32_t alarms;
};

static struct pon pon;
static unsigned int data_ifindex;
static struct monitor_dev monitor_devs[MONITOR_DEVS];
static bool json;
static FILE *out;
static bool text_first;
static unsigned int json_depth;
static bool json_items[JSON_DEPTH_MAX];

/**
 * number_get - a decimal argument within a range
 * @text: the argument
 * @max: the largest value taken
 * @out: the value
 *
 * Return: 0, or -EINVAL after a message when @text is not a decimal
 * number from 0 to @max.
 */
static int number_get(const char *text, uint32_t max, uint32_t *out)
{
	if (!pon_number_parse(text, 0, max, out))
		return 0;
	fprintf(stderr, "bad number: %s\n", text);
	return -EINVAL;
}

/**
 * int_get - a decimal argument for a field of type int
 * @text: the argument
 * @out: the value
 *
 * Return: 0, or -EINVAL after a message when @text is not a decimal
 * number that an int holds.
 */
static int int_get(const char *text, int *out)
{
	uint32_t value;

	if (number_get(text, ARG_INT_MAX, &value))
		return -EINVAL;
	*out = value;
	return 0;
}

/**
 * ids_get - the device and the object of a command
 * @dev: the argument of the device
 * @id: the argument of the object
 * @dev_id: the device
 * @out: the object
 *
 * Return: 0, or -EINVAL after a message.
 */
static int ids_get(const char *dev, const char *id, uint32_t *dev_id,
		   uint32_t *out)
{
	if (number_get(dev, UINT32_MAX, dev_id) ||
	    number_get(id, UINT32_MAX, out))
		return -EINVAL;
	return 0;
}

/**
 * report - print the error of a request to the pon family
 * @err: the result of the request
 *
 * Return: 0 for success, else ERR_REPORTED after the message.
 */
static int report(int err)
{
	if (!err)
		return 0;
	fprintf(stderr, "error: %s\n", pon_error(&pon));
	return ERR_REPORTED;
}

/**
 * args_refuse - refuse a command line with the wrong number of arguments
 * @verb: the command
 *
 * Return: -EINVAL after a message.
 */
static int args_refuse(const char *verb)
{
	fprintf(stderr, "wrong number of arguments for %s\n", verb);
	return -EINVAL;
}

/**
 * verb_refuse - refuse a command the tool does not know
 * @object: the command before the verb, or NULL
 * @verb: the word that is not a command
 *
 * Return: -EINVAL after a message.
 */
static int verb_refuse(const char *object, const char *verb)
{
	fprintf(stderr, "unknown command: %s%s%s\n", object ? object : "",
		object ? " " : "", verb);
	return -EINVAL;
}

/**
 * out_stream - the stream the printers write to
 *
 * Return: the buffer of a JSON reply, else stdout.
 */
static FILE *out_stream(void)
{
	return out ? out : stdout;
}

/**
 * json_member - start a value in the JSON object or array that is open
 * @key: the name of the member, or NULL inside an array
 *
 * A dash of a name of the family becomes an underscore.
 */
static void json_member(const char *key)
{
	FILE *to = out_stream();

	if (json_depth && json_items[json_depth - 1])
		fputc(',', to);
	if (json_depth)
		json_items[json_depth - 1] = true;
	if (!key)
		return;
	fputc('"', to);
	for (; *key; key++)
		fputc(*key == '-' ? '_' : *key, to);
	fputs("\":", to);
}

/**
 * json_open - open a JSON object or array
 * @key: the name of the member, or NULL
 * @bracket: '{' or '['
 */
static void json_open(const char *key, char bracket)
{
	json_member(key);
	fputc(bracket, out_stream());
	if (json_depth < JSON_DEPTH_MAX)
		json_items[json_depth] = false;
	json_depth++;
}

/**
 * json_close - close the JSON object or array json_open() opened last
 * @bracket: '}' or ']'
 */
static void json_close(char bracket)
{
	json_depth--;
	fputc(bracket, out_stream());
}

/**
 * text_sep - the space before a word of a text line
 */
static void text_sep(void)
{
	if (!text_first)
		fputc(' ', out_stream());
	text_first = false;
}

/**
 * obj_begin - start the output of one object
 * @key: the name of the object in a JSON notification, or NULL
 */
static void obj_begin(const char *key)
{
	if (json)
		json_open(key, '{');
	else
		text_first = true;
}

/**
 * obj_end - end the output of one object: its line, or its JSON object
 */
static void obj_end(void)
{
	if (json)
		json_close('}');
	else
		fputc('\n', out_stream());
}

/**
 * text_word - a word of the text line that JSON leaves out
 * @word: the word
 */
static void text_word(const char *word)
{
	if (json)
		return;
	text_sep();
	fputs(word, out_stream());
}

/**
 * field_u - a number of an object
 * @word: its word on the text line, or NULL for the number alone
 * @key: its attribute name in the family, the JSON name
 * @value: the number
 */
static void field_u(const char *word, const char *key, uint64_t value)
{
	if (json) {
		json_member(key);
	} else {
		text_sep();
		if (word)
			fprintf(out_stream(), "%s ", word);
	}
	fprintf(out_stream(), "%llu", (unsigned long long)value);
}

/**
 * field_s - a name of an object
 * @word: its word on the text line, or NULL for the name alone
 * @key: its attribute name in the family, the JSON name
 * @value: the name
 */
static void field_s(const char *word, const char *key, const char *value)
{
	if (json) {
		json_member(key);
		fprintf(out_stream(), "\"%s\"", value);
		return;
	}
	text_sep();
	if (word)
		fprintf(out_stream(), "%s ", word);
	fputs(value, out_stream());
}

/**
 * field_flag - a flag of an object
 * @word: the word the text line shows while the flag is set
 * @key: its attribute name in the family, the JSON name
 * @value: the value, 0 or 1
 */
static void field_flag(const char *word, const char *key, uint32_t value)
{
	if (json)
		field_u(NULL, key, value);
	else if (value)
		text_word(word);
}

/**
 * field_names - the names of the bits of a bitmap of an object
 * @word: its word on the text line
 * @key: its attribute name in the family, the JSON name of the array
 * @bits: the bitmap
 * @name: the name of a bit
 * @empty: the word the text line shows for no bit, or NULL for none
 */
static void field_names(const char *word, const char *key, uint32_t bits,
			const char *(*name)(uint32_t), const char *empty)
{
	unsigned int i;

	if (json)
		json_open(key, '[');
	else
		text_word(word);
	for (i = 0; i < 32; i++) {
		if (!(bits & (1u << i)))
			continue;
		if (json) {
			json_member(NULL);
			fprintf(out_stream(), "\"%s\"", name(i));
		} else {
			fprintf(out_stream(), " %s", name(i));
		}
	}
	if (json)
		json_close(']');
	else if (!bits && empty)
		fprintf(out_stream(), " %s", empty);
}

/**
 * list_begin - open the JSON array of the objects a command prints
 * @name: the name of the attribute set of the objects in the family
 */
static void list_begin(const char *name)
{
	if (!json)
		return;
	json_open(NULL, '{');
	json_open(name, '[');
}

/**
 * list_end - close the JSON array list_begin() opened
 */
static void list_end(void)
{
	if (!json)
		return;
	json_close(']');
	json_close('}');
	fputc('\n', out_stream());
}

/**
 * ploam_name - the name of the activation state of a device
 * @dev: the device
 *
 * Return: the name in the mode of the device when the reply carried it.
 */
static const char *ploam_name(const struct pon_dev_info *dev)
{
	if (dev->has & PON_DEV_F_MODE)
		return pon_ploam_state_name_mode(dev->ploam, dev->mode);
	return pon_ploam_state_name(dev->ploam);
}

/**
 * dev_print - print one PON device
 * @dev: the device
 * @key: its name in a JSON notification, or NULL
 *
 * The text form is one line; JSON leaves out what the kernel did not send.
 */
static void dev_print(const struct pon_dev_info *dev, const char *key)
{
	char serial[PON_SERIAL_TEXT_MAX + 1];

	obj_begin(key);
	field_u("dev", "id", dev->dev_id);
	if (!json || (dev->has & PON_DEV_F_IFINDEX))
		field_u("ifindex", "ifindex", dev->ifindex);
	if (dev->has & PON_DEV_F_MODE)
		field_s("mode", "mode", pon_mode_name(dev->mode));
	else
		text_word("mode ?");
	if (dev->has & PON_DEV_F_PLOAM)
		field_s("ploam", "ploam-state", ploam_name(dev));
	else
		text_word("ploam ?");
	if (dev->has & PON_DEV_F_MAX_TCONTS)
		field_u("max-tconts", "max-tconts", dev->max_tconts);
	if (dev->has & PON_DEV_F_MAX_GEMS)
		field_u("max-gems", "max-gems", dev->max_gems);
	if (dev->has & PON_DEV_F_MODES_CAP)
		field_names("modes", "modes-cap", dev->modes_cap, pon_mode_name,
			    NULL);
	if (dev->has & PON_DEV_F_SERIAL) {
		pon_serial_format(dev->serial, serial, sizeof(serial));
		field_s("serial", "serial", serial);
	}
	if (dev->has & PON_DEV_F_RESPONSE_TIME)
		field_u("response-time", "response-time", dev->response_time);
	if (dev->has & PON_DEV_F_ENABLE)
		field_u("enable", "enable", dev->enable);
	if (dev->has & PON_DEV_F_DISABLED)
		field_u("disabled", "disabled", dev->disabled);
	obj_end();
}

/**
 * dev_cb - print a device of a dump
 * @arg: unused
 * @dev: the device
 */
static void dev_cb(void *arg, const struct pon_dev_info *dev)
{
	dev_print(dev, NULL);
}

/**
 * tcont_print - print one T-CONT
 * @tcont: the T-CONT
 * @key: its name in a JSON notification, or NULL
 */
static void tcont_print(const struct pon_tcont *tcont, const char *key)
{
	obj_begin(key);
	field_u("dev", "dev-id", tcont->dev_id);
	field_u("tcont", "index", tcont->index);
	if (!json || tcont->alloc_id_set)
		field_u("alloc-id", "alloc-id", tcont->alloc_id);
	obj_end();
}

/**
 * tcont_cb - print a T-CONT of a dump
 * @arg: unused
 * @tcont: the T-CONT
 */
static void tcont_cb(void *arg, const struct pon_tcont *tcont)
{
	tcont_print(tcont, NULL);
}

/**
 * alarm_print - print the raised alarms of a device
 * @alarms: the alarms of the device
 * @key: its name in a JSON notification, or NULL
 */
static void alarm_print(const struct pon_alarms *alarms, const char *key)
{
	obj_begin(key);
	field_u("dev", "dev-id", alarms->dev_id);
	field_names("alarms", "alarms", alarms->alarms, pon_alarm_name, "none");
	obj_end();
}

/**
 * alarm_cb - print the raised alarms of a device of a dump
 * @arg: unused
 * @alarms: the alarms of the device
 */
static void alarm_cb(void *arg, const struct pon_alarms *alarms)
{
	alarm_print(alarms, NULL);
}

/**
 * gem_map_print - print one mapper rule
 * @map: the rule
 * @key: its name in a JSON notification, or NULL
 */
static void gem_map_print(const struct pon_gem_map *map, const char *key)
{
	obj_begin(key);
	field_u("dev", "dev-id", map->dev_id);
	field_u("gem", "gem-id", map->gem_id);
	field_u("pbit", "pbit", map->pbit);
	if (map->ifindex)
		field_u("mapper", "ifindex", map->ifindex);
	obj_end();
}

/**
 * gem_map_cb - print a mapper rule of a dump
 * @arg: unused
 * @map: the rule
 */
static void gem_map_cb(void *arg, const struct pon_gem_map *map)
{
	gem_map_print(map, NULL);
}

/**
 * gem_print - print one GEM port
 * @gem: the port
 * @key: its name in a JSON notification, or NULL
 */
static void gem_print(const struct pon_gem *gem, const char *key)
{
	obj_begin(key);
	field_u("dev", "dev-id", gem->dev_id);
	field_u("gem", "id", gem->id);
	field_s("dir", "dir", pon_gem_dir_name(gem->dir));
	if (gem->tcont_index >= 0)
		field_u("tcont", "tcont-index", gem->tcont_index);
	if (gem->key_ring >= 0)
		field_s("key-ring", "key-ring",
			pon_gem_key_ring_name(gem->key_ring));
	if (gem->queue >= 0)
		field_u("queue", "queue", gem->queue);
	if (gem->no_offload >= 0)
		field_flag("no-offload", "no-offload", gem->no_offload);
	obj_end();
}

/**
 * gem_cb - print a GEM port of a dump
 * @arg: unused
 * @gem: the port
 */
static void gem_cb(void *arg, const struct pon_gem *gem)
{
	gem_print(gem, NULL);
}

/**
 * stats_print - print the counters of one GEM port
 * @stats: the counters
 */
static void stats_print(const struct pon_gem_stats *stats)
{
	obj_begin(NULL);
	field_u("dev", "dev-id", stats->dev_id);
	field_u("gem", "gem-id", stats->gem_id);
	field_u("rx-frames", "rx-frames", stats->rx_frames);
	field_u("rx-bytes", "rx-bytes", stats->rx_bytes);
	field_u("tx-frames", "tx-frames", stats->tx_frames);
	field_u("tx-bytes", "tx-bytes", stats->tx_bytes);
	obj_end();
}

/**
 * stats_cb - print the counters of a GEM port of a dump
 * @arg: unused
 * @stats: the counters
 */
static void stats_cb(void *arg, const struct pon_gem_stats *stats)
{
	stats_print(stats);
}

/**
 * ntf_begin - start the output of one notification
 * @name: the name of the notification in the family
 * @text: the start of the text line, or NULL for none
 *
 * JSON gives one object per line: the name, then the object of the
 * notification under the name of its attribute set.
 */
static void ntf_begin(const char *name, const char *text)
{
	if (json) {
		json_open(NULL, '{');
		field_s(NULL, "notification", name);
	} else if (text) {
		fputs(text, out_stream());
	}
}

/**
 * ntf_end - end the output of one notification and flush it
 */
static void ntf_end(void)
{
	if (json) {
		json_close('}');
		fputc('\n', out_stream());
	}
	fflush(out_stream());
}

/**
 * monitor_dev_find - the entry of a device in the monitor
 * @dev_id: the device
 * @add: take a free entry when the device has none
 *
 * Return: the entry, or NULL when the device has none and @add is false
 * or every entry is taken.
 */
static struct monitor_dev *monitor_dev_find(uint32_t dev_id, bool add)
{
	struct monitor_dev *free_entry = NULL;
	unsigned int i;

	for (i = 0; i < MONITOR_DEVS; i++) {
		struct monitor_dev *entry = &monitor_devs[i];

		if (entry->used && entry->dev_id == dev_id)
			return entry;
		if (!entry->used && !free_entry)
			free_entry = entry;
	}
	if (!add || !free_entry)
		return NULL;
	memset(free_entry, 0, sizeof(*free_entry));
	free_entry->used = true;
	free_entry->dev_id = dev_id;
	return free_entry;
}

/**
 * monitor_dev_keep - keep the mode of a device for the monitor
 * @arg: unused
 * @dev: the device of a dump or of a device notification
 */
static void monitor_dev_keep(void *arg, const struct pon_dev_info *dev)
{
	struct monitor_dev *entry = monitor_dev_find(dev->dev_id, true);

	if (!entry || !(dev->has & PON_DEV_F_MODE))
		return;
	entry->mode = dev->mode;
	entry->mode_set = true;
}

/**
 * ntf_ploam - print a change of the activation state
 * @client: the client
 * @dev: the device with its new state
 *
 * The state takes its name in the mode the monitor keeps for the device.
 */
static void ntf_ploam(struct pon *client, const struct pon_dev_info *dev)
{
	struct monitor_dev *entry = monitor_dev_find(dev->dev_id, false);
	struct pon_dev_info info = *dev;

	if (!(info.has & PON_DEV_F_MODE) && entry && entry->mode_set) {
		info.mode = entry->mode;
		info.has |= PON_DEV_F_MODE;
	}
	ntf_begin("ploam-ntf", NULL);
	obj_begin("dev");
	text_word("ploam");
	field_u("dev", "id", dev->dev_id);
	field_s("state", "ploam-state", ploam_name(&info));
	obj_end();
	ntf_end();
}

/**
 * ntf_event - print an event with the attributes it carries
 * @client: the client
 * @ev: the event
 */
static void ntf_event(struct pon *client, const struct pon_event *ev)
{
	ntf_begin("event-ntf", NULL);
	obj_begin("event");
	text_word("event");
	field_u("dev", "dev-id", ev->dev_id);
	field_s(NULL, "type", pon_event_type_name(ev->type));
	if (ev->has & PON_EVENT_F_ALLOC_ID)
		field_u("alloc-id", "alloc-id", ev->alloc_id);
	if (ev->has & PON_EVENT_F_DEPTH)
		field_u("depth", "depth", ev->depth);
	if (ev->has & PON_EVENT_F_IMAGE)
		field_u("image", "image", ev->image);
	if (ev->has & PON_EVENT_F_CALLS)
		field_u("calls", "calls", ev->calls);
	obj_end();
	ntf_end();
}

/**
 * ntf_alarm - print each alarm a device raised or cleared
 * @client: the client
 * @alarms: the alarms of the device
 *
 * The notification carries the whole bitmap. One text line goes out for
 * each bit that differs from the bitmap the monitor keeps for the device;
 * JSON gives the bitmap of the notification.
 */
static void ntf_alarm(struct pon *client, const struct pon_alarms *alarms)
{
	struct monitor_dev *entry = monitor_dev_find(alarms->dev_id, true);
	uint32_t before = entry ? entry->alarms : 0;
	uint32_t changed = before ^ alarms->alarms;
	unsigned int i;

	if (entry)
		entry->alarms = alarms->alarms;
	if (json) {
		ntf_begin("alarm-ntf", NULL);
		alarm_print(alarms, "alarm");
		ntf_end();
		return;
	}
	for (i = 0; i < 32; i++) {
		if (!(changed & (1u << i)))
			continue;
		printf("alarm dev %u %s %s\n", alarms->dev_id,
		       (alarms->alarms & (1u << i)) ? "raised" : "cleared",
		       pon_alarm_name(i));
	}
	fflush(stdout);
}

/**
 * ntf_tcont - print a T-CONT that was added, changed or removed
 * @client: the client
 * @cmd: the notification, a PON_CMD_TCONT_*_NTF command
 * @tcont: the T-CONT
 */
static void ntf_tcont(struct pon *client, uint8_t cmd,
		      const struct pon_tcont *tcont)
{
	if (cmd == PON_CMD_TCONT_ADD_NTF)
		ntf_begin("tcont-add-ntf", "tcont added: ");
	else if (cmd == PON_CMD_TCONT_CHANGE_NTF)
		ntf_begin("tcont-change-ntf", "tcont changed: ");
	else
		ntf_begin("tcont-del-ntf", "tcont removed: ");
	tcont_print(tcont, "tcont");
	ntf_end();
}

/**
 * ntf_gem - print a GEM port that was added or removed
 * @client: the client
 * @cmd: the notification, a PON_CMD_GEM_*_NTF command
 * @gem: the port
 */
static void ntf_gem(struct pon *client, uint8_t cmd, const struct pon_gem *gem)
{
	if (cmd == PON_CMD_GEM_ADD_NTF)
		ntf_begin("gem-add-ntf", "gem added: ");
	else
		ntf_begin("gem-del-ntf", "gem removed: ");
	gem_print(gem, "gem");
	ntf_end();
}

/**
 * ntf_gem_map - print a mapper rule that was added or removed
 * @client: the client
 * @cmd: the notification, a PON_CMD_GEM_MAP_*_NTF command
 * @map: the rule
 */
static void ntf_gem_map(struct pon *client, uint8_t cmd,
			const struct pon_gem_map *map)
{
	if (cmd == PON_CMD_GEM_MAP_ADD_NTF)
		ntf_begin("gem-map-add-ntf", "rule added: ");
	else
		ntf_begin("gem-map-del-ntf", "rule removed: ");
	gem_map_print(map, "gem-map");
	ntf_end();
}

/**
 * ntf_dev - print a device that was added, changed or removed
 * @client: the client
 * @cmd: the notification, a PON_CMD_DEV_*_NTF command
 * @dev: the device
 *
 * The monitor keeps the mode of an added or changed device and forgets a
 * deleted one.
 */
static void ntf_dev(struct pon *client, uint8_t cmd,
		    const struct pon_dev_info *dev)
{
	struct monitor_dev *entry;

	if (cmd != PON_CMD_DEV_DEL_NTF) {
		monitor_dev_keep(NULL, dev);
	} else {
		entry = monitor_dev_find(dev->dev_id, false);
		if (entry)
			entry->used = false;
	}
	if (cmd == PON_CMD_DEV_ADD_NTF)
		ntf_begin("dev-add-ntf", "dev added: ");
	else if (cmd == PON_CMD_DEV_DEL_NTF)
		ntf_begin("dev-del-ntf", "dev removed: ");
	else
		ntf_begin("dev-change-ntf", "dev changed: ");
	dev_print(dev, "dev");
	ntf_end();
}

static const struct pon_notify_ops monitor_ops = {
	.dev = ntf_dev,
	.ploam = ntf_ploam,
	.event = ntf_event,
	.alarm = ntf_alarm,
	.tcont = ntf_tcont,
	.gem = ntf_gem,
	.gem_map = ntf_gem_map,
};

/**
 * monitor_alarms_keep - keep the alarms of a device for the monitor
 * @arg: unused
 * @alarms: the alarms of a device of the dump
 */
static void monitor_alarms_keep(void *arg, const struct pon_alarms *alarms)
{
	struct monitor_dev *entry = monitor_dev_find(alarms->dev_id, true);

	if (entry)
		entry->alarms = alarms->alarms;
}

/**
 * monitor_seed - read what the monitor keeps of every device
 *
 * Called after the client joined the notification groups, so that no
 * change after the dump is lost.
 *
 * Return: 0, or ERR_REPORTED after a message.
 */
static int monitor_seed(void)
{
	int err = report(pon_dev_dump(&pon, monitor_dev_keep, NULL));

	if (err)
		return err;
	return report(pon_alarm_dump(&pon, PON_DEV_ANY, monitor_alarms_keep,
				     NULL));
}

/**
 * monitor_events - print the notifications that wait on the socket
 *
 * After lost notifications the monitor reads the mode and the alarms of
 * every device again, so that the next change is printed against the
 * state of the kernel.
 */
static void monitor_events(void)
{
	if (!pon_event_process(&pon))
		return;
	fprintf(stderr, "notifications lost\n");
	monitor_seed();
}

/**
 * monitor_run - pon monitor
 * @argc: the number of arguments from "monitor"
 * @argv: the arguments from "monitor"
 *
 * Joins the notification groups of the pon family, reads the mode of
 * every device and prints every notification as it arrives, until a
 * failure.
 *
 * Return: -EINVAL for an argument, else ERR_REPORTED after a failure.
 */
static int monitor_run(int argc, char **argv)
{
	struct pollfd pfd;
	int err;

	if (argc != 1)
		return args_refuse(argv[0]);
	pon_close(&pon);
	if (pon_open(&pon, &monitor_ops, NULL)) {
		fprintf(stderr, "cannot join the notification groups\n");
		return ERR_REPORTED;
	}
	err = monitor_seed();
	if (err)
		return err;

	pfd.fd = pon_event_fd(&pon);
	pfd.events = POLLIN;
	for (;;) {
		if (poll(&pfd, 1, -1) < 0 && errno != EINTR)
			return ERR_REPORTED;
		monitor_events();
	}
	return 0;
}

/**
 * omci_print - print a received OMCI PDU in hex on one line
 * @dev_id: the device
 * @pdu: the message
 * @len: its length
 */
static void omci_print(uint32_t dev_id, const uint8_t *pdu, int len)
{
	int i;

	printf("omci dev %u rx %d ", dev_id, len);
	for (i = 0; i < len; i++)
		printf("%02x", pdu[i]);
	printf("\n");
	fflush(stdout);
}

/**
 * omci_open - own the OMCI channel of a device
 * @omci: the channel
 * @dev: the argument of the device
 *
 * Return: 0, -EINVAL for a bad device, or ERR_REPORTED.
 */
static int omci_open(struct pon_omci *omci, const char *dev)
{
	uint32_t dev_id;
	int err;

	if (number_get(dev, UINT32_MAX, &dev_id))
		return -EINVAL;
	err = pon_omci_open(omci, dev_id);
	if (!err)
		return 0;
	fprintf(stderr, "error: %s\n",
		omci->nl.err_msg[0] ? omci->nl.err_msg : strerror(-err));
	return ERR_REPORTED;
}

/**
 * omci_drain - read every PDU that waits on the OMCI channel
 * @omci: the channel
 * @show: whether to print the PDUs
 *
 * Reports lost PDUs and a PDU that the channel refused to send.
 *
 * Return: 0, or the error of a read.
 */
static int omci_drain(struct pon_omci *omci, bool show)
{
	uint8_t pdu[PON_OMCI_PDU_MAX];
	uint64_t refused = omci->tx_refused;
	int len;

	while ((len = pon_omci_recv(omci, pdu, sizeof(pdu))) != -EAGAIN) {
		if (len == -ENOBUFS) {
			fprintf(stderr, "OMCI PDUs lost\n");
			continue;
		}
		if (len < 0)
			return len;
		if (omci->tx_refused != refused) {
			refused = omci->tx_refused;
			fprintf(stderr, "omci dev %u tx refused: %s\n",
				omci->dev_id, strerror(-omci->tx_err));
		}
		if (len && show)
			omci_print(omci->dev_id, pdu, len);
	}
	return 0;
}

/**
 * omci_listen - pon omci listen <dev-id>
 * @dev: the argument of the device
 *
 * Owns the OMCI channel of the device and prints every PDU, until a
 * failure.
 *
 * Return: -EINVAL for a bad device, or ERR_REPORTED.
 */
static int omci_listen(const char *dev)
{
	struct pon_omci omci;
	struct pollfd pfd;
	int err;

	err = omci_open(&omci, dev);
	if (err)
		return err;

	pfd.fd = pon_omci_fd(&omci);
	pfd.events = POLLIN;
	for (;;) {
		if (poll(&pfd, 1, -1) < 0 && errno != EINTR) {
			err = -errno;
			break;
		}
		err = omci_drain(&omci, true);
		if (err)
			break;
	}
	fprintf(stderr, "error: %s\n", strerror(-err));
	pon_omci_close(&omci);
	return ERR_REPORTED;
}

/**
 * omci_send - pon omci send <dev-id> <hex>
 * @dev: the argument of the device
 * @hex: the PDU in hex
 *
 * Owns the OMCI channel of the device and sends one PDU.
 *
 * Return: 0, -EINVAL for bad arguments, or ERR_REPORTED.
 */
static int omci_send(const char *dev, const char *hex)
{
	uint8_t pdu[PON_OMCI_PDU_MAX];
	struct pon_omci omci;
	size_t len;
	int err;

	if (pon_hex_parse(hex, pdu, sizeof(pdu), &len) ||
	    len < PON_OMCI_PDU_MIN) {
		fprintf(stderr, "bad PDU: %s\n", hex);
		return -EINVAL;
	}

	err = omci_open(&omci, dev);
	if (err)
		return err;

	err = pon_omci_send(&omci, pdu, len);
	if (!err)
		err = omci_drain(&omci, false);
	if (!err && omci.tx_refused)
		err = ERR_REPORTED;
	else if (err)
		fprintf(stderr, "error: %s\n", strerror(-err));
	pon_omci_close(&omci);
	return err ? ERR_REPORTED : 0;
}

/**
 * omci_cmd - pon omci listen|send ...
 * @argc: the number of arguments from "omci"
 * @argv: the arguments from "omci"
 *
 * Return: 0, -EINVAL for bad arguments, or ERR_REPORTED.
 */
static int omci_cmd(int argc, char **argv)
{
	if (argc < 2)
		return args_refuse("omci");
	if (!strcmp(argv[1], "listen"))
		return argc == 3 ? omci_listen(argv[2]) :
				   args_refuse("omci listen");
	if (!strcmp(argv[1], "send"))
		return argc == 4 ? omci_send(argv[2], argv[3]) :
				   args_refuse("omci send");
	return verb_refuse("omci", argv[1]);
}

/**
 * flag_value - the value of a flag argument
 * @text: the argument
 *
 * Return: 0 or 1, or -1 when @text is neither "0" nor "1".
 */
static int flag_value(const char *text)
{
	if (!strcmp(text, "0"))
		return 0;
	if (!strcmp(text, "1"))
		return 1;
	return -1;
}

/**
 * dev_set_cmd - pon dev set <dev-id> <key> <value> ...
 * @argc: the number of arguments from "dev"
 * @argv: the arguments from "dev"
 *
 * The disabled state is a flag of the kernel, so only 1 can be set; 0 is
 * refused here.
 *
 * Return: 0, -EINVAL for bad arguments, or ERR_REPORTED.
 */
static int dev_set_cmd(int argc, char **argv)
{
	struct pon_dev_config cfg = { 0 };
	int i, value;

	if (argc < 4 || (argc - 3) % 2)
		return args_refuse("dev set");

	if (number_get(argv[2], UINT32_MAX, &cfg.dev_id))
		return -EINVAL;

	for (i = 3; i < argc; i += 2) {
		const char *key = argv[i], *val = argv[i + 1];

		if (!strcmp(key, "mode")) {
			value = pon_mode_value(val);
			if (value < 0)
				goto err_bad_value;
			cfg.mode = value;
			cfg.has |= PON_DEV_F_MODE;
		} else if (!strcmp(key, "enable")) {
			value = flag_value(val);
			if (value < 0)
				goto err_bad_value;
			cfg.enable = value;
			cfg.has |= PON_DEV_F_ENABLE;
		} else if (!strcmp(key, "disabled")) {
			value = flag_value(val);
			if (value < 1)
				goto err_bad_value;
			cfg.disabled = value;
			cfg.has |= PON_DEV_F_DISABLED;
		} else if (!strcmp(key, "serial")) {
			if (pon_serial_parse(val, cfg.serial))
				goto err_bad_value;
			cfg.has |= PON_DEV_F_SERIAL;
		} else if (!strcmp(key, "regid")) {
			if (pon_hex_parse(val, cfg.registration_id,
					  PON_REGID_MAX,
					  &cfg.registration_id_len))
				goto err_bad_value;
			cfg.has |= PON_DEV_F_REGISTRATION_ID;
		} else {
			fprintf(stderr, "unknown setting: %s\n", key);
			return -EINVAL;
		}
	}

	return report(pon_dev_set(&pon, &cfg));

err_bad_value:
	fprintf(stderr, "bad value for %s\n", argv[i]);
	return -EINVAL;
}

/**
 * dev_cmd - pon dev [dev-id], pon dev set ...
 * @argc: the number of arguments from "dev"
 * @argv: the arguments from "dev"
 *
 * Lists the PON devices, or shows one.
 *
 * Return: 0, -EINVAL for bad arguments, or ERR_REPORTED.
 */
static int dev_cmd(int argc, char **argv)
{
	struct pon_dev_info dev;
	uint32_t dev_id;
	int err;

	if (argc > 1 && !strcmp(argv[1], "set"))
		return dev_set_cmd(argc, argv);
	if (argc > 2)
		return args_refuse(argv[0]);
	if (argc == 2 && number_get(argv[1], UINT32_MAX, &dev_id))
		return -EINVAL;
	list_begin("dev");
	if (argc == 1) {
		err = report(pon_dev_dump(&pon, dev_cb, NULL));
	} else {
		err = report(pon_dev_get(&pon, dev_id, &dev));
		if (!err)
			dev_print(&dev, NULL);
	}
	list_end();
	return err;
}

/**
 * tconts_cmd - pon tconts [dev-id index]
 * @argc: the number of arguments from "tconts"
 * @argv: the arguments from "tconts"
 *
 * Lists the T-CONTs, or shows one.
 *
 * Return: 0, -EINVAL for bad arguments, or ERR_REPORTED.
 */
static int tconts_cmd(int argc, char **argv)
{
	uint32_t dev_id, index;
	struct pon_tcont tcont;
	int err;

	if (argc != 1 && argc != 3)
		return args_refuse(argv[0]);
	if (argc == 3 && ids_get(argv[1], argv[2], &dev_id, &index))
		return -EINVAL;
	list_begin("tcont");
	if (argc == 1) {
		err = report(pon_tcont_dump(&pon, PON_DEV_ANY, tcont_cb, NULL));
	} else {
		err = report(pon_tcont_get(&pon, dev_id, index, &tcont));
		if (!err)
			tcont_print(&tcont, NULL);
	}
	list_end();
	return err;
}

/**
 * gems_cmd - pon gems [dev-id gem-id]
 * @argc: the number of arguments from "gems"
 * @argv: the arguments from "gems"
 *
 * Lists the GEM ports, or shows one.
 *
 * Return: 0, -EINVAL for bad arguments, or ERR_REPORTED.
 */
static int gems_cmd(int argc, char **argv)
{
	uint32_t dev_id, gem_id;
	struct pon_gem gem;
	int err;

	if (argc != 1 && argc != 3)
		return args_refuse(argv[0]);
	if (argc == 3 && ids_get(argv[1], argv[2], &dev_id, &gem_id))
		return -EINVAL;
	list_begin("gem");
	if (argc == 1) {
		err = report(pon_gem_dump(&pon, PON_DEV_ANY, gem_cb, NULL));
	} else {
		err = report(pon_gem_get(&pon, dev_id, gem_id, &gem));
		if (!err)
			gem_print(&gem, NULL);
	}
	list_end();
	return err;
}

/**
 * alarms_cmd - pon alarms [dev-id]
 * @argc: the number of arguments from "alarms"
 * @argv: the arguments from "alarms"
 *
 * Lists the raised alarms of every device, or of one.
 *
 * Return: 0, -EINVAL for bad arguments, or ERR_REPORTED.
 */
static int alarms_cmd(int argc, char **argv)
{
	struct pon_alarms alarms;
	uint32_t dev_id;
	int err;

	if (argc > 2)
		return args_refuse(argv[0]);
	if (argc == 2 && number_get(argv[1], UINT32_MAX, &dev_id))
		return -EINVAL;
	list_begin("alarm");
	if (argc == 1) {
		err = report(pon_alarm_dump(&pon, PON_DEV_ANY, alarm_cb, NULL));
	} else {
		err = report(pon_alarm_get(&pon, dev_id, &alarms));
		if (!err)
			alarm_print(&alarms, NULL);
	}
	list_end();
	return err;
}

/**
 * data_ifindex_cb - keep the data interface of the first device that has one
 * @arg: unused
 * @dev: a device of the dump
 */
static void data_ifindex_cb(void *arg, const struct pon_dev_info *dev)
{
	if (!data_ifindex && (dev->has & PON_DEV_F_IFINDEX))
		data_ifindex = dev->ifindex;
}

/**
 * lower_resolve - the lower device of a GEM network device
 * @lower: the name of the lower device, or NULL for the data interface of
 *	   the first PON device that has one
 * @ifindex: the ifindex of the lower device
 *
 * Return: 0, or ERR_REPORTED after a message.
 */
static int lower_resolve(const char *lower, unsigned int *ifindex)
{
	int err;

	if (lower) {
		*ifindex = if_nametoindex(lower);
		if (*ifindex)
			return 0;
		fprintf(stderr, "no such interface: %s\n", lower);
		return ERR_REPORTED;
	}

	err = report(pon_dev_dump(&pon, data_ifindex_cb, NULL));
	if (err)
		return err;
	if (!data_ifindex) {
		fprintf(stderr, "no PON device with a data interface\n");
		return ERR_REPORTED;
	}
	*ifindex = data_ifindex;
	return 0;
}

/**
 * link_report - print the error of a link request and close the client
 * @rtnl: the route netlink client
 * @err: the result of the request
 *
 * Return: 0 for success, else ERR_REPORTED after the message.
 */
static int link_report(struct pon_nl *rtnl, int err)
{
	if (err)
		fprintf(stderr, "error: %s\n", pon_nl_error(rtnl));
	pon_nl_close(rtnl);
	return err ? ERR_REPORTED : 0;
}

/**
 * gem_netdev_add - pon gem netdev add <gem-id> <name> [lower]
 * @name: the name of the new device
 * @gem_id: the GEM port
 * @lower: the name of the lower device, or NULL
 *
 * The "gem" link kind is created over rtnetlink like any other. No released
 * iproute2 knows how to encode its one attribute, so the tool that owns the
 * family does it.
 *
 * Return: 0, or ERR_REPORTED after a message.
 */
static int gem_netdev_add(const char *name, uint32_t gem_id, const char *lower)
{
	struct pon_nl rtnl;
	unsigned int lower_idx;
	int err;

	err = lower_resolve(lower, &lower_idx);
	if (err)
		return err;

	if (pon_nl_open(&rtnl, NETLINK_ROUTE)) {
		fprintf(stderr, "cannot open a route netlink socket\n");
		return ERR_REPORTED;
	}
	return link_report(&rtnl, pon_gem_link_add(&rtnl, name, lower_idx,
						   gem_id, NULL));
}

/**
 * link_ifindex_keep - keep the device of one ifindex from a link dump
 * @link: a device of the dump
 * @priv: the ifindex, an unsigned int
 *
 * Return: true for the device of that ifindex.
 */
static bool link_ifindex_keep(const struct oa_link *link, void *priv)
{
	return link->ifindex == *(unsigned int *)priv;
}

/**
 * mapper_netdev_add - pon gem mapper add <name> [lower]
 * @name: the name of the new device
 * @lower: the name of the lower device, or NULL
 *
 * Return: 0, or ERR_REPORTED after a message.
 */
static int mapper_netdev_add(const char *name, const char *lower)
{
	struct pon_nl rtnl;
	unsigned int lower_idx;
	int err;

	err = lower_resolve(lower, &lower_idx);
	if (err)
		return err;

	if (pon_nl_open(&rtnl, NETLINK_ROUTE)) {
		fprintf(stderr, "cannot open a route netlink socket\n");
		return ERR_REPORTED;
	}
	return link_report(&rtnl, pon_mapper_link_add(&rtnl, name, lower_idx,
						      NULL));
}

/**
 * pon_netdev_del - pon gem netdev del <name>, pon gem mapper del <name>
 * @name: the name of the device
 * @kind: the link kind the device must have
 * @what: the device in the refusal, such as "GEM network device"
 *
 * Deletes only a device of @kind; the link dump of libomci-apply gives the
 * kind. Any other device is refused.
 *
 * Return: 0, or ERR_REPORTED after a message.
 */
static int pon_netdev_del(const char *name, const char *kind,
			  const char *what)
{
	static struct oa_links links;
	struct pon_nl rtnl;
	unsigned int ifindex = if_nametoindex(name);
	int err;

	if (!ifindex) {
		fprintf(stderr, "no such interface: %s\n", name);
		return ERR_REPORTED;
	}

	if (pon_nl_open(&rtnl, NETLINK_ROUTE)) {
		fprintf(stderr, "cannot open a route netlink socket\n");
		return ERR_REPORTED;
	}
	err = oa_links_dump(&rtnl, link_ifindex_keep, &ifindex, &links);
	if (err)
		return link_report(&rtnl, err);
	if (!links.n || strcmp(links.l[0].kind, kind)) {
		fprintf(stderr, "not a %s: %s\n", what, name);
		pon_nl_close(&rtnl);
		return ERR_REPORTED;
	}
	return link_report(&rtnl, pon_gem_link_del(&rtnl, ifindex));
}

/**
 * gem_netdev_cmd - pon gem netdev add|del ...
 * @argc: the number of arguments from "netdev"
 * @argv: the arguments from "netdev"
 *
 * Return: 0, -EINVAL for bad arguments, or ERR_REPORTED.
 */
static int gem_netdev_cmd(int argc, char **argv)
{
	uint32_t gem_id;

	if ((argc == 4 || argc == 5) && !strcmp(argv[1], "add")) {
		if (number_get(argv[2], UINT32_MAX, &gem_id))
			return -EINVAL;
		return gem_netdev_add(argv[3], gem_id,
				      argc > 4 ? argv[4] : NULL);
	}
	if (argc == 3 && !strcmp(argv[1], "del"))
		return pon_netdev_del(argv[2], "gem", "GEM network device");
	if (!strcmp(argv[1], "add"))
		return args_refuse("gem netdev add");
	if (!strcmp(argv[1], "del"))
		return args_refuse("gem netdev del");
	return verb_refuse("gem netdev", argv[1]);
}

/**
 * gem_mapper_cmd - pon gem mapper add|del ...
 * @argc: the number of arguments from "mapper"
 * @argv: the arguments from "mapper"
 *
 * Return: 0, -EINVAL for bad arguments, or ERR_REPORTED.
 */
static int gem_mapper_cmd(int argc, char **argv)
{
	if ((argc == 3 || argc == 4) && !strcmp(argv[1], "add"))
		return mapper_netdev_add(argv[2], argc > 3 ? argv[3] : NULL);
	if (argc == 3 && !strcmp(argv[1], "del"))
		return pon_netdev_del(argv[2], "gem-mapper",
				      "gem-mapper network device");
	if (!strcmp(argv[1], "add"))
		return args_refuse("gem mapper add");
	if (!strcmp(argv[1], "del"))
		return args_refuse("gem mapper del");
	return verb_refuse("gem mapper", argv[1]);
}

/**
 * gem_map_cmd - pon gem map add|del <dev-id> <gem-id> <pbit> [mapper <name>]
 * @argc: the number of arguments from "map"
 * @argv: the arguments from "map"
 *
 * Without a mapper the rule belongs to the mapper of the data interface.
 *
 * Return: 0, -EINVAL for bad arguments, or ERR_REPORTED.
 */
static int gem_map_cmd(int argc, char **argv)
{
	struct pon_gem_map map = {};
	bool add;

	add = !strcmp(argv[1], "add");
	if (!add && strcmp(argv[1], "del"))
		return verb_refuse("gem map", argv[1]);
	if (argc != 5 && !(argc == 7 && !strcmp(argv[5], "mapper")))
		return args_refuse(add ? "gem map add" : "gem map del");

	if (ids_get(argv[2], argv[3], &map.dev_id, &map.gem_id) ||
	    number_get(argv[4], 7, &map.pbit))
		return -EINVAL;

	if (argc == 7) {
		map.ifindex = if_nametoindex(argv[6]);
		if (!map.ifindex) {
			fprintf(stderr, "no such interface: %s\n", argv[6]);
			return ERR_REPORTED;
		}
	}

	if (add)
		return report(pon_gem_map_new(&pon, &map));
	return report(pon_gem_map_del(&pon, &map));
}

/**
 * tcont_cmd - pon tcont set|del ...
 * @argc: the number of arguments from "tcont"
 * @argv: the arguments from "tcont"
 *
 * Return: 0, -EINVAL for bad arguments, or ERR_REPORTED.
 */
static int tcont_cmd(int argc, char **argv)
{
	uint32_t dev_id, index, alloc_id;
	bool set;

	if (argc < 2)
		return args_refuse("tcont");
	set = !strcmp(argv[1], "set");
	if (!set && strcmp(argv[1], "del"))
		return verb_refuse("tcont", argv[1]);
	if (argc != (set ? 5 : 4))
		return args_refuse(set ? "tcont set" : "tcont del");
	if (ids_get(argv[2], argv[3], &dev_id, &index))
		return -EINVAL;
	if (!set)
		return report(pon_tcont_del(&pon, dev_id, index));
	if (number_get(argv[4], UINT32_MAX, &alloc_id))
		return -EINVAL;
	return report(pon_tcont_set(&pon, dev_id, index, alloc_id));
}

/**
 * gem_setting_get - one key and value of pon gem new
 * @gem: the port, changed
 * @key: the key
 * @val: the value
 *
 * Return: 0, or -EINVAL after a message.
 */
static int gem_setting_get(struct pon_gem *gem, const char *key,
			   const char *val)
{
	if (!strcmp(key, "tcont"))
		return int_get(val, &gem->tcont_index);
	if (!strcmp(key, "queue"))
		return int_get(val, &gem->queue);
	if (!strcmp(key, "no-offload")) {
		gem->no_offload = flag_value(val);
		if (gem->no_offload >= 0)
			return 0;
	} else if (!strcmp(key, "key-ring")) {
		gem->key_ring = pon_gem_key_ring_value(val);
		if (gem->key_ring >= 0)
			return 0;
	} else {
		fprintf(stderr, "unknown setting: %s\n", key);
		return -EINVAL;
	}
	fprintf(stderr, "bad value for %s\n", key);
	return -EINVAL;
}

/**
 * gem_new_cmd - pon gem new <dev-id> <gem-id> <dir> [key value ...]
 * @argc: the number of arguments from "gem"
 * @argv: the arguments from "gem"
 *
 * The direction is upstream, downstream or bidir. The keys are tcont,
 * key-ring, queue and no-offload. An upstream or bidir port needs tcont; a
 * downstream port takes none, as the kernel requires.
 *
 * Return: 0, -EINVAL for bad arguments, or ERR_REPORTED.
 */
static int gem_new_cmd(int argc, char **argv)
{
	struct pon_gem gem = {
		.tcont_index = -1,
		.key_ring = -1,
		.queue = -1,
		.no_offload = -1,
	};
	int i, dir;

	if (argc < 5 || (argc - 5) % 2)
		return args_refuse("gem new");
	if (ids_get(argv[2], argv[3], &gem.dev_id, &gem.id))
		return -EINVAL;
	dir = pon_gem_dir_value(argv[4]);
	if (dir < 0) {
		fprintf(stderr, "bad value for dir\n");
		return -EINVAL;
	}
	gem.dir = dir;
	for (i = 5; i < argc; i += 2) {
		if (gem_setting_get(&gem, argv[i], argv[i + 1]))
			return -EINVAL;
	}
	if (gem.dir == PON_GEM_DIR_DOWNSTREAM && gem.tcont_index >= 0) {
		fprintf(stderr, "a downstream GEM port takes no tcont\n");
		return -EINVAL;
	}
	if (gem.dir != PON_GEM_DIR_DOWNSTREAM && gem.tcont_index < 0) {
		fprintf(stderr, "an upstream or bidir GEM port needs tcont\n");
		return -EINVAL;
	}
	return report(pon_gem_new(&pon, &gem));
}

/**
 * maps_cmd - pon maps
 * @argc: the number of arguments from "maps"
 * @argv: the arguments from "maps"
 *
 * Lists every classifier rule.
 *
 * Return: 0, -EINVAL for an argument, or ERR_REPORTED.
 */
static int maps_cmd(int argc, char **argv)
{
	int err;

	if (argc != 1)
		return args_refuse(argv[0]);
	list_begin("gem-map");
	err = report(pon_gem_map_dump(&pon, PON_DEV_ANY, gem_map_cb, NULL));
	list_end();
	return err;
}

/**
 * gem_cmd - pon gem [new|del|netdev|mapper|map|maps ...]
 * @argc: the number of arguments from "gem"
 * @argv: the arguments from "gem"
 *
 * Return: 0, -EINVAL for bad arguments, or ERR_REPORTED.
 */
static int gem_cmd(int argc, char **argv)
{
	uint32_t dev_id, gem_id;

	if (argc == 1)
		return gems_cmd(argc, argv);
	if (!strcmp(argv[1], "new"))
		return gem_new_cmd(argc, argv);
	if (!strcmp(argv[1], "netdev"))
		return argc >= 3 ? gem_netdev_cmd(argc - 1, argv + 1) :
				   args_refuse("gem netdev");
	if (!strcmp(argv[1], "maps"))
		return maps_cmd(argc - 1, argv + 1);
	if (!strcmp(argv[1], "map"))
		return argc >= 3 ? gem_map_cmd(argc - 1, argv + 1) :
				   args_refuse("gem map");
	if (!strcmp(argv[1], "mapper"))
		return argc >= 3 ? gem_mapper_cmd(argc - 1, argv + 1) :
				   args_refuse("gem mapper");
	if (strcmp(argv[1], "del"))
		return verb_refuse("gem", argv[1]);
	if (argc != 4)
		return args_refuse("gem del");
	if (ids_get(argv[2], argv[3], &dev_id, &gem_id))
		return -EINVAL;
	return report(pon_gem_del(&pon, dev_id, gem_id));
}

/**
 * stats_cmd - pon stats [dev-id gem-id]
 * @argc: the number of arguments from "stats"
 * @argv: the arguments from "stats"
 *
 * Prints the counters of every GEM port, or of one.
 *
 * Return: 0, -EINVAL for bad arguments, or ERR_REPORTED.
 */
static int stats_cmd(int argc, char **argv)
{
	uint32_t dev_id, gem_id;
	struct pon_gem_stats stats;
	int err;

	if (argc != 1 && argc != 3)
		return args_refuse(argv[0]);
	if (argc == 3 && ids_get(argv[1], argv[2], &dev_id, &gem_id))
		return -EINVAL;
	list_begin("gem-stats");
	if (argc == 1) {
		err = report(pon_stats_dump(&pon, PON_DEV_ANY, stats_cb, NULL));
	} else {
		err = report(pon_stats_get(&pon, dev_id, gem_id, &stats));
		if (!err)
			stats_print(&stats);
	}
	list_end();
	return err;
}

/**
 * fec_cmd - pon fec <dev-id>
 * @argc: the number of arguments from "fec"
 * @argv: the arguments from "fec"
 *
 * Prints the FEC counters of one device.
 *
 * Return: 0, -EINVAL for bad arguments, or ERR_REPORTED.
 */
static int fec_cmd(int argc, char **argv)
{
	struct pon_fec_stats fec;
	uint32_t dev_id;
	int err;

	if (argc != 2)
		return args_refuse(argv[0]);
	if (number_get(argv[1], UINT32_MAX, &dev_id))
		return -EINVAL;
	err = report(pon_fec_get(&pon, dev_id, &fec));
	if (err)
		return err;
	list_begin("fec");
	obj_begin(NULL);
	field_u("dev", "dev-id", fec.dev_id);
	text_word("fec");
	field_u("corrected", "corrected", fec.corrected);
	field_u("uncorrectable", "uncorrectable", fec.uncorrectable);
	field_u("corrected-bytes", "corrected-bytes", fec.corrected_bytes);
	field_u("total-codewords", "total-codewords", fec.total_codewords);
	field_u("seconds", "seconds", fec.seconds);
	obj_end();
	list_end();
	return 0;
}

/**
 * tc_groups_get - the groups of the TC layer counters a command names
 * @argc: the number of group names
 * @argv: the group names
 * @groups: the groups, bit N for enum pon_tc_stats_group value N, or
 *	    PON_TC_GROUPS_ALL when @argc is 0
 *
 * Return: 0, or -EINVAL for a name that is no group.
 */
static int tc_groups_get(int argc, char **argv, uint32_t *groups)
{
	int group, i;

	*groups = PON_TC_GROUPS_ALL;
	for (i = 0; i < argc; i++) {
		group = pon_tc_group_value(argv[i]);
		if (group < 0) {
			fprintf(stderr, "pon: unknown counter group %s\n",
				argv[i]);
			return -EINVAL;
		}
		*groups |= 1u << group;
	}
	return 0;
}

/**
 * tc_group_print - print the counters of one group of the TC layer
 * @stats: the counters pon_tc_stats_get() filled
 * @group: an enum pon_tc_stats_group value
 */
static void tc_group_print(const struct pon_tc_stats *stats,
			   unsigned int group)
{
	const struct pon_tc_group *counters = &stats->group[group];
	unsigned int counter;

	if (!counters->has)
		return;
	if (json)
		json_open(pon_tc_group_name(group), '{');
	for (counter = 1; counter <= PON_TC_GROUP_COUNTERS; counter++) {
		if (!pon_tc_stats_has(stats, group, counter))
			continue;
		if (json) {
			field_u(NULL, pon_tc_stat_name(group, counter),
				counters->counter[counter]);
			continue;
		}
		printf("dev %u tc %s %s %llu\n", stats->dev_id,
		       pon_tc_group_name(group),
		       pon_tc_stat_name(group, counter),
		       (unsigned long long)counters->counter[counter]);
	}
	if (json)
		json_close('}');
}

/**
 * tc_stats_cmd - pon tc-stats <dev-id> [group ...]
 * @argc: the number of arguments from "tc-stats"
 * @argv: the arguments from "tc-stats"
 *
 * Prints the counters of the TC layer that the device keeps, group by group,
 * or those of the groups named after the device.
 *
 * Return: 0, -EINVAL for bad arguments, or ERR_REPORTED.
 */
static int tc_stats_cmd(int argc, char **argv)
{
	struct pon_tc_stats stats;
	unsigned int group;
	uint32_t dev_id;
	uint32_t groups;
	int err;

	if (argc < 2)
		return args_refuse(argv[0]);
	if (number_get(argv[1], UINT32_MAX, &dev_id) ||
	    tc_groups_get(argc - 2, argv + 2, &groups))
		return -EINVAL;
	err = report(pon_tc_stats_get(&pon, dev_id, groups, &stats));
	if (err)
		return err;
	list_begin("tc-stats");
	if (json) {
		obj_begin(NULL);
		field_u(NULL, "dev-id", stats.dev_id);
	}
	for (group = 0; group < PON_TC_GROUPS; group++)
		tc_group_print(&stats, group);
	if (json)
		obj_end();
	list_end();
	return 0;
}

/**
 * omci_stats_cmd - pon omci-stats <dev-id>
 * @argc: the number of arguments from "omci-stats"
 * @argv: the arguments from "omci-stats"
 *
 * Prints the counters of the OMCI channel of one device.
 *
 * Return: 0, -EINVAL for bad arguments, or ERR_REPORTED.
 */
static int omci_stats_cmd(int argc, char **argv)
{
	struct pon_omci_stats stats;
	uint32_t dev_id;
	int err;

	if (argc != 2)
		return args_refuse(argv[0]);
	if (number_get(argv[1], UINT32_MAX, &dev_id))
		return -EINVAL;
	err = report(pon_omci_stats_get(&pon, dev_id, &stats));
	if (err)
		return err;
	list_begin("omci-stats");
	obj_begin(NULL);
	field_u("dev", "dev-id", stats.dev_id);
	text_word("omci");
	field_u("rx", "rx", stats.rx);
	field_u("rx-dropped", "rx-dropped", stats.rx_dropped);
	field_u("rx-errors", "rx-errors", stats.rx_errors);
	field_u("tx", "tx", stats.tx);
	field_u("tx-errors", "tx-errors", stats.tx_errors);
	obj_end();
	list_end();
	return 0;
}

/**
 * usage_print - print the usage text
 * @to: the stream, stderr after bad arguments, stdout for pon help
 */
static void usage_print(FILE *to)
{
	fprintf(to,
		"usage: pon dev [dev-id] | gems [dev-id gem-id] | maps\n"
		"       pon gem | gem maps, the same lists as gems and maps\n"
		"       pon tconts [dev-id index] | alarms [dev-id] | monitor\n"
		"       pon omci listen <dev-id> | omci send <dev-id> <hex>\n"
		"       pon stats [dev-id gem-id] | fec <dev-id> | omci-stats <dev-id>\n"
		"       pon tc-stats <dev-id> [xgtc | ploam-ds | ploam-us ...]\n"
		"       pon tcont set <dev-id> <index> <alloc-id>\n"
		"       pon tcont del <dev-id> <index>\n"
		"       pon gem new <dev-id> <gem-id> <dir> [tcont N]\n"
		"                   <dir> upstream|downstream|bidir\n"
		"                   tcont N for upstream and bidir only\n"
		"                   [key-ring none|unicast|unicast-downstream]\n"
		"                   [queue N]\n"
		"                   [no-offload 0|1]\n"
		"       pon gem del <dev-id> <gem-id>\n"
		"       pon gem netdev add <gem-id> <name> [lower]\n"
		"       pon gem netdev del <name>\n"
		"       pon gem mapper add <name> [lower]\n"
		"       pon gem mapper del <name>\n"
		"       pon gem map add|del <dev-id> <gem-id> <pbit>\n"
		"                   [mapper <name>]\n"
		"       pon dev set <dev-id> <key> <value> ...\n"
		"                   mode, serial, regid, enable, disabled 1\n"
		"       pon -j <command>, JSON for every command but omci\n"
		"       pon help | -h | --help\n");
}

/**
 * help_asked - whether the command line asks for the usage text
 * @argc: the number of arguments
 * @argv: the arguments
 *
 * Return: true for pon help, pon -h and pon --help.
 */
static bool help_asked(int argc, char **argv)
{
	return argc == 2 && (!strcmp(argv[1], "help") ||
			     !strcmp(argv[1], "-h") ||
			     !strcmp(argv[1], "--help"));
}

/**
 * command_run - run the command of a command line
 * @argc: the number of arguments from the command
 * @argv: the arguments from the command
 *
 * Return: 0, -EINVAL for bad arguments, or ERR_REPORTED.
 */
static int command_run(int argc, char **argv)
{
	if (!strcmp(argv[0], "dev"))
		return dev_cmd(argc, argv);
	if (!strcmp(argv[0], "tconts"))
		return tconts_cmd(argc, argv);
	if (!strcmp(argv[0], "alarms"))
		return alarms_cmd(argc, argv);
	if (!strcmp(argv[0], "gems"))
		return gems_cmd(argc, argv);
	if (!strcmp(argv[0], "maps"))
		return maps_cmd(argc, argv);
	if (!strcmp(argv[0], "stats"))
		return stats_cmd(argc, argv);
	if (!strcmp(argv[0], "fec"))
		return fec_cmd(argc, argv);
	if (!strcmp(argv[0], "tc-stats"))
		return tc_stats_cmd(argc, argv);
	if (!strcmp(argv[0], "omci-stats"))
		return omci_stats_cmd(argc, argv);
	if (!strcmp(argv[0], "monitor"))
		return monitor_run(argc, argv);
	if (!strcmp(argv[0], "omci"))
		return omci_cmd(argc, argv);
	if (!strcmp(argv[0], "tcont"))
		return tcont_cmd(argc, argv);
	if (!strcmp(argv[0], "gem"))
		return gem_cmd(argc, argv);
	return verb_refuse(NULL, argv[0]);
}

/**
 * main - pon, the command line tool of the generic netlink family "pon"
 * @argc: the number of arguments
 * @argv: the arguments
 *
 * pon maps prints every classifier rule, as pon gem maps does. pon help
 * prints the usage text on stdout without opening the family. With -j
 * first, a command prints JSON instead of text; a command that fails
 * prints nothing on stdout. pon -j monitor prints one JSON object per
 * notification.
 *
 * Return: 0 for success and for pon help, 1 after a failure, 2 for bad
 * arguments.
 */
int main(int argc, char **argv)
{
	char *reply = NULL;
	size_t reply_len = 0;
	int err;

	if (argc > 1 && !strcmp(argv[1], "-j")) {
		json = true;
		argc--;
		argv++;
	}
	if (help_asked(argc, argv)) {
		usage_print(stdout);
		return 0;
	}
	if (argc < 2) {
		usage_print(stderr);
		return 2;
	}
	if (json && !strcmp(argv[1], "omci")) {
		fprintf(stderr, "pon omci has no JSON form\n");
		usage_print(stderr);
		return 2;
	}

	err = pon_open(&pon, NULL, NULL);
	if (err) {
		fprintf(stderr, err == -ENODEV ?
			"the pon family is not registered\n" :
			"cannot connect a generic netlink socket\n");
		return 1;
	}
	if (json && strcmp(argv[1], "monitor")) {
		out = open_memstream(&reply, &reply_len);
		if (!out) {
			fprintf(stderr, "error: %s\n", strerror(errno));
			pon_close(&pon);
			return 1;
		}
	}

	err = command_run(argc - 1, argv + 1);

	pon_close(&pon);
	if (out) {
		fclose(out);
		out = NULL;
		if (!err)
			fwrite(reply, 1, reply_len, stdout);
		free(reply);
	}
	if (err == -EINVAL) {
		usage_print(stderr);
		return 2;
	}
	return err ? 1 : 0;
}
