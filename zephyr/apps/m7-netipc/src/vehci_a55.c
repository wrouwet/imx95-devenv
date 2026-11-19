/*
 * Virtual EHCI ("Wimmo", modules/devenv subsys/vehci) for Linux on the A55.
 *
 * Transport for the A55 stand-in of the later PCIe host:
 *  - registers: the first 4 KiB of the m7shm window (DTCM 0x20020000, Linux
 *    0x20420000), mapped by Linux's stock ehci-platform driver
 *  - host memory: Linux DDR 0x90000000-0xDFFFFFFF, read and written directly
 *    (SM grant, non-cacheable MPU regions in the overlay)
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
		.irq_set = irq_set,
	};

	(void)sys_read32(VEHCI_MSGINTR_BASE + MSGINTR_MSIR);	/* start deasserted */
	vehci_start(&t);
#if defined(CONFIG_APP_VEHCI_TESTDEV)
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
	return 0;
}

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
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(vehci, &vehci_cmds, "Wimmo virtual EHCI for Linux", NULL);
