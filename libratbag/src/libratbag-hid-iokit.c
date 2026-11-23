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
 * macOS HID backend built on IOKit's IOHIDDevice API.
 *
 * Each IOHIDDevice is one HID interface with its full report descriptor,
 * which is what a hidraw node is on Linux. Devices are identified by their
 * IORegistry entry ID so they can be re-opened later; interfaces of the
 * same physical device share a LocationID, which becomes the node group.
 *
 * Devices are opened non-exclusively so the mouse keeps working while we
 * talk to it. Input reports arrive on a per-device thread running a
 * CFRunLoop and are forwarded into the socketpair pump; output reports
 * written by libratbag are sent with IOHIDDeviceSetReport from the pump's
 * thread.
 *
 * Since macOS 10.15, opening an interface with keyboard or pointer usages
 * needs the "Input Monitoring" privacy permission. Logitech's HID++
 * interface is vendor-defined and doesn't, so we don't ask for it up front.
 */

#include "config.h"

#ifdef __APPLE__

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/hid/IOHIDDevice.h>
#include <IOKit/hid/IOHIDKeys.h>
#include <IOKit/hid/IOHIDManager.h>

#include "libratbag-private.h"
#include "libratbag-hid.h"

#ifndef BUS_USB
#define BUS_USB 0x03
#endif
#ifndef BUS_BLUETOOTH
#define BUS_BLUETOOTH 0x05
#endif

struct iokit_handle {
	IOHIDDeviceRef device;
	struct ratbag_hid_pump *pump;

	uint8_t *input_buf;
	CFIndex input_buf_len;

	pthread_t thread;
	bool thread_started;
	CFRunLoopRef run_loop;
	volatile bool stop;
	volatile bool removed;

	pthread_mutex_t lock;
	pthread_cond_t cond;
	bool running;
};

/* ------------------------------------------------------------------------ */
/* properties                                                               */
/* ------------------------------------------------------------------------ */

static bool
cf_number_value(CFTypeRef ref, int64_t *value)
{
	if (!ref || CFGetTypeID(ref) != CFNumberGetTypeID())
		return false;
	return CFNumberGetValue((CFNumberRef)ref, kCFNumberSInt64Type, value);
}

static int64_t
get_int_property(IOHIDDeviceRef device, CFStringRef key, int64_t fallback)
{
	int64_t value;

	if (cf_number_value(IOHIDDeviceGetProperty(device, key), &value))
		return value;
	return fallback;
}

static char *
cf_string_dup(CFTypeRef ref)
{
	CFIndex len;
	char *buf;

	if (!ref || CFGetTypeID(ref) != CFStringGetTypeID())
		return NULL;

	len = CFStringGetMaximumSizeForEncoding(CFStringGetLength((CFStringRef)ref),
						kCFStringEncodingUTF8) + 1;
	buf = zalloc(len);
	if (!CFStringGetCString((CFStringRef)ref, buf, len, kCFStringEncodingUTF8)) {
		free(buf);
		return NULL;
	}
	return buf;
}

static char *
get_string_property(IOHIDDeviceRef device, CFStringRef key)
{
	return cf_string_dup(IOHIDDeviceGetProperty(device, key));
}

static uint16_t
get_bustype(IOHIDDeviceRef device)
{
	_cleanup_free_ char *transport = get_string_property(device, CFSTR(kIOHIDTransportKey));

	if (!transport)
		return 0;
	if (streq(transport, "USB"))
		return BUS_USB;
	if (strneq(transport, "Bluetooth", 9))
		return BUS_BLUETOOTH;
	return 0;
}

/* The USB interface number lives on a parent of the IOHIDDevice */
static int
get_interface_number(io_service_t service)
{
	CFTypeRef ref;
	int64_t value = -1;

	ref = IORegistryEntrySearchCFProperty(service, kIOServicePlane,
					      CFSTR("bInterfaceNumber"),
					      kCFAllocatorDefault,
					      kIORegistryIterateRecursively |
					      kIORegistryIterateParents);
	if (ref) {
		cf_number_value(ref, &value);
		CFRelease(ref);
	}
	return (int)value;
}

/* ------------------------------------------------------------------------ */
/* enumeration                                                              */
/* ------------------------------------------------------------------------ */

static int
iokit_enumerate(struct ratbag *ratbag, struct ratbag_hid_node **nodes_out)
{
	IOHIDManagerRef manager;
	CFSetRef set;
	CFIndex count;
	const void **devices = NULL;
	struct ratbag_hid_node *nodes = NULL;
	int n = 0;

	*nodes_out = NULL;

	manager = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
	if (!manager)
		return -ENOMEM;

	IOHIDManagerSetDeviceMatching(manager, NULL);
	set = IOHIDManagerCopyDevices(manager);
	if (!set) {
		CFRelease(manager);
		return 0;
	}

	count = CFSetGetCount(set);
	devices = zalloc(max(count, 1) * sizeof(*devices));
	nodes = zalloc(max(count, 1) * sizeof(*nodes));
	CFSetGetValues(set, devices);

	for (CFIndex i = 0; i < count; i++) {
		IOHIDDeviceRef device = (IOHIDDeviceRef)devices[i];
		io_service_t service = IOHIDDeviceGetService(device);
		struct ratbag_hid_node *node = &nodes[n];
		uint64_t entry_id = 0;
		int64_t location;
		char *name;

		if (service == MACH_PORT_NULL ||
		    IORegistryEntryGetRegistryEntryID(service, &entry_id) != KERN_SUCCESS)
			continue;

		node->vendor = (uint16_t)get_int_property(device, CFSTR(kIOHIDVendorIDKey), 0);
		node->product = (uint16_t)get_int_property(device, CFSTR(kIOHIDProductIDKey), 0);
		node->bustype = get_bustype(device);
		node->interface_number = get_interface_number(service);

		/* All interfaces of one USB device share the LocationID. Fall
		 * back to the entry ID so unrelated devices never merge. */
		location = get_int_property(device, CFSTR(kIOHIDLocationIDKey), -1);
		if (location >= 0)
			xasprintf(&node->group, "loc-%08llx-%04x-%04x",
				  (unsigned long long)location,
				  node->vendor, node->product);
		else
			xasprintf(&node->group, "id-%llu", (unsigned long long)entry_id);

		xasprintf(&node->path, "IOService:%llu", (unsigned long long)entry_id);

		name = get_string_property(device, CFSTR(kIOHIDProductKey));
		if (!name)
			xasprintf(&name, "HID %04x:%04x", node->vendor, node->product);
		node->name = name;

		n++;
	}

	free(devices);
	CFRelease(set);
	CFRelease(manager);

	*nodes_out = nodes;
	return n;
}

/* ------------------------------------------------------------------------ */
/* I/O                                                                      */
/* ------------------------------------------------------------------------ */

static void
input_report_callback(void *context, IOReturn result, void *sender,
		      IOHIDReportType type, uint32_t report_id,
		      uint8_t *report, CFIndex report_length)
{
	struct iokit_handle *h = context;

	if (result != kIOReturnSuccess || report_length <= 0)
		return;

	/* For numbered reports the buffer starts with the report ID, the
	 * same layout hidraw uses */
	ratbag_hid_pump_push_input(h->pump, report, (size_t)report_length);
}

static void
removal_callback(void *context, IOReturn result, void *sender)
{
	struct iokit_handle *h = context;

	h->removed = true;
}

static void *
run_loop_thread(void *data)
{
	struct iokit_handle *h = data;

	pthread_mutex_lock(&h->lock);
	h->run_loop = CFRunLoopGetCurrent();
	IOHIDDeviceScheduleWithRunLoop(h->device, h->run_loop, kCFRunLoopDefaultMode);
	IOHIDDeviceRegisterInputReportCallback(h->device, h->input_buf,
					       h->input_buf_len,
					       input_report_callback, h);
	IOHIDDeviceRegisterRemovalCallback(h->device, removal_callback, h);
	h->running = true;
	pthread_cond_signal(&h->cond);
	pthread_mutex_unlock(&h->lock);

	/* Short slices so a stop request is noticed even if CFRunLoopStop()
	 * raced with us entering the loop */
	while (!h->stop)
		CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.25, false);

	IOHIDDeviceRegisterInputReportCallback(h->device, h->input_buf,
					       h->input_buf_len, NULL, NULL);
	IOHIDDeviceRegisterRemovalCallback(h->device, NULL, NULL);
	IOHIDDeviceUnscheduleFromRunLoop(h->device, h->run_loop, kCFRunLoopDefaultMode);

	return NULL;
}

static int
ioreturn_to_errno(IOReturn ret)
{
	switch (ret) {
	case kIOReturnSuccess:
		return 0;
	case kIOReturnNotPermitted:
	case kIOReturnNotPrivileged:
		return -EACCES;
	case kIOReturnExclusiveAccess:
		return -EBUSY;
	case kIOReturnNoDevice:
	case kIOReturnNotAttached:
		return -ENODEV;
	case kIOReturnTimeout:
		return -ETIMEDOUT;
	case kIOReturnBadArgument:
		return -EINVAL;
	case kIOReturnUnsupported:
		return -ENOTSUP;
	default:
		return -EIO;
	}
}

static int
send_report(IOHIDDeviceRef device, IOHIDReportType type,
	    const uint8_t *buf, size_t len)
{
	uint8_t report_id;
	IOReturn ret;

	if (len < 1)
		return -EINVAL;

	/* Same convention as hidraw: buf[0] is the report ID, 0 for devices
	 * without numbered reports, in which case it is not sent */
	report_id = buf[0];
	if (report_id == 0) {
		buf++;
		len--;
	}

	ret = IOHIDDeviceSetReport(device, type, report_id, buf, (CFIndex)len);
	return ioreturn_to_errno(ret);
}

static int
iokit_output(void *userdata, const uint8_t *buf, size_t len)
{
	struct iokit_handle *h = userdata;

	if (h->removed)
		return -ENODEV;
	return send_report(h->device, kIOHIDReportTypeOutput, buf, len);
}

static void
iokit_close(struct ratbag *ratbag, void *handle)
{
	struct iokit_handle *h = handle;

	if (!h)
		return;

	if (h->thread_started) {
		h->stop = true;
		pthread_mutex_lock(&h->lock);
		if (h->run_loop)
			CFRunLoopStop(h->run_loop);
		pthread_mutex_unlock(&h->lock);
		pthread_join(h->thread, NULL);
	}

	/* stops the output thread, so no more IOHIDDeviceSetReport calls */
	ratbag_hid_pump_destroy(h->pump);

	if (h->device) {
		IOHIDDeviceClose(h->device, kIOHIDOptionsTypeNone);
		CFRelease(h->device);
	}

	pthread_mutex_destroy(&h->lock);
	pthread_cond_destroy(&h->cond);
	free(h->input_buf);
	free(h);
}

static int
iokit_open(struct ratbag *ratbag, const struct ratbag_hid_node *node,
	   int *fd, uint8_t **desc, size_t *desc_len, void **handle)
{
	unsigned long long entry_id;
	io_service_t service;
	struct iokit_handle *h;
	CFTypeRef ref;
	IOReturn ret;
	int rc;

	*desc = NULL;
	*desc_len = 0;

	if (sscanf(node->path, "IOService:%llu", &entry_id) != 1)
		return -EINVAL;

	/* MACH_PORT_NULL is the default main port on every macOS version */
	service = IOServiceGetMatchingService(MACH_PORT_NULL,
					      IORegistryEntryIDMatching(entry_id));
	if (service == MACH_PORT_NULL)
		return -ENODEV;

	h = zalloc(sizeof(*h));
	pthread_mutex_init(&h->lock, NULL);
	pthread_cond_init(&h->cond, NULL);

	h->device = IOHIDDeviceCreate(kCFAllocatorDefault, service);
	IOObjectRelease(service);
	if (!h->device) {
		rc = -ENODEV;
		goto err;
	}

	/* Not kIOHIDOptionsTypeSeizeDevice: the pointer must keep moving */
	ret = IOHIDDeviceOpen(h->device, kIOHIDOptionsTypeNone);
	if (ret != kIOReturnSuccess) {
		rc = ioreturn_to_errno(ret);
		/* Expected for the keyboard/pointer interfaces of a device
		 * without Input Monitoring permission; often we don't need
		 * those. ratbag_devices_new_from_group() complains if it
		 * mattered. */
		log_debug(ratbag, "IOHIDDeviceOpen(%s, %s) failed: 0x%08x\n",
			  node->path, node->name, ret);
		CFRelease(h->device);
		h->device = NULL;
		goto err;
	}

	ref = IOHIDDeviceGetProperty(h->device, CFSTR(kIOHIDReportDescriptorKey));
	if (!ref || CFGetTypeID(ref) != CFDataGetTypeID()) {
		rc = -EIO;
		goto err;
	}
	*desc_len = (size_t)CFDataGetLength((CFDataRef)ref);
	*desc = zalloc(max(*desc_len, 1U));
	memcpy(*desc, CFDataGetBytePtr((CFDataRef)ref), *desc_len);

	h->input_buf_len = (CFIndex)get_int_property(h->device,
						     CFSTR(kIOHIDMaxInputReportSizeKey),
						     64);
	if (h->input_buf_len < 64)
		h->input_buf_len = 64;
	h->input_buf = zalloc((size_t)h->input_buf_len);

	h->pump = ratbag_hid_pump_new(iokit_output, h);
	if (!h->pump) {
		rc = -ENOMEM;
		goto err;
	}

	if (pthread_create(&h->thread, NULL, run_loop_thread, h) != 0) {
		rc = -EAGAIN;
		goto err;
	}
	h->thread_started = true;

	/* don't hand out the fd before the input callback is registered or
	 * the first reply could get lost */
	pthread_mutex_lock(&h->lock);
	while (!h->running)
		pthread_cond_wait(&h->cond, &h->lock);
	pthread_mutex_unlock(&h->lock);

	*fd = ratbag_hid_pump_fd(h->pump);
	*handle = h;
	return 0;

err:
	iokit_close(ratbag, h);
	free(*desc);
	*desc = NULL;
	*desc_len = 0;
	return rc;
}

static int
iokit_get_feature(void *handle, uint8_t *buf, size_t len)
{
	struct iokit_handle *h = handle;
	uint8_t report_id = buf[0];
	uint8_t *report = buf;
	CFIndex report_len = (CFIndex)len;
	IOReturn ret;

	if (report_id == 0) {
		report++;
		report_len--;
	}

	ret = IOHIDDeviceGetReport(h->device, kIOHIDReportTypeFeature,
				   report_id, report, &report_len);
	if (ret != kIOReturnSuccess)
		return ioreturn_to_errno(ret);

	return (int)(report_id == 0 ? report_len + 1 : report_len);
}

static int
iokit_set_feature(void *handle, const uint8_t *buf, size_t len)
{
	struct iokit_handle *h = handle;
	int rc;

	rc = send_report(h->device, kIOHIDReportTypeFeature, buf, len);
	return rc < 0 ? rc : (int)len;
}

const struct ratbag_hid_backend ratbag_hid_backend_iokit = {
	.name = "iokit",
	.enumerate = iokit_enumerate,
	.open = iokit_open,
	.close = iokit_close,
	.get_feature = iokit_get_feature,
	.set_feature = iokit_set_feature,
};

#endif /* __APPLE__ */
