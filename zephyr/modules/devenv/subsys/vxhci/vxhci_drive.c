/*
 * Virtual USB 3.0 drive behind the Wimmo virtual xHCI: a SuperSpeed mass
 * storage device (1209:0003, SCSI transparent command set) backed by M7
 * memory, with two interface settings:
 *
 *  alt 0  bulk-only transport (BOT): CBW (31 bytes OUT) -> data -> CSW
 *         (13 bytes IN), one command at a time; Linux usb-storage
 *  alt 1  USB Attached SCSI (UAS): command, status, data-in and data-out
 *         pipes; the status and data pipes use streams, stream ID = command
 *         tag, so up to 32 commands are in flight; Linux uas
 *
 * Every TRB is served at once. Data moves until the device has no more or
 * the host's TD ends (vxhci_data.td_end); a short IN TRB ends the TD. UAS
 * status and data TRBs for a tag whose command has not arrived yet get
 * -EAGAIN: the controller parks that stream until the drive made progress.
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
#define EP_DATA_IN	1	/* 0x81: BOT IN, UAS data-in */
#define EP_DATA_OUT	2	/* 0x02: BOT OUT, UAS data-out */
#define EP_STATUS	3	/* 0x83: UAS status */
#define EP_CMD		4	/* 0x04: UAS command */

#define UAS_STREAMS_EXP	5	/* 2^5 = 32 streams per endpoint */
#define MAX_TAGS	64	/* tags (= stream IDs) 1..63 */

#define CBW_SIG		0x43425355	/* "USBC" */
#define CSW_SIG		0x53425355	/* "USBS" */

#define IU_COMMAND	0x01
#define IU_SENSE	0x03
#define IU_RESPONSE	0x04
#define IU_TASK_MGMT	0x05

enum cmd_state { C_FREE, C_DATA_IN, C_DATA_OUT, C_STATUS, C_RESPONSE };
enum bot_state { BOT_CBW, BOT_BUSY, BOT_HALTED };

static const uint8_t dev_desc[18] = {
	18, 1,			/* bLength, DEVICE */
	0x00, 0x03,		/* bcdUSB 3.0 */
	0, 0, 0,		/* class per interface */
	9,			/* bMaxPacketSize0: 2^9 = 512 */
	0x09, 0x12,		/* idVendor 0x1209 (pid.codes) */
	0x03, 0x00,		/* idProduct 0x0003 */
	0x00, 0x02,		/* bcdDevice 2.00 */
	1, 2, 3,		/* strings */
	1,			/* configurations */
};

#define BULK_EP(addr, streams) \
	7, 5, (addr), 2, 0x00, 0x04, 0,		/* bulk, 1024 bytes */ \
	6, 48, 15, (streams), 0, 0		/* SS companion: burst 16, streams */
#define PIPE_USAGE(id)	4, 0x24, (id), 0

static const uint8_t cfg_desc[121] = {
	9, 2, 121, 0, 1, 1, 0, 0x80, 50,	/* bus powered, 400 mA (8 mA units) */
	/* alt 0: bulk-only */
	9, 4, 0, 0, 2, 0x08, 0x06, 0x50, 0,
	BULK_EP(0x80 | EP_DATA_IN, 0),
	BULK_EP(EP_DATA_OUT, 0),
	/* alt 1: UAS */
	9, 4, 0, 1, 4, 0x08, 0x06, 0x62, 0,
	BULK_EP(EP_CMD, 0), PIPE_USAGE(1),
	BULK_EP(0x80 | EP_STATUS, UAS_STREAMS_EXP), PIPE_USAGE(2),
	BULK_EP(0x80 | EP_DATA_IN, UAS_STREAMS_EXP), PIPE_USAGE(3),
	BULK_EP(EP_DATA_OUT, UAS_STREAMS_EXP), PIPE_USAGE(4),
};

static const uint8_t bos_desc[22] = {
	5, 15, 22, 0, 2,			/* BOS, 2 capabilities */
	7, 16, 2, 0, 0, 0, 0,			/* USB 2.0 extension: no LPM */
	10, 16, 3, 0, 0x0e, 0x00, 1, 0, 0, 0,	/* SuperSpeed: FS/HS/SS, no U1/U2 */
};

static const char *const strings[] = {NULL, "Wimmo", "Virtual USB 3.0 drive", "WIMMO0000003"};

/* One SCSI command: BOT has one, UAS one per tag */
struct cmd {
	uint8_t state;
	bool uas;
	uint16_t tag;		/* UAS tag = stream ID */
	uint32_t host_len;	/* BOT: dCBWDataTransferLength */
	bool host_in;		/* BOT: bmCBWFlags data IN */
	uint32_t done;		/* bytes moved in the data phase */
	uint32_t dev_len;	/* bytes the device moves */
	bool dev_in;
	uint8_t status;		/* SCSI status: 0 good, 2 check condition */
	uint8_t sense_key, asc;
	uint8_t rc;		/* UAS response code (task management) */
	const uint8_t *src;	/* IN data: store or resp[] */
	uint8_t *dst;		/* OUT data: store, or NULL to discard */
	uint8_t resp[64];
};

static struct drive {
	uint8_t *store;
	uint32_t blocks;
	bool null;
	uint8_t config;
	uint8_t alt;

	enum bot_state bot;
	uint32_t tag;		/* BOT dCBWTag */
	struct cmd bc;		/* BOT command */
	struct cmd uc[MAX_TAGS];
	uint8_t sense_key, asc;	/* for REQUEST SENSE */

	uint32_t commands;
	uint32_t inflight;
	uint32_t max_inflight;
} drv;

void vxhci_drive_set_null(bool null)
{
	drv.null = null;
}

bool vxhci_drive_null(void)
{
	return drv.null;
}

void vxhci_drive_get_stats(struct vxhci_drive_stats *st)
{
	st->uas = drv.alt == 1;
	st->commands = drv.commands;
	st->inflight = drv.inflight;
	st->max_inflight = drv.max_inflight;
	drv.max_inflight = drv.inflight;
}

size_t vxhci_drive_dump(char *buf, size_t len)
{
	static const char *const st[] = {"free", "data-in", "data-out", "status", "response"};
	size_t n = 0;

	n += snprintk(buf, len, "drive: alt %u, bot %u, %u in flight\n", drv.alt, drv.bot,
		      drv.inflight);
	for (int i = 1; i < MAX_TAGS; i++) {
		struct cmd *c = &drv.uc[i];

		if (c->state != C_FREE) {
			n += snprintk(buf + n, n < len ? len - n : 0,
				      " tag %d: %s, done %u of %u\n", i, st[c->state], c->done,
				      c->dev_len);
		}
	}
	return n;
}

/* --- Control requests --- */

static int reply(const void *src, size_t n, uint8_t *buf, uint16_t *len)
{
	*len = MIN(*len, n);
	memcpy(buf, src, *len);
	return 0;
}

static int string_desc(uint8_t idx, uint8_t *buf, uint16_t *len)
{
	static const uint8_t lang[] = {4, 3, 0x09, 0x04};
	uint8_t d[2 + 2 * 32];
	size_t n;

	if (idx == 0) {
		return reply(lang, sizeof(lang), buf, len);
	}
	if (idx >= ARRAY_SIZE(strings)) {
		return -EPIPE;
	}
	n = MIN(strlen(strings[idx]), 32);
	d[0] = 2 + 2 * n;
	d[1] = 3;
	for (size_t i = 0; i < n; i++) {
		d[2 + 2 * i] = strings[idx][i];
		d[3 + 2 * i] = 0;
	}
	return reply(d, d[0], buf, len);
}

static void cmds_reset(void)
{
	drv.bot = BOT_CBW;
	memset(&drv.bc, 0, sizeof(drv.bc));
	memset(drv.uc, 0, sizeof(drv.uc));
	drv.inflight = 0;
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
		drv.alt = 0;
		cmds_reset();
		if (drv.config) {
			LOG_INF("drive: configured (%u MiB)", drv.blocks / 2048);
		}
		return 0;
	case 0x8008:	/* GET_CONFIGURATION */
		return reply(&drv.config, 1, buf, len);
	case 0x010b:	/* SET_INTERFACE */
		if (value > 1) {
			return -EPIPE;
		}
		if (value != drv.alt) {
			LOG_INF("drive: %s", value ? "UAS (USB Attached SCSI)" : "bulk-only transport");
		}
		drv.alt = value;
		cmds_reset();
		return 0;
	case 0x810a:	/* GET_INTERFACE */
		return reply(&drv.alt, 1, buf, len);
	case 0x8000:	/* GET_STATUS device */
	case 0x8100:	/* interface */
	case 0x8200: {	/* endpoint */
		uint8_t st[2] = {0, 0};

		if (type == 0x82 && drv.bot == BOT_HALTED &&
		    ((index & 0x7f) == EP_DATA_IN || (index & 0x7f) == EP_DATA_OUT)) {
			st[0] = 1;
		}
		return reply(st, 2, buf, len);
	}
	case 0x0201:	/* CLEAR_FEATURE(ENDPOINT_HALT) */
		if (value == 0 && drv.bot == BOT_HALTED) {
			drv.bot = BOT_CBW;
		}
		return 0;
	case 0x0001:	/* CLEAR_FEATURE device (U1/U2 enable) */
	case 0x0003:	/* SET_FEATURE device */
	case 0x0101:	/* CLEAR_FEATURE interface */
	case 0x0103:	/* SET_FEATURE interface (function suspend) */
	case 0x0203:	/* SET_FEATURE endpoint */
	case 0x0030:	/* SET_SEL */
	case 0x0031:	/* SET_ISOCH_DELAY */
		return 0;
	case 0x21ff:	/* Bulk-Only Mass Storage Reset */
		cmds_reset();
		return 0;
	case 0xa1fe:	/* Get Max LUN */
		return reply(zero2, 1, buf, len);
	default:
		LOG_DBG("drive: control %02x %02x %04x not supported", type, req, value);
		return -EPIPE;
	}
}

/* --- SCSI --- */

static void data_in(struct cmd *c, const void *src, uint32_t n)
{
	c->src = src;
	c->dev_len = n;
	c->dev_in = true;
}

static void fail(struct cmd *c, uint8_t key, uint8_t asc)
{
	c->status = 2;		/* CHECK CONDITION */
	c->sense_key = key;
	c->asc = asc;
	c->dev_len = 0;
	drv.sense_key = key;
	drv.asc = asc;
}

static bool lba_ok(struct cmd *c, uint64_t lba, uint32_t n)
{
	if (lba + n > drv.blocks) {
		fail(c, 0x05, 0x21);	/* ILLEGAL REQUEST, LBA out of range */
		return false;
	}
	return true;
}

static void scsi(struct cmd *c, const uint8_t *cdb, bool lun0)
{
	uint8_t *r = c->resp;
	uint64_t lba;
	uint32_t n;
	bool out = false;

	c->status = 0;
	c->done = 0;
	c->dev_len = 0;
	c->dev_in = false;
	c->src = NULL;
	c->dst = NULL;
	memset(r, 0, sizeof(c->resp));

	if (!lun0 && cdb[0] != 0x12 && cdb[0] != 0x03 && cdb[0] != 0xa0) {
		fail(c, 0x05, 0x25);	/* LOGICAL UNIT NOT SUPPORTED */
		goto phase;
	}
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
		drv.sense_key = drv.asc = 0;
		data_in(c, r, MIN(18, cdb[4]));
		break;
	case 0x12:	/* INQUIRY */
		if (!lun0) {
			r[0] = 0x7f;	/* no logical unit here */
			data_in(c, r, MIN(36, sys_get_be16(&cdb[3])));
			break;
		}
		if (cdb[1] & 1) {	/* VPD */
			if (cdb[2] == 0x00) {
				static const uint8_t pages[] = {0, 0, 0, 2, 0x00, 0x80};

				memcpy(r, pages, sizeof(pages));
				data_in(c, r, MIN(sizeof(pages), sys_get_be16(&cdb[3])));
			} else if (cdb[2] == 0x80) {
				r[1] = 0x80;
				r[3] = 12;
				memcpy(&r[4], strings[3], 12);
				data_in(c, r, MIN(16, sys_get_be16(&cdb[3])));
			} else {
				fail(c, 0x05, 0x24);	/* invalid field in CDB */
			}
			break;
		}
		r[0] = 0x00;		/* direct access block device */
		r[2] = 0x06;		/* SPC-4 */
		r[3] = 0x02;		/* response data format */
		r[4] = 31;
		r[7] = 0x02;		/* CmdQue: tagged commands */
		memcpy(&r[8], "Wimmo   ", 8);
		memcpy(&r[16], "USB 3.0 Drive   ", 16);
		memcpy(&r[32], "0200", 4);
		data_in(c, r, MIN(36, sys_get_be16(&cdb[3])));
		break;
	case 0xa0:	/* REPORT LUNS: LUN 0 only */
		sys_put_be32(8, &r[0]);
		data_in(c, r, MIN(16, sys_get_be32(&cdb[6])));
		break;
	case 0x25:	/* READ CAPACITY (10) */
		sys_put_be32(drv.blocks - 1, &r[0]);
		sys_put_be32(BLOCK_SIZE, &r[4]);
		data_in(c, r, 8);
		break;
	case 0x9e:	/* SERVICE ACTION IN: READ CAPACITY (16) */
		if ((cdb[1] & 0x1f) != 0x10) {
			fail(c, 0x05, 0x20);
			break;
		}
		sys_put_be64(drv.blocks - 1, &r[0]);
		sys_put_be32(BLOCK_SIZE, &r[8]);
		data_in(c, r, MIN(32, sys_get_be32(&cdb[10])));
		break;
	case 0x1a:	/* MODE SENSE (6): header only, not write protected */
		r[0] = 3;
		data_in(c, r, MIN(4, cdb[4]));
		break;
	case 0x5a:	/* MODE SENSE (10) */
		r[1] = 6;
		data_in(c, r, MIN(8, sys_get_be16(&cdb[7])));
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
		if (!lba_ok(c, lba, n)) {
			break;
		}
		if (out) {
			c->dst = drv.store + lba * BLOCK_SIZE;
			c->dev_len = n * BLOCK_SIZE;
		} else {
			data_in(c, drv.store + lba * BLOCK_SIZE, n * BLOCK_SIZE);
		}
		break;
	default:
		LOG_DBG("drive: SCSI 0x%02x not supported", cdb[0]);
		fail(c, 0x05, 0x20);	/* ILLEGAL REQUEST, invalid command operation code */
		break;
	}
phase:
	if (!c->uas) {
		/* BOT: the host's length and direction decide the data phase */
		c->dev_len = MIN(c->dev_len, c->host_len);
		if (c->dev_len && c->dev_in != c->host_in) {
			fail(c, 0x05, 0x24);
		}
		c->state = !c->host_len ? C_STATUS : c->host_in ? C_DATA_IN : C_DATA_OUT;
	} else {
		/* UAS: the device's; data TRBs of a command without data come back empty */
		c->state = !c->dev_len ? C_STATUS : c->dev_in ? C_DATA_IN : C_DATA_OUT;
	}
}

/* UAS: tell the controller which stream of this command can move now */
static void uas_ready(struct cmd *c)
{
	if (!c->uas) {
		return;
	}
	switch (c->state) {
	case C_DATA_IN:
		vxhci_stream_ready(&drv, EP_DATA_IN, true, c->tag);
		break;
	case C_DATA_OUT:
		vxhci_stream_ready(&drv, EP_DATA_OUT, false, c->tag);
		break;
	case C_STATUS:
	case C_RESPONSE:
		vxhci_stream_ready(&drv, EP_STATUS, true, c->tag);
		break;
	default:
		break;
	}
}

/* Data phase, one TRB. Ends when the device runs out (short IN) or the host's TD ends. */
static int data_trb(struct cmd *c, bool in, struct vxhci_data *d)
{
	uint32_t n, keep;

	if (in) {
		n = c->dev_in && c->done < c->dev_len ? MIN(d->len, c->dev_len - c->done) : 0;
		if (n && !(drv.null && c->src != c->resp) &&
		    vxhci_data_write(d, 0, c->src + c->done, n)) {
			return -EIO;
		}
		d->actual = n;
		c->done += n;
		if (n < d->len || d->td_end) {
			c->state = C_STATUS;
			uas_ready(c);
		}
		return 0;
	}
	n = d->len;
	keep = !c->dev_in && c->done < c->dev_len ? MIN(n, c->dev_len - c->done) : 0;
	if (keep && c->dst && !drv.null && vxhci_data_read(d, 0, c->dst + c->done, keep)) {
		return -EIO;
	}
	d->actual = n;
	c->done += n;
	if (d->td_end) {
		c->state = C_STATUS;
		uas_ready(c);
	}
	return 0;
}

/* --- Bulk-only transport (alt 0) --- */

static int bot_transfer(bool in, struct vxhci_data *d)
{
	struct cmd *c = &drv.bc;

	if (drv.bot == BOT_HALTED) {
		return -EPIPE;
	}
	if (drv.bot == BOT_CBW) {
		uint8_t cbw[31];

		if (in) {
			return -EAGAIN;
		}
		if (d->len != sizeof(cbw) || vxhci_data_read(d, 0, cbw, sizeof(cbw)) ||
		    sys_get_le32(cbw) != CBW_SIG || cbw[13] != 0 || cbw[14] < 1 || cbw[14] > 16) {
			LOG_WRN("drive: invalid CBW (%u bytes), halting", d->len);
			drv.bot = BOT_HALTED;
			return -EPIPE;
		}
		drv.tag = sys_get_le32(&cbw[4]);
		c->uas = false;
		c->host_len = sys_get_le32(&cbw[8]);
		c->host_in = cbw[12] & 0x80;
		scsi(c, &cbw[15], true);
		drv.commands++;
		drv.bot = BOT_BUSY;
		d->actual = d->len;
		return 0;
	}
	if ((c->state == C_DATA_IN && in) || (c->state == C_DATA_OUT && !in)) {
		return data_trb(c, in, d);
	}
	if (c->state == C_STATUS && in) {
		uint8_t csw[13];

		sys_put_le32(CSW_SIG, csw);
		sys_put_le32(drv.tag, &csw[4]);
		sys_put_le32(c->host_len - MIN(c->done, c->host_len), &csw[8]);
		csw[12] = c->status ? 1 : 0;
		d->actual = MIN(d->len, sizeof(csw));
		if (vxhci_data_write(d, 0, csw, d->actual)) {
			return -EIO;
		}
		c->state = C_FREE;
		drv.bot = BOT_CBW;
		return 0;
	}
	return -EAGAIN;
}

/* --- USB Attached SCSI (alt 1) --- */

static int uas_command(struct vxhci_data *d)
{
	uint8_t iu[48];
	uint32_t n = MIN(d->len, sizeof(iu));
	uint16_t tag;
	struct cmd *c;

	d->actual = d->len;
	if (n < 16 || vxhci_data_read(d, 0, iu, n)) {
		return 0;
	}
	tag = sys_get_be16(&iu[2]);
	if (tag == 0 || tag >= MAX_TAGS) {
		LOG_WRN("drive: UAS tag %u out of range", tag);
		return 0;
	}
	c = &drv.uc[tag];
	if (iu[0] == IU_TASK_MGMT) {
		uint16_t task = sys_get_be16(&iu[6]);

		LOG_INF("drive: UAS task management 0x%02x (tag %u)", iu[4], task);
		for (int i = 1; i < MAX_TAGS; i++) {
			if (i != tag && drv.uc[i].state != C_FREE &&
			    (iu[4] != 0x01 || i == task)) {	/* abort task: just that one */
				drv.uc[i].state = C_FREE;
				drv.inflight--;
			}
		}
		if (c->state == C_FREE) {
			drv.inflight++;
		}
		c->state = C_RESPONSE;
		c->rc = 0x00;		/* TMF complete */
		c->uas = true;
		c->tag = tag;
		uas_ready(c);
		return 0;
	}
	if (iu[0] != IU_COMMAND || n < 32) {
		return 0;
	}
	if (c->state != C_FREE) {
		LOG_WRN("drive: UAS overlapped tag %u", tag);
	} else {
		drv.inflight++;
		drv.max_inflight = MAX(drv.max_inflight, drv.inflight);
	}
	c->uas = true;
	c->tag = tag;
	scsi(c, &iu[16], iu[8] == 0 && iu[9] == 0);
	drv.commands++;
	uas_ready(c);
	return 0;
}

static int uas_status(uint16_t tag, struct vxhci_data *d)
{
	struct cmd *c = tag < MAX_TAGS ? &drv.uc[tag] : NULL;
	uint8_t iu[16 + 18] = {0};
	uint32_t n = 16;

	if (!c || (c->state != C_STATUS && c->state != C_RESPONSE)) {
		return -EAGAIN;		/* the command has not come (or not finished) yet */
	}
	sys_put_be16(tag, &iu[2]);
	if (c->state == C_RESPONSE) {
		iu[0] = IU_RESPONSE;
		iu[7] = c->rc;
		n = 8;
	} else {
		iu[0] = IU_SENSE;
		iu[6] = c->status;
		if (c->status) {
			sys_put_be16(18, &iu[14]);
			iu[16] = 0x70;
			iu[18] = c->sense_key;
			iu[23] = 10;
			iu[28] = c->asc;
			n = sizeof(iu);
		}
	}
	d->actual = MIN(d->len, n);
	if (vxhci_data_write(d, 0, iu, d->actual)) {
		return -EIO;
	}
	c->state = C_FREE;
	drv.inflight--;
	return 0;
}

static int uas_data(uint16_t tag, bool in, struct vxhci_data *d)
{
	struct cmd *c = tag < MAX_TAGS ? &drv.uc[tag] : NULL;

	if (!c || c->state == C_FREE || c->state == C_RESPONSE) {
		return -EAGAIN;
	}
	if (c->state == C_STATUS || (c->state == C_DATA_IN) != in) {
		d->actual = 0;		/* no (more) data for this command: empty/short */
		return 0;
	}
	return data_trb(c, in, d);
}

static int drive_transfer(void *ctx, uint8_t ep, bool in, uint16_t stream, struct vxhci_data *d)
{
	if (!drv.config) {
		return -EPIPE;
	}
	if (drv.alt == 0) {
		if ((in && ep == EP_DATA_IN) || (!in && ep == EP_DATA_OUT)) {
			return bot_transfer(in, d);
		}
		return -EPIPE;
	}
	if (!in && ep == EP_CMD) {
		return uas_command(d);
	}
	if (in && ep == EP_STATUS) {
		return uas_status(stream, d);
	}
	if ((in && ep == EP_DATA_IN) || (!in && ep == EP_DATA_OUT)) {
		return uas_data(stream, in, d);
	}
	return -EPIPE;
}

static void drive_reset(void *ctx)
{
	drv.config = 0;
	drv.alt = 0;
	cmds_reset();
	drv.sense_key = drv.asc = 0;
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
