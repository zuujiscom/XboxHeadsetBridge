/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The bridge as a library, so it can run either as the gip-bridge CLI or
 * directly inside the menu bar app.
 *
 * There is exactly one bridge per process: the implementation keeps its state
 * in file-scope globals and its IOKit event sources are attached to
 * CFRunLoopGetCurrent(). Everything below must therefore be called from a
 * single dedicated thread, and that same thread must own the run loop --
 * `bridge_run` polls it internally, so the thread needs no run loop of its own.
 */
#pragma once

#include <stdbool.h>

/* Prints the twice-a-second packet counter line. Terminal-only noise
 * otherwise; call before bridge_run. */
void bridge_set_verbose(bool on);

/* Maps the shared ring, then runs the connect/stream/reconnect loop until
 * bridge_stop is called or the session ends unrecoverably. Blocks. Returns 0
 * on a clean stop, non-zero if the ring could not be mapped. */
int bridge_run(void);

/* Asks bridge_run to return. Safe to call from another thread or from a signal
 * handler; the run loop is polled, so this takes effect within ~0.5 s. */
void bridge_stop(void);

/* True while bridge_run is executing. */
bool bridge_is_running(void);
