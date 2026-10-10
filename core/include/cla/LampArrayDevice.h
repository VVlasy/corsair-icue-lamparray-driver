/*
 * Copyright (c) Corsair Dynamic Lighting Bridge contributors.
 * SPDX-License-Identifier: MIT
 *
 * The LampArray protocol state machine.
 *
 * This is the only code that parses host-supplied bytes, and it is compiled
 * verbatim into the kernel driver, so it obeys kernel rules: no CRT, no STL,
 * no exceptions, no RTTI, no allocation.  All storage is supplied by the
 * caller.  Every entry point takes an explicit buffer length and validates it
 * before touching the buffer.
 *
 * Behaviour is specified by USB HID Usage Tables v1.4 section 26.
 */
#ifndef CLA_LAMP_ARRAY_DEVICE_H
#define CLA_LAMP_ARRAY_DEVICE_H

#include "ClaTypes.h"
#include "LampArrayProtocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* An RGBI tuple as carried by the update reports. */
typedef struct _ClaLampColor
{
    cla_u8 Red;
    cla_u8 Green;
    cla_u8 Blue;
    cla_u8 Intensity;
} ClaLampColor;

/* Static per-lamp attributes.  HUT 1.4 s26.8.3: "All Lamp attributes are
 * static and can never change across device resets or external events."  The
 * service builds this table once from the Corsair LED topology and hands it to
 * the driver; the driver then answers LampAttributesResponseReport without any
 * user-mode round trip. */
typedef struct _ClaLampInfo
{
    cla_u32 PositionXInMicrometers;
    cla_u32 PositionYInMicrometers;
    cla_u32 PositionZInMicrometers;
    cla_u32 UpdateLatencyInMicroseconds;
    cla_u32 LampPurposes;          /* CLA_LAMP_PURPOSE_* bitmask */
    cla_u8  RedLevelCount;
    cla_u8  GreenLevelCount;
    cla_u8  BlueLevelCount;
    cla_u8  IntensityLevelCount;
    cla_u8  IsProgrammable;        /* 0 or 1 */
    cla_u8  InputBinding;          /* HID keyboard/button usage, or 0 */
    cla_u8  Reserved[2];
} ClaLampInfo;                     /* 28 bytes */

/* Device-wide attributes reported by LampArrayAttributesReport. */
typedef struct _ClaLampArrayAttributes
{
    cla_u16 LampCount;
    cla_u32 BoundingBoxWidthInMicrometers;
    cla_u32 BoundingBoxHeightInMicrometers;
    cla_u32 BoundingBoxDepthInMicrometers;
    cla_u32 LampArrayKind;                    /* ClaLampArrayKind */
    cla_u32 MinUpdateIntervalInMicroseconds;
} ClaLampArrayAttributes;

/* What a SET_FEATURE did, so the caller knows whether to publish a frame. */
typedef struct _ClaUpdateOutcome
{
    /* Non-zero when the host set LampUpdateComplete: the accumulated lamp
     * state is now a coherent frame and should be pushed to the backend. */
    cla_u8  FrameComplete;
    /* Non-zero when AutonomousMode changed value in this call. */
    cla_u8  AutonomousModeChanged;
    /* Current autonomous-mode value after the call. */
    cla_u8  AutonomousMode;
    cla_u8  Reserved;
    /* Number of lamps whose colour actually changed in this call. */
    cla_u32 LampsChanged;
} ClaUpdateOutcome;

/* Diagnostic counters.  Rejected reports are invisible to the host (the spec
 * requires silently ignoring them), so they must be observable somewhere. */
typedef struct _ClaLampArrayCounters
{
    cla_u64 GetFeatureCalls;
    cla_u64 SetFeatureCalls;
    cla_u64 FramesCompleted;
    cla_u64 RejectedBadBufferSize;
    cla_u64 RejectedInvalidReport;
    cla_u64 RejectedUnknownReportId;
    cla_u64 RejectedDuplicateLampId;
    cla_u64 RejectedLampIdOutOfRange;
    cla_u64 RejectedChannelOverLevelCount;
} ClaLampArrayCounters;

typedef struct _ClaLampArrayState
{
    ClaLampArrayAttributes Attributes;

    /* Caller-owned, Attributes.LampCount entries.  Read only. */
    const ClaLampInfo *Lamps;

    /* Caller-owned, Attributes.LampCount entries.  The shadow lamp state that
     * update reports accumulate into. */
    ClaLampColor *Colors;

    /* Cursor for LampAttributesResponseReport (HUT 1.4 s26.8.2). */
    cla_u16 NextLampId;

    /* HUT 1.4 s26.10.1: "Default state for this field is enabled/true." */
    cla_u8 AutonomousMode;

    /* HUT 1.4 s26.11.1 lists "Identical LampId in multiple slots" as an error
     * that must void the whole report.  Enforced by default; the escape hatch
     * exists only in case a host is ever found to violate it, in which case
     * rejecting whole frames would break lighting for no benefit. */
    cla_u8 StrictDuplicateLampIdCheck;

    ClaLampArrayCounters Counters;
} ClaLampArrayState;

/*
 * Initialise the state machine.
 *
 * lamps  - array of at least attributes->LampCount entries, owned by caller,
 *          must outlive the state.
 * colors - array of at least attributes->LampCount entries, owned by caller.
 *          Zeroed here: HUT 1.4 s26.11 "Default state for all Lamps is off
 *          (RGBI=0,0,0,0)".
 *
 * Returns ClaStatusInvalidArgument for null pointers or a LampCount of zero
 * or above CLA_MAX_LAMP_COUNT.
 */
ClaStatus ClaLampArrayInit(
    ClaLampArrayState *state,
    const ClaLampArrayAttributes *attributes,
    const ClaLampInfo *lamps,
    ClaLampColor *colors);

/*
 * Handle IOCTL_HID_GET_FEATURE.
 *
 * buffer/bufferLength describe the whole HID transfer buffer INCLUDING the
 * leading report id byte.  On success the report id is written to buffer[0],
 * the payload follows, and *bytesWritten is set to the full buffer size.
 *
 * Valid report ids are CLA_REPORT_ID_LAMP_ARRAY_ATTRIBUTES and
 * CLA_REPORT_ID_LAMP_ATTRIBUTES_RESPONSE; every other id returns
 * ClaStatusInvalidOperation or ClaStatusUnknownReportId.
 */
ClaStatus ClaLampArrayGetFeature(
    ClaLampArrayState *state,
    cla_u8 reportId,
    cla_u8 *buffer,
    cla_u32 bufferLength,
    cla_u32 *bytesWritten);

/*
 * Handle IOCTL_HID_SET_FEATURE.
 *
 * buffer/bufferLength describe the whole HID transfer buffer INCLUDING the
 * leading report id byte.  outcome may be null if the caller does not care.
 *
 * A ClaStatusInvalidReport return means the report was well-formed but
 * semantically invalid and has been ignored in its entirety, as HUT 1.4
 * s26.11.1/s26.11.2 require.  Lamp state is left untouched in that case.
 */
ClaStatus ClaLampArraySetFeature(
    ClaLampArrayState *state,
    cla_u8 reportId,
    const cla_u8 *buffer,
    cla_u32 bufferLength,
    ClaUpdateOutcome *outcome);

/* Expected full transfer-buffer length for a report id, or 0 if the id is not
 * one of ours. */
cla_u32 ClaGetReportBufferSize(cla_u8 reportId);

/* Reset every lamp to off without disturbing autonomous mode or the attribute
 * cursor.  Used when the host relinquishes control or the backend goes away. */
void ClaLampArrayBlank(ClaLampArrayState *state);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CLA_LAMP_ARRAY_DEVICE_H */
