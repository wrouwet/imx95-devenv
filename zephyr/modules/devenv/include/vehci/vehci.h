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
 *   irq_set     drive the host's interrupt line (level): MSGINTR today, MSI
 *               later.
 *
 * Devices behind the root port implement struct vehci_device_ops; transactions
 * may NAK (-EAGAIN) and are retried, so a device can be backed by something
 * asynchronous (e.g. a translator to a physical USB controller).
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
	VEHCI_SPEED_FULL,
	VEHCI_SPEED_LOW,
	VEHCI_SPEED_HIGH,
};

struct vehci_device_ops {
	/*
	 * One transaction stage to endpoint @ep (number, 0-15) with PID @pid.
	 * OUT/SETUP: @buf holds @len bytes from the host. IN: fill up to @len
	 * bytes. Set *actual. Return 0, -EAGAIN (NAK, retried later), -EPIPE
	 * (STALL) or another negative errno (transaction error).
	 */
	int (*transfer)(void *ctx, uint8_t ep, uint8_t pid, uint8_t *buf, size_t len,
			size_t *actual);
	/* USB bus reset on the port: back to address 0, unconfigured */
	void (*reset)(void *ctx);
	/* Current USB address (the device applies SET_ADDRESS itself) */
	uint8_t (*address)(void *ctx);
};

struct vehci_transport {
	volatile uint32_t *regs;
	int (*host_read)(void *ctx, uint32_t addr, void *buf, size_t len);
	int (*host_write)(void *ctx, uint32_t addr, const void *buf, size_t len);
	void (*irq_set)(void *ctx, bool asserted);
	void *ctx;
};

/* Initialize the register page and start the emulation thread */
int vehci_start(const struct vehci_transport *t);

/* Attach/detach the device behind the (single) root port */
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
};

void vehci_get_stats(struct vehci_stats *s);
const char *vehci_port_state(void);

/* Built-in test device: "Wimmo" virtual test device (VID 0x1209, PID 0x0001) */
extern const struct vehci_device_ops vehci_testdev_ops;
void *vehci_testdev_ctx(void);

#endif /* VEHCI_VEHCI_H_ */
