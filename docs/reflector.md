# L2 reflector on the M7 (latency measurement)

The M7 firmware (`zephyr/apps/m7-netipc`) sends every Ethernet frame with EtherType **0x88B5**
(IEEE 802 local experimental) straight back to its sender. A peer on the same link timestamps
the transmit and the receive and gets the round-trip time through the M7.

| | |
|---|---|
| Port | the EVK RJ45 that Linux calls `eth0` (ENETC0); the M7 owns VF1 on it |
| Reflector MAC | `02:04:9f:95:00:07` (fixed; see [ethernet.md](ethernet.md)) |
| Reflected | untagged frames with EtherType 0x88B5, unicast to the MAC above or broadcast |
| Rewrite | dst := incoming src, src := `02:04:9f:95:00:07`; EtherType, payload and length unchanged |
| Not reflected | VLAN-tagged frames (no VLAN support in this build), frames from the reflector's own MAC |

## How it works (stage 1: Zephyr network stack)

`src/reflector.c` registers an EtherType handler with `ETH_NET_L3_REGISTER`. The path per frame:

1. NETC RX interrupt → the VSI driver's RX thread wakes up.
2. The driver copies the frame out of the RX ring into a `net_pkt` (`EP_ReceiveFrameCopy`).
3. The Ethernet L2 dispatches on EtherType. With `CONFIG_NET_TC_RX_COUNT=0` this runs in the driver
   thread: no RX queue and no second thread.
4. The handler swaps the MACs in place and calls the driver's `send()` directly, under the
   interface TX lock: no L2 send path and no TX queue (`CONFIG_NET_TC_TX_COUNT=0`).
5. The driver copies the frame into the TX ring and starts transmission.

ARP, IPv4, the UDP echo on port 7, DHCP and the RPMsg shell keep working alongside it.

Stage 2 (not done): bypass Zephyr's stack completely and busy-poll the VSI rings with the IRQs
off, reusing the RX buffer for TX (no copies), with buffers in OCRAM or TCM instead of DDR.

## Measured (stage 1, 2026-11-17)

Peer: NXP MIMXRT1060-EVK (KSZ8081, 100 Mb/s) on the RJ45. The round trip runs from the RT1060's
TX SFD to its RX SFD, using ENET IEEE 1588 hardware timestamps (40 ns resolution). No losses.

| Frame | Mode | Round trip min / avg / max | Minus one frame's serialization |
|---|---|---|---|
| 78 bytes | broadcast, 10 frames | 34.2 / 37.3 / 51.4 µs (first frame) | ~27 µs |
| 78 bytes | unicast, 100 frames | 33.0 / 35.5 / 47.2 µs | ~26-28 µs |
| 1514 bytes | unicast, 20 frames | 195.4 / 198.3 / 210.0 µs | ~73-76 µs |

The M7's handler time over all 130 frames was 3.07 / 5.68 / 19.6 µs. The remaining M7 turnaround
is the RX interrupt, the driver thread wake-up and the RX copies. The size dependence (~48 µs extra
at 1514 bytes) comes from copying each frame three times through non-cacheable DDR: RX ring →
driver bounce buffer → `net_pkt` → TX ring. Stage 2 targets exactly these costs.

## On the M7

```
m7:~$ reflector
ethertype 0x88b5: 1000 reflected, 0 errors
handler (header swap + driver send): min ... ns, avg ... ns, max ... ns
(DWT cycle counter at ... MHz)
m7:~$ reflector reset
```

The handler time comes from the M7's DWT cycle counter. It covers the MAC swap plus the driver's
TX copy and submit, but not the RX interrupt, the thread wake-up or the RX copy.

## Measuring from a Linux PC: `host/l2rtt`

```sh
make -C host/l2rtt
sudo setcap cap_net_raw,cap_net_admin+ep host/l2rtt/l2rtt   # once per build (or run with sudo)
host/l2rtt/l2rtt -i enp0s31f6 -d 02:04:9f:95:00:07 -c 10000 -p 200
```

It sends one frame at a time and reports min/avg/p50/p99/max per timestamp source:

- **hw**: NIC MAC TX timestamp → NIC MAC RX timestamp (`SO_TIMESTAMPING`, hardware). Only the wire,
  the PHYs and the reflector are in this figure. It needs a NIC with hardware timestamps
  (`ethtool -T`; e.g. the Intel I219/e1000e). l2rtt switches the NIC to TX on / RX all and restores the
  previous setting on exit.
- **sw**: the kernel's software timestamps. These add the host's driver and NIC queueing.
- **user**: `send()` → `recvmsg()` return. This adds the host scheduler.

Without `-d`, the first frame goes to broadcast and the reflector's MAC is learned from the reply.

## Measuring from another MCU

Any device that can send raw frames works. Example frame:

```
dst 02:04:9f:95:00:07 | src <yours> | 88 b5 | 4-byte sequence number | padding (46..1500 bytes payload)
```

Match the reply on the sequence number; its dst is your MAC. With MAC hardware timestamps (IEEE
1588) on both TX and RX, RX − TX is the round trip through the cable, both PHYs and the M7.
