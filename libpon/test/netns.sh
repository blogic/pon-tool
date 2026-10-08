#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-only
# Copyright (C) 2026 John Crispin <john@phrozen.org>
T_SKIP=77

unshare -rn true 2>/dev/null || exit $T_SKIP
exec unshare -rn "$@"
