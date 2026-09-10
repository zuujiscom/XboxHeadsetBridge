/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * gip-bridge - the bridge as a standalone daemon.
 *
 * The menu bar app links the same code directly (see bridge.h); this target
 * exists so the bridge can still be run and watched from a terminal, which is
 * how the protocol work gets diagnosed.
 */
#include <signal.h>
#include <stdio.h>
#include <string.h>

#include "bridge.h"

static void on_sigint(int sig) { (void)sig; bridge_stop(); }

static void usage(const char *argv0)
{
	fprintf(stderr,
		"usage: %s [-v|--verbose] [-h|--help]\n"
		"  -v, --verbose   print the live packet-counter line twice a second\n",
		argv0);
}

int main(int argc, char **argv)
{
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--verbose")) {
			bridge_set_verbose(true);
		} else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
			usage(argv[0]);
			return 0;
		} else {
			fprintf(stderr, "unknown option: %s\n", argv[i]);
			usage(argv[0]);
			return 1;
		}
	}

	/* SIGTERM as well as SIGINT: without a handler the default action kills us
	 * before the reconnect loop can clear device_online, leaving the ring
	 * claiming a headset is streaming when nothing is. */
	signal(SIGINT, on_sigint);
	signal(SIGTERM, on_sigint);

	/* Unbuffer stdout: when this runs under a pipe, default full buffering
	 * would withhold handshake progress for minutes. */
	setvbuf(stdout, NULL, _IONBF, 0);

	return bridge_run();
}
