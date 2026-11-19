// SPDX-License-Identifier: MIT
/*
 * m7shm-tool: poke at the M7 DTCM memory window through /dev/m7shm.
 *
 *   m7shm-tool info
 *   m7shm-tool dump  [OFF [LEN]]
 *   m7shm-tool write OFF VALUE           one 32-bit word
 *   m7shm-tool fill  VALUE|seq [OFF [LEN]]
 *   m7shm-tool check VALUE|seq [OFF [LEN]]
 *   m7shm-tool bench                     A55 access timing over the window
 *
 * "seq" writes/expects each word's own byte offset, so a range filled on one
 * core can be checked on the other (the M7 shell has the same commands).
 * OFF and LEN are bytes, multiples of 4. All accesses are 32-bit.
 */
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "m7shm_uapi.h"

static struct m7shm_info info;
static volatile uint32_t *win;

static void usage(void)
{
	fprintf(stderr,
		"usage: m7shm-tool info | dump [OFF [LEN]] | write OFF VALUE |\n"
		"                  fill VALUE|seq [OFF [LEN]] | check VALUE|seq [OFF [LEN]] | bench\n");
	exit(2);
}

static uint32_t num(const char *s)
{
	char *end;
	unsigned long v = strtoul(s, &end, 0);

	if (*s == '\0' || *end != '\0')
		usage();
	return (uint32_t)v;
}

/* Parse [OFF [LEN]] at argv[i]; default: the whole window from OFF */
static void range(int argc, char **argv, int i, uint32_t *off, uint32_t *len)
{
	*off = argc > i ? num(argv[i]) : 0;
	*len = argc > i + 1 ? num(argv[i + 1]) : info.size - (*off < info.size ? *off : info.size);
	if ((*off | *len) & 3 || *off > info.size || *len > info.size - *off) {
		fprintf(stderr, "range 0x%x+0x%x outside the window (0x%x bytes) or not 4-byte aligned\n",
			*off, *len, info.size);
		exit(2);
	}
}

static double now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void bench(void)
{
	const uint32_t words = info.size / 4;
	const int rounds = 16;
	uint32_t sum = 0;
	double t;

	t = now();
	for (int r = 0; r < rounds; r++)
		for (uint32_t i = 0; i < words; i++)
			win[i] = i;
	t = now() - t;
	printf("write: %.1f MB/s, %.1f ns per 32-bit store\n",
	       rounds * info.size / t / 1e6, t / (rounds * (double)words) * 1e9);

	t = now();
	for (int r = 0; r < rounds; r++)
		for (uint32_t i = 0; i < words; i++)
			sum += win[i];
	t = now() - t;
	printf("read:  %.1f MB/s, %.1f ns per 32-bit load (sum %08" PRIx32 ")\n",
	       rounds * info.size / t / 1e6, t / (rounds * (double)words) * 1e9, sum);

	/* Dependent loads: each address comes from the previous load, so this is latency */
	for (uint32_t i = 0; i < words; i++)
		win[i] = (i + 1) % words;
	t = now();
	uint32_t p = 0;
	for (uint32_t n = 0; n < words * 4; n++)
		p = win[p];
	t = now() - t;
	printf("read latency (dependent loads): %.1f ns (end %" PRIu32 ")\n",
	       t / (words * 4.0) * 1e9, p);
	for (uint32_t i = 0; i < words; i++)
		win[i] = 0;
	printf("window zeroed\n");
}

int main(int argc, char **argv)
{
	uint32_t off, len, val = 0;
	int seq = 0, fd;

	if (argc < 2)
		usage();

	fd = open("/dev/m7shm", O_RDWR | O_SYNC);
	if (fd < 0 || ioctl(fd, M7SHM_IOC_INFO, &info)) {
		perror("/dev/m7shm");
		return 1;
	}

	if (!strcmp(argv[1], "info")) {
		printf("host (A55) address: 0x%08" PRIx64 "\n", (uint64_t)info.host_addr);
		printf("M7 address:         0x%08" PRIx32 "\n", info.m7_addr);
		printf("size:               %" PRIu32 " bytes (%" PRIu32 " KiB)\n", info.size, info.size / 1024);
		printf("state:              %s\n", info.online ? "online (M7 running)" : "offline (M7 stopped)");
		return 0;
	}

	if (!info.online) {
		fprintf(stderr, "window offline: the M7 is not running\n");
		return 1;
	}
	win = mmap(NULL, info.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (win == MAP_FAILED) {
		perror("mmap");
		return 1;
	}

	if (!strcmp(argv[1], "dump")) {
		range(argc, argv, 2, &off, &len);
		if (argc <= 3 && len > 256)
			len = 256;
		for (uint32_t o = off; o < off + len; o += 4) {
			if ((o - off) % 16 == 0)
				printf("%s%05" PRIx32 ":", o == off ? "" : "\n", o);
			printf(" %08" PRIx32, win[o / 4]);
		}
		printf("\n");
	} else if (!strcmp(argv[1], "write") && argc == 4) {
		off = num(argv[2]);
		val = num(argv[3]);
		if (off & 3 || off >= info.size)
			usage();
		win[off / 4] = val;
		printf("%05" PRIx32 ": %08" PRIx32 "\n", off, win[off / 4]);
	} else if ((!strcmp(argv[1], "fill") || !strcmp(argv[1], "check")) && argc >= 3) {
		seq = !strcmp(argv[2], "seq");
		if (!seq)
			val = num(argv[2]);
		range(argc, argv, 3, &off, &len);
		if (argv[1][0] == 'f') {
			for (uint32_t o = off; o < off + len; o += 4)
				win[o / 4] = seq ? o : val;
			printf("filled 0x%" PRIx32 "+0x%" PRIx32 " with %s\n", off, len, seq ? "seq" : argv[2]);
		} else {
			uint32_t bad = 0;

			for (uint32_t o = off; o < off + len; o += 4) {
				uint32_t want = seq ? o : val, got = win[o / 4];

				if (got != want && !bad++)
					printf("first mismatch at 0x%05" PRIx32 ": got %08" PRIx32 " want %08" PRIx32 "\n",
					       o, got, want);
			}
			printf("check 0x%" PRIx32 "+0x%" PRIx32 ": %s (%" PRIu32 " bad words)\n", off, len,
			       bad ? "FAIL" : "OK", bad);
			return bad ? 1 : 0;
		}
	} else if (!strcmp(argv[1], "bench")) {
		bench();
	} else {
		usage();
	}
	return 0;
}
