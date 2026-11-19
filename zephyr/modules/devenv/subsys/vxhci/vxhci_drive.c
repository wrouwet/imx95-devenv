/*
 * Virtual USB 3.0 drive behind the Wimmo virtual xHCI: a SuperSpeed mass
 * storage device (1209:0003, bulk-only transport, SCSI transparent command
 * set) backed by M7 memory.
 *
 * Every bulk TRB is served at once: CBW (31 bytes OUT) -> data (IN from or
 * OUT to the backing store, any number of TRBs) -> CSW (13 bytes IN). When the
 * device has less data than the host asked for, the IN TRB comes back short,
 * which ends the TD, and the CSW reports the residue.
 *
 * Null mode skips the copy between the store and the host buffer, so the
 * xHCI protocol path can be measured on its own.
 *
 * Copyright (c) 2026, imx95-devenv contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include <vxhci/vxhci.h>

LOG_MODULE_DECLARE(vxhci, CONFIG_VXHCI_LOG_LEVEL);

#define BLOCK_SIZE	512
#define EP_IN		1
#define EP_OUT		2

#define CBW_SIG		0x43425355	/* "USBC" */
#define CSW_SIG		0x53425355	/* "USBS" */

enum bot_state { BOT_CBW, BOT_DATA_IN, BOT_DATA_OUT, BOT_CSW, BOT_HALTED };

static const uint8_t dev_desc[18] = {
	18, 1,			/* bLength, DEVICE */
	0x00, 0x03,		/* bcdUSB 3.0 */
	0, 0, 0,		/* class per interface */
	9,			/* bMaxPacketSize0: 2^9 = 512 */
	0x09, 0x12,		/* idVendor 0x1209 (pid.codes) */
	0x03, 0x00,		/* idProduct 0x0003 */
	0x00, 0x01,		/* bcdDevice 1.00 */
	1, 2, 3,		/* strings */
	1,			/* configurations */
};

static const uint8_t cfg_desc[44] = {
	9, 2, 44, 0, 1, 1, 0, 0x80, 50,		/* bus powered, 400 mA (8 mA units) */
	9, 4, 0, 0, 2, 0x08, 0x06, 0x50, 0,	/* mass storage, SCSI, bulk-only */
	7, 5, 0x80 | EP_IN, 2, 0x00, 0x04, 0,	/* bulk IN, 1024 */
	6, 48, 15, 0, 0, 0,			/* SS companion: burst 16 */
	7, 5, EP_OUT, 2, 0x00, 0x04, 0,		/* bulk OUT, 1024 */
	6, 48, 15, 0, 0, 0,
};

static const uint8_t bos_desc[22] = {
	5, 15, 22, 0, 2,			/* BOS, 2 capabilities */
	7, 16, 2, 0, 0, 0, 0,			/* USB 2.0 extension: no LPM */
	10, 16, 3, 0, 0x0e, 0x00, 1, 0, 0, 0,	/* SuperSpeed: FS/HS/SS, no U1/U2 */
};

static const char *const strings[] = {NULL, "Wimmo", "Virtual USB 3.0 drive", "WIMMO0000003"};

static struct drive {
	uint8_t *store;
	uint32_t blocks;
	bool null;
	uint8_t config;

	enum bot_state state;
	uint32_t tag;
	uint32_t host_len;	/* dCBWDataTransferLength */
	bool host_in;		/* bmCBWFlags: data IN */
	uint32_t done;		/* bytes moved in the data phase */
	uint32_t dev_len;	/* bytes the device moves */
	uint8_t status;		/* CSW status */
	const uint8_t *src;	/* IN data: store or resp[] */
	uint8_t *dst;		/* OUT data: store, or NULL to discard */
	uint8_t resp[64];

	uint8_t sense_key, asc, ascq;
} drv;

void vxhci_drive_set_null(bool null)
{
	drv.null = null;
}

bool vxhci_drive_null(void)
{
	return drv.null;
}

static void sense(uint8_t key, uint8_t asc, uint8_t ascq)
{
	drv.sense_key = key;
	drv.asc = asc;
	drv.ascq = ascq;
}

/* --- Control requests --- */

static int string_desc(uint8_t idx, uint8_t *buf, uint16_t *len)
{
	const char *s;
	size_t n;

	if (idx == 0) {
		static const uint8_t lang[] = {4, 3, 0x09, 0x04};

		memcpy(buf, lang, MIN(*len, sizeof(lang)));
		*len = MIN(*len, sizeof(lang));
		return 0;
	}
	if (idx >= ARRAY_SIZE(strings)) {
		return -EPIPE;
	}
	s = strings[idx];
	n = strlen(s);
	uint8_t d[2 + 2 * 32] = {(uint8_t)(2 + 2 * n), 3};

	for (size_t i = 0; i < n; i++) {
		d[2 + 2 * i] = s[i];
		d[3 + 2 * i] = 0;
	}
	*len = MIN(*len, d[0]);
	memcpy(buf, d, *len);
	return 0;
}

static int reply(const void *src, size_t n, uint8_t *buf, uint16_t *len)
{
	*len = MIN(*len, n);
	memcpy(buf, src, *len);
	return 0;
}

static void bot_reset(void)
{
	drv.state = BOT_CBW;
}

static int drive_control(void *ctx, const uint8_t setup[8], uint8_t *buf, uint16_t *len)
{
	uint8_t type = setup[0], req = setup[1];
	uint16_t value = sys_get_le16(&setup[2]);
	uint16_t index = sys_get_le16(&setup[4]);
	static const uint8_t zero2[2];

	switch ((type << 8) | req) {
	case 0x8006:	/* GET_DESCRIPTOR */
		switch (value >> 8) {
		case 1:
			return reply(dev_desc, sizeof(dev_desc), buf, len);
		case 2:
			return reply(cfg_desc, sizeof(cfg_desc), buf, len);
		case 3:
			return string_desc(value & 0xff, buf, len);
		case 15:
			return reply(bos_desc, sizeof(bos_desc), buf, len);
		default:
			return -EPIPE;
		}
	case 0x0009:	/* SET_CONFIGURATION */
		if ((value & 0xff) > 1) {
			return -EPIPE;
		}
		drv.config = value & 0xff;
		bot_reset();
		if (drv.config) {
			LOG_INF("drive: configured (%u MiB)", drv.blocks / 2048);
		}
		return 0;
	case 0x8008:	/* GET_CONFIGURATION */
		return reply(&drv.config, 1, buf, len);
	case 0x8000:	/* GET_STATUS device */
	case 0x8100:	/* interface */
	case 0x8200:	/* endpoint */
		{
			uint8_t st[2] = {0, 0};

			if (type == 0x82 && drv.state == BOT_HALTED &&
			    ((index & 0x7f) == EP_IN || (index & 0x7f) == EP_OUT)) {
				st[0] = 1;
			}
			return reply(st, 2, buf, len);
		}
	case 0x0201:	/* CLEAR_FEATURE(ENDPOINT_HALT) */
		if (value == 0 && drv.state == BOT_HALTED) {
			drv.state = BOT_CBW;
		}
		return 0;
	case 0x0001:	/* CLEAR_FEATURE device (U1/U2 enable) */
	case 0x0003:	/* SET_FEATURE device */
	case 0x0103:	/* SET_FEATURE interface (function suspend) */
	case 0x0101:
	case 0x0203:
	case 0x0030:	/* SET_SEL */
	case 0x0031:	/* SET_ISOCH_DELAY */
		return 0;
	case 0x010b:	/* SET_INTERFACE */
		return value == 0 ? 0 : -EPIPE;
	case 0x810a:	/* GET_INTERFACE */
		return reply(zero2, 1, buf, len);
	case 0x21ff:	/* Bulk-Only Mass Storage Reset */
		bot_reset();
		return 0;
	case 0xa1fe:	/* Get Max LUN */
		return reply(zero2, 1, buf, len);
	default:
		LOG_DBG("drive: control %02x %02x %04x not supported", type, req, value);
		return -EPIPE;
	}
}

/* --- SCSI --- */

static void data_in(const void *src, uint32_t n)
{
	drv.src = src;
	drv.dev_len = MIN(n, drv.host_len);
	drv.state = BOT_DATA_IN;
}

static void fail(uint8_t key, uint8_t asc)
{
	sense(key, asc, 0);
	drv.status = 1;
	drv.dev_len = 0;
}

static bool lba_ok(uint64_t lba, uint32_t n)
{
	if (lba + n > drv.blocks) {
		fail(0x05, 0x21);	/* ILLEGAL REQUEST, LBA out of range */
		return false;
	}
	return true;
}

static void scsi(const uint8_t *cdb)
{
	uint8_t *r = drv.resp;
	uint64_t lba;
	uint32_t n;
	bool out = false;

	drv.status = 0;
	drv.dev_len = 0;
	drv.src = NULL;
	drv.dst = NULL;
	memset(r, 0, sizeof(drv.resp));

	switch (cdb[0]) {
	case 0x00:	/* TEST UNIT READY */
	case 0x1b:	/* START STOP UNIT */
	case 0x1e:	/* PREVENT ALLOW MEDIUM REMOVAL */
	case 0x35:	/* SYNCHRONIZE CACHE (10) */
	case 0x91:	/* SYNCHRONIZE CACHE (16) */
	case 0x2f:	/* VERIFY (10) */
		break;
	case 0x03:	/* REQUEST SENSE */
		r[0] = 0x70;
		r[2] = drv.sense_key;
		r[7] = 10;
		r[12] = drv.asc;
		r[13] = drv.ascq;
		sense(0, 0, 0);
		data_in(r, MIN(18, cdb[4]));
		break;
	case 0x12:	/* INQUIRY */
		if (cdb[1] & 1) {	/* VPD */
			if (cdb[2] == 0x00) {
				static const uint8_t pages[] = {0, 0, 0, 2, 0x00, 0x80};

				memcpy(r, pages, sizeof(pages));
				data_in(r, MIN(sizeof(pages), sys_get_be16(&cdb[3])));
			} else if (cdb[2] == 0x80) {
				r[1] = 0x80;
				r[3] = 12;
				memcpy(&r[4], strings[3], 12);
				data_in(r, MIN(16, sys_get_be16(&cdb[3])));
			} else {
				fail(0x05, 0x24);	/* invalid field in CDB */
			}
			break;
		}
		r[0] = 0x00;		/* direct access block device */
		r[2] = 0x06;		/* SPC-4 */
		r[3] = 0x02;		/* response data format */
		r[4] = 31;
		memcpy(&r[8], "Wimmo   ", 8);
		memcpy(&r[16], "USB 3.0 Drive   ", 16);
		memcpy(&r[32], "0100", 4);
		data_in(r, MIN(36, sys_get_be16(&cdb[3])));
		break;
	case 0x25:	/* READ CAPACITY (10) */
		sys_put_be32(drv.blocks - 1, &r[0]);
		sys_put_be32(BLOCK_SIZE, &r[4]);
		data_in(r, 8);
		break;
	case 0x9e:	/* SERVICE ACTION IN: READ CAPACITY (16) */
		if ((cdb[1] & 0x1f) != 0x10) {
			fail(0x05, 0x20);
			break;
		}
		sys_put_be64(drv.blocks - 1, &r[0]);
		sys_put_be32(BLOCK_SIZE, &r[8]);
		data_in(r, MIN(32, sys_get_be32(&cdb[10])));
		break;
	case 0x1a:	/* MODE SENSE (6): header only, not write protected */
		r[0] = 3;
		data_in(r, MIN(4, cdb[4]));
		break;
	case 0x5a:	/* MODE SENSE (10) */
		r[1] = 6;
		data_in(r, MIN(8, sys_get_be16(&cdb[7])));
		break;
	case 0x28:	/* READ (10) */
	case 0x2a:	/* WRITE (10) */
		lba = sys_get_be32(&cdb[2]);
		n = sys_get_be16(&cdb[7]);
		out = cdb[0] == 0x2a;
		goto rw;
	case 0xa8:	/* READ (12) */
	case 0xaa:	/* WRITE (12) */
		lba = sys_get_be32(&cdb[2]);
		n = sys_get_be32(&cdb[6]);
		out = cdb[0] == 0xaa;
		goto rw;
	case 0x88:	/* READ (16) */
	case 0x8a:	/* WRITE (16) */
		lba = sys_get_be64(&cdb[2]);
		n = sys_get_be32(&cdb[10]);
		out = cdb[0] == 0x8a;
rw:
		if (!lba_ok(lba, n)) {
			break;
		}
		if (out) {
			drv.dst = drv.store + lba * BLOCK_SIZE;
			drv.dev_len = MIN(n * BLOCK_SIZE, drv.host_len);
			drv.state = BOT_DATA_OUT;
		} else {
			data_in(drv.store + lba * BLOCK_SIZE, n * BLOCK_SIZE);
		}
		break;
	default:
		LOG_DBG("drive: SCSI 0x%02x not supported", cdb[0]);
		fail(0x05, 0x20);	/* ILLEGAL REQUEST, invalid command operation code */
		break;
	}
	if (drv.state == BOT_CBW) {
		/* no device data: the host's data stage (if any) gets nothing */
		drv.state = drv.host_len ? (drv.host_in ? BOT_DATA_IN : BOT_DATA_OUT)
					 : BOT_CSW;
	}
}

/* --- Bulk-only transport --- */

static int bulk_out(struct vxhci_data *d)
{
	if (drv.state == BOT_CBW) {
		uint8_t cbw[31];

		if (d->len != sizeof(cbw) || vxhci_data_read(d, 0, cbw, sizeof(cbw)) ||
		    sys_get_le32(cbw) != CBW_SIG || cbw[13] != 0 || cbw[14] < 1 || cbw[14] > 16) {
			LOG_WRN("drive: invalid CBW (%u bytes), halting", d->len);
			drv.state = BOT_HALTED;
			return -EPIPE;
		}
		drv.tag = sys_get_le32(&cbw[4]);
		drv.host_len = sys_get_le32(&cbw[8]);
		drv.host_in = cbw[12] & 0x80;
		drv.done = 0;
		scsi(&cbw[15]);
		d->actual = d->len;
		return 0;
	}
	if (drv.state != BOT_DATA_OUT) {
		return drv.state == BOT_HALTED ? -EPIPE : -EAGAIN;
	}

	uint32_t n = MIN(d->len, drv.host_len - drv.done);
	uint32_t keep = drv.done < drv.dev_len ? MIN(n, drv.dev_len - drv.done) : 0;

	if (keep && drv.dst && !drv.null && vxhci_data_read(d, 0, drv.dst + drv.done, keep)) {
		return -EIO;
	}
	drv.done += n;
	d->actual = n;
	if (drv.done >= drv.host_len) {
		drv.state = BOT_CSW;
	}
	return 0;
}

static int bulk_in(struct vxhci_data *d)
{
	if (drv.state == BOT_DATA_IN) {
		uint32_t n = drv.done < drv.dev_len ? MIN(d->len, drv.dev_len - drv.done) : 0;

		if (n && !(drv.null && drv.src != drv.resp) &&
		    vxhci_data_write(d, 0, drv.src + drv.done, n)) {
			return -EIO;
		}
		drv.done += n;
		d->actual = n;
		if (n < d->len || drv.done >= drv.host_len) {
			drv.state = BOT_CSW;	/* short packet ends the data stage */
		}
		return 0;
	}
	if (drv.state == BOT_CSW) {
		uint8_t csw[13];

		sys_put_le32(CSW_SIG, csw);
		sys_put_le32(drv.tag, &csw[4]);
		sys_put_le32(drv.host_len - MIN(drv.done, drv.host_len), &csw[8]);
		csw[12] = drv.status;
		d->actual = MIN(d->len, sizeof(csw));
		if (vxhci_data_write(d, 0, csw, d->actual)) {
			return -EIO;
		}
		drv.state = BOT_CBW;
		return 0;
	}
	return drv.state == BOT_HALTED ? -EPIPE : -EAGAIN;
}

static int drive_transfer(void *ctx, uint8_t ep, bool in, struct vxhci_data *d)
{
	if (!drv.config) {
		return -EPIPE;
	}
	if (in && ep == EP_IN) {
		return bulk_in(d);
	}
	if (!in && ep == EP_OUT) {
		return bulk_out(d);
	}
	return -EPIPE;
}

static void drive_reset(void *ctx)
{
	drv.config = 0;
	bot_reset();
	sense(0, 0, 0);
}

const struct vxhci_device_ops vxhci_drive_ops = {
	.control = drive_control,
	.transfer = drive_transfer,
	.reset = drive_reset,
};

void *vxhci_drive_init(void *storage, size_t size)
{
	drv.store = storage;
	drv.blocks = size / BLOCK_SIZE;
	drive_reset(NULL);
	return &drv;
}
