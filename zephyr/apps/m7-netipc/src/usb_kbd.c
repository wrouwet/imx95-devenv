/*
 * Demo: print the keys pressed on a USB keyboard attached to USB2 (M7).
 *
 * A minimal HID class driver for boot-protocol keyboards (interface class 3,
 * subclass 1, protocol 1). It switches the keyboard to the boot protocol (a
 * fixed 8-byte report: modifiers, reserved, six key codes), keeps one
 * interrupt IN transfer in flight and prints every newly pressed key.
 * Debugging/demo only: optional at build time (CONFIG_APP_USB2_KBD_DEMO) and
 * at run time ("usb2 keys on|off").
 *
 * Copyright (c) 2026, imx95-devenv contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/usb/usbh.h>

/* Private host stack headers (subsys/usb/host) */
#include "usbh_ch9.h"
#include "usbh_class.h"
#include "usbh_desc.h"
#include "usbh_device.h"

LOG_MODULE_REGISTER(m7_usb_kbd, LOG_LEVEL_INF);

#define HID_REQ_SET_IDLE	0x0a
#define HID_REQ_SET_PROTOCOL	0x0b
#define HID_PROTOCOL_BOOT	0

struct kbd_data {
	struct usb_device *udev;
	uint8_t iface;
	uint8_t ep;
	uint16_t mps;
	bool connected;
	uint8_t prev[6];
};

static struct kbd_data kbd;
static bool keys_enabled = true;

static const char *const named_keys[] = {
	[0x28] = "Enter", [0x29] = "Esc", [0x2a] = "Backspace", [0x2b] = "Tab", [0x2c] = "Space",
	[0x39] = "CapsLock", [0x46] = "PrintScreen", [0x47] = "ScrollLock", [0x48] = "Pause",
	[0x49] = "Insert", [0x4a] = "Home", [0x4b] = "PageUp", [0x4c] = "Delete", [0x4d] = "End",
	[0x4e] = "PageDown", [0x4f] = "Right", [0x50] = "Left", [0x51] = "Down", [0x52] = "Up",
	[0x53] = "NumLock", [0x58] = "KP-Enter", [0x65] = "Menu",
};

/* Printable keys: usage 0x1e..0x38 (digits and punctuation), unshifted/shifted */
static const char digits_punct[][2] = {
	{'1', '!'}, {'2', '@'}, {'3', '#'}, {'4', '$'}, {'5', '%'}, {'6', '^'}, {'7', '&'},
	{'8', '*'}, {'9', '('}, {'0', ')'},				/* 0x1e..0x27 */
	{0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},				/* 0x28..0x2c named */
	{'-', '_'}, {'=', '+'}, {'[', '{'}, {']', '}'}, {'\\', '|'},	/* 0x2d..0x31 */
	{'#', '~'}, {';', ':'}, {'\'', '"'}, {'`', '~'}, {',', '<'},	/* 0x32..0x36 */
	{'.', '>'}, {'/', '?'},						/* 0x37..0x38 */
};

static void key_name(uint8_t usage, bool shift, char *out, size_t len)
{
	if (usage >= 0x04 && usage <= 0x1d) {
		char c = 'a' + (usage - 0x04);

		snprintk(out, len, "'%c'", shift ? c - 'a' + 'A' : c);
	} else if (usage >= 0x1e && usage <= 0x38 && digits_punct[usage - 0x1e][0]) {
		snprintk(out, len, "'%c'", digits_punct[usage - 0x1e][shift ? 1 : 0]);
	} else if (usage >= 0x3a && usage <= 0x45) {
		snprintk(out, len, "F%u", usage - 0x3a + 1);
	} else if (usage < ARRAY_SIZE(named_keys) && named_keys[usage]) {
		snprintk(out, len, "%s", named_keys[usage]);
	} else {
		snprintk(out, len, "key 0x%02x", usage);
	}
}

static void kbd_report(const uint8_t *r, size_t len)
{
	if (len < 8) {
		return;
	}
	/* All slots 0x01: rollover error, report carries no keys */
	if (r[2] == 0x01) {
		return;
	}

	uint8_t mod = r[0];
	bool shift = mod & (BIT(1) | BIT(5));

	for (int i = 2; i < 8; i++) {
		uint8_t k = r[i];
		bool was_down = false;
		char name[16];

		if (k == 0) {
			continue;
		}
		for (int j = 0; j < 6; j++) {
			was_down |= kbd.prev[j] == k;
		}
		if (was_down || !keys_enabled) {
			continue;
		}
		key_name(k, shift, name, sizeof(name));
		LOG_INF("USB2 key: %s%s%s%s%s", (mod & (BIT(0) | BIT(4))) ? "Ctrl+" : "",
			(mod & (BIT(2) | BIT(6))) ? "Alt+" : "",
			(mod & (BIT(3) | BIT(7))) ? "Cmd+" : "",
			(shift && name[0] != '\'') ? "Shift+" : "", name);
	}
	memcpy(kbd.prev, &r[2], sizeof(kbd.prev));
}

static int kbd_in_cb(struct usb_device *const udev, struct uhc_transfer *const xfer);

static int kbd_arm(void)
{
	struct uhc_transfer *xfer;
	struct net_buf *buf;
	int ret;

	xfer = usbh_xfer_alloc(kbd.udev, kbd.ep, kbd_in_cb, &kbd);
	if (xfer == NULL) {
		return -ENOMEM;
	}
	buf = usbh_xfer_buf_alloc(kbd.udev, kbd.mps);
	if (buf == NULL) {
		(void)uhc_xfer_unref(xfer);
		return -ENOMEM;
	}
	xfer->buf = buf;
	ret = usbh_xfer_enqueue(kbd.udev, xfer);
	if (ret) {
		usbh_xfer_buf_free(kbd.udev, buf);
		(void)uhc_xfer_unref(xfer);
	}
	return ret;
}

static int kbd_in_cb(struct usb_device *const udev, struct uhc_transfer *const xfer)
{
	struct net_buf *buf = xfer->buf;
	int err = xfer->err;

	if (err == 0 && buf) {
		kbd_report(buf->data, buf->len);
	}
	if (buf) {
		net_buf_unref(buf);
	}
	(void)uhc_xfer_unref(xfer);

	if (!kbd.connected) {
		return 0;
	}
	if (err && err != -EPIPE) {
		LOG_WRN("USB2 keyboard: report transfer failed (%d), stopping", err);
		return 0;
	}
	return kbd_arm();
}

static int kbd_init(struct usbh_class_data *const c_data)
{
	return 0;
}

static int kbd_probe(struct usbh_class_data *const c_data, struct usb_device *const udev,
		     const uint8_t iface)
{
	const struct usb_desc_header *h;
	int ret;

	if (iface == USBH_CLASS_IFNUM_DEVICE || kbd.connected) {
		return -ENOTSUP;	/* interface-level only, one keyboard at a time */
	}

	h = usbh_desc_get_iface(udev, iface);
	kbd.ep = 0;
	for (h = h ? usbh_desc_get_next(h) : NULL; h != NULL; h = usbh_desc_get_next(h)) {
		if (h->bDescriptorType == USB_DESC_INTERFACE) {
			break;
		}
		if (usbh_desc_is_valid_endpoint(h)) {
			const struct usb_ep_descriptor *ep = (const void *)h;

			if (USB_EP_DIR_IS_IN(ep->bEndpointAddress) &&
			    (ep->bmAttributes & USB_EP_TRANSFER_TYPE_MASK) == USB_EP_TYPE_INTERRUPT) {
				kbd.ep = ep->bEndpointAddress;
				kbd.mps = sys_le16_to_cpu(ep->wMaxPacketSize) & 0x7ff;
				break;
			}
		}
	}
	if (kbd.ep == 0) {
		return -ENOTSUP;
	}

	kbd.udev = udev;
	kbd.iface = iface;
	memset(kbd.prev, 0, sizeof(kbd.prev));

	/* Boot protocol: fixed 8-byte reports; idle 0: report only on change */
	ret = usbh_req_setup(udev, USB_REQTYPE_TYPE_CLASS << 5 | USB_REQTYPE_RECIPIENT_INTERFACE,
			     HID_REQ_SET_PROTOCOL, HID_PROTOCOL_BOOT, iface, 0, NULL);
	if (ret) {
		LOG_WRN("USB2 keyboard: SET_PROTOCOL failed (%d)", ret);
	}
	(void)usbh_req_setup(udev, USB_REQTYPE_TYPE_CLASS << 5 | USB_REQTYPE_RECIPIENT_INTERFACE,
			     HID_REQ_SET_IDLE, 0, iface, 0, NULL);

	kbd.connected = true;
	ret = kbd_arm();
	if (ret) {
		kbd.connected = false;
		LOG_ERR("USB2 keyboard: cannot start reports (%d)", ret);
		return ret;
	}
	LOG_INF("USB2 keyboard at address %u interface %u: key presses are printed%s",
		udev->addr, iface, keys_enabled ? "" : " (now off: usb2 keys on)");
	return 0;
}

static int kbd_removed(struct usbh_class_data *const c_data)
{
	kbd.connected = false;	/* the pending transfer completes with an error and stops */
	LOG_INF("USB2 keyboard removed");
	return 0;
}

static struct usbh_class_api kbd_api = {
	.init = kbd_init,
	.probe = kbd_probe,
	.removed = kbd_removed,
};

static struct usbh_class_filter kbd_filters[] = {
	{
		.class = USB_BCC_HID,
		.sub = 1,	/* boot interface */
		.proto = 1,	/* keyboard */
		.flags = USBH_CLASS_MATCH_CODE_TRIPLE,
	},
	{0},
};

USBH_DEFINE_CLASS(m7_usb_kbd_demo, &kbd_api, NULL, kbd_filters);

static int cmd_keys(const struct shell *sh, size_t argc, char **argv)
{
	if (argc > 1) {
		keys_enabled = strcmp(argv[1], "off") != 0;
	}
	shell_print(sh, "key printing %s, keyboard %s", keys_enabled ? "on" : "off",
		    kbd.connected ? "attached" : "not attached");
	return 0;
}

SHELL_SUBCMD_ADD((usb2), keys, NULL, "[on|off] print keyboard key presses (demo)", cmd_keys, 1,
		 1);
