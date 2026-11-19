/*
 * Virtual EHCI internals shared by the core (vehci.c) and the virtual hub.
 *
 * Copyright (c) 2026, imx95-devenv contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef VEHCI_INTERNAL_H_
#define VEHCI_INTERNAL_H_

#include <vehci/vehci.h>

/* The virtual hub sits on the root port */
extern const struct vehci_device_ops vehci_hub_ops;

/* Which device answers @addr: the hub, the device on its port, or nobody (false) */
bool vehci_hub_route(uint8_t addr, const struct vehci_device_ops **ops, void **ctx);

/* Port reset timing etc., called from the emulation thread with the lock held */
void vehci_hub_poll(int64_t now);

/* USB address the host gave the virtual hub (0 before SET_ADDRESS) */
uint8_t vehci_hub_address(void);

/* True when the (suspended) hub would signal remote wakeup: a port changed and the host armed it */
bool vehci_hub_wakeup_pending(void);

/* Downstream port, called with the lock held */
void vehci_hub_attach(const struct vehci_device_ops *ops, void *ctx, enum vehci_speed speed);
void vehci_hub_detach(void);
const char *vehci_hub_port_state(void);

/* Standard control-request helper shared by the emulated devices */
struct vehci_ctrl {
	uint8_t setup[8];
	bool stall;
	bool data_in;
	const uint8_t *in;
	size_t in_len;
	size_t in_pos;
	uint8_t resp[64];
};

/* Run one stage of an emulated control endpoint; @setup_cb parses a SETUP */
int vehci_ctrl_stage(struct vehci_ctrl *c, struct vehci_xfer *x,
		     void (*setup_cb)(struct vehci_ctrl *c, void *ctx),
		     void (*status_cb)(struct vehci_ctrl *c, void *ctx), void *ctx);

/* String descriptor from ASCII (index 0: language list) */
size_t vehci_string_desc(const char *const *strings, size_t count, uint8_t idx, uint8_t *out,
			 size_t max);

#endif /* VEHCI_INTERNAL_H_ */
