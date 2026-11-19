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
 * active for the next pass. No isochronous (iTD/siTD), no split transactions
 * (a high-speed device sits on the root port; full/low speed will come
 * through a virtual high-speed hub).
 *
 * Copyright (c) 2026, imx95-devenv contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <vehci/vehci.h>

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
#define QH_SMASK(i)		((i) & 0xff)

#define VENDOR_MAGIC0		0x6d6d6957	/* "Wimm" */
#define VENDOR_MAGIC1		0x0000006f	/* "o" */
#define VENDOR_VERSION		0x00010000
#define VENDOR_STATE_READY	1

#define CSC_AUTOCLEAR_MS	300
#define RESET_ENABLE_MS		10
#define MAX_QH_PER_PASS		64
#define MAX_QTD_PER_QH		16
#define XFER_MAX		20480	/* what one qTD can describe */

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

struct port {
	const struct vehci_device_ops *ops;
	void *ctx;
	enum vehci_speed speed;
	bool ccs, csc, pe, pr, susp, po;
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
	bool irq;
	uint32_t frindex;
	uint32_t us_acc;
	uint32_t last_cyc;
	uint32_t last_frame;
	struct port port;
	struct vehci_stats st;
} v;

static uint8_t xbuf[XFER_MAX];

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
		if (p->ops) {
			p->ops->reset(p->ctx);
		}
		LOG_INF("vEHCI: port reset");
	} else if (!(val & PORT_PR) && p->pr) {
		p->pr = false;
		if (p->ccs && p->speed == VEHCI_SPEED_HIGH) {
			p->pe = true;
		}
		LOG_INF("vEHCI: port reset done, %s", p->pe ? "enabled (high speed)" : "not enabled");
	} else if (!(val & PORT_PE) && p->pe) {
		p->pe = false;
		LOG_INF("vEHCI: port disabled by the host");
	}
	p->susp = p->pe && (val & PORT_SUSP) && !(val & PORT_FPR);
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
	if (changed) {
		port_publish();
	}
}

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
	v.running = false;
	v.iaa_req = false;
	v.frindex = 0;

	p->pe = p->pr = p->susp = p->po = false;
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

/* Copy between xbuf and the pages a qTD's buffer pointers describe */
static int qtd_data(const uint32_t bufp[5], size_t len, bool to_host)
{
	uint32_t off = bufp[0] & 0xfff;
	size_t done = 0;

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
		ret = to_host ? host_wr(addr, xbuf + done, n) : host_rd(addr, xbuf + done, n);
		if (ret) {
			return ret;
		}
		done += n;
	}
	return 0;
}

/* --- transactions --- */

enum exec_result { EXEC_DONE, EXEC_NAK };

/* Run the overlay's qTD against the device; updates *tok, sets *short_pkt */
static enum exec_result qtd_execute(const struct hw_qh *qh, uint32_t *tok, bool *short_pkt)
{
	struct port *p = &v.port;
	uint32_t t = qh->token;
	uint8_t pid = TOK_PID(t);
	size_t total = TOK_BYTES(t);
	size_t len = MIN(total, sizeof(xbuf));
	size_t actual = 0;
	uint16_t mps = QH_MPS(qh->info1) ? QH_MPS(qh->info1) : 64;
	int ret;

	*short_pkt = false;
	if (!p->ops || !p->pe || p->susp || p->po || p->pr ||
	    QH_ADDR(qh->info1) != p->ops->address(p->ctx)) {
		/* nobody answers: transaction errors until the error counter runs out */
		*tok = (t & ~(TOK_ACTIVE | TOK_CERR_MASK)) | TOK_HALTED | TOK_XACT;
		return EXEC_DONE;
	}

	if (pid != VEHCI_PID_IN && len && qtd_data(qh->buf, len, false)) {
		*tok = (t & ~TOK_ACTIVE) | TOK_HALTED | TOK_XACT;
		return EXEC_DONE;
	}

	ret = p->ops->transfer(p->ctx, QH_EP(qh->info1), pid, xbuf, len, &actual);
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

	if (pid == VEHCI_PID_IN) {
		actual = MIN(actual, len);
		if (actual && qtd_data(qh->buf, actual, true)) {
			*tok = (t & ~TOK_ACTIVE) | TOK_HALTED | TOK_XACT;
			return EXEC_DONE;
		}
		*short_pkt = actual < total;
	} else {
		actual = len;
	}

	/* One data toggle flip per packet (a zero-length transfer is one packet) */
	if ((actual ? DIV_ROUND_UP(actual, mps) : 1) & 1) {
		t ^= TOK_DT;
	}
	*tok = (t & ~(TOK_ACTIVE | TOK_BYTES_MASK)) | ((uint32_t)(total - actual) << 16);
	v.st.qtds++;
	return EXEC_DONE;
}

static void qh_process(uint32_t addr)
{
	struct hw_qh qh;

	if (host_rd(addr, &qh, sizeof(qh))) {
		v.pending |= STS_FATAL;
		return;
	}

	for (int n = 0; n < MAX_QTD_PER_QH; n++) {
		uint32_t tok;
		bool short_pkt;

		if (qh.token & TOK_HALTED) {
			return;
		}
		if (!(qh.token & TOK_ACTIVE)) {
			struct hw_qtd qtd;
			uint32_t dt;

			if (qh.next & LINK_T) {
				return;
			}
			if (host_rd(LINK_ADDR(qh.next), &qtd, sizeof(qtd))) {
				v.pending |= STS_FATAL;
				return;
			}
			if (!(qtd.token & TOK_ACTIVE)) {
				return;
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
		}

		if (qtd_execute(&qh, &tok, &short_pkt) == EXEC_NAK) {
			v.st.naks++;
			return;
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
			return;
		}
		if ((tok & TOK_IOC) || short_pkt) {
			v.pending |= STS_INT;
		}
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

		qh_process(addr);
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
				qh_process(addr);
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

	v.st.polls++;

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
	frindex_update();

	if (v.running && (v.cmd & CMD_ASE)) {
		async_pass();
	}
	if (v.running && (v.cmd & CMD_PSE)) {
		periodic_pass();
	}
	if (v.iaa_req) {
		/* A full async pass happened since the doorbell: nothing cached */
		v.iaa_req = false;
		v.cmd &= ~CMD_IAAD;
		reg_wr(R_CMD, v.cmd);
		v.pending |= STS_IAA;
	}

	sts_publish();
	irq_update();
}

static void vehci_thread(void *a, void *b, void *c)
{
	while (true) {
		k_mutex_lock(&v.lock, K_FOREVER);
		poll_once();
		k_mutex_unlock(&v.lock);
		k_yield();	/* lowest priority: everything else runs first */
	}
}

/* --- API --- */

int vehci_start(const struct vehci_transport *t)
{
	volatile uint32_t *vend;
	static const char name[] = "Wimmo Virtual EHCI";

	v.t = *t;
	k_mutex_init(&v.lock);

	reg_wr(CAP_LENGTH_VERSION, (0x0100U << 16) | VEHCI_CAPLENGTH);	/* EHCI 1.00 */
	reg_wr(CAP_HCSPARAMS, 1);	/* one port, no port power control */
	reg_wr(CAP_HCCPARAMS, 1 << 4);	/* IST 1, 32-bit, fixed frame list, no EECP */
	v.port.speed = VEHCI_SPEED_HIGH;
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
	if (speed != VEHCI_SPEED_HIGH) {
		return -ENOTSUP;	/* full/low speed: behind a virtual high-speed hub (to do) */
	}
	k_mutex_lock(&v.lock, K_FOREVER);
	v.port.ops = ops;
	v.port.ctx = ctx;
	v.port.speed = speed;
	v.port.ccs = true;
	v.port.pe = false;
	port_change();
	port_publish();
	k_mutex_unlock(&v.lock);
	LOG_INF("vEHCI: device attached to the root port");
	return 0;
}

void vehci_port_detach(void)
{
	k_mutex_lock(&v.lock, K_FOREVER);
	v.port.ops = NULL;
	v.port.ccs = false;
	v.port.pe = false;
	v.port.pr = false;
	port_change();
	port_publish();
	k_mutex_unlock(&v.lock);
	LOG_INF("vEHCI: device detached from the root port");
}

void vehci_get_stats(struct vehci_stats *s)
{
	*s = v.st;
}

const char *vehci_port_state(void)
{
	struct port *p = &v.port;

	if (!p->ccs) {
		return "empty";
	}
	if (p->pr) {
		return "resetting";
	}
	if (!p->pe) {
		return "connected, disabled";
	}
	return p->susp ? "enabled, suspended" : "enabled";
}
