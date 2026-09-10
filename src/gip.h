/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * GIP (Gaming Input Protocol) definitions.
 *
 * Ported from xone by Severin von Wnuck-Lipinski:
 *   https://github.com/medusalix/xone  (GPL-2.0-or-later)
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#define GIP_HDR_MIN_LENGTH  3
#define GIP_PKT_MAX_LENGTH  58
#define GIP_AUDIO_INTERVAL  8   /* ms between audio packets */

enum gip_command {
	GIP_CMD_ACKNOWLEDGE   = 0x01,
	GIP_CMD_ANNOUNCE      = 0x02,
	GIP_CMD_STATUS        = 0x03,
	GIP_CMD_IDENTIFY      = 0x04,
	GIP_CMD_POWER         = 0x05,
	GIP_CMD_AUTHENTICATE  = 0x06,
	GIP_CMD_VIRTUAL_KEY   = 0x07,
	GIP_CMD_AUDIO_CONTROL = 0x08,
	GIP_CMD_RUMBLE        = 0x09,
	GIP_CMD_LED           = 0x0a,
	GIP_CMD_HID_REPORT    = 0x0b,
	GIP_CMD_FIRMWARE      = 0x0c,
	GIP_CMD_SERIAL_NUMBER = 0x1e,
	GIP_CMD_INPUT         = 0x20,
	GIP_CMD_AUDIO_SAMPLES = 0x60,
};

enum gip_option {
	GIP_OPT_ACKNOWLEDGE = (1 << 4),
	GIP_OPT_INTERNAL    = (1 << 5),
	GIP_OPT_CHUNK_START = (1 << 6),
	GIP_OPT_CHUNK       = (1 << 7),
};

enum gip_power_mode {
	GIP_PWR_ON    = 0x00,
	GIP_PWR_SLEEP = 0x01,
	GIP_PWR_OFF   = 0x04,
	GIP_PWR_RESET = 0x07,
};

enum gip_audio_format {
	GIP_AUD_FORMAT_16KHZ_MONO   = 0x05,
	GIP_AUD_FORMAT_24KHZ_MONO   = 0x09,
	GIP_AUD_FORMAT_48KHZ_STEREO = 0x10,
};

enum gip_audio_control {
	GIP_AUD_CTRL_VOLUME_CHAT = 0x00,
	GIP_AUD_CTRL_FORMAT_CHAT = 0x01,
	GIP_AUD_CTRL_FORMAT      = 0x02,
	GIP_AUD_CTRL_VOLUME      = 0x03,
};

/* The mute field of both volume packets is an enum, not a bitmask. Testing it
 * with `& 0x04` reports unmuted for the muted value too. */
enum gip_audio_volume_mute {
	GIP_AUD_VOLUME_UNMUTED   = 0x04,
	GIP_AUD_VOLUME_MIC_MUTED = 0x05,
};

/* GIP_AUD_CTRL_VOLUME_CHAT (0x00) payload, after the subcommand byte. */
struct gip_pkt_audio_volume_chat {
	uint8_t subcommand;
	uint8_t mute;
	uint8_t gain_out;
	uint8_t out;
	uint8_t in;
} __attribute__((packed));

/* GIP_AUD_CTRL_VOLUME (0x03) payload. Note the field order differs from the
 * chat variant: `out` comes before `chat`, and there is no gain byte. */
struct gip_pkt_audio_volume {
	uint8_t subcommand;
	uint8_t mute;
	uint8_t out;
	uint8_t chat;
	uint8_t in;
	uint8_t unknown1;
	uint8_t unknown2[2];
} __attribute__((packed));

struct gip_header {
	uint8_t  command;
	uint8_t  options;
	uint8_t  sequence;
	uint32_t packet_length;
	uint32_t chunk_offset;
};

/* GIP_CMD_STATUS payload. Byte 0 packs the battery type and level; the rest is
 * device-specific. Layout follows xone's gip_handle_pkt_status. */
struct gip_pkt_status {
	uint8_t  status;
	uint8_t  unknown[3];
} __attribute__((packed));

#define GIP_STATUS_BATT_LEVEL  0x03    /* bits 0-1 */
#define GIP_STATUS_BATT_TYPE   0x0c    /* bits 2-3 */

enum gip_battery_type {
	GIP_BATT_TYPE_NONE         = 0x00,
	GIP_BATT_TYPE_STANDARD     = 0x01,
	GIP_BATT_TYPE_RECHARGEABLE = 0x02,
};

enum gip_battery_level {
	GIP_BATT_LEVEL_EMPTY  = 0x00,
	GIP_BATT_LEVEL_LOW    = 0x01,
	GIP_BATT_LEVEL_MEDIUM = 0x02,
	GIP_BATT_LEVEL_FULL   = 0x03,
};

/* GIP_CMD_ANNOUNCE payload */
struct gip_pkt_announce {
	uint8_t  address[6];
	uint16_t unknown;
	uint16_t vendor_id;
	uint16_t product_id;
	struct { uint16_t major, minor, build, revision; } fw_version, hw_version;
} __attribute__((packed));

/* GIP_CMD_IDENTIFY response payload (offsets are into the whole payload) */
struct gip_pkt_identify {
	uint8_t  unknown[16];
	uint16_t client_commands_offset;
	uint16_t firmware_versions_offset;
	uint16_t audio_formats_offset;
	uint16_t capabilities_out_offset;
	uint16_t capabilities_in_offset;
	uint16_t classes_offset;
	uint16_t interfaces_offset;
	uint16_t hid_descriptor_offset;
} __attribute__((packed));

struct gip_pkt_acknowledge {
	uint8_t  unknown;
	uint8_t  command;
	uint8_t  options;
	uint16_t length;
	uint8_t  padding[2];
	uint16_t remaining;
} __attribute__((packed));

/* GIP_CMD_AUDIO_SAMPLES payload. The first field is metadata, not PCM. */
struct gip_pkt_audio_samples {
	uint16_t length_out;
	uint8_t  samples[];
} __attribute__((packed));

int  gip_encode_header(const struct gip_header *hdr, uint8_t *buf);
int  gip_decode_header(struct gip_header *hdr, const uint8_t *data, int len);
const char *gip_command_name(uint8_t cmd);
const char *gip_audio_format_name(uint8_t fmt);
