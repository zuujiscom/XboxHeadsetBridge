/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * XboxHeadset - a Core Audio server plug-in publishing a virtual output
 * and input device for the Xbox GIP headset.
 */

#include <CoreAudio/AudioServerPlugIn.h>
#include <mach/mach_time.h>
#include <math.h>
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
	/* Without these two control objects the device publishes no volume, so
	 * the keyboard volume keys and the menu bar slider are inert whenever the
	 * headset is the default output. */
	kObjectID_Volume_Output = 5,
	kObjectID_Mute_Output   = 6,
};

/* Volume taper. Scalar 0...1 maps linearly onto this dB range, so 50% lands at
 * -20 dB, which is roughly where built-in output sits at half volume. */
#define kVolumeMinDB        (-40.0f)
#define kVolumeMaxDB        (0.0f)

static pthread_mutex_t gMutex = PTHREAD_MUTEX_INITIALIZER;
static UInt32   gRefCount;
static AudioServerPlugInHostRef gHost;
static UInt32   gIOCount;
static UInt64   gAnchorHostTime;
static UInt64   gTimeStampCount;
static Float64  gHostTicksPerFrame;
static ring_t  *gRing;
/* Guarded by gMutex for writes; read unlocked on the IO thread, where a torn
 * read would at worst apply one stale block's gain. */
static Float32  gVolumeScalar = 1.0f;
static UInt32   gMuted;

static Float32 VolumeScalarToDB(Float32 scalar)
{
	if (scalar < 0.0f)
		scalar = 0.0f;
	if (scalar > 1.0f)
		scalar = 1.0f;

	return kVolumeMinDB + scalar * (kVolumeMaxDB - kVolumeMinDB);
}

static Float32 VolumeDBToScalar(Float32 db)
{
	Float32 scalar = (db - kVolumeMinDB) / (kVolumeMaxDB - kVolumeMinDB);

	if (scalar < 0.0f)
		scalar = 0.0f;
	if (scalar > 1.0f)
		scalar = 1.0f;

	return scalar;
}

/* Linear gain applied to playback. Scalar 0 is a true zero rather than
 * -40 dB, so dragging the slider to the bottom actually silences the headset. */
static Float32 VolumeGain(void)
{
	Float32 scalar = gVolumeScalar;

	if (gMuted || scalar <= 0.0f)
		return 0.0f;

	return powf(10.0f, VolumeScalarToDB(scalar) / 20.0f);
}

/* Mirror the control state into the shared ring so gip-status and the menu bar
 * app can display it. */
static void PublishVolumeToRing(void)
{
	if (!gRing)
		return;

	atomic_store(&gRing->host_vol_out,
		     (uint32_t)(gVolumeScalar * 100.0f + 0.5f));
	atomic_store(&gRing->host_muted, gMuted);
}

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
		/* Publish the current control state up front: otherwise host_vol_out
		 * reads 0 until the user happens to touch the volume. */
		PublishVolumeToRing();
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
		float *mix = mainBuffer;
		Float32 gain = VolumeGain();

		/* The device owns the mix buffer for WriteMix, so scaling in place is
		 * what a hardware driver's volume control would do. */
		if (gain != 1.0f) {
			for (UInt32 i = 0; i < frames * kOutputChannels; i++)
				mix[i] *= gain;
		}
		ring_out_write(gRing, mix, frames);
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

	/* Everything else is fixed; only the volume and mute controls are writable,
	 * and the HAL refuses to call SetPropertyData unless we say so here. */
	switch (addr->mSelector) {
	case kAudioLevelControlPropertyScalarValue:
	case kAudioLevelControlPropertyDecibelValue:
		*out = (obj == kObjectID_Volume_Output);
		break;
	case kAudioBooleanControlPropertyValue:
		*out = (obj == kObjectID_Mute_Output);
		break;
	default:
		*out = false;
		break;
	}
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
	case kAudioObjectPropertyControlList: {
		AudioObjectID controls[2] = {
			kObjectID_Volume_Output,
			kObjectID_Mute_Output,
		};

		*outSize = sizeof(controls);
		if (outData && dataSize >= *outSize)
			memcpy(outData, controls, *outSize);
		return 0;
	}
	case kAudioObjectPropertyOwnedObjects:
	case kAudioDevicePropertyStreams: {
		AudioObjectID objects[4];
		UInt32 count = 0;
		bool wants_output = addr->mScope == kAudioObjectPropertyScopeOutput ||
				    addr->mScope == kAudioObjectPropertyScopeGlobal;

		if (wants_output)
			objects[count++] = kObjectID_Stream_Output;
		if (addr->mScope == kAudioObjectPropertyScopeInput ||
		    addr->mScope == kAudioObjectPropertyScopeGlobal) {
			objects[count++] = kObjectID_Stream_Input;
		}
		/* Controls belong to the owned-object list but not to the stream
		 * list, or the HAL would treat them as streams. */
		if (addr->mSelector == kAudioObjectPropertyOwnedObjects && wants_output) {
			objects[count++] = kObjectID_Volume_Output;
			objects[count++] = kObjectID_Mute_Output;
		}

		*outSize = count * sizeof(AudioObjectID);
		if (outData && dataSize >= *outSize) {
			memcpy(outData, objects, *outSize);
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

static OSStatus GetControlProperty(AudioObjectID obj,
				   const AudioObjectPropertyAddress *addr,
				   UInt32 dataSize, UInt32 *outSize, void *outData)
{
	bool is_mute = (obj == kObjectID_Mute_Output);

	switch (addr->mSelector) {
	case kAudioObjectPropertyBaseClass:
		RETURN_VALUE(sizeof(AudioClassID),
			     *(AudioClassID *)outData = is_mute ?
				     kAudioBooleanControlClassID :
				     kAudioLevelControlClassID);
	case kAudioObjectPropertyClass:
		RETURN_VALUE(sizeof(AudioClassID),
			     *(AudioClassID *)outData = is_mute ?
				     kAudioMuteControlClassID :
				     kAudioVolumeControlClassID);
	case kAudioObjectPropertyOwner:
		RETURN_VALUE(sizeof(AudioObjectID),
			     *(AudioObjectID *)outData = kObjectID_Device);
	case kAudioObjectPropertyOwnedObjects:
		*outSize = 0;
		return 0;
	case kAudioControlPropertyScope:
		RETURN_VALUE(sizeof(AudioObjectPropertyScope),
			     *(AudioObjectPropertyScope *)outData =
				     kAudioObjectPropertyScopeOutput);
	case kAudioControlPropertyElement:
		RETURN_VALUE(sizeof(AudioObjectPropertyElement),
			     *(AudioObjectPropertyElement *)outData =
				     kAudioObjectPropertyElementMain);
	case kAudioBooleanControlPropertyValue:
		if (!is_mute)
			return kAudioHardwareUnknownPropertyError;
		RETURN_VALUE(sizeof(UInt32), *(UInt32 *)outData = gMuted);
	case kAudioLevelControlPropertyScalarValue:
		if (is_mute)
			return kAudioHardwareUnknownPropertyError;
		RETURN_VALUE(sizeof(Float32), *(Float32 *)outData = gVolumeScalar);
	case kAudioLevelControlPropertyDecibelValue:
		if (is_mute)
			return kAudioHardwareUnknownPropertyError;
		RETURN_VALUE(sizeof(Float32),
			     *(Float32 *)outData = VolumeScalarToDB(gVolumeScalar));
	case kAudioLevelControlPropertyDecibelRange:
		if (is_mute)
			return kAudioHardwareUnknownPropertyError;
		*outSize = sizeof(AudioValueRange);
		if (outData && dataSize >= sizeof(AudioValueRange)) {
			AudioValueRange *r = outData;

			r->mMinimum = kVolumeMinDB;
			r->mMaximum = kVolumeMaxDB;
		}
		return 0;
	case kAudioLevelControlPropertyConvertScalarToDecibels:
		if (is_mute)
			return kAudioHardwareUnknownPropertyError;
		*outSize = sizeof(Float32);
		if (outData && dataSize >= sizeof(Float32))
			*(Float32 *)outData = VolumeScalarToDB(*(Float32 *)outData);
		return 0;
	case kAudioLevelControlPropertyConvertDecibelsToScalar:
		if (is_mute)
			return kAudioHardwareUnknownPropertyError;
		*outSize = sizeof(Float32);
		if (outData && dataSize >= sizeof(Float32))
			*(Float32 *)outData = VolumeDBToScalar(*(Float32 *)outData);
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
	case kObjectID_Volume_Output:
	case kObjectID_Mute_Output:
		return GetControlProperty(obj, addr, dataSize, outSize, outData);
	default:
		return kAudioHardwareBadObjectError;
	}
}

static OSStatus SetPropertyData(AudioServerPlugInDriverRef d, AudioObjectID obj,
				pid_t client, const AudioObjectPropertyAddress *addr,
				UInt32 qdlen, const void *qd, UInt32 dataSize,
				const void *data)
{
	AudioObjectPropertyAddress changed[2];
	UInt32 changed_count = 0;

	if (!addr || !data)
		return kAudioHardwareIllegalOperationError;

	switch (addr->mSelector) {
	case kAudioLevelControlPropertyScalarValue:
	case kAudioLevelControlPropertyDecibelValue: {
		Float32 scalar;

		if (obj != kObjectID_Volume_Output)
			return kAudioHardwareBadObjectError;
		if (dataSize < sizeof(Float32))
			return kAudioHardwareBadPropertySizeError;

		scalar = addr->mSelector == kAudioLevelControlPropertyScalarValue ?
			 *(const Float32 *)data :
			 VolumeDBToScalar(*(const Float32 *)data);
		if (scalar < 0.0f)
			scalar = 0.0f;
		if (scalar > 1.0f)
			scalar = 1.0f;

		pthread_mutex_lock(&gMutex);
		gVolumeScalar = scalar;
		EnsureRing();
		PublishVolumeToRing();
		pthread_mutex_unlock(&gMutex);

		/* Report both representations: the menu bar slider watches the
		 * scalar, Audio MIDI Setup watches the decibel value. */
		changed[changed_count++] = (AudioObjectPropertyAddress){
			kAudioLevelControlPropertyScalarValue,
			kAudioObjectPropertyScopeGlobal,
			kAudioObjectPropertyElementMain
		};
		changed[changed_count++] = (AudioObjectPropertyAddress){
			kAudioLevelControlPropertyDecibelValue,
			kAudioObjectPropertyScopeGlobal,
			kAudioObjectPropertyElementMain
		};
		break;
	}
	case kAudioBooleanControlPropertyValue:
		if (obj != kObjectID_Mute_Output)
			return kAudioHardwareBadObjectError;
		if (dataSize < sizeof(UInt32))
			return kAudioHardwareBadPropertySizeError;

		pthread_mutex_lock(&gMutex);
		gMuted = *(const UInt32 *)data ? 1 : 0;
		EnsureRing();
		PublishVolumeToRing();
		pthread_mutex_unlock(&gMutex);

		changed[changed_count++] = (AudioObjectPropertyAddress){
			kAudioBooleanControlPropertyValue,
			kAudioObjectPropertyScopeGlobal,
			kAudioObjectPropertyElementMain
		};
		break;
	default:
		return kAudioHardwareUnsupportedOperationError;
	}

	if (changed_count && gHost)
		gHost->PropertiesChanged(gHost, obj, changed_count, changed);

	return 0;
}
