/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "gipusb.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <stdio.h>
#include <string.h>

static IOUSBDeviceInterface500 **open_device(uint16_t vid, uint16_t pid)
{
	CFMutableDictionaryRef match = IOServiceMatching(kIOUSBDeviceClassName);
	io_iterator_t iter;
	io_service_t svc;
	IOUSBDeviceInterface500 **found = NULL;

	if (!match)
		return NULL;
	if (IOServiceGetMatchingServices(kIOMainPortDefault, match, &iter) != KERN_SUCCESS)
		return NULL;

	while ((svc = IOIteratorNext(iter))) {
		IOCFPlugInInterface **plug = NULL;
		IOUSBDeviceInterface500 **dev = NULL;
		SInt32 score;
		UInt16 v = 0, p = 0;

		if (IOCreatePlugInInterfaceForService(svc, kIOUSBDeviceUserClientTypeID,
						      kIOCFPlugInInterfaceID, &plug,
						      &score) != KERN_SUCCESS || !plug) {
			IOObjectRelease(svc);
			continue;
		}

		(*plug)->QueryInterface(plug,
					CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID500),
					(LPVOID *)&dev);
		(*plug)->Release(plug);
		IOObjectRelease(svc);

		if (!dev)
			continue;

		(*dev)->GetDeviceVendor(dev, &v);
		(*dev)->GetDeviceProduct(dev, &p);

		if (v == vid && (pid == 0 || p == pid)) {
			found = dev;
			break;
		}

		(*dev)->Release(dev);
	}

	IOObjectRelease(iter);
	return found;
}

static IOUSBInterfaceInterface500 **claim_interface(IOUSBDeviceInterface500 **dev,
						    uint8_t ifnum)
{
	IOUSBFindInterfaceRequest req = {
		.bInterfaceClass    = kIOUSBFindInterfaceDontCare,
		.bInterfaceSubClass = kIOUSBFindInterfaceDontCare,
		.bInterfaceProtocol = kIOUSBFindInterfaceDontCare,
		.bAlternateSetting  = kIOUSBFindInterfaceDontCare,
	};
	io_iterator_t iter;
	io_service_t svc;
	IOUSBInterfaceInterface500 **found = NULL;

	if ((*dev)->CreateInterfaceIterator(dev, &req, &iter) != kIOReturnSuccess)
		return NULL;

	while ((svc = IOIteratorNext(iter))) {
		IOCFPlugInInterface **plug = NULL;
		IOUSBInterfaceInterface500 **intf = NULL;
		SInt32 score;
		UInt8 n = 0xff;

		if (IOCreatePlugInInterfaceForService(svc, kIOUSBInterfaceUserClientTypeID,
						      kIOCFPlugInInterfaceID, &plug,
						      &score) != KERN_SUCCESS || !plug) {
			IOObjectRelease(svc);
			continue;
		}

		(*plug)->QueryInterface(plug,
					CFUUIDGetUUIDBytes(kIOUSBInterfaceInterfaceID500),
					(LPVOID *)&intf);
		(*plug)->Release(plug);
		IOObjectRelease(svc);

		if (!intf)
			continue;

		(*intf)->GetInterfaceNumber(intf, &n);
		if (n == ifnum) {
			found = intf;
			break;
		}

		(*intf)->Release(intf);
	}

	IOObjectRelease(iter);
	return found;
}

int gipusb_reenumerate(uint16_t vid, uint16_t pid)
{
	IOUSBDeviceInterface500 **dev = open_device(vid, pid);
	IOReturn ret;

	if (!dev)
		return -1;

	if ((*dev)->USBDeviceOpen(dev) != kIOReturnSuccess) {
		(*dev)->Release(dev);
		return -1;
	}

	/* forces the device to detach and re-announce itself on reattach */
	ret = (*dev)->USBDeviceReEnumerate(dev, 0);
	(*dev)->Release(dev);

	return ret == kIOReturnSuccess ? 0 : -1;
}

int gipusb_open(gipusb *u, uint16_t vid, uint16_t pid, uint8_t ifnum)
{
	IOReturn ret;
	UInt8 nconf = 0, cur = 0, neps = 0;

	memset(u, 0, sizeof(*u));

	u->dev = open_device(vid, pid);
	if (!u->dev) {
		fprintf(stderr, "device %04x:%04x not found\n", vid, pid);
		return -1;
	}

	ret = (*u->dev)->USBDeviceOpen(u->dev);
	if (ret != kIOReturnSuccess) {
		fprintf(stderr, "USBDeviceOpen failed: 0x%08x%s\n", ret,
			ret == kIOReturnExclusiveAccess ?
			" (another driver has the device)" : " (try sudo)");
		return -1;
	}

	/* nothing matched this vendor-class device, so no configuration is set */
	(*u->dev)->GetNumberOfConfigurations(u->dev, &nconf);
	(*u->dev)->GetConfiguration(u->dev, &cur);
	if (cur != 1) {
		ret = (*u->dev)->SetConfiguration(u->dev, 1);
		if (ret != kIOReturnSuccess) {
			fprintf(stderr, "SetConfiguration(1) failed: 0x%08x\n", ret);
			return -1;
		}
	}

	u->intf = claim_interface(u->dev, ifnum);
	if (!u->intf) {
		fprintf(stderr, "interface %u not found\n", ifnum);
		return -1;
	}

	ret = (*u->intf)->USBInterfaceOpen(u->intf);
	if (ret != kIOReturnSuccess) {
		fprintf(stderr, "USBInterfaceOpen failed: 0x%08x\n", ret);
		return -1;
	}

	(*u->intf)->GetNumEndpoints(u->intf, &neps);
	for (UInt8 pipe = 1; pipe <= neps; pipe++) {
		UInt8 dir, num, type, interval;
		UInt16 maxpkt;

		if ((*u->intf)->GetPipeProperties(u->intf, pipe, &dir, &num, &type,
						  &maxpkt, &interval) != kIOReturnSuccess)
			continue;
		if (type != kUSBInterrupt)
			continue;

		if (dir == kUSBIn) {
			u->pipe_in = pipe;
			u->ep_in = num;
			u->max_in = maxpkt;
		} else if (dir == kUSBOut) {
			u->pipe_out = pipe;
			u->ep_out = num;
			u->max_out = maxpkt;
		}
	}

	if (!u->pipe_in || !u->pipe_out) {
		fprintf(stderr, "interrupt pipes not found on interface %u\n", ifnum);
		return -1;
	}

	return 0;
}

void gipusb_close(gipusb *u)
{
	if (u->source) {
		CFRunLoopRemoveSource(CFRunLoopGetCurrent(), u->source,
				      kCFRunLoopDefaultMode);
		u->source = NULL;
	}
	if (u->intf) {
		(*u->intf)->USBInterfaceClose(u->intf);
		(*u->intf)->Release(u->intf);
		u->intf = NULL;
	}
	if (u->dev) {
		(*u->dev)->USBDeviceClose(u->dev);
		(*u->dev)->Release(u->dev);
		u->dev = NULL;
	}
}

static void rx_complete(void *refcon, IOReturn result, void *arg0)
{
	gipusb *u = refcon;
	uint32_t len = (uint32_t)(uintptr_t)arg0;

	if (result == kIOReturnAborted)
		return;

	if (result == kIOUSBPipeStalled) {
		(*u->intf)->ClearPipeStallBothEnds(u->intf, u->pipe_in);
	} else if (result != kIOReturnSuccess) {
		fprintf(stderr, "read failed: 0x%08x\n", result);
		return;
	} else if (len && u->cb) {
		u->cb(u->ctx, u->rxbuf, len);
	}

	/* resubmit */
	(*u->intf)->ReadPipeAsync(u->intf, u->pipe_in, u->rxbuf, sizeof(u->rxbuf),
				  rx_complete, u);
}

int gipusb_start_reader(gipusb *u, gipusb_rx_cb cb, void *ctx)
{
	IOReturn ret;

	u->cb = cb;
	u->ctx = ctx;

	ret = (*u->intf)->CreateInterfaceAsyncEventSource(u->intf, &u->source);
	if (ret != kIOReturnSuccess) {
		fprintf(stderr, "CreateInterfaceAsyncEventSource failed: 0x%08x\n", ret);
		return -1;
	}
	CFRunLoopAddSource(CFRunLoopGetCurrent(), u->source, kCFRunLoopDefaultMode);

	ret = (*u->intf)->ReadPipeAsync(u->intf, u->pipe_in, u->rxbuf,
					sizeof(u->rxbuf), rx_complete, u);
	if (ret != kIOReturnSuccess) {
		fprintf(stderr, "ReadPipeAsync failed: 0x%08x\n", ret);
		return -1;
	}

	return 0;
}

int gipusb_write(gipusb *u, const void *buf, uint32_t len)
{
	IOReturn ret = (*u->intf)->WritePipe(u->intf, u->pipe_out, (void *)buf, len);

	if (ret != kIOReturnSuccess) {
		fprintf(stderr, "WritePipe(pipe %u) failed: 0x%08x\n", u->pipe_out, ret);
		return -1;
	}
	return 0;
}
