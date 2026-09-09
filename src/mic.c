/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * gip-mic - milestone 3
 *
 * Runs the GIP handshake, negotiates 24 kHz mono input / 48 kHz stereo output,
 * switches the audio interface to alt setting 1, and streams microphone audio
 * from the isochronous IN endpoint (0x83).
 *
 * Displays live audio levels and optionally records to a WAV file.
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

/* negotiated input format: 24 kHz mono S16, per device identify */
#define MIC_RATE        24000
#define MIC_CHANNELS    1
#define AUDIO_PKTS      8               /* USB frames per isochronous transfer */
#define NUM_XFERS       4               /* transfers in flight */

/* buffer_size = rate * channels * sizeof(s16) * interval / 1000 */
#define MIC_BUFFER_SIZE   (MIC_RATE * MIC_CHANNELS * 2 * GIP_AUDIO_INTERVAL / 1000)  /* 384 */
#define MIC_FRAGMENT_SIZE (MIC_BUFFER_SIZE / AUDIO_PKTS)                             /* 48  */

#define OUT_RATE          48000
#define OUT_CHANNELS      2
#define OUT_BUFFER_SIZE   (OUT_RATE * OUT_CHANNELS * 2 * GIP_AUDIO_INTERVAL / 1000)
#define OUT_FRAGMENT_SIZE (OUT_BUFFER_SIZE / AUDIO_PKTS)

static volatile sig_atomic_t stop;
static void on_sigint(int sig) { (void)sig; stop = 1; }

static gipusb   u;
static uint8_t  seq = 1;
static bool     streaming;
static uint16_t req_pkt_size;
static uint64_t next_frame;
static int out_packet_size;
static uint64_t next_out_frame;
static uint8_t audio_seq = 1;
static volatile bool volume_chat_seen;
static unsigned submitted, completed, errors;
static unsigned out_submitted, out_completed, out_errors;
static uint64_t total_samples;
static float    peak_level;
static FILE    *wav_file;
static uint32_t wav_data_bytes;

struct xfer {
	uint8_t       *buf;
	IOUSBIsocFrame list[AUDIO_PKTS];
};
static struct xfer xfers[NUM_XFERS];

struct out_xfer {
	uint8_t *buf;
	IOUSBIsocFrame list[AUDIO_PKTS];
};
static struct out_xfer out_xfers[NUM_XFERS];

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

static void set_power(uint8_t mode)
{
	printf("  --> POWER mode=0x%02x\n", mode);
	send_pkt(GIP_CMD_POWER, GIP_OPT_INTERNAL, 0, &mode, 1);
}

static void write_wav_header(FILE *f)
{
	uint8_t header[44] = {
		'R', 'I', 'F', 'F',
		0, 0, 0, 0,             /* file size - 8 */
		'W', 'A', 'V', 'E',
		'f', 'm', 't', ' ',
		16, 0, 0, 0,            /* PCM chunk size */
		1, 0,                   /* format: PCM */
		MIC_CHANNELS, 0,        /* channels */
		(uint8_t)(MIC_RATE & 0xff), (uint8_t)((MIC_RATE >> 8) & 0xff),
		(uint8_t)((MIC_RATE >> 16) & 0xff), (uint8_t)((MIC_RATE >> 24) & 0xff),
		(uint8_t)((MIC_RATE * 2) & 0xff), (uint8_t)(((MIC_RATE * 2) >> 8) & 0xff),
		(uint8_t)(((MIC_RATE * 2) >> 16) & 0xff), (uint8_t)(((MIC_RATE * 2) >> 24) & 0xff),
		2, 0,                   /* block align (1 ch * 2 B) */
		16, 0,                  /* bits per sample */
		'd', 'a', 't', 'a',
		0, 0, 0, 0              /* data size */
	};
	fwrite(header, 1, sizeof(header), f);
}

static void finalize_wav_header(FILE *f, uint32_t data_bytes)
{
	uint32_t riff_size = data_bytes + 36;
	fseek(f, 4, SEEK_SET);
	fwrite(&riff_size, 4, 1, f);
	fseek(f, 40, SEEK_SET);
	fwrite(&data_bytes, 4, 1, f);
}

static void process_mic_samples(const int16_t *samples, uint32_t count)
{
	for (uint32_t i = 0; i < count; i++) {
		float val = fabsf((float)samples[i] / 32768.0f);
		if (val > peak_level)
			peak_level = val;
	}

	if (wav_file && count > 0) {
		fwrite(samples, sizeof(int16_t), count, wav_file);
		wav_data_bytes += count * sizeof(int16_t);
	}
	total_samples += count;
}

static void submit_xfer(struct xfer *x);

static void fill_silence(struct out_xfer *x)
{
	struct gip_header hdr = {
		.command = GIP_CMD_AUDIO_SAMPLES,
		.options = GIP_OPT_INTERNAL,
		.packet_length = OUT_FRAGMENT_SIZE,
	};

	for (int p = 0; p < AUDIO_PKTS; p++) {
		uint8_t *dest = x->buf + p * out_packet_size;
		int hdr_len;

		do {
			hdr.sequence = audio_seq++;
		} while (!hdr.sequence);
		hdr_len = gip_encode_header(&hdr, dest);
		memset(dest + hdr_len, 0, OUT_FRAGMENT_SIZE);
	}
}

static void submit_out_xfer(struct out_xfer *x);

static void out_xfer_done(void *refcon, IOReturn result, void *arg0)
{
	struct out_xfer *x = refcon;

	out_completed++;
	if (result != kIOReturnSuccess) {
		if (out_errors++ < 5)
			fprintf(stderr, "iso silence write failed: 0x%08x\n", result);
		if (result == kIOReturnNoDevice || result == kIOReturnAborted)
			return;
		next_out_frame = gipusb_frame_number(&u) + 16;
	}
	if (!stop && streaming)
		submit_out_xfer(x);
}

static void submit_out_xfer(struct out_xfer *x)
{
	IOReturn ret;

	fill_silence(x);
	for (int i = 0; i < AUDIO_PKTS; i++) {
		x->list[i].frStatus = 0;
		x->list[i].frReqCount = (UInt16)out_packet_size;
		x->list[i].frActCount = 0;
	}
	ret = gipusb_iso_write(&u, x->buf, next_out_frame, AUDIO_PKTS,
			       x->list, out_xfer_done, x);
	if (ret != kIOReturnSuccess) {
		if (out_errors++ < 5)
			fprintf(stderr, "WriteIsochPipeAsync failed: 0x%08x\n", ret);
		return;
	}
	next_out_frame += AUDIO_PKTS;
	out_submitted++;
}

static void xfer_done(void *refcon, IOReturn result, void *arg0)
{
	struct xfer *x = refcon;

	completed++;
	if (result != kIOReturnSuccess) {
		if (errors++ < 5)
			fprintf(stderr, "iso read failed: 0x%08x\n", result);
		if (result == kIOReturnNoDevice || result == kIOReturnAborted)
			return;
		next_frame = gipusb_frame_number(&u) + 16;
	} else {
		for (int p = 0; p < AUDIO_PKTS; p++) {
			uint32_t act = x->list[p].frActCount;
			uint8_t *src = x->buf + p * req_pkt_size;

			if (x->list[p].frStatus == kIOReturnSuccess && act >= GIP_HDR_MIN_LENGTH) {
				struct gip_header hdr;
				int hdr_len = gip_decode_header(&hdr, src, act);

				if (hdr_len > 0 && hdr.command == GIP_CMD_AUDIO_SAMPLES &&
				    (uint32_t)hdr_len <= act &&
				    hdr.packet_length <= act - (uint32_t)hdr_len &&
				    hdr.packet_length >= sizeof(struct gip_pkt_audio_samples)) {
					int pcm_bytes = (int)hdr.packet_length -
							(int)sizeof(struct gip_pkt_audio_samples);
					if (pcm_bytes > 0) {
						int16_t *pcm = (int16_t *)(src + hdr_len +
									 sizeof(struct gip_pkt_audio_samples));
						process_mic_samples(pcm, pcm_bytes / sizeof(int16_t));
					}
				}
			}
		}
	}

	if (!stop && streaming)
		submit_xfer(x);
}

static void submit_xfer(struct xfer *x)
{
	IOReturn ret;

	for (int i = 0; i < AUDIO_PKTS; i++) {
		x->list[i].frStatus = 0;
		x->list[i].frReqCount = req_pkt_size;
		x->list[i].frActCount = 0;
	}

	ret = gipusb_iso_read(&u, x->buf, next_frame, AUDIO_PKTS, x->list,
			      xfer_done, x);
	if (ret != kIOReturnSuccess) {
		if (errors++ < 5)
			fprintf(stderr, "ReadIsochPipeAsync failed: 0x%08x\n", ret);
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

	if (hdr.command == GIP_CMD_AUDIO_CONTROL) {
		const uint8_t *p = data + hdr_len;
		uint32_t plen = len - hdr_len;
		if (plen >= 5 && p[0] == GIP_AUD_CTRL_VOLUME_CHAT) {
			volume_chat_seen = true;
			printf("  <-- AUDIO_CONTROL: VOLUME_CHAT (audio ready)\n");
		} else if (plen >= 2 && p[0] == GIP_AUD_CTRL_VOLUME) {
			bool unmuted = (p[1] & 0x04) != 0;
			uint8_t vol_out = plen > 2 ? p[2] : 0;
			uint8_t vol_in = plen > 4 ? p[4] : 0;
			printf("  <-- AUDIO_CONTROL: mute=%s vol_out=%u vol_in=%u\n",
			       unmuted ? "UNMUTED" : "MUTED", vol_out, vol_in);
		}
	}

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

static void print_vu_meter(float peak, uint64_t samples)
{
	char bar[21];
	int bars = (int)(peak * 20.0f);
	if (bars > 20) bars = 20;

	for (int i = 0; i < 20; i++)
		bar[i] = (i < bars) ? '#' : '-';
	bar[20] = '\0';

	printf("\r  mic [%s] peak=%.2f samples=%llu err=%u   ",
	       bar, peak, (unsigned long long)samples, errors);
	fflush(stdout);
}

int main(int argc, char **argv)
{
	int secs = argc > 1 ? atoi(argv[1]) : 10;
	const char *wav_path = argc > 2 ? argv[2] : NULL;
	double t;

	signal(SIGINT, on_sigint);

	if (wav_path) {
		wav_file = fopen(wav_path, "wb");
		if (wav_file) {
			write_wav_header(wav_file);
			printf("recording to: %s\n", wav_path);
		} else {
			fprintf(stderr, "could not open %s for writing\n", wav_path);
		}
	}

	printf("gip-mic: format %d Hz mono (interval %d ms, fragment %d B)\n",
	       MIC_RATE, GIP_AUDIO_INTERVAL, MIC_FRAGMENT_SIZE);

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
	set_power(GIP_PWR_ON);
	CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.3, false);
	send_pkt(GIP_CMD_IDENTIFY, GIP_OPT_INTERNAL, 0, NULL, 0);
	for (t = 0; t < 1.0; t += 0.1)
		CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.1, false);

	printf("\nenabling audio interface\n");
	if (gipusb_open_audio(&u) < 0)
		goto fail;
	printf("iso out ep=0x%02x (%u B) in=0x%02x (%u B)\n",
	       u.iso_ep_out, u.iso_max_out, u.iso_ep_in | 0x80, u.iso_max_in);
	{
		struct gip_header out_hdr = { .packet_length = OUT_FRAGMENT_SIZE };
		uint8_t tmp[16];
		out_packet_size = gip_encode_header(&out_hdr, tmp) + OUT_FRAGMENT_SIZE;
	}
	if (out_packet_size > u.iso_max_out) {
		fprintf(stderr, "output packet %d exceeds endpoint max %u\n",
			out_packet_size, u.iso_max_out);
		goto fail;
	}

	if (!u.iso_pipe_in) {
		fprintf(stderr, "isochronous IN pipe not found\n");
		goto fail;
	}

	req_pkt_size = u.iso_max_in ? u.iso_max_in : 128;

	for (int i = 0; i < NUM_XFERS; i++) {
		xfers[i].buf = calloc(1, (size_t)req_pkt_size * AUDIO_PKTS);
		out_xfers[i].buf = calloc(1, (size_t)out_packet_size * AUDIO_PKTS);
		if (!xfers[i].buf || !out_xfers[i].buf)
			goto fail;
	}

	next_frame = gipusb_frame_number(&u) + 32;
	next_out_frame = next_frame;
	streaming = true;
	for (int i = 0; i < NUM_XFERS; i++)
		submit_xfer(&xfers[i]);

	/* Arm capture before format negotiation, as required by the GIP headset
	 * implementation. The standalone headset reports its own volume state;
	 * do not send the generic VOLUME subcommand back to it. */
	set_audio_format(GIP_AUD_FORMAT_24KHZ_MONO, GIP_AUD_FORMAT_48KHZ_STEREO);
	CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.3, false);
	set_power(GIP_PWR_ON);
	for (t = 0; t < 2.0 && !volume_chat_seen; t += 0.1)
		CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.1, false);

	/* Start silent playback only after the headset signals that its negotiated
	 * audio configuration is ready. */
	next_out_frame = gipusb_frame_number(&u) + 32;
	for (int i = 0; i < NUM_XFERS; i++)
		submit_out_xfer(&out_xfers[i]);

	printf("\ncapturing microphone for %d s (speak into the headset, ctrl-c to stop)\n\n", secs);

	for (t = 0; (secs == 0 || t < secs) && !stop; t += 0.1) {
		CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.1, false);
		print_vu_meter(peak_level, total_samples);
		peak_level *= 0.85f; /* decay */
	}

	streaming = false;
	CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.3, false);

	printf("\n\ninput submitted=%u completed=%u errors=%u; output submitted=%u "
	       "completed=%u errors=%u; total_samples=%llu\n",
	       submitted, completed, errors, out_submitted, out_completed, out_errors,
	       (unsigned long long)total_samples);

	if (wav_file) {
		finalize_wav_header(wav_file, wav_data_bytes);
		fclose(wav_file);
		printf("saved %u bytes PCM to %s\n", wav_data_bytes, wav_path);
	}

	for (int i = 0; i < NUM_XFERS; i++)
		free(xfers[i].buf);
	for (int i = 0; i < NUM_XFERS; i++)
		free(out_xfers[i].buf);
	gipusb_close(&u);
	return errors ? 1 : 0;

fail:
	if (wav_file)
		fclose(wav_file);
	gipusb_close(&u);
	return 1;
}
