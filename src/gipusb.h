/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Minimal userspace USB layer over IOKit/IOUSBLib (no kext, no DriverKit). */
#pragma once

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/usb/IOUSBLib.h>
#include <stdint.h>

#define GIP_INTF_DATA  0   /* interrupt in/out, GIP control + input */
#define GIP_INTF_AUDIO 1   /* isochronous in/out, alt setting 1 */

#define GIPUSB_RX_LEN  64

typedef void (*gipusb_rx_cb)(void *ctx, const uint8_t *data, uint32_t len);

typedef struct {
	IOUSBDeviceInterface500    **dev;
	IOUSBInterfaceInterface500 **intf;
	CFRunLoopSourceRef  source;
	uint8_t  pipe_in,  pipe_out;    /* IOKit pipe refs, not endpoint addresses */
	uint8_t  ep_in,    ep_out;      /* endpoint addresses, for logging */
	uint16_t max_in,   max_out;
	uint8_t  rxbuf[GIPUSB_RX_LEN];
	gipusb_rx_cb cb;
	void    *ctx;
} gipusb;

/* force a USB re-enumeration so the device announces itself again */
int  gipusb_reenumerate(uint16_t vid, uint16_t pid);

/* pid == 0 matches any product for the given vendor */
int  gipusb_open(gipusb *u, uint16_t vid, uint16_t pid, uint8_t ifnum);
void gipusb_close(gipusb *u);

/*
 * Interrupt pipes do not support the ...TO timeout variants (they return
 * kIOReturnBadArgument), so reads go through the async API and a run loop.
 */
int  gipusb_start_reader(gipusb *u, gipusb_rx_cb cb, void *ctx);
int  gipusb_write(gipusb *u, const void *buf, uint32_t len);
