/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Lock-free single-producer/single-consumer ring buffer shared between the
 * Core Audio HAL plug-in (inside sandboxed coreaudiod) and the USB bridge daemon.
 *
 * Provides bidirectional channels:
 *   - Playback (Output): HAL plug-in -> Bridge daemon -> USB ISO OUT (48 kHz stereo)
 *   - Capture  (Input) : USB ISO IN -> Bridge daemon -> HAL plug-in (48 kHz mono)
 */
#pragma once

#include <stdatomic.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <string.h>

#define RING_SHM_NAME       "/XboxHeadsetRing"
#define RING_MAGIC          0x58425332u   /* 'XBS2' */
#define RING_OUT_CHANNELS   2
#define RING_IN_CHANNELS    1
#define RING_FRAMES         32768u        /* power of two */

typedef struct {
	uint32_t magic;
	uint32_t out_channels;
	uint32_t in_channels;
	uint32_t capacity;              /* frames, power of two */

	/* Playback (HAL -> Daemon) */
	_Atomic uint64_t out_write_frames;
	_Atomic uint64_t out_read_frames;
	_Atomic uint32_t out_io_running;

	/* Capture (Daemon -> HAL) */
	_Atomic uint64_t in_write_frames;
	_Atomic uint64_t in_read_frames;
	_Atomic uint32_t in_io_running;

	/* Controls & Status (Bridge -> HAL / UI) */
	_Atomic uint32_t mic_muted;     /* 1 if muted, 0 if unmuted */
	_Atomic uint32_t vol_out;       /* 0-100 */
	_Atomic uint32_t vol_in;        /* 0-100 */
	_Atomic uint32_t battery_level; /* 0-3 */
	_Atomic uint32_t device_online; /* 1 if USB device connected & streaming */

	uint32_t _reserved[6];

	float out_data[RING_FRAMES * RING_OUT_CHANNELS];
	float in_data[RING_FRAMES * RING_IN_CHANNELS];
} ring_t;

static inline void ring_init(ring_t *r)
{
	memset(r, 0, sizeof(*r));
	r->magic = RING_MAGIC;
	r->out_channels = RING_OUT_CHANNELS;
	r->in_channels = RING_IN_CHANNELS;
	r->capacity = RING_FRAMES;
	r->vol_out = 100;
	r->vol_in = 100;
}

/* create == true for the bridge daemon (owner), false for the plug-in.
 *
 * Core Audio loads the plug-in inside coreaudiod, whose sandbox may not allow
 * it to open arbitrary files in /Users/Shared. POSIX shared memory is the
 * common namespace available to both processes, so there must be no file
 * fallback here: a fallback could leave the daemon and plug-in on different
 * rings. The daemon is the sole creator; the plug-in retries on its next I/O
 * start after the daemon has created the object. */
static inline ring_t *ring_map(bool create)
{
	int fd = -1;
	ring_t *r;
	bool fresh = false;
	struct stat st;

	if (create) {
		fd = shm_open(RING_SHM_NAME, O_CREAT | O_EXCL | O_RDWR, 0666);
		if (fd >= 0) {
			fresh = true;
			/* Do not let the caller's umask deny coreaudiod write access. */
			(void)fchmod(fd, 0666);
			if (ftruncate(fd, sizeof(ring_t)) != 0) {
				close(fd);
				return NULL;
			}
		} else if (errno == EEXIST) {
			fd = shm_open(RING_SHM_NAME, O_RDWR, 0);
		}
	} else {
		fd = shm_open(RING_SHM_NAME, O_RDWR, 0);
	}

	if (fd < 0)
		return NULL;

	/* Never resize an existing object: a live peer may have it mapped. */
	if (fstat(fd, &st) != 0) {
		close(fd);
		return NULL;
	}
	if ((size_t)st.st_size < sizeof(ring_t)) {
		if (!create || ftruncate(fd, sizeof(ring_t)) != 0) {
			close(fd);
			return NULL;
		}
		fresh = true;
	}

	r = mmap(NULL, sizeof(ring_t), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	close(fd);
	if (r == MAP_FAILED)
		return NULL;

	/* A bridge restart must preserve the counters and data of a valid ring. */
	if (fresh || r->magic != RING_MAGIC)
		ring_init(r);

	return r;
}

/* HAL writes mixed playback audio */
static inline void ring_out_write(ring_t *r, const float *src, uint32_t frames)
{
	uint64_t w = atomic_load_explicit(&r->out_write_frames, memory_order_relaxed);

	for (uint32_t i = 0; i < frames; i++) {
		uint32_t slot = (uint32_t)((w + i) & (RING_FRAMES - 1));

		r->out_data[slot * RING_OUT_CHANNELS]     = src[i * RING_OUT_CHANNELS];
		r->out_data[slot * RING_OUT_CHANNELS + 1] = src[i * RING_OUT_CHANNELS + 1];
	}

	atomic_store_explicit(&r->out_write_frames, w + frames, memory_order_release);
}

/* Daemon reads playback audio to send over USB OUT */
static inline uint32_t ring_out_read(ring_t *r, float *dst, uint32_t frames)
{
	uint64_t w = atomic_load_explicit(&r->out_write_frames, memory_order_acquire);
	uint64_t rd = atomic_load_explicit(&r->out_read_frames, memory_order_relaxed);
	uint64_t avail = w > rd ? w - rd : 0;
	uint32_t n = avail > frames ? frames : (uint32_t)avail;

	/* if the writer ran far ahead, skip forward rather than play stale audio */
	if (avail > RING_FRAMES) {
		rd = w - frames;
		n = frames;
	}

	for (uint32_t i = 0; i < n; i++) {
		uint32_t slot = (uint32_t)((rd + i) & (RING_FRAMES - 1));

		dst[i * RING_OUT_CHANNELS]     = r->out_data[slot * RING_OUT_CHANNELS];
		dst[i * RING_OUT_CHANNELS + 1] = r->out_data[slot * RING_OUT_CHANNELS + 1];
	}
	for (uint32_t i = n; i < frames; i++) {
		dst[i * RING_OUT_CHANNELS] = 0.0f;
		dst[i * RING_OUT_CHANNELS + 1] = 0.0f;
	}

	atomic_store_explicit(&r->out_read_frames, rd + n, memory_order_release);
	return n;
}

/* Daemon writes captured mic audio (48 kHz mono float) */
static inline void ring_in_write(ring_t *r, const float *src, uint32_t frames)
{
	uint64_t w = atomic_load_explicit(&r->in_write_frames, memory_order_relaxed);

	for (uint32_t i = 0; i < frames; i++) {
		uint32_t slot = (uint32_t)((w + i) & (RING_FRAMES - 1));
		r->in_data[slot] = src[i];
	}

	atomic_store_explicit(&r->in_write_frames, w + frames, memory_order_release);
}

/* HAL reads captured mic audio */
static inline uint32_t ring_in_read(ring_t *r, float *dst, uint32_t frames)
{
	uint64_t w = atomic_load_explicit(&r->in_write_frames, memory_order_acquire);
	uint64_t rd = atomic_load_explicit(&r->in_read_frames, memory_order_relaxed);
	uint64_t avail = w > rd ? w - rd : 0;
	uint32_t n = avail > frames ? frames : (uint32_t)avail;

	if (avail > RING_FRAMES) {
		rd = w - frames;
		n = frames;
	}

	for (uint32_t i = 0; i < n; i++) {
		uint32_t slot = (uint32_t)((rd + i) & (RING_FRAMES - 1));
		dst[i] = r->in_data[slot];
	}
	for (uint32_t i = n; i < frames; i++) {
		dst[i] = 0.0f;
	}

	atomic_store_explicit(&r->in_read_frames, rd + n, memory_order_release);
	return n;
}
