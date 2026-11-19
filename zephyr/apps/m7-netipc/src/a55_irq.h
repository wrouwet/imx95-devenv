/* SPDX-License-Identifier: Apache-2.0 */
/*
 * The A55 interrupt line of the Wimmo virtual controllers: MSGINTR1 channel 0,
 * wired to the A55 GIC as SPI 236 (level). The virtual EHCI and xHCI share it
 * (Linux requests it shared): the line is held while either has one pending.
 */
#ifndef M7_NETIPC_A55_IRQ_H
#define M7_NETIPC_A55_IRQ_H

#include <stdbool.h>

enum a55_irq_src {
	A55_IRQ_VEHCI,
	A55_IRQ_VXHCI,
};

void a55_irq_set(enum a55_irq_src src, bool asserted);

#endif
