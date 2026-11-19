/*
 * Virtual EHCI host controller ("Wimmo") emulated on the M7.
 *
 * A standard EHCI driver on the host (Linux ehci-platform today, ehci-pci on a
 * PCIe endpoint later) drives a register page that is plain memory. Nothing
 * traps host accesses, so a thread polls the page and infers writes:
 *
 *  - USBCMD and PORTSC: any difference from what was last published is a
 *    host write.
 *  - USBSTS: the host only ever writes write-1-to-clear masks of the
 *    interrupt bits. A reserved bit (31) is kept set in every published
 *    value; a value without it is a host write, even one that rewrites the
 *    same bits.
 *  - PORTSC connect change (CSC): a host write that only clears CSC leaves
 *    the value unchanged and cannot be seen. CSC clears itself 300 ms after
 *    it was set (or when the host starts a port reset), which covers the
 *    driver's connect debounce.
 *  - Port reset: the port is enabled 10 ms into the reset, while PR is still
 *    set, so it is already enabled when the host ends the reset and reads
 *    the port back right away.
 *
 * Schedules: the async ring (ASYNCLISTADDR) and the periodic frame list
 * (PERIODICLISTBASE) are walked in host memory. A queue head's overlay is
 * loaded from its next qTD as the EHCI spec describes; each qTD stage goes to
 * the device behind the port in one piece, the token is written back to the
 * qTD and the overlay, and USBINT/USBERRINT are raised. NAKs leave the qTD
 * active for the next pass. No isochronous (iTD/siTD).
 *
 * The root port carries the virtual high-speed hub (vehci_hub.c); stages are
 * routed by USB address to the hub or to the device on its port. Split
 * transactions to full/low-speed devices behind the hub need nothing special:
 * the stage simply runs, and the queue head's speed and TT fields are passed
 * on (vehci_xfer).
 *
 * Copyright (c) 2026, imx95-devenv contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>
#include <zephyr/timing/timing.h>

#include "vehci_internal.h"

LOG_MODULE_REGISTER(vehci, CONFIG_VEHCI_LOG_LEVEL);

/* Capability registers */
#define CAP_LENGTH_VERSION	0x00
#define CAP_HCSPARAMS		0x04
#define CAP_HCCPARAMS		0x08

/* Operational registers, relative to the page */
#define OP(x)			(VEHCI_CAPLENGTH + (x))
#define R_CMD			OP(0x00)
#define R_STS			OP(0x04)
#define R_INTR			OP(0x08)
#define R_FRINDEX		OP(0x0c)
#define R_SEGMENT		OP(0x10)
#define R_PERIODIC		OP(0x14)
#define R_ASYNC			OP(0x18)
#define R_CONFIGFLAG		OP(0x40)
#define R_PORTSC		OP(0x44)

#define CMD_RUN			BIT(0)
#define CMD_RESET		BIT(1)
#define CMD_PSE			BIT(4)
#define CMD_ASE			BIT(5)
#define CMD_IAAD		BIT(6)
#define CMD_DEFAULT		0x00080000	/* ITC = 8 microframes */

#define STS_INT			BIT(0)
#define STS_ERR			BIT(1)
#define STS_PCD			BIT(2)
#define STS_FLR			BIT(3)
#define STS_FATAL		BIT(4)
#define STS_IAA			BIT(5)
#define STS_HALT		BIT(12)
#define STS_PSS			BIT(14)
#define STS_ASS			BIT(15)
#define STS_CANARY		BIT(31)	/* reserved bit: absent = the host wrote the register */
#define STS_W1C			(0x3f | STS_FLR)

#define PORT_CCS		BIT(0)
#define PORT_CSC		BIT(1)
#define PORT_PE			BIT(2)
#define PORT_PEC		BIT(3)
#define PORT_OCC		BIT(5)
#define PORT_FPR		BIT(6)
#define PORT_SUSP		BIT(7)
#define PORT_PR			BIT(8)
#define PORT_LS_J		(2 << 10)
#define PORT_LS_K		(1 << 10)
#define PORT_PP			BIT(12)
#define PORT_PO			BIT(13)
#define PORT_HOST_RW		(GENMASK(22, 14))	/* indicators, test control, wake enables */

/* Link pointers and qTD token */
#define LINK_T			BIT(0)
#define LINK_TYPE(x)		(((x) >> 1) & 3)
#define LINK_TYPE_QH		1
#define LINK_ADDR(x)		((x) & ~0x1fU)

#define TOK_ACTIVE		BIT(7)
#define TOK_HALTED		BIT(6)
#define TOK_XACT		BIT(3)
#define TOK_PID(t)		(((t) >> 8) & 3)
#define TOK_CERR_MASK		(3 << 10)
#define TOK_IOC			BIT(15)
#define TOK_BYTES(t)		(((t) >> 16) & 0x7fff)
#define TOK_BYTES_MASK		(0x7fffU << 16)
#define TOK_DT			BIT(31)

#define QH_ADDR(i)		((i) & 0x7f)
#define QH_EP(i)		(((i) >> 8) & 0xf)
#define QH_DTC			BIT(14)
#define QH_MPS(i)		(((i) >> 16) & 0x7ff)
#define QH_EPS(i)		(((i) >> 12) & 3)
#define QH_C			BIT(27)
#define QH_SMASK(i)		((i) & 0xff)
#define QH_HUB(i)		(((i) >> 16) & 0x7f)
#define QH_PORT(i)		(((i) >> 23) & 0x7f)

#define VENDOR_MAGIC0		0x6d6d6957	/* "Wimm" */
#define VENDOR_MAGIC1		0x0000006f	/* "o" */
#define VENDOR_VERSION		0x00010000
#define VENDOR_STATE_READY	1

#define CSC_AUTOCLEAR_MS	300
#define RESET_ENABLE_MS		10
#define MAX_QH_PER_PASS		64
#define MAX_QTD_PER_QH		16
#define PREFETCH_DEPTH		8

struct hw_qh {
	uint32_t link;
	uint32_t info1;
	uint32_t info2;
	uint32_t cur;
	uint32_t next;
	uint32_t alt;
	uint32_t token;
	uint32_t buf[5];
};

struct hw_qtd {
	uint32_t next;
	uint32_t alt;
	uint32_t token;
	uint32_t buf[5];
};

/* Root port; the virtual hub is always on it */
struct port {
	enum vehci_speed speed;
	bool ccs, csc, pe, pr, susp, fpr, po;
	uint32_t host_rw;
	int64_t csc_time;
	int64_t reset_start;
};

static struct {
	struct vehci_transport t;
	struct k_mutex lock;
	uint32_t cmd;		/* last USBCMD seen */
	uint32_t pending;	/* USBSTS interrupt/status bits not yet cleared by the host */
	uint32_t portsc;	/* last PORTSC published */
	bool running;
	bool iaa_req;
	bool active;		/* this poll did something: keep polling without sleeping */
	bool adaptive;
	int64_t last_active;
	bool irq;
	uint32_t frindex;
	uint32_t us_acc;
	uint32_t last_cyc;
	uint32_t last_frame;
	struct port port;
	struct vehci_stats st;
} v;

K_THREAD_STACK_DEFINE(vehci_stack, CONFIG_VEHCI_THREAD_STACK_SIZE);
static struct k_thread vehci_thread_data;

static inline uint32_t reg_rd(uint32_t off)
{
	return v.t.regs[off / 4];
}

static inline void reg_wr(uint32_t off, uint32_t val)
{
	v.t.regs[off / 4] = val;
}

/* --- port --- */

static uint32_t portsc_compose(void)
{
	struct port *p = &v.port;
	bool ccs = p->ccs && !p->po;
	uint32_t val = PORT_PP | p->host_rw;

	val |= ccs ? PORT_CCS : 0;
	val |= p->csc ? PORT_CSC : 0;
	val |= p->pe ? PORT_PE : 0;
	val |= p->pr ? PORT_PR : 0;
	val |= p->susp ? PORT_SUSP : 0;
	val |= p->fpr ? PORT_FPR : 0;
	val |= p->po ? PORT_PO : 0;
	if (ccs && !p->pe) {
		val |= p->speed == VEHCI_SPEED_LOW ? PORT_LS_K : PORT_LS_J;
	}
	return val;
}

static void port_publish(void)
{
	v.portsc = portsc_compose();
	reg_wr(R_PORTSC, v.portsc);
}

static void port_change(void)
{
	v.port.csc = true;
	v.port.csc_time = k_uptime_get();
	v.pending |= STS_PCD;
}

static void port_host_write(uint32_t val)
{
	struct port *p = &v.port;

	if (val & PORT_CSC) {
		p->csc = false;
	}
	if ((val & PORT_PR) && !p->pr) {
		p->pr = true;
		p->pe = false;
		p->csc = false;	/* the host has seen the connect */
		p->reset_start = k_uptime_get();
		vehci_hub_ops.reset(NULL);
		LOG_INF("vEHCI: root port reset");
	} else if (!(val & PORT_PR) && p->pr) {
		p->pr = false;
		if (p->ccs && p->speed == VEHCI_SPEED_HIGH) {
			p->pe = true;
		}
		LOG_INF("vEHCI: root port %s", p->pe ? "enabled, Wimmo hub (high speed)" : "not enabled");
	} else if (!(val & PORT_PE) && p->pe) {
		p->pe = false;
		LOG_INF("vEHCI: root port disabled by the host");
	}
	/* Suspend/resume: SUSP and FPR follow the host (it ends a resume by clearing both) */
	p->susp = p->pe && (val & PORT_SUSP);
	p->fpr = p->susp && (val & PORT_FPR);
	if ((val & PORT_PO) != (p->po ? PORT_PO : 0)) {
		p->po = val & PORT_PO;
		LOG_INF("vEHCI: port owner -> %s", p->po ? "companion" : "EHCI");
	}
	p->host_rw = val & PORT_HOST_RW;
	port_publish();
}

static void port_timers(int64_t now)
{
	struct port *p = &v.port;
	bool changed = false;

	if (p->csc && now - p->csc_time >= CSC_AUTOCLEAR_MS) {
		p->csc = false;
		changed = true;
	}
	if (p->pr && !p->pe && p->ccs && p->speed == VEHCI_SPEED_HIGH &&
	    now - p->reset_start >= RESET_ENABLE_MS) {
		p->pe = true;	/* see header: enabled before the host ends the reset */
		changed = true;
	}
	/* Remote wakeup: the suspended hub has a port change to report (EHCI 4.3.1) */
	if (p->susp && !p->fpr && vehci_hub_wakeup_pending()) {
		p->fpr = true;
		v.pending |= STS_PCD;
		changed = true;
		LOG_INF("vEHCI: remote wakeup from the Wimmo hub");
	}
	if (changed) {
		port_publish();
	}
}

static void shadows_drop(void);

/* --- controller --- */

static void hc_reset(void)
{
	struct port *p = &v.port;

	reg_wr(R_CMD, CMD_DEFAULT);
	reg_wr(R_INTR, 0);
	reg_wr(R_FRINDEX, 0);
	reg_wr(R_SEGMENT, 0);
	reg_wr(R_PERIODIC, 0);
	reg_wr(R_ASYNC, 0);
	reg_wr(R_CONFIGFLAG, 0);
	v.cmd = CMD_DEFAULT;
	v.pending = 0;
	shadows_drop();
	v.running = false;
	v.iaa_req = false;
	v.frindex = 0;

	p->pe = p->pr = p->susp = p->fpr = p->po = false;
	p->host_rw = 0;
	p->csc = false;
	if (p->ccs) {
		port_change();
	}
	port_publish();
	v.st.resets++;
}

static void cmd_host_write(uint32_t cmd)
{
	if (cmd & CMD_RESET) {
		LOG_INF("vEHCI: host controller reset");
		hc_reset();
		return;
	}
	if ((cmd ^ v.cmd) & (CMD_RUN | CMD_ASE)) {
		shadows_drop();
	}
	if ((cmd ^ v.cmd) & CMD_RUN) {
		v.running = cmd & CMD_RUN;
		v.last_cyc = k_cycle_get_32();
		LOG_INF("vEHCI: %s", v.running ? "run" : "stop");
	}
	if ((cmd ^ v.cmd) & (CMD_ASE | CMD_PSE)) {
		LOG_DBG("vEHCI: schedules async %s periodic %s", (cmd & CMD_ASE) ? "on" : "off",
			(cmd & CMD_PSE) ? "on" : "off");
	}
	if ((cmd & CMD_IAAD) && !(v.cmd & CMD_IAAD)) {
		v.iaa_req = true;
	}
	v.cmd = cmd;
}

static void sts_check_host_write(void)
{
	uint32_t sts = reg_rd(R_STS);

	if (!(sts & STS_CANARY)) {
		v.pending &= ~(sts & STS_W1C);
		v.st.host_writes++;
	}
}

static void sts_publish(void)
{
	uint32_t val;

	sts_check_host_write();	/* narrow the window for a host write to be overwritten */
	val = (v.pending & STS_W1C) | STS_CANARY;
	val |= v.running ? 0 : STS_HALT;
	val |= (v.cmd & CMD_ASE) ? STS_ASS : 0;
	val |= (v.cmd & CMD_PSE) ? STS_PSS : 0;
	if (reg_rd(R_STS) != val) {
		reg_wr(R_STS, val);
	}
}

static void irq_update(void)
{
	bool level = (v.pending & reg_rd(R_INTR) & 0x3f) != 0;

	if (level != v.irq) {
		v.irq = level;
		v.t.irq_set(v.t.ctx, level);
		if (level) {
			v.st.irqs++;
		}
	}
}

static void frindex_update(void)
{
	uint32_t now = k_cycle_get_32();
	uint32_t us, uframes, old;

	if (!v.running) {
		v.frindex = reg_rd(R_FRINDEX) & 0x3fff;	/* writable while halted */
		return;
	}
	us = (uint32_t)k_cyc_to_us_floor64(now - v.last_cyc) + v.us_acc;
	v.last_cyc = now;
	uframes = us / 125;
	v.us_acc = us % 125;
	if (!uframes) {
		return;
	}
	old = v.frindex;
	v.frindex = (v.frindex + uframes) & 0x3fff;
	if ((old ^ v.frindex) & BIT(13) || uframes >= 0x2000) {
		v.pending |= STS_FLR;
	}
	reg_wr(R_FRINDEX, v.frindex);
}

/* --- host memory --- */

static int host_rd(uint32_t addr, void *buf, size_t len)
{
	return v.t.host_read(v.t.ctx, addr, buf, len);
}

static int host_wr(uint32_t addr, const void *buf, size_t len)
{
	return v.t.host_write(v.t.ctx, addr, buf, len);
}

/* Copy between local memory and the pages a qTD's buffer pointers describe */
static int qtd_copy(const uint32_t bufp[5], size_t off, uint8_t *local, size_t len, bool to_host)
{
	size_t done = 0;

	off += bufp[0] & 0xfff;
	while (done < len) {
		uint32_t pos = off + done;
		uint32_t page = pos >> 12;
		size_t n = MIN(len - done, 0x1000 - (pos & 0xfff));
		uint32_t addr;
		int ret;

		if (page > 4) {
			return -EINVAL;
		}
		addr = (bufp[page] & ~0xfffU) + (pos & 0xfff);
		ret = to_host ? host_wr(addr, local + done, n) : host_rd(addr, local + done, n);
		if (ret) {
			return ret;
		}
		done += n;
	}
	return 0;
}

int vehci_xfer_read(const struct vehci_xfer *x, size_t off, void *dst, size_t n)
{
	return off + n > x->len ? -EINVAL : qtd_copy(x->bufp, off, dst, n, false);
}

int vehci_xfer_write(const struct vehci_xfer *x, size_t off, const void *src, size_t n)
{
	return off + n > x->len ? -EINVAL : qtd_copy(x->bufp, off, (uint8_t *)src, n, true);
}

uint32_t vehci_dma_addr(uint32_t host_addr)
{
	return v.t.dma_addr ? v.t.dma_addr(v.t.ctx, host_addr) : host_addr;
}

/* --- transactions --- */

enum exec_result { EXEC_DONE, EXEC_NAK };

/* Run the overlay's qTD against the device; updates *tok, sets *short_pkt */
static enum exec_result qtd_execute(const struct hw_qh *qh, bool periodic, uint32_t *tok,
				    bool *short_pkt)
{
	const struct vehci_device_ops *ops;
	void *ctx;
	uint32_t t = qh->token;
	struct vehci_xfer x = {
		.id = qh->cur,
		.addr = QH_ADDR(qh->info1),
		.ep = QH_EP(qh->info1),
		.pid = TOK_PID(t),
		.periodic = periodic,
		.speed = (enum vehci_speed)QH_EPS(qh->info1),
		.tt_hub = QH_HUB(qh->info2),
		.tt_port = QH_PORT(qh->info2),
		.mps = QH_MPS(qh->info1) ? QH_MPS(qh->info1) : 64,
		.dt = t & TOK_DT,
		.len = TOK_BYTES(t),
	};
	struct port *p = &v.port;
	int ret;

	x.control = x.ep == 0 || (qh->info1 & QH_C);
	memcpy(x.bufp, qh->buf, sizeof(x.bufp));
	*short_pkt = false;

	if (!p->pe || p->susp || p->fpr || p->po || p->pr || !vehci_hub_route(x.addr, &ops, &ctx)) {
		/* nobody answers: transaction errors until the error counter runs out */
		*tok = (t & ~(TOK_ACTIVE | TOK_CERR_MASK)) | TOK_HALTED | TOK_XACT;
		return EXEC_DONE;
	}

	ret = ops->transfer(ctx, &x);
	if (ret == -EAGAIN) {
		return EXEC_NAK;
	}
	if (ret == -EPIPE) {
		*tok = (t & ~TOK_ACTIVE) | TOK_HALTED;	/* STALL: halted, no other error bit */
		return EXEC_DONE;
	}
	if (ret) {
		*tok = (t & ~(TOK_ACTIVE | TOK_CERR_MASK)) | TOK_HALTED | TOK_XACT;
		return EXEC_DONE;
	}

	x.actual = MIN(x.actual, x.len);
	if (x.pid == VEHCI_PID_IN) {
		*short_pkt = x.actual < x.len;
	} else {
		x.actual = x.len;
	}

	/* One data toggle flip per packet (a zero-length transfer is one packet) */
	if ((x.actual ? DIV_ROUND_UP(x.actual, x.mps) : 1) & 1) {
		t ^= TOK_DT;
	}
	*tok = (t & ~(TOK_ACTIVE | TOK_BYTES_MASK)) | ((uint32_t)(x.len - x.actual) << 16);
	v.st.qtds++;
	v.st.bytes += x.actual;
	return EXEC_DONE;
}

/*
 * Queue head shadows (async schedule). While a queue head's overlay is active,
 * only the controller (us) writes it, so a local copy saves re-reading the
 * queue head on every poll. The host changes an overlay only when it is idle
 * or after unlinking the queue head, which always goes through the async
 * advance doorbell (IAA): all shadows are dropped then, and at reset/stop.
 * The prefetch cursor remembers which qTDs behind the active one were
 * already offered (with their next pointers), so a look-ahead does not read
 * them again.
 */
#define SHADOWS		8

struct qh_shadow {
	uint32_t addr;
	bool valid;
	struct hw_qh qh;
	uint8_t pf_n;
	uint32_t pf_id[PREFETCH_DEPTH];
	uint32_t pf_next[PREFETCH_DEPTH];
	uint16_t pf_len[PREFETCH_DEPTH];
	bool pf_dt[PREFETCH_DEPTH];
};

static struct qh_shadow shadows[SHADOWS];
static uint8_t shadow_rr;

static void shadows_drop(void)
{
	for (int i = 0; i < SHADOWS; i++) {
		shadows[i].valid = false;
		shadows[i].addr = 0;
	}
}

static struct qh_shadow *shadow_get(uint32_t addr)
{
	for (int i = 0; i < SHADOWS; i++) {
		if (shadows[i].addr == addr) {
			return &shadows[i];
		}
	}
	struct qh_shadow *sh = &shadows[shadow_rr++ % SHADOWS];

	sh->addr = addr;
	sh->valid = false;
	sh->pf_n = 0;
	return sh;
}

/* Offer the qTDs queued behind the in-flight one (bulk only) to the device */
static void prefetch_ahead(const struct hw_qh *qh, struct qh_shadow *sh)
{
	const struct vehci_device_ops *ops;
	void *ctx;
	uint32_t next = qh->next;
	uint16_t mps = QH_MPS(qh->info1) ? QH_MPS(qh->info1) : 512;
	uint8_t kept = 0;
	/* Toggle the next queued stage starts with: one flip per packet before it */
#define PACKETS_ODD(len) (((len) ? DIV_ROUND_UP((len), mps) : 1) & 1)
	bool dt = (qh->token & TOK_DT) ^ PACKETS_ODD(TOK_BYTES(qh->token));

	if ((qh->info1 & QH_C) || QH_EP(qh->info1) == 0 ||
	    !vehci_hub_route(QH_ADDR(qh->info1), &ops, &ctx) || !ops->prefetch) {
		return;
	}
	for (int n = 0; n < PREFETCH_DEPTH && !(next & LINK_T); n++) {
		struct hw_qtd qtd;
		uint32_t id = LINK_ADDR(next);
		int c = sh ? sh->pf_n : 0;

		/* Already offered with a known successor: no need to read it again */
		for (int i = 0; sh && i < sh->pf_n; i++) {
			if (sh->pf_id[i] == id && !(sh->pf_next[i] & LINK_T)) {
				c = i;
				break;
			}
		}
		if (sh && c < sh->pf_n) {
			uint32_t nx = sh->pf_next[c];
			uint16_t len = sh->pf_len[c];

			sh->pf_id[kept] = id;
			sh->pf_next[kept] = nx;
			sh->pf_len[kept] = len;
			sh->pf_dt[kept] = dt;
			kept++;
			dt ^= PACKETS_ODD(len);
			next = nx;
			continue;
		}

		if (host_rd(id, &qtd, sizeof(qtd)) || !(qtd.token & TOK_ACTIVE)) {
			break;
		}

		struct vehci_xfer x = {
			.id = id,
			.addr = QH_ADDR(qh->info1),
			.ep = QH_EP(qh->info1),
			.pid = TOK_PID(qtd.token),
			.speed = (enum vehci_speed)QH_EPS(qh->info1),
			.tt_hub = QH_HUB(qh->info2),
			.tt_port = QH_PORT(qh->info2),
			.mps = mps,
			.dt = dt,
			.len = TOK_BYTES(qtd.token),
		};

		memcpy(x.bufp, qtd.buf, sizeof(x.bufp));
		if (ops->prefetch(ctx, &x)) {
			break;
		}
		if (sh && kept < PREFETCH_DEPTH) {
			sh->pf_id[kept] = id;
			sh->pf_next[kept] = qtd.next;
			sh->pf_len[kept] = x.len;
			sh->pf_dt[kept] = dt;
			kept++;
		}
		dt ^= PACKETS_ODD(x.len);
		next = qtd.next;
	}
#undef PACKETS_ODD
	if (sh) {
		sh->pf_n = kept;
	}
}

static void qh_process(uint32_t addr, bool periodic)
{
	struct qh_shadow *sh = periodic ? NULL : shadow_get(addr);
	struct hw_qh qh;
	/* Look ahead only when something changed here (stage loaded or retired), not every poll */
	bool changed = false;

	if (sh && sh->valid && (sh->qh.token & TOK_ACTIVE)) {
		qh = sh->qh;	/* active overlay: only we write it */
	} else {
		/* Idle check first: overlay next/alt/token, then the next qTD's token */
		uint32_t ov[3];
		uint32_t qtok;

		if (host_rd(addr + offsetof(struct hw_qh, next), ov, sizeof(ov))) {
			v.pending |= STS_FATAL;
			return;
		}
		if ((ov[2] & TOK_HALTED) || (!(ov[2] & TOK_ACTIVE) &&
		    ((ov[0] & LINK_T) ||
		     host_rd(LINK_ADDR(ov[0]) + offsetof(struct hw_qtd, token), &qtok,
			     sizeof(qtok)) || !(qtok & TOK_ACTIVE)))) {
			if (sh) {
				sh->valid = false;
			}
			return;	/* halted or nothing queued */
		}
		if (host_rd(addr, &qh, sizeof(qh))) {
			v.pending |= STS_FATAL;
			return;
		}
	}

	for (int n = 0; n < MAX_QTD_PER_QH; n++) {
		uint32_t tok;
		bool short_pkt;

		if (qh.token & TOK_HALTED) {
			break;
		}
		if (!(qh.token & TOK_ACTIVE)) {
			struct hw_qtd qtd;
			uint32_t dt;

			if (qh.next & LINK_T) {
				break;
			}
			if (host_rd(LINK_ADDR(qh.next), &qtd, sizeof(qtd))) {
				v.pending |= STS_FATAL;
				break;
			}
			if (!(qtd.token & TOK_ACTIVE)) {
				break;
			}
			/* Load the overlay (EHCI 4.10.2); the toggle comes from the qTD if DTC */
			dt = (qh.info1 & QH_DTC) ? (qtd.token & TOK_DT) : (qh.token & TOK_DT);
			qh.cur = LINK_ADDR(qh.next);
			qh.next = qtd.next;
			qh.alt = qtd.alt;
			qh.token = (qtd.token & ~TOK_DT) | dt;
			memcpy(qh.buf, qtd.buf, sizeof(qh.buf));
			host_wr(addr + offsetof(struct hw_qh, cur), &qh.cur,
				sizeof(qh) - offsetof(struct hw_qh, cur));
			changed = true;
		}

		if (qtd_execute(&qh, periodic, &tok, &short_pkt) == EXEC_NAK) {
			v.st.naks++;
			if (!periodic) {
				v.active = true;	/* a pending async stage: poll fast */
				if (changed) {
					prefetch_ahead(&qh, sh);
				}
			}
			break;
		}

		/* Retire: token to the qTD and the overlay; short IN packet follows alt */
		qh.token = tok;
		if (short_pkt && !(qh.alt & LINK_T)) {
			qh.next = qh.alt;
		}
		host_wr(qh.cur + offsetof(struct hw_qtd, token), &tok, sizeof(tok));
		host_wr(addr + offsetof(struct hw_qh, next), &qh.next,
			offsetof(struct hw_qh, buf) - offsetof(struct hw_qh, next));

		if (tok & TOK_HALTED) {
			v.pending |= STS_ERR;
			v.st.errors++;
			break;
		}
		if ((tok & TOK_IOC) || short_pkt) {
			v.pending |= STS_INT;
		}
		changed = true;
	}

	if (sh) {
		sh->qh = qh;
		sh->valid = true;
	}
}

static void async_pass(void)
{
	uint32_t head = LINK_ADDR(reg_rd(R_ASYNC));
	uint32_t addr = head;

	if (!head) {
		return;
	}
	for (int i = 0; i < MAX_QH_PER_PASS; i++) {
		uint32_t link;

		qh_process(addr, false);
		if (host_rd(addr, &link, sizeof(link)) || (link & LINK_T)) {
			return;
		}
		addr = LINK_ADDR(link);
		if (addr == head) {
			return;
		}
	}
}

static void periodic_pass(void)
{
	uint32_t frame = (v.frindex >> 3) & 0x3ff;
	uint32_t base = reg_rd(R_PERIODIC) & ~0xfffU;
	uint32_t link;

	if (frame == v.last_frame || !base) {
		return;
	}
	v.last_frame = frame;
	if (host_rd(base + frame * 4, &link, sizeof(link))) {
		v.pending |= STS_FATAL;
		return;
	}
	for (int i = 0; i < MAX_QH_PER_PASS && !(link & LINK_T); i++) {
		uint32_t addr = LINK_ADDR(link);

		if (LINK_TYPE(link) == LINK_TYPE_QH) {
			struct hw_qh qh;

			if (host_rd(addr, &qh, sizeof(qh))) {
				return;
			}
			if (QH_SMASK(qh.info2)) {
				qh_process(addr, true);
			}
			link = qh.link;
		} else {
			/* iTD/siTD/FSTN: not emulated; all start with the next link */
			if (host_rd(addr, &link, sizeof(link))) {
				return;
			}
		}
	}
}

static void poll_once(void)
{
	uint32_t val;
	uint32_t qtds = v.st.qtds;
	timing_t t0 = timing_counter_get(), t1;

	v.st.polls++;

	uint32_t writes = v.st.host_writes;

	val = reg_rd(R_CMD);
	if (val != v.cmd) {
		v.st.host_writes++;
		cmd_host_write(val);
	}
	sts_check_host_write();
	val = reg_rd(R_PORTSC);
	if (val != v.portsc) {
		v.st.host_writes++;
		port_host_write(val);
	}

	port_timers(k_uptime_get());
	vehci_hub_poll(k_uptime_get());
	frindex_update();

	if (v.running && (v.cmd & CMD_ASE)) {
		async_pass();
	}
	if (v.running && (v.cmd & CMD_PSE)) {
		periodic_pass();
	}
	if (v.iaa_req) {
		/* A full async pass happened since the doorbell: nothing cached */
		shadows_drop();
		v.iaa_req = false;
		v.cmd &= ~CMD_IAAD;
		reg_wr(R_CMD, v.cmd);
		v.pending |= STS_IAA;
	}

	sts_publish();
	irq_update();

	if (v.st.host_writes != writes || v.st.qtds != qtds) {
		v.active = true;
	}

	t1 = timing_counter_get();
	uint64_t cyc = timing_cycles_get(&t0, &t1);

	v.st.poll_cycles += cyc;
	if (v.st.qtds != qtds) {
		v.st.work_cycles += cyc;
		v.st.work_polls++;
	}
}

/*
 * Lowest priority: everything else runs first. Busy polling while anything
 * happens; in adaptive mode, after 2 ms without activity sleep one tick (~1 ms
 * at 1024 Hz), so the core can idle. EHCI's tightest host deadline is 2 ms
 * (halt after RS=0).
 */
static void vehci_thread(void *a, void *b, void *c)
{
	while (true) {
		int64_t now;

		k_mutex_lock(&v.lock, K_FOREVER);
		v.active = false;
		poll_once();
		k_mutex_unlock(&v.lock);

		now = k_uptime_get();
		if (v.active) {
			v.last_active = now;
		}
		if (v.adaptive && now - v.last_active > 2) {
			k_sleep(K_TICKS(1));
		} else {
			k_yield();
		}
	}
}

void vehci_set_adaptive(bool adaptive)
{
	v.adaptive = adaptive;
}

bool vehci_adaptive(void)
{
	return v.adaptive;
}

/* --- API --- */

int vehci_start(const struct vehci_transport *t)
{
	volatile uint32_t *vend;
	static const char name[] = "Wimmo Virtual EHCI";

	v.t = *t;
	k_mutex_init(&v.lock);
	timing_init();
	timing_start();
	v.adaptive = IS_ENABLED(CONFIG_VEHCI_ADAPTIVE_POLL);

	reg_wr(CAP_LENGTH_VERSION, (0x0100U << 16) | VEHCI_CAPLENGTH);	/* EHCI 1.00 */
	reg_wr(CAP_HCSPARAMS, 1);	/* one port, no port power control */
	reg_wr(CAP_HCCPARAMS, 1 << 4);	/* IST 1, 32-bit, fixed frame list, no EECP */
	v.port.speed = VEHCI_SPEED_HIGH;
	v.port.ccs = true;	/* the virtual hub */
	vehci_hub_ops.reset(NULL);
	hc_reset();
	sts_publish();

	vend = &v.t.regs[VEHCI_VENDOR / 4];
	vend[1] = VENDOR_MAGIC1;
	vend[2] = VENDOR_VERSION;
	memcpy((void *)&vend[8], name, sizeof(name));
	vend[3] = VENDOR_STATE_READY;
	vend[0] = VENDOR_MAGIC0;	/* last: the host side waits for it */

	k_thread_create(&vehci_thread_data, vehci_stack, K_THREAD_STACK_SIZEOF(vehci_stack),
			vehci_thread, NULL, NULL, NULL, K_LOWEST_APPLICATION_THREAD_PRIO, 0,
			K_NO_WAIT);
	k_thread_name_set(&vehci_thread_data, "vehci");
	LOG_INF("vEHCI: \"%s\" ready (registers at %p)", name, (void *)v.t.regs);
	return 0;
}

int vehci_port_attach(const struct vehci_device_ops *ops, void *ctx, enum vehci_speed speed)
{
	k_mutex_lock(&v.lock, K_FOREVER);
	vehci_hub_attach(ops, ctx, speed);
	k_mutex_unlock(&v.lock);
	return 0;
}

void vehci_port_detach(void)
{
	k_mutex_lock(&v.lock, K_FOREVER);
	vehci_hub_detach();
	k_mutex_unlock(&v.lock);
}

void vehci_get_stats(struct vehci_stats *s)
{
	*s = v.st;
}

const char *vehci_port_state(void)
{
	struct port *p = &v.port;

	if (!p->pe) {
		return p->pr ? "root port resetting" : "root port not enabled";
	}
	return vehci_hub_port_state();
}
