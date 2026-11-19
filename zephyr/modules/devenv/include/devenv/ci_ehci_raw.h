/*
 * Raw transfer interface of the ChipIdea EHCI driver (uhc_ci_ehci), for the
 * virtual EHCI translator: one stage is described the way an EHCI qTD/QH
 * describes it (address, endpoint, PID, speed, TT routing, data toggle,
 * buffer page pointers) and becomes one physical qTD. Zephyr's USB host stack
 * must not use the controller at the same time; port events still arrive
 * through the uhc_init() event callback (CONNECTED_x/REMOVED).
 *
 * Copyright (c) 2026, imx95-devenv contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef DEVENV_CI_EHCI_RAW_H_
#define DEVENV_CI_EHCI_RAW_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <zephyr/device.h>
#include <zephyr/sys/dlist.h>

#define CI_EHCI_RAW_PID_OUT	0
#define CI_EHCI_RAW_PID_IN	1
#define CI_EHCI_RAW_PID_SETUP	2

#define CI_EHCI_RAW_SPEED_FULL	0	/* EHCI EPS encoding */
#define CI_EHCI_RAW_SPEED_LOW	1
#define CI_EHCI_RAW_SPEED_HIGH	2

struct ci_ehci_raw_xfer;
typedef void (*ci_ehci_raw_done_t)(struct ci_ehci_raw_xfer *x);

struct ci_ehci_raw_xfer {
	/* in */
	uint8_t addr;
	uint8_t ep;
	uint8_t pid;
	uint8_t speed;
	bool control;
	bool periodic;
	uint8_t tt_hub;		/* full/low speed: TT hub address and port (0/1: the root TT) */
	uint8_t tt_port;
	uint16_t mps;
	bool dt;		/* data toggle to start with */
	uint32_t bufp[5];	/* DMA page pointers, EHCI qTD layout (page 0 carries the offset) */
	size_t len;
	ci_ehci_raw_done_t done;	/* called from the driver thread, keep it short */
	void *user;
	/* out */
	int status;		/* 0, -EPIPE (STALL), -EIO, -EOVERFLOW (babble), -ENODEV, -ECONNRESET */
	size_t actual;
	bool dt_out;		/* toggle after the stage */
	/* driver private */
	void *priv;
};

/* Queue one stage; at most one per (address, endpoint, direction) at a time (-EBUSY) */
int ci_ehci_raw_submit(const struct device *dev, struct ci_ehci_raw_xfer *x);

/*
 * Check a stage now instead of waiting for the completion interrupt (which
 * comes at a microframe boundary at best, ITC >= 1): returns true if it has
 * completed (its done callback ran). For callers that poll anyway.
 */
bool ci_ehci_raw_poll(const struct device *dev, struct ci_ehci_raw_xfer *x);

/* Take a stage back; its done callback is not called */
int ci_ehci_raw_cancel(const struct device *dev, struct ci_ehci_raw_xfer *x);

/* Root port reset (blocks ~70 ms); returns the speed afterwards */
int ci_ehci_raw_port_reset(const struct device *dev, uint8_t *speed);

#endif /* DEVENV_CI_EHCI_RAW_H_ */
