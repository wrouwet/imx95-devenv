/*
 * RPMsg link to Linux on the A55 (OpenAMP, Linux = host, M7 = remote).
 *
 * Endpoints announced to Linux:
 *   "rpmsg-tty"            Zephyr shell  -> /dev/ttyRPMSG0 (rpmsg_tty)
 *   "rpmsg-client-sample"  echo service  -> Linux rpmsg_client_sample module
 *
 * Platform/vdev setup adapted from zephyr samples/subsys/ipc/openamp_rsc_table
 * Copyright (c) 2020, STMICROELECTRONICS
 * Copyright (c) 2026, imx95-devenv contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/ipm.h>
#include <zephyr/shell/shell_rpmsg.h>
#include <zephyr/logging/log.h>
#include <zephyr/version.h>
#include <string.h>

#include <openamp/open_amp.h>
#include <metal/sys.h>
#include <metal/io.h>
#include <resource_table.h>
#include <addr_translation.h>

#include "app.h"

LOG_MODULE_REGISTER(m7_rpmsg, LOG_LEVEL_INF);

#if !DT_HAS_CHOSEN(zephyr_ipc_shm)
#error "zephyr,ipc_shm must be defined (see boards/*.overlay)"
#endif

#define SHM_NODE       DT_CHOSEN(zephyr_ipc_shm)
#define SHM_START_ADDR DT_REG_ADDR(SHM_NODE)
#define SHM_SIZE       DT_REG_SIZE(SHM_NODE)

#if CONFIG_IPM_MAX_DATA_SIZE > 0
#define IPM_SEND(dev, w, id, d, s) ipm_send(dev, w, id, d, s)
#else
#define IPM_SEND(dev, w, id, d, s) ipm_send(dev, w, id, NULL, 0)
#endif

static const struct device *const ipm_handle = DEVICE_DT_GET(DT_CHOSEN(zephyr_ipc));

static metal_phys_addr_t shm_physmap = SHM_START_ADDR;
static metal_phys_addr_t rsc_tab_physmap;
static struct metal_io_region shm_io_data;
static struct metal_io_region rsc_io_data;
static struct rpmsg_virtio_device rvdev;
static void *rsc_table;
static struct rpmsg_device *rpdev;

static struct rpmsg_endpoint echo_ept;
static K_SEM_DEFINE(ipm_sem, 0, 1);

static void ipm_cb(const struct device *dev, void *context, uint32_t id, volatile void *data)
{
	k_sem_give(&ipm_sem);
}

static int mailbox_notify(void *priv, uint32_t id)
{
	IPM_SEND(ipm_handle, 0, id, &id, 4);
	return 0;
}

static void ns_cb(struct rpmsg_device *rdev, const char *name, uint32_t src)
{
	LOG_WRN("unexpected name service announcement from Linux: %s", name);
}

/* Echo everything back, prefixed - pairs with Linux's rpmsg_client_sample */
static int echo_cb(struct rpmsg_endpoint *ept, void *data, size_t len, uint32_t src, void *priv)
{
	char buf[RPMSG_BUFFER_SIZE];
	int n = snprintf(buf, sizeof(buf), "M7 echo: ");

	len = MIN(len, sizeof(buf) - n);
	memcpy(buf + n, data, len);
	rpmsg_send(ept, buf, n + len);
	return RPMSG_SUCCESS;
}

static int platform_init(void)
{
	struct metal_init_params metal_params = METAL_INIT_DEFAULTS;
	int rsc_size;
	int ret;

	ret = metal_init(&metal_params);
	if (ret) {
		LOG_ERR("metal_init failed: %d", ret);
		return ret;
	}

	metal_io_init(&shm_io_data, (void *)SHM_START_ADDR, &shm_physmap, SHM_SIZE, -1, 0,
		      addr_translation_get_ops(shm_physmap));

	/*
	 * Use the resource table Linux placed at the shared rsc_table address
	 * (0x88220000). Linux imx_rproc copies its cached table (with the vring
	 * addresses it allocated) there before starting the M7 and later writes
	 * the virtio status into it. CONFIG_OPENAMP_COPY_RSC_TABLE would memcpy
	 * the firmware defaults over that copy at boot and break vring setup
	 * ("failed to create RPMsg virtio device", seen on hardware 2026-11-16),
	 * so it is off. The .resource_table section in the ELF is what Linux
	 * parses at load time.
	 */
	rsc_table = (void *)DT_REG_ADDR(DT_CHOSEN(zephyr_ipc_rsc_table));
	rsc_size = sizeof(struct fw_resource_table);
	rsc_tab_physmap = (uintptr_t)rsc_table;
	metal_io_init(&rsc_io_data, rsc_table, &rsc_tab_physmap, rsc_size, -1, 0, NULL);

	if (!device_is_ready(ipm_handle)) {
		LOG_ERR("IPM device not ready");
		return -ENODEV;
	}
	ipm_register_callback(ipm_handle, ipm_cb, NULL);
	return ipm_set_enabled(ipm_handle, 1);
}

static struct rpmsg_device *create_vdev(void)
{
	struct fw_rsc_vdev_vring *vr;
	struct virtio_device *vdev;

	vdev = rproc_virtio_create_vdev(VIRTIO_DEV_DEVICE, VDEV_ID, rsc_table_to_vdev(rsc_table),
					&rsc_io_data, NULL, mailbox_notify, NULL);
	if (!vdev) {
		return NULL;
	}

	/* Blocks until Linux (remoteproc/virtio) has set up its side */
	rproc_virtio_wait_remote_ready(vdev);

	vr = rsc_table_get_vring0(rsc_table);
	if (rproc_virtio_init_vring(vdev, 0, vr->notifyid, (void *)vr->da, &rsc_io_data, vr->num,
				    vr->align)) {
		goto fail;
	}
	vr = rsc_table_get_vring1(rsc_table);
	if (rproc_virtio_init_vring(vdev, 1, vr->notifyid, (void *)vr->da, &rsc_io_data, vr->num,
				    vr->align)) {
		goto fail;
	}
	if (rpmsg_init_vdev(&rvdev, vdev, ns_cb, &shm_io_data, NULL)) {
		goto fail;
	}
	return rpmsg_virtio_get_rpmsg_device(&rvdev);

fail:
	rproc_virtio_remove_vdev(vdev);
	return NULL;
}

static void rpmsg_thread(void *a, void *b, void *c)
{
	if (platform_init()) {
		return;
	}

	LOG_INF("waiting for Linux to bring up the RPMsg link");
	rpdev = create_vdev();
	if (!rpdev) {
		LOG_ERR("failed to create RPMsg virtio device");
		return;
	}

	if (shell_backend_rpmsg_init_transport(rpdev)) {
		LOG_ERR("RPMsg shell backend failed");
	}
	if (rpmsg_create_ept(&echo_ept, rpdev, "rpmsg-client-sample", RPMSG_ADDR_ANY,
			     RPMSG_ADDR_ANY, echo_cb, NULL)) {
		LOG_ERR("echo endpoint failed");
	}
	LOG_INF("RPMsg link up: shell on rpmsg-tty, echo on rpmsg-client-sample");
	shell_print(shell_backend_rpmsg_get_ptr(),
		    "\n*** i.MX95 Cortex-M7 online: Zephyr %s, m7-netipc, board %s ***\n"
		    "*** RPMsg shell from Linux. Try: m7 status | net iface | kernel threads ***",
		    KERNEL_VERSION_STRING, CONFIG_BOARD_TARGET);
	app_rpmsg_up = true;

	while (1) {
		k_sem_take(&ipm_sem, K_FOREVER);
		rproc_virtio_notified(rvdev.vdev, VRING1_ID);
	}
}

K_THREAD_DEFINE(rpmsg_tid, 4096, rpmsg_thread, NULL, NULL, NULL, K_PRIO_COOP(8), 0, 0);
