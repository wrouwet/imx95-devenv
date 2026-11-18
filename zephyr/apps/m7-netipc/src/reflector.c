/*
 * Layer-2 reflector for latency measurement.
 *
 * Every Ethernet frame with EtherType 0x88B5 (IEEE 802 "local experimental")
 * that reaches the M7's VF is sent straight back to its sender: destination
 * MAC := source MAC, source MAC := our MAC, payload untouched. The handler runs
 * in the NETC driver's RX thread (CONFIG_NET_TC_RX_COUNT=0, no RX queue hop)
 * and calls the driver's send() directly (no L2/TX queue), so the path is
 * driver RX copy -> handler -> driver TX copy.
 *
 * Measure with host/l2rtt on a PC on the same link. "m7 reflector" on the
 * shell prints counters and the time spent in the handler.
 *
 * Copyright (c) 2026, imx95-devenv contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/net/ethernet.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_pkt.h>
#include <zephyr/shell/shell.h>
#include <zephyr/timing/timing.h>

#define REFLECT_ETHERTYPE 0x88B5

static struct {
	uint32_t reflected;
	uint32_t errors;
	uint64_t cyc_min;
	uint64_t cyc_max;
	uint64_t cyc_sum;
} stats = {.cyc_min = UINT64_MAX};

static enum net_verdict reflect(struct net_if *iface, uint16_t ptype, struct net_pkt *pkt)
{
	const struct device *dev = net_if_get_device(iface);
	const struct ethernet_api *api = dev->api;
	const struct net_linkaddr *own = net_if_get_link_addr(iface);
	timing_t t0 = timing_counter_get();
	timing_t t1;
	struct net_eth_hdr *hdr;
	uint64_t dt;
	int ret;

	ARG_UNUSED(ptype);

	/* The L2 has pulled the 14-byte header off the first fragment; the bytes
	 * are still in front of the data pointer.
	 */
	if (net_buf_headroom(pkt->frags) < sizeof(*hdr)) {
		stats.errors++;
		return NET_DROP;
	}
	hdr = net_buf_push(pkt->frags, sizeof(*hdr));
	if (memcmp(hdr->src.addr, own->addr, sizeof(hdr->src.addr)) == 0) {
		return NET_DROP; /* our own frame; never bounce it again */
	}
	memcpy(hdr->dst.addr, hdr->src.addr, sizeof(hdr->dst.addr));
	memcpy(hdr->src.addr, own->addr, sizeof(hdr->src.addr));
	net_pkt_cursor_init(pkt);

	net_if_tx_lock(iface);
	ret = api->send(dev, pkt);
	net_if_tx_unlock(iface);
	net_pkt_unref(pkt);

	t1 = timing_counter_get();
	dt = timing_cycles_get(&t0, &t1);
	if (ret == 0) {
		stats.reflected++;
		stats.cyc_sum += dt;
		stats.cyc_min = MIN(stats.cyc_min, dt);
		stats.cyc_max = MAX(stats.cyc_max, dt);
	} else {
		stats.errors++;
	}
	return NET_OK;
}

ETH_NET_L3_REGISTER(reflector, REFLECT_ETHERTYPE, reflect);

/* The kernel cycle counter runs at 32768 Hz on this board; time the handler
 * with the core's DWT cycle counter instead.
 */
static int reflector_timing_init(void)
{
	timing_init();
	timing_start();
	return 0;
}
SYS_INIT(reflector_timing_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

static uint32_t cyc_to_ns(uint64_t cyc)
{
	return (uint32_t)timing_cycles_to_ns(cyc);
}

static int cmd_reflector(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t n = stats.reflected;

	if (argc > 1 && strcmp(argv[1], "reset") == 0) {
		memset(&stats, 0, sizeof(stats));
		stats.cyc_min = UINT64_MAX;
		shell_print(sh, "reflector counters reset");
		return 0;
	}
	shell_print(sh, "ethertype 0x%04x: %u reflected, %u errors", REFLECT_ETHERTYPE, n,
		    stats.errors);
	if (n > 0) {
		shell_print(sh, "handler (header swap + driver send): min %u ns, avg %u ns, max %u ns",
			    cyc_to_ns(stats.cyc_min), cyc_to_ns(stats.cyc_sum / n),
			    cyc_to_ns(stats.cyc_max));
		shell_print(sh, "(DWT cycle counter at %u MHz)", timing_freq_get_mhz());
	}
	return 0;
}

SHELL_CMD_ARG_REGISTER(reflector, NULL, "L2 reflector counters ('reflector reset' clears them)",
		       cmd_reflector, 1, 1);
