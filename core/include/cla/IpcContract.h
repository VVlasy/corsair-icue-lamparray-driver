/*
 * Copyright (c) Corsair Dynamic Lighting Bridge contributors.
 * SPDX-License-Identifier: MIT
 *
 * The driver <-> service message contract, and the validation of it.
 *
 * Why this lives in the portable core rather than beside the IOCTL codes:
 * validating these messages is the driver's entire attack surface against a
 * compromised or buggy user-mode service, and validation that cannot be unit
 * tested is validation nobody trusts.  Keeping the structures and the checks
 * here means the exact code the kernel runs is exercised by tests/IpcTests on
 * every CI run, on any OS.
 *
 * The Windows-only half -- IOCTL codes and the device interface GUID -- is in
 * inc/CorsairLampArrayIpc.h.
 *
 * Rules for this file:
 *   - Fixed-size, explicitly-padded structures only.  No pointers: a message
 *     is always a single self-contained buffer, so the driver never chases a
 *     user-mode pointer.
 *   - Every structure starts with ClaMessageHeader carrying a version and the
 *     total byte count, and every structure's size is asserted below.
 *   - UTF-16 strings use cla_u16 arrays, not wchar_t: wchar_t is 2 bytes on
 *     Windows and 4 on Linux, which would silently change the layout between
 *     the driver and these tests.
 */
#ifndef CLA_IPC_CONTRACT_H
#define CLA_IPC_CONTRACT_H

#include "ClaTypes.h"
#include "LampArrayDevice.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bumped whenever a structure layout or meaning changes.  The driver refuses
 * any message that does not carry exactly this value, so a stale service
 * cannot talk to a newer driver (or vice versa). */
#define CLA_IPC_PROTOCOL_VERSION 1

/* Limits.  These bound every allocation the driver makes on behalf of the
 * service, so they are enforced, not advisory. */
#define CLA_MAX_LAMP_ARRAYS        32u
/* The iCUE SDK caps a device at CORSAIR_DEVICE_LEDCOUNT_MAX (512) LEDs; 1024
 * leaves headroom without letting a single request pin much memory. */
#define CLA_MAX_LAMPS_PER_ARRAY    1024u
#define CLA_MAX_NAME_CHARS         64u
#define CLA_MAX_SERIAL_CHARS       64u

typedef struct _ClaMessageHeader
{
    /* Must equal CLA_IPC_PROTOCOL_VERSION. */
    cla_u32 Version;
    /* Total size of the whole message in bytes, including this header and any
     * trailing array.  Must equal the IOCTL buffer length exactly. */
    cla_u32 Size;
} ClaMessageHeader;

/* ------------------------------------------------------------------ */
/* GET_DEVICE_INFO                                                     */
/* ------------------------------------------------------------------ */
typedef struct _ClaDriverInfo
{
    ClaMessageHeader Header;
    cla_u32 ProtocolVersion;
    cla_u32 DriverVersionMajor;
    cla_u32 DriverVersionMinor;
    cla_u32 DriverVersionPatch;
    cla_u32 ActiveLampArrayCount;
    cla_u32 MaxLampArrayCount;
    cla_u32 MaxLampsPerArray;
    cla_u32 Reserved;
} ClaDriverInfo;

/* ------------------------------------------------------------------ */
/* CREATE_LAMP_ARRAY  (service -> driver)                              */
/*                                                                     */
/* The service is the authority on Corsair topology, so it pushes the   */
/* complete, immutable lamp table down once at creation.  The driver    */
/* then answers LampAttributes* reports from its own copy with no       */
/* user-mode round trip -- which is what keeps the HID path fast and    */
/* keeps it working while the service is restarting.                    */
/* ------------------------------------------------------------------ */
typedef struct _ClaLampArrayCreateRequest
{
    ClaMessageHeader Header;

    cla_u32 LampCount;
    cla_u32 BoundingBoxWidthInMicrometers;
    cla_u32 BoundingBoxHeightInMicrometers;
    cla_u32 BoundingBoxDepthInMicrometers;
    cla_u32 LampArrayKind;                    /* ClaLampArrayKind */
    cla_u32 MinUpdateIntervalInMicroseconds;

    cla_u16 VendorId;
    cla_u16 ProductId;
    cla_u16 VersionNumber;
    cla_u16 Reserved;

    /* NUL-terminated UTF-16. Shown in Device Manager and Settings. */
    cla_u16 DeviceName[CLA_MAX_NAME_CHARS];
    /* Distinguishes one virtual LampArray from another; derived from the
     * Corsair device id so it is stable across service restarts. */
    cla_u16 SerialNumber[CLA_MAX_SERIAL_CHARS];

    /* Exactly LampCount entries follow. */
    ClaLampInfo Lamps[1];
} ClaLampArrayCreateRequest;

typedef struct _ClaLampArrayCreateResponse
{
    ClaMessageHeader Header;
    cla_u32 LampArrayId;
    cla_u32 Reserved;
} ClaLampArrayCreateResponse;

typedef struct _ClaLampArrayIdRequest
{
    ClaMessageHeader Header;
    cla_u32 LampArrayId;
    cla_u32 Reserved;
} ClaLampArrayIdRequest;

/* ------------------------------------------------------------------ */
/* GET_NEXT_UPDATE  (driver -> service, inverted call)                 */
/*                                                                     */
/* The service parks requests on the driver; the driver completes one   */
/* when Windows finishes a frame.  Only the newest frame per lamp array */
/* is retained, so a slow service coalesces rather than backs up: for   */
/* RGB, a stale frame has no value.                                     */
/* ------------------------------------------------------------------ */
typedef enum _ClaUpdateKind
{
    ClaUpdateKindNone = 0,
    /* A complete frame of lamp colours. */
    ClaUpdateKindLampColors = 1,
    /* Host enabled or disabled autonomous mode. */
    ClaUpdateKindAutonomousMode = 2,
    /* The virtual device is going away. */
    ClaUpdateKindRemoved = 3
} ClaUpdateKind;

typedef struct _ClaUpdateMessage
{
    ClaMessageHeader Header;
    cla_u32 LampArrayId;
    cla_u32 Kind;                 /* ClaUpdateKind */
    /* Monotonic per lamp array. Gaps mean frames were coalesced. */
    cla_u64 FrameSequence;
    cla_u32 AutonomousMode;
    /* Number of Colors entries; equals the array's lamp count for
     * ClaUpdateKindLampColors, otherwise 0. */
    cla_u32 LampCount;
    ClaLampColor Colors[1];
} ClaUpdateMessage;
/* NOTE: FrameSequence gives this struct 8-byte alignment, so sizeof() includes
 * trailing padding after Colors[1] and is LARGER than the wire size of a
 * one-lamp message.  Always size these messages with
 * ClaUpdateMessageSizeForLampCount(), never with sizeof() plus a lamp count. */

/* ------------------------------------------------------------------ */
/* Diagnostics                                                         */
/* ------------------------------------------------------------------ */
typedef struct _ClaStatisticsResponse
{
    ClaMessageHeader Header;
    cla_u32 LampArrayId;
    cla_u32 Reserved;
    ClaLampArrayCounters Counters;
    cla_u64 FramesPublished;
    cla_u64 FramesCoalesced;
} ClaStatisticsResponse;

typedef struct _ClaLampInfoRequest
{
    ClaMessageHeader Header;
    cla_u32 LampArrayId;
    cla_u32 LampId;
} ClaLampInfoRequest;

typedef struct _ClaLampInfoResponse
{
    ClaMessageHeader Header;
    cla_u32 LampArrayId;
    cla_u32 LampId;
    ClaLampInfo Info;
} ClaLampInfoResponse;

typedef struct _ClaLampCountResponse
{
    ClaMessageHeader Header;
    cla_u32 LampArrayId;
    cla_u32 LampCount;
} ClaLampCountResponse;

/* ------------------------------------------------------------------ */
/* SET_BRIGHTNESS                                                      */
/*                                                                     */
/* The HID LampArray protocol has no device-wide brightness control --  */
/* brightness is per-lamp via IntensityUpdateChannel.  This message is  */
/* a local scale factor the service applies when translating to the     */
/* Corsair SDK, exposed so tools can drive it.                          */
/* ------------------------------------------------------------------ */
typedef struct _ClaSetBrightnessRequest
{
    ClaMessageHeader Header;
    cla_u32 LampArrayId;
    /* 0..255, where 255 is unity gain. */
    cla_u32 Brightness;
} ClaSetBrightnessRequest;

/* Backend availability, pushed by the service (DEVICE_STATE_CHANGED). */
typedef enum _ClaDeviceState
{
    ClaDeviceStateUnavailable = 0,   /* iCUE gone, or device disconnected */
    ClaDeviceStateAvailable = 1
} ClaDeviceState;

typedef struct _ClaSetDeviceStateRequest
{
    ClaMessageHeader Header;
    cla_u32 LampArrayId;
    cla_u32 State;                   /* ClaDeviceState */
} ClaSetDeviceStateRequest;

/* ------------------------------------------------------------------ */
/* Validation.                                                         */
/*                                                                     */
/* Every one of these is called by the driver before it touches a byte  */
/* of the message body.  They are total functions: any pointer/length   */
/* pair at all is a legal call, including null and zero.                */
/* ------------------------------------------------------------------ */

/* Byte offset of the trailing Lamps array, and the exact total size a
 * create request with `lampCount` lamps must have.  Returns 0 for a lamp
 * count that would be rejected, so callers cannot use the result to build an
 * over-large request. */
cla_u32 ClaCreateRequestSizeForLampCount(cla_u32 lampCount);

/* Exact total size of an update message carrying `lampCount` colours, or 0
 * if the count is out of range. */
cla_u32 ClaUpdateMessageSizeForLampCount(cla_u32 lampCount);

/*
 * Validate a CREATE_LAMP_ARRAY buffer received from user mode.
 *
 * Checks, in order: buffer present and large enough for the fixed part; header
 * version; header size equals the actual buffer length; lamp count in range;
 * total size exactly matches the lamp count; strings NUL-terminated; per-lamp
 * fields self-consistent.
 *
 * Returns ClaStatusOk only if the buffer can then be read in full safely.
 */
ClaStatus ClaValidateCreateRequest(const void *buffer, cla_u32 bufferLength);

/* Validate any of the fixed-size request messages.  expectedSize is the
 * sizeof() of the specific structure. */
ClaStatus ClaValidateFixedMessage(const void *buffer, cla_u32 bufferLength, cla_u32 expectedSize);

/* Validate an output buffer the service offered for an inverted-call update:
 * it must be able to hold a full frame for `lampCount` lamps. */
ClaStatus ClaValidateUpdateOutputBuffer(cla_u32 bufferLength, cla_u32 lampCount);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CLA_IPC_CONTRACT_H */
