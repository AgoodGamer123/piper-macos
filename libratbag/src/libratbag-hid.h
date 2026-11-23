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
 * Portable HID access for platforms without udev/hidraw (macOS).
 *
 * The drivers and the HID++ helpers talk to a device through a plain file
 * descriptor: write() sends one output report, read() returns one input
 * report, poll() waits for input. Linux' hidraw gives us exactly that. On
 * other platforms a backend opens the HID interface and hands libratbag one
 * end of a datagram socketpair that behaves the same way, pumping reports
 * between the socket and the OS HID API on a helper thread. That keeps every
 * driver unchanged.
 *
 * Feature reports have no fd equivalent, so they go through the backend's
 * get_feature/set_feature callbacks.
 */

#pragma once

#ifdef RATBAG_PORTABLE_HID

#include <stddef.h>
#include <stdint.h>

struct ratbag;

/* One HID interface, the equivalent of one /dev/hidraw node. */
struct ratbag_hid_node {
	/* backend specific, unique per HID interface */
	char *path;
	/* identical for all interfaces of the same physical device; used to
	 * find sibling interfaces like udev's "parent usb_device" */
	char *group;
	char *name;
	uint16_t bustype;
	uint16_t vendor;
	uint16_t product;
	/* USB interface number, used to order siblings; -1 if unknown */
	int interface_number;
};

struct ratbag_hid_backend {
	const char *name;

	/* Returns the number of nodes and a malloc'ed array in *nodes, or a
	 * negative errno. Free with ratbag_hid_nodes_free(). */
	int (*enumerate)(struct ratbag *ratbag, struct ratbag_hid_node **nodes);

	/* Open a node. On success *fd behaves like a hidraw fd (see above),
	 * *desc is a malloc'ed copy of the report descriptor and *handle is
	 * passed to the other callbacks. Returns 0 or a negative errno. */
	int (*open)(struct ratbag *ratbag, const struct ratbag_hid_node *node,
		    int *fd, uint8_t **desc, size_t *desc_len, void **handle);

	/* Close the node, including the fd returned by open */
	void (*close)(struct ratbag *ratbag, void *handle);

	/* buf[0] is the report ID. Return the number of bytes, or a negative
	 * errno. */
	int (*get_feature)(void *handle, uint8_t *buf, size_t len);
	int (*set_feature)(void *handle, const uint8_t *buf, size_t len);
};

/* Picks the backend for this process: the emulator backend if
 * RATBAG_EMU_SOCKET is set, otherwise the native one (IOKit on macOS). May
 * return NULL if there is no backend for this platform. */
const struct ratbag_hid_backend *
ratbag_hid_backend_get(void);

extern const struct ratbag_hid_backend ratbag_hid_backend_emu;
#ifdef __APPLE__
extern const struct ratbag_hid_backend ratbag_hid_backend_iokit;
#endif

void
ratbag_hid_node_clear(struct ratbag_hid_node *node);

void
ratbag_hid_node_copy(struct ratbag_hid_node *dst,
		     const struct ratbag_hid_node *src);

void
ratbag_hid_nodes_free(struct ratbag_hid_node *nodes, int count);

/*
 * Socketpair "pump" shared by backends whose OS API is callback based.
 * ratbag_hid_pump_new() creates the pair; the backend gives
 * ratbag_hid_pump_fd() to libratbag, calls ratbag_hid_pump_push_input() for
 * every input report it receives and gets a callback for every output
 * report libratbag writes.
 */
struct ratbag_hid_pump;

typedef int (*ratbag_hid_pump_output_func)(void *userdata,
					   const uint8_t *buf, size_t len);

struct ratbag_hid_pump *
ratbag_hid_pump_new(ratbag_hid_pump_output_func output, void *userdata);

int
ratbag_hid_pump_fd(const struct ratbag_hid_pump *pump);

void
ratbag_hid_pump_push_input(struct ratbag_hid_pump *pump,
			   const uint8_t *buf, size_t len);

/* Stops the output thread and closes both ends of the socketpair */
void
ratbag_hid_pump_destroy(struct ratbag_hid_pump *pump);

#endif /* RATBAG_PORTABLE_HID */
