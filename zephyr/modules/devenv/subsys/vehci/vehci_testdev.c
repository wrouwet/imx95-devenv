/*
 * "Wimmo" virtual test device behind the virtual EHCI root port: a high-speed
 * USB device with only a control endpoint, answering the standard requests a
 * host needs to enumerate it. It stands in for the translated physical devices
 * that come later. VID:PID 1209:0001 is the pid.codes test ID.
 *
 * Copyright (c) 2026, imx95-devenv contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <string.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include <vehci/vehci.h>

LOG_MODULE_REGISTER(vehci_testdev, CONFIG_VEHCI_LOG_LEVEL);

#define REQ_GET_STATUS		0x00
#define REQ_CLEAR_FEATURE	0x01
#define REQ_SET_FEATURE		0x03
#define REQ_SET_ADDRESS		0x05
#define REQ_GET_DESCRIPTOR	0x06
#define REQ_GET_CONFIGURATION	0x08
#define REQ_SET_CONFIGURATION	0x09
#define REQ_GET_INTERFACE	0x0a
#define REQ_SET_INTERFACE	0x0b

static const uint8_t dev_desc[18] = {
	18, 0x01,		/* bLength, DEVICE */
	0x00, 0x02,		/* bcdUSB 2.00 */
	0x00, 0x00, 0x00,	/* class/subclass/protocol: per interface */
	64,			/* bMaxPacketSize0 */
	0x09, 0x12,		/* idVendor 0x1209 (pid.codes) */
	0x01, 0x00,		/* idProduct 0x0001 (test) */
	0x00, 0x01,		/* bcdDevice 1.00 */
	1, 2, 3,		/* iManufacturer, iProduct, iSerialNumber */
	1,			/* bNumConfigurations */
};

static const uint8_t cfg_desc[18] = {
	9, 0x02, 18, 0,		/* CONFIGURATION, wTotalLength 18 */
	1, 1, 0,		/* bNumInterfaces, bConfigurationValue, iConfiguration */
	0x80, 50,		/* bus powered, 100 mA */
	9, 0x04, 0, 0,		/* INTERFACE 0, alt 0 */
	0,			/* bNumEndpoints: control only */
	0xff, 0x00, 0x00,	/* vendor specific */
	4,			/* iInterface */
};

static const char *const strings[] = {
	NULL, "Wimmo", "Virtual EHCI test device", "0001", "Wimmo test interface",
};

static struct testdev {
	uint8_t addr;
	uint8_t new_addr;
	bool addr_pending;
	uint8_t config;
	bool stall;
	bool data_in;
	uint8_t resp[64];
	const uint8_t *in;
	size_t in_len;
	size_t in_pos;
} td;

static size_t string_desc(uint8_t idx, uint8_t *out, size_t max)
{
	if (idx == 0) {
		static const uint8_t langs[] = {4, 0x03, 0x09, 0x04};	/* en-US */

		memcpy(out, langs, MIN(max, sizeof(langs)));
		return sizeof(langs);
	}
	if (idx >= ARRAY_SIZE(strings)) {
		return 0;
	}

	size_t n = strlen(strings[idx]);
	size_t len = 2 + 2 * n;

	if (len > max) {
		n = (max - 2) / 2;
		len = 2 + 2 * n;
	}
	out[0] = len;
	out[1] = 0x03;
	for (size_t i = 0; i < n; i++) {
		out[2 + 2 * i] = strings[idx][i];
		out[3 + 2 * i] = 0;
	}
	return len;
}

static void setup(const uint8_t *s)
{
	uint8_t type = s[0], req = s[1];
	uint16_t value = sys_get_le16(&s[2]);
	uint16_t length = sys_get_le16(&s[6]);
	static const char *const names[] = {"?", "device", "configuration", "string"};

	td.stall = false;
	td.data_in = type & 0x80;
	td.in = NULL;
	td.in_len = 0;
	td.in_pos = 0;

	switch (req) {
	case REQ_GET_DESCRIPTOR:
		switch (value >> 8) {
		case 0x01:
			td.in = dev_desc;
			td.in_len = sizeof(dev_desc);
			break;
		case 0x02:
			td.in = cfg_desc;
			td.in_len = sizeof(cfg_desc);
			break;
		case 0x03:
			td.in_len = string_desc(value & 0xff, td.resp, sizeof(td.resp));
			td.in = td.resp;
			td.stall = td.in_len == 0;
			break;
		default:
			td.stall = true;	/* e.g. device qualifier: high-speed only */
			break;
		}
		if (!td.stall && (value >> 8) <= 3) {
			LOG_INF("vEHCI test device: GET_DESCRIPTOR %s %u (%u bytes)",
				names[value >> 8], value & 0xff, MIN(length, td.in_len));
		}
		break;
	case REQ_SET_ADDRESS:
		td.new_addr = value & 0x7f;
		td.addr_pending = true;	/* takes effect after the status stage */
		break;
	case REQ_SET_CONFIGURATION:
		td.config = value & 0xff;
		LOG_INF("vEHCI test device: SET_CONFIGURATION %u", td.config);
		break;
	case REQ_GET_CONFIGURATION:
		td.resp[0] = td.config;
		td.in = td.resp;
		td.in_len = 1;
		break;
	case REQ_GET_STATUS:
		td.resp[0] = td.resp[1] = 0;
		td.in = td.resp;
		td.in_len = 2;
		break;
	case REQ_GET_INTERFACE:
		td.resp[0] = 0;
		td.in = td.resp;
		td.in_len = 1;
		break;
	case REQ_SET_INTERFACE:
		td.stall = value != 0;
		break;
	case REQ_CLEAR_FEATURE:
	case REQ_SET_FEATURE:
		break;
	default:
		td.stall = true;
		break;
	}
	td.in_len = MIN(td.in_len, length);
}

static int testdev_transfer(void *ctx, uint8_t ep, uint8_t pid, uint8_t *buf, size_t len,
			    size_t *actual)
{
	*actual = 0;
	if (ep != 0) {
		return -EPIPE;
	}

	switch (pid) {
	case VEHCI_PID_SETUP:
		if (len != 8) {
			return -EIO;
		}
		setup(buf);
		*actual = 8;
		return 0;
	case VEHCI_PID_IN:
		if (td.stall) {
			return -EPIPE;
		}
		if (td.data_in && td.in_pos < td.in_len) {
			size_t n = MIN(len, td.in_len - td.in_pos);

			memcpy(buf, td.in + td.in_pos, n);
			td.in_pos += n;
			*actual = n;
			return 0;
		}
		/* status stage of an OUT or no-data request */
		if (td.addr_pending) {
			td.addr = td.new_addr;
			td.addr_pending = false;
			LOG_INF("vEHCI test device: SET_ADDRESS %u", td.addr);
		}
		return 0;
	case VEHCI_PID_OUT:
		if (td.stall) {
			return -EPIPE;
		}
		*actual = len;	/* data of an OUT request, or the status stage of an IN one */
		return 0;
	default:
		return -EIO;
	}
}

static void testdev_reset(void *ctx)
{
	memset(&td, 0, sizeof(td));
}

static uint8_t testdev_address(void *ctx)
{
	return td.addr;
}

const struct vehci_device_ops vehci_testdev_ops = {
	.transfer = testdev_transfer,
	.reset = testdev_reset,
	.address = testdev_address,
};

void *vehci_testdev_ctx(void)
{
	return &td;
}
