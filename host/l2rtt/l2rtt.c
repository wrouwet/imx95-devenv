/*
 * l2rtt - round-trip time to an Ethernet layer-2 reflector, measured with NIC
 * hardware timestamps where the NIC supports them.
 *
 * Sends frames with EtherType 0x88B5 (the M7 reflector in
 * zephyr/apps/m7-netipc/src/reflector.c sends them back with the MACs swapped)
 * one at a time and reports, per timestamp source:
 *   hw    NIC MAC TX timestamp -> NIC MAC RX timestamp (wire + reflector only)
 *   sw    kernel driver TX -> kernel RX (adds the host driver/NIC paths)
 *   user  sendto() -> recvmsg() return (adds the host scheduler)
 *
 *   sudo ./l2rtt -i enp0s31f6 [-d MAC] [-c COUNT] [-p USEC] [-s BYTES] [-v]
 *
 * Without -d the first frame goes to ff:ff:ff:ff:ff:ff and the reflector's
 * MAC is learned from its reply; all following frames are unicast to it.
 * Hardware timestamping is switched on for the interface (SIOCSHWTSTAMP, TX on,
 * RX all) and the previous setting is restored on exit.
 *
 * Copyright (c) 2026, imx95-devenv contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <linux/errqueue.h>
#include <linux/if_packet.h>
#include <linux/net_tstamp.h>
#include <linux/sockios.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define ETHERTYPE_L2RTT 0x88B5
#define MAGIC 0x4c325254u /* "L2RT" */
#define MIN_FRAME 60      /* without FCS */
#define MAX_FRAME 1514
#define TIMEOUT_MS 200

struct payload {
	uint32_t magic;
	uint32_t seq;
} __attribute__((packed));

enum { SRC_HW, SRC_SW, SRC_USER, SRC_N };
static const char *const src_name[SRC_N] = {"hw", "sw", "user"};

static volatile sig_atomic_t stop;
static int hw_restore_valid;
static struct hwtstamp_config hw_saved;
static struct ifreq hw_ifr;
static int ioctl_fd = -1;

static void on_signal(int sig)
{
	(void)sig;
	stop = 1;
}

static int64_t ts_ns(const struct timespec *t)
{
	return (int64_t)t->tv_sec * 1000000000LL + t->tv_nsec;
}

static int64_t now_ns(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return ts_ns(&t);
}

static int parse_mac(const char *s, uint8_t mac[6])
{
	unsigned int b[6];

	if (sscanf(s, "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) {
		return -1;
	}
	for (int i = 0; i < 6; i++) {
		if (b[i] > 0xff) {
			return -1;
		}
		mac[i] = (uint8_t)b[i];
	}
	return 0;
}

static void restore_hwtstamp(void)
{
	if (hw_restore_valid) {
		hw_ifr.ifr_data = (char *)&hw_saved;
		if (ioctl(ioctl_fd, SIOCSHWTSTAMP, &hw_ifr) < 0) {
			perror("l2rtt: restoring the hardware timestamp setting");
		}
		hw_restore_valid = 0;
	}
}

/* Switch on NIC timestamping for all frames; returns 1 if it is on. */
static int enable_hwtstamp(int fd, const char *ifname)
{
	struct hwtstamp_config cfg = {.tx_type = HWTSTAMP_TX_ON, .rx_filter = HWTSTAMP_FILTER_ALL};

	ioctl_fd = fd;
	memset(&hw_ifr, 0, sizeof(hw_ifr));
	snprintf(hw_ifr.ifr_name, sizeof(hw_ifr.ifr_name), "%s", ifname);
	hw_ifr.ifr_data = (char *)&hw_saved;
	if (ioctl(fd, SIOCGHWTSTAMP, &hw_ifr) == 0) {
		hw_restore_valid = 1;
		atexit(restore_hwtstamp);
	}
	hw_ifr.ifr_data = (char *)&cfg;
	if (ioctl(fd, SIOCSHWTSTAMP, &hw_ifr) < 0) {
		fprintf(stderr, "l2rtt: no hardware timestamps on %s (%s); using sw/user only\n",
			ifname, strerror(errno));
		return 0;
	}
	if (cfg.tx_type != HWTSTAMP_TX_ON || cfg.rx_filter == HWTSTAMP_FILTER_NONE) {
		fprintf(stderr, "l2rtt: %s refused TX on / RX all timestamping\n", ifname);
		return 0;
	}
	return 1;
}

/* Timestamps from a SCM_TIMESTAMPING control message: [0] sw, [2] hw. */
static void get_cmsg_ts(struct msghdr *msg, int64_t *sw, int64_t *hw)
{
	for (struct cmsghdr *c = CMSG_FIRSTHDR(msg); c != NULL; c = CMSG_NXTHDR(msg, c)) {
		if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_TIMESTAMPING) {
			struct scm_timestamping *t = (struct scm_timestamping *)CMSG_DATA(c);

			if (t->ts[0].tv_sec != 0 || t->ts[0].tv_nsec != 0) {
				*sw = ts_ns(&t->ts[0]);
			}
			if (t->ts[2].tv_sec != 0 || t->ts[2].tv_nsec != 0) {
				*hw = ts_ns(&t->ts[2]);
			}
		}
	}
}

static int cmp_i64(const void *a, const void *b)
{
	int64_t x = *(const int64_t *)a;
	int64_t y = *(const int64_t *)b;

	return (x > y) - (x < y);
}

static void report(const char *name, int64_t *v, size_t n)
{
	double sum = 0;

	if (n == 0) {
		printf("  %-4s  no samples\n", name);
		return;
	}
	qsort(v, n, sizeof(*v), cmp_i64);
	for (size_t i = 0; i < n; i++) {
		sum += (double)v[i];
	}
	printf("  %-4s  min %8.3f  avg %8.3f  p50 %8.3f  p99 %8.3f  max %8.3f us  (%zu)\n", name,
	       v[0] / 1e3, sum / n / 1e3, v[n / 2] / 1e3, v[(n * 99) / 100] / 1e3,
	       v[n - 1] / 1e3, n);
}

static void usage(void)
{
	fprintf(stderr,
		"usage: l2rtt -i IFACE [-d MAC] [-c COUNT] [-p USEC] [-s BYTES] [-v]\n"
		"  -i  host interface on the same link as the board\n"
		"  -d  reflector MAC (default: broadcast once, then learn it)\n"
		"  -c  frames to send (default 1000, 0 = until Ctrl-C)\n"
		"  -p  pause between frames in microseconds (default 1000)\n"
		"  -s  frame size without FCS, %d..%d (default %d)\n"
		"  -v  print every sample\n",
		MIN_FRAME, MAX_FRAME, MIN_FRAME);
	exit(2);
}

int main(int argc, char **argv)
{
	const char *ifname = NULL;
	uint8_t dst[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
	int learn = 1, verbose = 0, opt, hw;
	long count = 1000, pause_us = 1000, size = MIN_FRAME;
	uint8_t frame[MAX_FRAME], rx[MAX_FRAME + 64];
	char ctrl[512];
	int64_t *samples[SRC_N];
	size_t nsamp[SRC_N] = {0};
	long sent = 0, lost = 0;

	while ((opt = getopt(argc, argv, "i:d:c:p:s:vh")) != -1) {
		switch (opt) {
		case 'i': ifname = optarg; break;
		case 'd':
			if (parse_mac(optarg, dst) < 0) {
				usage();
			}
			learn = 0;
			break;
		case 'c': count = strtol(optarg, NULL, 0); break;
		case 'p': pause_us = strtol(optarg, NULL, 0); break;
		case 's': size = strtol(optarg, NULL, 0); break;
		case 'v': verbose = 1; break;
		default: usage();
		}
	}
	if (ifname == NULL || size < MIN_FRAME || size > MAX_FRAME || count < 0 || pause_us < 0) {
		usage();
	}

	int fd = socket(AF_PACKET, SOCK_RAW, htons(ETHERTYPE_L2RTT));

	if (fd < 0) {
		perror("l2rtt: socket (run as root or with CAP_NET_RAW)");
		return 1;
	}
	int ifindex = (int)if_nametoindex(ifname);

	if (ifindex == 0) {
		fprintf(stderr, "l2rtt: no interface %s\n", ifname);
		return 1;
	}
	struct sockaddr_ll sll = {.sll_family = AF_PACKET,
				  .sll_protocol = htons(ETHERTYPE_L2RTT),
				  .sll_ifindex = ifindex};

	if (bind(fd, (struct sockaddr *)&sll, sizeof(sll)) < 0) {
		perror("l2rtt: bind");
		return 1;
	}
	int one = 1;

	/* Don't receive our own transmitted frames back on this socket. */
	(void)setsockopt(fd, SOL_PACKET, PACKET_IGNORE_OUTGOING, &one, sizeof(one));

	struct ifreq ifr = {0};

	snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", ifname);
	if (ioctl(fd, SIOCGIFHWADDR, &ifr) < 0) {
		perror("l2rtt: SIOCGIFHWADDR");
		return 1;
	}

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	hw = enable_hwtstamp(fd, ifname);

	int tsflags = SOF_TIMESTAMPING_TX_SOFTWARE | SOF_TIMESTAMPING_RX_SOFTWARE |
		      SOF_TIMESTAMPING_SOFTWARE | SOF_TIMESTAMPING_OPT_TSONLY;

	if (hw) {
		tsflags |= SOF_TIMESTAMPING_TX_HARDWARE | SOF_TIMESTAMPING_RX_HARDWARE |
			   SOF_TIMESTAMPING_RAW_HARDWARE;
	}
	if (setsockopt(fd, SOL_SOCKET, SO_TIMESTAMPING, &tsflags, sizeof(tsflags)) < 0) {
		perror("l2rtt: SO_TIMESTAMPING");
		return 1;
	}

	size_t cap = count > 0 ? (size_t)count : 1000000;

	for (int s = 0; s < SRC_N; s++) {
		samples[s] = calloc(cap, sizeof(int64_t));
		if (samples[s] == NULL) {
			perror("l2rtt: calloc");
			return 1;
		}
	}

	memset(frame, 0, sizeof(frame));
	memcpy(frame + 6, ifr.ifr_hwaddr.sa_data, 6);
	frame[12] = ETHERTYPE_L2RTT >> 8;
	frame[13] = ETHERTYPE_L2RTT & 0xff;

	printf("l2rtt: %s -> %s, %ld-byte frames, timestamps: %s\n", ifname,
	       learn ? "broadcast (learning the reflector MAC)" : "given MAC", size,
	       hw ? "hw + sw + user" : "sw + user");

	for (uint32_t seq = 1; !stop && (count == 0 || sent < count); seq++) {
		struct payload p = {.magic = htonl(MAGIC), .seq = htonl(seq)};
		int64_t tx_sw = 0, tx_hw = 0, rx_sw = 0, rx_hw = 0, t_user0, t_user1 = 0;
		int got_reply = 0;
		int discovery = learn; /* broadcast frame; not counted as a sample */

		memcpy(frame, dst, 6);
		memcpy(frame + 14, &p, sizeof(p));

		t_user0 = now_ns();
		if (send(fd, frame, (size_t)size, 0) != size) {
			perror("l2rtt: send");
			break;
		}
		sent++;

		/* Wait for the reply and, with hw, for the TX hardware timestamp
		 * (it can arrive after the reply).
		 */
		int64_t deadline = t_user0 + TIMEOUT_MS * 1000000LL;

		while (!stop && (!got_reply || (hw && tx_hw == 0) || tx_sw == 0)) {
			int64_t left = (deadline - now_ns()) / 1000000;
			struct pollfd pfd = {.fd = fd, .events = POLLIN};

			if (left < 0) {
				break;
			}
			if (poll(&pfd, 1, (int)left + 1) <= 0) {
				continue;
			}
			struct iovec iov = {.iov_base = rx, .iov_len = sizeof(rx)};
			struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1,
					     .msg_control = ctrl, .msg_controllen = sizeof(ctrl)};

			if (pfd.revents & POLLERR) {
				if (recvmsg(fd, &msg, MSG_ERRQUEUE | MSG_DONTWAIT) >= 0) {
					get_cmsg_ts(&msg, &tx_sw, &tx_hw);
				}
				continue;
			}
			if (!(pfd.revents & POLLIN)) {
				continue;
			}
			ssize_t n = recvmsg(fd, &msg, MSG_DONTWAIT);
			int64_t t = now_ns();
			struct payload rp;

			if (n < 14 + (ssize_t)sizeof(rp)) {
				continue;
			}
			memcpy(&rp, rx + 14, sizeof(rp));
			if (rp.magic != htonl(MAGIC) || rp.seq != htonl(seq) ||
			    memcmp(rx, ifr.ifr_hwaddr.sa_data, 6) != 0) {
				continue; /* not the reply to this frame */
			}
			t_user1 = t;
			get_cmsg_ts(&msg, &rx_sw, &rx_hw);
			got_reply = 1;
			if (learn) {
				memcpy(dst, rx + 6, 6);
				learn = 0;
				printf("l2rtt: reflector is %02x:%02x:%02x:%02x:%02x:%02x\n", dst[0],
				       dst[1], dst[2], dst[3], dst[4], dst[5]);
			}
		}

		if (!got_reply) {
			if (!stop) {
				lost++;
				if (verbose) {
					printf("seq %u: no reply\n", seq);
				}
			}
		} else if (!discovery) {
			int64_t v[SRC_N] = {
				(tx_hw && rx_hw) ? rx_hw - tx_hw : -1,
				(tx_sw && rx_sw) ? rx_sw - tx_sw : -1,
				t_user1 - t_user0,
			};

			for (int s = 0; s < SRC_N; s++) {
				if (v[s] >= 0 && nsamp[s] < cap) {
					samples[s][nsamp[s]++] = v[s];
				}
			}
			if (verbose) {
				printf("seq %u: hw %.3f  sw %.3f  user %.3f us\n", seq, v[0] / 1e3,
				       v[1] / 1e3, v[2] / 1e3);
			}
		}
		if (pause_us > 0 && !stop) {
			usleep((useconds_t)pause_us);
		}
	}

	printf("l2rtt: %ld sent, %ld lost\n", sent, lost);
	for (int s = 0; s < SRC_N; s++) {
		report(src_name[s], samples[s], nsamp[s]);
	}
	restore_hwtstamp();
	return lost == sent ? 1 : 0;
}
