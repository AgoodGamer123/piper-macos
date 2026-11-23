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
 * The portable replacement for the udev/hidraw half of libratbag-hidraw.c,
 * plus device discovery for platforms without udev. See libratbag-hid.h.
 */

#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "libratbag-private.h"
#include "libratbag-hidraw.h"
#include "libratbag-hid.h"
#include "libratbag-data.h"

#define HID_MAX_BUFFER_SIZE	4096
#define LOGITECH_VENDOR_ID	0x046d
#define HIDPP_SHORT		0x10
#define HIDPP_LONG		0x11
#define HIDPP_SHORT_LEN		7
#define HIDPP_LONG_LEN		20
#define HIDPP_MAX_PAIRED	6

/* ------------------------------------------------------------------------ */
/* nodes                                                                    */
/* ------------------------------------------------------------------------ */

void
ratbag_hid_node_clear(struct ratbag_hid_node *node)
{
	if (!node)
		return;
	free(node->path);
	free(node->group);
	free(node->name);
	memset(node, 0, sizeof(*node));
}

void
ratbag_hid_node_copy(struct ratbag_hid_node *dst,
		     const struct ratbag_hid_node *src)
{
	*dst = *src;
	dst->path = strdup_safe(src->path);
	dst->group = strdup_safe(src->group);
	dst->name = strdup_safe(src->name ? src->name : "");
}

void
ratbag_hid_nodes_free(struct ratbag_hid_node *nodes, int count)
{
	if (!nodes)
		return;
	for (int i = 0; i < count; i++)
		ratbag_hid_node_clear(&nodes[i]);
	free(nodes);
}

const struct ratbag_hid_backend *
ratbag_hid_backend_get(void)
{
	const char *emu = getenv("RATBAG_EMU_SOCKET");

	if (emu && *emu)
		return &ratbag_hid_backend_emu;
#ifdef __APPLE__
	return &ratbag_hid_backend_iokit;
#else
	return NULL;
#endif
}

static int
node_cmp(const void *a, const void *b)
{
	const struct ratbag_hid_node *na = a, *nb = b;
	int rc;

	rc = strcmp(na->group, nb->group);
	if (rc)
		return rc;
	if (na->interface_number != nb->interface_number)
		return na->interface_number < nb->interface_number ? -1 : 1;
	return strcmp(na->path, nb->path);
}

/* All nodes, sorted by group and interface number */
static int
enumerate_sorted(struct ratbag *ratbag, struct ratbag_hid_node **nodes)
{
	int n;

	if (!ratbag->hid_backend)
		return -ENOTSUP;

	n = ratbag->hid_backend->enumerate(ratbag, nodes);
	if (n > 1)
		qsort(*nodes, n, sizeof(**nodes), node_cmp);
	return n;
}

/* ------------------------------------------------------------------------ */
/* the socketpair pump                                                      */
/* ------------------------------------------------------------------------ */

struct ratbag_hid_pump {
	int fds[2];		/* [0]: libratbag's end, [1]: ours */
	int wake[2];		/* pipe to stop the output thread */
	pthread_t thread;
	bool thread_started;
	ratbag_hid_pump_output_func output;
	void *userdata;
};

static void *
pump_output_thread(void *data)
{
	struct ratbag_hid_pump *pump = data;
	uint8_t buf[HID_MAX_BUFFER_SIZE];

	while (true) {
		struct pollfd fds[2] = {
			{ .fd = pump->fds[1], .events = POLLIN },
			{ .fd = pump->wake[0], .events = POLLIN },
		};
		ssize_t len;

		if (poll(fds, 2, -1) < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (fds[1].revents)
			break;
		if (!(fds[0].revents & POLLIN))
			continue;

		len = recv(pump->fds[1], buf, sizeof(buf), 0);
		if (len <= 0) {
			if (len < 0 && (errno == EINTR || errno == EAGAIN))
				continue;
			break;
		}
		pump->output(pump->userdata, buf, (size_t)len);
	}

	return NULL;
}

static void
set_socket_options(int fd)
{
	int size = 256 * 1024;

	setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size));
	setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size));
#ifdef SO_NOSIGPIPE
	{
		int one = 1;
		setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
	}
#endif
	fcntl(fd, F_SETFD, FD_CLOEXEC);
}

struct ratbag_hid_pump *
ratbag_hid_pump_new(ratbag_hid_pump_output_func output, void *userdata)
{
	struct ratbag_hid_pump *pump = zalloc(sizeof(*pump));

	pump->fds[0] = pump->fds[1] = -1;
	pump->wake[0] = pump->wake[1] = -1;
	pump->output = output;
	pump->userdata = userdata;

	/* SOCK_DGRAM keeps report boundaries, like hidraw */
	if (socketpair(AF_UNIX, SOCK_DGRAM, 0, pump->fds) < 0)
		goto err;
	if (pipe(pump->wake) < 0)
		goto err;

	set_socket_options(pump->fds[0]);
	set_socket_options(pump->fds[1]);
	fcntl(pump->wake[0], F_SETFD, FD_CLOEXEC);
	fcntl(pump->wake[1], F_SETFD, FD_CLOEXEC);

	if (pthread_create(&pump->thread, NULL, pump_output_thread, pump) != 0)
		goto err;
	pump->thread_started = true;

	return pump;

err:
	ratbag_hid_pump_destroy(pump);
	return NULL;
}

int
ratbag_hid_pump_fd(const struct ratbag_hid_pump *pump)
{
	return pump->fds[0];
}

void
ratbag_hid_pump_push_input(struct ratbag_hid_pump *pump,
			   const uint8_t *buf, size_t len)
{
	ssize_t rc;

	/* Never block the OS callback thread. If nobody reads (e.g. pointer
	 * motion on an interface we only use for configuration) the socket
	 * fills up and we drop reports, just like a full hidraw queue. */
	do {
		rc = send(pump->fds[1], buf, len, MSG_DONTWAIT);
	} while (rc < 0 && errno == EINTR);
}

void
ratbag_hid_pump_destroy(struct ratbag_hid_pump *pump)
{
	if (!pump)
		return;

	if (pump->thread_started) {
		char c = 'q';
		ssize_t rc;

		do {
			rc = write(pump->wake[1], &c, 1);
		} while (rc < 0 && errno == EINTR);
		pthread_join(pump->thread, NULL);
	}

	for (int i = 0; i < 2; i++) {
		if (pump->fds[i] >= 0)
			close(pump->fds[i]);
		if (pump->wake[i] >= 0)
			close(pump->wake[i]);
	}
	free(pump);
}

/* ------------------------------------------------------------------------ */
/* the hidraw API, portable flavour                                         */
/* ------------------------------------------------------------------------ */

static int
parse_descriptor(struct ratbag_device *device, struct ratbag_hidraw *hidraw,
		 const uint8_t *desc, size_t desc_len)
{
	size_t reports_size;
	int rc;

	hidraw->reports = NULL;
	rc = ratbag_hidraw_parse_report_descriptor_buf(device, hidraw, desc, desc_len);
	if (rc)
		return rc;

	reports_size = max(hidraw->num_reports, 1U) * sizeof(struct ratbag_hid_report);
	hidraw->reports = zalloc(reports_size);
	return ratbag_hidraw_parse_report_descriptor_buf(device, hidraw, desc, desc_len);
}

static int
ratbag_open_hid_node(struct ratbag_device *device,
		     const struct ratbag_hid_node *node, int idx)
{
	struct ratbag *ratbag = device->ratbag;
	struct ratbag_hidraw *hidraw = &device->hidraw[idx];
	_cleanup_free_ uint8_t *desc = NULL;
	size_t desc_len = 0;
	void *handle = NULL;
	int fd = -1, rc;

	assert(idx >= 0 && idx < MAX_HIDRAW);
	hidraw->fd = -1;

	/* Same basic sanity check as the hidraw path. A device behind a
	 * receiver has its own product ID but lives on the receiver's
	 * node, so only check the vendor there. */
	if (node->vendor != device->ids.vendor)
		return -ENODEV;
	if (device->hidpp_index < 0 &&
	    (node->product != device->ids.product ||
	     node->bustype != device->ids.bustype))
		return -ENODEV;

	rc = ratbag->hid_backend->open(ratbag, node, &fd, &desc, &desc_len, &handle);
	if (rc == -EACCES)
		ratbag->hid_access_denied = true;
	if (rc) {
		log_debug(ratbag, "%s: failed to open %s: %s\n",
			  device->name, node->path, strerror(-rc));
		return rc;
	}

	hidraw->fd = fd;
	hidraw->backend = handle;

	rc = parse_descriptor(device, hidraw, desc, desc_len);
	if (rc) {
		log_error(ratbag,
			  "Error while parsing the report descriptor: '%s' (%d)\n",
			  strerror(-rc), rc);
		ratbag_close_hidraw_index(device, idx);
		return rc;
	}

	hidraw->sysname = strdup_safe(node->path);
	log_debug(ratbag, "%s is device '%s'.\n", device->name, node->path);

	return 0;
}

static int
ratbag_find_hidraw_node(struct ratbag_device *device,
			int (*match)(struct ratbag_device *device),
			int use_usb_parent,
			int match_index, int hidraw_index)
{
	struct ratbag *ratbag = device->ratbag;
	struct ratbag_hid_node *nodes = NULL;
	int n, rc = -ENODEV, matched, endpoint_index = 0;

	assert(match);

	if (!device->hid_node)
		return -ENODEV;

	n = enumerate_sorted(ratbag, &nodes);
	if (n < 0)
		return n;

	for (int i = 0; i < n; i++) {
		const struct ratbag_hid_node *node = &nodes[i];

		if (use_usb_parent) {
			if (!streq(node->group, device->hid_node->group))
				continue;
		} else {
			if (!streq(node->path, device->hid_node->path))
				continue;
		}

		if (match_index > 0 && match_index != endpoint_index++)
			continue;

		rc = ratbag_open_hid_node(device, node, hidraw_index);
		if (rc)
			continue;

		matched = match(device);
		rc = matched ? 0 : -ENODEV;
		if (matched == 1)
			goto out;

		ratbag_close_hidraw_index(device, hidraw_index);
	}

out:
	ratbag_hid_nodes_free(nodes, n);
	return rc;
}

int
ratbag_find_hidraw(struct ratbag_device *device, int (*match)(struct ratbag_device *device))
{
	return ratbag_find_hidraw_node(device, match, true, -1, 0);
}

static int
hidraw_match_all(struct ratbag_device *device)
{
	return 1;
}

int
ratbag_open_hidraw(struct ratbag_device *device)
{
	return ratbag_find_hidraw_node(device, hidraw_match_all, false, -1, 0);
}

int
ratbag_open_hidraw_index(struct ratbag_device *device, int endpoint_index, int hidraw_index)
{
	return ratbag_find_hidraw_node(device, hidraw_match_all, true, endpoint_index, hidraw_index);
}

void
ratbag_close_hidraw_index(struct ratbag_device *device, int idx)
{
	struct ratbag_hidraw *hidraw;

	assert(idx >= 0 && idx < MAX_HIDRAW);
	hidraw = &device->hidraw[idx];

	if (hidraw->backend) {
		device->ratbag->hid_backend->close(device->ratbag, hidraw->backend);
		hidraw->backend = NULL;
	} else if (hidraw->fd >= 0) {
		close(hidraw->fd);
	}
	hidraw->fd = -1;

	free(hidraw->sysname);
	hidraw->sysname = NULL;
	free(hidraw->reports);
	hidraw->reports = NULL;
	hidraw->num_reports = 0;
}

int
ratbag_hidraw_raw_request(struct ratbag_device *device, unsigned char reportnum,
			  uint8_t *buf, size_t len, unsigned char rtype, int reqtype)
{
	const struct ratbag_hid_backend *backend = device->ratbag->hid_backend;
	void *handle = device->hidraw[0].backend;
	uint8_t tmp_buf[HID_MAX_BUFFER_SIZE];
	int rc;

	if (len < 1 || len > HID_MAX_BUFFER_SIZE || !buf || !handle)
		return -EINVAL;

	if (rtype != HID_FEATURE_REPORT)
		return -ENOTSUP;

	switch (reqtype) {
	case HID_REQ_GET_REPORT:
		memset(tmp_buf, 0, len);
		tmp_buf[0] = reportnum;

		rc = backend->get_feature(handle, tmp_buf, len);
		if (rc < 0)
			return rc;

		log_buf_raw(device->ratbag, "feature get:   ", tmp_buf, (unsigned)rc);
		memcpy(buf, tmp_buf, rc);
		return rc;
	case HID_REQ_SET_REPORT:
		buf[0] = reportnum;
		log_buf_raw(device->ratbag, "feature set:   ", buf, len);
		return backend->set_feature(handle, buf, len);
	}

	return -EINVAL;
}

/* ------------------------------------------------------------------------ */
/* talking to a receiver                                                    */
/* ------------------------------------------------------------------------ */

struct paired_device {
	int index;
	int ncandidates;
	uint16_t candidates[4];		/* product IDs to look up */
};

static int64_t
now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Wait up to timeout_ms for a report accepted by match(), copy it to buf */
static int
hidpp_wait_for(int fd, uint8_t *buf, size_t len, int timeout_ms,
	       bool (*match)(const uint8_t *buf, ssize_t len, const uint8_t *req),
	       const uint8_t *req)
{
	int64_t end = now_ms() + timeout_ms;

	while (true) {
		int64_t left = end - now_ms();
		struct pollfd pfd = { .fd = fd, .events = POLLIN };
		ssize_t rc;

		if (left <= 0)
			return -ETIMEDOUT;
		rc = poll(&pfd, 1, (int)left);
		if (rc < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		if (rc == 0)
			return -ETIMEDOUT;

		rc = read(fd, buf, len);
		if (rc < 0) {
			if (errno == EINTR || errno == EAGAIN)
				continue;
			return -errno;
		}
		if (match(buf, rc, req))
			return (int)rc;
	}
}

static bool
match_register_reply(const uint8_t *buf, ssize_t len, const uint8_t *req)
{
	if (len < 5)
		return false;
	if (buf[0] != HIDPP_SHORT && buf[0] != HIDPP_LONG)
		return false;
	if (buf[1] != req[1])
		return false;
	/* answer: same sub_id/address/subregister */
	if (buf[2] == req[2] && buf[3] == req[3] && buf[4] == req[4])
		return true;
	/* HID++ 1.0 error: 0x8f, sub_id, address */
	if (buf[2] == 0x8f && buf[3] == req[2] && buf[4] == req[3])
		return true;
	return false;
}

static bool
match_feature_reply(const uint8_t *buf, ssize_t len, const uint8_t *req)
{
	if (len < 5)
		return false;
	if (buf[0] != HIDPP_SHORT && buf[0] != HIDPP_LONG)
		return false;
	if (buf[1] != req[1])
		return false;
	if (buf[2] == req[2] && buf[3] == req[3])
		return true;
	/* HID++ 2.0 error: 0xff, feature index, address */
	if (buf[2] == 0xff && buf[3] == req[2] && buf[4] == req[3])
		return true;
	/* HID++ 1.0 error from the receiver, e.g. device not connected */
	if (buf[2] == 0x8f && buf[3] == req[2] && buf[4] == req[3])
		return true;
	return false;
}

static int
hidpp_send(int fd, const uint8_t *buf, size_t len)
{
	ssize_t rc;

	do {
		rc = write(fd, buf, len);
	} while (rc < 0 && errno == EINTR);

	return rc < 0 ? -errno : 0;
}

/* Read the receiver's pairing information register for one slot */
static int
receiver_get_wpid(struct ratbag *ratbag, int fd, bool has_short, int slot,
		  uint16_t *wpid)
{
	uint8_t req[HIDPP_LONG_LEN] = {0};
	uint8_t reply[64];
	size_t len = has_short ? HIDPP_SHORT_LEN : HIDPP_LONG_LEN;
	int rc;

	req[0] = has_short ? HIDPP_SHORT : HIDPP_LONG;
	req[1] = 0xff;			/* the receiver itself */
	req[2] = 0x83;			/* get long register */
	req[3] = 0xb5;			/* pairing information */
	req[4] = 0x20 + slot - 1;	/* device pairing information, slot */

	rc = hidpp_send(fd, req, len);
	if (rc)
		return rc;

	rc = hidpp_wait_for(fd, reply, sizeof(reply), 500, match_register_reply, req);
	if (rc < 0)
		return rc;
	if (reply[2] == 0x8f)
		return -ENOENT;
	if (rc < 9)
		return -EPROTO;

	*wpid = (uint16_t)(reply[7] << 8 | reply[8]);
	return *wpid ? 0 : -ENOENT;
}

/* HID++ 2.0 request to a paired device; returns the reply length */
static int
device_feature_request(int fd, int slot, uint8_t feature_index, uint8_t function,
		       const uint8_t *params, size_t nparams, uint8_t *reply, size_t reply_len)
{
	const uint8_t sw_id = 0x0a;
	uint8_t req[HIDPP_LONG_LEN] = {0};
	int rc;

	req[0] = HIDPP_LONG;
	req[1] = slot;
	req[2] = feature_index;
	req[3] = (function << 4) | sw_id;
	memcpy(&req[4], params, min(nparams, sizeof(req) - 4));

	rc = hidpp_send(fd, req, sizeof(req));
	if (rc)
		return rc;

	/* a sleeping wireless device can take a moment to answer */
	rc = hidpp_wait_for(fd, reply, reply_len, 1500, match_feature_reply, req);
	if (rc < 0)
		return rc;
	if (reply[2] == 0xff || reply[2] == 0x8f)
		return -EIO;
	return rc;
}

/* Fallback for receivers without the pairing register: ask the device for
 * its model IDs via HID++ 2.0 feature 0x0003 (device information) */
static int
device_get_model_ids(int fd, int slot, struct paired_device *paired)
{
	const uint8_t feature_0003[] = { 0x00, 0x03 };
	uint8_t reply[64];
	uint8_t index;
	int rc;

	rc = device_feature_request(fd, slot, 0x00, 0x00, feature_0003,
				    sizeof(feature_0003), reply, sizeof(reply));
	if (rc < 0)
		return rc;
	index = reply[4];
	if (index == 0)
		return -ENOTSUP;

	rc = device_feature_request(fd, slot, index, 0x00, NULL, 0, reply, sizeof(reply));
	if (rc < 17)
		return rc < 0 ? rc : -EPROTO;

	/* params: entity count, unit id[4], transport[2], model id[6] */
	for (int i = 0; i < 3; i++) {
		uint16_t pid = (uint16_t)(reply[11 + i * 2] << 8 | reply[12 + i * 2]);

		if (pid)
			paired->candidates[paired->ncandidates++] = pid;
	}

	return paired->ncandidates ? 0 : -ENOENT;
}

static bool
node_has_hidpp_long(struct ratbag *ratbag, const uint8_t *desc, size_t desc_len,
		    bool *has_short)
{
	struct ratbag_device dummy = { .ratbag = ratbag };
	struct ratbag_hidraw hidraw = {0};
	bool has_long = false;

	*has_short = false;
	if (parse_descriptor(&dummy, &hidraw, desc, desc_len) == 0) {
		for (unsigned i = 0; i < hidraw.num_reports; i++) {
			if (hidraw.reports[i].usage_page < 0xff00)
				continue;
			if (hidraw.reports[i].report_id == HIDPP_LONG)
				has_long = true;
			if (hidraw.reports[i].report_id == HIDPP_SHORT)
				*has_short = true;
		}
	}
	free(hidraw.reports);

	return has_long;
}

/* Fills paired[] with the devices paired to the receiver at node and
 * returns how many there are */
static int
receiver_list_paired(struct ratbag *ratbag, const struct ratbag_hid_node *node,
		     struct paired_device *paired)
{
	const struct ratbag_hid_backend *backend = ratbag->hid_backend;
	_cleanup_free_ uint8_t *desc = NULL;
	size_t desc_len = 0;
	void *handle = NULL;
	bool has_short;
	int fd, rc, n = 0;

	rc = backend->open(ratbag, node, &fd, &desc, &desc_len, &handle);
	if (rc == -EACCES)
		ratbag->hid_access_denied = true;
	if (rc != 0)
		return 0;

	if (!node_has_hidpp_long(ratbag, desc, desc_len, &has_short))
		goto out;

	log_debug(ratbag, "%s: looking for paired devices on %s\n",
		  node->name, node->path);

	for (int slot = 1; slot <= HIDPP_MAX_PAIRED; slot++) {
		struct paired_device *p = &paired[n];
		uint16_t wpid;

		memset(p, 0, sizeof(*p));
		p->index = slot;

		if (receiver_get_wpid(ratbag, fd, has_short, slot, &wpid) == 0) {
			p->candidates[p->ncandidates++] = wpid;
			log_debug(ratbag, "  slot %d: paired device %04x\n", slot, wpid);
		} else if (device_get_model_ids(fd, slot, p) == 0) {
			log_debug(ratbag, "  slot %d: device reports model %04x\n",
				  slot, p->candidates[0]);
		} else {
			continue;
		}
		n++;
	}

out:
	backend->close(ratbag, handle);
	return n;
}

/* ------------------------------------------------------------------------ */
/* public API                                                               */
/* ------------------------------------------------------------------------ */

static int
default_open_restricted(const char *path, int flags, void *user_data)
{
	int fd = open(path, flags | O_CLOEXEC);

	return fd < 0 ? -errno : fd;
}

static void
default_close_restricted(int fd, void *user_data)
{
	close(fd);
}

static const struct ratbag_interface default_interface = {
	.open_restricted = default_open_restricted,
	.close_restricted = default_close_restricted,
};

LIBRATBAG_EXPORT struct ratbag *
ratbag_create_context_default(void)
{
	return ratbag_create_context(&default_interface, NULL);
}

LIBRATBAG_EXPORT void
ratbag_free_string(char *str)
{
	free(str);
}

LIBRATBAG_EXPORT char *
ratbag_hid_list_groups(struct ratbag *ratbag)
{
	struct ratbag_hid_node *nodes = NULL;
	char *out = NULL;
	size_t size = 0;
	FILE *f;
	int n;

	n = enumerate_sorted(ratbag, &nodes);
	if (n < 0)
		return NULL;

	f = open_memstream(&out, &size);
	if (!f) {
		ratbag_hid_nodes_free(nodes, n);
		return NULL;
	}

	for (int i = 0; i < n; i++) {
		if (i > 0 && streq(nodes[i].group, nodes[i - 1].group))
			continue;
		fprintf(f, "%s\t%04x\t%04x\t%04x\t%s\n",
			nodes[i].group, nodes[i].bustype, nodes[i].vendor,
			nodes[i].product, nodes[i].name ? nodes[i].name : "");
	}
	fclose(f);
	ratbag_hid_nodes_free(nodes, n);

	return out;
}

static struct ratbag_device *
try_device(struct ratbag *ratbag, const struct ratbag_hid_node *node,
	   const struct input_id *id, int hidpp_index, const char *name)
{
	struct ratbag_device *device;

	device = ratbag_device_new(ratbag, (ratbag_os_device *)node, name, id);
	if (!device)
		return NULL;

	device->hidpp_index = hidpp_index;

	if (!device->data || !ratbag_assign_driver(device, &device->ids, NULL)) {
		ratbag_device_destroy(device);
		return NULL;
	}

	return device;
}

static bool
have_data_for(struct ratbag *ratbag, const struct input_id *id)
{
	struct ratbag_device_data *data = ratbag_device_data_new_for_id(ratbag, id);
	bool found = data != NULL;

	ratbag_device_data_unref(data);
	return found;
}

LIBRATBAG_EXPORT int
ratbag_devices_new_from_group(struct ratbag *ratbag,
			      const char *group,
			      struct ratbag_device **devices,
			      int max)
{
	struct ratbag_hid_node *nodes = NULL;
	int n, first = -1, last = -1, count = 0;
	struct input_id id = {0};

	n = enumerate_sorted(ratbag, &nodes);
	if (n <= 0)
		return 0;

	ratbag->hid_access_denied = false;

	for (int i = 0; i < n; i++) {
		if (!streq(nodes[i].group, group))
			continue;
		if (first < 0)
			first = i;
		last = i;
	}
	if (first < 0)
		goto out;

	id.bustype = nodes[first].bustype;
	id.vendor = nodes[first].vendor;
	id.product = nodes[first].product;

	if (have_data_for(ratbag, &id)) {
		/* A directly connected device. Like ratbagd on Linux, offer
		 * each interface in turn until the driver accepts one. */
		for (int i = first; i <= last && count < max; i++) {
			struct ratbag_device *d;

			d = try_device(ratbag, &nodes[i], &id, -1, nodes[i].name);
			if (d) {
				devices[count++] = d;
				break;
			}
		}
		goto out;
	}

	/* Not a known device, but it may be a receiver with known devices
	 * paired to it. Only Logitech receivers are handled for now; like
	 * libratbag-data.c, treat every 046d:c5xx as one. */
	if (id.vendor != LOGITECH_VENDOR_ID || (id.product & 0xff00) != 0xc500)
		goto out;

	for (int i = first; i <= last && count < max; i++) {
		struct paired_device paired[HIDPP_MAX_PAIRED];
		int npaired = receiver_list_paired(ratbag, &nodes[i], paired);

		for (int p = 0; p < npaired && count < max; p++) {
			for (int c = 0; c < paired[p].ncandidates; c++) {
				struct input_id wid = id;
				struct ratbag_device_data *data;
				struct ratbag_device *d;
				_cleanup_free_ char *name = NULL;

				wid.product = paired[p].candidates[c];
				data = ratbag_device_data_new_for_id(ratbag, &wid);
				if (!data)
					continue;
				name = strdup_safe(ratbag_device_data_get_name(data));
				ratbag_device_data_unref(data);

				log_debug(ratbag, "%s: trying %s in slot %d\n",
					  nodes[i].name, name, paired[p].index);
				d = try_device(ratbag, &nodes[i], &wid,
					       paired[p].index, name);
				if (d) {
					devices[count++] = d;
					break;
				}
			}
		}

		/* All paired devices hang off one interface */
		if (npaired > 0)
			break;
	}

out:
	if (count == 0 && ratbag->hid_access_denied && first >= 0)
		log_error(ratbag,
			  "%s: macOS denied access to some of its interfaces. If this device should\n"
			  "be supported, allow this app (or your terminal) in System Settings >\n"
			  "Privacy & Security > Input Monitoring, then restart it.\n",
			  nodes[first].name);
	ratbag_hid_nodes_free(nodes, n);
	return count;
}

LIBRATBAG_EXPORT const char *
ratbag_device_get_group(const struct ratbag_device *device)
{
	return device->hid_node ? device->hid_node->group : NULL;
}

LIBRATBAG_EXPORT int
ratbag_device_get_receiver_index(const struct ratbag_device *device)
{
	return device->hidpp_index;
}
