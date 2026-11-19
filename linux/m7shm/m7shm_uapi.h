/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note OR MIT */
/*
 * /dev/m7shm user API: the memory window in the i.MX95 Cortex-M7 DTCM.
 *
 *   mmap(MAP_SHARED)   the window, write-combined (Normal non-cacheable);
 *                      SIGBUS on access while the M7 is stopped
 *   pread/pwrite       copies in and out of the window; ENODEV while stopped
 *   M7SHM_IOC_INFO     addresses, size and state
 */
#ifndef M7SHM_UAPI_H
#define M7SHM_UAPI_H

#include <linux/ioctl.h>
#include <linux/types.h>

struct m7shm_info {
	__u64 host_addr;	/* window as seen by the A55 (physical) */
	__u32 m7_addr;		/* the same window as seen by the M7 */
	__u32 size;		/* bytes */
	__u32 online;		/* 1 while the M7 runs and the window is accessible */
	__u32 reserved[3];
};

#define M7SHM_IOC_INFO	_IOR(0xe7, 0x00, struct m7shm_info)

#endif
