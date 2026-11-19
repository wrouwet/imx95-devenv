/*
 * Virtual EHCI ("Wimmo", modules/devenv subsys/vehci) for Linux on the A55.
 *
 * Transport for the A55 stand-in of the later PCIe host:
 *  - registers: the first 4 KiB of the m7shm window (DTCM 0x20020000, Linux
 *    0x20420000), mapped by Linux's stock ehci-platform driver
 *  - host memory: Linux DDR 0x90000000-0xDFFFFFFF, read and written directly
 *    (SM grant, non-cacheable MPU regions in the overlay); the physical USB2
 *    controller reaches it at the same addresses (zero-copy)
 *  - devices: the translator (vehci_xlat) relays the physical USB2 port, or
 *    the built-in test device
 *  - interrupt: MSGINTR1 channel 0, wired to the A55 GIC as SPI 236 (level).
 *    Writing MSIIR raises it, reading MSIR clears it; Linux never touches it.
 *
 * On a PCIe endpoint the same core gets a BAR, an outbound window and MSIs.
 *
 * Copyright (c) 2026, imx95-devenv contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/sys_io.h>

#include <vehci/vehci.h>
#include <vehci/vehci_xlat.h>

#include "m7shm.h"

#define HOST_MEM_START	0x90000000U
#define HOST_MEM_END	0xe0000000U

#define VEHCI_MSGINTR_BASE	0x44690000U
#define MSGINTR_MSIIR	0x0	/* channel 0 */
#define MSGINTR_MSIR	0x4

BUILD_ASSERT(VEHCI_REGS_SIZE <= M7SHM_SIZE);

static bool host_range_ok(uint32_t addr, size_t len)
{
	return addr >= HOST_MEM_START && len <= HOST_MEM_END - addr;
}

static int host_read(void *ctx, uint32_t addr, void *buf, size_t len)
{
	if (!host_range_ok(addr, len)) {
		return -EFAULT;
	}
	memcpy(buf, (const void *)addr, len);
	return 0;
}

static int host_write(void *ctx, uint32_t addr, const void *buf, size_t len)
{
	if (!host_range_ok(addr, len)) {
		return -EFAULT;
	}
	memcpy((void *)addr, buf, len);
	return 0;
}

/* The physical USB2 controller DMAs as the M7 domain: same addresses (SMMU identity) */
static uint32_t dma_addr(void *ctx, uint32_t host_addr)
{
	return host_range_ok(host_addr, 1) ? host_addr : 0;
}

static void irq_set(void *ctx, bool asserted)
{
	if (asserted) {
		sys_write32(0, VEHCI_MSGINTR_BASE + MSGINTR_MSIIR);
	} else {
		(void)sys_read32(VEHCI_MSGINTR_BASE + MSGINTR_MSIR);
	}
}

static int vehci_a55_init(void)
{
	static const struct vehci_transport t = {
		.regs = (volatile uint32_t *)M7SHM_BASE,
		.host_read = host_read,
		.host_write = host_write,
		.dma_addr = dma_addr,
		.irq_set = irq_set,
	};

	(void)sys_read32(VEHCI_MSGINTR_BASE + MSGINTR_MSIR);	/* start deasserted */
	vehci_start(&t);
#if defined(CONFIG_APP_VEHCI_XLAT)
	vehci_xlat_start(DEVICE_DT_GET(DT_NODELABEL(usb2)));
#elif defined(CONFIG_APP_VEHCI_TESTDEV)
	vehci_port_attach(&vehci_testdev_ops, vehci_testdev_ctx(), VEHCI_SPEED_HIGH);
#endif
	return 0;
}
SYS_INIT(vehci_a55_init, APPLICATION, 98);

static int cmd_status(const struct shell *sh, size_t argc, char **argv)
{
	volatile uint32_t *r = (volatile uint32_t *)M7SHM_BASE;
	struct vehci_stats s;

	vehci_get_stats(&s);
	shell_print(sh, "Wimmo virtual EHCI, registers at 0x%08x (Linux 0x%08x)",
		    (uint32_t)M7SHM_BASE, (uint32_t)M7SHM_BASE + 0x400000);
	shell_print(sh, "USBCMD 0x%08x USBSTS 0x%08x USBINTR 0x%08x FRINDEX 0x%04x", r[8], r[9],
		    r[10], r[11]);
	shell_print(sh, "PERIODIC 0x%08x ASYNC 0x%08x CONFIGFLAG %u PORTSC 0x%08x", r[13], r[14],
		    r[24], r[25]);
	shell_print(sh, "port: %s", vehci_port_state());
	shell_print(sh, "polls %u, host writes %u, resets %u, irqs %u, qTDs %u, NAKs %u, errors %u",
		    s.polls, s.host_writes, s.resets, s.irqs, s.qtds, s.naks, s.errors);
#if defined(CONFIG_APP_VEHCI_XLAT)
	struct vehci_xlat_stats x;

	vehci_xlat_get_stats(&x);
	shell_print(sh, "translator (%s): stages %u (zero-copy %u, bounced %u), stalls %u, errors %u, "
		    "cancelled %u, port resets %u", vehci_xlat_bounce() ? "bounce" : "zero-copy",
		    x.stages, x.zero_copy, x.bounced, x.stalls, x.errors, x.cancelled, x.resets);
	if (x.timed) {
		/* 800 cycles per microsecond */
		shell_print(sh, "per stage (avg over %u): physical %llu us, completion -> host %llu us, "
			    "host -> next submit %llu us", x.timed, x.hw_cycles / x.timed / 800,
			    x.notice_cycles / x.timed / 800, x.gap_cycles / x.timed / 800);
	}
#endif
	return 0;
}

struct thread_find {
	const char *name;
	k_tid_t tid;
};

static void thread_find_cb(const struct k_thread *t, void *user)
{
	struct thread_find *f = user;
	const char *n = k_thread_name_get((k_tid_t)t);

	if (n && strcmp(n, f->name) == 0) {
		f->tid = (k_tid_t)t;
	}
}

static uint64_t thread_cycles(const char *name)
{
	struct thread_find f = {.name = name};
	k_thread_runtime_stats_t rs;

	k_thread_foreach(thread_find_cb, &f);
	if (!f.tid || k_thread_runtime_stats_get(f.tid, &rs)) {
		return 0;
	}
	return rs.execution_cycles;
}

/*
 * M7 load while the host moves data: the vEHCI thread polls all the time (lowest priority),
 * so its raw CPU share is meaningless; count the cycles of polls that retired a stage,
 * plus the USB2 driver thread (completions, physical scheduling).
 */
#define PERF_THREADS 32

static struct {
	k_tid_t tid;
	uint64_t c0, c1;
} perf_thr[PERF_THREADS];
static int perf_n;

static void perf_snap_cb(const struct k_thread *t, void *user)
{
	bool after = (bool)(uintptr_t)user;
	k_thread_runtime_stats_t rs;
	int i;

	if (k_thread_runtime_stats_get((k_tid_t)t, &rs)) {
		return;
	}
	for (i = 0; i < perf_n && perf_thr[i].tid != (k_tid_t)t; i++) {
	}
	if (i == perf_n) {
		if (after || perf_n == PERF_THREADS) {
			return;
		}
		perf_thr[perf_n++].tid = (k_tid_t)t;
	}
	if (after) {
		perf_thr[i].c1 = rs.execution_cycles;
	} else {
		perf_thr[i].c0 = rs.execution_cycles;
	}
}

static int cmd_perf(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t secs = argc > 1 ? strtoul(argv[1], NULL, 0) : 5;
	const char *drv = DEVICE_DT_GET(DT_NODELABEL(usb2))->name;
	struct vehci_stats a, b;
	uint64_t d0, d1;
	int64_t t0, t1;

	k_thread_runtime_stats_t cpu0, cpu1;

	/* No k_cycle_get_64() here: the 32 kHz system timer has no 64-bit counter (it traps) */
	perf_n = 0;
	k_thread_foreach(perf_snap_cb, (void *)false);
	k_thread_runtime_stats_cpu_get(0, &cpu0);
	vehci_get_stats(&a);
	d0 = thread_cycles(drv);
	t0 = k_uptime_get();
	k_sleep(K_SECONDS(secs));
	t1 = k_uptime_get();
	d1 = thread_cycles(drv);
	vehci_get_stats(&b);
	k_thread_runtime_stats_cpu_get(0, &cpu1);
	k_thread_foreach(perf_snap_cb, (void *)true);

	uint64_t bytes = b.bytes - a.bytes;
	uint64_t work = b.work_cycles - a.work_cycles;	/* CPU (DWT) cycles */
	uint64_t poll = b.poll_cycles - a.poll_cycles;
	uint64_t drvc = d1 - d0;
	uint64_t cpu_hz = 800000000ULL;	/* Cortex-M7 core clock */
	uint64_t cpu_el = (uint64_t)(t1 - t0) * (cpu_hz / 1000);
	uint64_t load = work + drvc;

	shell_print(sh, "%u s: %llu bytes (%llu KB/s), %u stages, %u work polls of %u polls", secs,
		    bytes, bytes / secs / 1000, b.qtds - a.qtds, b.work_polls - a.work_polls,
		    b.polls - a.polls);
	shell_print(sh, "M7 work: vEHCI %llu.%02llu %%, USB2 driver thread %llu.%02llu %% of the core",
		    work * 100 / cpu_el, work * 10000 / cpu_el % 100, drvc * 100 / cpu_el,
		    drvc * 10000 / cpu_el % 100);
	shell_print(sh, "vEHCI polling (incl. idle polls) %llu %% of the core (%s polling)",
		    poll * 100 / cpu_el, vehci_adaptive() ? "adaptive" : "busy");

	uint64_t cpu_tot = cpu1.execution_cycles - cpu0.execution_cycles;
	uint64_t cpu_idle = cpu1.idle_cycles - cpu0.idle_cycles;

	if (cpu_tot) {
		shell_print(sh, "core: idle %llu.%02llu %%, busy %llu.%02llu %% (idle thread vs all threads)",
			    cpu_idle * 100 / cpu_tot, cpu_idle * 10000 / cpu_tot % 100,
			    (cpu_tot - cpu_idle) * 100 / cpu_tot,
			    (cpu_tot - cpu_idle) * 10000 / cpu_tot % 100);
		for (int i = 0; i < perf_n; i++) {
			uint64_t c = perf_thr[i].c1 - perf_thr[i].c0;
			const char *n = k_thread_name_get(perf_thr[i].tid);

			if (c * 1000 / cpu_tot >= 1) {	/* >= 0.1 % */
				shell_print(sh, "  %-20s %3llu.%01llu %%", n ? n : "?", c * 100 / cpu_tot,
					    c * 1000 / cpu_tot % 10);
			}
		}
	}
	if (bytes) {
		uint64_t cpb100 = load * 100 / bytes;	/* cycles per byte x100 */

		shell_print(sh, "cost %llu.%02llu cycles/byte -> data rate at 100 %% M7 work: %llu Mbit/s",
			    cpb100 / 100, cpb100 % 100, cpu_hz * 8 * 100 / cpb100 / 1000000);
	}
	return 0;
}

static int cmd_poll(const struct shell *sh, size_t argc, char **argv)
{
	if (argc > 1) {
		vehci_set_adaptive(strcmp(argv[1], "busy") != 0);
	}
	shell_print(sh, "polling: %s", vehci_adaptive() ? "adaptive" : "busy");
	return 0;
}

#if defined(CONFIG_APP_VEHCI_XLAT)
static int cmd_bounce(const struct shell *sh, size_t argc, char **argv)
{
	if (argc > 1) {
		vehci_xlat_set_bounce(strcmp(argv[1], "off") != 0);
	}
	shell_print(sh, "data path: %s", vehci_xlat_bounce() ? "bounce buffers" : "zero-copy");
	return 0;
}
#endif

static int cmd_plug(const struct shell *sh, size_t argc, char **argv)
{
	return vehci_port_attach(&vehci_testdev_ops, vehci_testdev_ctx(), VEHCI_SPEED_HIGH);
}

static int cmd_unplug(const struct shell *sh, size_t argc, char **argv)
{
	vehci_port_detach();
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(vehci_cmds,
	SHELL_CMD(status, NULL, "registers, port and counters", cmd_status),
	SHELL_CMD(plug, NULL, "attach the test device to the root port", cmd_plug),
	SHELL_CMD(unplug, NULL, "detach it", cmd_unplug),
	SHELL_CMD_ARG(perf, NULL, "[seconds] throughput and M7 load", cmd_perf, 1, 1),
	SHELL_CMD_ARG(poll, NULL, "[busy|adaptive] emulation polling", cmd_poll, 1, 1),
#if defined(CONFIG_APP_VEHCI_XLAT)
	SHELL_CMD_ARG(bounce, NULL, "[on|off] translator data path (default zero-copy)", cmd_bounce,
		      1, 1),
#endif
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(vehci, &vehci_cmds, "Wimmo virtual EHCI for Linux", NULL);
