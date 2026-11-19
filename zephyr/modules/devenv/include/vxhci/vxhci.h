/*
 * Virtual xHCI host controller ("Wimmo"), emulated on the M7.
 *
 * Same rules as the virtual EHCI (vehci): a standard xHCI driver on another
 * processor (Linux xhci-plat on the A55 today, xhci-pci behind a PCIe
 * endpoint later) sees a register page, keeps its rings and contexts in its
 * own memory and gets interrupts; nothing on the host side is changed. The
 * integration supplies the transport:
 *
 *   regs        4 KiB register page (plain memory: M7 TCM now, a PCIe BAR later).
 *               No write trap: doorbells are cleared after reading, other
 *               registers are compared with what was published.
 *   host_read/  host memory by host bus address
 *   host_write
 *   irq_set     the host's level interrupt (MSGINTR now, MSI later)
 *
 * Ports: port 1 is USB 3 (SuperSpeed), port 2 USB 2. Devices implement
 * struct vxhci_device_ops; the controller assigns USB addresses (Address
 * Device command), so devices never see SET_ADDRESS.
 *
 * Copyright (c) 2026, imx95-devenv contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef VXHCI_VXHCI_H_
#define VXHCI_VXHCI_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VXHCI_REGS_SIZE		0x1000
#define VXHCI_VENDOR		0xf00	/* "Wimmo" signature, state, name */

enum vxhci_speed {
	VXHCI_SPEED_FULL = 1,	/* xHCI protocol speed IDs (default PSIVs) */
	VXHCI_SPEED_LOW = 2,
	VXHCI_SPEED_HIGH = 3,
	VXHCI_SPEED_SUPER = 4,
};

/* One host data buffer (one TRB): contiguous in host memory */
struct vxhci_data {
	uint64_t host;		/* host bus address */
	uint32_t len;		/* bytes the TRB offers */
	uint32_t actual;	/* out: bytes transferred */
};

int vxhci_data_read(const struct vxhci_data *d, uint32_t off, void *dst, uint32_t n);
int vxhci_data_write(const struct vxhci_data *d, uint32_t off, const void *src, uint32_t n);

struct vxhci_device_ops {
	/*
	 * Control request on endpoint 0 (never SET_ADDRESS: the controller
	 * assigns addresses). @buf holds up to *len bytes (wLength, capped at
	 * VXHCI_CTRL_MAX): OUT data on entry; IN: fill it and set *len.
	 * Return 0 or -EPIPE (STALL).
	 */
	int (*control)(void *ctx, const uint8_t setup[8], uint8_t *buf, uint16_t *len);
	/*
	 * Bulk/interrupt TRB on endpoint @ep (1-15), direction @in; set
	 * d->actual (less than d->len on IN = short packet). Return 0,
	 * -EAGAIN (nothing to send/accept now: retried later), -EPIPE (STALL),
	 * or another negative errno (transaction error).
	 */
	int (*transfer)(void *ctx, uint8_t ep, bool in, struct vxhci_data *d);
	/* Port reset or Reset Device: back to the default state */
	void (*reset)(void *ctx);
};

#define VXHCI_CTRL_MAX		1024

struct vxhci_transport {
	volatile uint32_t *regs;
	int (*host_read)(void *ctx, uint64_t addr, void *buf, size_t len);
	int (*host_write)(void *ctx, uint64_t addr, const void *buf, size_t len);
	void (*irq_set)(void *ctx, bool asserted);
	void *ctx;
};

int vxhci_start(const struct vxhci_transport *t);

/* Attach/detach a device on port 1 (USB 3) or 2 (USB 2) */
int vxhci_port_attach(int port, const struct vxhci_device_ops *ops, void *ctx,
		      enum vxhci_speed speed);
void vxhci_port_detach(int port);

struct vxhci_stats {
	uint32_t polls;
	uint32_t commands;
	uint32_t trbs;
	uint32_t events;
	uint32_t irqs;
	uint32_t doorbells;
	uint32_t naks;
	uint32_t errors;
	uint64_t bytes;
	uint64_t work_cycles;	/* polls that did something */
	uint64_t poll_cycles;
};

void vxhci_get_stats(struct vxhci_stats *s);
void vxhci_set_adaptive(bool adaptive);
bool vxhci_adaptive(void);
const char *vxhci_state(void);

/* Built-in virtual USB 3.0 drive (mass storage, bulk-only, RAM backed) */
extern const struct vxhci_device_ops vxhci_drive_ops;
void *vxhci_drive_init(void *storage, size_t size);
/* null mode: no data copy (benchmarks the protocol path only) */
void vxhci_drive_set_null(bool null);
bool vxhci_drive_null(void);

#endif /* VXHCI_VXHCI_H_ */
