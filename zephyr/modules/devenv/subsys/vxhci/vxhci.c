/*
 * Virtual xHCI host controller ("Wimmo") emulated on the M7.
 *
 * A standard xHCI driver on the host (Linux xhci-plat today, xhci-pci on a
 * PCIe endpoint later) drives a register page that is plain memory. Nothing
 * traps host accesses, so a thread polls the page and infers writes:
 *
 *  - Doorbells: every doorbell reads back as 0xffffffff while idle. Any
 *    other value is a host write (the command doorbell writes 0). The thread
 *    puts the sentinel back first and then walks the ring, so a doorbell
 *    written in between still finds its TRBs.
 *  - USBCMD, CRCR, ERSTBA, ERDP, IMAN: compared with what was last published
 *    or loaded.
 *  - PORTSC: reserved bit 2 is kept set in every published value. The host
 *    writes back only read-only and RWS bits (xhci_port_state_to_neutral)
 *    plus the bits it acts on, never bit 2, so a value without it is a host
 *    write, even one that rewrites the same bits.
 *  - Interrupt acknowledge: the host clears USBSTS.EINT and IMAN.IP by
 *    writing back the value it read, which cannot be seen. Instead the
 *    interrupt stays pending until the host's event ring dequeue pointer
 *    (ERDP) has caught up with every event posted, then EINT/IP clear and
 *    the line drops. IMOD spaces interrupts as the host asked.
 *
 * Rings and contexts live in host memory: the command ring, one event ring
 * (one interrupter, ERST of up to 16 segments), the device context base
 * array and the transfer rings. Commands: Enable/Disable Slot, Address
 * Device (the controller assigns the USB address: the slot id), Configure
 * Endpoint, Evaluate Context, Reset/Stop Endpoint, Set TR Dequeue Pointer,
 * Reset Device, No Op. Transfers: Normal, Setup/Data/Status, Link, Event
 * Data, No Op TRBs; short packets end the TD. Streams (for UAS): linear
 * primary stream arrays of up to 64 entries; a stream the device cannot serve
 * yet is parked until the device makes progress. No isochronous.
 *
 * Ports: 1 = USB 3 (SuperSpeed), 2 = USB 2, each described by a Supported
 * Protocol capability. Devices implement struct vxhci_device_ops.
 *
 * Copyright (c) 2026, imx95-devenv contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/barrier.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>
#include <zephyr/timing/timing.h>

#include <vxhci/vxhci.h>

LOG_MODULE_REGISTER(vxhci, CONFIG_VXHCI_LOG_LEVEL);

/* --- Register page layout --- */

#define CAPLENGTH		0x40
#define RTSOFF			0x600
#define DBOFF			0x700
#define XECP			0x800

#define CAP_VERSION		0x00	/* CAPLENGTH | HCIVERSION */
#define CAP_HCSPARAMS1		0x04
#define CAP_HCSPARAMS2		0x08
#define CAP_HCSPARAMS3		0x0c
#define CAP_HCCPARAMS1		0x10
#define CAP_DBOFF		0x14
#define CAP_RTSOFF		0x18
#define CAP_HCCPARAMS2		0x1c

#define OP(x)			(CAPLENGTH + (x))
#define R_USBCMD		OP(0x00)
#define R_USBSTS		OP(0x04)
#define R_PAGESIZE		OP(0x08)
#define R_DNCTRL		OP(0x14)
#define R_CRCR_LO		OP(0x18)
#define R_CRCR_HI		OP(0x1c)
#define R_DCBAAP_LO		OP(0x30)
#define R_DCBAAP_HI		OP(0x34)
#define R_CONFIG		OP(0x38)
#define R_PORTSC(p)		OP(0x400 + 0x10 * ((p) - 1))
#define R_PORTPMSC(p)		(R_PORTSC(p) + 4)
#define R_PORTLI(p)		(R_PORTSC(p) + 8)
#define R_PORTHLPMC(p)		(R_PORTSC(p) + 12)

#define R_MFINDEX		RTSOFF
#define IR0			(RTSOFF + 0x20)
#define R_IMAN			(IR0 + 0x00)
#define R_IMOD			(IR0 + 0x04)
#define R_ERSTSZ		(IR0 + 0x08)
#define R_ERSTBA_LO		(IR0 + 0x10)
#define R_ERSTBA_HI		(IR0 + 0x14)
#define R_ERDP_LO		(IR0 + 0x18)
#define R_ERDP_HI		(IR0 + 0x1c)

#define R_DB(s)			(DBOFF + 4 * (s))
#define DB_IDLE			0xffffffffU

#define MAX_SLOTS		8
#define NUM_PORTS		2
#define PORT_USB3		1
#define PORT_USB2		2
#define MAX_ERST		16

/* USBCMD / USBSTS */
#define CMD_RS			BIT(0)
#define CMD_HCRST		BIT(1)
#define CMD_INTE		BIT(2)
#define CMD_CSS			BIT(8)
#define CMD_CRS			BIT(9)
#define STS_HCH			BIT(0)
#define STS_EINT		BIT(3)
#define STS_PCD			BIT(4)

/* CRCR */
#define CRCR_RCS		BIT(0)
#define CRCR_CS			BIT(1)
#define CRCR_CA			BIT(2)
#define CRCR_CRR		BIT(3)

/* IMAN */
#define IMAN_IP			BIT(0)
#define IMAN_IE			BIT(1)

/* PORTSC */
#define PS_CCS			BIT(0)
#define PS_PED			BIT(1)
#define PS_CANARY		BIT(2)	/* RsvdZ: published set, host writes 0 */
#define PS_PR			BIT(4)
#define PS_PLS_SHIFT		5
#define PS_PLS_MASK		(0xfU << PS_PLS_SHIFT)
#define PS_PP			BIT(9)
#define PS_SPEED_SHIFT		10
#define PS_PIC_MASK		(3U << 14)
#define PS_LWS			BIT(16)
#define PS_CSC			BIT(17)
#define PS_PEC			BIT(18)
#define PS_WRC			BIT(19)
#define PS_OCC			BIT(20)
#define PS_PRC			BIT(21)
#define PS_PLC			BIT(22)
#define PS_CEC			BIT(23)
#define PS_CHANGE		(0x7fU << 17)
#define PS_WAKE_MASK		(7U << 25)
#define PS_WPR			BIT(31)

#define PLS_U0			0
#define PLS_U3			3
#define PLS_DISABLED		4
#define PLS_RXDETECT		5
#define PLS_POLLING		7
#define PLS_RESUME		15

/* TRBs */
#define TRB_CYCLE		BIT(0)
#define TRB_ENT_TC		BIT(1)	/* Link: toggle cycle */
#define TRB_ISP			BIT(2)
#define TRB_CH			BIT(4)
#define TRB_IOC			BIT(5)
#define TRB_IDT			BIT(6)
#define TRB_BSR			BIT(9)	/* Address Device: block SET_ADDRESS */
#define TRB_DC			BIT(9)	/* Configure Endpoint: deconfigure */
#define TRB_BEI			BIT(9)	/* transfer TRBs: block event interrupt */
#define TRB_TYPE(c)		(((c) >> 10) & 0x3f)
#define TRB_SLOT(c)		((c) >> 24)
#define TRB_EP(c)		(((c) >> 16) & 0x1f)
#define TRB_LEN(s)		((s) & 0x1ffff)

enum {
	T_NORMAL = 1, T_SETUP, T_DATA, T_STATUS, T_ISOCH, T_LINK, T_EVENT_DATA, T_NOOP,
	T_ENABLE_SLOT, T_DISABLE_SLOT, T_ADDRESS_DEV, T_CONFIG_EP, T_EVAL_CTX, T_RESET_EP,
	T_STOP_EP, T_SET_DEQ, T_RESET_DEV, T_FORCE_EVENT, T_NEG_BW, T_SET_LT, T_GET_BW,
	T_FORCE_HEADER, T_NOOP_CMD,
	T_EV_TRANSFER = 32, T_EV_CMD, T_EV_PORT,
};

enum {
	CC_SUCCESS = 1, CC_TX_ERR = 4, CC_TRB_ERR = 5, CC_STALL = 6, CC_NO_SLOTS = 9,
	CC_SLOT_NOT_EN = 11, CC_EP_NOT_EN = 12, CC_SHORT = 13, CC_PARAM = 17,
	CC_CTX_STATE = 19, CC_CMD_RING_STOPPED = 24, CC_STOPPED_LEN_INVALID = 27,
};

/* Contexts (32-byte, HCCPARAMS1.CSZ = 0) */
#define CTX_SIZE		32
#define SLOT_STATE_ENABLED	0
#define SLOT_STATE_DEFAULT	1
#define SLOT_STATE_ADDRESSED	2
#define SLOT_STATE_CONFIGURED	3

#define EP_DISABLED		0
#define EP_RUNNING		1
#define EP_HALTED		2
#define EP_STOPPED		3

#define VENDOR_MAGIC0		0x6d6d6957	/* "Wimm" */
#define VENDOR_MAGIC1		0x0000006f	/* "o" */
#define VENDOR_VERSION		0x00010000
#define VENDOR_STATE_READY	1

#define TRBS_PER_RUN		64	/* per endpoint per poll */

/* A transfer ring: an endpoint's, or one stream's */
struct ring {
	uint64_t deq;
	bool ccs;
	bool td_active;		/* a TRB of the current TD was consumed */
	bool trb_valid;		/* trb[] holds the TRB at deq (retry after NAK) */
	uint32_t edtla;		/* event data transfer length accumulator */
	uint32_t trb[4];
};

#define MAX_STREAMS		64	/* HCCPARAMS1.MaxPSASize 5: 64-entry stream arrays */
#define STREAM_POOLS		4	/* endpoints with streams at once */

struct ep {
	uint8_t state;
	struct ring r;		/* without streams */
	/* streams (linear primary stream array; stream ID = index) */
	struct ring *streams;
	uint16_t nstreams;
	uint64_t sctx;		/* stream context array */
	uint64_t sloaded;	/* streams whose ring state was read from their context */
	uint64_t swork;		/* streams with a doorbell / TRBs to look at */
	uint64_t sparked;	/* streams the device cannot serve yet */
	uint32_t restart_seq;	/* Stopped: restartable once the host consumed this event */
	/* control transfers (endpoint 0) */
	uint8_t setup[8];
	bool ctl_done;
	int ctl_ret;
	uint16_t ctl_len;	/* IN: response length; OUT: data collected */
	uint16_t ctl_off;	/* IN: bytes handed out */
};

struct slot {
	bool enabled;
	uint8_t state;
	uint8_t port;
	uint8_t addr;
	bool progress;		/* a TRB completed in this poll */
	uint64_t out_ctx;
	uint32_t work;		/* endpoints (DCI bits) with a doorbell pending */
	uint32_t parked;	/* endpoints with parked streams */
	struct ep ep[32];	/* by DCI; 0 unused */
};

struct port {
	const struct vxhci_device_ops *ops;
	void *ctx;
	enum vxhci_speed speed;
	bool ccs;
	bool ped;
	uint8_t pls;
	uint32_t change;
	uint32_t rws;		/* PIC and wake enables, as the host wrote them */
	uint32_t published;
};

static struct {
	struct vxhci_transport t;
	struct k_mutex lock;
	bool adaptive;
	bool active;
	int64_t last_active;

	uint32_t cmd;		/* USBCMD as published */
	uint32_t sts;		/* USBSTS as published */
	bool running;

	/* command ring */
	uint64_t cr_deq;
	bool cr_ccs;
	bool crr;
	bool cr_work;
	uint32_t crcr_lo, crcr_hi;	/* as published */

	/* event ring (interrupter 0) */
	uint64_t erstba;	/* as loaded */
	uint64_t seg_base[MAX_ERST];
	uint16_t seg_size[MAX_ERST];
	uint8_t nseg;
	uint8_t seg;
	uint16_t idx;
	bool pcs;
	bool ev_valid;
	uint32_t ring_trbs;

	/* interrupter state */
	bool ip;		/* IMAN.IP / USBSTS.EINT set, line asserted (if enabled) */
	bool ev_pending;	/* events posted, not yet signalled */
	bool line;
	uint32_t last_irq_cyc;
	uint32_t last_sweep;
	uint32_t ev_seq;	/* events posted */

	struct port port[NUM_PORTS + 1];
	struct slot slot[MAX_SLOTS + 1];

	struct vxhci_stats st;
} x;

K_THREAD_STACK_DEFINE(vxhci_stack, CONFIG_VXHCI_THREAD_STACK_SIZE);
static struct k_thread vxhci_thread_data;
static uint8_t ctl_buf[VXHCI_CTRL_MAX] __aligned(4);
static struct ring stream_pool[STREAM_POOLS][MAX_STREAMS];
static bool pool_used[STREAM_POOLS];

static inline uint32_t rd(uint32_t off)
{
	return x.t.regs[off / 4];
}

static inline void wr(uint32_t off, uint32_t val)
{
	x.t.regs[off / 4] = val;
}

static inline uint32_t cyc32(void)
{
	return (uint32_t)timing_counter_get();
}

static bool hread(uint64_t addr, void *buf, size_t len)
{
	if (x.t.host_read(x.t.ctx, addr, buf, len)) {
		LOG_ERR("host read 0x%llx+%u failed", addr, (unsigned int)len);
		x.st.errors++;
		return false;
	}
	return true;
}

static bool hwrite(uint64_t addr, const void *buf, size_t len)
{
	if (x.t.host_write(x.t.ctx, addr, buf, len)) {
		LOG_ERR("host write 0x%llx+%u failed", addr, (unsigned int)len);
		x.st.errors++;
		return false;
	}
	return true;
}

int vxhci_data_read(const struct vxhci_data *d, uint32_t off, void *dst, uint32_t n)
{
	if (off + n > d->len) {
		return -EINVAL;
	}
	return x.t.host_read(x.t.ctx, d->host + off, dst, n);
}

int vxhci_data_write(const struct vxhci_data *d, uint32_t off, const void *src, uint32_t n)
{
	if (off + n > d->len) {
		return -EINVAL;
	}
	return x.t.host_write(x.t.ctx, d->host + off, src, n);
}

static const char *speed_str(enum vxhci_speed s)
{
	switch (s) {
	case VXHCI_SPEED_LOW: return "low-speed";
	case VXHCI_SPEED_FULL: return "full-speed";
	case VXHCI_SPEED_HIGH: return "high-speed";
	default: return "SuperSpeed";
	}
}

/* --- Event ring and interrupter --- */

static void er_load(void)
{
	uint32_t sz = rd(R_ERSTSZ) & 0xffff;
	uint32_t ent[4];

	x.ev_valid = false;
	x.nseg = 0;
	x.ring_trbs = 0;
	if (!x.erstba || sz == 0 || sz > MAX_ERST) {
		return;
	}
	for (uint32_t i = 0; i < sz; i++) {
		if (!hread(x.erstba + 16 * i, ent, sizeof(ent))) {
			return;
		}
		x.seg_base[i] = ((uint64_t)ent[1] << 32) | (ent[0] & ~0x3fU);
		x.seg_size[i] = ent[2] & 0xffff;
		if (x.seg_size[i] < 16) {
			LOG_ERR("ERST segment %u: %u TRBs", i, x.seg_size[i]);
			return;
		}
		x.ring_trbs += x.seg_size[i];
	}
	x.nseg = sz;
	x.seg = 0;
	x.idx = 0;
	x.pcs = true;
	x.ev_valid = true;
	LOG_DBG("event ring: %u segment(s), %u TRBs", x.nseg, x.ring_trbs);
}

/* Position of a host address in the event ring (0..ring_trbs-1), or the enqueue position */
static uint32_t er_pos(uint64_t addr)
{
	uint32_t base = 0;

	for (int i = 0; i < x.nseg; i++) {
		if (addr >= x.seg_base[i] && addr < x.seg_base[i] + 16U * x.seg_size[i]) {
			return base + (uint32_t)((addr - x.seg_base[i]) / 16);
		}
		base += x.seg_size[i];
	}
	return UINT32_MAX;
}

static uint32_t er_enq_pos(void)
{
	uint32_t base = 0;

	for (int i = 0; i < x.seg; i++) {
		base += x.seg_size[i];
	}
	return base + x.idx;
}

static uint64_t erdp(void)
{
	return (((uint64_t)rd(R_ERDP_HI) << 32) | rd(R_ERDP_LO)) & ~0xfULL;
}

/* Free event TRBs (one is always left empty: full ring = enqueue + 1 == dequeue) */
static uint32_t er_free(void)
{
	uint32_t d, e;

	if (!x.ev_valid) {
		return 0;
	}
	d = er_pos(erdp());
	if (d == UINT32_MAX) {
		d = 0;	/* not written yet: the ring starts empty */
	}
	e = er_enq_pos();
	return (d + x.ring_trbs - e - 1) % x.ring_trbs;
}

static bool er_caught_up(void)
{
	uint32_t d = er_pos(erdp());

	return d == UINT32_MAX ? er_enq_pos() == 0 : d == er_enq_pos();
}

/* Events the host has consumed (ERDP passed), as a running count */
static uint32_t ev_consumed(void)
{
	uint32_t d = er_pos(erdp());

	if (!x.ev_valid || d == UINT32_MAX) {
		return x.ev_seq;
	}
	return x.ev_seq - (er_enq_pos() + x.ring_trbs - d) % x.ring_trbs;
}

static bool post_event(uint32_t p0, uint32_t p1, uint32_t status, uint32_t ctrl, bool bei)
{
	uint32_t ev[3] = {p0, p1, status};
	uint64_t a;

	if (er_free() == 0) {
		LOG_WRN("event ring full, event dropped");
		x.st.errors++;
		return false;
	}
	a = x.seg_base[x.seg] + 16U * x.idx;
	ctrl = (ctrl & ~TRB_CYCLE) | (x.pcs ? TRB_CYCLE : 0);
	hwrite(a, ev, sizeof(ev));
	barrier_dmem_fence_full();
	hwrite(a + 12, &ctrl, 4);	/* cycle bit last: hands the TRB to the host */
	barrier_dmem_fence_full();
	if (++x.idx == x.seg_size[x.seg]) {
		x.idx = 0;
		if (++x.seg == x.nseg) {
			x.seg = 0;
			x.pcs = !x.pcs;
		}
	}
	x.st.events++;
	x.ev_seq++;
	if (!bei) {
		x.ev_pending = true;
	}
	return true;
}

static void cmd_event(uint64_t trb_addr, uint8_t cc, uint8_t slot)
{
	post_event((uint32_t)trb_addr, (uint32_t)(trb_addr >> 32), (uint32_t)cc << 24,
		   ((uint32_t)slot << 24) | (T_EV_CMD << 10), false);
}

static void xfer_event(uint8_t slot, uint8_t dci, uint64_t ptr, uint8_t cc, uint32_t residue,
		       bool ed, bool bei)
{
	post_event((uint32_t)ptr, (uint32_t)(ptr >> 32), ((uint32_t)cc << 24) | (residue & 0xffffff),
		   ((uint32_t)slot << 24) | ((uint32_t)dci << 16) | (T_EV_TRANSFER << 10) |
		   (ed ? BIT(2) : 0), bei);
}

static void sts_publish(void)
{
	x.sts = (x.running ? 0 : STS_HCH) | (x.ip ? STS_EINT : 0) |
		(x.ip && (x.sts & STS_PCD) ? STS_PCD : 0);
	wr(R_USBSTS, x.sts);
}

static void line_set(bool on)
{
	if (on != x.line) {
		x.line = on;
		x.t.irq_set(x.t.ctx, on);
		if (on) {
			x.st.irqs++;
		}
	}
}

/*
 * Interrupt: pending once the host consumed nothing of what was posted since
 * the last interrupt. Cleared when ERDP catches up with the enqueue pointer.
 */
static void irq_update(void)
{
	uint32_t iman = rd(R_IMAN);

	if (x.ip && er_caught_up()) {
		x.ip = false;
		x.sts &= ~(STS_EINT | STS_PCD);
		wr(R_IMAN, iman & IMAN_IE);
		sts_publish();
		x.last_irq_cyc = cyc32();
		x.active = true;
	}
	if (!x.ip && x.ev_pending && (iman & IMAN_IE) && (x.cmd & CMD_INTE) && !er_caught_up()) {
		/* IMODI: 250 ns units = 200 core cycles at 800 MHz */
		uint32_t imodi = rd(R_IMOD) & 0xffff;

		if (cyc32() - x.last_irq_cyc >= imodi * 200U) {
			x.ip = true;
			x.ev_pending = false;
			wr(R_IMAN, IMAN_IE | IMAN_IP);
			x.sts |= STS_EINT;
			sts_publish();
		}
	} else if (x.ev_pending && er_caught_up()) {
		x.ev_pending = false;	/* the host polled them away */
	}
	line_set(x.ip && (iman & IMAN_IE) && (x.cmd & CMD_INTE));
}

/* --- Ports --- */

static uint32_t portsc(int i)
{
	struct port *p = &x.port[i];

	return (p->ccs ? PS_CCS : 0) | (p->ped ? PS_PED : 0) | PS_CANARY |
	       ((uint32_t)p->pls << PS_PLS_SHIFT) | PS_PP |
	       (p->ccs ? (uint32_t)p->speed << PS_SPEED_SHIFT : 0) | p->rws | p->change;
}

static void port_publish(int i, uint32_t old_change)
{
	struct port *p = &x.port[i];

	p->published = portsc(i);
	wr(R_PORTSC(i), p->published);
	if ((p->change & ~old_change) && x.running) {
		/* a change bit went 0 -> 1: Port Status Change Event */
		post_event((uint32_t)i << 24, 0, (uint32_t)CC_SUCCESS << 24, T_EV_PORT << 10,
			   false);
		x.sts |= STS_PCD;
	}
	x.active = true;
}

/* Link trained (USB 3) or reset done (USB 2): enabled, U0 */
static void port_enable(int i)
{
	struct port *p = &x.port[i];

	p->ped = p->ccs;
	p->pls = p->ccs ? PLS_U0 : PLS_RXDETECT;
}

static void port_connect_state(int i)
{
	struct port *p = &x.port[i];

	p->ccs = p->ops != NULL;
	p->ped = false;
	if (!p->ccs) {
		p->pls = PLS_RXDETECT;
	} else if (i == PORT_USB3) {
		port_enable(i);	/* USB 3 ports enable themselves after link training */
	} else {
		p->pls = PLS_POLLING;
	}
}

static void port_reset(int i, bool warm)
{
	struct port *p = &x.port[i];
	uint32_t old = p->change;

	if (p->ops) {
		p->ops->reset(p->ctx);
	}
	port_enable(i);
	p->change |= PS_PRC | (warm ? PS_WRC : 0);
	LOG_DBG("port %d %sreset: %s", i, warm ? "warm " : "", p->ped ? "enabled" : "empty");
	port_publish(i, old);
}

static void port_write(int i, uint32_t w)
{
	struct port *p = &x.port[i];
	uint32_t old = p->change;

	p->change &= ~(w & PS_CHANGE);
	if ((w & PS_PED) && p->ped) {
		p->ped = false;	/* software disable */
		p->pls = (i == PORT_USB3) ? PLS_DISABLED : p->pls;
	}
	p->rws = w & (PS_PIC_MASK | PS_WAKE_MASK);
	if (w & PS_LWS) {
		uint8_t pls = (w & PS_PLS_MASK) >> PS_PLS_SHIFT;

		if (pls == PLS_U3 && p->ped) {
			p->pls = PLS_U3;
		} else if (pls == PLS_RESUME && p->ped) {
			p->pls = PLS_RESUME;
		} else if (pls == PLS_U0 && p->ped && p->pls != PLS_U0) {
			p->pls = PLS_U0;
			p->change |= PS_PLC;
		} else if (pls == PLS_DISABLED && i == PORT_USB3) {
			p->ped = false;
			p->pls = PLS_DISABLED;
		} else if (pls == PLS_RXDETECT && i == PORT_USB3) {
			port_connect_state(i);
			if (p->ccs) {
				p->change |= PS_CSC;
			}
		}
	}
	if (w & (PS_PR | PS_WPR)) {
		port_publish(i, old);
		port_reset(i, (w & PS_WPR) && i == PORT_USB3);
		return;
	}
	port_publish(i, old);
}

static void ports_check(void)
{
	for (int i = 1; i <= NUM_PORTS; i++) {
		uint32_t v = rd(R_PORTSC(i));

		if (v != x.port[i].published) {
			port_write(i, v);
		}
	}
}

/* --- Slots and contexts --- */

static void ep_ctx_state(struct slot *s, int dci)
{
	struct ep *e = &s->ep[dci];
	uint32_t dw0;
	uint64_t a = s->out_ctx + CTX_SIZE * dci;

	if (!s->out_ctx || !hread(a, &dw0, 4)) {
		return;
	}
	dw0 = (dw0 & ~7U) | e->state;
	hwrite(a, &dw0, 4);
}

/* TR dequeue pointer of a ring: in the endpoint context, or in its stream context */
static void ring_ctx_write(struct slot *s, int dci, int sid)
{
	struct ep *e = &s->ep[dci];
	struct ring *r = e->streams ? &e->streams[sid] : &e->r;
	uint32_t dq[2] = {(uint32_t)r->deq | (r->ccs ? 1 : 0), (uint32_t)(r->deq >> 32)};

	if (e->streams) {
		dq[0] |= 1U << 1;	/* SCT: primary transfer ring */
		hwrite(e->sctx + 16U * sid, dq, sizeof(dq));
	} else if (s->out_ctx) {
		hwrite(s->out_ctx + CTX_SIZE * dci + 8, dq, sizeof(dq));
	}
}

/* Output context: state and dequeue pointer(s) */
static void ep_ctx_deq(struct slot *s, int dci)
{
	struct ep *e = &s->ep[dci];

	ep_ctx_state(s, dci);
	if (!e->streams) {
		ring_ctx_write(s, dci, 0);
		return;
	}
	for (uint64_t m = e->sloaded; m; m &= m - 1) {
		ring_ctx_write(s, dci, __builtin_ctzll(m));
	}
}

static void ep_release(struct ep *e)
{
	if (e->streams) {
		pool_used[(e->streams - stream_pool[0]) / MAX_STREAMS] = false;
		e->streams = NULL;
	}
}

static void ep_disable(struct slot *s, int dci)
{
	struct ep *e = &s->ep[dci];

	ep_release(e);
	memset(e, 0, sizeof(*e));
	s->work &= ~BIT(dci);
	s->parked &= ~BIT(dci);
}

static void ring_reset(struct ring *r)
{
	r->td_active = false;
	r->trb_valid = false;
	r->edtla = 0;
}

/* From an input endpoint context. Returns false if no stream array is free. */
static bool ep_init(struct ep *e, const uint32_t *ctx)
{
	uint8_t maxp = (ctx[0] >> 10) & 0x1f;	/* MaxPStreams */
	uint64_t deq = (((uint64_t)ctx[3] << 32) | ctx[2]) & ~0xfULL;

	ep_release(e);
	memset(e, 0, sizeof(*e));
	if (maxp) {
		int i;

		for (i = 0; i < STREAM_POOLS && pool_used[i]; i++) {
		}
		if (i == STREAM_POOLS || !(ctx[0] & BIT(15))) {
			LOG_ERR("no stream array free (or secondary arrays requested)");
			return false;
		}
		pool_used[i] = true;
		e->streams = stream_pool[i];
		e->nstreams = MIN(1U << (maxp + 1), MAX_STREAMS);
		e->sctx = deq;
	} else {
		e->r.deq = deq;
		e->r.ccs = ctx[2] & 1;
	}
	e->state = EP_RUNNING;
	return true;
}

/* A stream's ring, its state read from the stream context on first use */
static struct ring *stream_get(struct ep *e, uint16_t sid)
{
	struct ring *r;
	uint32_t sc[2];

	if (sid == 0 || sid >= e->nstreams) {
		return NULL;
	}
	r = &e->streams[sid];
	if (!(e->sloaded & BIT64(sid))) {
		if (!hread(e->sctx + 16U * sid, sc, sizeof(sc))) {
			return NULL;
		}
		memset(r, 0, sizeof(*r));
		r->deq = (((uint64_t)sc[1] << 32) | sc[0]) & ~0xfULL;
		r->ccs = sc[0] & 1;
		e->sloaded |= BIT64(sid);
	}
	return r;
}

static struct slot *slot_get(uint8_t id)
{
	return (id >= 1 && id <= MAX_SLOTS && x.slot[id].enabled) ? &x.slot[id] : NULL;
}

static void slot_ctx_write(struct slot *s, uint32_t *sc)
{
	sc[3] = ((uint32_t)s->state << 27) | (s->state >= SLOT_STATE_ADDRESSED ? s->addr : 0);
	hwrite(s->out_ctx, sc, CTX_SIZE);
}

static bool slot_has_eps(struct slot *s)
{
	for (int i = 2; i < 32; i++) {
		if (s->ep[i].state != EP_DISABLED) {
			return true;
		}
	}
	return false;
}

static uint8_t cmd_address_device(struct slot *s, uint8_t id, uint64_t in, bool bsr)
{
	uint32_t icc[2], sc[8], ep0[8];
	uint64_t dcbaap = ((uint64_t)rd(R_DCBAAP_HI) << 32) | (rd(R_DCBAAP_LO) & ~0x3fU);
	uint32_t ptr[2];
	uint8_t port;

	if (s->state > SLOT_STATE_DEFAULT) {
		return CC_CTX_STATE;
	}
	if (!hread(in, icc, sizeof(icc)) || !hread(in + CTX_SIZE, sc, sizeof(sc)) ||
	    !hread(in + 2 * CTX_SIZE, ep0, sizeof(ep0)) || !hread(dcbaap + 8U * id, ptr, 8)) {
		return CC_TRB_ERR;
	}
	if ((icc[1] & 3) != 3) {
		return CC_PARAM;
	}
	port = (sc[1] >> 16) & 0xff;
	if (port < 1 || port > NUM_PORTS || !x.port[port].ped) {
		return CC_TX_ERR;	/* no device answers there */
	}
	s->out_ctx = ((uint64_t)ptr[1] << 32) | (ptr[0] & ~0x3fU);
	s->port = port;
	s->addr = id;
	ep_init(&s->ep[1], ep0);
	s->state = bsr ? SLOT_STATE_DEFAULT : SLOT_STATE_ADDRESSED;

	ep0[0] = (ep0[0] & ~7U) | EP_RUNNING;
	hwrite(s->out_ctx + CTX_SIZE, ep0, sizeof(ep0));
	slot_ctx_write(s, sc);
	if (!bsr) {
		LOG_INF("slot %u: %s device on port %u, address %u", id,
			speed_str(x.port[port].speed), port, s->addr);
	}
	return CC_SUCCESS;
}

static uint8_t cmd_configure_ep(struct slot *s, uint8_t id, uint64_t in, bool dc)
{
	uint32_t icc[2], sc[8], ec[8];

	if (s->state < SLOT_STATE_ADDRESSED) {
		return CC_CTX_STATE;
	}
	if (dc) {
		for (int i = 2; i < 32; i++) {
			if (s->ep[i].state != EP_DISABLED) {
				ep_disable(s, i);
				ep_ctx_state(s, i);
			}
		}
		s->state = SLOT_STATE_ADDRESSED;
		if (hread(s->out_ctx, sc, sizeof(sc))) {
			sc[0] = (sc[0] & ~(0x1fU << 27)) | (1U << 27);
			slot_ctx_write(s, sc);
		}
		return CC_SUCCESS;
	}
	if (!hread(in, icc, sizeof(icc))) {
		return CC_TRB_ERR;
	}
	for (int i = 2; i < 32; i++) {
		if (icc[0] & BIT(i)) {
			ep_disable(s, i);
			ep_ctx_state(s, i);
		}
		if (icc[1] & BIT(i)) {
			if (!hread(in + CTX_SIZE * (i + 1), ec, sizeof(ec))) {
				return CC_TRB_ERR;
			}
			s->work &= ~BIT(i);
			s->parked &= ~BIT(i);
			if (!ep_init(&s->ep[i], ec)) {
				return 7;	/* Resource Error */
			}
			ec[0] = (ec[0] & ~7U) | EP_RUNNING;
			hwrite(s->out_ctx + CTX_SIZE * i, ec, sizeof(ec));
		}
	}
	if (!hread((icc[1] & 1) ? in + CTX_SIZE : s->out_ctx, sc, sizeof(sc))) {
		return CC_TRB_ERR;
	}
	if (s->state == SLOT_STATE_ADDRESSED && slot_has_eps(s)) {
		LOG_INF("slot %u: configured", id);
	}
	s->state = slot_has_eps(s) ? SLOT_STATE_CONFIGURED : SLOT_STATE_ADDRESSED;
	slot_ctx_write(s, sc);
	return CC_SUCCESS;
}

static uint8_t cmd_evaluate_ctx(struct slot *s, uint64_t in)
{
	uint32_t icc[2], isc[8], osc[8], iep[8], oep[8];

	if (!hread(in, icc, sizeof(icc)) || !hread(s->out_ctx, osc, sizeof(osc))) {
		return CC_TRB_ERR;
	}
	if (icc[1] & BIT(0)) {
		if (!hread(in + CTX_SIZE, isc, sizeof(isc))) {
			return CC_TRB_ERR;
		}
		osc[1] = (osc[1] & ~0xffffU) | (isc[1] & 0xffff);		/* max exit latency */
		osc[2] = (osc[2] & ~(0x3ffU << 22)) | (isc[2] & (0x3ffU << 22));	/* interrupter */
		slot_ctx_write(s, osc);
	}
	if (icc[1] & BIT(1)) {
		if (!hread(in + 2 * CTX_SIZE, iep, sizeof(iep)) ||
		    !hread(s->out_ctx + CTX_SIZE, oep, sizeof(oep))) {
			return CC_TRB_ERR;
		}
		oep[1] = (oep[1] & 0xffff) | (iep[1] & 0xffff0000U);	/* max packet size */
		hwrite(s->out_ctx + CTX_SIZE, oep, sizeof(oep));
	}
	return CC_SUCCESS;
}

static uint8_t cmd_reset_device(struct slot *s)
{
	uint32_t sc[8];

	if (s->state < SLOT_STATE_DEFAULT) {
		return CC_CTX_STATE;
	}
	for (int i = 2; i < 32; i++) {
		if (s->ep[i].state != EP_DISABLED) {
			ep_disable(s, i);
			ep_ctx_state(s, i);
		}
	}
	s->state = SLOT_STATE_DEFAULT;
	if (!hread(s->out_ctx, sc, sizeof(sc))) {
		return CC_TRB_ERR;
	}
	sc[0] = (sc[0] & ~(0x1fU << 27)) | (1U << 27);
	slot_ctx_write(s, sc);
	return CC_SUCCESS;
}

static uint8_t do_command(const uint32_t *trb, uint8_t *slot_out)
{
	uint8_t id = TRB_SLOT(trb[3]);
	uint8_t dci = TRB_EP(trb[3]);
	uint64_t param = ((uint64_t)trb[1] << 32) | trb[0];
	struct slot *s;
	struct ep *e;

	*slot_out = id;
	switch (TRB_TYPE(trb[3])) {
	case T_ENABLE_SLOT:
		for (int i = 1; i <= MAX_SLOTS; i++) {
			if (!x.slot[i].enabled) {
				memset(&x.slot[i], 0, sizeof(x.slot[i]));
				x.slot[i].enabled = true;
				*slot_out = i;
				return CC_SUCCESS;
			}
		}
		*slot_out = 0;
		return CC_NO_SLOTS;
	case T_NOOP_CMD:
		*slot_out = 0;
		return CC_SUCCESS;
	default:
		break;
	}

	s = slot_get(id);
	if (!s) {
		return CC_SLOT_NOT_EN;
	}
	switch (TRB_TYPE(trb[3])) {
	case T_DISABLE_SLOT:
		for (int i = 1; i < 32; i++) {
			ep_release(&s->ep[i]);
		}
		memset(s, 0, sizeof(*s));
		return CC_SUCCESS;
	case T_ADDRESS_DEV:
		return cmd_address_device(s, id, param & ~0xfULL, trb[3] & TRB_BSR);
	case T_CONFIG_EP:
		return cmd_configure_ep(s, id, param & ~0xfULL, trb[3] & TRB_DC);
	case T_EVAL_CTX:
		return cmd_evaluate_ctx(s, param & ~0xfULL);
	case T_RESET_DEV:
		return cmd_reset_device(s);
	default:
		break;
	}

	if (dci < 1 || s->ep[dci].state == EP_DISABLED) {
		return CC_EP_NOT_EN;
	}
	e = &s->ep[dci];
	switch (TRB_TYPE(trb[3])) {
	case T_RESET_EP:
		if (e->state != EP_HALTED) {
			return CC_CTX_STATE;
		}
		e->state = EP_STOPPED;
		ring_reset(&e->r);
		for (int i = 0; e->streams && i < e->nstreams; i++) {
			ring_reset(&e->streams[i]);
		}
		ep_ctx_state(s, dci);
		e->restart_seq = x.ev_seq + 1;
		return CC_SUCCESS;
	case T_STOP_EP:
		if (e->state == EP_HALTED) {
			return CC_CTX_STATE;
		}
		if (e->state == EP_RUNNING) {
			/* stopped inside a TD: the host counts the TRBs before this one */
			for (int i = 0; i < (e->streams ? e->nstreams : 1); i++) {
				struct ring *r = e->streams ? &e->streams[i] : &e->r;

				if (r->td_active) {
					xfer_event(id, dci, r->deq, CC_STOPPED_LEN_INVALID, 0, false,
						   false);
				}
			}
		}
		e->state = EP_STOPPED;
		ring_reset(&e->r);
		for (int i = 0; e->streams && i < e->nstreams; i++) {
			ring_reset(&e->streams[i]);
		}
		e->swork = 0;
		e->sparked = 0;
		s->work &= ~BIT(dci);
		s->parked &= ~BIT(dci);
		ep_ctx_deq(s, dci);
		e->restart_seq = x.ev_seq + 1;	/* our completion event */
		x.st.stops++;
		return CC_SUCCESS;
	case T_SET_DEQ: {
		uint16_t sid = trb[2] >> 16;
		struct ring *r = e->streams ? stream_get(e, sid) : &e->r;

		if (e->state != EP_STOPPED) {
			return CC_CTX_STATE;
		}
		if (!r) {
			return 10;	/* Invalid Stream ID Error */
		}
		r->deq = param & ~0xfULL;
		r->ccs = param & 1;
		ring_reset(r);
		ring_ctx_write(s, dci, sid);
		e->restart_seq = x.ev_seq + 1;
		return CC_SUCCESS;
	}
	default:
		return CC_TRB_ERR;
	}
}

static void cmd_ring_run(void)
{
	uint32_t trb[4];

	while (x.crr && x.running) {
		if (er_free() < 2) {
			return;	/* wait for the host to make room */
		}
		if (!hread(x.cr_deq, trb, sizeof(trb))) {
			x.cr_work = false;
			return;
		}
		if (!!(trb[3] & TRB_CYCLE) != x.cr_ccs) {
			x.cr_work = false;
			return;
		}
		if (TRB_TYPE(trb[3]) == T_LINK) {
			if (trb[3] & TRB_ENT_TC) {
				x.cr_ccs = !x.cr_ccs;
			}
			x.cr_deq = (((uint64_t)trb[1] << 32) | trb[0]) & ~0xfULL;
			continue;
		}

		uint8_t slot, cc = do_command(trb, &slot);

		LOG_DBG("command %u slot %u -> %u", TRB_TYPE(trb[3]), slot, cc);
		cmd_event(x.cr_deq, cc, slot);
		x.cr_deq += 16;
		x.st.commands++;
		x.active = true;
	}
}

static void crcr_check(void)
{
	uint32_t lo = rd(R_CRCR_LO), hi = rd(R_CRCR_HI);

	if (lo == x.crcr_lo && hi == x.crcr_hi) {
		return;
	}
	if (x.crr && (lo & (CRCR_CA | CRCR_CS))) {
		/* abort/stop: commands complete at once, so nothing is in progress */
		x.crr = false;
		x.cr_work = false;
		cmd_event(x.cr_deq, CC_CMD_RING_STOPPED, 0);
	} else if (!x.crr) {
		if (lo != x.crcr_lo) {
			x.cr_deq = (x.cr_deq & ~0xffffffffULL) | (lo & ~0x3fU);
			x.cr_ccs = lo & CRCR_RCS;
		}
		if (hi != x.crcr_hi) {
			x.cr_deq = (x.cr_deq & 0xffffffffULL) | ((uint64_t)hi << 32);
		}
	}
	x.crcr_lo = x.crr ? CRCR_CRR : 0;
	x.crcr_hi = 0;
	wr(R_CRCR_LO, x.crcr_lo);
	wr(R_CRCR_HI, 0);
	x.active = true;
}

/* --- Transfers --- */

enum { TRB_DONE, TRB_WAIT, TRB_HALT };
enum { RUN_EMPTY, RUN_WAIT, RUN_HALT, RUN_MORE };

static struct port *slot_port(struct slot *s)
{
	struct port *p = &x.port[s->port];

	return (p->ops && p->ped) ? p : NULL;
}

static void ep_halt(struct slot *s, uint8_t id, int dci, struct ring *r, int sid, uint8_t cc,
		    uint32_t residue)
{
	struct ep *e = &s->ep[dci];

	xfer_event(id, dci, r->deq, cc, residue, false, false);
	e->state = EP_HALTED;
	ring_reset(r);
	e->swork = 0;
	e->sparked = 0;
	s->work &= ~BIT(dci);
	s->parked &= ~BIT(dci);
	ep_ctx_state(s, dci);
	ring_ctx_write(s, dci, sid);	/* dequeue pointer at the failed TRB */
	x.st.errors += cc != CC_STALL;
}

/* After a short packet: skip the rest of the TD (TRBs chained to this one) */
static void td_skip(struct ring *r, uint32_t ctrl)
{
	uint32_t trb[4];

	while (ctrl & TRB_CH) {
		r->deq += 16;
		if (!hread(r->deq, trb, sizeof(trb)) || !!(trb[3] & TRB_CYCLE) != r->ccs) {
			return;
		}
		if (TRB_TYPE(trb[3]) == T_LINK) {
			if (trb[3] & TRB_ENT_TC) {
				r->ccs = !r->ccs;
			}
			r->deq = ((((uint64_t)trb[1] << 32) | trb[0]) & ~0xfULL) - 16;
		}
		ctrl = trb[3];
	}
}

static void td_end(struct ring *r, uint32_t ctrl)
{
	if (!(ctrl & TRB_CH)) {
		r->td_active = false;
		r->edtla = 0;
	} else {
		r->td_active = true;
	}
}

static int trb_data(struct slot *s, uint8_t id, int dci, struct ring *r, int sid,
		    const uint32_t *trb)
{
	struct port *p = slot_port(s);
	bool in = dci & 1;
	struct vxhci_data d = {
		.host = (trb[3] & TRB_IDT) ? r->deq : (((uint64_t)trb[1] << 32) | trb[0]),
		.len = TRB_LEN(trb[2]),
		.td_end = !(trb[3] & TRB_CH),
	};
	bool bei = trb[3] & TRB_BEI;
	int ret;

	if (!p) {
		ep_halt(s, id, dci, r, sid, CC_TX_ERR, d.len);
		return TRB_HALT;
	}
	ret = d.len ? p->ops->transfer(p->ctx, dci / 2, in, sid, &d) : 0;
	if (ret == -EAGAIN) {
		x.st.naks++;
		return TRB_WAIT;
	}
	if (ret == -EPIPE) {
		ep_halt(s, id, dci, r, sid, CC_STALL, d.len);
		return TRB_HALT;
	}
	if (ret < 0) {
		ep_halt(s, id, dci, r, sid, CC_TX_ERR, d.len);
		return TRB_HALT;
	}
	if (d.actual > d.len) {
		d.actual = d.len;
	}
	x.st.bytes += d.actual;
	r->edtla += d.actual;
	if (in && d.actual < d.len) {
		if (trb[3] & (TRB_ISP | TRB_IOC)) {
			xfer_event(id, dci, r->deq, CC_SHORT, d.len - d.actual, false, bei);
		}
		td_skip(r, trb[3]);
		r->td_active = false;
		r->edtla = 0;
		return TRB_DONE;
	}
	if (trb[3] & TRB_IOC) {
		xfer_event(id, dci, r->deq, CC_SUCCESS, d.len - d.actual, false, bei);
	}
	td_end(r, trb[3]);
	return TRB_DONE;
}

static int trb_control(struct slot *s, uint8_t id, const uint32_t *trb)
{
	struct ep *e = &s->ep[1];
	struct ring *r = &e->r;
	struct port *p = slot_port(s);
	uint32_t len = TRB_LEN(trb[2]);
	uint64_t buf = ((uint64_t)trb[1] << 32) | trb[0];
	uint16_t wlen;

	if (!p) {
		ep_halt(s, id, 1, r, 0, CC_TX_ERR, len);
		return TRB_HALT;
	}
	switch (TRB_TYPE(trb[3])) {
	case T_SETUP:
		memcpy(e->setup, trb, 8);	/* immediate data */
		wlen = MIN(sys_get_le16(&e->setup[6]), VXHCI_CTRL_MAX);
		e->ctl_done = false;
		e->ctl_len = 0;
		e->ctl_off = 0;
		r->td_active = true;
		if (e->setup[0] & 0x80) {
			/* IN: the device answers now, the data stage hands it out */
			e->ctl_len = wlen;
			e->ctl_ret = p->ops->control(p->ctx, e->setup, ctl_buf, &e->ctl_len);
			e->ctl_len = MIN(e->ctl_len, wlen);
			e->ctl_done = true;
		}
		if (trb[3] & TRB_IOC) {
			xfer_event(id, 1, r->deq, CC_SUCCESS, 0, false, false);
		}
		return TRB_DONE;
	case T_DATA:
	case T_NORMAL:
		if (e->setup[0] & 0x80) {
			uint32_t n;

			if (e->ctl_ret) {
				ep_halt(s, id, 1, r, 0, CC_STALL, len);
				return TRB_HALT;
			}
			n = MIN(len, (uint32_t)(e->ctl_len - e->ctl_off));
			if (n && !hwrite(buf, ctl_buf + e->ctl_off, n)) {
				ep_halt(s, id, 1, r, 0, CC_TX_ERR, len);
				return TRB_HALT;
			}
			e->ctl_off += n;
			x.st.bytes += n;
			if (n < len) {
				if (trb[3] & (TRB_ISP | TRB_IOC)) {
					xfer_event(id, 1, r->deq, CC_SHORT, len - n, false, false);
				}
				td_skip(r, trb[3]);
				return TRB_DONE;
			}
		} else {
			uint32_t n = MIN(len, (uint32_t)(VXHCI_CTRL_MAX - e->ctl_len));

			if (n && !hread((trb[3] & TRB_IDT) ? r->deq : buf, ctl_buf + e->ctl_len, n)) {
				ep_halt(s, id, 1, r, 0, CC_TX_ERR, len);
				return TRB_HALT;
			}
			e->ctl_len += n;
			x.st.bytes += n;
		}
		if (trb[3] & TRB_IOC) {
			xfer_event(id, 1, r->deq, CC_SUCCESS, 0, false, false);
		}
		return TRB_DONE;
	case T_STATUS:
		if (!e->ctl_done) {
			e->ctl_ret = p->ops->control(p->ctx, e->setup, ctl_buf, &e->ctl_len);
			e->ctl_done = true;
		}
		if (e->ctl_ret) {
			ep_halt(s, id, 1, r, 0, CC_STALL, 0);
			return TRB_HALT;
		}
		r->td_active = false;
		if (trb[3] & TRB_IOC) {
			xfer_event(id, 1, r->deq, CC_SUCCESS, 0, false, false);
		}
		return TRB_DONE;
	default:
		return -1;
	}
}

/* Work through one ring (an endpoint's or a stream's) */
static int ring_run(struct slot *s, uint8_t id, int dci, struct ring *r, int sid)
{
	struct ep *e = &s->ep[dci];

	for (int n = 0; n < TRBS_PER_RUN; n++) {
		uint32_t *trb = r->trb;
		int res;

		if (e->state != EP_RUNNING) {
			return RUN_HALT;
		}
		if (er_free() < 3) {
			return RUN_MORE;	/* room for this TRB's events first */
		}
		if (!r->trb_valid) {
			if (!hread(r->deq, trb, 16)) {
				ep_halt(s, id, dci, r, sid, CC_TRB_ERR, 0);
				return RUN_HALT;
			}
			if (!!(trb[3] & TRB_CYCLE) != r->ccs) {
				return RUN_EMPTY;	/* until the next doorbell */
			}
			r->trb_valid = true;
		}

		switch (TRB_TYPE(trb[3])) {
		case T_LINK:
			if (trb[3] & TRB_ENT_TC) {
				r->ccs = !r->ccs;
			}
			r->deq = (((uint64_t)trb[1] << 32) | trb[0]) & ~0xfULL;
			r->trb_valid = false;
			continue;
		case T_NOOP:
			if (trb[3] & TRB_IOC) {
				xfer_event(id, dci, r->deq, CC_SUCCESS, 0, false, trb[3] & TRB_BEI);
			}
			td_end(r, trb[3]);
			res = TRB_DONE;
			break;
		case T_EVENT_DATA:
			if (trb[3] & TRB_IOC) {
				xfer_event(id, dci, ((uint64_t)trb[1] << 32) | trb[0], CC_SUCCESS,
					   r->edtla, true, trb[3] & TRB_BEI);
			}
			td_end(r, trb[3]);
			res = TRB_DONE;
			break;
		case T_SETUP:
		case T_DATA:
		case T_STATUS:
			res = dci == 1 ? trb_control(s, id, trb) : -1;
			break;
		case T_NORMAL:
			res = dci == 1 ? trb_control(s, id, trb) : trb_data(s, id, dci, r, sid, trb);
			break;
		default:
			res = -1;
			break;
		}
		if (res < 0) {
			LOG_WRN("slot %u ep %d: TRB type %u not supported", id, dci,
				TRB_TYPE(trb[3]));
			ep_halt(s, id, dci, r, sid, CC_TRB_ERR, 0);
			return RUN_HALT;
		}
		if (res == TRB_WAIT) {
			return RUN_WAIT;
		}
		if (res == TRB_HALT) {
			return RUN_HALT;
		}
		x.st.trbs++;
		x.active = true;
		s->progress = true;
		r->deq += 16;
		r->trb_valid = false;
	}
	return RUN_MORE;
}

static void ep_run(struct slot *s, uint8_t id, int dci)
{
	struct ep *e = &s->ep[dci];

	if (!e->streams) {
		/* a NAK keeps the endpoint in the work set: retried next poll */
		if (ring_run(s, id, dci, &e->r, 0) == RUN_EMPTY || e->state != EP_RUNNING) {
			s->work &= ~BIT(dci);
		}
		return;
	}
	/*
	 * Streams: a stream the device cannot serve yet (its command has not
	 * arrived) is parked, so it costs nothing per poll and blocks no other
	 * stream; any progress on the device re-arms the parked ones.
	 */
	for (uint64_t m = e->swork; m; m &= m - 1) {
		int sid = __builtin_ctzll(m);

		switch (ring_run(s, id, dci, &e->streams[sid], sid)) {
		case RUN_EMPTY:
			e->swork &= ~BIT64(sid);
			break;
		case RUN_WAIT:
			e->swork &= ~BIT64(sid);
			e->sparked |= BIT64(sid);
			s->parked |= BIT(dci);
			break;
		case RUN_HALT:
			return;
		default:
			break;
		}
	}
	if (!e->swork) {
		s->work &= ~BIT(dci);
	}
}

static void slot_run(struct slot *s, uint8_t id)
{
	s->progress = false;
	for (uint32_t w = s->work; w; w &= w - 1) {
		ep_run(s, id, __builtin_ctz(w));
	}
	if (s->progress && s->parked) {
		for (uint32_t w = s->parked; w; w &= w - 1) {
			struct ep *e = &s->ep[__builtin_ctz(w)];

			e->swork |= e->sparked;
			e->sparked = 0;
		}
		s->work |= s->parked;
		s->parked = 0;
	}
}

/*
 * One doorbell register per slot: back-to-back doorbells for different
 * endpoints (UAS rings status, data and command for each command) overwrite
 * each other before the poll sees them. So any doorbell (and a slow periodic
 * sweep) also looks at every running endpoint without streams and re-arms
 * parked streams; streams the device has something for are signalled by the
 * device itself (vxhci_stream_ready, like a USB 3 ERDY).
 */
static void slot_sweep(struct slot *s)
{
	bool cmds_idle = !x.cr_work;
	uint32_t consumed = ev_consumed();

	for (int i = 1; i < 32; i++) {
		struct ep *e = &s->ep[i];

		/*
		 * A Stopped endpoint waits for a doorbell, which may be lost too. Once
		 * the host has handled our Stop/Set TR Dequeue completion and queued
		 * no further command, it only rings doorbells: restart.
		 */
		if (e->state == EP_STOPPED && cmds_idle &&
		    (int32_t)(consumed - e->restart_seq) >= 0) {
			e->state = EP_RUNNING;
			ep_ctx_state(s, i);
			if (e->streams) {
				e->swork |= e->sloaded;
				s->work |= BIT(i);
			}
		}
		if (e->state == EP_RUNNING && !e->streams) {
			s->work |= BIT(i);
		}
	}
	for (uint32_t w = s->parked; w; w &= w - 1) {
		struct ep *e = &s->ep[__builtin_ctz(w)];

		e->swork |= e->sparked;
		e->sparked = 0;
	}
	s->work |= s->parked;
	s->parked = 0;
}

static void doorbell(int sid, uint32_t val)
{
	struct slot *s;
	struct ep *e;
	uint8_t dci = val & 0xff;
	uint16_t stream = val >> 16;

	x.st.doorbells++;
	x.active = true;
	if (sid == 0) {
		if (dci == 0 && x.running) {
			x.crr = true;
			x.cr_work = true;
			x.crcr_lo = CRCR_CRR;
			wr(R_CRCR_LO, x.crcr_lo);
		}
		return;
	}
	s = slot_get(sid);
	if (!s || dci < 1 || dci > 31) {
		return;
	}
	e = &s->ep[dci];
	if (e->state == EP_STOPPED) {
		e->state = EP_RUNNING;
		ep_ctx_state(s, dci);
	}
	slot_sweep(s);
	if (e->state != EP_RUNNING) {
		return;
	}
	if (e->streams) {
		if (!stream_get(e, stream)) {
			return;
		}
		e->swork |= BIT64(stream);
		e->sparked &= ~BIT64(stream);
	}
	s->work |= BIT(dci);
}

/* Called by a device from its callbacks: stream @stream of endpoint @ep has data/status */
void vxhci_stream_ready(void *dev_ctx, uint8_t ep, bool in, uint16_t stream)
{
	uint8_t dci = ep * 2 + (in ? 1 : 0);

	for (int i = 1; i <= MAX_SLOTS; i++) {
		struct slot *s = &x.slot[i];
		struct ep *e;

		if (!s->enabled || x.port[s->port].ctx != dev_ctx || dci > 31) {
			continue;
		}
		e = &s->ep[dci];
		if (e->state == EP_RUNNING && e->streams && stream_get(e, stream)) {
			e->swork |= BIT64(stream);
			e->sparked &= ~BIT64(stream);
			s->parked &= e->sparked ? ~0U : ~BIT(dci);
			s->work |= BIT(dci);
		}
	}
}

/* Diagnostics: endpoint, ring and stream state, as text */
size_t vxhci_dump(char *buf, size_t len)
{
	size_t n = 0;

#define OUT(...) (n += snprintk(buf + n, n < len ? len - n : 0, __VA_ARGS__))
	k_mutex_lock(&x.lock, K_FOREVER);
	OUT("ip %d pending %d line %d, event enq %u free %u, ERDP pos %u\n", x.ip, x.ev_pending,
	    x.line, er_enq_pos(), er_free(), er_pos(erdp()));
	for (int i = 1; i <= MAX_SLOTS; i++) {
		struct slot *s = &x.slot[i];

		if (!s->enabled) {
			continue;
		}
		OUT("slot %d: state %u work 0x%x parked 0x%x\n", i, s->state, s->work, s->parked);
		for (int d = 1; d < 32; d++) {
			struct ep *e = &s->ep[d];
			uint32_t trb[4] = {0};

			if (e->state == EP_DISABLED) {
				continue;
			}
			if (!e->streams) {
				hread(e->r.deq, trb, 16);
				OUT(" dci %d: state %u deq 0x%llx ccs %d valid %d, TRB ctrl 0x%08x\n", d,
				    e->state, e->r.deq, e->r.ccs, e->r.trb_valid, trb[3]);
				continue;
			}
			OUT(" dci %d: state %u, %u streams, loaded 0x%llx work 0x%llx parked 0x%llx\n",
			    d, e->state, e->nstreams, e->sloaded, e->swork, e->sparked);
			for (uint64_t m = e->sloaded; m; m &= m - 1) {
				int sid = __builtin_ctzll(m);
				struct ring *r = &e->streams[sid];

				hread(r->deq, trb, 16);
				if (!!(trb[3] & TRB_CYCLE) == r->ccs) {
					OUT("  stream %d: deq 0x%llx TRB pending, ctrl 0x%08x len %u\n",
					    sid, r->deq, trb[3], TRB_LEN(trb[2]));
				}
			}
		}
	}
	k_mutex_unlock(&x.lock);
#undef OUT
	return n;
}

/* --- Controller --- */

/* Controller reset. USBCMD is written last: HCRST reads 0 only once all else is back */
static void hc_reset(void)
{
	for (uint32_t off = OP(4); off < OP(0x400); off += 4) {
		wr(off, off == R_PAGESIZE ? 1 : 0);	/* page size 4 KiB */
	}
	for (uint32_t off = RTSOFF; off < DBOFF + 4 * (MAX_SLOTS + 1); off += 4) {
		wr(off, 0);
	}
	for (int i = 0; i <= MAX_SLOTS; i++) {
		wr(R_DB(i), DB_IDLE);
	}
	wr(R_IMOD, 4000);		/* spec default: 1 ms */
	memset(x.slot, 0, sizeof(x.slot));
	memset(pool_used, 0, sizeof(pool_used));
	x.cmd = 0;
	x.running = false;
	x.crr = false;
	x.cr_work = false;
	x.cr_deq = 0;
	x.crcr_lo = x.crcr_hi = 0;
	x.erstba = 0;
	x.ev_valid = false;
	x.ip = false;
	x.ev_pending = false;
	line_set(false);
	for (int i = 1; i <= NUM_PORTS; i++) {
		struct port *p = &x.port[i];

		port_connect_state(i);
		if (p->ops) {
			p->ops->reset(p->ctx);
		}
		p->change = p->ccs ? PS_CSC : 0;
		p->rws = 0;
		p->published = portsc(i);
		wr(R_PORTSC(i), p->published);
	}
	sts_publish();
	barrier_dmem_fence_full();
	wr(R_USBCMD, 0);
}

static void usbcmd_check(void)
{
	uint32_t c = rd(R_USBCMD);

	if (c == x.cmd) {
		return;
	}
	x.active = true;
	if (c & CMD_HCRST) {
		LOG_INF("host reset the controller");
		hc_reset();
		return;
	}
	if ((c & CMD_RS) && !x.running) {
		x.running = true;
		if (!x.ev_valid) {
			er_load();
		}
		LOG_INF("controller running (Linux xhci)");
	} else if (!(c & CMD_RS) && x.running) {
		x.running = false;
		x.crr = false;
		x.crcr_lo = 0;
		wr(R_CRCR_LO, 0);
		LOG_INF("controller halted");
	}
	/*
	 * Write back only what the controller changes (save/restore complete at
	 * once): a write-back could overwrite a second host write that came in
	 * since the read (Linux sets INTE and then RS back to back).
	 */
	x.cmd = c & ~(CMD_CSS | CMD_CRS);
	if (c & (CMD_CSS | CMD_CRS)) {
		wr(R_USBCMD, x.cmd);
	}
	sts_publish();
}

static void ir_check(void)
{
	uint64_t ba = ((uint64_t)rd(R_ERSTBA_HI) << 32) | (rd(R_ERSTBA_LO) & ~0x3fU);

	if (ba != x.erstba) {
		x.erstba = ba;
		er_load();
		x.active = true;
	}
	if ((rd(R_USBSTS)) != x.sts) {
		wr(R_USBSTS, x.sts);	/* host W1C write-back: the emulation owns these bits */
	}
}

static void poll_once(void)
{
	timing_t t0 = timing_counter_get(), t1;
	bool was_active;

	usbcmd_check();
	ports_check();
	ir_check();
	crcr_check();
	if (x.running) {
		for (int i = 0; i <= MAX_SLOTS; i++) {
			uint32_t v = rd(R_DB(i));

			if (v != DB_IDLE) {
				wr(R_DB(i), DB_IDLE);
				barrier_dmem_fence_full();
				doorbell(i, v);
			}
		}
		if (x.cr_work) {
			cmd_ring_run();
		}
		if (k_uptime_get_32() - x.last_sweep >= 5) {
			/* safety net for a doorbell written between our read and reset */
			x.last_sweep = k_uptime_get_32();
			for (int i = 1; i <= MAX_SLOTS; i++) {
				if (x.slot[i].enabled) {
					slot_sweep(&x.slot[i]);
				}
			}
		}
		for (int i = 1; i <= MAX_SLOTS; i++) {
			if (x.slot[i].work) {
				slot_run(&x.slot[i], i);
			}
		}
		wr(R_MFINDEX, (k_uptime_get_32() * 8U) & 0x3fff);	/* 125 us microframes */
	}
	was_active = x.active;
	irq_update();

	t1 = timing_counter_get();
	uint64_t cyc = timing_cycles_get(&t0, &t1);

	x.st.polls++;
	x.st.poll_cycles += cyc;
	if (was_active) {
		x.st.work_cycles += cyc;
	}
}

/*
 * Lowest priority: everything else runs first. Busy polling while anything
 * happens; in adaptive mode, after 2 ms without activity sleep one tick, so
 * the core can idle (the doorbell then waits up to a tick).
 */
static void vxhci_thread(void *a, void *b, void *c)
{
	while (true) {
		int64_t now;

		k_mutex_lock(&x.lock, K_FOREVER);
		x.active = false;
		poll_once();
		k_mutex_unlock(&x.lock);

		now = k_uptime_get();
		if (x.active || x.ip) {
			x.last_active = now;
		}
		if (x.adaptive && now - x.last_active > 2) {
			k_sleep(K_TICKS(1));
		} else {
			k_yield();
		}
	}
}

/* --- API --- */

int vxhci_start(const struct vxhci_transport *t)
{
	static const char name[] = "Wimmo Virtual xHCI";
	static const uint32_t xecp[] = {
		/* Supported Protocol: USB 3.0, port 1 */
		0x03000002 | (4 << 8), 0x20425355 /* "USB " */, (1 << 8) | PORT_USB3, 0,
		/* Supported Protocol: USB 2.0, port 2 (last capability) */
		0x02000002, 0x20425355, (1 << 8) | PORT_USB2, 0,
	};
	volatile uint32_t *vend;

	x.t = *t;
	k_mutex_init(&x.lock);
	timing_init();
	timing_start();
	x.adaptive = IS_ENABLED(CONFIG_VXHCI_ADAPTIVE_POLL);

	for (uint32_t off = 0; off < VXHCI_REGS_SIZE; off += 4) {
		wr(off, 0);
	}
	wr(CAP_VERSION, (0x0100U << 16) | CAPLENGTH);	/* xHCI 1.0 */
	wr(CAP_HCSPARAMS1, MAX_SLOTS | (1 << 8) | ((uint32_t)NUM_PORTS << 24));
	wr(CAP_HCSPARAMS2, 1 | (4 << 4));	/* IST 1 microframe, ERST max 16, no scratchpad */
	wr(CAP_HCSPARAMS3, 0);
	/*
	 * 32-bit addressing, 32-byte contexts, no port power control, primary
	 * stream arrays of up to 64 entries (MaxPSASize 5), no secondary ones
	 */
	wr(CAP_HCCPARAMS1, ((XECP / 4) << 16) | (5 << 12) | BIT(7));
	wr(CAP_DBOFF, DBOFF);
	wr(CAP_RTSOFF, RTSOFF);
	wr(CAP_HCCPARAMS2, 0);
	for (size_t i = 0; i < ARRAY_SIZE(xecp); i++) {
		wr(XECP + 4 * i, xecp[i]);
	}
	x.port[PORT_USB3].speed = VXHCI_SPEED_SUPER;
	x.port[PORT_USB2].speed = VXHCI_SPEED_HIGH;
	hc_reset();
	x.sts = STS_HCH;
	sts_publish();

	vend = &x.t.regs[VXHCI_VENDOR / 4];
	vend[1] = VENDOR_MAGIC1;
	vend[2] = VENDOR_VERSION;
	memcpy((void *)&vend[8], name, sizeof(name));
	vend[3] = VENDOR_STATE_READY;
	vend[0] = VENDOR_MAGIC0;	/* last: the host side waits for it */

	k_thread_create(&vxhci_thread_data, vxhci_stack, K_THREAD_STACK_SIZEOF(vxhci_stack),
			vxhci_thread, NULL, NULL, NULL, K_LOWEST_APPLICATION_THREAD_PRIO, 0,
			K_NO_WAIT);
	k_thread_name_set(&vxhci_thread_data, "vxhci");
	LOG_INF("vxHCI: \"%s\" ready (registers at %p)", name, (void *)x.t.regs);
	return 0;
}

int vxhci_port_attach(int port, const struct vxhci_device_ops *ops, void *ctx,
		      enum vxhci_speed speed)
{
	struct port *p;
	uint32_t old;

	if (port < 1 || port > NUM_PORTS || (port == PORT_USB3) != (speed == VXHCI_SPEED_SUPER)) {
		return -EINVAL;
	}
	k_mutex_lock(&x.lock, K_FOREVER);
	p = &x.port[port];
	old = p->change;
	p->ops = ops;
	p->ctx = ctx;
	p->speed = speed;
	ops->reset(ctx);
	port_connect_state(port);
	p->change |= PS_CSC;
	port_publish(port, old);
	k_mutex_unlock(&x.lock);
	LOG_INF("port %d: %s device attached", port, speed_str(speed));
	return 0;
}

void vxhci_port_detach(int port)
{
	struct port *p;
	uint32_t old;

	if (port < 1 || port > NUM_PORTS) {
		return;
	}
	k_mutex_lock(&x.lock, K_FOREVER);
	p = &x.port[port];
	old = p->change;
	if (p->ops) {
		bool was_enabled = p->ped;

		p->ops = NULL;
		port_connect_state(port);
		p->change |= PS_CSC | (was_enabled ? PS_PEC : 0);
		port_publish(port, old);
		LOG_INF("port %d: device detached", port);
	}
	k_mutex_unlock(&x.lock);
}

void vxhci_set_adaptive(bool adaptive)
{
	x.adaptive = adaptive;
}

bool vxhci_adaptive(void)
{
	return x.adaptive;
}

void vxhci_get_stats(struct vxhci_stats *s)
{
	*s = x.st;
}

const char *vxhci_state(void)
{
	if (!x.running) {
		return (x.cmd & CMD_HCRST) ? "resetting" : "halted";
	}
	for (int i = 1; i <= MAX_SLOTS; i++) {
		if (x.slot[i].enabled && x.slot[i].state == SLOT_STATE_CONFIGURED) {
			return "running, device configured";
		}
	}
	return "running";
}
