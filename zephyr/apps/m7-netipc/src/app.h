/* SPDX-License-Identifier: Apache-2.0 */
#ifndef M7_NETIPC_APP_H
#define M7_NETIPC_APP_H

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/net/net_ip.h>

extern volatile bool app_rpmsg_up;
extern char app_ip_str[NET_IPV4_ADDR_LEN];
extern volatile uint32_t app_udp_echo_count;

void app_net_start(void);

#endif
