/*
 * Virtual EHCI host controller ("Wimmo"), emulated on the M7.
 *
 * A standard EHCI driver on another processor (Linux ehci-platform on the
 * A55 today, ehci-pci behind a PCIe endpoint later) sees a register page,
 * owns its queue heads/qTDs/buffers in its own memory and gets interrupts.
 * The emulation knows nothing about the transport; the integration provides:
 *
 *   regs        4 KiB register page, plain memory the host maps as its MMIO
 *               (M7 TCM today, a PCIe BAR later). There is no write trap:
 *               the core polls it and infers host writes from changed values.
 *   host_read/  access to host memory by host bus address (the addresses the
 *   host_write  host driver writes into the schedule): a direct DDR window
 *               today, a PCIe outbound window later.
 *   dma_addr    the address another local DMA master (the physical USB
 *               controller) uses for a host address: identity today, the
 *               outbound window on PCIe. Used for zero-copy transfers.
 *   irq_set     drive the host's interrupt line (level): MSGINTR today, MSI
 *               later.
 *
 * Topology: the root port carries a virtual high-speed hub ("Wimmo hub",
 * emulated here) with one downstream port. A device on that port (the
 * built-in test device, or the translator to the physical USB controller)
 * implements struct vehci_device_ops. Full/low-speed devices work like behind
 * any high-speed hub: the host addresses them through the hub's TT.
 *
 * Copyright (c) 2026, imx95-devenv contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef VEHCI_VEHCI_H_
#define VEHCI_VEHCI_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VEHCI_REGS_SIZE		0x1000

/* Register page layout: EHCI capability + operational registers, then a vendor block */
#define VEHCI_CAPLENGTH		0x20
#define VEHCI_VENDOR		0x100	/* "Wimmo" signature, state and counters */

#define VEHCI_PID_OUT		0
#define VEHCI_PID_IN		1
#define VEHCI_PID_SETUP		2

enum vehci_speed {
	VEHCI_SPEED_FULL = 0,	/* EHCI EPS encoding */
	VEHCI_SPEED_LOW = 1,
	VEHCI_SPEED_HIGH = 2,
};

/* One qTD stage, as the host described it */
struct vehci_xfer {
	uint32_t id;		/* host qTD address: stable while the stage is retried */
	uint8_t addr;		/* USB device address */
	uint8_t ep;		/* endpoint number 0-15 */
	uint8_t pid;		/* VEHCI_PID_* */
	bool periodic;		/* from the periodic schedule (interrupt endpoint) */
	bool control;		/* control endpoint */
	enum vehci_speed speed;	/* endpoint speed from the queue head */
	uint8_t tt_hub;		/* FS/LS: address of the hub with the TT, and its port */
	uint8_t tt_port;
	uint16_t mps;
	bool dt;		/* data toggle the stage starts with */
	uint32_t bufp[5];	/* host buffer page pointers (EHCI qTD layout) */
	size_t len;		/* bytes in this stage */
	size_t actual;		/* out: bytes transferred */
};

/* Copy between a stage's host buffer and local memory (at byte offset @off) */
int vehci_xfer_read(const struct vehci_xfer *x, size_t off, void *dst, size_t n);
int vehci_xfer_write(const struct vehci_xfer *x, size_t off, const void *src, size_t n);
/* Local DMA address of a host address (zero-copy), or 0 if not reachable */
uint32_t vehci_dma_addr(uint32_t host_addr);

struct vehci_device_ops {
	/*
	 * Run one stage. OUT/SETUP: read the data with vehci_xfer_read; IN:
	 * write it with vehci_xfer_write. Set x->actual. Return 0, -EAGAIN
	 * (NAK / still in progress: the same stage, same x->id, comes back
	 * later), -EPIPE (STALL) or another negative errno (transaction error).
	 */
	int (*transfer)(void *ctx, struct vehci_xfer *x);
	/* Start a bus reset of the device's port */
	void (*reset)(void *ctx);
	/* Optional: true while that reset is still running */
	bool (*reset_busy)(void *ctx);
	/* Optional: current address; without it the device takes every address but the hub's */
	uint8_t (*address)(void *ctx);
};

struct vehci_transport {
	volatile uint32_t *regs;
	int (*host_read)(void *ctx, uint32_t addr, void *buf, size_t len);
	int (*host_write)(void *ctx, uint32_t addr, const void *buf, size_t len);
	uint32_t (*dma_addr)(void *ctx, uint32_t host_addr);
	void (*irq_set)(void *ctx, bool asserted);
	void *ctx;
};

/* Initialize the register page, attach the virtual hub, start the emulation thread */
int vehci_start(const struct vehci_transport *t);

/* Attach/detach the device on the virtual hub's downstream port */
int vehci_port_attach(const struct vehci_device_ops *ops, void *ctx, enum vehci_speed speed);
void vehci_port_detach(void);

struct vehci_stats {
	uint32_t polls;
	uint32_t host_writes;
	uint32_t resets;
	uint32_t irqs;
	uint32_t qtds;
	uint32_t naks;
	uint32_t errors;
	/* load measurement (M7 cycle counter) */
	uint64_t bytes;		/* data moved by retired stages */
	uint64_t poll_cycles;	/* all polls */
	uint64_t work_cycles;	/* polls that retired at least one stage */
	uint32_t work_polls;
};

void vehci_get_stats(struct vehci_stats *s);

/* Polling: busy (false) or adaptive (sleep ~1 ms after 2 ms without activity) */
void vehci_set_adaptive(bool adaptive);
bool vehci_adaptive(void);
const char *vehci_port_state(void);

/* Built-in test device: "Wimmo" virtual test device (VID 0x1209, PID 0x0001) */
extern const struct vehci_device_ops vehci_testdev_ops;
void *vehci_testdev_ctx(void);

#endif /* VEHCI_VEHCI_H_ */
