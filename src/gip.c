/* SPDX-License-Identifier: GPL-2.0-or-later */
/* GIP header codec, ported from xone (https://github.com/medusalix/xone). */

#include "gip.h"

static int encode_varint(uint8_t *buf, uint32_t val)
{
	int i;

	for (i = 0; i < (int)sizeof(val); i++) {
		buf[i] = (uint8_t)val;
		if (val > 0x7f)
			buf[i] |= 0x80;

		val >>= 7;
		if (!val)
			break;
	}

	return i + 1;
}

static int decode_varint(const uint8_t *data, int len, uint32_t *val)
{
	int i;

	*val = 0;
	for (i = 0; i < (int)sizeof(*val) && i < len; i++) {
		*val |= (uint32_t)(data[i] & 0x7f) << (i * 7);
		if (!(data[i] & 0x80))
			break;
	}

	return i + 1;
}

/* header length before the "round up to even" rule is applied */
static int actual_header_length(const struct gip_header *hdr)
{
	uint32_t pkt_len = hdr->packet_length;
	uint32_t chunk_offset = hdr->chunk_offset;
	int len = GIP_HDR_MIN_LENGTH;

	do {
		len++;
		pkt_len >>= 7;
	} while (pkt_len);

	if (hdr->options & GIP_OPT_CHUNK) {
		while (chunk_offset) {
			len++;
			chunk_offset >>= 7;
		}
	}

	return len;
}

int gip_encode_header(const struct gip_header *hdr, uint8_t *buf)
{
	int n = 0;

	buf[n++] = hdr->command;
	buf[n++] = hdr->options;
	buf[n++] = hdr->sequence;

	n += encode_varint(buf + n, hdr->packet_length);

	/* header length must be even: extend the varint with a zero byte */
	if (actual_header_length(hdr) % 2) {
		buf[n - 1] |= 0x80;
		buf[n++] = 0;
	}

	if (hdr->options & GIP_OPT_CHUNK)
		n += encode_varint(buf + n, hdr->chunk_offset);

	return n;
}

int gip_decode_header(struct gip_header *hdr, const uint8_t *data, int len)
{
	int n = 0;

	if (len < GIP_HDR_MIN_LENGTH + 1)
		return -1;

	hdr->command = data[n++];
	hdr->options = data[n++];
	hdr->sequence = data[n++];
	hdr->packet_length = 0;
	hdr->chunk_offset = 0;

	n += decode_varint(data + n, len - n, &hdr->packet_length);

	if (hdr->options & GIP_OPT_CHUNK)
		n += decode_varint(data + n, len - n, &hdr->chunk_offset);

	return n;
}

const char *gip_command_name(uint8_t cmd)
{
	switch (cmd) {
	case GIP_CMD_ACKNOWLEDGE:   return "ACKNOWLEDGE";
	case GIP_CMD_ANNOUNCE:      return "ANNOUNCE";
	case GIP_CMD_STATUS:        return "STATUS";
	case GIP_CMD_IDENTIFY:      return "IDENTIFY";
	case GIP_CMD_POWER:         return "POWER";
	case GIP_CMD_AUTHENTICATE:  return "AUTHENTICATE";
	case GIP_CMD_VIRTUAL_KEY:   return "VIRTUAL_KEY";
	case GIP_CMD_AUDIO_CONTROL: return "AUDIO_CONTROL";
	case GIP_CMD_RUMBLE:        return "RUMBLE";
	case GIP_CMD_LED:           return "LED";
	case GIP_CMD_HID_REPORT:    return "HID_REPORT";
	case GIP_CMD_FIRMWARE:      return "FIRMWARE";
	case GIP_CMD_SERIAL_NUMBER: return "SERIAL_NUMBER";
	case GIP_CMD_INPUT:         return "INPUT";
	case GIP_CMD_AUDIO_SAMPLES: return "AUDIO_SAMPLES";
	default:                    return "UNKNOWN";
	}
}

const char *gip_audio_format_name(uint8_t fmt)
{
	switch (fmt) {
	case GIP_AUD_FORMAT_16KHZ_MONO:   return "16 kHz mono";
	case GIP_AUD_FORMAT_24KHZ_MONO:   return "24 kHz mono";
	case GIP_AUD_FORMAT_48KHZ_STEREO: return "48 kHz stereo";
	default:                          return "unknown";
	}
}
