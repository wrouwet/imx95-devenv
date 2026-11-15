/*
 * m7-netipc: reference Zephyr application for the i.MX95 Cortex-M7.
 *
 *  - loaded and started by Linux via remoteproc (m7ctl run <elf>)
 *  - RPMsg link to Linux: Zephyr shell on /dev/ttyRPMSG0, echo endpoint
 *  - Ethernet on ENETC0 VF1, sharing the RJ45 port with Linux (own MAC/IP)
 *  - "m7 status" shell command summarising all of the above
 *
 * Copyright (c) 2026, imx95-devenv contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/net/net_if.h>
#include <zephyr/logging/log.h>

#include "app.h"

LOG_MODULE_REGISTER(m7_netipc, LOG_LEVEL_INF);

volatile bool app_rpmsg_up;
char app_ip_str[NET_IPV4_ADDR_LEN] = "none";
volatile uint32_t app_udp_echo_count;

static int cmd_m7_status(const struct shell *sh, size_t argc, char **argv)
{
	struct net_if *iface = net_if_get_default();
	char mac[sizeof("xx:xx:xx:xx:xx:xx")] = "-";

	if (iface) {
		struct net_linkaddr *l = net_if_get_link_addr(iface);

		if (l->len == 6) {
			snprintk(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x", l->addr[0],
				 l->addr[1], l->addr[2], l->addr[3], l->addr[4], l->addr[5]);
		}
	}
	shell_print(sh, "m7-netipc on %s, up %u s", CONFIG_BOARD_TARGET,
		    (uint32_t)(k_uptime_get() / 1000));
	shell_print(sh, "rpmsg:    %s", app_rpmsg_up ? "up" : "waiting for Linux");
	shell_print(sh, "ethernet: VF %s, carrier %s, mac %s",
		    iface ? "present" : "missing",
		    (iface && net_if_is_carrier_ok(iface)) ? "up" : "down", mac);
	shell_print(sh, "ipv4:     %s", app_ip_str);
	shell_print(sh, "udp echo: port 7, %u datagrams", app_udp_echo_count);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(m7_cmds,
	SHELL_CMD(status, NULL, "RPMsg/Ethernet status", cmd_m7_status),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(m7, &m7_cmds, "imx95-devenv M7 commands", NULL);

int main(void)
{
	LOG_INF("m7-netipc starting on %s", CONFIG_BOARD_TARGET);
	app_net_start();
	return 0;
}
