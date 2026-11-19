/*
 * EHCI host controller driver for the ChipIdea USB 2.0 controller with an
 * integrated transaction translator (i.MX95 USB2), for Zephyr's UHC API.
 *
 * Written for imx95-devenv from the EHCI 1.0 specification and the i.MX
 * reference manual register descriptions.
 *
 * Scope ("basic"): one root port; control, bulk and interrupt transfers;
 * high-, full- and low-speed devices, the latter two through the integrated
 * TT (no companion controller), also behind hubs. No isochronous, no
 * suspend/resume. One transfer in flight per endpoint; the rest queue.
 *
 * Memory: EHCI structures (periodic frame list, queue heads, qTDs) live in a
 * pool the controller reaches by DMA and the CPU never caches (M7 DTCM).
 * Transfer buffers come from the UHC buffer pool in cached memory; they are
 * cache-line aligned and padded, so they get cleaned/invalidated around DMA.
 *
 * Interrupt endpoints are polled once per frame (1 ms), whatever their
 * bInterval; that is within what the USB spec allows a host to do.
 *
 * Besides the UHC API there is a raw interface (devenv/ci_ehci_raw.h) for the
 * virtual EHCI translator: one stage as an EHCI qTD describes it (address,
 * endpoint, PID, speed, TT routing, toggle, buffer page list) becomes one
 * physical qTD on a queue head per (address, endpoint, direction). The toggle
 * always comes from the qTD (DTC=1), so the caller's toggle state rules.
 *
 * Copyright (c) 2026, imx95-devenv contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT devenv_ci_ehci

#include <string.h>
#include <zephyr/cache.h>
#include <zephyr/device.h>
#include <zephyr/drivers/usb/uhc.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/barrier.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/sys_io.h>

#include "uhc_common.h"
#include <devenv/ci_ehci_raw.h>

LOG_MODULE_REGISTER(uhc_ci_ehci, CONFIG_UHC_DRIVER_LOG_LEVEL);

/* --- ChipIdea controller registers (offsets from the controller base) --- */
#define CI_ID			0x000
#define CI_CAPLENGTH		0x100	/* 8 bit */
#define CI_HCIVERSION		0x102	/* 16 bit */
#define CI_HCSPARAMS		0x104
#define CI_USBMODE		0x1a8

#define HCSPARAMS_N_PORTS(x)	((x) & 0xf)
#define HCSPARAMS_PPC		BIT(4)

#define USBMODE_CM_MASK		0x3
#define USBMODE_CM_HOST		0x3

/* EHCI operational registers (offsets from base + CAPLENGTH) */
#define OP_USBCMD		0x00
#define OP_USBSTS		0x04
#define OP_USBINTR		0x08
#define OP_FRINDEX		0x0c
#define OP_PERIODICLISTBASE	0x14
#define OP_ASYNCLISTADDR	0x18
#define OP_CONFIGFLAG		0x40
#define OP_PORTSC1		0x44

#define USBCMD_RS		BIT(0)
#define USBCMD_RST		BIT(1)
#define USBCMD_PSE		BIT(4)
#define USBCMD_ASE		BIT(5)
#define USBCMD_IAA		BIT(6)
#define USBCMD_ITC(n)		((n) << 16)
#define USBCMD_ITC_MASK		(0xff << 16)

#define USBSTS_UI		BIT(0)
#define USBSTS_UEI		BIT(1)
#define USBSTS_PCI		BIT(2)
#define USBSTS_SEI		BIT(4)
#define USBSTS_AAI		BIT(5)
#define USBSTS_HCH		BIT(12)
#define USBSTS_PS		BIT(14)
#define USBSTS_AS		BIT(15)
#define USBSTS_IRQS		(USBSTS_UI | USBSTS_UEI | USBSTS_PCI | USBSTS_SEI | USBSTS_AAI)

#define PORTSC_CCS		BIT(0)
#define PORTSC_CSC		BIT(1)
#define PORTSC_PE		BIT(2)
#define PORTSC_PEC		BIT(3)
#define PORTSC_OCA		BIT(4)
#define PORTSC_OCC		BIT(5)
#define PORTSC_SUSP		BIT(7)
#define PORTSC_PR		BIT(8)
#define PORTSC_LS(x)		(((x) >> 10) & 0x3)
#define PORTSC_PP		BIT(12)
#define PORTSC_PHCD		BIT(23)	/* ChipIdea: PHY low-power clock disable */
#define PORTSC_PSPD(x)		(((x) >> 26) & 0x3)	/* ChipIdea: 0 FS, 1 LS, 2 HS */
#define PORTSC_W1C		(PORTSC_CSC | PORTSC_PEC | PORTSC_OCC)

/* USB non-core (USBNC) block */
#define USBNC_CTRL1		0x000
#define USBNC_CTRL1_OC_DIS	BIT(7)

/* --- EHCI data structures (EHCI 1.0, 3.5 and 3.6) --- */
#define LINK_T			BIT(0)
#define LINK_TYP_QH		(1 << 1)

#define QTD_STS_ACTIVE		BIT(7)
#define QTD_STS_HALTED		BIT(6)
#define QTD_STS_DBE		BIT(5)
#define QTD_STS_BABBLE		BIT(4)
#define QTD_STS_XACT		BIT(3)
#define QTD_PID_OUT		(0 << 8)
#define QTD_PID_IN		(1 << 8)
#define QTD_PID_SETUP		(2 << 8)
#define QTD_CERR(n)		((n) << 10)
#define QTD_IOC			BIT(15)
#define QTD_BYTES(n)		((uint32_t)(n) << 16)
#define QTD_GET_BYTES(t)	(((t) >> 16) & 0x7fff)
#define QTD_DT			BIT(31)

#define QH_ADDR(a)		((a) & 0x7f)
#define QH_EP(e)		(((e) & 0xf) << 8)
#define QH_EPS_FS		(0 << 12)
#define QH_EPS_LS		(1 << 12)
#define QH_EPS_HS		(2 << 12)
#define QH_DTC			BIT(14)
#define QH_H			BIT(15)
#define QH_MPS(m)		(((m) & 0x7ff) << 16)
#define QH_C			BIT(27)
#define QH_RL(n)		((n) << 28)

#define QH_SMASK(m)		((m) & 0xff)
#define QH_CMASK(m)		(((m) & 0xff) << 8)
#define QH_HUB(a)		(((a) & 0x7f) << 16)
#define QH_PORT(p)		(((p) & 0x7f) << 23)
#define QH_MULT(n)		((n) << 30)

struct ehci_qtd {
	volatile uint32_t next;
	volatile uint32_t alt;
	volatile uint32_t token;
	volatile uint32_t buf[5];
};

struct ehci_qh {
	volatile uint32_t link;
	volatile uint32_t info1;
	volatile uint32_t info2;
	volatile uint32_t cur;
	/* transfer overlay */
	volatile uint32_t next;
	volatile uint32_t alt;
	volatile uint32_t token;
	volatile uint32_t buf[5];
	uint32_t pad[4];	/* 64-byte stride keeps 32-byte alignment */
};

BUILD_ASSERT(sizeof(struct ehci_qtd) == 32);
BUILD_ASSERT(sizeof(struct ehci_qh) == 64);

#define EHCI_FRAMES		1024
#define EHCI_NUM_EP		32	/* queue heads besides the async head */
#define EHCI_NUM_QTD		128
#define EHCI_XFER_QTDS		8	/* max qTDs per transfer */
#define EHCI_QTD_MAX_BYTES	16384	/* per qTD; multiple of every max packet size */

/* Pool layout: frame list (4 KiB aligned), queue heads, qTDs */
struct ehci_pool {
	volatile uint32_t frames[EHCI_FRAMES];
	struct ehci_qh qh[EHCI_NUM_EP + 1];	/* [0] = async list head */
	struct ehci_qtd qtd[EHCI_NUM_QTD];
};

struct ehci_ep {
	bool used;
	bool linked;
	bool periodic;
	bool reset_toggle;
	struct usb_device *udev;
	uint8_t ep;		/* endpoint address, direction bit included; 0 = control */
	uint8_t type;
	struct ehci_qh *qh;
	/* Queued transfers; the head is on the bus while active */
	sys_dlist_t queue;
	struct uhc_transfer *active;
	uint8_t qtds[EHCI_XFER_QTDS];
	uint8_t nqtd;
	uint8_t data_first;	/* index range in qtds[] holding the data stage */
	uint8_t data_n;
	uint32_t data_len;
	/* raw interface */
	bool is_raw;
	uint16_t raw_key;
	struct ci_ehci_raw_xfer *raw;
};

struct ehci_config {
	uintptr_t base;
	uintptr_t usbnc;
	uintptr_t pool;
	size_t pool_size;
	uint32_t pool_bus;
	bool disable_oc;
	void (*irq_config)(void);
	unsigned int irq;
};

struct ehci_priv {
	const struct device *dev;
	uintptr_t op;
	struct ehci_pool *pool;
	uint32_t bus_off;	/* bus address = CPU address + bus_off, inside the pool */
	struct ehci_ep eps[EHCI_NUM_EP];
	uint32_t qtd_used[EHCI_NUM_QTD / 32];
	atomic_t sts;		/* USBSTS bits collected by the ISR */
	struct k_sem work;
	struct k_sem iaa;
	bool connected;
	bool enabled;
	K_KERNEL_STACK_MEMBER(stack, CONFIG_UHC_CI_EHCI_THREAD_STACK_SIZE);
	struct k_thread thread;
};

/* --- helpers --- */

static inline uint32_t op_read(struct ehci_priv *p, uint32_t reg)
{
	return sys_read32(p->op + reg);
}

static inline void op_write(struct ehci_priv *p, uint32_t reg, uint32_t val)
{
	sys_write32(val, p->op + reg);
}

/* Write an operational register and warn if it does not hold the value (bring-up aid) */
static void op_write_check(struct ehci_priv *p, uint32_t reg, uint32_t val, uint32_t mask)
{
	op_write(p, reg, val);
	for (int t = 0; t < 10; t++) {
		if ((op_read(p, reg) & mask) == (val & mask)) {
			return;
		}
		k_busy_wait(100);
	}
	LOG_WRN("USB2: op reg 0x%02x = 0x%08x after writing 0x%08x", reg, op_read(p, reg), val);
}

static uint32_t bus_addr(struct ehci_priv *p, const volatile void *ptr)
{
	uintptr_t a = (uintptr_t)ptr;
	uintptr_t base = (uintptr_t)p->pool;

	if (a >= base && a < base + sizeof(struct ehci_pool)) {
		return (uint32_t)(a + p->bus_off);
	}
	return (uint32_t)a;
}

static const char *speed_name(enum usb_device_speed s)
{
	switch (s) {
	case USB_SPEED_SPEED_LS:
		return "low";
	case USB_SPEED_SPEED_FS:
		return "full";
	case USB_SPEED_SPEED_HS:
		return "high";
	default:
		return "unknown";
	}
}

static int qtd_alloc(struct ehci_priv *p)
{
	for (int i = 0; i < EHCI_NUM_QTD; i++) {
		if (!(p->qtd_used[i / 32] & BIT(i % 32))) {
			p->qtd_used[i / 32] |= BIT(i % 32);
			memset((void *)&p->pool->qtd[i], 0, sizeof(struct ehci_qtd));
			return i;
		}
	}
	return -1;
}

static void qtd_free(struct ehci_priv *p, int i)
{
	p->qtd_used[i / 32] &= ~BIT(i % 32);
}

static void ep_free_qtds(struct ehci_priv *p, struct ehci_ep *ep)
{
	for (int i = 0; i < ep->nqtd; i++) {
		qtd_free(p, ep->qtds[i]);
	}
	ep->nqtd = 0;
}

/* --- schedule management --- */

static void periodic_rebuild(struct ehci_priv *p)
{
	struct ehci_qh *first = NULL, *prev = NULL;

	for (int i = 0; i < EHCI_NUM_EP; i++) {
		struct ehci_ep *ep = &p->eps[i];

		if (!ep->used || !ep->linked || !ep->periodic) {
			continue;
		}
		ep->qh->link = LINK_T;
		if (prev) {
			prev->link = bus_addr(p, ep->qh) | LINK_TYP_QH;
		} else {
			first = ep->qh;
		}
		prev = ep->qh;
	}
	barrier_dmem_fence_full();

	uint32_t entry = first ? (bus_addr(p, first) | LINK_TYP_QH) : LINK_T;

	for (int f = 0; f < EHCI_FRAMES; f++) {
		p->pool->frames[f] = entry;
	}
	barrier_dsync_fence_full();
}

static void ep_link(struct ehci_priv *p, struct ehci_ep *ep)
{
	struct ehci_qh *head = &p->pool->qh[0];

	if (ep->linked) {
		return;
	}
	ep->linked = true;
	if (ep->periodic) {
		periodic_rebuild(p);
		return;
	}
	ep->qh->link = head->link;
	barrier_dmem_fence_full();
	head->link = bus_addr(p, ep->qh) | LINK_TYP_QH;
	barrier_dsync_fence_full();
}

/* Take a queue head off the schedule and wait until the controller has let go of it */
static void ep_unlink(struct ehci_priv *p, struct ehci_ep *ep)
{
	if (!ep->linked) {
		return;
	}
	ep->linked = false;

	if (ep->periodic) {
		periodic_rebuild(p);
		k_msleep(2);	/* one frame to drain, one spare */
		return;
	}

	uint32_t me = bus_addr(p, ep->qh) | LINK_TYP_QH;
	struct ehci_qh *prev = &p->pool->qh[0];

	if (prev->link != me) {
		for (int i = 0; i < EHCI_NUM_EP; i++) {
			struct ehci_ep *o = &p->eps[i];

			if (o->used && o->linked && !o->periodic && o->qh->link == me) {
				prev = o->qh;
				break;
			}
		}
	}
	prev->link = ep->qh->link;
	barrier_dsync_fence_full();

	/* Interrupt on Async Advance: the controller no longer caches the QH */
	k_sem_reset(&p->iaa);
	op_write(p, OP_USBCMD, op_read(p, OP_USBCMD) | USBCMD_IAA);
	if (k_sem_take(&p->iaa, K_MSEC(20)) != 0) {
		LOG_WRN("async advance timeout");
	}
}

/* --- endpoints --- */

static void ep_setup_qh(struct ehci_priv *p, struct ehci_ep *ep, struct uhc_transfer *xfer)
{
	struct usb_device *udev = xfer->udev;
	uint32_t info1, info2 = QH_MULT(1);

	info1 = QH_ADDR(udev->addr) | QH_EP(USB_EP_GET_IDX(xfer->ep)) |
		QH_MPS(USB_MPS_EP_SIZE(xfer->mps));

	switch (udev->speed) {
	case USB_SPEED_SPEED_HS:
		info1 |= QH_EPS_HS;
		if (!ep->periodic) {
			info1 |= QH_RL(4);
		}
		break;
	case USB_SPEED_SPEED_LS:
		info1 |= QH_EPS_LS;
		break;
	default:
		info1 |= QH_EPS_FS;
		break;
	}

	if (ep->type == USB_EP_TYPE_CONTROL) {
		info1 |= QH_DTC;	/* toggle comes from the qTDs */
		if (udev->speed != USB_SPEED_SPEED_HS) {
			info1 |= QH_C;
		}
	}

	if (udev->speed != USB_SPEED_SPEED_HS) {
		/* Full/low speed: the transaction translator is in the nearest
		 * high-speed hub, or the controller's own (root port 1).
		 */
		uint8_t hub = 0, port = 1;

		for (struct usb_device *d = udev; d->hub != NULL; d = d->hub) {
			if (d->hub->speed == USB_SPEED_SPEED_HS) {
				hub = d->hub->addr;
				port = d->hub_port;
				break;
			}
		}
		info2 |= QH_HUB(hub) | QH_PORT(port);
		if (ep->periodic) {
			info2 |= QH_SMASK(0x01) | QH_CMASK(0x1c);
		}
	} else if (ep->periodic) {
		info2 |= QH_SMASK(0x01);
	}

	ep->qh->info1 = info1;
	ep->qh->info2 = info2;
}

static struct ehci_ep *ep_find(struct ehci_priv *p, struct uhc_transfer *xfer)
{
	uint8_t key = USB_EP_GET_IDX(xfer->ep) == 0 ? 0 : xfer->ep;

	for (int i = 0; i < EHCI_NUM_EP; i++) {
		struct ehci_ep *ep = &p->eps[i];

		if (ep->used && ep->udev == xfer->udev && ep->ep == key) {
			return ep;
		}
	}
	return NULL;
}

static bool ep_idle(struct ehci_ep *ep)
{
	return ep->active == NULL && ep->raw == NULL && sys_dlist_is_empty(&ep->queue);
}

static void ep_release(struct ehci_priv *p, struct ehci_ep *ep)
{
	ep_unlink(p, ep);
	ep_free_qtds(p, ep);
	ep->used = false;
}

static struct ehci_ep *ep_get(struct ehci_priv *p, struct uhc_transfer *xfer)
{
	struct ehci_ep *ep = ep_find(p, xfer);
	struct ehci_ep *spare = NULL;

	if (ep) {
		return ep;
	}

	for (int i = 0; i < EHCI_NUM_EP && !spare; i++) {
		if (!p->eps[i].used) {
			spare = &p->eps[i];
		}
	}
	/* Pool full: recycle an idle endpoint (stale ones from unplugged devices) */
	for (int i = 0; i < EHCI_NUM_EP && !spare; i++) {
		if (ep_idle(&p->eps[i])) {
			ep_release(p, &p->eps[i]);
			spare = &p->eps[i];
		}
	}
	if (!spare) {
		return NULL;
	}

	ep = spare;
	memset((void *)ep->qh, 0, sizeof(*ep->qh));
	ep->used = true;
	ep->linked = false;
	ep->udev = xfer->udev;
	ep->ep = USB_EP_GET_IDX(xfer->ep) == 0 ? 0 : xfer->ep;
	ep->type = xfer->type;
	ep->periodic = xfer->type == USB_EP_TYPE_INTERRUPT;
	ep->reset_toggle = false;
	ep->active = NULL;
	ep->is_raw = false;
	ep->raw = NULL;
	ep->nqtd = 0;
	sys_dlist_init(&ep->queue);
	ep->qh->link = LINK_T;
	ep->qh->next = LINK_T;
	ep->qh->alt = LINK_T;
	ep->qh->token = 0;
	return ep;
}

/* --- transfers --- */

static void qtd_fill(struct ehci_priv *p, struct ehci_qtd *qtd, uint32_t pid, bool dt,
		     const void *data, uint32_t len, bool ioc)
{
	uint32_t addr = data ? bus_addr(p, data) : 0;

	qtd->next = LINK_T;
	qtd->alt = LINK_T;
	qtd->buf[0] = addr;
	for (int i = 1; i < 5; i++) {
		qtd->buf[i] = (addr & ~0xfffU) + i * 0x1000U;
	}
	qtd->token = QTD_BYTES(len) | (ioc ? QTD_IOC : 0) | QTD_CERR(3) | pid |
		     (dt ? QTD_DT : 0) | QTD_STS_ACTIVE;
}

static int qtd_add(struct ehci_priv *p, struct ehci_ep *ep)
{
	int i;

	if (ep->nqtd >= EHCI_XFER_QTDS || (i = qtd_alloc(p)) < 0) {
		return -ENOMEM;
	}
	ep->qtds[ep->nqtd++] = i;
	return i;
}

static int xfer_start(struct ehci_priv *p, struct ehci_ep *ep, struct uhc_transfer *xfer)
{
	struct ehci_pool *pool = p->pool;
	bool ctrl = ep->type == USB_EP_TYPE_CONTROL;
	bool in;
	uint8_t *data = NULL;
	uint32_t len = 0;
	int i, status_i = -1;

	if (ctrl) {
		in = USB_REQTYPE_GET_DIR(xfer->setup_pkt[0]) == USB_REQTYPE_DIR_TO_HOST;
	} else {
		in = USB_EP_DIR_IS_IN(xfer->ep);
	}
	if (xfer->buf) {
		if (in) {
			data = net_buf_tail(xfer->buf);
			len = net_buf_tailroom(xfer->buf);
		} else {
			data = xfer->buf->data;
			len = xfer->buf->len;
		}
	}
	if (in && len > EHCI_QTD_MAX_BYTES) {
		len = EHCI_QTD_MAX_BYTES;	/* single IN qTD keeps short packets simple */
	}

	/* DMA coherency for buffers in cached memory */
	if (ctrl) {
		sys_cache_data_flush_range(xfer->setup_pkt, sizeof(xfer->setup_pkt));
	}
	if (len) {
		if (in) {
			sys_cache_data_flush_and_invd_range(data, len);
		} else {
			sys_cache_data_flush_range(data, len);
		}
	}

	ep->nqtd = 0;
	if (ctrl) {
		i = qtd_add(p, ep);
		if (i < 0) {
			goto nomem;
		}
		qtd_fill(p, &pool->qtd[i], QTD_PID_SETUP, false, xfer->setup_pkt, 8, false);
	}

	ep->data_first = ep->nqtd;
	ep->data_len = len;
	if (len || !ctrl) {
		uint32_t off = 0;

		do {
			uint32_t n = MIN(len - off, EHCI_QTD_MAX_BYTES);

			i = qtd_add(p, ep);
			if (i < 0) {
				goto nomem;
			}
			/* dt only matters for control (DTC=1): data stage starts with DATA1 */
			qtd_fill(p, &pool->qtd[i], in ? QTD_PID_IN : QTD_PID_OUT, ctrl,
				 len ? data + off : NULL, n, false);
			off += n;
		} while (off < len);
	}
	ep->data_n = ep->nqtd - ep->data_first;

	if (ctrl && !xfer->no_status) {
		status_i = qtd_add(p, ep);
		if (status_i < 0) {
			goto nomem;
		}
		/* Status stage: opposite direction of the data stage, IN if none, DATA1 */
		qtd_fill(p, &pool->qtd[status_i], (in && len) ? QTD_PID_OUT : QTD_PID_IN, true,
			 NULL, 0, false);
	}

	/* Chain; interrupt on the last; IN data short packet skips to status */
	for (int k = 0; k < ep->nqtd; k++) {
		struct ehci_qtd *q = &pool->qtd[ep->qtds[k]];

		if (k + 1 < ep->nqtd) {
			q->next = bus_addr(p, &pool->qtd[ep->qtds[k + 1]]);
		} else {
			q->token |= QTD_IOC;
		}
		if (in && status_i >= 0 && k >= ep->data_first &&
		    k < ep->data_first + ep->data_n) {
			q->alt = bus_addr(p, &pool->qtd[status_i]);
		}
	}

	ep_setup_qh(p, ep, xfer);

	/* Restart the overlay: keep the data toggle unless the endpoint was halted */
	uint32_t tok = ep->qh->token;

	ep->qh->token = (ctrl || ep->reset_toggle) ? 0 : (tok & QTD_DT);
	ep->reset_toggle = false;
	ep->qh->alt = LINK_T;
	barrier_dmem_fence_full();
	ep->qh->next = bus_addr(p, &pool->qtd[ep->qtds[0]]);
	barrier_dsync_fence_full();

	ep->active = xfer;
	ep_link(p, ep);
	return 0;

nomem:
	ep_free_qtds(p, ep);
	return -ENOMEM;
}

static int qtd_error(uint32_t tok)
{
	if (tok & QTD_STS_BABBLE) {
		return -EOVERFLOW;
	}
	if (tok & (QTD_STS_XACT | QTD_STS_DBE)) {
		return -EIO;
	}
	return -EPIPE;	/* halted without another error: STALL */
}

/* Returns true if the active transfer on @ep has finished */
static bool xfer_check(struct ehci_priv *p, struct ehci_ep *ep)
{
	struct uhc_transfer *xfer = ep->active;
	uint32_t actual = 0;
	int err = 0;

	/* Done when a qTD halted, or when the last one (status, or the last data
	 * qTD) has run. A short IN data stage jumps to the status qTD through the
	 * alternate pointer, leaving its own successors active.
	 */
	for (int k = 0; k < ep->nqtd; k++) {
		uint32_t tok = p->pool->qtd[ep->qtds[k]].token;

		if (tok & QTD_STS_HALTED) {
			err = qtd_error(tok);
			break;
		}
	}
	if (!err && (p->pool->qtd[ep->qtds[ep->nqtd - 1]].token & QTD_STS_ACTIVE)) {
		return false;
	}

	if (err == 0 || err == -EPIPE) {
		uint32_t req = ep->data_len;

		for (int k = ep->data_first; k < ep->data_first + ep->data_n; k++) {
			uint32_t tok = p->pool->qtd[ep->qtds[k]].token;

			if (!(tok & QTD_STS_ACTIVE)) {
				uint32_t n = MIN(req, (uint32_t)EHCI_QTD_MAX_BYTES);

				actual += n - QTD_GET_BYTES(tok);
			}
			req -= MIN(req, (uint32_t)EHCI_QTD_MAX_BYTES);
		}
	}

	if (err) {
		/* Halted queue head: clear the overlay, restart toggle at DATA0 */
		ep->qh->next = LINK_T;
		ep->qh->token = 0;
		ep->reset_toggle = true;
		LOG_DBG("ep 0x%02x addr %u: error %d", xfer->ep, xfer->udev->addr, err);
	}

	bool in = ep->type == USB_EP_TYPE_CONTROL
			  ? USB_REQTYPE_GET_DIR(xfer->setup_pkt[0]) == USB_REQTYPE_DIR_TO_HOST
			  : USB_EP_DIR_IS_IN(xfer->ep);

	if (in && xfer->buf && actual) {
		sys_cache_data_invd_range(net_buf_tail(xfer->buf), actual);
		net_buf_add(xfer->buf, actual);
	}

	ep_free_qtds(p, ep);
	ep->active = NULL;
	uhc_xfer_return(p->dev, xfer, err);
	return true;
}

/* Fail everything queued or active on @ep */
static void ep_flush(struct ehci_priv *p, struct ehci_ep *ep, int err)
{
	struct uhc_transfer *xfer;
	sys_dnode_t *node;

	if (ep->active) {
		ep_unlink(p, ep);
		ep_free_qtds(p, ep);
		ep->qh->next = LINK_T;
		ep->active = NULL;
	}
	while ((node = sys_dlist_peek_head(&ep->queue)) != NULL) {
		xfer = CONTAINER_OF(node, struct uhc_transfer, node);
		uhc_xfer_return(p->dev, xfer, err);
	}
}

static int port_reset(struct ehci_priv *p, enum usb_device_speed *speed);

/* --- raw interface (virtual EHCI translator) --- */

static void raw_finish(struct ehci_priv *p, struct ehci_ep *ep, int status, size_t actual, bool dt)
{
	struct ci_ehci_raw_xfer *x = ep->raw;

	ep_free_qtds(p, ep);
	ep->raw = NULL;
	x->priv = NULL;
	x->status = status;
	x->actual = actual;
	x->dt_out = dt;
	if (x->done) {
		x->done(x);
	}
}

static void raw_check(struct ehci_priv *p, struct ehci_ep *ep)
{
	struct ci_ehci_raw_xfer *x = ep->raw;
	uint32_t tok = p->pool->qtd[ep->qtds[0]].token;
	int status = 0;

	if (tok & QTD_STS_HALTED) {
		status = qtd_error(tok);
		ep->qh->next = LINK_T;	/* clear the halted overlay */
		ep->qh->token = 0;
	} else if (tok & QTD_STS_ACTIVE) {
		return;
	}
	raw_finish(p, ep, status, x->len - QTD_GET_BYTES(tok), tok & QTD_DT);
}

static uint16_t raw_key(const struct ci_ehci_raw_xfer *x)
{
	/* control endpoints share one queue head for both directions */
	uint8_t dir = x->control ? 2 : (x->pid == CI_EHCI_RAW_PID_IN);

	return (x->addr << 8) | (x->ep << 4) | (dir << 1) | x->periodic;
}

static struct ehci_ep *raw_ep_get(struct ehci_priv *p, const struct ci_ehci_raw_xfer *x)
{
	uint16_t key = raw_key(x);
	struct ehci_ep *spare = NULL;

	for (int i = 0; i < EHCI_NUM_EP; i++) {
		if (p->eps[i].used && p->eps[i].is_raw && p->eps[i].raw_key == key) {
			return &p->eps[i];
		}
	}
	for (int i = 0; i < EHCI_NUM_EP && !spare; i++) {
		if (!p->eps[i].used) {
			spare = &p->eps[i];
		}
	}
	for (int i = 0; i < EHCI_NUM_EP && !spare; i++) {
		if (ep_idle(&p->eps[i])) {
			ep_release(p, &p->eps[i]);
			spare = &p->eps[i];
		}
	}
	if (!spare) {
		return NULL;
	}
	memset((void *)spare->qh, 0, sizeof(*spare->qh));
	spare->used = true;
	spare->linked = false;
	spare->udev = NULL;
	spare->ep = x->ep;
	spare->type = x->control ? USB_EP_TYPE_CONTROL :
		      x->periodic ? USB_EP_TYPE_INTERRUPT : USB_EP_TYPE_BULK;
	spare->periodic = x->periodic;
	spare->active = NULL;
	spare->nqtd = 0;
	spare->is_raw = true;
	spare->raw_key = key;
	spare->raw = NULL;
	sys_dlist_init(&spare->queue);
	spare->qh->link = LINK_T;
	spare->qh->next = LINK_T;
	spare->qh->alt = LINK_T;
	return spare;
}

int ci_ehci_raw_submit(const struct device *dev, struct ci_ehci_raw_xfer *x)
{
	struct ehci_priv *p = uhc_get_private(dev);
	struct ehci_ep *ep;
	struct ehci_qtd *qtd;
	uint32_t info1, info2 = QH_MULT(1);
	int i, ret = 0;

	if (x->len > 5 * 0x1000 - (x->bufp[0] & 0xfff)) {
		return -EINVAL;
	}
	uhc_lock_internal(dev, K_FOREVER);
	if (!p->connected) {
		ret = -ENODEV;
		goto out;
	}
	ep = raw_ep_get(p, x);
	if (!ep) {
		ret = -ENOMEM;
		goto out;
	}
	if (ep->raw) {
		ret = -EBUSY;
		goto out;
	}
	i = qtd_alloc(p);
	if (i < 0) {
		ret = -ENOMEM;
		goto out;
	}
	ep->qtds[0] = i;
	ep->nqtd = 1;

	qtd = &p->pool->qtd[i];
	qtd->next = LINK_T;
	qtd->alt = LINK_T;
	for (int k = 0; k < 5; k++) {
		qtd->buf[k] = x->bufp[k];
	}
	qtd->token = QTD_BYTES(x->len) | QTD_IOC | QTD_CERR(3) | ((uint32_t)x->pid << 8) |
		     (x->dt ? QTD_DT : 0) | QTD_STS_ACTIVE;

	/* Queue head: toggle from the qTD; TT routing for full/low speed */
	info1 = QH_ADDR(x->addr) | QH_EP(x->ep) | QH_MPS(x->mps) | QH_DTC |
		((uint32_t)x->speed << 12);
	if (x->speed == CI_EHCI_RAW_SPEED_HIGH) {
		if (!x->periodic) {
			info1 |= QH_RL(4);
		}
		if (x->periodic) {
			info2 |= QH_SMASK(0x01);
		}
	} else {
		if (x->control) {
			info1 |= QH_C;
		}
		info2 |= QH_HUB(x->tt_hub) | QH_PORT(x->tt_port);
		if (x->periodic) {
			info2 |= QH_SMASK(0x01) | QH_CMASK(0x1c);
		}
	}
	ep->qh->info1 = info1;
	ep->qh->info2 = info2;
	ep->qh->token = 0;
	ep->qh->alt = LINK_T;
	barrier_dmem_fence_full();
	ep->qh->next = bus_addr(p, qtd);
	barrier_dsync_fence_full();

	x->priv = ep;
	ep->raw = x;
	ep_link(p, ep);
	k_sem_give(&p->work);
out:
	uhc_unlock_internal(dev);
	return ret;
}

int ci_ehci_raw_cancel(const struct device *dev, struct ci_ehci_raw_xfer *x)
{
	struct ehci_priv *p = uhc_get_private(dev);
	struct ehci_ep *ep;

	uhc_lock_internal(dev, K_FOREVER);
	ep = x->priv;
	if (ep && ep->raw == x) {
		ep_unlink(p, ep);
		ep_free_qtds(p, ep);
		ep->qh->next = LINK_T;
		ep->qh->token = 0;
		ep->raw = NULL;
		x->priv = NULL;
	}
	uhc_unlock_internal(dev);
	return 0;
}

int ci_ehci_raw_port_reset(const struct device *dev, uint8_t *speed)
{
	struct ehci_priv *p = uhc_get_private(dev);
	enum usb_device_speed s;
	int ret = port_reset(p, &s);

	op_write(p, OP_PORTSC1, op_read(p, OP_PORTSC1) & ~PORTSC_PHCD);	/* ack changes */
	if (ret == 0) {
		*speed = s == USB_SPEED_SPEED_HS ? CI_EHCI_RAW_SPEED_HIGH :
			 s == USB_SPEED_SPEED_LS ? CI_EHCI_RAW_SPEED_LOW : CI_EHCI_RAW_SPEED_FULL;
	}
	return ret;
}

static void schedule(struct ehci_priv *p)
{
	for (int i = 0; i < EHCI_NUM_EP; i++) {
		struct ehci_ep *ep = &p->eps[i];
		sys_dnode_t *node;

		if (!ep->used) {
			continue;
		}
		if (ep->raw) {
			raw_check(p, ep);
			continue;
		}
		if (ep->active && !xfer_check(p, ep)) {
			continue;
		}
		node = sys_dlist_peek_head(&ep->queue);
		if (node && p->connected) {
			struct uhc_transfer *xfer = CONTAINER_OF(node, struct uhc_transfer, node);

			if (xfer_start(p, ep, xfer) != 0) {
				LOG_ERR("out of qTDs");
				uhc_xfer_return(p->dev, xfer, -ENOMEM);
			}
		}
	}
}

/* --- root port --- */

static int port_reset(struct ehci_priv *p, enum usb_device_speed *speed)
{
	uint32_t sc = op_read(p, OP_PORTSC1) & ~(PORTSC_W1C | PORTSC_PHCD);

	op_write(p, OP_PORTSC1, (sc & ~PORTSC_PE) | PORTSC_PR);
	k_msleep(50);	/* USB 2.0: root port reset, at least 50 ms */
	sc = op_read(p, OP_PORTSC1) & ~PORTSC_W1C;
	op_write(p, OP_PORTSC1, sc & ~PORTSC_PR);

	for (int t = 0; t < 20; t++) {
		if (!(op_read(p, OP_PORTSC1) & PORTSC_PR)) {
			break;
		}
		k_msleep(1);
	}
	k_msleep(10);	/* reset recovery */

	sc = op_read(p, OP_PORTSC1);
	if (!(sc & PORTSC_CCS)) {
		return -ENODEV;
	}
	if (!(sc & PORTSC_PE)) {
		LOG_WRN("USB2: port not enabled after reset (PORTSC 0x%08x)", sc);
		return -EIO;
	}
	switch (PORTSC_PSPD(sc)) {
	case 1:
		*speed = USB_SPEED_SPEED_LS;
		break;
	case 2:
		*speed = USB_SPEED_SPEED_HS;
		break;
	default:
		*speed = USB_SPEED_SPEED_FS;
		break;
	}
	return 0;
}

static void port_disconnected(struct ehci_priv *p)
{
	uhc_lock_internal(p->dev, K_FOREVER);
	p->connected = false;
	for (int i = 0; i < EHCI_NUM_EP; i++) {
		struct ehci_ep *ep = &p->eps[i];

		if (!ep->used) {
			continue;
		}
		if (ep->raw) {
			raw_finish(p, ep, -ENODEV, 0, false);
		}
		ep_flush(p, ep, -ENODEV);
		ep_release(p, ep);
	}
	uhc_unlock_internal(p->dev);
	LOG_INF("USB2: device disconnected");
	uhc_submit_event(p->dev, UHC_EVT_DEV_REMOVED, 0);
}

static void port_change(struct ehci_priv *p)
{
	uint32_t sc = op_read(p, OP_PORTSC1);
	enum usb_device_speed speed;
	static const enum uhc_event_type evt[] = {
		[USB_SPEED_SPEED_LS] = UHC_EVT_DEV_CONNECTED_LS,
		[USB_SPEED_SPEED_FS] = UHC_EVT_DEV_CONNECTED_FS,
		[USB_SPEED_SPEED_HS] = UHC_EVT_DEV_CONNECTED_HS,
	};

	/* Acknowledge the change bits */
	op_write(p, OP_PORTSC1, sc & ~PORTSC_PHCD);

	if (sc & PORTSC_OCC) {
		LOG_WRN("USB2: over-current %s", (sc & PORTSC_OCA) ? "detected" : "cleared");
	}

	/* Disconnect = no device present; CSC alone also follows our own port resets */
	if (p->connected && !(sc & PORTSC_CCS)) {
		port_disconnected(p);
	}
	if (p->connected || !(sc & PORTSC_CCS)) {
		return;
	}

	k_msleep(100);	/* attach debounce */
	sc = op_read(p, OP_PORTSC1);
	if (!(sc & PORTSC_CCS)) {
		return;
	}
	LOG_INF("USB2: device attached (line state %u), resetting port", PORTSC_LS(sc));
	if (port_reset(p, &speed) != 0) {
		return;
	}
	op_write(p, OP_PORTSC1, op_read(p, OP_PORTSC1) & ~PORTSC_PHCD);	/* clear CSC from reset */
	p->connected = true;
	LOG_INF("USB2: %s-speed device connected", speed_name(speed));
	uhc_submit_event(p->dev, evt[speed], 0);
}

static void ehci_thread(void *a, void *b, void *c)
{
	struct ehci_priv *p = a;

	while (true) {
		k_sem_take(&p->work, K_MSEC(100));
		uint32_t sts = atomic_clear(&p->sts);

		if (!p->enabled) {
			continue;
		}
		if (sts & USBSTS_SEI) {
			LOG_ERR("USB2: host system error (DMA access fault?)");
		}
		if (sts & USBSTS_PCI) {
			port_change(p);
		}
		uhc_lock_internal(p->dev, K_FOREVER);
		schedule(p);
		uhc_unlock_internal(p->dev);
	}
}

static void ehci_isr(const struct device *dev)
{
	struct ehci_priv *p = uhc_get_private(dev);
	uint32_t sts = op_read(p, OP_USBSTS) & USBSTS_IRQS;

	if (!sts) {
		return;
	}
	op_write(p, OP_USBSTS, sts);
	if (sts & USBSTS_AAI) {
		k_sem_give(&p->iaa);
	}
	atomic_or(&p->sts, sts);
	k_sem_give(&p->work);
}

/* --- UHC API --- */

static int ehci_lock(const struct device *dev)
{
	return uhc_lock_internal(dev, K_FOREVER);
}

static int ehci_unlock(const struct device *dev)
{
	return uhc_unlock_internal(dev);
}

static int ehci_hw_reset(const struct device *dev)
{
	const struct ehci_config *cfg = dev->config;
	struct ehci_priv *p = uhc_get_private(dev);

	op_write(p, OP_USBCMD, op_read(p, OP_USBCMD) & ~USBCMD_RS);
	for (int t = 0; t < 20 && !(op_read(p, OP_USBSTS) & USBSTS_HCH); t++) {
		k_msleep(1);
	}
	op_write(p, OP_USBCMD, USBCMD_RST);
	for (int t = 0; t < 100 && (op_read(p, OP_USBCMD) & USBCMD_RST); t++) {
		k_msleep(1);
	}
	if (op_read(p, OP_USBCMD) & USBCMD_RST) {
		LOG_ERR("USB2: controller reset timeout");
		return -EIO;
	}
	/* ChipIdea: host mode must be selected right after reset */
	sys_write32((sys_read32(cfg->base + CI_USBMODE) & ~USBMODE_CM_MASK) | USBMODE_CM_HOST,
		    cfg->base + CI_USBMODE);
	return 0;
}

static int ehci_init(const struct device *dev)
{
	const struct ehci_config *cfg = dev->config;
	struct ehci_priv *p = uhc_get_private(dev);
	struct ehci_qh *head;
	uint16_t ver = sys_read16(cfg->base + CI_HCIVERSION);
	uint32_t hcs = sys_read32(cfg->base + CI_HCSPARAMS);
	int ret;

	/* Operational registers follow the capability registers (at 0x100) */
	p->op = cfg->base + CI_CAPLENGTH + sys_read8(cfg->base + CI_CAPLENGTH);
	LOG_INF("USB2: ChipIdea EHCI %x.%02x, id 0x%08x, %u port(s)%s", ver >> 8, ver & 0xff,
		sys_read32(cfg->base + CI_ID), HCSPARAMS_N_PORTS(hcs),
		(hcs & HCSPARAMS_PPC) ? ", port power control" : "");

	if (cfg->disable_oc) {
		sys_write32(sys_read32(cfg->usbnc + USBNC_CTRL1) | USBNC_CTRL1_OC_DIS,
			    cfg->usbnc + USBNC_CTRL1);
	}

	ret = ehci_hw_reset(dev);
	if (ret) {
		return ret;
	}

	memset(p->pool, 0, sizeof(*p->pool));
	memset(p->qtd_used, 0, sizeof(p->qtd_used));
	for (int i = 0; i < EHCI_NUM_EP; i++) {
		p->eps[i].used = false;
		p->eps[i].qh = &p->pool->qh[i + 1];
	}
	for (int f = 0; f < EHCI_FRAMES; f++) {
		p->pool->frames[f] = LINK_T;
	}

	/* Async list head: points to itself, never executes */
	head = &p->pool->qh[0];
	head->link = bus_addr(p, head) | LINK_TYP_QH;
	head->info1 = QH_H | QH_EPS_HS;
	head->next = LINK_T;
	head->alt = LINK_T;
	head->token = QTD_STS_HALTED;
	barrier_dsync_fence_full();

	op_write_check(p, OP_PERIODICLISTBASE, bus_addr(p, p->pool->frames), ~0xfffU);
	op_write_check(p, OP_ASYNCLISTADDR, bus_addr(p, head), ~0x1fU);
	p->connected = false;
	return 0;
}

static int ehci_enable(const struct device *dev)
{
	const struct ehci_config *cfg = dev->config;
	struct ehci_priv *p = uhc_get_private(dev);
	uint32_t hcs = sys_read32(cfg->base + CI_HCSPARAMS);
	uint32_t cmd;

	op_write(p, OP_USBSTS, op_read(p, OP_USBSTS));
	op_write_check(p, OP_USBINTR, USBSTS_IRQS, USBSTS_IRQS);

	cmd = op_read(p, OP_USBCMD) & ~USBCMD_ITC_MASK;
	op_write(p, OP_USBCMD, cmd | USBCMD_ITC(1) | USBCMD_RS);
	op_write(p, OP_CONFIGFLAG, 1);

	if (hcs & HCSPARAMS_PPC) {
		op_write(p, OP_PORTSC1,
			 (op_read(p, OP_PORTSC1) & ~(PORTSC_W1C | PORTSC_PHCD)) | PORTSC_PP);
	} else {
		op_write(p, OP_PORTSC1, op_read(p, OP_PORTSC1) & ~(PORTSC_W1C | PORTSC_PHCD));
	}

	op_write(p, OP_USBCMD, op_read(p, OP_USBCMD) | USBCMD_ASE | USBCMD_PSE);
	for (int t = 0; t < 20; t++) {
		uint32_t sts = op_read(p, OP_USBSTS);

		if ((sts & (USBSTS_AS | USBSTS_PS)) == (USBSTS_AS | USBSTS_PS)) {
			break;
		}
		k_msleep(1);
	}

	p->enabled = true;
	irq_enable(cfg->irq);

	/* A device that was already plugged in: handle it like an attach */
	if (op_read(p, OP_PORTSC1) & PORTSC_CCS) {
		atomic_or(&p->sts, USBSTS_PCI);
		k_sem_give(&p->work);
	}

	LOG_INF("USB2: EHCI running (USBCMD 0x%08x, USBSTS 0x%08x, PORTSC 0x%08x)",
		op_read(p, OP_USBCMD), op_read(p, OP_USBSTS), op_read(p, OP_PORTSC1));
	return 0;
}

static int ehci_disable(const struct device *dev)
{
	const struct ehci_config *cfg = dev->config;
	struct ehci_priv *p = uhc_get_private(dev);

	irq_disable(cfg->irq);
	p->enabled = false;
	op_write(p, OP_USBINTR, 0);
	op_write(p, OP_USBCMD, op_read(p, OP_USBCMD) & ~(USBCMD_RS | USBCMD_ASE | USBCMD_PSE));
	return 0;
}

static int ehci_shutdown(const struct device *dev)
{
	return ehci_disable(dev);
}

static int ehci_bus_reset(const struct device *dev)
{
	struct ehci_priv *p = uhc_get_private(dev);
	enum usb_device_speed speed;
	int ret;

	ret = port_reset(p, &speed);
	op_write(p, OP_PORTSC1, op_read(p, OP_PORTSC1) & ~PORTSC_PHCD);	/* ack changes */
	if (ret == 0) {
		uhc_submit_event(dev, UHC_EVT_RESETED, 0);
	}
	return ret;
}

static int ehci_sof_enable(const struct device *dev)
{
	return 0;	/* the schedule runs whenever the controller does */
}

static int ehci_bus_suspend(const struct device *dev)
{
	return -ENOTSUP;
}

static int ehci_bus_resume(const struct device *dev)
{
	return -ENOTSUP;
}

static int ehci_ep_enqueue(const struct device *dev, struct uhc_transfer *const xfer)
{
	struct ehci_priv *p = uhc_get_private(dev);
	struct ehci_ep *ep;

	if (xfer->type == USB_EP_TYPE_ISO) {
		return -ENOTSUP;
	}
	if (!p->connected) {
		return -ENODEV;
	}

	/* Default address on endpoint 0: a (re)enumerating device, drop its stale endpoints */
	if (USB_EP_GET_IDX(xfer->ep) == 0 && xfer->udev->addr == 0) {
		for (int i = 0; i < EHCI_NUM_EP; i++) {
			struct ehci_ep *o = &p->eps[i];

			if (o->used && o->udev == xfer->udev && o->ep != 0 && ep_idle(o)) {
				ep_release(p, o);
			}
		}
	}

	ep = ep_get(p, xfer);
	if (!ep) {
		LOG_ERR("no free endpoint for ep 0x%02x", xfer->ep);
		return -ENOMEM;
	}
	if (ep->type != xfer->type) {
		return -EINVAL;
	}
	sys_dlist_append(&ep->queue, &xfer->node);
	k_sem_give(&p->work);
	return 0;
}

static int ehci_ep_dequeue(const struct device *dev, struct uhc_transfer *const xfer)
{
	struct ehci_priv *p = uhc_get_private(dev);
	struct ehci_ep *ep = ep_find(p, xfer);

	if (!ep) {
		return -ENOENT;
	}
	if (ep->active == xfer) {
		ep_unlink(p, ep);
		ep_free_qtds(p, ep);
		ep->qh->next = LINK_T;
		ep->qh->token &= QTD_DT;	/* inactive; keep the toggle */
		ep->active = NULL;
	}
	if (sys_dnode_is_linked(&xfer->node)) {
		uhc_xfer_return(dev, xfer, -ECONNRESET);
	}
	k_sem_give(&p->work);
	return 0;
}

static int ehci_driver_init(const struct device *dev)
{
	const struct ehci_config *cfg = dev->config;
	struct uhc_data *data = dev->data;
	struct ehci_priv *p = data->priv;

	if (cfg->pool_size < sizeof(struct ehci_pool) || (cfg->pool & 0xfff)) {
		LOG_ERR("DMA pool too small or not 4 KiB aligned");
		return -EINVAL;
	}

	/* No register access here: the controller is only touched once the host
	 * stack initializes it (the M7 may not own it, e.g. with an older SM config).
	 */
	p->dev = dev;
	p->pool = (struct ehci_pool *)cfg->pool;
	p->bus_off = cfg->pool_bus - (uint32_t)cfg->pool;
	k_sem_init(&p->work, 0, 1);
	k_sem_init(&p->iaa, 0, 1);
	k_mutex_init(&data->mutex);
	data->caps.hs = 1;

	cfg->irq_config();
	k_thread_create(&p->thread, p->stack, K_KERNEL_STACK_SIZEOF(p->stack), ehci_thread, p,
			NULL, NULL, K_PRIO_COOP(CONFIG_UHC_CI_EHCI_THREAD_PRIORITY), 0, K_NO_WAIT);
	k_thread_name_set(&p->thread, dev->name);
	return 0;
}

static DEVICE_API(uhc, ehci_api) = {
	.lock = ehci_lock,
	.unlock = ehci_unlock,
	.init = ehci_init,
	.enable = ehci_enable,
	.disable = ehci_disable,
	.shutdown = ehci_shutdown,
	.bus_reset = ehci_bus_reset,
	.sof_enable = ehci_sof_enable,
	.bus_suspend = ehci_bus_suspend,
	.bus_resume = ehci_bus_resume,
	.ep_enqueue = ehci_ep_enqueue,
	.ep_dequeue = ehci_ep_dequeue,
};

#define EHCI_DEFINE(n)                                                                             \
	static void ehci_irq_config_##n(void)                                                      \
	{                                                                                          \
		IRQ_CONNECT(DT_INST_IRQN(n), DT_INST_IRQ(n, priority), ehci_isr,                   \
			    DEVICE_DT_INST_GET(n), 0);                                             \
	}                                                                                          \
	static const struct ehci_config ehci_config_##n = {                                        \
		.base = DT_INST_REG_ADDR_BY_IDX(n, 0),                                             \
		.usbnc = DT_INST_REG_ADDR_BY_IDX(n, 1),                                            \
		.pool = DT_INST_PROP_BY_IDX(n, devenv_pool, 0),                                    \
		.pool_size = DT_INST_PROP_BY_IDX(n, devenv_pool, 1),                               \
		.pool_bus = DT_INST_PROP(n, devenv_pool_bus_addr),                                 \
		.disable_oc = DT_INST_PROP(n, disable_over_current),                               \
		.irq_config = ehci_irq_config_##n,                                                 \
		.irq = DT_INST_IRQN(n),                                                            \
	};                                                                                         \
	static struct ehci_priv ehci_priv_##n;                                                     \
	static struct uhc_data ehci_data_##n = {                                                   \
		.priv = &ehci_priv_##n,                                                            \
	};                                                                                         \
	DEVICE_DT_INST_DEFINE(n, ehci_driver_init, NULL, &ehci_data_##n, &ehci_config_##n,         \
			      POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEVICE, &ehci_api);

DT_INST_FOREACH_STATUS_OKAY(EHCI_DEFINE)
