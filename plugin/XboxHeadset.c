/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * XboxHeadset - a Core Audio server plug-in publishing a virtual output
 * and input device for the Xbox GIP headset.
 */

#include <CoreAudio/AudioServerPlugIn.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "../shared/ring.h"

#define kDeviceUID          "XboxHeadsetBridge:Device"
#define kDeviceModelUID     "XboxHeadsetBridge:Model"
#define kDeviceName         "Xbox Wireless Headset"
#define kManufacturerName   "XboxHeadsetBridge"
#define kBoxUID             "XboxHeadsetBridge:Box"

#define kSampleRate         48000.0
#define kOutputChannels     2
#define kInputChannels      1
#define kZeroTimeStampPeriod 2048

enum {
	kObjectID_PlugIn        = kAudioObjectPlugInObject,
	kObjectID_Device        = 2,
	kObjectID_Stream_Output = 3,
	kObjectID_Stream_Input  = 4,
};

static pthread_mutex_t gMutex = PTHREAD_MUTEX_INITIALIZER;
static UInt32   gRefCount;
static AudioServerPlugInHostRef gHost;
static UInt32   gIOCount;
static UInt64   gAnchorHostTime;
static UInt64   gTimeStampCount;
static Float64  gHostTicksPerFrame;
static ring_t  *gRing;

static void EnsureRing(void)
{
	if (!gRing)
		gRing = ring_map(false);
}

static void FillOutputFormat(AudioStreamBasicDescription *f)
{
	f->mSampleRate       = kSampleRate;
	f->mFormatID         = kAudioFormatLinearPCM;
	f->mFormatFlags      = kAudioFormatFlagIsFloat |
			       kAudioFormatFlagsNativeEndian |
			       kAudioFormatFlagIsPacked;
	f->mBytesPerPacket   = kOutputChannels * sizeof(Float32);
	f->mFramesPerPacket  = 1;
	f->mBytesPerFrame    = kOutputChannels * sizeof(Float32);
	f->mChannelsPerFrame = kOutputChannels;
	f->mBitsPerChannel   = 32;
	f->mReserved         = 0;
}

static void FillInputFormat(AudioStreamBasicDescription *f)
{
	f->mSampleRate       = kSampleRate;
	f->mFormatID         = kAudioFormatLinearPCM;
	f->mFormatFlags      = kAudioFormatFlagIsFloat |
			       kAudioFormatFlagsNativeEndian |
			       kAudioFormatFlagIsPacked;
	f->mBytesPerPacket   = kInputChannels * sizeof(Float32);
	f->mFramesPerPacket  = 1;
	f->mBytesPerFrame    = kInputChannels * sizeof(Float32);
	f->mChannelsPerFrame = kInputChannels;
	f->mBitsPerChannel   = 32;
	f->mReserved         = 0;
}

#pragma mark - property helpers

static Boolean HasProperty(AudioServerPlugInDriverRef d, AudioObjectID obj,
			   pid_t client, const AudioObjectPropertyAddress *addr);
static OSStatus IsPropertySettable(AudioServerPlugInDriverRef d, AudioObjectID obj,
				   pid_t client, const AudioObjectPropertyAddress *addr,
				   Boolean *out);
static OSStatus GetPropertyDataSize(AudioServerPlugInDriverRef d, AudioObjectID obj,
				    pid_t client, const AudioObjectPropertyAddress *addr,
				    UInt32 qdlen, const void *qd, UInt32 *outSize);
static OSStatus GetPropertyData(AudioServerPlugInDriverRef d, AudioObjectID obj,
				pid_t client, const AudioObjectPropertyAddress *addr,
				UInt32 qdlen, const void *qd, UInt32 dataSize,
				UInt32 *outSize, void *outData);
static OSStatus SetPropertyData(AudioServerPlugInDriverRef d, AudioObjectID obj,
				pid_t client, const AudioObjectPropertyAddress *addr,
				UInt32 qdlen, const void *qd, UInt32 dataSize,
				const void *data);

#pragma mark - lifecycle

static HRESULT QueryInterface(void *self, REFIID iid, LPVOID *outIface)
{
	CFUUIDRef uuid = CFUUIDCreateFromUUIDBytes(NULL, iid);

	if (!uuid)
		return kAudioHardwareIllegalOperationError;

	if (CFEqual(uuid, IUnknownUUID) ||
	    CFEqual(uuid, kAudioServerPlugInDriverInterfaceUUID)) {
		pthread_mutex_lock(&gMutex);
		gRefCount++;
		pthread_mutex_unlock(&gMutex);
		*outIface = self;
		CFRelease(uuid);
		return 0;
	}

	CFRelease(uuid);
	return E_NOINTERFACE;
}

static ULONG AddRef(void *self)
{
	ULONG n;

	pthread_mutex_lock(&gMutex);
	n = ++gRefCount;
	pthread_mutex_unlock(&gMutex);
	return n;
}

static ULONG Release(void *self)
{
	ULONG n;

	pthread_mutex_lock(&gMutex);
	n = gRefCount ? --gRefCount : 0;
	pthread_mutex_unlock(&gMutex);
	return n;
}

static OSStatus Initialize(AudioServerPlugInDriverRef d,
			   AudioServerPlugInHostRef host)
{
	struct mach_timebase_info tb;

	gHost = host;
	mach_timebase_info(&tb);
	gHostTicksPerFrame = ((Float64)tb.denom / (Float64)tb.numer) *
			     1000000000.0 / kSampleRate;
	EnsureRing();
	return 0;
}

static OSStatus CreateDevice(AudioServerPlugInDriverRef d, CFDictionaryRef desc,
			     const AudioServerPlugInClientInfo *info,
			     AudioObjectID *outID)
{
	return kAudioHardwareUnsupportedOperationError;
}

static OSStatus DestroyDevice(AudioServerPlugInDriverRef d, AudioObjectID id)
{
	return kAudioHardwareUnsupportedOperationError;
}

static OSStatus AddDeviceClient(AudioServerPlugInDriverRef d, AudioObjectID id,
				const AudioServerPlugInClientInfo *info)
{
	return 0;
}

static OSStatus RemoveDeviceClient(AudioServerPlugInDriverRef d, AudioObjectID id,
				   const AudioServerPlugInClientInfo *info)
{
	return 0;
}

static OSStatus PerformDeviceConfigurationChange(AudioServerPlugInDriverRef d,
						 AudioObjectID id, UInt64 action,
						 void *info)
{
	return 0;
}

static OSStatus AbortDeviceConfigurationChange(AudioServerPlugInDriverRef d,
					       AudioObjectID id, UInt64 action,
					       void *info)
{
	return 0;
}

#pragma mark - IO

static OSStatus StartIO(AudioServerPlugInDriverRef d, AudioObjectID id,
			UInt32 client)
{
	if (id != kObjectID_Device)
		return kAudioHardwareBadObjectError;

	pthread_mutex_lock(&gMutex);
	if (gIOCount == 0) {
		gAnchorHostTime = mach_absolute_time();
		gTimeStampCount = 0;
		EnsureRing();
		if (gRing) {
			atomic_store(&gRing->out_read_frames,
				     atomic_load(&gRing->out_write_frames));
			atomic_store(&gRing->in_read_frames,
				     atomic_load(&gRing->in_write_frames));
			atomic_store(&gRing->out_io_running, 1);
			atomic_store(&gRing->in_io_running, 1);
		}
	}
	gIOCount++;
	pthread_mutex_unlock(&gMutex);

	return 0;
}

static OSStatus StopIO(AudioServerPlugInDriverRef d, AudioObjectID id,
		       UInt32 client)
{
	if (id != kObjectID_Device)
		return kAudioHardwareBadObjectError;

	pthread_mutex_lock(&gMutex);
	if (gIOCount)
		gIOCount--;
	if (gIOCount == 0 && gRing) {
		atomic_store(&gRing->out_io_running, 0);
		atomic_store(&gRing->in_io_running, 0);
	}
	pthread_mutex_unlock(&gMutex);

	return 0;
}

static OSStatus GetZeroTimeStamp(AudioServerPlugInDriverRef d, AudioObjectID id,
				 UInt32 client, Float64 *outSampleTime,
				 UInt64 *outHostTime, UInt64 *outSeed)
{
	UInt64 now, next;

	if (id != kObjectID_Device)
		return kAudioHardwareBadObjectError;

	pthread_mutex_lock(&gMutex);

	now = mach_absolute_time();
	next = gAnchorHostTime +
	       (UInt64)((Float64)((gTimeStampCount + 1) * kZeroTimeStampPeriod) *
			gHostTicksPerFrame);
	if (now >= next)
		gTimeStampCount++;

	*outSampleTime = (Float64)(gTimeStampCount * kZeroTimeStampPeriod);
	*outHostTime = gAnchorHostTime +
		       (UInt64)(*outSampleTime * gHostTicksPerFrame);
	*outSeed = 1;

	pthread_mutex_unlock(&gMutex);
	return 0;
}

static OSStatus WillDoIOOperation(AudioServerPlugInDriverRef d, AudioObjectID id,
				  UInt32 client, UInt32 op, Boolean *outWillDo,
				  Boolean *outWillDoInPlace)
{
	Boolean will = (op == kAudioServerPlugInIOOperationWriteMix ||
			op == kAudioServerPlugInIOOperationReadInput);

	if (id != kObjectID_Device)
		return kAudioHardwareBadObjectError;

	if (outWillDo)
		*outWillDo = will;
	if (outWillDoInPlace)
		*outWillDoInPlace = true;

	return 0;
}

static OSStatus BeginIOOperation(AudioServerPlugInDriverRef d, AudioObjectID id,
				 UInt32 client, UInt32 op, UInt32 frames,
				 const AudioServerPlugInIOCycleInfo *cycle)
{
	return 0;
}

static OSStatus DoIOOperation(AudioServerPlugInDriverRef d, AudioObjectID id,
			      AudioObjectID stream, UInt32 client, UInt32 op,
			      UInt32 frames, const AudioServerPlugInIOCycleInfo *cycle,
			      void *mainBuffer, void *secondaryBuffer)
{
	if (id != kObjectID_Device)
		return kAudioHardwareBadObjectError;

	if (!gRing)
		return 0;

	if (op == kAudioServerPlugInIOOperationWriteMix && mainBuffer) {
		ring_out_write(gRing, (const float *)mainBuffer, frames);
	} else if (op == kAudioServerPlugInIOOperationReadInput && mainBuffer) {
		ring_in_read(gRing, (float *)mainBuffer, frames);
	}

	return 0;
}

static OSStatus EndIOOperation(AudioServerPlugInDriverRef d, AudioObjectID id,
			       UInt32 client, UInt32 op, UInt32 frames,
			       const AudioServerPlugInIOCycleInfo *cycle)
{
	return 0;
}

#pragma mark - driver interface

static AudioServerPlugInDriverInterface gInterface = {
	NULL,
	QueryInterface,
	AddRef,
	Release,
	Initialize,
	CreateDevice,
	DestroyDevice,
	AddDeviceClient,
	RemoveDeviceClient,
	PerformDeviceConfigurationChange,
	AbortDeviceConfigurationChange,
	HasProperty,
	IsPropertySettable,
	GetPropertyDataSize,
	GetPropertyData,
	SetPropertyData,
	StartIO,
	StopIO,
	GetZeroTimeStamp,
	WillDoIOOperation,
	BeginIOOperation,
	DoIOOperation,
	EndIOOperation,
};

static AudioServerPlugInDriverInterface *gInterfacePtr = &gInterface;
static AudioServerPlugInDriverRef gDriverRef = &gInterfacePtr;

void *XboxHeadsetCreate(CFAllocatorRef allocator, CFUUIDRef requestedTypeUUID);

void *XboxHeadsetCreate(CFAllocatorRef allocator, CFUUIDRef requestedTypeUUID)
{
	if (!CFEqual(requestedTypeUUID, kAudioServerPlugInTypeUUID))
		return NULL;

	return gDriverRef;
}

#pragma mark - properties

static Boolean HasProperty(AudioServerPlugInDriverRef d, AudioObjectID obj,
			   pid_t client, const AudioObjectPropertyAddress *addr)
{
	UInt32 size = 0;

	return GetPropertyDataSize(d, obj, client, addr, 0, NULL, &size) == 0;
}

static OSStatus IsPropertySettable(AudioServerPlugInDriverRef d, AudioObjectID obj,
				   pid_t client, const AudioObjectPropertyAddress *addr,
				   Boolean *out)
{
	UInt32 size = 0;
	OSStatus err = GetPropertyDataSize(d, obj, client, addr, 0, NULL, &size);

	if (err)
		return err;

	*out = false;
	return 0;
}

static OSStatus GetPropertyDataSize(AudioServerPlugInDriverRef d, AudioObjectID obj,
				    pid_t client, const AudioObjectPropertyAddress *addr,
				    UInt32 qdlen, const void *qd, UInt32 *outSize)
{
	return GetPropertyData(d, obj, client, addr, qdlen, qd, 0, outSize, NULL);
}

#define RETURN_VALUE(n, expr)                                    \
	do {                                                     \
		*outSize = (n);                                  \
		if (outData && dataSize >= (n)) { expr; }        \
		return 0;                                        \
	} while (0)

static OSStatus GetPlugInProperty(const AudioObjectPropertyAddress *addr,
				  UInt32 qdlen, const void *qd, UInt32 dataSize,
				  UInt32 *outSize, void *outData)
{
	switch (addr->mSelector) {
	case kAudioObjectPropertyBaseClass:
		RETURN_VALUE(sizeof(AudioClassID),
			     *(AudioClassID *)outData = kAudioObjectClassID);
	case kAudioObjectPropertyClass:
		RETURN_VALUE(sizeof(AudioClassID),
			     *(AudioClassID *)outData = kAudioPlugInClassID);
	case kAudioObjectPropertyOwner:
		RETURN_VALUE(sizeof(AudioObjectID),
			     *(AudioObjectID *)outData = kAudioObjectUnknown);
	case kAudioObjectPropertyManufacturer:
		RETURN_VALUE(sizeof(CFStringRef),
			     *(CFStringRef *)outData =
				     CFSTR(kManufacturerName));
	case kAudioObjectPropertyOwnedObjects:
	case kAudioPlugInPropertyDeviceList:
		*outSize = sizeof(AudioObjectID);
		if (outData && dataSize >= sizeof(AudioObjectID))
			*(AudioObjectID *)outData = kObjectID_Device;
		return 0;
	case kAudioPlugInPropertyTranslateUIDToDevice: {
		CFStringRef uid = qd ? *(CFStringRef *)qd : NULL;

		RETURN_VALUE(sizeof(AudioObjectID),
			     *(AudioObjectID *)outData =
				     (uid && CFStringCompare(uid, CFSTR(kDeviceUID), 0) ==
				      kCFCompareEqualTo) ?
				     kObjectID_Device : kAudioObjectUnknown);
	}
	case kAudioPlugInPropertyResourceBundle:
		RETURN_VALUE(sizeof(CFStringRef),
			     *(CFStringRef *)outData = CFSTR(""));
	default:
		return kAudioHardwareUnknownPropertyError;
	}
}

static OSStatus GetDeviceProperty(const AudioObjectPropertyAddress *addr,
				  UInt32 dataSize, UInt32 *outSize, void *outData)
{
	switch (addr->mSelector) {
	case kAudioObjectPropertyBaseClass:
		RETURN_VALUE(sizeof(AudioClassID),
			     *(AudioClassID *)outData = kAudioObjectClassID);
	case kAudioObjectPropertyClass:
		RETURN_VALUE(sizeof(AudioClassID),
			     *(AudioClassID *)outData = kAudioDeviceClassID);
	case kAudioObjectPropertyOwner:
		RETURN_VALUE(sizeof(AudioObjectID),
			     *(AudioObjectID *)outData = kObjectID_PlugIn);
	case kAudioObjectPropertyName:
		RETURN_VALUE(sizeof(CFStringRef),
			     *(CFStringRef *)outData = CFSTR(kDeviceName));
	case kAudioObjectPropertyManufacturer:
		RETURN_VALUE(sizeof(CFStringRef),
			     *(CFStringRef *)outData = CFSTR(kManufacturerName));
	case kAudioDevicePropertyDeviceUID:
		RETURN_VALUE(sizeof(CFStringRef),
			     *(CFStringRef *)outData = CFSTR(kDeviceUID));
	case kAudioDevicePropertyModelUID:
		RETURN_VALUE(sizeof(CFStringRef),
			     *(CFStringRef *)outData = CFSTR(kDeviceModelUID));
	case kAudioDevicePropertyTransportType:
		RETURN_VALUE(sizeof(UInt32),
			     *(UInt32 *)outData = kAudioDeviceTransportTypeVirtual);
	case kAudioDevicePropertyClockDomain:
		RETURN_VALUE(sizeof(UInt32), *(UInt32 *)outData = 0);
	case kAudioDevicePropertyDeviceIsAlive:
		RETURN_VALUE(sizeof(UInt32), *(UInt32 *)outData = 1);
	case kAudioDevicePropertyDeviceIsRunning:
		RETURN_VALUE(sizeof(UInt32), *(UInt32 *)outData = gIOCount > 0);
	case kAudioDevicePropertyDeviceCanBeDefaultDevice:
	case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
		RETURN_VALUE(sizeof(UInt32), *(UInt32 *)outData = 1);
	case kAudioDevicePropertyLatency:
	case kAudioDevicePropertySafetyOffset:
		RETURN_VALUE(sizeof(UInt32), *(UInt32 *)outData = 0);
	case kAudioDevicePropertyNominalSampleRate:
		RETURN_VALUE(sizeof(Float64), *(Float64 *)outData = kSampleRate);
	case kAudioDevicePropertyAvailableNominalSampleRates:
		*outSize = sizeof(AudioValueRange);
		if (outData && dataSize >= sizeof(AudioValueRange)) {
			AudioValueRange *r = outData;

			r->mMinimum = kSampleRate;
			r->mMaximum = kSampleRate;
		}
		return 0;
	case kAudioDevicePropertyIsHidden:
		RETURN_VALUE(sizeof(UInt32), *(UInt32 *)outData = 0);
	case kAudioDevicePropertyZeroTimeStampPeriod:
		RETURN_VALUE(sizeof(UInt32),
			     *(UInt32 *)outData = kZeroTimeStampPeriod);
	case kAudioDevicePropertyPreferredChannelsForStereo:
		*outSize = 2 * sizeof(UInt32);
		if (outData && dataSize >= 2 * sizeof(UInt32)) {
			((UInt32 *)outData)[0] = 1;
			((UInt32 *)outData)[1] = 2;
		}
		return 0;
	case kAudioDevicePropertyPreferredChannelLayout: {
		UInt32 n = offsetof(AudioChannelLayout, mChannelDescriptions) +
			   kOutputChannels * sizeof(AudioChannelDescription);

		*outSize = n;
		if (outData && dataSize >= n) {
			AudioChannelLayout *l = outData;

			memset(l, 0, n);
			l->mChannelLayoutTag =
				kAudioChannelLayoutTag_UseChannelDescriptions;
			l->mNumberChannelDescriptions = kOutputChannels;
			l->mChannelDescriptions[0].mChannelLabel =
				kAudioChannelLabel_Left;
			l->mChannelDescriptions[1].mChannelLabel =
				kAudioChannelLabel_Right;
		}
		return 0;
	}
	case kAudioObjectPropertyControlList:
		*outSize = 0;
		return 0;
	case kAudioObjectPropertyOwnedObjects:
	case kAudioDevicePropertyStreams: {
		AudioObjectID streams[2];
		UInt32 count = 0;

		if (addr->mScope == kAudioObjectPropertyScopeOutput ||
		    addr->mScope == kAudioObjectPropertyScopeGlobal) {
			streams[count++] = kObjectID_Stream_Output;
		}
		if (addr->mScope == kAudioObjectPropertyScopeInput ||
		    addr->mScope == kAudioObjectPropertyScopeGlobal) {
			streams[count++] = kObjectID_Stream_Input;
		}

		*outSize = count * sizeof(AudioObjectID);
		if (outData && dataSize >= *outSize) {
			memcpy(outData, streams, *outSize);
		}
		return 0;
	}
	case kAudioDevicePropertyRelatedDevices:
		*outSize = sizeof(AudioObjectID);
		if (outData && dataSize >= sizeof(AudioObjectID))
			*(AudioObjectID *)outData = kObjectID_Device;
		return 0;
	default:
		return kAudioHardwareUnknownPropertyError;
	}
}

static OSStatus GetStreamProperty(AudioObjectID obj,
				  const AudioObjectPropertyAddress *addr,
				  UInt32 dataSize, UInt32 *outSize, void *outData)
{
	bool is_input = (obj == kObjectID_Stream_Input);

	switch (addr->mSelector) {
	case kAudioObjectPropertyBaseClass:
		RETURN_VALUE(sizeof(AudioClassID),
			     *(AudioClassID *)outData = kAudioObjectClassID);
	case kAudioObjectPropertyClass:
		RETURN_VALUE(sizeof(AudioClassID),
			     *(AudioClassID *)outData = kAudioStreamClassID);
	case kAudioObjectPropertyOwner:
		RETURN_VALUE(sizeof(AudioObjectID),
			     *(AudioObjectID *)outData = kObjectID_Device);
	case kAudioObjectPropertyName:
		RETURN_VALUE(sizeof(CFStringRef),
			     *(CFStringRef *)outData = CFSTR(kDeviceName));
	case kAudioStreamPropertyIsActive:
		RETURN_VALUE(sizeof(UInt32), *(UInt32 *)outData = 1);
	case kAudioStreamPropertyDirection:
		RETURN_VALUE(sizeof(UInt32), *(UInt32 *)outData = is_input ? 1 : 0);
	case kAudioStreamPropertyTerminalType:
		RETURN_VALUE(sizeof(UInt32),
			     *(UInt32 *)outData = is_input ?
			     kAudioStreamTerminalTypeMicrophone :
			     kAudioStreamTerminalTypeHeadphones);
	case kAudioStreamPropertyStartingChannel:
		RETURN_VALUE(sizeof(UInt32), *(UInt32 *)outData = 1);
	case kAudioStreamPropertyLatency:
		RETURN_VALUE(sizeof(UInt32), *(UInt32 *)outData = 0);
	case kAudioStreamPropertyVirtualFormat:
	case kAudioStreamPropertyPhysicalFormat:
		*outSize = sizeof(AudioStreamBasicDescription);
		if (outData && dataSize >= sizeof(AudioStreamBasicDescription)) {
			if (is_input)
				FillInputFormat(outData);
			else
				FillOutputFormat(outData);
		}
		return 0;
	case kAudioStreamPropertyAvailableVirtualFormats:
	case kAudioStreamPropertyAvailablePhysicalFormats:
		*outSize = sizeof(AudioStreamRangedDescription);
		if (outData && dataSize >= sizeof(AudioStreamRangedDescription)) {
			AudioStreamRangedDescription *r = outData;

			if (is_input)
				FillInputFormat(&r->mFormat);
			else
				FillOutputFormat(&r->mFormat);

			r->mSampleRateRange.mMinimum = kSampleRate;
			r->mSampleRateRange.mMaximum = kSampleRate;
		}
		return 0;
	default:
		return kAudioHardwareUnknownPropertyError;
	}
}

static OSStatus GetPropertyData(AudioServerPlugInDriverRef d, AudioObjectID obj,
				pid_t client, const AudioObjectPropertyAddress *addr,
				UInt32 qdlen, const void *qd, UInt32 dataSize,
				UInt32 *outSize, void *outData)
{
	if (!addr || !outSize)
		return kAudioHardwareIllegalOperationError;

	switch (obj) {
	case kObjectID_PlugIn:
		return GetPlugInProperty(addr, qdlen, qd, dataSize, outSize, outData);
	case kObjectID_Device:
		return GetDeviceProperty(addr, dataSize, outSize, outData);
	case kObjectID_Stream_Output:
	case kObjectID_Stream_Input:
		return GetStreamProperty(obj, addr, dataSize, outSize, outData);
	default:
		return kAudioHardwareBadObjectError;
	}
}

static OSStatus SetPropertyData(AudioServerPlugInDriverRef d, AudioObjectID obj,
				pid_t client, const AudioObjectPropertyAddress *addr,
				UInt32 qdlen, const void *qd, UInt32 dataSize,
				const void *data)
{
	return kAudioHardwareUnsupportedOperationError;
}
