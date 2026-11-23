/*
 * Minimal stand-in for <linux/input.h> used by the portable (non-Linux)
 * libratbag build. libratbag only needs the key/button codes, the bus types
 * and struct input_id from this header; nothing here talks to the kernel.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdint.h>
#include <linux/input-event-codes.h>

struct input_id {
	uint16_t bustype;
	uint16_t vendor;
	uint16_t product;
	uint16_t version;
};

#ifndef BUS_USB
#define BUS_PCI			0x01
#define BUS_ISAPNP		0x02
#define BUS_USB			0x03
#define BUS_HIL			0x04
#define BUS_BLUETOOTH		0x05
#define BUS_VIRTUAL		0x06
#endif
