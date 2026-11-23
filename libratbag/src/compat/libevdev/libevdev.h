/*
 * Minimal stand-in for <libevdev/libevdev.h> used by the portable (non-Linux)
 * libratbag build. The drivers only use libevdev to turn key codes into
 * names for debug log messages, so a numeric name is good enough.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdio.h>
#include <linux/input.h>

static inline const char *
libevdev_event_code_get_name(unsigned int type, unsigned int code)
{
	static __thread char name[32];

	snprintf(name, sizeof(name), "%s_%u", type == EV_KEY ? "KEY" : "CODE", code);
	return name;
}
