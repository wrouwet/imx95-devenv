/*
 * Networking on the M7's ENETC0 VF (VSI), sharing the RJ45 port with Linux.
 *
 * DHCPv4 gets the M7 its own address on the LAN (its own MAC; the NETC
 * hardware steers frames for that MAC to the VF and everything else to the
 * Linux PF). A UDP echo service runs on port 7 so the M7 path can be tested
 * from Linux on the board or any host: `echo hi | nc -u -w1 <m7-ip> 7`.
 *
 * Copyright (c) 2026, imx95-devenv contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/dhcpv4.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/ethernet_mgmt.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/sys/sys_io.h>
#include <string.h>
#include <zephyr/logging/log.h>

#include "app.h"

LOG_MODULE_REGISTER(m7_net, LOG_LEVEL_INF);

#define UDP_ECHO_PORT 7

static struct net_mgmt_event_callback mgmt_cb;

static void ipv4_event(struct net_mgmt_event_callback *cb, uint64_t event, struct net_if *iface)
{
	char buf[NET_IPV4_ADDR_LEN];

	if (event != NET_EVENT_IPV4_ADDR_ADD) {
		return;
	}
	ARRAY_FOR_EACH(iface->config.ip.ipv4->unicast, i) {
		struct net_if_addr *a = &iface->config.ip.ipv4->unicast[i].ipv4;

		if (a->is_used && a->addr_type == NET_ADDR_DHCP) {
			net_addr_ntop(AF_INET, &a->address.in_addr, buf, sizeof(buf));
			snprintk(app_ip_str, sizeof(app_ip_str), "%s", buf);
			LOG_INF("M7 IPv4 address (DHCP): %s", buf);
		}
	}
}

static void udp_echo_thread(void *a, void *b, void *c)
{
	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_port = htons(UDP_ECHO_PORT),
		.sin_addr.s_addr = htonl(INADDR_ANY),
	};
	static char buf[1500];
	int s = zsock_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

	if (s < 0 || zsock_bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		LOG_ERR("UDP echo socket setup failed: %d", errno);
		return;
	}
	LOG_INF("UDP echo listening on port %d", UDP_ECHO_PORT);

	while (1) {
		struct sockaddr_in peer;
		socklen_t plen = sizeof(peer);
		ssize_t n = zsock_recvfrom(s, buf, sizeof(buf), 0, (struct sockaddr *)&peer,
					   &plen);

		if (n > 0) {
			app_udp_echo_count++;
			zsock_sendto(s, buf, n, 0, (struct sockaddr *)&peer, plen);
		}
	}
}

K_THREAD_DEFINE(udp_echo_tid, 4096, udp_echo_thread, NULL, NULL, NULL, 7, 0, 1000);

/*
 * Use the MAC address Linux (the PSI/PF owner) assigned to this VF.
 *
 * Linux enetc4 gives every VF an administrative MAC (`ip link show eth0`:
 * "vf 1 link/ether 02:04:9f:95:00:07") and its MAC filter only accepts that address
 * for the VF, so the PSI rejects any other primary MAC the VSI asks for
 * ("VSI MAC filter not accepted by PSI", seen on hardware 2026-11-16) and
 * unicast frames for it never reach the M7. The assigned address is readable
 * from the VSI's own SIPMAR0/1 registers once the VF is enabled. m7-vf-handoff
 * assigns the same fixed MAC as the overlay's local-mac-address, so normally
 * nothing changes here; this keeps the M7 working if VF_MAC is overridden.
 */
#define VSI_NODE    DT_NODELABEL(enetc0_vsi1)
#define VSI_BASE    DT_REG_ADDR_BY_NAME(VSI_NODE, port)
#define VSI_SIPMAR0 0x80
#define VSI_SIPMAR1 0x84

static void adopt_psi_mac(struct net_if *iface)
{
	struct ethernet_req_params params = {0};
	uint32_t lo, hi;
	int ret;

	/* The SI registers are only valid once Linux has enabled SR-IOV and the
	 * driver brought the SI up (it then reports carrier).
	 */
	for (int i = 0; i < 300 && !net_if_is_carrier_ok(iface); i++) {
		k_msleep(100);
	}
	if (!net_if_is_carrier_ok(iface)) {
		LOG_WRN("VF not up after 30 s; keeping the overlay MAC");
		return;
	}

	lo = sys_read32(VSI_BASE + VSI_SIPMAR0);
	hi = sys_read32(VSI_BASE + VSI_SIPMAR1) & 0xffff;
	if (lo == 0 && hi == 0) {
		LOG_WRN("Linux assigned no VF MAC; keeping the random one");
		return;
	}
	for (int i = 0; i < 4; i++) {
		params.mac_address.addr[i] = (lo >> (8 * i)) & 0xff;
	}
	params.mac_address.addr[4] = hi & 0xff;
	params.mac_address.addr[5] = hi >> 8;

	if (memcmp(params.mac_address.addr, net_if_get_link_addr(iface)->addr, 6) == 0) {
		return;
	}

	/* The MAC can only be changed while the interface is down. */
	net_if_down(iface);
	ret = net_mgmt(NET_REQUEST_ETHERNET_SET_MAC_ADDRESS, iface, &params, sizeof(params));
	net_if_up(iface);
	if (ret) {
		LOG_ERR("setting the Linux-assigned VF MAC failed: %d", ret);
		return;
	}
	const uint8_t *m = params.mac_address.addr;

	LOG_INF("using the VF MAC assigned by Linux: %02x:%02x:%02x:%02x:%02x:%02x",
		m[0], m[1], m[2], m[3], m[4], m[5]);
}

void app_net_start(void)
{
	struct net_if *iface = net_if_get_default();

	if (!iface) {
		LOG_ERR("no network interface (is enetc0_vsi1 enabled?)");
		return;
	}
	net_mgmt_init_event_callback(&mgmt_cb, ipv4_event, NET_EVENT_IPV4_ADDR_ADD);
	net_mgmt_add_event_callback(&mgmt_cb);

	adopt_psi_mac(iface);

	/* DHCP keeps retrying until the link is up. */
	net_dhcpv4_start(iface);
	LOG_INF("DHCPv4 started on the ENETC0 VF");
}
