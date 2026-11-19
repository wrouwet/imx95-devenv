/*
 * Shared A55 interrupt line of the Wimmo virtual controllers (see a55_irq.h).
 * Writing MSIIR raises MSGINTR1, reading MSIR clears it; Linux never touches it.
 *
 * Copyright (c) 2026, imx95-devenv contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/sys_io.h>

#include "a55_irq.h"

#define A55_MSGINTR_BASE	0x44690000U
#define MSGINTR_MSIIR	0x0	/* channel 0 */
#define MSGINTR_MSIR	0x4

static struct k_spinlock lock;
static uint32_t sources;

void a55_irq_set(enum a55_irq_src src, bool asserted)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	uint32_t before = sources;

	if (asserted) {
		sources |= BIT(src);
	} else {
		sources &= ~BIT(src);
	}
	if (sources && !before) {
		sys_write32(0, A55_MSGINTR_BASE + MSGINTR_MSIIR);
	} else if (!sources && before) {
		(void)sys_read32(A55_MSGINTR_BASE + MSGINTR_MSIR);
	}
	k_spin_unlock(&lock, key);
}

static int a55_irq_init(void)
{
	(void)sys_read32(A55_MSGINTR_BASE + MSGINTR_MSIR);	/* start deasserted */
	return 0;
}
SYS_INIT(a55_irq_init, APPLICATION, 97);
