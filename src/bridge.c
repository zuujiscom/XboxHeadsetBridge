/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * gip-bridge - milestone 4 daemon
 *
 * Runs the GIP handshake, negotiates 24 kHz mono input / 48 kHz stereo output,
 * and handles bidirectional audio streaming via the shared ring buffer:
 *   - Output: HAL ring -> USB ISO OUT (48 kHz stereo S16)
 *   - Input:  USB ISO IN -> HAL ring (24 kHz mono -> 48 kHz mono float)
 *
 * Auto-reconnects on USB disconnect (device pull, headset power-off, etc).
 */

#include "gip.h"
#include "gipusb.h"
#include "gip_auth.h"
#include "bridge.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <math.h>
#include <time.h>
#include <unistd.h>

#include "../shared/ring.h"

#define VID_PDP     0x0e6f
#define PID_LVL50   0x0234

/* The Windows trace for this standalone PDP LVL50 performs the GIP v2
 * authentication exchange immediately after device Start.  xone starts the
 * exchange for standalone headsets too; its per-device list only determines
 * whether playback must wait for authentication to finish. */
#define HEADSET_NEEDS_AUTH 1

/* negotiated formats */
#define OUT_RATE        48000
#define OUT_CHANNELS    2
#define IN_RATE         24000
#define IN_CHANNELS     1

#define AUDIO_PKTS      8               /* USB frames per isochronous transfer */
#define NUM_XFERS       4               /* transfers in flight */

/* buffer_size = rate * channels * sizeof(s16) * interval / 1000 */
#define OUT_BUF_SIZE    (OUT_RATE * OUT_CHANNELS * 2 * GIP_AUDIO_INTERVAL / 1000)  /* 1536 */
#define OUT_FRAG_SIZE   (OUT_BUF_SIZE / AUDIO_PKTS)                                /* 192  */

#define IN_BUF_SIZE     (IN_RATE * IN_CHANNELS * 2 * GIP_AUDIO_INTERVAL / 1000)    /* 384  */
#define IN_FRAG_SIZE    (IN_BUF_SIZE / AUDIO_PKTS)                                 /* 48   */

/* max mic samples per USB frame (128 B endpoint / 2 B per sample = 64) */
#define MIC_SAMPLES_MAX  64

static volatile sig_atomic_t stop;
static volatile sig_atomic_t running;

void bridge_stop(void) { stop = 1; }
bool bridge_is_running(void) { return running != 0; }

/* The live counter line is redrawn twice a second with \r. That is useful at a
 * terminal and pure noise anywhere else (a log file, or the menu bar app's
 * captured stdout), so it is opt-in. Handshake logging stays on: it is what
 * makes a failed pairing diagnosable. */
static bool verbose;

void bridge_set_verbose(bool on) { verbose = on; }

static void log_line(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	fflush(stdout);
}

static gipusb   u;
static uint8_t  seq;
static uint8_t  audio_seq;
static uint8_t  auth_seq;
static bool     streaming;
static int      out_packet_size;
static uint16_t in_req_pkt_size;
static ring_t  *ring;
static struct gip_auth auth;
static uint8_t auth_out_buf[4096];
static size_t auth_out_len, auth_out_off;
static uint8_t auth_out_seq;
static size_t auth_out_total;
static bool auth_out_terminator_pending;
static uint8_t auth_in_buf[4096];
static size_t auth_in_len, auth_in_total;
static bool auth_armed;

static void start_auth_after_status(void)
{
#if !HEADSET_NEEDS_AUTH
	return;
#else
	if (!auth_armed || auth.started) return;
	/* Windows waits about 60 ms after Set Device State = Start before its
	 * first authentication packet.  Keep that quiet interval so the PDP has
	 * finished transitioning from Idle to Active. */
	usleep(50000);
	if (auth_armed && !auth.started)
		gip_auth_start(&auth);
#endif
}

static unsigned underruns;
static uint64_t frames_out;
static uint64_t frames_in;
static uint64_t next_out_frame;
static uint64_t next_in_frame;
static unsigned submitted_out, completed_out, errors_out;
static unsigned submitted_in, completed_in, errors_in;
static uint64_t in_usb_packets, in_usb_bytes, in_audio_packets, in_bad_packets;
static bool headset_audio_ready;
static bool restarting_capture;

/* IOKit refuses an isochronous transfer whose first USB frame is already in
 * the past.  Leave generous room for control/authentication callbacks. */
#define ISO_LEAD_FRAMES 64
/* Windows' working capture submits one 54-byte GIP audio packet per 1 ms
 * slot: 6-byte GIP header + 48 bytes of 24 kHz mono PCM. */
#define MIC_USB_PACKET_SIZE 54

struct xfer {
	uint8_t       *buf;
	IOUSBIsocFrame list[AUDIO_PKTS];
};
static struct xfer out_xfers[NUM_XFERS];
static struct xfer in_xfers[NUM_XFERS];

static void reset_counters(void)
{
	seq = 1;
	audio_seq = 1;
	auth_seq = 1;
	auth_armed = false;
	auth_out_len = auth_out_off = auth_out_total = 0;
	auth_out_terminator_pending = false;
	streaming = false;
	underruns = 0;
	frames_out = 0;
	frames_in = 0;
	submitted_out = completed_out = errors_out = 0;
	submitted_in = completed_in = errors_in = 0;
	in_usb_packets = in_usb_bytes = in_audio_packets = in_bad_packets = 0;
	headset_audio_ready = false;
}

static void free_xfers(void)
{
	for (int i = 0; i < NUM_XFERS; i++) {
		free(out_xfers[i].buf);
		free(in_xfers[i].buf);
		out_xfers[i].buf = NULL;
		in_xfers[i].buf = NULL;
	}
}

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

static int auth_send(void *ctx, const void *payload, size_t len, bool acknowledge)
{
	uint8_t buf[64] = {0};
	uint8_t packet_seq;
	/* PR #200: delay every AUTH message; several PDP/legacy headset firmwares
	 * incorrectly treat back-to-back AUTH replies as fragmented traffic. */
	usleep(50000);
	packet_seq = auth_seq++;
	if (!packet_seq)
		packet_seq = auth_seq++;
	if (len > 58) {
		if (len > sizeof(auth_out_buf)) return -1;
		memcpy(auth_out_buf, payload, len);
		auth_out_len = len;
		auth_out_off = 0;
		auth_out_total = len;
		auth_out_terminator_pending = false;
		auth_out_seq = packet_seq;
	}
	struct gip_header hdr = {
		.command = GIP_CMD_AUTHENTICATE,
		.options = GIP_OPT_INTERNAL | (acknowledge ? GIP_OPT_ACKNOWLEDGE : 0),
		.sequence = packet_seq,
		.packet_length = (uint32_t)(len > 58 ? 58 : len),
	};
	if (len > 58) {
		hdr.options |= GIP_OPT_CHUNK | GIP_OPT_CHUNK_START;
		hdr.chunk_offset = (uint32_t)len;
		len = 58;
	}
	int h = gip_encode_header(&hdr, buf);
	if (h + (int)len > (int)sizeof(buf)) {
		fprintf(stderr, "  AUTH packet too large for interrupt endpoint (%zu)\n", len);
		return -1;
	}
	memcpy(buf + h, len == 58 && auth_out_len ? auth_out_buf : payload, len);
	if (auth_out_len) auth_out_off = len;
	int ret = gipusb_write(&u, buf, (uint32_t)(h + len));
	if (ret != kIOReturnSuccess)
		fprintf(stderr, "  AUTH first fragment write failed: 0x%08x\n", ret);
	else if (auth_out_len)
		fprintf(stderr, "  --> AUTH fragment 0/%zu\n", auth_out_len);
	return ret;
}

static void auth_send_next_chunk(uint8_t sequence)
{
	uint8_t buf[64] = {0};
	if (!auth_out_len || auth_out_off >= auth_out_len) {
		if (auth_out_terminator_pending) {
			struct gip_header hdr = {
				.command = GIP_CMD_AUTHENTICATE,
				.options = GIP_OPT_INTERNAL | GIP_OPT_CHUNK,
				.sequence = auth_out_seq ? auth_out_seq : sequence,
				.packet_length = 0,
				.chunk_offset = (uint32_t)auth_out_total,
			};
			/* Windows sends the zero-length completion about 12 ms after the
			 * final-fragment acknowledgement.  The PDP firmware ignores an
			 * immediate completion submitted from the ACK callback. */
			usleep(12000);
			int h = gip_encode_header(&hdr, buf);
			if (gipusb_write(&u, buf, (uint32_t)h) == kIOReturnSuccess) {
				fprintf(stderr, "  --> AUTH fragment terminator %zu/%zu\n",
					auth_out_total, auth_out_total);
				auth_out_terminator_pending = false;
			}
		}
		return;
	}

	/* The PDP acknowledges the first fragment, then expects all remaining
	 * fragments back-to-back.  Only the final one requests an acknowledgement.
	 * (Windows: f0, a0, a0, ..., b0.) */
	while (auth_out_off < auth_out_len) {
		size_t n = auth_out_len - auth_out_off;
		bool final;
		/* The Windows trace spaces the encrypted-secret continuations by
		 * roughly four milliseconds.  This PDP firmware drops a burst even
		 * though the GIP fragment contents are otherwise valid. */
		usleep(auth_out_off == 58 ? 8000 : 4000);
		if (n > 58) n = 58;
		final = auth_out_off + n == auth_out_len;
		struct gip_header hdr = { .command = GIP_CMD_AUTHENTICATE,
			.options = GIP_OPT_INTERNAL | GIP_OPT_CHUNK |
				(final ? GIP_OPT_ACKNOWLEDGE : 0),
			.sequence = auth_out_seq ? auth_out_seq : sequence,
			.packet_length = (uint32_t)n,
			.chunk_offset = (uint32_t)auth_out_off };
		int h = gip_encode_header(&hdr, buf);
		memcpy(buf + h, auth_out_buf + auth_out_off, n);
		if (gipusb_write(&u, buf, (uint32_t)(h + n)) != 0) {
			fprintf(stderr, "  AUTH fragment write failed at %zu/%zu\n",
				auth_out_off, auth_out_len);
			return;
		}
		fprintf(stderr, "  --> AUTH fragment %zu/%zu%s\n", auth_out_off,
			auth_out_len, final ? " (final)" : "");
		auth_out_off += n;
	}
	auth_out_len = 0;
	auth_out_terminator_pending = true;
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

static void pdp_prepare_start(void)
{
	/* Windows sends this PDP-specific extended state payload immediately
	 * before Start.  It is distinct from the normal one-byte state command. */
	static const uint8_t state[] = {
		0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x55,
		0x53, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	};
	static const uint8_t led[] = { 0x00, 0x01, 0x14 };

	printf("  --> PDP extended state; LED initialize\n");
	send_pkt(GIP_CMD_POWER, GIP_OPT_INTERNAL, 0, state, sizeof(state));
	set_power(GIP_PWR_ON);
	send_pkt(GIP_CMD_LED, GIP_OPT_INTERNAL, 0, led, sizeof(led));
}

/* frames of PCM carried by one output transfer */
#define OUT_XFER_FRAMES (AUDIO_PKTS * OUT_FRAG_SIZE / (int)sizeof(int16_t) / OUT_CHANNELS)

static void fill_out_xfer(struct xfer *x)
{
	struct gip_header hdr = {
		.command = GIP_CMD_AUDIO_SAMPLES,
		.options = GIP_OPT_INTERNAL,
		.packet_length = OUT_FRAG_SIZE,
	};
	static float scratch[OUT_XFER_FRAMES * OUT_CHANNELS];
	uint32_t got = 0;
	int frame = 0;

	if (ring)
		got = ring_out_read(ring, scratch, OUT_XFER_FRAMES);
	else
		memset(scratch, 0, sizeof(scratch));

	/* Only a short read *while the HAL is actually running IO* is starvation.
	 * When nothing is playing, coreaudiod stops the plug-in's IO and the ring
	 * stops being written, but this isochronous stream keeps going at 125
	 * transfers a second -- so counting those made the number meaningless:
	 * 8.7M "underruns" was simply 19.4 hours of an idle device. */
	if (got < (uint32_t)OUT_XFER_FRAMES &&
	    ring && atomic_load(&ring->out_io_running))
		underruns++;
	frames_out += got;

	/* No host-side volume is applied here. vol_out is a handshake artifact on
	 * this dongle, not a live dial (see AGENTS.md), so scaling by it silences
	 * playback the moment the value is stale or zero — which is exactly what
	 * happened. macOS-side volume belongs in the HAL plug-in, where the system
	 * volume control actually drives it. */

	for (int p = 0; p < AUDIO_PKTS; p++) {
		uint8_t *dest = x->buf + p * out_packet_size;
		int hdr_len;
		int16_t *pcm;

		do {
			hdr.sequence = audio_seq++;
		} while (!hdr.sequence);

		hdr_len = gip_encode_header(&hdr, dest);
		pcm = (int16_t *)(dest + hdr_len);

		for (int i = 0; i < OUT_FRAG_SIZE / (int)sizeof(int16_t) / OUT_CHANNELS; i++) {
			for (int c = 0; c < OUT_CHANNELS; c++) {
				float v = scratch[frame * OUT_CHANNELS + c];

				if (v > 1.0f)
					v = 1.0f;
				else if (v < -1.0f)
					v = -1.0f;
				pcm[i * OUT_CHANNELS + c] = (int16_t)(v * 32767.0f);
			}
			frame++;
		}
	}
}

static void submit_out_xfer(struct xfer *x);
static volatile sig_atomic_t dev_disconnected;

static void out_xfer_done(void *refcon, IOReturn result, void *arg0)
{
	struct xfer *x = refcon;

	completed_out++;
	if (result != kIOReturnSuccess) {
		if (errors_out++ < 5)
			fprintf(stderr, "iso write failed: 0x%08x\n", result);
		if (result == kIOReturnAborted && restarting_capture)
			return;
		/* AbortPipe is used deliberately when restarting capture after auth;
		 * kIOReturnAborted is not evidence that the dongle disconnected. */
		if (result == kIOReturnNoDevice) {
			dev_disconnected = 1;
			return;
		}
		next_out_frame = gipusb_frame_number(&u) + ISO_LEAD_FRAMES;
	}

	if (!stop && streaming && !dev_disconnected)
		submit_out_xfer(x);
}

static void submit_out_xfer(struct xfer *x)
{
	IOReturn ret;

	fill_out_xfer(x);
	for (int i = 0; i < AUDIO_PKTS; i++) {
		x->list[i].frStatus = 0;
		x->list[i].frReqCount = (UInt16)out_packet_size;
		x->list[i].frActCount = 0;
	}

	ret = gipusb_iso_write(&u, x->buf, next_out_frame, AUDIO_PKTS, x->list,
			       out_xfer_done, x);
	if (ret != kIOReturnSuccess) {
		if (errors_out++ < 5)
			fprintf(stderr, "WriteIsochPipeAsync failed: 0x%08x\n", ret);
		/* The previous schedule has expired; rebase this transfer instead of
		 * losing the xfer permanently. */
		next_out_frame = gipusb_frame_number(&u) + ISO_LEAD_FRAMES;
		ret = gipusb_iso_write(&u, x->buf, next_out_frame, AUDIO_PKTS,
				       x->list, out_xfer_done, x);
		if (ret != kIOReturnSuccess)
			return;
	}

	next_out_frame += AUDIO_PKTS;
	submitted_out++;
}

/* Process incoming mic samples (24 kHz mono S16 -> resample to 48 kHz mono float) */
static void process_incoming_mic(const uint8_t *src, uint32_t act)
{
	struct gip_header hdr;
	int hdr_len = gip_decode_header(&hdr, src, act);
	const uint8_t *payload;
	uint32_t payload_len;

	if (hdr_len <= 0 || hdr.command != GIP_CMD_AUDIO_SAMPLES)
		return;

	/* The USB packet can contain padding; trust the GIP payload length. */
	if ((uint32_t)hdr_len > act || hdr.packet_length > act - (uint32_t)hdr_len ||
	    hdr.packet_length < sizeof(struct gip_pkt_audio_samples))
		return;

	payload = src + hdr_len;
	payload_len = hdr.packet_length;
	int pcm_bytes = (int)payload_len - (int)sizeof(struct gip_pkt_audio_samples);
	if (pcm_bytes <= 0)
		return;

	const int16_t *pcm = (const int16_t *)(payload + sizeof(struct gip_pkt_audio_samples));
	int in_samples = pcm_bytes / (int)sizeof(int16_t);
	float out_buf[MIC_SAMPLES_MAX * 2]; /* fixed-size, no VLA */
	uint32_t out_count = 0;

	for (int i = 0; i < in_samples && out_count < sizeof(out_buf) / sizeof(float); i++) {
		float cur = (float)pcm[i] / 32767.0f;
		float next = (i + 1 < in_samples) ? (float)pcm[i + 1] / 32767.0f : cur;

		out_buf[out_count++] = cur;
		if (out_count < sizeof(out_buf) / sizeof(float))
			out_buf[out_count++] = (cur + next) * 0.5f;
	}

	if (ring && out_count > 0) {
		ring_in_write(ring, out_buf, out_count);
		frames_in += out_count;
	}
}

static void submit_in_xfer(struct xfer *x);

static void in_xfer_done(void *refcon, IOReturn result, void *arg0)
{
	struct xfer *x = refcon;

	completed_in++;
	if (result != kIOReturnSuccess) {
		if (errors_in++ < 5)
			fprintf(stderr, "iso read failed: 0x%08x\n", result);
		/* AbortPipe is used deliberately when restarting capture after auth;
		 * kIOReturnAborted is not evidence that the dongle disconnected. */
		if (result == kIOReturnNoDevice) {
			dev_disconnected = 1;
			return;
		}
		next_in_frame = gipusb_frame_number(&u) + ISO_LEAD_FRAMES;
	} else {
		for (int p = 0; p < AUDIO_PKTS; p++) {
			uint32_t act = x->list[p].frActCount;
			uint8_t *pkt = x->buf + p * in_req_pkt_size;

			if (x->list[p].frStatus != kIOReturnSuccess)
				continue;
			if (act) {
				in_usb_packets++;
				in_usb_bytes += act;
			}
			if (act >= GIP_HDR_MIN_LENGTH && pkt[0] == GIP_CMD_AUDIO_SAMPLES) {
				in_audio_packets++;
				process_incoming_mic(pkt, act);
			} else if (act) {
				in_bad_packets++;
			}
		}
	}

	if (!stop && streaming && !dev_disconnected)
		submit_in_xfer(x);
}

static void submit_in_xfer(struct xfer *x)
{
	IOReturn ret;

	for (int i = 0; i < AUDIO_PKTS; i++) {
		x->list[i].frStatus = 0;
		x->list[i].frReqCount = in_req_pkt_size;
		x->list[i].frActCount = 0;
	}

	ret = gipusb_iso_read(&u, x->buf, next_in_frame, AUDIO_PKTS, x->list,
			      in_xfer_done, x);
	if (ret != kIOReturnSuccess) {
		if (errors_in++ < 5)
			fprintf(stderr, "ReadIsochPipeAsync failed: 0x%08x\n", ret);
		next_in_frame = gipusb_frame_number(&u) + ISO_LEAD_FRAMES;
		ret = gipusb_iso_read(&u, x->buf, next_in_frame, AUDIO_PKTS,
				      x->list, in_xfer_done, x);
		if (ret == kIOReturnSuccess) {
			next_in_frame += AUDIO_PKTS;
			submitted_in++;
		}
		return;
	}

	next_in_frame += AUDIO_PKTS;
	submitted_in++;
}

static void on_rx(void *ctx, const uint8_t *data, uint32_t len)
{
	struct gip_header hdr;
	int hdr_len = gip_decode_header(&hdr, data, len);

	if (hdr_len < 0)
		return;

	if (hdr.command == GIP_CMD_ANNOUNCE) {
		uint8_t power = GIP_PWR_ON;

		printf("\n  <-- ANNOUNCE; waking headset and requesting identity\n");
		send_pkt(GIP_CMD_POWER, GIP_OPT_INTERNAL, 0, &power, sizeof(power));
		send_pkt(GIP_CMD_IDENTIFY, GIP_OPT_INTERNAL, 0, NULL, 0);
	}
	if (hdr.command == GIP_CMD_STATUS) {
		const uint8_t *p = data + hdr_len;
		uint32_t plen = len - hdr_len;

		/* Byte 0 packs the battery type (bits 2-3) and level (bits 0-1).
		 * The headset emits this unprompted whenever either changes, so it
		 * is the only source of battery state we get. */
		if (plen >= 1 && ring) {
			uint32_t level = p[0] & GIP_STATUS_BATT_LEVEL;
			uint32_t type = (p[0] & GIP_STATUS_BATT_TYPE) >> 2;

			atomic_store(&ring->battery_level, level);
			atomic_store(&ring->battery_type, type);
			atomic_store(&ring->battery_seen, 1);
			log_line("  <-- STATUS battery type=%u level=%u\n", type, level);
		}
		start_auth_after_status();
	}

	if (hdr.command == GIP_CMD_AUDIO_CONTROL) {
		const uint8_t *p = data + hdr_len;
		uint32_t plen = len - hdr_len;

		if (plen >= 1) {
			char hex[3 * 16 + 1];
			uint32_t n = plen < 16 ? plen : 16;

			for (uint32_t i = 0; i < n; i++)
				snprintf(hex + i * 3, 4, "%02x ", p[i]);
			hex[n * 3 ? n * 3 - 1 : 0] = '\0';
			log_line("\n  <-- AUDIO_CONTROL subcommand=0x%02x payload=[%s]\n",
				 p[0], hex);
		}
		if (plen >= 5 && p[0] == GIP_AUD_CTRL_VOLUME_CHAT)
			headset_audio_ready = true;
		if (plen >= 2 && p[0] == GIP_AUD_CTRL_VOLUME)
			headset_audio_ready = true;
		if (plen >= 5 && p[0] == GIP_AUD_CTRL_VOLUME_CHAT)
			start_auth_after_status();
		/* The two volume subcommands have *different* field orders; see
		 * struct gip_pkt_audio_volume_chat and gip_pkt_audio_volume.
		 * This dongle sends 0x00, whose payload reads 00 04 60 00 64:
		 * mute=0x04 (exactly GIP_AUD_VOLUME_UNMUTED, which confirms the
		 * alignment), gain_out=96, out=0, in=100. The reported `out` is a
		 * constant 0 on this hardware — it is not a live dial. */
		if (plen >= 5 && p[0] == GIP_AUD_CTRL_VOLUME_CHAT && ring) {
			const struct gip_pkt_audio_volume_chat *v = (const void *)p;

			atomic_store(&ring->mic_muted,
				     v->mute == GIP_AUD_VOLUME_MIC_MUTED);
			atomic_store(&ring->vol_gain_out, v->gain_out);
			atomic_store(&ring->vol_out, v->out);
			atomic_store(&ring->vol_in, v->in);
			atomic_store(&ring->vol_seen, 1);
		} else if (plen >= 5 && p[0] == GIP_AUD_CTRL_VOLUME && ring) {
			const struct gip_pkt_audio_volume *v = (const void *)p;

			atomic_store(&ring->mic_muted,
				     v->mute == GIP_AUD_VOLUME_MIC_MUTED);
			atomic_store(&ring->vol_out, v->out);
			atomic_store(&ring->vol_in, v->in);
			atomic_store(&ring->vol_seen, 1);
		}
	}
	if (hdr.command == GIP_CMD_AUTHENTICATE) {
		const uint8_t *p = data + hdr_len;
		uint32_t plen = len - hdr_len;
		const uint8_t *auth_data = p;
		size_t auth_len = plen;
		if (hdr.options & GIP_OPT_CHUNK) {
			uint32_t offset = hdr.chunk_offset;
			if (hdr.options & GIP_OPT_CHUNK_START) {
				/* In a first GIP fragment, chunk_offset is the total message
				 * length.  The fragment itself always starts at byte zero. */
				auth_in_total = hdr.chunk_offset;
				auth_in_len = 0;
				offset = 0;
			}
			if (offset + plen <= sizeof(auth_in_buf)) {
				memcpy(auth_in_buf + offset, p, plen);
				if (offset + plen > auth_in_len) auth_in_len = offset + plen;
			}
			/* GIP chunk transfers are committed by an explicit zero-length
			 * terminator.  Receiving every data byte is not sufficient: replying
			 * before that terminator races the PDP's authentication state. */
			if (plen == 0 && auth_in_total && offset == auth_in_total &&
			    auth_in_len == auth_in_total) {
				auth_data = auth_in_buf;
				auth_len = auth_in_total;
				auth_in_total = 0;
				auth_in_len = 0;
			} else
				auth_len = 0;
		}
		if (auth_len && gip_auth_process(&auth, auth_data, auth_len) < 0)
			fprintf(stderr, "  AUTH processing error (len=%u)\n", plen);
	}
	if (hdr.command == GIP_CMD_ACKNOWLEDGE && hdr_len + 5 <= len) {
		const uint8_t *p = data + hdr_len;
		if (p[1] == GIP_CMD_AUTHENTICATE) {
			fprintf(stderr, "  <-- AUTH GIP acknowledgement (options=0x%02x)\n",
				p[2]);
			auth_send_next_chunk(hdr.sequence);
			/* This PDP firmware acknowledges the client-finish request but,
			 * like the Windows capture, sends no client-finish payload. */
			if (auth.last_cmd == 0x08 && !auth.authenticated)
				gip_auth_complete_without_client_finish(&auth);
		}
	}

	/* Log control packets nothing above handles. The headset's volume dial and
	 * mute button do not appear on AUDIO_CONTROL — every one of those is
	 * byte-identical and only arrives during the handshake — so if the dial
	 * reports at all, it reports here. Capped so an unexpected talker cannot
	 * fill the log. */
	switch (hdr.command) {
	case GIP_CMD_ANNOUNCE:
	case GIP_CMD_STATUS:
	case GIP_CMD_AUDIO_CONTROL:
	case GIP_CMD_AUTHENTICATE:
	case GIP_CMD_ACKNOWLEDGE:
	case GIP_CMD_AUDIO_SAMPLES:
		break;
	default: {
		static unsigned logged;
		const uint8_t *p = data + hdr_len;
		uint32_t plen = len - hdr_len;
		char hex[3 * 16 + 1];
		uint32_t n = plen < 16 ? plen : 16;

		if (logged++ < 200) {
			for (uint32_t i = 0; i < n; i++)
				snprintf(hex + i * 3, 4, "%02x ", p[i]);
			hex[n ? n * 3 - 1 : 0] = '\0';
			log_line("  <-- %s (0x%02x) len=%u payload=[%s]\n",
				 gip_command_name(hdr.command), hdr.command,
				 plen, hex);
		}
		break;
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

/* Attempt one connect+stream cycle. Returns 0 on clean exit, 1 on error, 2 on disconnect. */
static int run_session(void)
{
	struct gip_header probe_hdr = { .packet_length = OUT_FRAG_SIZE };
	uint8_t tmp[16];
	double t;

	reset_counters();
	dev_disconnected = 0;

	out_packet_size = gip_encode_header(&probe_hdr, tmp) + OUT_FRAG_SIZE;

	printf("re-enumerating %04x:%04x\n", VID_PDP, PID_LVL50);
	if (gipusb_reenumerate(VID_PDP, PID_LVL50) == 0) {
		struct timespec ts = { .tv_sec = 2 };
		nanosleep(&ts, NULL);
	}

	if (gipusb_open(&u, VID_PDP, PID_LVL50, GIP_INTF_DATA) < 0)
		return 2;
	if (gipusb_start_reader(&u, on_rx, &u) < 0) {
		gipusb_close(&u);
		return 2;
	}
#if HEADSET_NEEDS_AUTH
	/* Prepare authentication before the reader can receive any control
	 * messages, but do not start it until we have explicitly sent Start. */
	gip_auth_init(&auth, auth_send, &u);
#endif

	printf("handshake\n");
	set_power(GIP_PWR_ON);
	CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.3, false);
	send_pkt(GIP_CMD_IDENTIFY, GIP_OPT_INTERNAL, 0, NULL, 0);
	for (t = 0; t < 1.0; t += 0.1)
		CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.1, false);

	printf("\nenabling audio interface\n");
	if (gipusb_open_audio(&u) < 0) {
		gipusb_close(&u);
		return 2;
	}
	printf("iso out ep=0x%02x (%u B) in=0x%02x (%u B)\n",
	       u.iso_ep_out, u.iso_max_out, u.iso_ep_in | 0x80, u.iso_max_in);

	if (out_packet_size > u.iso_max_out) {
		fprintf(stderr, "output packet %d exceeds endpoint max %u\n",
			out_packet_size, u.iso_max_out);
		gipusb_close(&u);
		return 1;
	}
	if (!u.iso_pipe_in) {
		fprintf(stderr, "isochronous input pipe not found\n");
		gipusb_close(&u);
		return 1;
	}

	/* The endpoint advertises 128 bytes, but this headset only emits 54-byte
	 * capture packets.  Request the packet size observed in the Windows trace;
	 * some macOS USB stacks complete oversized IN frames as zero bytes. */
	in_req_pkt_size = MIC_USB_PACKET_SIZE;

	for (int i = 0; i < NUM_XFERS; i++) {
		out_xfers[i].buf = calloc(1, (size_t)out_packet_size * AUDIO_PKTS);
		in_xfers[i].buf  = calloc(1, (size_t)in_req_pkt_size * AUDIO_PKTS);
		if (!out_xfers[i].buf || !in_xfers[i].buf) {
			free_xfers();
			gipusb_close(&u);
			return 1;
		}
	}

	uint64_t bus_frame = gipusb_frame_number(&u);
	next_out_frame = bus_frame + ISO_LEAD_FRAMES;
	next_in_frame = bus_frame + ISO_LEAD_FRAMES;
	/* The PDP firmware will not produce control/auth replies unless its IN
	 * endpoint has a pending transfer.  Arm capture early, but keep playback
	 * deferred until authentication is complete. */
	streaming = true;
	for (int i = 0; i < NUM_XFERS; i++)
		submit_in_xfer(&in_xfers[i]);

	/* Exact headset configuration order from xone: force idle first, suggest
	 * the format, then allow the device time to prepare before POWER ON. */
	set_power(GIP_PWR_SLEEP);
	CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.1, false);
	set_audio_format(GIP_AUD_FORMAT_24KHZ_MONO, GIP_AUD_FORMAT_48KHZ_STEREO);
	/* The captured Windows driver gives the PDP firmware about one second to
	 * apply its formats before the extended state, Start, and LED sequence. */
	CFRunLoopRunInMode(kCFRunLoopDefaultMode, 1.0, false);
	pdp_prepare_start();
	/* The Windows capture begins the v2 authentication exchange 59 ms after
	 * this Set Device State = Start command. */
	auth_armed = true;
	start_auth_after_status();
	/* Audio transfers must not begin until the authentication state machine has
	 * accepted the client's Finished record.  ISO callbacks and auth messages
	 * share this run loop, so beginning earlier can make the next ISO frame old. */
	for (int wait = 0; wait < 60 && !auth.authenticated && !stop; wait++) {
		CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.1, false);
		if (wait && wait % 10 == 9 && !auth.authenticated)
			set_power(GIP_PWR_ON);
	}
	if (!auth.authenticated) {
		fprintf(stderr, "authentication did not complete; audio streams were not armed\n");
		free_xfers();
		gipusb_close(&u);
		return 1;
	}
	/* Authentication resets the accessory's streaming state.  Windows sends
	 * Set Device State = Start again at the point where the authenticated
	 * audio session is opened; without this, the IN endpoint remains silent
	 * even though the isochronous reads are completing. */
	printf("  --> AUDIO capture state: Start (post-auth)\n");
	set_power(GIP_PWR_ON);
	CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, false);
	/* The reads submitted before authentication have stale frame schedules.
	 * Reset the audio IN pipe so capture starts with a fresh USB frame window. */
	restarting_capture = true;
	(*u.audio)->AbortPipe(u.audio, u.iso_pipe_in);
	CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.02, false);
	next_in_frame = gipusb_frame_number(&u) + ISO_LEAD_FRAMES;
	restarting_capture = false;
	for (int i = 0; i < NUM_XFERS; i++)
		submit_in_xfer(&in_xfers[i]);
	printf("\nbridge running - bidirectional audio active\n");
	printf("(ctrl-c to stop)\n\n");

	/* Setup and authentication deliberately wait on control replies.  Do not
	 * queue real-time USB traffic until those waits have finished: otherwise
	 * its target frame is stale and IOKit reports kIOReturnIsoTooOld (0x2ee). */
	bus_frame = gipusb_frame_number(&u);
	/* The input transfers have remained armed throughout authentication; do
	 * not submit the same xfer objects a second time while they are pending. */
	next_in_frame = bus_frame + ISO_LEAD_FRAMES;
	next_out_frame = bus_frame + ISO_LEAD_FRAMES + NUM_XFERS * AUDIO_PKTS;
	streaming = true;
	for (int i = 0; i < NUM_XFERS; i++)
		submit_out_xfer(&out_xfers[i]);

	for (t = 0; !stop && !dev_disconnected; t += 0.5) {
		CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.5, false);
		if (!verbose)
			continue;
		printf("\r  out=%llu in=%llu in_usb=%llu/%lluB audio=%llu other=%llu underruns=%u out_err=%u in_err=%u   ",
		       (unsigned long long)frames_out, (unsigned long long)frames_in,
		       (unsigned long long)in_usb_packets, (unsigned long long)in_usb_bytes,
		       (unsigned long long)in_audio_packets, (unsigned long long)in_bad_packets,
		       underruns, errors_out, errors_in);
		fflush(stdout);
	}

	streaming = false;
	CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.3, false);

	if (verbose)
		printf("\n\nsubmitted_out=%u completed_out=%u submitted_in=%u completed_in=%u "
		       "in_usb=%llu/%lluB audio=%llu other=%llu underruns=%u\n",
		       submitted_out, completed_out, submitted_in, completed_in,
		       (unsigned long long)in_usb_packets, (unsigned long long)in_usb_bytes,
		       (unsigned long long)in_audio_packets, (unsigned long long)in_bad_packets,
		       underruns);

	free_xfers();
	gipusb_close(&u);

	if (dev_disconnected && !stop) {
		printf("\ndisconnected - waiting 2s before reconnect...\n");
		struct timespec ts = { .tv_sec = 2 };
		nanosleep(&ts, NULL);
		return 2;
	}

	return 0;
}

int bridge_run(void)
{
	stop = 0;
	running = 1;

	ring = ring_map(true);
	if (!ring) {
		fprintf(stderr, "could not map the shared ring buffer\n");
		running = 0;
		return 1;
	}
	/* ring_map deliberately preserves an existing ring so a bridge restart does
	 * not disturb a live HAL peer. The device-status fields must still be
	 * cleared: they describe the headset in front of us, not the last one. */
	ring_reset_status(ring);
	log_line("ring mapped: %u frames, %u out ch, %u in ch\n",
		 ring->capacity, ring->out_channels, ring->in_channels);

	/* auto-reconnect loop */
	while (!stop) {
		int rc;

		atomic_store(&ring->device_online, 1);
		rc = run_session();
		atomic_store(&ring->device_online, 0);

		if (rc == 0 || rc == 1)
			break;
		/* rc == 2: disconnect detected, loop reconnects */
	}

	log_line("\nexiting.\n");
	running = 0;
	return 0;
}
