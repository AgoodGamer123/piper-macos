/*
 * Copyright © 2026 libratbag contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/*
 * ratbag-probe: list the HID devices libratbag can see and dump what it
 * finds on the supported ones. Portable (macOS) builds only; on Linux use
 * ratbagctl.
 *
 *   ratbag-probe              list devices and dump the supported ones
 *   ratbag-probe -v           ... with debug output
 *   ratbag-probe -r           ... with every HID report sent/received
 *   ratbag-probe --set-dpi N  set the active resolution of the active
 *                             profile to N dpi and write it to the device
 */

#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libratbag.h"

#define MAX_DEVICES 8

static const char *
action_str(struct ratbag_button *button, char *buf, size_t len)
{
	switch (ratbag_button_get_action_type(button)) {
	case RATBAG_BUTTON_ACTION_TYPE_NONE:
		return "disabled";
	case RATBAG_BUTTON_ACTION_TYPE_BUTTON:
		snprintf(buf, len, "button %u", ratbag_button_get_button(button));
		return buf;
	case RATBAG_BUTTON_ACTION_TYPE_SPECIAL:
		snprintf(buf, len, "special 0x%x", ratbag_button_get_special(button));
		return buf;
	case RATBAG_BUTTON_ACTION_TYPE_KEY:
		snprintf(buf, len, "key %u", ratbag_button_get_key(button));
		return buf;
	case RATBAG_BUTTON_ACTION_TYPE_MACRO:
		return "macro";
	default:
		return "unknown";
	}
}

static void
dump_device(struct ratbag_device *device)
{
	unsigned int nprofiles = ratbag_device_get_num_profiles(device);
	const char *fw = ratbag_device_get_firmware_version(device);
	int slot = ratbag_device_get_receiver_index(device);

	printf("  device: %s\n", ratbag_device_get_name(device));
	printf("    id: %s:%04x:%04x%s", ratbag_device_get_bustype(device) ?: "?",
	       ratbag_device_get_vendor_id(device),
	       ratbag_device_get_product_id(device),
	       slot >= 0 ? "" : "\n");
	if (slot >= 0)
		printf(" (via receiver, slot %d)\n", slot);
	if (fw && *fw)
		printf("    firmware: %s\n", fw);
	printf("    profiles: %u, buttons: %u, leds: %u\n", nprofiles,
	       ratbag_device_get_num_buttons(device),
	       ratbag_device_get_num_leds(device));

	for (unsigned int p = 0; p < nprofiles; p++) {
		struct ratbag_profile *profile = ratbag_device_get_profile(device, p);
		unsigned int rates[16];
		size_t nrates;
		const char *name;

		if (!profile)
			continue;

		name = ratbag_profile_get_name(profile);
		printf("    profile %u%s%s%s%s:\n", p,
		       name && *name ? " \"" : "", name && *name ? name : "",
		       name && *name ? "\"" : "",
		       ratbag_profile_is_active(profile) ? " (active)" : "");
		if (!ratbag_profile_is_enabled(profile))
			printf("      disabled\n");

		nrates = ratbag_profile_get_report_rate_list(profile, rates, 16);
		printf("      report rate: %d Hz (", ratbag_profile_get_report_rate(profile));
		for (size_t i = 0; i < nrates; i++)
			printf("%s%u", i ? ", " : "", rates[i]);
		printf(")\n");

		for (unsigned int r = 0; r < ratbag_profile_get_num_resolutions(profile); r++) {
			struct ratbag_resolution *res = ratbag_profile_get_resolution(profile, r);

			if (!res)
				continue;
			printf("      resolution %u: %d dpi%s%s\n", r,
			       ratbag_resolution_get_dpi(res),
			       ratbag_resolution_is_active(res) ? " (active)" : "",
			       ratbag_resolution_is_default(res) ? " (default)" : "");
			ratbag_resolution_unref(res);
		}

		for (unsigned int b = 0; b < ratbag_device_get_num_buttons(device); b++) {
			struct ratbag_button *button = ratbag_profile_get_button(profile, b);
			char buf[64];

			if (!button)
				continue;
			printf("      button %u: %s\n", b, action_str(button, buf, sizeof(buf)));
			ratbag_button_unref(button);
		}

		for (unsigned int l = 0; l < ratbag_device_get_num_leds(device); l++) {
			struct ratbag_led *led = ratbag_profile_get_led(profile, l);
			struct ratbag_color c;

			if (!led)
				continue;
			c = ratbag_led_get_color(led);
			printf("      led %u: mode %d, color #%02x%02x%02x, brightness %u\n", l,
			       ratbag_led_get_mode(led), c.red, c.green, c.blue,
			       ratbag_led_get_brightness(led));
			ratbag_led_unref(led);
		}

		ratbag_profile_unref(profile);
	}
}

static int
set_dpi(struct ratbag_device *device, int dpi)
{
	unsigned int nprofiles = ratbag_device_get_num_profiles(device);
	int rc = -ENOENT;

	for (unsigned int p = 0; p < nprofiles; p++) {
		struct ratbag_profile *profile = ratbag_device_get_profile(device, p);

		if (!profile)
			continue;
		if (ratbag_profile_is_active(profile)) {
			for (unsigned int r = 0; r < ratbag_profile_get_num_resolutions(profile); r++) {
				struct ratbag_resolution *res = ratbag_profile_get_resolution(profile, r);

				if (res && ratbag_resolution_is_active(res)) {
					rc = ratbag_resolution_set_dpi(res, dpi);
					printf("  set resolution %u to %d dpi: %s\n", r, dpi,
					       rc == 0 ? "ok" : "failed");
				}
				ratbag_resolution_unref(res);
			}
		}
		ratbag_profile_unref(profile);
	}

	if (rc == 0) {
		rc = ratbag_device_commit(device);
		printf("  commit: %s\n", rc == 0 ? "ok" : "failed");
	}
	return rc;
}

int
main(int argc, char **argv)
{
	enum ratbag_log_priority priority = RATBAG_LOG_PRIORITY_ERROR;
	struct ratbag *ratbag;
	char *list, *line, *saveptr = NULL;
	int dpi = 0, found = 0;

	while (1) {
		static struct option opts[] = {
			{ "verbose", no_argument, 0, 'v' },
			{ "raw", no_argument, 0, 'r' },
			{ "set-dpi", required_argument, 0, 'd' },
			{ "help", no_argument, 0, 'h' },
			{ 0, 0, 0, 0 },
		};
		int c = getopt_long(argc, argv, "vrd:h", opts, NULL);

		if (c == -1)
			break;
		switch (c) {
		case 'v':
			priority = RATBAG_LOG_PRIORITY_DEBUG;
			break;
		case 'r':
			priority = RATBAG_LOG_PRIORITY_RAW;
			break;
		case 'd':
			dpi = atoi(optarg);
			break;
		default:
			printf("Usage: %s [--verbose] [--raw] [--set-dpi N]\n", argv[0]);
			return c == 'h' ? 0 : 1;
		}
	}

	ratbag = ratbag_create_context_default();
	if (!ratbag) {
		fprintf(stderr, "Failed to create a libratbag context\n");
		return 1;
	}
	ratbag_log_set_priority(ratbag, priority);

	list = ratbag_hid_list_groups(ratbag);
	if (!list) {
		fprintf(stderr, "No HID backend available on this system\n");
		ratbag_unref(ratbag);
		return 1;
	}

	for (line = strtok_r(list, "\n", &saveptr); line; line = strtok_r(NULL, "\n", &saveptr)) {
		struct ratbag_device *devices[MAX_DEVICES];
		char group[256], name[256] = "";
		unsigned int bus, vid, pid;
		int n;

		if (sscanf(line, "%255[^\t]\t%x\t%x\t%x\t%255[^\n]", group, &bus, &vid, &pid, name) < 4)
			continue;

		printf("%04x:%04x %s [%s]\n", vid, pid, name, group);

		n = ratbag_devices_new_from_group(ratbag, group, devices, MAX_DEVICES);
		for (int i = 0; i < n; i++) {
			found++;
			dump_device(devices[i]);
			if (dpi > 0)
				set_dpi(devices[i], dpi);
			ratbag_device_unref(devices[i]);
		}
	}

	if (!found)
		printf("\nNo supported device found.\n");

	ratbag_free_string(list);
	ratbag_unref(ratbag);

	return found ? 0 : 2;
}
