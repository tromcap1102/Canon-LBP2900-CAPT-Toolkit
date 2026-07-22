/*
 * Copyright (C) 2013 Alexey Galakhov <agalakhov@gmail.com>
 *
 * Licensed under the GNU General Public License Version 3
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#define _XOPEN_SOURCE   600
#define _POSIX_C_SOURCE 200112L

#include "capt-command.h"

#include "std.h"
#include "word.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>	/* memmove() -- reply-stream resync in capt_sendrecv */

#include <unistd.h>

#include <cups/cups.h>
#include <cups/sidechannel.h>

static uint8_t capt_iobuf[0x10000];
static size_t  capt_iosize;

/* Max bytes capt_sendrecv() will discard while trying to realign a reply
 * stream that a late reply from the previous job has shifted (see the resync
 * block in capt_sendrecv). A stale status/heartbeat reply is only a few dozen
 * bytes; anything beyond this is a genuinely broken stream, not a shift. */
#define CAPT_RESYNC_MAX_BYTES 256

static void capt_debug_buf(const char *level, size_t size)
{
	size_t i;
	if (size > capt_iosize)
		size = capt_iosize;
	for (i = 0; i < size; ++i) {
		if (i != 0 && (i % 16) == 0)
			fprintf(stderr, "\n%s: CAPT:", level);
		fprintf(stderr, " %02X", capt_iobuf[i]);
	}
	if (size < capt_iosize)
		fprintf(stderr, "... (%u more)", (unsigned) (capt_iosize - size));
	fprintf(stderr, "\n");
}

static void capt_send_buf(void)
{
	const uint8_t *iopos = capt_iobuf;
	size_t iosize = capt_iosize;

	if (debug) {
		fprintf(stderr, "DEBUG: CAPT: send ");
		capt_debug_buf("DEBUG", 128);
	}

	while (iosize) {
		cups_sc_status_t status;
		uint8_t tmpbuf[128];
		size_t tmpsize = sizeof(tmpbuf);
		size_t sendsize = iosize;
		if (sendsize > 4096)
			sendsize = 4096;

		fwrite(iopos, 1, sendsize, stdout);
		iopos += sendsize;
		iosize -= sendsize;
		fflush(stdout);

		status = cupsSideChannelDoRequest(CUPS_SC_CMD_DRAIN_OUTPUT,
				(char *) tmpbuf, (int *) &tmpsize, 1.0);
		if (status != CUPS_SC_STATUS_OK) {
			if (status == CUPS_SC_STATUS_TIMEOUT) {
				/* Overcome race conditions in usb backend */
				fprintf(stderr, "DEBUG: CAPT: output already empty, not drained\n");
			} else {
				fprintf(stderr, "ERROR: CAPT: no reply from backend, err=%i\n",
					(int) status);
				exit(1);
			}
		}
	}
}

/*
 * Read exactly `expected` bytes into capt_iobuf at `offset`.
 *
 * cupsBackChannelRead() returns however many bytes happen to be available at
 * that moment -- a SHORT READ is a normal, legal outcome, not an error. The
 * original upstream code issued a single read and assumed it returned
 * everything, then capt_sendrecv() saw capt_iosize != 6, declared "bad reply
 * from printer" and exit(1)'d -- while the rest of the reply was still sitting
 * in the back-channel pipe, desynchronising every later read.
 *
 * This is defensive hardening, not the fix for the "bad reply from printer"
 * failures seen on this printer: instrumenting the loop on real hardware
 * (2026-07-22) showed the short-read path never actually triggered, and those
 * failures turned out to be a reply-stream misalignment instead -- see the
 * resync block in capt_sendrecv(). The loop is kept anyway because a short
 * read is a legal return from cupsBackChannelRead() that the original code
 * would have mishandled.
 *
 * Only a genuine stall (no data at all for the whole timeout) is a failure.
 */
static void capt_recv_buf(size_t offset, size_t expected)
{
	size_t got = 0;
	int idle_rounds = 0;

	if (offset + expected > sizeof(capt_iobuf)) {
		fprintf(stderr, "ALERT: bug in CAPT driver, input buffer overflow\n");
		exit(1);
	}
	fprintf(stderr, "DEBUG: CAPT: waiting for %u bytes\n", (unsigned) expected);

	while (got < expected) {
		ssize_t size = cupsBackChannelRead((char *) capt_iobuf + offset + got,
						   expected - got, 15.0);
		if (size < 0) {
			fprintf(stderr, "ERROR: CAPT: no reply from printer\n");
			exit(1);
		}
		if (size == 0) {
			/* Timed out with nothing new. Allow a couple of retries so a
			 * momentarily slow printer does not kill the job, but do not
			 * spin forever. */
			if (++idle_rounds >= 3) {
				fprintf(stderr, "ERROR: CAPT: incomplete reply from printer "
						"(got %u of %u bytes)\n",
					(unsigned) got, (unsigned) expected);
				exit(1);
			}
			continue;
		}
		idle_rounds = 0;
		got += (size_t) size;
		if (got < expected)
			fprintf(stderr, "DEBUG: CAPT: short read, have %u of %u bytes, "
					"waiting for the rest\n",
				(unsigned) got, (unsigned) expected);
	}

	capt_iosize = offset + got;
}

const char *capt_identify(void)
{
	while (1) {
		cups_sc_status_t status;
		capt_iosize = sizeof(capt_iobuf) - 1;
		status = cupsSideChannelDoRequest(CUPS_SC_CMD_GET_DEVICE_ID,
				(char *) capt_iobuf, (int *) &capt_iosize, 60.0);
		if (status != CUPS_SC_STATUS_OK) {
			fprintf(stderr, "ERROR: CAPT: unable to communicate with printer\n");
			exit(1);
		}
		capt_iobuf[capt_iosize] = '\0';
		fprintf(stderr, "DEBUG: CAPT: printer ID string %s\n", capt_iobuf);
		if (capt_iosize)
			return (const char*) capt_iobuf;
		usleep(50000);
	}
}

static void capt_copy_cmd(uint16_t cmd, const void *buf, size_t size)
{
	if (capt_iosize + 4 + size > sizeof(capt_iobuf)) {
		fprintf(stderr, "ALERT: bug in CAPT driver, output buffer overflow\n");
		exit(1);
	}
	if (buf)
		memcpy(capt_iobuf + capt_iosize + 4, buf, size);
	else
		size = 0;
	capt_iobuf[capt_iosize + 0] = LO(cmd);
	capt_iobuf[capt_iosize + 1] = HI(cmd);
	capt_iobuf[capt_iosize + 2] = LO(size + 4);
	capt_iobuf[capt_iosize + 3] = HI(size + 4);
	capt_iosize += size + 4;
}

void capt_send(uint16_t cmd, const void *buf, size_t size)
{
	capt_iosize = 0;
	capt_copy_cmd(cmd, buf, size);
	capt_send_buf();
}

void capt_sendrecv(uint16_t cmd, const void *buf, size_t size, void *reply, size_t *reply_size)
{
	capt_send(cmd, buf, size);
	capt_recv_buf(0, 6);
	if (capt_iosize != 6 || WORD(capt_iobuf[0], capt_iobuf[1]) != cmd) {
		/*
		 * The reply stream is misaligned: what we just read is not the
		 * header of OUR reply.
		 *
		 * Cause (diagnosed on real hardware 2026-07-22): the previous job's
		 * filter can exit while the printer is still pushing out a reply to
		 * one of its trailing SetJobInfo2 heartbeats / status polls. Because
		 * captd keeps ONE USB session open across all jobs (that persistence
		 * is exactly what stops the job-5 engine wedge), those late bytes are
		 * still queued when the next job starts, so the new job's first read
		 * returns the tail of the old job's reply and every read after it is
		 * shifted. Typical signature:
		 *     expected A1 A0 xx xx xx xx, got D0 00 00 02 B0 09
		 *
		 * captd drains the endpoint on both connect and disconnect, but a
		 * drain is time-based and therefore inherently racy -- a reply that
		 * lands just after the drain still slips through (measured: ~1 job in
		 * 40 even with a 400ms settle delay).
		 *
		 * So recover here instead of dying: slide the window forward one byte
		 * at a time until the expected command word appears. The stale reply
		 * is consumed in the process and the stream is back in sync. Bounded,
		 * so a genuinely broken stream still fails loudly rather than
		 * spinning forever.
		 *
		 * Upstream simply exit(1)'d here, which killed the whole print job.
		 */
		unsigned shifts = 0;
		int resynced = 0;

		fprintf(stderr, "WARNING: CAPT: reply stream out of sync, "
				"expected %02X %02X xx xx xx xx, got", LO(cmd), HI(cmd));
		capt_debug_buf("WARNING", 6);

		while (capt_iosize == 6 && shifts < CAPT_RESYNC_MAX_BYTES) {
			memmove(capt_iobuf, capt_iobuf + 1, 5);
			capt_recv_buf(5, 1);
			shifts++;
			if (capt_iosize == 6 && WORD(capt_iobuf[0], capt_iobuf[1]) == cmd) {
				resynced = 1;
				break;
			}
		}

		if (!resynced) {
			fprintf(stderr, "ERROR: CAPT: could not resynchronise reply "
					"stream after %u byte(s), giving up\n", shifts);
			exit(1);
		}
		fprintf(stderr, "WARNING: CAPT: reply stream resynchronised after "
				"discarding %u stale byte(s) -- job continues\n", shifts);
	}
	while (1) {
		if (WORD(capt_iobuf[2], capt_iobuf[3]) == capt_iosize)
			break;
		if (BCD(capt_iobuf[2], capt_iobuf[3]) == capt_iosize)
			break;
		/* block at 64 byte boundary is not the last one */
		if (WORD(capt_iobuf[2], capt_iobuf[3]) > capt_iosize && capt_iosize % 64 == 6) {
			capt_recv_buf(capt_iosize, WORD(capt_iobuf[2], capt_iobuf[3]) - capt_iosize);
			continue;
		}
		/* we should never get here */
		fprintf(stderr, "ERROR: CAPT: bad reply from printer, "
				"expected size %02X %02X, got %02X %02X\n",
				capt_iobuf[2], capt_iobuf[3], LO(capt_iosize), HI(capt_iosize));
		capt_debug_buf("ERROR", capt_iosize);
		exit(1);
	}
	if (debug) {
		fprintf(stderr, "DEBUG: CAPT: recv ");
		capt_debug_buf("DEBUG", capt_iosize);
	}
	if (reply) {
		size_t copysize = reply_size ? *reply_size : capt_iosize;
		if (copysize > capt_iosize)
			copysize = capt_iosize;
		memcpy(reply, capt_iobuf + 4, copysize);
	}
	if (reply_size)
		*reply_size = capt_iosize;
}

void capt_multi_begin(uint16_t cmd)
{
	capt_iobuf[0] = LO(cmd);
	capt_iobuf[1] = HI(cmd);
	capt_iosize = 4;
}

void capt_multi_add(uint16_t cmd, const void *buf, size_t size)
{
	capt_copy_cmd(cmd, buf, size);
}

void capt_multi_send(void)
{
	capt_iobuf[2] = LO(capt_iosize);
	capt_iobuf[3] = HI(capt_iosize);
	capt_send_buf();
}

