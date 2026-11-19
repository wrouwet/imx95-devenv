/*
 * "Wimmo" virtual test device on the virtual hub's port: a high-speed USB
 * device with only a control endpoint, answering the standard requests a host
 * needs to enumerate it. VID:PID 1209:0001 is the pid.codes test ID.
 *
 * Copyright (c) 2026, imx95-devenv contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <string.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include "vehci_internal.h"

LOG_MODULE_REGISTER(vehci_testdev, CONFIG_VEHCI_LOG_LEVEL);

static const uint8_t dev_desc[18] = {
	18, 0x01, 0x00, 0x02,	/* DEVICE, USB 2.00 */
	0x00, 0x00, 0x00,	/* class per interface */
	64,
	0x09, 0x12, 0x01, 0x00,	/* 1209:0001 (pid.codes test) */
	0x00, 0x01,
	1, 2, 3,
	1,
};

static const uint8_t cfg_desc[18] = {
	9, 0x02, 18, 0, 1, 1, 0, 0x80, 50,
	9, 0x04, 0, 0, 0, 0xff, 0x00, 0x00, 4,	/* vendor interface, control only */
};

static const char *const strings[] = {
	NULL, "Wimmo", "Virtual EHCI test device", "0001", "Wimmo test interface",
};

static struct {
	struct vehci_ctrl ctrl;
	uint8_t addr;
	uint8_t new_addr;
	bool addr_pending;
	uint8_t config;
} td;

static void td_setup(struct vehci_ctrl *c, void *ctx)
{
	uint8_t type = c->setup[0], req = c->setup[1];
	uint16_t value = sys_get_le16(&c->setup[2]);
	static const char *const names[] = {"?", "device", "configuration", "string"};

	switch ((type << 8) | req) {
	case 0x8006:	/* GET_DESCRIPTOR */
		switch (value >> 8) {
		case 0x01:
			c->in = dev_desc;
			c->in_len = sizeof(dev_desc);
			break;
		case 0x02:
			c->in = cfg_desc;
			c->in_len = sizeof(cfg_desc);
			break;
		case 0x03:
			c->in_len = vehci_string_desc(strings, ARRAY_SIZE(strings), value & 0xff,
						      c->resp, sizeof(c->resp));
			c->in = c->resp;
			c->stall = c->in_len == 0;
			break;
		default:
			c->stall = true;	/* e.g. device qualifier */
			break;
		}
		if (!c->stall) {
			LOG_INF("vEHCI test device: GET_DESCRIPTOR %s %u", names[value >> 8],
				value & 0xff);
		}
		break;
	case 0x0005:	/* SET_ADDRESS */
		td.new_addr = value & 0x7f;
		td.addr_pending = true;
		break;
	case 0x0009:	/* SET_CONFIGURATION */
		td.config = value & 0xff;
		LOG_INF("vEHCI test device: SET_CONFIGURATION %u", td.config);
		break;
	case 0x8008:	/* GET_CONFIGURATION */
		c->resp[0] = td.config;
		c->in = c->resp;
		c->in_len = 1;
		break;
	case 0x8000:	/* GET_STATUS */
	case 0x8100:
	case 0x8200:
		c->resp[0] = c->resp[1] = 0;
		c->in = c->resp;
		c->in_len = 2;
		break;
	case 0x810a:	/* GET_INTERFACE */
		c->resp[0] = 0;
		c->in = c->resp;
		c->in_len = 1;
		break;
	case 0x010b:	/* SET_INTERFACE */
		c->stall = value != 0;
		break;
	case 0x0001:	/* CLEAR/SET_FEATURE */
	case 0x0003:
	case 0x0201:
	case 0x0203:
		break;
	default:
		c->stall = true;
		break;
	}
}

static void td_status(struct vehci_ctrl *c, void *ctx)
{
	if (td.addr_pending) {
		td.addr = td.new_addr;
		td.addr_pending = false;
		LOG_INF("vEHCI test device: SET_ADDRESS %u", td.addr);
	}
}

static int testdev_transfer(void *ctx, struct vehci_xfer *x)
{
	if (x->ep != 0) {
		return -EPIPE;
	}
	return vehci_ctrl_stage(&td.ctrl, x, td_setup, td_status, NULL);
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
