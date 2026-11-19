/*
 * USB2 host on the M7: start Zephyr's USB host stack on the EHCI controller
 * (modules/devenv uhc_ci_ehci) and report devices on the M7 console.
 *
 * The EHCI driver prints root-port attach/detach and speed. The "monitor"
 * class below prints every device the host stack enumerates, also behind
 * hubs: it matches everything, prints, and declines, so the real class
 * drivers (hub, ...) still bind. Its name sorts before them, so it runs first.
 *
 * Copyright (c) 2026, imx95-devenv contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/usb/usbh.h>

LOG_MODULE_REGISTER(m7_usb, LOG_LEVEL_INF);

/* usbh_class.h (private): probe of the whole device rather than one interface */
#define IFNUM_DEVICE 0xff

USBH_CONTROLLER_DEFINE(usb2_host, DEVICE_DT_GET(DT_NODELABEL(usb2)));

static const char *speed_str(enum usb_device_speed s)
{
	static const char *const names[] = {"?", "low", "full", "high", "super"};

	return s < ARRAY_SIZE(names) ? names[s] : "?";
}

static const char *class_str(uint8_t c)
{
	switch (c) {
	case 0x00: return "per-interface";
	case 0x01: return "audio";
	case 0x02: return "CDC";
	case 0x03: return "HID";
	case 0x07: return "printer";
	case 0x08: return "mass storage";
	case 0x09: return "hub";
	case 0x0a: return "CDC data";
	case 0x0e: return "video";
	case 0xe0: return "wireless";
	case 0xef: return "misc";
	case 0xff: return "vendor";
	default: return "other";
	}
}

static int monitor_init(struct usbh_class_data *const c_data)
{
	return 0;
}

static int monitor_probe(struct usbh_class_data *const c_data, struct usb_device *const udev,
			 const uint8_t iface)
{
	const struct usb_device_descriptor *d = &udev->dev_desc;

	if (iface == IFNUM_DEVICE) {
		char where[24] = "root port";

		if (udev->hub) {
			snprintk(where, sizeof(where), "hub %u port %u", udev->hub->addr,
				 udev->hub_port);
		}
		LOG_INF("USB2: device %04x:%04x at address %u (%s-speed, %s), class %02x (%s), "
			"USB %x.%02x",
			sys_le16_to_cpu(d->idVendor), sys_le16_to_cpu(d->idProduct), udev->addr,
			speed_str(udev->speed), where, d->bDeviceClass, class_str(d->bDeviceClass),
			sys_le16_to_cpu(d->bcdUSB) >> 8, sys_le16_to_cpu(d->bcdUSB) & 0xff);
	} else {
		LOG_INF("USB2:   address %u interface %u", udev->addr, iface);
	}
	return -ENOTSUP;	/* let the real class drivers bind */
}

static struct usbh_class_api monitor_api = {
	.init = monitor_init,
	.probe = monitor_probe,
};

USBH_DEFINE_CLASS(devenv_usb_monitor, &monitor_api, NULL, NULL);

static bool started;

static int usb2_start(void)
{
	int err;

	if (started) {
		return -EALREADY;
	}
	err = usbh_init(&usb2_host);
	if (err == 0) {
		err = usbh_enable(&usb2_host);
	}
	if (err) {
		LOG_ERR("USB2: host start failed (%d)", err);
		return err;
	}
	started = true;
	LOG_INF("USB2: host started, waiting for devices");
	return 0;
}

static int cmd_usb2_start(const struct shell *sh, size_t argc, char **argv)
{
	return usb2_start();
}

static int cmd_usb2_regs(const struct shell *sh, size_t argc, char **argv)
{
	const uintptr_t base = DT_REG_ADDR_BY_IDX(DT_NODELABEL(usb2), 0);
	static const struct {
		uint16_t off;
		const char *name;
	} regs[] = {
		{0x140, "USBCMD"}, {0x144, "USBSTS"}, {0x148, "USBINTR"}, {0x14c, "FRINDEX"},
		{0x154, "PERIODICLISTBASE"}, {0x158, "ASYNCLISTADDR"}, {0x180, "CONFIGFLAG"},
		{0x184, "PORTSC1"}, {0x1a4, "OTGSC"}, {0x1a8, "USBMODE"},
	};

	if (!started) {
		shell_warn(sh, "host not started: registers may be unreachable");
	}
	for (size_t i = 0; i < ARRAY_SIZE(regs); i++) {
		shell_print(sh, "%-16s 0x%08x", regs[i].name, sys_read32(base + regs[i].off));
	}
	return 0;
}

/* Extendable: other files add subcommands with SHELL_SUBCMD_ADD((usb2), ...) */
SHELL_SUBCMD_SET_CREATE(usb2_cmds, (usb2));
SHELL_SUBCMD_ADD((usb2), start, NULL, "start the USB2 host (EHCI)", cmd_usb2_start, 1, 0);
SHELL_SUBCMD_ADD((usb2), regs, NULL, "dump EHCI operational registers", cmd_usb2_regs, 1, 0);
SHELL_CMD_REGISTER(usb2, &usb2_cmds, "USB2 host on the M7", NULL);

#if defined(CONFIG_APP_USB2_AUTOSTART)
static int usb2_autostart(void)
{
	return usb2_start();
}
SYS_INIT(usb2_autostart, APPLICATION, 99);
#endif
