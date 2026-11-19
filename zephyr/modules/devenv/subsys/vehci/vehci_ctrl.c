/*
 * Control endpoint helper for the devices the virtual EHCI emulates itself
 * (hub, test device): SETUP parsing hook, IN data from a buffer, status
 * stages, STALL.
 *
 * Copyright (c) 2026, imx95-devenv contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <string.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include "vehci_internal.h"

int vehci_ctrl_stage(struct vehci_ctrl *c, struct vehci_xfer *x,
		     void (*setup_cb)(struct vehci_ctrl *c, void *ctx),
		     void (*status_cb)(struct vehci_ctrl *c, void *ctx), void *ctx)
{
	x->actual = 0;

	switch (x->pid) {
	case VEHCI_PID_SETUP:
		if (x->len != 8 || vehci_xfer_read(x, 0, c->setup, 8)) {
			return -EIO;
		}
		c->stall = false;
		c->data_in = c->setup[0] & 0x80;
		c->in = NULL;
		c->in_len = 0;
		c->in_pos = 0;
		setup_cb(c, ctx);
		c->in_len = MIN(c->in_len, sys_get_le16(&c->setup[6]));
		x->actual = 8;
		return 0;
	case VEHCI_PID_IN:
		if (c->stall) {
			return -EPIPE;
		}
		if (c->data_in) {
			size_t n = MIN(x->len, c->in_len - c->in_pos);

			if (n && vehci_xfer_write(x, 0, c->in + c->in_pos, n)) {
				return -EIO;
			}
			c->in_pos += n;
			x->actual = n;
			return 0;
		}
		/* status stage of an OUT or no-data request */
		if (status_cb) {
			status_cb(c, ctx);
		}
		return 0;
	case VEHCI_PID_OUT:
		if (c->stall) {
			return -EPIPE;
		}
		/* data of an OUT request (ignored), or the status stage of an IN one */
		x->actual = c->data_in ? 0 : x->len;
		return 0;
	default:
		return -EIO;
	}
}

size_t vehci_string_desc(const char *const *strings, size_t count, uint8_t idx, uint8_t *out,
			 size_t max)
{
	if (idx == 0) {
		static const uint8_t langs[] = {4, 0x03, 0x09, 0x04};	/* en-US */

		memcpy(out, langs, MIN(max, sizeof(langs)));
		return sizeof(langs);
	}
	if (idx >= count || !strings[idx]) {
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
