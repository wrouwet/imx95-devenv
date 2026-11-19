/*
 * Translator between the virtual EHCI ("Wimmo") and a physical USB host
 * controller driven by uhc_ci_ehci's raw interface. See vehci_xlat.c.
 *
 * Copyright (c) 2026, imx95-devenv contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef VEHCI_VEHCI_XLAT_H_
#define VEHCI_VEHCI_XLAT_H_

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/device.h>

/* Take over @uhc (no Zephyr USB host stack on it) and serve its root port through the Wimmo hub */
int vehci_xlat_start(const struct device *uhc);

/* Data path: zero-copy (false, default) or bounce through M7 memory */
void vehci_xlat_set_bounce(bool bounce);
bool vehci_xlat_bounce(void);

struct vehci_xlat_stats {
	uint32_t stages;
	uint32_t zero_copy;
	uint32_t bounced;
	uint32_t stalls;
	uint32_t errors;
	uint32_t cancelled;
	uint32_t resets;
};

void vehci_xlat_get_stats(struct vehci_xlat_stats *s);

#endif /* VEHCI_VEHCI_XLAT_H_ */
