/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * gip-status - inspect headset battery, volume, mute state, and stream stats
 * from the shared ring buffer populated by gip-bridge and the HAL plug-in.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdbool.h>
#include <signal.h>

#include "../shared/ring.h"

static volatile sig_atomic_t stop;
static void on_sigint(int sig) { (void)sig; stop = 1; }

static const char *battery_name(uint32_t lvl)
{
	switch (lvl & 0x03) {
	case 0:  return "Empty / Critical";
	case 1:  return "Low";
	case 2:  return "Medium";
	case 3:  return "Full";
	default: return "Unknown";
	}
}

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
		uint32_t batt    = atomic_load(&r->battery_level);
		uint64_t w_out   = atomic_load(&r->out_write_frames);
		uint64_t r_out   = atomic_load(&r->out_read_frames);
		uint64_t w_in    = atomic_load(&r->in_write_frames);
		uint64_t r_in    = atomic_load(&r->in_read_frames);

		if (continuous)
			printf("\033[H\033[J");

		printf("=== Xbox Wireless Headset Status ===\n");
		printf("  Device Online : %s\n", online ? "YES (Streaming)" : "NO (Offline)");
		printf("  Microphone    : %s\n", muted ? "MUTED" : "UNMUTED");
		printf("  Volume (Out)  : %u%%\n", vol_out);
		printf("  Volume (In)   : %u%%\n", vol_in);
		printf("  Battery Level : %s (%u)\n", battery_name(batt), batt);
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
