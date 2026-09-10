/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Plain-C view of the shared ring for the Swift menu bar app.
 *
 * shared/ring.h uses _Atomic and an ~8 MB flexible payload, neither of which
 * imports cleanly into Swift, so the app only ever sees this flat snapshot.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
	uint32_t online;
	uint32_t mic_muted;
	uint32_t vol_out;         /* the headset's own dial */
	uint32_t vol_in;
	uint32_t gain_out;        /* the headset's dial, live as it is turned */
	uint32_t vol_seen;        /* 0 until the headset has sent a volume packet */
	uint32_t host_vol_out;    /* macOS-side volume, driven by the volume keys */
	uint32_t host_muted;
	uint32_t battery_level;   /* enum gip_battery_level, 0-3 */
	uint32_t battery_type;    /* enum gip_battery_type */
	uint32_t battery_seen;    /* 0 until a STATUS packet has been parsed */
	uint64_t out_write_frames;
	uint64_t out_read_frames;
	uint64_t in_write_frames;
	uint64_t in_read_frames;
} ring_status_t;

/* Attaches to the ring the bridge created. Returns false while no bridge has
 * ever run; call again later rather than treating it as fatal. */
bool ring_status_open(void);
void ring_status_close(void);

/* False if the ring is not mapped. */
bool ring_status_read(ring_status_t *out);
