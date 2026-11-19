/*
 * Translator between the virtual EHCI ("Wimmo") and the physical USB
 * controller (uhc_ci_ehci, raw interface).
 *
 * The host's EHCI driver enumerates and drives the physical devices itself;
 * the M7 sits in the middle and touches every descriptor:
 *  - The device on the physical root port appears on the Wimmo hub's port
 *    with the same speed; attach/detach follow the physical port, a reset of
 *    the hub port resets the physical port.
 *  - Every qTD stage the host queues for that device becomes one physical
 *    qTD (ci_ehci_raw_submit), keyed by the host qTD's address. The stage
 *    NAKs (vEHCI retries it) until the physical one completes; then the
 *    status and byte count go back into the host's qTD.
 *  - USB addresses pass through: the host's SET_ADDRESS reaches the real
 *    device. The data toggle is the host's (the physical qTD carries it).
 *  - TT routing: the host reaches full/low-speed devices through the
 *    virtual hub's TT; physically they hang off the controller's own TT on
 *    the root port (hub 0, port 1). TTs of real high-speed hubs pass through.
 *  - Data: zero-copy by default (the physical controller DMAs straight into
 *    the host's buffers, vehci_dma_addr), or bounced through M7 memory.
 *  - Pipelining: while a bulk stage is in flight, the stages the host already
 *    queued behind it are submitted too (vEHCI prefetch, up to
 *    CI_EHCI_RAW_DEPTH per endpoint), so the physical bus keeps streaming.
 *    Stages the controller did not run (behind a short packet) are simply
 *    submitted again when their turn comes.
 *  - A stage the host stops asking for (URB unlinked) is cancelled.
 *
 * Snooping, for the console: SET_ADDRESS, device VID:PID, SET_CONFIGURATION,
 * and key presses of boot-protocol keyboards found in the configuration
 * descriptors.
 *
 * Copyright (c) 2026, imx95-devenv contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <string.h>
#include <zephyr/cache.h>
#include <zephyr/drivers/usb/uhc.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>
#include <zephyr/timing/timing.h>

#include <devenv/ci_ehci_raw.h>
#include <vehci/vehci_xlat.h>

#include "vehci_internal.h"

LOG_MODULE_REGISTER(vehci_xlat, CONFIG_VEHCI_LOG_LEVEL);

#define MAX_INFLIGHT	32
#define BOUNCE_SLOTS	8
#define BOUNCE_SIZE	20480		/* what one qTD can describe */
#define STALE_MS	500		/* host stopped asking: cancel */
#define MAX_ADDR	128

enum state { FREE, PENDING, DONE };

struct inflight {
	volatile enum state state;
	uint16_t key;
	uint32_t id;
	int64_t last_seen;
	int64_t submitted;
	bool warned;
	timing_t t_sub;
	timing_t t_done;
	int bounce;			/* slot or -1 */
	struct vehci_xfer x;		/* the host's stage, as first seen */
	struct ci_ehci_raw_xfer raw;
};

/* What passed through, per USB address */
struct snoop {
	uint8_t setup[8];
	bool kbd;
	uint8_t kbd_ep;
	uint8_t keys[6];
};

static struct {
	const struct device *uhc;
	bool attached;
	uint8_t speed;
	bool bounce;
	atomic_t reset_busy;
	/* per endpoint key: when the last result went back (for the submit gap) */
	struct {
		uint16_t key;
		timing_t t;
	} last_back[MAX_INFLIGHT];
	struct k_work reset_work;
	struct inflight in[MAX_INFLIGHT];
	uint32_t bounce_used;
	struct snoop snoop[MAX_ADDR];
	struct vehci_xlat_stats st;
} xl;

/* Page aligned: a qTD's five page pointers cover 20 KiB only from a page start */
static uint8_t __aligned(4096) bounce_buf[BOUNCE_SLOTS][BOUNCE_SIZE];

/* --- key decoding for the snoop --- */

static void key_name(uint8_t usage, bool shift, char *out, size_t len)
{
	static const char punct[][2] = {
		{'1', '!'}, {'2', '@'}, {'3', '#'}, {'4', '$'}, {'5', '%'}, {'6', '^'},
		{'7', '&'}, {'8', '*'}, {'9', '('}, {'0', ')'},
		{0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
		{'-', '_'}, {'=', '+'}, {'[', '{'}, {']', '}'}, {'\\', '|'},
		{'#', '~'}, {';', ':'}, {'\'', '"'}, {'`', '~'}, {',', '<'}, {'.', '>'},
		{'/', '?'},
	};
	static const char *const named[] = {
		[0x28] = "Enter", [0x29] = "Esc", [0x2a] = "Backspace", [0x2b] = "Tab",
		[0x2c] = "Space", [0x39] = "CapsLock", [0x4a] = "Home", [0x4b] = "PageUp",
		[0x4c] = "Delete", [0x4d] = "End", [0x4e] = "PageDown", [0x4f] = "Right",
		[0x50] = "Left", [0x51] = "Down", [0x52] = "Up",
	};

	if (usage >= 0x04 && usage <= 0x1d) {
		snprintk(out, len, "'%c'", (shift ? 'A' : 'a') + usage - 0x04);
	} else if (usage >= 0x1e && usage <= 0x38 && punct[usage - 0x1e][0]) {
		snprintk(out, len, "'%c'", punct[usage - 0x1e][shift]);
	} else if (usage >= 0x3a && usage <= 0x45) {
		snprintk(out, len, "F%u", usage - 0x3a + 1);
	} else if (usage < ARRAY_SIZE(named) && named[usage]) {
		snprintk(out, len, "%s", named[usage]);
	} else {
		snprintk(out, len, "key 0x%02x", usage);
	}
}

static void snoop_keys(struct snoop *sn, const uint8_t *r, uint8_t addr)
{
	uint8_t mod = r[0];
	bool shift = mod & (BIT(1) | BIT(5));

	if (r[2] == 0x01) {
		return;	/* rollover */
	}
	for (int i = 2; i < 8; i++) {
		bool held = false;
		char name[16];

		if (!r[i]) {
			continue;
		}
		for (int j = 0; j < 6; j++) {
			held |= sn->keys[j] == r[i];
		}
		if (held) {
			continue;
		}
		key_name(r[i], shift, name, sizeof(name));
		LOG_INF("xlat: key %s%s%s%s (keyboard at address %u, passing to the host)",
			(mod & (BIT(0) | BIT(4))) ? "Ctrl+" : "", (mod & (BIT(2) | BIT(6))) ? "Alt+" : "",
			(mod & (BIT(3) | BIT(7))) ? "Cmd+" : "", name, addr);
	}
	memcpy(sn->keys, &r[2], 6);
}

/* Find boot keyboards (HID 3/1/1) and their interrupt IN endpoint */
static void snoop_config(struct snoop *sn, const uint8_t *d, size_t len, uint8_t addr)
{
	bool in_kbd = false;

	for (size_t i = 0; i + 2 <= len && d[i] >= 2 && i + d[i] <= len; i += d[i]) {
		if (d[i + 1] == 0x04 && d[i] >= 9) {
			in_kbd = d[i + 5] == 3 && d[i + 6] == 1 && d[i + 7] == 1;
		} else if (d[i + 1] == 0x05 && d[i] >= 7 && in_kbd && (d[i + 2] & 0x80) &&
			   (d[i + 3] & 3) == 3) {
			if (!sn->kbd) {
				LOG_INF("xlat: address %u is a boot keyboard (EP 0x%02x)", addr,
					d[i + 2]);
			}
			sn->kbd = true;
			sn->kbd_ep = d[i + 2] & 0xf;
			in_kbd = false;
		}
	}
}

static void snoop_stage(const struct vehci_xfer *x, int status)
{
	struct snoop *sn = &xl.snoop[x->addr & 0x7f];
	uint8_t buf[64];
	size_t n = MIN(x->actual, sizeof(buf));

	if (status) {
		return;
	}
	if (x->control && x->pid == VEHCI_PID_SETUP) {
		vehci_xfer_read(x, 0, sn->setup, 8);
		if (sn->setup[0] == 0x00 && sn->setup[1] == 0x09) {
			LOG_INF("xlat: host configures address %u (SET_CONFIGURATION %u)", x->addr,
				sn->setup[2]);
		}
		return;
	}
	if (x->control && x->pid == VEHCI_PID_IN && x->actual) {
		if (vehci_xfer_read(x, 0, buf, n)) {
			return;
		}
		if (sn->setup[0] == 0x80 && sn->setup[1] == 0x06 && sn->setup[3] == 0x01 &&
		    n >= 12) {
			LOG_INF("xlat: address %u is %04x:%04x (device descriptor, passed through)",
				x->addr, sys_get_le16(&buf[8]), sys_get_le16(&buf[10]));
		} else if (sn->setup[0] == 0x80 && sn->setup[1] == 0x06 && sn->setup[3] == 0x02 &&
			   n > 9) {
			snoop_config(sn, buf, n, x->addr);
		}
		return;
	}
	if (x->control && x->pid == VEHCI_PID_IN && !x->actual && sn->setup[0] == 0x00 &&
	    sn->setup[1] == 0x05) {
		uint8_t a = sn->setup[2] & 0x7f;

		LOG_INF("xlat: host gives the device address %u (SET_ADDRESS passed through)", a);
		memset(&xl.snoop[a], 0, sizeof(xl.snoop[a]));
		return;
	}
	if (IS_ENABLED(CONFIG_VEHCI_XLAT_KEYS) && sn->kbd && x->periodic &&
	    x->pid == VEHCI_PID_IN && x->ep == sn->kbd_ep && x->actual >= 8 &&
	    !vehci_xfer_read(x, 0, buf, 8)) {
		snoop_keys(sn, buf, x->addr);
	}
}

/* --- in-flight stages --- */

static uint16_t stage_key(const struct vehci_xfer *x)
{
	uint8_t dir = x->control ? 2 : (x->pid == VEHCI_PID_IN);

	return (x->addr << 8) | (x->ep << 4) | (dir << 1) | x->periodic;
}

static void raw_done(struct ci_ehci_raw_xfer *raw)
{
	struct inflight *f = raw->user;

	f->t_done = timing_counter_get();
	f->state = DONE;	/* the vEHCI thread picks it up on its next retry */
}

static void inflight_free(struct inflight *f)
{
	if (f->state == PENDING) {
		ci_ehci_raw_cancel(xl.uhc, &f->raw);
		xl.st.cancelled++;
	}
	if (f->bounce >= 0) {
		xl.bounce_used &= ~BIT(f->bounce);
	}
	f->state = FREE;
}

static void inflight_gc(int64_t now)
{
	for (int i = 0; i < MAX_INFLIGHT; i++) {
		struct inflight *f = &xl.in[i];

		if (f->state != FREE && now - f->last_seen > STALE_MS) {
			inflight_free(f);
		}
	}
}

static int bounce_get(void)
{
	for (int i = 0; i < BOUNCE_SLOTS; i++) {
		if (!(xl.bounce_used & BIT(i))) {
			xl.bounce_used |= BIT(i);
			return i;
		}
	}
	return -1;
}

/* Fill the physical page list: the host's pages (zero-copy) or a bounce buffer */
static int stage_buffers(struct inflight *f, const struct vehci_xfer *x)
{
	struct ci_ehci_raw_xfer *r = &f->raw;
	size_t pages = DIV_ROUND_UP((x->bufp[0] & 0xfff) + x->len, 0x1000);

	f->bounce = -1;
	if (!xl.bounce) {
		bool ok = true;

		for (int k = 0; k < 5; k++) {
			uint32_t a = 0;

			if (k < pages || (k == 0)) {
				a = vehci_dma_addr(x->bufp[k] & ~0xfffU);
				ok &= a != 0;
			}
			r->bufp[k] = a | (k == 0 ? (x->bufp[0] & 0xfff) : 0);
		}
		if (ok || !x->len) {
			xl.st.zero_copy++;
			return 0;
		}
	}

	/* bounce */
	f->bounce = bounce_get();
	if (f->bounce < 0) {
		return -EAGAIN;
	}
	uint8_t *b = bounce_buf[f->bounce];

	if (x->pid != VEHCI_PID_IN && x->len) {
		if (vehci_xfer_read(x, 0, b, x->len)) {
			return -EIO;
		}
		sys_cache_data_flush_range(b, x->len);
	} else if (x->len) {
		sys_cache_data_flush_and_invd_range(b, x->len);
	}
	for (int k = 0; k < 5; k++) {
		r->bufp[k] = (uint32_t)b + k * 0x1000;
	}
	xl.st.bounced++;
	return 0;
}

static struct inflight *find_stage(uint16_t key, uint32_t id, int *count)
{
	struct inflight *f = NULL;

	*count = 0;
	for (int i = 0; i < MAX_INFLIGHT; i++) {
		struct inflight *e = &xl.in[i];

		if (e->state != FREE && e->key == key) {
			(*count)++;
			if (e->id == id) {
				f = e;
			}
		}
	}
	return f;
}

static void drop_key(uint16_t key)
{
	for (int i = 0; i < MAX_INFLIGHT; i++) {
		if (xl.in[i].state != FREE && xl.in[i].key == key) {
			inflight_free(&xl.in[i]);
		}
	}
}

/* Build and queue the physical stage; 0, -EAGAIN (no room now) or -EIO */
static int submit_stage(const struct vehci_xfer *x, uint16_t key, int64_t now)
{
	struct inflight *f = NULL;
	int ret;

	for (int i = 0; i < MAX_INFLIGHT && !f; i++) {
		if (xl.in[i].state == FREE) {
			f = &xl.in[i];
		}
	}
	if (!f) {
		return -EAGAIN;
	}
	memset(&f->raw, 0, sizeof(f->raw));
	f->key = key;
	f->id = x->id;
	f->last_seen = now;
	f->submitted = now;
	f->warned = false;
	f->x = *x;
	ret = stage_buffers(f, x);
	if (ret) {
		if (f->bounce >= 0) {
			xl.bounce_used &= ~BIT(f->bounce);
		}
		return ret == -EAGAIN ? -EAGAIN : -EIO;
	}

	f->raw.addr = x->addr;
	f->raw.ep = x->ep;
	f->raw.pid = x->pid;
	f->raw.speed = x->speed;
	f->raw.control = x->control;
	f->raw.periodic = x->periodic;
	f->raw.mps = x->mps;
	f->raw.dt = x->dt;
	f->raw.len = x->len;
	f->raw.done = raw_done;
	f->raw.user = f;
	if (x->speed != VEHCI_SPEED_HIGH) {
		/* TT: the virtual hub's becomes the controller's own; real HS hubs pass through */
		bool virt = x->tt_hub == vehci_hub_address();

		f->raw.tt_hub = virt ? 0 : x->tt_hub;
		f->raw.tt_port = virt ? 1 : x->tt_port;
	}

	f->t_sub = timing_counter_get();
	for (int i = 0; i < MAX_INFLIGHT; i++) {
		if (xl.last_back[i].key == key) {
			xl.st.gap_cycles += timing_cycles_get(&xl.last_back[i].t, &f->t_sub);
			xl.last_back[i].key = 0xffff;	/* counted once */
			break;
		}
	}
	f->state = PENDING;
	ret = ci_ehci_raw_submit(xl.uhc, &f->raw);
	if (ret) {
		f->state = FREE;
		if (f->bounce >= 0) {
			xl.bounce_used &= ~BIT(f->bounce);
		}
		return (ret == -EBUSY || ret == -ENOMEM) ? -EAGAIN : -EIO;
	}
	return 0;
}

static int xlat_transfer(void *ctx, struct vehci_xfer *x)
{
	int64_t now = k_uptime_get();
	uint16_t key = stage_key(x);
	struct inflight *f;
	int count, ret;

	inflight_gc(now);
	f = find_stage(key, x->id, &count);

	if (f && f->state == DONE && f->raw.status == -ECONNRESET) {
		/* the controller did not run it (behind a short packet): submit again */
		inflight_free(f);
		f = NULL;
		count--;
	}
	if (!f && count) {
		/* the host moved on (unlinked or requeued): what we had is stale */
		drop_key(key);
	}

	if (f) {
		f->last_seen = now;
		if (f->state == PENDING) {
			/* look at the physical qTD directly rather than wait for its interrupt */
			ci_ehci_raw_poll(xl.uhc, &f->raw);
		}
		if (f->state == PENDING) {
			if (!f->warned && now - f->submitted > 5000) {
				f->warned = true;
				LOG_WRN("xlat: stage pending %lld ms: addr %u ep %u %s len %u dt %u %s",
					now - f->submitted, x->addr, x->ep,
					x->pid == VEHCI_PID_IN ? "IN" : x->pid == VEHCI_PID_OUT ? "OUT" : "SETUP",
					(unsigned int)x->len, x->dt, f->bounce >= 0 ? "bounce" : "zero-copy");
			}
			return -EAGAIN;
		}
		/* DONE: hand the result to the host */
		int status = f->raw.status;
		timing_t now_t = timing_counter_get();

		xl.st.timed++;
		xl.st.hw_cycles += timing_cycles_get(&f->t_sub, &f->t_done);
		xl.st.notice_cycles += timing_cycles_get(&f->t_done, &now_t);
		for (int i = 0; i < MAX_INFLIGHT; i++) {
			if (xl.last_back[i].key == key || xl.last_back[i].key == 0 ||
			    i == MAX_INFLIGHT - 1) {
				xl.last_back[i].key = key;
				xl.last_back[i].t = now_t;
				break;
			}
		}
		x->actual = f->raw.actual;
		if (!status && f->bounce >= 0 && x->pid == VEHCI_PID_IN && x->actual) {
			sys_cache_data_invd_range(bounce_buf[f->bounce], x->actual);
			if (vehci_xfer_write(x, 0, bounce_buf[f->bounce], x->actual)) {
				status = -EIO;
			}
		}
		inflight_free(f);
		snoop_stage(x, status);
		xl.st.stages++;
		if (status == -EPIPE) {
			xl.st.stalls++;
			return -EPIPE;
		}
		if (status) {
			xl.st.errors++;
			return -EIO;
		}
		return 0;
	}

	/* New stage */
	if (!xl.attached) {
		return -EIO;
	}
	ret = submit_stage(x, key, now);
	return ret == -EIO ? -EIO : -EAGAIN;
}

static int xlat_prefetch(void *ctx, struct vehci_xfer *x)
{
	int64_t now = k_uptime_get();
	uint16_t key = stage_key(x);
	struct inflight *f;
	int count;

	if (!xl.attached) {
		return -ENODEV;
	}
	f = find_stage(key, x->id, &count);
	if (f) {
		f->last_seen = now;
		return 0;	/* already queued: look further */
	}
	if (count >= CI_EHCI_RAW_DEPTH || submit_stage(x, key, now)) {
		return -ENOSPC;
	}
	xl.st.prefetched++;
	return 0;
}

static void cancel_all(void)
{
	for (int i = 0; i < MAX_INFLIGHT; i++) {
		if (xl.in[i].state != FREE) {
			inflight_free(&xl.in[i]);
		}
	}
}

/* --- port --- */

static void reset_work(struct k_work *w)
{
	uint8_t speed;
	int ret = ci_ehci_raw_port_reset(xl.uhc, &speed);

	if (ret) {
		LOG_WRN("xlat: physical port reset failed (%d)", ret);
	} else if (speed != xl.speed) {
		LOG_INF("xlat: physical device now at speed %u", speed);
	}
	atomic_set(&xl.reset_busy, 0);
}

static void xlat_reset(void *ctx)
{
	cancel_all();
	memset(xl.snoop, 0, sizeof(xl.snoop));	/* every address is 0 again */
	atomic_set(&xl.reset_busy, 1);
	k_work_submit(&xl.reset_work);
	xl.st.resets++;
}

static bool xlat_reset_busy(void *ctx)
{
	return atomic_get(&xl.reset_busy);
}

static const struct vehci_device_ops xlat_ops = {
	.transfer = xlat_transfer,
	.reset = xlat_reset,
	.reset_busy = xlat_reset_busy,
	.prefetch = xlat_prefetch,
	/* no .address: the host's addresses pass through, everything but the hub's is ours */
};

static int uhc_event(const struct device *dev, const struct uhc_event *const evt)
{
	static const char *const speeds[] = {"full", "low", "high"};
	uint8_t speed;

	switch (evt->type) {
	case UHC_EVT_DEV_CONNECTED_LS:
	case UHC_EVT_DEV_CONNECTED_FS:
	case UHC_EVT_DEV_CONNECTED_HS:
		speed = evt->type == UHC_EVT_DEV_CONNECTED_HS ? VEHCI_SPEED_HIGH :
			evt->type == UHC_EVT_DEV_CONNECTED_LS ? VEHCI_SPEED_LOW : VEHCI_SPEED_FULL;
		xl.speed = speed;
		xl.attached = true;
		LOG_INF("xlat: physical %s-speed device -> Wimmo hub port 1", speeds[speed]);
		vehci_port_attach(&xlat_ops, NULL, speed);
		break;
	case UHC_EVT_DEV_REMOVED:
		xl.attached = false;
		LOG_INF("xlat: physical device gone -> Wimmo hub port 1 empty");
		vehci_port_detach();
		cancel_all();
		break;
	default:
		break;
	}
	return 0;
}

/* --- API --- */

int vehci_xlat_start(const struct device *uhc)
{
	int ret;

	xl.uhc = uhc;
	xl.bounce = IS_ENABLED(CONFIG_VEHCI_XLAT_BOUNCE);
	k_work_init(&xl.reset_work, reset_work);
	ret = uhc_init(uhc, uhc_event, NULL);
	if (ret == 0) {
		ret = uhc_enable(uhc);
	}
	if (ret) {
		LOG_ERR("xlat: cannot start the physical controller (%d)", ret);
		return ret;
	}
	LOG_INF("xlat: translating between the Wimmo virtual EHCI and %s (%s)", uhc->name,
		xl.bounce ? "bounce buffers" : "zero-copy");
	return 0;
}

void vehci_xlat_set_bounce(bool bounce)
{
	xl.bounce = bounce;
}

bool vehci_xlat_bounce(void)
{
	return xl.bounce;
}

void vehci_xlat_get_stats(struct vehci_xlat_stats *s)
{
	*s = xl.st;
}
