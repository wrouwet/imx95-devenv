/* SPDX-License-Identifier: GPL-2.0 */
/*
 * m7shm in-kernel API: access to the memory window in the i.MX95 Cortex-M7
 * DTCM for other kernel drivers.
 *
 * The window is only accessible while the M7 runs: remoteproc powers the M7
 * (and its TCM) down on stop, and an A55 access then ends in an SError.
 * Bracket every access with m7shm_access_begin()/m7shm_access_end(); begin
 * returns NULL while the M7 is down. The M7SHM_OFFLINE notifier event comes
 * before the power-down, and the window stays mapped until all accesses
 * that started before it have ended.
 */
#ifndef M7SHM_H
#define M7SHM_H

#include <linux/notifier.h>
#include <linux/types.h>

struct m7shm;

/* Notifier events, called in process context with the remoteproc lock held */
#define M7SHM_ONLINE	1	/* M7 started: the window can be used */
#define M7SHM_OFFLINE	2	/* M7 about to stop: stop using the window */

struct m7shm *m7shm_get(void);
void m7shm_put(struct m7shm *shm);

size_t m7shm_size(struct m7shm *shm);
phys_addr_t m7shm_phys(struct m7shm *shm);
u32 m7shm_m7_addr(struct m7shm *shm);

void __iomem *m7shm_access_begin(struct m7shm *shm);
void m7shm_access_end(struct m7shm *shm);

int m7shm_register_notifier(struct m7shm *shm, struct notifier_block *nb);
int m7shm_unregister_notifier(struct m7shm *shm, struct notifier_block *nb);

#endif
