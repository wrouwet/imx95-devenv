/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Memory window in the M7 DTCM, shared with Linux (m7shm.ko, /dev/m7shm).
 * TCM is never cached on the M7, so no cache maintenance is needed; use
 * barrier_dmem_fence_full() where the other side must see writes in order.
 */
#ifndef M7_NETIPC_M7SHM_H
#define M7_NETIPC_M7SHM_H

#include <stddef.h>
#include <stdint.h>
#include <zephyr/devicetree.h>

#define M7SHM_NODE DT_NODELABEL(m7shm)
#define M7SHM_BASE DT_REG_ADDR(M7SHM_NODE)
#define M7SHM_SIZE DT_REG_SIZE(M7SHM_NODE)

static inline volatile void *m7shm_base(void)
{
	return (volatile void *)M7SHM_BASE;
}

static inline size_t m7shm_size(void)
{
	return M7SHM_SIZE;
}

#endif
