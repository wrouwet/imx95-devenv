/*
 * "Wimmo hub": the virtual USB 2.0 high-speed hub on the virtual EHCI's root
 * port, emulated here. One downstream port; the device on it is the built-in
 * test device or the translator to the physical USB controller.
 *
 * Why a hub: a plain EHCI (as seen through PCI) has no transaction translator,
 * so full/low-speed devices on its root port would go to companion
 * controllers. Behind a high-speed hub with a TT the host's stock EHCI driver
 * reaches them with split transactions, which the emulation simply executes.
 *
 * Copyright (c) 2026, imx95-devenv contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include "vehci_internal.h"

LOG_MODULE_REGISTER(vehci_hub, CONFIG_VEHCI_LOG_LEVEL);

/* Port status and change bits (USB 2.0 11.24.2.7) */
#define PS_CONNECTION		BIT(0)
#define PS_ENABLE		BIT(1)
#define PS_SUSPEND		BIT(2)
#define PS_RESET		BIT(4)
#define PS_POWER		BIT(8)
#define PS_LOW_SPEED		BIT(9)
#define PS_HIGH_SPEED		BIT(10)
#define PC_CONNECTION		BIT(0)
#define PC_ENABLE		BIT(1)
#define PC_SUSPEND		BIT(2)
#define PC_OVER_CURRENT		BIT(3)
#define PC_RESET		BIT(4)

/* Port features */
#define F_PORT_ENABLE		1
#define F_PORT_SUSPEND		2
#define F_PORT_RESET		4
#define F_PORT_POWER		8
#define F_C_PORT_CONNECTION	16
#define F_C_PORT_ENABLE		17
#define F_C_PORT_SUSPEND	18
#define F_C_PORT_OVER_CURRENT	19
#define F_C_PORT_RESET		20

#define PORT_RESET_MS		10

static const uint8_t dev_desc[18] = {
	18, 0x01, 0x00, 0x02,	/* DEVICE, USB 2.00 */
	0x09, 0x00, 0x01,	/* hub class, single TT */
	64,
	0x09, 0x12, 0x02, 0x00,	/* 1209:0002 */
	0x00, 0x01,
	1, 2, 0,
	1,
};

static const uint8_t cfg_desc[25] = {
	9, 0x02, 25, 0, 1, 1, 0, 0xe0, 0,	/* self powered, remote wakeup */
	9, 0x04, 0, 0, 1, 0x09, 0x00, 0x00, 0,	/* hub interface, single TT */
	7, 0x05, 0x81, 0x03, 1, 0, 12,		/* EP1 IN interrupt, 1 byte, 2^11 uframes */
};

static const uint8_t hub_desc[9] = {
	9, 0x29, 1,		/* HUB descriptor, one port */
	0x09, 0x00,		/* individual power switching and over-current, TT think time 8 FS bit times */
	50, 0,			/* power on to power good: 100 ms */
	0x00, 0xff,		/* device removable, port power control mask */
};

static const char *const strings[] = {NULL, "Wimmo", "Virtual USB 2.0 hub"};

static struct {
	struct vehci_ctrl ctrl;
	uint8_t addr;
	uint8_t new_addr;
	bool addr_pending;
	uint8_t config;
	bool rwakeup;		/* DEVICE_REMOTE_WAKEUP armed by the host */
	/* downstream port */
	const struct vehci_device_ops *dops;
	void *dctx;
	enum vehci_speed dspeed;
	bool present;
	bool powered;
	bool enabled;
	bool resetting;
	bool suspended;
	uint16_t change;
	int64_t reset_start;
} hub;

static uint16_t port_status(void)
{
	uint16_t s = 0;

	if (hub.powered) {
		s |= PS_POWER;
		if (hub.present) {
			s |= PS_CONNECTION;
			s |= hub.dspeed == VEHCI_SPEED_LOW ? PS_LOW_SPEED :
			     hub.dspeed == VEHCI_SPEED_HIGH ? PS_HIGH_SPEED : 0;
		}
	}
	s |= hub.enabled ? PS_ENABLE : 0;
	s |= hub.resetting ? PS_RESET : 0;
	s |= hub.suspended ? PS_SUSPEND : 0;
	return s;
}

static const char *speed_name(enum vehci_speed s)
{
	return s == VEHCI_SPEED_HIGH ? "high" : s == VEHCI_SPEED_LOW ? "low" : "full";
}

static void port_feature(bool set, uint16_t feature)
{
	switch (feature) {
	case F_PORT_RESET:
		if (set && hub.powered && hub.present) {
			hub.resetting = true;
			hub.enabled = false;
			hub.reset_start = k_uptime_get();
			if (hub.dops) {
				hub.dops->reset(hub.dctx);
			}
			LOG_INF("Wimmo hub: port 1 reset");
		}
		break;
	case F_PORT_POWER:
		if (set && !hub.powered) {
			hub.powered = true;
			if (hub.present) {
				hub.change |= PC_CONNECTION;
			}
			LOG_INF("Wimmo hub: port 1 power on%s", hub.present ? ", device present" : "");
		} else if (!set) {
			hub.powered = hub.enabled = hub.suspended = false;
		}
		break;
	case F_PORT_ENABLE:
		if (!set) {
			hub.enabled = false;
		}
		break;
	case F_PORT_SUSPEND:
		hub.suspended = set && hub.enabled;
		if (!set) {
			hub.change |= PC_SUSPEND;
		}
		break;
	case F_C_PORT_CONNECTION:
		hub.change &= ~PC_CONNECTION;
		break;
	case F_C_PORT_ENABLE:
		hub.change &= ~PC_ENABLE;
		break;
	case F_C_PORT_SUSPEND:
		hub.change &= ~PC_SUSPEND;
		break;
	case F_C_PORT_OVER_CURRENT:
		hub.change &= ~PC_OVER_CURRENT;
		break;
	case F_C_PORT_RESET:
		hub.change &= ~PC_RESET;
		break;
	default:
		break;
	}
}

static void hub_setup(struct vehci_ctrl *c, void *ctx)
{
	uint8_t type = c->setup[0], req = c->setup[1];
	uint16_t value = sys_get_le16(&c->setup[2]);
	uint16_t index = sys_get_le16(&c->setup[4]);

	switch ((type << 8) | req) {
	case 0x8006:	/* GET_DESCRIPTOR (device) */
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
			c->stall = true;
			break;
		}
		break;
	case 0xa006:	/* GET_DESCRIPTOR (hub class) */
		c->in = hub_desc;
		c->in_len = sizeof(hub_desc);
		break;
	case 0x0005:	/* SET_ADDRESS */
		hub.new_addr = value & 0x7f;
		hub.addr_pending = true;
		break;
	case 0x0009:	/* SET_CONFIGURATION */
		hub.config = value & 0xff;
		LOG_INF("Wimmo hub: configured");
		break;
	case 0x8008:	/* GET_CONFIGURATION */
		c->resp[0] = hub.config;
		c->in = c->resp;
		c->in_len = 1;
		break;
	case 0x8000:	/* GET_STATUS device: self powered */
		c->resp[0] = 0x01;
		c->resp[1] = 0;
		c->in = c->resp;
		c->in_len = 2;
		break;
	case 0x8100:	/* GET_STATUS interface/endpoint */
	case 0x8200:
	case 0xa000:	/* GET_STATUS hub */
		memset(c->resp, 0, 4);
		c->in = c->resp;
		c->in_len = (type == 0xa0) ? 4 : 2;
		break;
	case 0xa300:	/* GET_STATUS port */
		if (index != 1) {
			c->stall = true;
			break;
		}
		sys_put_le16(port_status(), &c->resp[0]);
		sys_put_le16(hub.change, &c->resp[2]);
		c->in = c->resp;
		c->in_len = 4;
		break;
	case 0x2303:	/* SET_FEATURE port */
	case 0x2301:	/* CLEAR_FEATURE port */
		if (index != 1) {
			c->stall = true;
			break;
		}
		port_feature(req == 0x03, value);
		break;
	case 0x0003:	/* SET/CLEAR_FEATURE device: remote wakeup */
	case 0x0001:
		if (value == 1) {
			hub.rwakeup = req == 0x03;
		}
		break;
	case 0x2003:	/* SET/CLEAR_FEATURE hub */
	case 0x2001:
	case 0x0203:	/* SET/CLEAR_FEATURE endpoint */
	case 0x0201:
	case 0x2308:	/* CLEAR_TT_BUFFER */
	case 0x2309:	/* RESET_TT */
	case 0x230b:	/* STOP_TT */
		break;
	case 0x810a:	/* GET_INTERFACE */
		c->resp[0] = 0;
		c->in = c->resp;
		c->in_len = 1;
		break;
	case 0x010b:	/* SET_INTERFACE */
		c->stall = value != 0;
		break;
	default:
		c->stall = true;
		break;
	}
}

static void hub_status(struct vehci_ctrl *c, void *ctx)
{
	if (hub.addr_pending) {
		hub.addr = hub.new_addr;
		hub.addr_pending = false;
		LOG_INF("Wimmo hub: address %u", hub.addr);
	}
}

static int hub_transfer(void *ctx, struct vehci_xfer *x)
{
	if (x->ep == 0) {
		return vehci_ctrl_stage(&hub.ctrl, x, hub_setup, hub_status, NULL);
	}
	if (x->ep != 1 || x->pid != VEHCI_PID_IN) {
		return -EPIPE;
	}
	/* Status change endpoint: bit 1 = port 1; NAK while nothing changed */
	if (!hub.change || !hub.config) {
		return -EAGAIN;
	}
	uint8_t bitmap = BIT(1);

	x->actual = MIN(x->len, 1);
	return x->actual ? vehci_xfer_write(x, 0, &bitmap, 1) : 0;
}

static void hub_reset(void *ctx)
{
	/* Bus reset of the hub itself: address 0, unconfigured, ports unpowered */
	memset(&hub.ctrl, 0, sizeof(hub.ctrl));
	hub.addr = 0;
	hub.addr_pending = false;
	hub.config = 0;
	hub.rwakeup = false;
	hub.powered = hub.enabled = hub.resetting = hub.suspended = false;
	hub.change = 0;
}

static uint8_t hub_address(void *ctx)
{
	return hub.addr;
}

const struct vehci_device_ops vehci_hub_ops = {
	.transfer = hub_transfer,
	.reset = hub_reset,
	.address = hub_address,
};

bool vehci_hub_route(uint8_t addr, const struct vehci_device_ops **ops, void **ctx)
{
	if (addr == hub.addr) {
		*ops = &vehci_hub_ops;
		*ctx = NULL;
		return true;
	}
	if (!hub.enabled || hub.suspended || !hub.dops) {
		return false;
	}
	if (hub.dops->address && hub.dops->address(hub.dctx) != addr) {
		return false;
	}
	*ops = hub.dops;
	*ctx = hub.dctx;
	return true;
}

void vehci_hub_poll(int64_t now)
{
	if (hub.resetting && now - hub.reset_start >= PORT_RESET_MS &&
	    !(hub.dops && hub.dops->reset_busy && hub.dops->reset_busy(hub.dctx))) {
		hub.resetting = false;
		hub.enabled = hub.present;
		hub.change |= PC_RESET;
		LOG_INF("Wimmo hub: port 1 enabled (%s speed)", speed_name(hub.dspeed));
	}
}

bool vehci_hub_wakeup_pending(void)
{
	return hub.rwakeup && hub.change;
}

void vehci_hub_attach(const struct vehci_device_ops *ops, void *ctx, enum vehci_speed speed)
{
	hub.dops = ops;
	hub.dctx = ctx;
	hub.dspeed = speed;
	hub.present = true;
	hub.enabled = false;
	if (hub.powered) {
		hub.change |= PC_CONNECTION;
	}
	LOG_INF("Wimmo hub: %s-speed device on port 1", speed_name(speed));
}

void vehci_hub_detach(void)
{
	bool was = hub.present;

	hub.present = false;
	hub.enabled = hub.resetting = hub.suspended = false;
	hub.dops = NULL;
	if (was && hub.powered) {
		hub.change |= PC_CONNECTION;
	}
	LOG_INF("Wimmo hub: port 1 empty");
}

const char *vehci_hub_port_state(void)
{
	if (!hub.powered) {
		return "unpowered";
	}
	if (!hub.present) {
		return "empty";
	}
	if (hub.resetting) {
		return "resetting";
	}
	if (!hub.enabled) {
		return "connected, disabled";
	}
	return hub.suspended ? "enabled, suspended" : "enabled";
}
