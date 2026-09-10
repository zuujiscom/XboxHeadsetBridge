/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ringshim.h"

#include "../shared/ring.h"

static ring_t *g_ring;

bool ring_status_open(void)
{
	if (!g_ring)
		g_ring = ring_map(false);

	return g_ring != NULL;
}

void ring_status_close(void)
{
	g_ring = NULL;
}

bool ring_status_read(ring_status_t *out)
{
	ring_t *r = g_ring;

	if (!r || !out)
		return false;

	out->online            = atomic_load(&r->device_online);
	out->mic_muted         = atomic_load(&r->mic_muted);
	out->vol_out           = atomic_load(&r->vol_out);
	out->vol_in            = atomic_load(&r->vol_in);
	out->host_vol_out      = atomic_load(&r->host_vol_out);
	out->host_muted        = atomic_load(&r->host_muted);
	out->battery_level     = atomic_load(&r->battery_level);
	out->battery_type      = atomic_load(&r->battery_type);
	out->battery_seen      = atomic_load(&r->battery_seen);
	out->out_write_frames  = atomic_load(&r->out_write_frames);
	out->out_read_frames   = atomic_load(&r->out_read_frames);
	out->in_write_frames   = atomic_load(&r->in_write_frames);
	out->in_read_frames    = atomic_load(&r->in_read_frames);
	return true;
}
