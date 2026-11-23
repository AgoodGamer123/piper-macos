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
 * HID backend that talks to a device emulator over a unix socket, selected
 * by setting RATBAG_EMU_SOCKET to the socket's path. See
 * tools/hidpp-emu.py for the emulator and the protocol:
 *
 *  - "LIST\n" answers one line per HID node and closes:
 *    path TAB group TAB bustype TAB vendor TAB product TAB interface TAB
 *    name TAB report-descriptor, numbers and descriptor in hex
 *  - "OPEN <path>\n" turns the connection into a report stream of frames
 *    [type, len_lo, len_hi, payload...] where type is 'O' (output report,
 *    to the device), 'I' (input report, from the device), 'G'/'g' (get
 *    feature request/reply) and 'S'/'s' (set feature request/reply). A
 *    feature reply starts with the result as int32 little endian (byte
 *    count or negative errno), followed by the report for 'g'.
 */

#include "config.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "libratbag-private.h"
#include "libratbag-hid.h"

#define EMU_MAX_REPORT 4096

struct emu_handle {
	int sock;
	struct ratbag_hid_pump *pump;
	pthread_t reader;
	bool reader_started;

	pthread_mutex_t lock;		/* serialises writes to sock */
	pthread_mutex_t reply_lock;
	pthread_cond_t reply_cond;
	bool reply_ready;
	uint8_t reply_type;
	uint8_t reply[EMU_MAX_REPORT];
	size_t reply_len;
	bool dead;
};

static int
emu_connect(void)
{
	const char *path = getenv("RATBAG_EMU_SOCKET");
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	int fd;

	if (!path || strlen(path) >= sizeof(addr.sun_path))
		return -EINVAL;

	strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
		return -errno;
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		int rc = -errno;
		close(fd);
		return rc;
	}
	return fd;
}

static int
write_all(int fd, const void *data, size_t len)
{
	const uint8_t *p = data;

	while (len > 0) {
		ssize_t rc = send(fd, p, len, 0
#ifdef MSG_NOSIGNAL
				  | MSG_NOSIGNAL
#endif
				  );
		if (rc < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		p += rc;
		len -= rc;
	}
	return 0;
}

static int
read_all(int fd, void *data, size_t len)
{
	uint8_t *p = data;

	while (len > 0) {
		ssize_t rc = recv(fd, p, len, 0);
		if (rc < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		if (rc == 0)
			return -ECONNRESET;
		p += rc;
		len -= rc;
	}
	return 0;
}

static int
hex_decode(const char *hex, uint8_t **out, size_t *out_len)
{
	size_t n = strlen(hex) / 2;
	uint8_t *buf = zalloc(max(n, 1U));

	for (size_t i = 0; i < n; i++) {
		unsigned int byte;

		if (sscanf(&hex[i * 2], "%2x", &byte) != 1) {
			free(buf);
			return -EINVAL;
		}
		buf[i] = byte;
	}
	*out = buf;
	*out_len = n;
	return 0;
}

/* Reads the whole LIST answer */
static char *
emu_list(void)
{
	char *out = NULL;
	size_t size = 0, used = 0;
	int fd = emu_connect();

	if (fd < 0)
		return NULL;

	if (write_all(fd, "LIST\n", 5) < 0)
		goto out;

	while (true) {
		ssize_t rc;

		if (size - used < 4096) {
			size += 8192;
			out = realloc(out, size);
		}
		rc = recv(fd, out + used, size - used - 1, 0);
		if (rc < 0 && errno == EINTR)
			continue;
		if (rc <= 0)
			break;
		used += rc;
	}
	if (out)
		out[used] = '\0';
out:
	close(fd);
	return out;
}

/* Parses one LIST line into node, and the descriptor if desc is not NULL */
static bool
parse_line(char *line, struct ratbag_hid_node *node, uint8_t **desc, size_t *desc_len)
{
	char *fields[8];
	unsigned int bus, vid, pid;
	int iface, n = 0;
	char *saveptr = NULL;

	for (char *tok = strtok_r(line, "\t", &saveptr);
	     tok && n < 8;
	     tok = strtok_r(NULL, "\t", &saveptr))
		fields[n++] = tok;
	if (n != 8)
		return false;

	if (sscanf(fields[2], "%x", &bus) != 1 ||
	    sscanf(fields[3], "%x", &vid) != 1 ||
	    sscanf(fields[4], "%x", &pid) != 1 ||
	    sscanf(fields[5], "%d", &iface) != 1)
		return false;

	memset(node, 0, sizeof(*node));
	node->path = strdup_safe(fields[0]);
	node->group = strdup_safe(fields[1]);
	node->bustype = bus;
	node->vendor = vid;
	node->product = pid;
	node->interface_number = iface;
	node->name = strdup_safe(fields[6]);

	if (desc && hex_decode(fields[7], desc, desc_len) < 0) {
		ratbag_hid_node_clear(node);
		return false;
	}

	return true;
}

static int
emu_enumerate(struct ratbag *ratbag, struct ratbag_hid_node **nodes_out)
{
	_cleanup_free_ char *list = emu_list();
	struct ratbag_hid_node *nodes = NULL;
	char *saveptr = NULL;
	int n = 0;

	if (!list) {
		log_error(ratbag, "emu: cannot reach the emulator at %s\n",
			  getenv("RATBAG_EMU_SOCKET"));
		*nodes_out = NULL;
		return 0;
	}

	for (char *line = strtok_r(list, "\n", &saveptr);
	     line;
	     line = strtok_r(NULL, "\n", &saveptr)) {
		struct ratbag_hid_node node;

		if (!parse_line(line, &node, NULL, NULL))
			continue;
		nodes = realloc(nodes, (n + 1) * sizeof(*nodes));
		nodes[n++] = node;
	}

	*nodes_out = nodes;
	return n;
}

static int
emu_send_frame(struct emu_handle *h, uint8_t type, const uint8_t *buf, size_t len)
{
	uint8_t hdr[3] = { type, len & 0xff, (len >> 8) & 0xff };
	int rc;

	pthread_mutex_lock(&h->lock);
	rc = write_all(h->sock, hdr, sizeof(hdr));
	if (rc == 0)
		rc = write_all(h->sock, buf, len);
	pthread_mutex_unlock(&h->lock);

	return rc;
}

static int
emu_output(void *userdata, const uint8_t *buf, size_t len)
{
	return emu_send_frame(userdata, 'O', buf, len);
}

static void *
emu_reader_thread(void *data)
{
	struct emu_handle *h = data;
	uint8_t buf[EMU_MAX_REPORT];

	while (true) {
		uint8_t hdr[3];
		size_t len;

		if (read_all(h->sock, hdr, sizeof(hdr)) < 0)
			break;
		len = hdr[1] | (hdr[2] << 8);
		if (len > sizeof(buf) || read_all(h->sock, buf, len) < 0)
			break;

		if (hdr[0] == 'I') {
			ratbag_hid_pump_push_input(h->pump, buf, len);
		} else if (hdr[0] == 'g' || hdr[0] == 's') {
			pthread_mutex_lock(&h->reply_lock);
			memcpy(h->reply, buf, len);
			h->reply_len = len;
			h->reply_type = hdr[0];
			h->reply_ready = true;
			pthread_cond_signal(&h->reply_cond);
			pthread_mutex_unlock(&h->reply_lock);
		}
	}

	pthread_mutex_lock(&h->reply_lock);
	h->dead = true;
	pthread_cond_broadcast(&h->reply_cond);
	pthread_mutex_unlock(&h->reply_lock);

	return NULL;
}

static void
emu_close(struct ratbag *ratbag, void *handle)
{
	struct emu_handle *h = handle;

	if (!h)
		return;

	if (h->sock >= 0)
		shutdown(h->sock, SHUT_RDWR);
	if (h->reader_started)
		pthread_join(h->reader, NULL);
	ratbag_hid_pump_destroy(h->pump);
	if (h->sock >= 0)
		close(h->sock);
	pthread_mutex_destroy(&h->lock);
	pthread_mutex_destroy(&h->reply_lock);
	pthread_cond_destroy(&h->reply_cond);
	free(h);
}

static int
emu_open(struct ratbag *ratbag, const struct ratbag_hid_node *node,
	 int *fd, uint8_t **desc, size_t *desc_len, void **handle)
{
	_cleanup_free_ char *list = emu_list();
	_cleanup_free_ char *cmd = NULL;
	struct emu_handle *h;
	char *saveptr = NULL;
	bool found = false;
	int rc;

	if (!list)
		return -ENODEV;

	for (char *line = strtok_r(list, "\n", &saveptr);
	     line && !found;
	     line = strtok_r(NULL, "\n", &saveptr)) {
		struct ratbag_hid_node n;
		uint8_t *d = NULL;
		size_t dl = 0;

		if (!parse_line(line, &n, &d, &dl))
			continue;
		if (streq(n.path, node->path)) {
			*desc = d;
			*desc_len = dl;
			found = true;
		} else {
			free(d);
		}
		ratbag_hid_node_clear(&n);
	}
	if (!found)
		return -ENODEV;

	h = zalloc(sizeof(*h));
	h->sock = -1;
	pthread_mutex_init(&h->lock, NULL);
	pthread_mutex_init(&h->reply_lock, NULL);
	pthread_cond_init(&h->reply_cond, NULL);

	rc = emu_connect();
	if (rc < 0)
		goto err;
	h->sock = rc;

	if (asprintf(&cmd, "OPEN %s\n", node->path) < 0) {
		rc = -ENOMEM;
		goto err;
	}
	rc = write_all(h->sock, cmd, strlen(cmd));
	if (rc < 0)
		goto err;

	h->pump = ratbag_hid_pump_new(emu_output, h);
	if (!h->pump) {
		rc = -errno ? -errno : -ENOMEM;
		goto err;
	}

	if (pthread_create(&h->reader, NULL, emu_reader_thread, h) != 0) {
		rc = -EAGAIN;
		goto err;
	}
	h->reader_started = true;

	*fd = ratbag_hid_pump_fd(h->pump);
	*handle = h;
	return 0;

err:
	emu_close(ratbag, h);
	free(*desc);
	*desc = NULL;
	return rc;
}

static int
emu_feature(struct emu_handle *h, uint8_t type, uint8_t reply_type,
	    uint8_t *buf, size_t len)
{
	int rc;

	pthread_mutex_lock(&h->reply_lock);
	h->reply_ready = false;
	pthread_mutex_unlock(&h->reply_lock);

	rc = emu_send_frame(h, type, buf, len);
	if (rc < 0)
		return rc;

	pthread_mutex_lock(&h->reply_lock);
	while (!h->reply_ready && !h->dead)
		pthread_cond_wait(&h->reply_cond, &h->reply_lock);
	if (h->reply_ready && h->reply_type == reply_type && h->reply_len >= 4) {
		/* reply: int32 LE result, then the report for 'g' */
		int32_t v = (int32_t)(h->reply[0] | h->reply[1] << 8 |
				      h->reply[2] << 16 | (uint32_t)h->reply[3] << 24);

		rc = v;
		if (type == 'G' && v > 0) {
			rc = (int)min(min((size_t)v, h->reply_len - 4), len);
			memcpy(buf, &h->reply[4], rc);
		}
	} else {
		rc = -EIO;
	}
	pthread_mutex_unlock(&h->reply_lock);

	return rc;
}

static int
emu_get_feature(void *handle, uint8_t *buf, size_t len)
{
	return emu_feature(handle, 'G', 'g', buf, len);
}

static int
emu_set_feature(void *handle, const uint8_t *buf, size_t len)
{
	uint8_t tmp[EMU_MAX_REPORT];

	len = min(len, sizeof(tmp));
	memcpy(tmp, buf, len);
	return emu_feature(handle, 'S', 's', tmp, len);
}

const struct ratbag_hid_backend ratbag_hid_backend_emu = {
	.name = "emu",
	.enumerate = emu_enumerate,
	.open = emu_open,
	.close = emu_close,
	.get_feature = emu_get_feature,
	.set_feature = emu_set_feature,
};
