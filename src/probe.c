/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * gip-probe - milestone 1
 *
 * Claims interface 0 of an Xbox GIP device, runs the announce/identify
 * handshake, and dumps everything the device says. The point is to learn
 * which audio formats this dongle advertises before any audio code exists.
 */

#include "gip.h"
#include "gipusb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>

#define VID_PDP        0x0e6f
#define PID_LVL50      0x0234
#define BUF_LEN        64
#define DEFAULT_SECS   10

static volatile sig_atomic_t stop;
static void on_sigint(int sig) { (void)sig; stop = 1; }

static uint8_t  seq = 1;
static uint8_t *chunk;
static uint32_t chunk_len, chunk_got;
static uint8_t  chunk_cmd;
static int      packets;
static bool     announced;

static double now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static void hexdump(const char *label, const uint8_t *p, uint32_t len)
{
	printf("    %s (%u bytes)\n", label, len);
	for (uint32_t i = 0; i < len; i += 16) {
		printf("      %04x  ", i);
		for (uint32_t j = 0; j < 16; j++)
			if (i + j < len)
				printf("%02x ", p[i + j]);
			else
				printf("   ");
		printf(" |");
		for (uint32_t j = 0; j < 16 && i + j < len; j++)
			printf("%c", p[i + j] >= 32 && p[i + j] < 127 ? p[i + j] : '.');
		printf("|\n");
	}
}

static int send_pkt(gipusb *u, uint8_t cmd, uint8_t options, uint8_t sequence,
		    const void *payload, uint32_t payload_len)
{
	uint8_t buf[BUF_LEN] = {0};
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

	printf("  --> %-14s seq=%u len=%u\n", gip_command_name(cmd),
	       hdr.sequence, payload_len);

	return gipusb_write(u, buf, hdr_len + payload_len);
}

static void send_ack(gipusb *u, const struct gip_header *in)
{
	struct gip_pkt_acknowledge pkt = {0};
	uint32_t len = in->chunk_offset + in->packet_length;

	pkt.command = in->command;
	pkt.options = GIP_OPT_INTERNAL;
	pkt.length = (uint16_t)len;
	/* xone computes this unguarded; devices expect the wrapped value */
	if ((in->options & GIP_OPT_CHUNK) && chunk)
		pkt.remaining = (uint16_t)(chunk_len - len);

	send_pkt(u, GIP_CMD_ACKNOWLEDGE, GIP_OPT_INTERNAL, in->sequence,
		 &pkt, sizeof(pkt));
}

static void parse_announce(const uint8_t *p, uint32_t len)
{
	struct gip_pkt_announce a;

	if (len < sizeof(a))
		return;
	memcpy(&a, p, sizeof(a));

	printf("    device %04x:%04x  fw %u.%u.%u.%u  hw %u.%u.%u.%u\n",
	       a.vendor_id, a.product_id,
	       a.fw_version.major, a.fw_version.minor,
	       a.fw_version.build, a.fw_version.revision,
	       a.hw_version.major, a.hw_version.minor,
	       a.hw_version.build, a.hw_version.revision);
}

/* info elements are: one count byte, then count * item_size bytes */
static void dump_info_element(const char *label, const uint8_t *p, uint32_t len,
			      uint16_t offset, uint8_t item_size, bool as_audio)
{
	uint8_t count;

	if (!offset || offset >= len)
		return;

	count = p[offset];
	if (offset + 1 + (uint32_t)count * item_size > len)
		return;

	printf("    %s: count=%u\n", label, count);
	for (uint8_t i = 0; i < count; i++) {
		const uint8_t *e = p + offset + 1 + (uint32_t)i * item_size;

		printf("      [%u] ", i);
		for (uint8_t j = 0; j < item_size; j++)
			printf("%02x ", e[j]);
		if (as_audio && item_size == 2)
			printf(" in=%s out=%s",
			       gip_audio_format_name(e[0]),
			       gip_audio_format_name(e[1]));
		printf("\n");
	}
}

static void dump_classes(const uint8_t *b, uint32_t len, uint16_t offset)
{
	uint8_t count;
	uint32_t pos;

	if (!offset || offset >= len)
		return;

	count = b[offset];
	pos = offset + 1;
	printf("    device classes: count=%u\n", count);

	for (uint8_t i = 0; i < count && pos + 2 <= len; i++) {
		uint16_t slen = (uint16_t)(b[pos] | (b[pos + 1] << 8));

		pos += 2;
		if (pos + slen > len)
			break;
		printf("      [%u] %.*s\n", i, (int)slen, (const char *)b + pos);
		pos += slen;
	}
}

static void parse_identify(const uint8_t *p, uint32_t len)
{
	struct gip_pkt_identify id;
	const uint8_t *base;
	uint32_t base_len;

	if (len < sizeof(id)) {
		printf("    identify payload too short (%u)\n", len);
		return;
	}
	memcpy(&id, p, sizeof(id));

	/*
	 * Verified empirically against the LVL50 dongle: the info-element
	 * offsets are relative to the start of the offset table itself,
	 * i.e. 16 bytes into the payload - not to the payload start.
	 */
	base = p + 16;
	base_len = len - 16;

	printf("    offsets: cmds=%u fw=%u audio=%u capout=%u capin=%u "
	       "classes=%u intf=%u hid=%u\n",
	       id.client_commands_offset, id.firmware_versions_offset,
	       id.audio_formats_offset, id.capabilities_out_offset,
	       id.capabilities_in_offset, id.classes_offset,
	       id.interfaces_offset, id.hid_descriptor_offset);

	dump_classes(base, base_len, id.classes_offset);

	printf("\n  *** AUDIO FORMATS ***\n");
	dump_info_element("audio formats", base, base_len,
			  id.audio_formats_offset, 2, true);

	dump_info_element("capabilities out", base, base_len,
			  id.capabilities_out_offset, 1, false);
	dump_info_element("capabilities in", base, base_len,
			  id.capabilities_in_offset, 1, false);
}

static void handle_packet(gipusb *u, const uint8_t *data, uint32_t len)
{
	struct gip_header hdr;
	int hdr_len = gip_decode_header(&hdr, data, len);
	const uint8_t *payload;
	uint32_t payload_len;

	if (hdr_len < 0)
		return;

	payload = data + hdr_len;
	payload_len = len - hdr_len;
	if (payload_len > hdr.packet_length)
		payload_len = hdr.packet_length;

	printf("  <-- %-14s (0x%02x) opt=0x%02x seq=%u len=%u",
	       gip_command_name(hdr.command), hdr.command,
	       hdr.options, hdr.sequence, hdr.packet_length);
	if (hdr.options & GIP_OPT_CHUNK)
		printf(" chunk_off=%u", hdr.chunk_offset);
	printf("\n");

	hexdump("raw", data, len);

	/*
	 * A chunk-start packet carries the total length in chunk_offset.
	 * xone zeroes it before acknowledging, and the transfer completes
	 * when an empty chunk arrives - not when the byte count is reached.
	 */
	if (hdr.options & GIP_OPT_CHUNK_START) {
		free(chunk);
		chunk_len = hdr.chunk_offset;
		chunk_got = 0;
		chunk_cmd = hdr.command;
		chunk = calloc(1, chunk_len ? chunk_len : 1);
		hdr.chunk_offset = 0;
		printf("    chunk start: total=%u\n", chunk_len);
	}

	if (hdr.options & GIP_OPT_ACKNOWLEDGE)
		send_ack(u, &hdr);

	if (hdr.options & GIP_OPT_CHUNK) {
		if (!chunk || hdr.command != chunk_cmd)
			return;

		if (hdr.packet_length) {
			if (hdr.chunk_offset + payload_len <= chunk_len) {
				memcpy(chunk + hdr.chunk_offset, payload, payload_len);
				chunk_got = hdr.chunk_offset + payload_len;
				printf("    chunk: off=%u len=%u (%u/%u)\n",
				       hdr.chunk_offset, payload_len,
				       chunk_got, chunk_len);
			}
			return;
		}

		/* empty chunk signals completion */
		printf("  === reassembled %s (%u bytes)\n",
		       gip_command_name(chunk_cmd), chunk_len);
		hexdump("payload", chunk, chunk_len);
		if (chunk_cmd == GIP_CMD_IDENTIFY)
			parse_identify(chunk, chunk_len);
		free(chunk);
		chunk = NULL;
		chunk_len = chunk_got = 0;
		return;
	}

	switch (hdr.command) {
	case GIP_CMD_ANNOUNCE: {
		uint8_t mode = GIP_PWR_ON;

		parse_announce(payload, payload_len);
		if (announced) {
			printf("  (already announced, not re-requesting)\n");
			break;
		}
		announced = true;
		printf("  (announce seen -> powering on and requesting identify)\n");
		send_pkt(u, GIP_CMD_POWER, GIP_OPT_INTERNAL, 0, &mode, sizeof(mode));
		send_pkt(u, GIP_CMD_IDENTIFY, GIP_OPT_INTERNAL, 0, NULL, 0);
		break;
	}
	case GIP_CMD_IDENTIFY:
		parse_identify(payload, payload_len);
		break;
	case GIP_CMD_STATUS:
		if (payload_len >= 1)
			printf("    status=0x%02x connected=%d battery=%u\n",
			       payload[0], !!(payload[0] & 0x80), payload[0] & 0x03);
		break;
	}
}

static void on_rx(void *ctx, const uint8_t *data, uint32_t len)
{
	printf("[%8.1f ms]\n", now_ms());
	handle_packet(ctx, data, len);
	packets++;
}

int main(int argc, char **argv)
{
	gipusb u;
	double deadline;
	int secs = argc > 1 ? atoi(argv[1]) : DEFAULT_SECS;

	signal(SIGINT, on_sigint);

	printf("gip-probe: re-enumerating %04x:%04x\n", VID_PDP, PID_LVL50);
	if (gipusb_reenumerate(VID_PDP, PID_LVL50) == 0) {
		struct timespec ts = { .tv_sec = 2 };
		nanosleep(&ts, NULL);
	}

	printf("gip-probe: opening %04x:%04x interface %u\n",
	       VID_PDP, PID_LVL50, GIP_INTF_DATA);

	if (gipusb_open(&u, VID_PDP, PID_LVL50, GIP_INTF_DATA) < 0) {
		gipusb_close(&u);
		return 1;
	}

	printf("claimed. interrupt in=0x%02x (%u B) out=0x%02x (%u B)\n",
	       u.ep_in | 0x80, u.max_in, u.ep_out, u.max_out);

	if (gipusb_start_reader(&u, on_rx, &u) < 0) {
		gipusb_close(&u);
		return 1;
	}

	printf("listening %d s (ctrl-c to stop)\n\n", secs);

	/* nudge the device in case it announced before we attached */
	{
		uint8_t power = GIP_PWR_ON;
		send_pkt(&u, GIP_CMD_POWER, GIP_OPT_INTERNAL, 0,
			 &power, sizeof(power));
	}
	send_pkt(&u, GIP_CMD_IDENTIFY, GIP_OPT_INTERNAL, 0, NULL, 0);

	deadline = now_ms() + secs * 1000.0;
	while (!stop && now_ms() < deadline)
		CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.2, true);

	printf("\ndone: %d packets\n", packets);
	if (!packets)
		printf("no traffic. is the headset powered on and paired to the dongle?\n");

	free(chunk);
	gipusb_close(&u);
	return 0;
}
