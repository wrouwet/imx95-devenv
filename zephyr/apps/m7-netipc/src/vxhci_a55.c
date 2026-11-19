/*
 * Virtual xHCI ("Wimmo", modules/devenv subsys/vxhci) for Linux on the A55,
 * with the virtual USB 3.0 drive on its SuperSpeed port.
 *
 * Transport for the A55 stand-in of the later PCIe host:
 *  - registers: the second 4 KiB of the m7shm window (DTCM 0x20021000, Linux
 *    0x20421000), mapped by Linux's stock xhci-plat driver
 *  - host memory: Linux DDR 0x90000000-0xDFFFFFFF, read and written directly
 *    (SM grant, non-cacheable MPU regions in the overlay)
 *  - interrupt: MSGINTR1 -> A55 GIC SPI 236 (level), shared with the virtual
 *    EHCI (a55_irq.c)
 *  - drive: 64 MiB of M7 DDR at 0x83000000 (vxhci_drive region in the
 *    overlay; reserved for the M7, outside Linux RAM), cached on the M7
 *
 * On a PCIe endpoint the same core gets a BAR, an outbound window and MSIs.
 *
 * Copyright (c) 2026, imx95-devenv contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>

#include <vxhci/vxhci.h>

#include "a55_irq.h"
#include "m7shm.h"

#define HOST_MEM_START	0x90000000U
#define HOST_MEM_END	0xe0000000U

#define VXHCI_REGS_OFF	0x1000	/* in the m7shm window, after the virtual EHCI */
#define VXHCI_REGS	(M7SHM_BASE + VXHCI_REGS_OFF)

#define DRIVE_NODE	DT_NODELABEL(vxhci_drive)
#define DRIVE_BASE	DT_REG_ADDR(DRIVE_NODE)
#define DRIVE_SIZE	DT_REG_SIZE(DRIVE_NODE)

BUILD_ASSERT(VXHCI_REGS_OFF + VXHCI_REGS_SIZE <= M7SHM_SIZE);

static void *drive;

static bool host_range_ok(uint64_t addr, size_t len)
{
	return addr >= HOST_MEM_START && addr < HOST_MEM_END && len <= HOST_MEM_END - addr;
}

static int host_read(void *ctx, uint64_t addr, void *buf, size_t len)
{
	if (!host_range_ok(addr, len)) {
		return -EFAULT;
	}
	memcpy(buf, (const void *)(uintptr_t)addr, len);
	return 0;
}

static int host_write(void *ctx, uint64_t addr, const void *buf, size_t len)
{
	if (!host_range_ok(addr, len)) {
		return -EFAULT;
	}
	memcpy((void *)(uintptr_t)addr, buf, len);
	return 0;
}

static void irq_set(void *ctx, bool asserted)
{
	a55_irq_set(A55_IRQ_VXHCI, asserted);
}

static int vxhci_a55_init(void)
{
	static const struct vxhci_transport t = {
		.regs = (volatile uint32_t *)VXHCI_REGS,
		.host_read = host_read,
		.host_write = host_write,
		.irq_set = irq_set,
	};

	memset((void *)DRIVE_BASE, 0, DRIVE_SIZE);	/* a blank disk */
	drive = vxhci_drive_init((void *)DRIVE_BASE, DRIVE_SIZE);
	vxhci_start(&t);
	vxhci_port_attach(1, &vxhci_drive_ops, drive, VXHCI_SPEED_SUPER);
	return 0;
}
SYS_INIT(vxhci_a55_init, APPLICATION, 99);

static int cmd_status(const struct shell *sh, size_t argc, char **argv)
{
	volatile uint32_t *r = (volatile uint32_t *)VXHCI_REGS;
	struct vxhci_stats s;

	vxhci_get_stats(&s);
	shell_print(sh, "Wimmo virtual xHCI, registers at 0x%08x (Linux 0x%08x): %s",
		    (uint32_t)VXHCI_REGS, (uint32_t)VXHCI_REGS + 0x400000, vxhci_state());
	shell_print(sh, "USBCMD 0x%08x USBSTS 0x%08x CONFIG 0x%08x DCBAAP 0x%08x", r[0x40 / 4],
		    r[0x44 / 4], r[0x78 / 4], r[0x70 / 4]);
	shell_print(sh, "PORTSC1 (USB 3) 0x%08x PORTSC2 (USB 2) 0x%08x", r[0x440 / 4], r[0x450 / 4]);
	shell_print(sh, "IMAN 0x%08x IMOD 0x%08x ERSTSZ %u ERSTBA 0x%08x ERDP 0x%08x", r[0x620 / 4],
		    r[0x624 / 4], r[0x628 / 4], r[0x630 / 4], r[0x638 / 4]);
	shell_print(sh, "polls %u, doorbells %u, commands %u, TRBs %u, events %u, irqs %u, "
		    "NAKs %u, errors %u, %llu bytes", s.polls, s.doorbells, s.commands, s.trbs,
		    s.events, s.irqs, s.naks, s.errors, s.bytes);
	shell_print(sh, "drive: %u MiB at 0x%08x, %s", (uint32_t)(DRIVE_SIZE >> 20),
		    (uint32_t)DRIVE_BASE, vxhci_drive_null() ? "null mode (no data copy)" : "RAM");
	return 0;
}

/*
 * M7 load while the host moves data: the emulation thread polls all the time
 * while active (lowest priority), so count the cycles of polls that did
 * something; with the core's idle time for comparison.
 */
static int cmd_perf(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t secs = argc > 1 ? strtoul(argv[1], NULL, 0) : 5;
	struct vxhci_stats a, b;
	k_thread_runtime_stats_t c0, c1;
	int64_t t0, t1;

	k_thread_runtime_stats_cpu_get(0, &c0);
	vxhci_get_stats(&a);
	t0 = k_uptime_get();
	k_sleep(K_SECONDS(secs));
	t1 = k_uptime_get();
	vxhci_get_stats(&b);
	k_thread_runtime_stats_cpu_get(0, &c1);

	uint64_t bytes = b.bytes - a.bytes;
	uint64_t work = b.work_cycles - a.work_cycles;
	uint64_t poll = b.poll_cycles - a.poll_cycles;
	uint64_t cpu_hz = 800000000ULL;
	uint64_t el = (uint64_t)(t1 - t0) * (cpu_hz / 1000);
	uint64_t tot = c1.execution_cycles - c0.execution_cycles;
	uint64_t idle = c1.idle_cycles - c0.idle_cycles;
	uint32_t trbs = b.trbs - a.trbs;

	shell_print(sh, "%u s: %llu bytes (%llu KB/s), %u TRBs, %u doorbells, %u events, %u irqs",
		    secs, bytes, bytes / secs / 1000, trbs, b.doorbells - a.doorbells,
		    b.events - a.events, b.irqs - a.irqs);
	shell_print(sh, "M7 work: vxHCI %llu.%02llu %% of the core (polling incl. idle polls %llu %%, "
		    "%s)", work * 100 / el, work * 10000 / el % 100, poll * 100 / el,
		    vxhci_adaptive() ? "adaptive" : "busy");
	if (tot) {
		shell_print(sh, "core: idle %llu.%02llu %%", idle * 100 / tot, idle * 10000 / tot % 100);
	}
	if (trbs) {
		shell_print(sh, "per TRB: %llu cycles, %llu bytes", work / trbs, bytes / trbs);
	}
	if (bytes) {
		uint64_t cpb100 = work * 100 / bytes;

		shell_print(sh, "cost %llu.%02llu cycles/byte -> data rate at 100 %% M7 work: %llu Mbit/s",
			    cpb100 / 100, cpb100 % 100, cpb100 ? cpu_hz * 8 * 100 / cpb100 / 1000000 : 0);
	}
	return 0;
}

static int cmd_null(const struct shell *sh, size_t argc, char **argv)
{
	if (argc > 1) {
		vxhci_drive_set_null(strcmp(argv[1], "off") != 0);
	}
	shell_print(sh, "drive data: %s", vxhci_drive_null() ? "null (no copy)" : "RAM");
	return 0;
}

static int cmd_poll(const struct shell *sh, size_t argc, char **argv)
{
	if (argc > 1) {
		vxhci_set_adaptive(strcmp(argv[1], "busy") != 0);
	}
	shell_print(sh, "polling: %s", vxhci_adaptive() ? "adaptive" : "busy");
	return 0;
}

static int cmd_plug(const struct shell *sh, size_t argc, char **argv)
{
	return vxhci_port_attach(1, &vxhci_drive_ops, drive, VXHCI_SPEED_SUPER);
}

static int cmd_unplug(const struct shell *sh, size_t argc, char **argv)
{
	vxhci_port_detach(1);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(vxhci_cmds,
	SHELL_CMD(status, NULL, "registers, state and counters", cmd_status),
	SHELL_CMD_ARG(perf, NULL, "[seconds] throughput and M7 load", cmd_perf, 1, 1),
	SHELL_CMD(plug, NULL, "attach the virtual USB 3.0 drive (port 1)", cmd_plug),
	SHELL_CMD(unplug, NULL, "detach it", cmd_unplug),
	SHELL_CMD_ARG(null, NULL, "[on|off] drive without data copy (protocol cost only)",
		      cmd_null, 1, 1),
	SHELL_CMD_ARG(poll, NULL, "[busy|adaptive] emulation polling", cmd_poll, 1, 1),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(vxhci, &vxhci_cmds, "Wimmo virtual xHCI for Linux", NULL);
