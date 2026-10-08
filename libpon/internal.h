/* SPDX-License-Identifier: LGPL-2.1-only */
/* Copyright (C) 2026 John Crispin <john@phrozen.org> */
#ifndef LIBPON_INTERNAL_H
#define LIBPON_INTERNAL_H

#include "libpon/pon.h"

#define ARRAY_SIZE(tab)	(sizeof(tab) / sizeof((tab)[0]))

int lp_errno(int err);
int lp_refuse(struct pon_nl *nl, int err, const char *why);
int lp_nomem(struct pon_nl *nl);
int lp_put_failed(struct pon_nl *nl, struct nl_msg *msg);
int lp_seq_ok(struct nl_msg *msg, void *arg);
struct nl_msg *lp_msg_new(struct pon *pon, int cmd, int flags);
int lp_dev_decode(struct nl_msg *msg, struct pon_dev_info *out);
int lp_tcont_decode(struct nl_msg *msg, struct pon_tcont *out);
int lp_gem_decode(struct nl_msg *msg, struct pon_gem *out);
int lp_gem_map_decode(struct nl_msg *msg, struct pon_gem_map *out);
int lp_alarm_decode(struct nl_msg *msg, struct pon_alarms *out);
int lp_event_decode(struct nl_msg *msg, struct pon_event *out);

#endif
