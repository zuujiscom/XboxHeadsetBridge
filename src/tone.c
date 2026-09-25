/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * gip-tone - milestone 2
 *
 * Runs the GIP handshake, negotiates 48 kHz stereo output, switches the audio
 * interface to alt setting 1, and streams a sine wave over the isochronous
 * OUT endpoint. If this works you hear a tone in the headset.
 */

#include "gip.h"
#include "gipusb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <math.h>
#include <time.h>

#define VID_PDP     0x0e6f
#define PID_LVL50   0x0234

/* negotiated output format: 48 kHz stereo S16, per the device's identify */
#define RATE        48000
#define CHANNELS    2
#define AUDIO_PKTS  8               /* USB frames per isochronous transfer */
#define NUM_XFERS   4               /* transfers in flight */
#define TONE_HZ     440.0

/* buffer_size = rate * channels * sizeof(s16) * interval / 1000 */
#define BUFFER_SIZE   (RATE * CHANNELS * 2 * GIP_AUDIO_INTERVAL / 1000)  /* 1536 */
#define FRAGMENT_SIZE (BUFFER_SIZE / AUDIO_PKTS)                          /* 192  */

static volatile sig_atomic_t stop;
static void on_sigint(int sig) { (void)sig; stop = 1; }

static gipusb   u;
static uint8_t  seq = 1;
static uint8_t  audio_seq = 1;
static bool     streaming;
static int      packet_size;        /* GIP header + fragment, per USB frame */
static double   phase;
static uint64_t next_frame;
static unsigned submitted, completed, errors;

struct xfer {
	uint8_t       *buf;
	IOUSBIsocFrame list[AUDIO_PKTS];
};
static struct xfer xfers[NUM_XFERS];

static int send_pkt(uint8_t cmd, uint8_t options, uint8_t sequence,
		    const void *payload, uint32_t payload_len)
{
	uint8_t buf[64] = {0};
	struct gip_header hdr = {
		.command = cmd,
		.options = options,
		.sequence = sequence ? sequence : seq++,
		.packet_length = payload_len,
	};
	int hdr_len = gip_encode_header(&hdr, buf);

	if (!seq)
		seq = 1;
	if (payload && payload_len)
		memcpy(buf + hdr_len, payload, payload_len);

	return gipusb_write(&u, buf, hdr_len + payload_len);
}

static void set_audio_format(uint8_t in, uint8_t out)
{
	uint8_t pkt[3] = { GIP_AUD_CTRL_FORMAT, in, out };

	printf("  --> AUDIO_CONTROL format in=%s out=%s\n",
	       gip_audio_format_name(in), gip_audio_format_name(out));
	send_pkt(GIP_CMD_AUDIO_CONTROL, GIP_OPT_INTERNAL, 0, pkt, sizeof(pkt));
}

static void set_audio_volume(uint8_t in, uint8_t chat, uint8_t out)
{
	uint8_t pkt[8] = { GIP_AUD_CTRL_VOLUME, 0x04 /* unmuted */,
			   out, chat, in, 0, 0, 0 };

	printf("  --> AUDIO_CONTROL volume out=%u in=%u\n", out, in);
	send_pkt(GIP_CMD_AUDIO_CONTROL, GIP_OPT_INTERNAL, 0, pkt, sizeof(pkt));
}

static void set_power(uint8_t mode)
{
	printf("  --> POWER mode=0x%02x\n", mode);
	send_pkt(GIP_CMD_POWER, GIP_OPT_INTERNAL, 0, &mode, 1);
}

/* fill one transfer: AUDIO_PKTS x (GIP header + FRAGMENT_SIZE PCM bytes) */
static void fill_xfer(struct xfer *x)
{
	struct gip_header hdr = {
		.command = GIP_CMD_AUDIO_SAMPLES,
		.options = GIP_OPT_INTERNAL,
		.packet_length = FRAGMENT_SIZE,
	};
	double step = 2.0 * M_PI * TONE_HZ / RATE;

	for (int p = 0; p < AUDIO_PKTS; p++) {
		uint8_t *dest = x->buf + p * packet_size;
		int hdr_len;
		int16_t *pcm;

		do {
			hdr.sequence = audio_seq++;
		} while (!hdr.sequence);

		hdr_len = gip_encode_header(&hdr, dest);
		pcm = (int16_t *)(dest + hdr_len);

		for (int i = 0; i < FRAGMENT_SIZE / (int)sizeof(int16_t) / CHANNELS; i++) {
			int16_t s = (int16_t)(sin(phase) * 8000.0);

			pcm[i * CHANNELS] = s;
			pcm[i * CHANNELS + 1] = s;
			phase += step;
			if (phase > 2.0 * M_PI)
				phase -= 2.0 * M_PI;
		}
	}
}

static void submit_xfer(struct xfer *x);

static void xfer_done(void *refcon, IOReturn result, void *arg0)
{
	struct xfer *x = refcon;

	completed++;
	if (result != kIOReturnSuccess) {
		if (errors++ < 5)
			fprintf(stderr, "iso write failed: 0x%08x\n", result);
		if (result == kIOReturnNoDevice || result == kIOReturnAborted)
			return;
		/* resync the schedule after an underrun */
		next_frame = gipusb_frame_number(&u) + 16;
	}

	if (!stop && streaming)
		submit_xfer(x);
}

static void submit_xfer(struct xfer *x)
{
	IOReturn ret;

	fill_xfer(x);
	for (int i = 0; i < AUDIO_PKTS; i++) {
		x->list[i].frStatus = 0;
		x->list[i].frReqCount = (UInt16)packet_size;
		x->list[i].frActCount = 0;
	}

	ret = gipusb_iso_write(&u, x->buf, next_frame, AUDIO_PKTS, x->list,
			       xfer_done, x);
	if (ret != kIOReturnSuccess) {
		if (errors++ < 5)
			fprintf(stderr, "WriteIsochPipeAsync failed: 0x%08x\n", ret);
		return;
	}

	next_frame += AUDIO_PKTS;
	submitted++;
}

static void on_rx(void *ctx, const uint8_t *data, uint32_t len)
{
	struct gip_header hdr;
	int hdr_len = gip_decode_header(&hdr, data, len);

	if (hdr_len < 0)
		return;

	printf("  <-- %-14s opt=0x%02x seq=%u len=%u\n",
	       gip_command_name(hdr.command), hdr.options,
	       hdr.sequence, hdr.packet_length);

	/* acknowledge chunked traffic so the device does not stall */
	if (hdr.options & GIP_OPT_ACKNOWLEDGE) {
		struct gip_pkt_acknowledge ack = {0};
		uint32_t l = (hdr.options & GIP_OPT_CHUNK_START) ?
			     hdr.packet_length :
			     hdr.chunk_offset + hdr.packet_length;

		ack.command = hdr.command;
		ack.options = GIP_OPT_INTERNAL;
		ack.length = (uint16_t)l;
		send_pkt(GIP_CMD_ACKNOWLEDGE, GIP_OPT_INTERNAL, hdr.sequence,
			 &ack, sizeof(ack));
	}
}

int main(int argc, char **argv)
{
	int secs = argc > 1 ? atoi(argv[1]) : 8;
	struct timespec settle = { .tv_nsec = 300 * 1000 * 1000 };
	struct gip_header probe_hdr = { .packet_length = FRAGMENT_SIZE };
	uint8_t tmp[16];
	double t;

	signal(SIGINT, on_sigint);

	packet_size = gip_encode_header(&probe_hdr, tmp) + FRAGMENT_SIZE;
	printf("format: %d Hz x%d, interval %d ms\n", RATE, CHANNELS, GIP_AUDIO_INTERVAL);
	printf("buffer=%d fragment=%d packet=%d (%d frames/transfer)\n\n",
	       BUFFER_SIZE, FRAGMENT_SIZE, packet_size, AUDIO_PKTS);

	printf("re-enumerating %04x:%04x\n", VID_PDP, PID_LVL50);
	if (gipusb_reenumerate(VID_PDP, PID_LVL50) == 0) {
		struct timespec ts = { .tv_sec = 2 };
		nanosleep(&ts, NULL);
	}

	if (gipusb_open(&u, VID_PDP, PID_LVL50, GIP_INTF_DATA) < 0)
		goto fail;
	if (gipusb_start_reader(&u, on_rx, &u) < 0)
		goto fail;

	printf("handshake\n");
	send_pkt(GIP_CMD_IDENTIFY, GIP_OPT_INTERNAL, 0, NULL, 0);
	for (t = 0; t < 1.0; t += 0.1)
		CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.1, false);

	set_audio_format(GIP_AUD_FORMAT_24KHZ_MONO, GIP_AUD_FORMAT_48KHZ_STEREO);
	nanosleep(&settle, NULL);
	set_power(GIP_PWR_ON);
	nanosleep(&settle, NULL);
	set_audio_volume(100, 100, 100);
	nanosleep(&settle, NULL);

	printf("\nenabling audio interface\n");
	if (gipusb_open_audio(&u) < 0)
		goto fail;
	printf("iso out ep=0x%02x (%u B) in=0x%02x (%u B)\n",
	       u.iso_ep_out, u.iso_max_out, u.iso_ep_in | 0x80, u.iso_max_in);

	if (packet_size > u.iso_max_out) {
		fprintf(stderr, "packet %d exceeds endpoint max %u\n",
			packet_size, u.iso_max_out);
		goto fail;
	}

	for (int i = 0; i < NUM_XFERS; i++) {
		xfers[i].buf = calloc(1, (size_t)packet_size * AUDIO_PKTS);
		if (!xfers[i].buf)
			goto fail;
	}

	next_frame = gipusb_frame_number(&u) + 32;
	streaming = true;
	printf("\nstreaming %.0f Hz tone for %d s (ctrl-c to stop)\n", TONE_HZ, secs);

	for (int i = 0; i < NUM_XFERS; i++)
		submit_xfer(&xfers[i]);

	for (t = 0; t < secs && !stop; t += 0.2)
		CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.2, false);

	streaming = false;
	CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.3, false);

	printf("\nsubmitted=%u completed=%u errors=%u\n",
	       submitted, completed, errors);

	for (int i = 0; i < NUM_XFERS; i++)
		free(xfers[i].buf);
	gipusb_close(&u);
	return errors ? 1 : 0;

fail:
	gipusb_close(&u);
	return 1;
}
