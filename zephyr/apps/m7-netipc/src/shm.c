/*
 * Memory window in the M7 DTCM, shared with Linux.
 *
 * The window is zeroed once at boot, before Linux can see the M7 as running
 * through remoteproc (Linux marks the window online once the M7 started),
 * so both sides start from known contents. Nothing else here touches it;
 * the shell commands below are for testing from the M7 side and match
 * m7shm-tool on Linux ("seq" = each word holds its own byte offset).
 *
 * Copyright (c) 2026, imx95-devenv contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <string.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/timing/timing.h>

#include "m7shm.h"

static int m7shm_init(void)
{
	volatile uint32_t *w = m7shm_base();

	for (size_t i = 0; i < M7SHM_SIZE / 4; i++) {
		w[i] = 0;
	}
	return 0;
}
SYS_INIT(m7shm_init, PRE_KERNEL_1, 0);

static int parse_u32(const struct shell *sh, const char *s, uint32_t *v)
{
	char *end;

	*v = strtoul(s, &end, 0);
	if (*s == '\0' || *end != '\0') {
		shell_error(sh, "bad number: %s", s);
		return -EINVAL;
	}
	return 0;
}

/* [OFF [LEN]] from argv[i]; default: from OFF to the end of the window */
static int parse_range(const struct shell *sh, size_t argc, char **argv, size_t i,
		       uint32_t *off, uint32_t *len)
{
	*off = 0;
	if (argc > i && parse_u32(sh, argv[i], off)) {
		return -EINVAL;
	}
	*len = *off <= M7SHM_SIZE ? M7SHM_SIZE - *off : 0;
	if (argc > i + 1 && parse_u32(sh, argv[i + 1], len)) {
		return -EINVAL;
	}
	if (((*off | *len) & 3) || *off > M7SHM_SIZE || *len > M7SHM_SIZE - *off) {
		shell_error(sh, "range 0x%x+0x%x outside the window (0x%x bytes) or not 4-byte aligned",
			    *off, *len, M7SHM_SIZE);
		return -EINVAL;
	}
	return 0;
}

static int cmd_info(const struct shell *sh, size_t argc, char **argv)
{
	shell_print(sh, "M7 address:         0x%08x", (uint32_t)M7SHM_BASE);
	shell_print(sh, "host (A55) address: 0x%08x", (uint32_t)M7SHM_BASE + 0x400000);
	shell_print(sh, "size:               %u bytes (%u KiB)", M7SHM_SIZE, M7SHM_SIZE / 1024);
	return 0;
}

static int cmd_dump(const struct shell *sh, size_t argc, char **argv)
{
	volatile uint32_t *w = m7shm_base();
	uint32_t off, len;

	if (parse_range(sh, argc, argv, 1, &off, &len)) {
		return -EINVAL;
	}
	if (argc <= 2 && len > 256) {
		len = 256;
	}
	for (uint32_t o = off; o < off + len; o += 16) {
		char line[64];
		int n = snprintk(line, sizeof(line), "%05x:", o);

		for (uint32_t k = o; k < o + 16 && k < off + len; k += 4) {
			n += snprintk(line + n, sizeof(line) - n, " %08x", w[k / 4]);
		}
		shell_print(sh, "%s", line);
	}
	return 0;
}

static int cmd_write(const struct shell *sh, size_t argc, char **argv)
{
	volatile uint32_t *w = m7shm_base();
	uint32_t off, val;

	if (parse_u32(sh, argv[1], &off) || parse_u32(sh, argv[2], &val)) {
		return -EINVAL;
	}
	if ((off & 3) || off >= M7SHM_SIZE) {
		shell_error(sh, "offset must be 4-byte aligned and below 0x%x", M7SHM_SIZE);
		return -EINVAL;
	}
	w[off / 4] = val;
	shell_print(sh, "%05x: %08x", off, w[off / 4]);
	return 0;
}

static int fill_or_check(const struct shell *sh, size_t argc, char **argv, bool check)
{
	volatile uint32_t *w = m7shm_base();
	bool seq = strcmp(argv[1], "seq") == 0;
	uint32_t off, len, val = 0, bad = 0;

	if ((!seq && parse_u32(sh, argv[1], &val)) || parse_range(sh, argc, argv, 2, &off, &len)) {
		return -EINVAL;
	}
	for (uint32_t o = off; o < off + len; o += 4) {
		uint32_t want = seq ? o : val;

		if (!check) {
			w[o / 4] = want;
		} else if (w[o / 4] != want && !bad++) {
			shell_print(sh, "first mismatch at 0x%05x: got %08x want %08x", o, w[o / 4],
				    want);
		}
	}
	if (check) {
		shell_print(sh, "check 0x%x+0x%x: %s (%u bad words)", off, len, bad ? "FAIL" : "OK",
			    bad);
		return bad ? -EIO : 0;
	}
	shell_print(sh, "filled 0x%x+0x%x with %s", off, len, argv[1]);
	return 0;
}

static int cmd_fill(const struct shell *sh, size_t argc, char **argv)
{
	return fill_or_check(sh, argc, argv, false);
}

static int cmd_check(const struct shell *sh, size_t argc, char **argv)
{
	return fill_or_check(sh, argc, argv, true);
}

static int cmd_bench(const struct shell *sh, size_t argc, char **argv)
{
	volatile uint32_t *w = m7shm_base();
	const uint32_t words = M7SHM_SIZE / 4;
	timing_t t0, t1;
	uint32_t sum = 0;
	uint64_t ns;

	timing_init();
	timing_start();

	t0 = timing_counter_get();
	for (uint32_t i = 0; i < words; i++) {
		w[i] = i;
	}
	t1 = timing_counter_get();
	ns = timing_cycles_to_ns(timing_cycles_get(&t0, &t1));
	shell_print(sh, "write: %u MB/s, %u ns per 32-bit store", (uint32_t)(M7SHM_SIZE * 1000ULL / ns),
		    (uint32_t)(ns / words));

	t0 = timing_counter_get();
	for (uint32_t i = 0; i < words; i++) {
		sum += w[i];
	}
	t1 = timing_counter_get();
	ns = timing_cycles_to_ns(timing_cycles_get(&t0, &t1));
	shell_print(sh, "read:  %u MB/s, %u ns per 32-bit load (sum %08x)",
		    (uint32_t)(M7SHM_SIZE * 1000ULL / ns), (uint32_t)(ns / words), sum);

	timing_stop();
	for (uint32_t i = 0; i < words; i++) {
		w[i] = 0;
	}
	shell_print(sh, "window zeroed");
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(shm_cmds,
	SHELL_CMD_ARG(info, NULL, "window addresses and size", cmd_info, 1, 0),
	SHELL_CMD_ARG(dump, NULL, "[OFF [LEN]]  hex dump (default 256 bytes)", cmd_dump, 1, 2),
	SHELL_CMD_ARG(write, NULL, "OFF VALUE  write one 32-bit word", cmd_write, 3, 0),
	SHELL_CMD_ARG(fill, NULL, "VALUE|seq [OFF [LEN]]  fill 32-bit words", cmd_fill, 2, 2),
	SHELL_CMD_ARG(check, NULL, "VALUE|seq [OFF [LEN]]  verify 32-bit words", cmd_check, 2, 2),
	SHELL_CMD_ARG(bench, NULL, "M7 access timing (zeroes the window)", cmd_bench, 1, 0),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(shm, &shm_cmds, "memory window shared with Linux (M7 DTCM)", NULL);
