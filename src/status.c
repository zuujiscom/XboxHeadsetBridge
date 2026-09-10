/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * gip-status - inspect headset volume, mute state, and stream stats
 * from the shared ring buffer populated by gip-bridge and the HAL plug-in.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdbool.h>
#include <signal.h>
#include <string.h>

#include "../shared/ring.h"

static volatile sig_atomic_t stop;
static void on_sigint(int sig) { (void)sig; stop = 1; }


int main(int argc, char **argv)
{
	bool continuous = argc > 1 && strcmp(argv[1], "-c") == 0;
	ring_t *r;

	signal(SIGINT, on_sigint);

	r = ring_map(false);
	if (!r) {
		fprintf(stderr, "could not map shared ring buffer. Is gip-bridge running?\n");
		return 1;
	}

	if (r->magic != RING_MAGIC) {
		fprintf(stderr, "ring buffer magic mismatch (not initialized by gip-bridge)\n");
		return 1;
	}

	do {
		uint32_t online  = atomic_load(&r->device_online);
		uint32_t muted   = atomic_load(&r->mic_muted);
		uint32_t vol_out = atomic_load(&r->vol_out);
		uint32_t vol_in  = atomic_load(&r->vol_in);
		uint32_t hvol    = atomic_load(&r->host_vol_out);
		uint32_t hmute   = atomic_load(&r->host_muted);
		uint32_t gain    = atomic_load(&r->vol_gain_out);
		uint32_t vseen   = atomic_load(&r->vol_seen);
		uint64_t w_out   = atomic_load(&r->out_write_frames);
		uint64_t r_out   = atomic_load(&r->out_read_frames);
		uint64_t w_in    = atomic_load(&r->in_write_frames);
		uint64_t r_in    = atomic_load(&r->in_read_frames);

		if (continuous)
			printf("\033[H\033[J");

		printf("=== Xbox Wireless Headset Status ===\n");
		/* device_online is set by whichever bridge last ran; a bridge killed
		 * outright cannot clear it, so treat a stalled playback counter as
		 * the real signal. */
		printf("  Device Online : %s\n", online ? "YES (Streaming)" : "NO (Offline)");
		printf("  Microphone    : %s\n", muted ? "MUTED" : "UNMUTED");
		printf("  Volume (macOS): %u%%%s\n", hvol, hmute ? "  [MUTED]" : "");
		/* Meaningless until the headset has actually sent a volume
		 * packet; ring_reset_status seeds them.
		 *
		 * Empirical mapping, each confirmed by sweeping one dial in
		 * isolation: gain_out (p[2]) is the main dial, out (p[3]) is the
		 * chat dial, and in (p[4]) is a constant on this device. */
		if (vseen) {
			printf("  Headset Dial  : %u%%\n", gain);
			printf("  Chat Dial     : %u%%\n", vol_out);
			printf("  Unused (p[4]) : %u\n", vol_in);
		} else {
			printf("  Headset Dials : not reported yet\n");
		}
		printf("  --- Stream Buffers ---\n");
		printf("  Playback Frames Written : %llu (Read: %llu)\n",
		       (unsigned long long)w_out, (unsigned long long)r_out);
		printf("  Capture Frames Written  : %llu (Read: %llu)\n",
		       (unsigned long long)w_in, (unsigned long long)r_in);

		if (!continuous)
			break;

		usleep(500000);
	} while (!stop);

	return 0;
}
