/*
 * Copyright (c) Corsair Dynamic Lighting Bridge contributors.
 * SPDX-License-Identifier: MIT
 *
 * LampArray protocol state machine.  See LampArrayDevice.h.
 *
 * Compiled into the kernel driver, so: no CRT, no STL, no allocation.
 */

#include "cla/LampArrayDevice.h"

/* ------------------------------------------------------------------ */
/* Little-endian serialisation helpers.                                */
/*                                                                     */
/* HID reports are little-endian byte streams.  Writing them out byte  */
/* by byte keeps the code free of struct-packing assumptions and of    */
/* unaligned/strict-aliasing hazards, and makes it identical under     */
/* MSVC (kernel) and GCC/Clang (tests).                                */
/* ------------------------------------------------------------------ */

static void ClaWriteU8(cla_u8 *p, cla_u32 offset, cla_u8 value)
{
    p[offset] = value;
}

static void ClaWriteU16(cla_u8 *p, cla_u32 offset, cla_u16 value)
{
    p[offset + 0] = (cla_u8)(value & 0xFFu);
    p[offset + 1] = (cla_u8)((value >> 8) & 0xFFu);
}

static void ClaWriteU32(cla_u8 *p, cla_u32 offset, cla_u32 value)
{
    p[offset + 0] = (cla_u8)(value & 0xFFu);
    p[offset + 1] = (cla_u8)((value >> 8) & 0xFFu);
    p[offset + 2] = (cla_u8)((value >> 16) & 0xFFu);
    p[offset + 3] = (cla_u8)((value >> 24) & 0xFFu);
}

static cla_u8 ClaReadU8(const cla_u8 *p, cla_u32 offset)
{
    return p[offset];
}

static cla_u16 ClaReadU16(const cla_u8 *p, cla_u32 offset)
{
    return (cla_u16)((cla_u16)p[offset] | ((cla_u16)p[offset + 1] << 8));
}

static void ClaZeroMemory(void *dst, cla_u32 length)
{
    cla_u8 *d = (cla_u8 *)dst;
    for (cla_u32 i = 0; i < length; ++i)
    {
        d[i] = 0;
    }
}

/* ------------------------------------------------------------------ */

cla_u32 ClaGetReportBufferSize(cla_u8 reportId)
{
    switch (reportId)
    {
    case CLA_REPORT_ID_LAMP_ARRAY_ATTRIBUTES:   return CLA_BUFFER_SIZE_LAMP_ARRAY_ATTRIBUTES;
    case CLA_REPORT_ID_LAMP_ATTRIBUTES_REQUEST: return CLA_BUFFER_SIZE_LAMP_ATTRIBUTES_REQUEST;
    case CLA_REPORT_ID_LAMP_ATTRIBUTES_RESPONSE:return CLA_BUFFER_SIZE_LAMP_ATTRIBUTES_RESPONSE;
    case CLA_REPORT_ID_LAMP_MULTI_UPDATE:       return CLA_BUFFER_SIZE_LAMP_MULTI_UPDATE;
    case CLA_REPORT_ID_LAMP_RANGE_UPDATE:       return CLA_BUFFER_SIZE_LAMP_RANGE_UPDATE;
    case CLA_REPORT_ID_LAMP_ARRAY_CONTROL:      return CLA_BUFFER_SIZE_LAMP_ARRAY_CONTROL;
    default:                                    return 0;
    }
}

ClaStatus ClaLampArrayInit(
    ClaLampArrayState *state,
    const ClaLampArrayAttributes *attributes,
    const ClaLampInfo *lamps,
    ClaLampColor *colors)
{
    if (state == 0 || attributes == 0 || lamps == 0 || colors == 0)
    {
        return ClaStatusInvalidArgument;
    }
    /*
     * The upper bound is structural rather than checked at run time: LampCount
     * is a 16-bit field and CLA_MAX_LAMP_COUNT is 65535, so it cannot be
     * exceeded.  Asserting that at compile time documents the bound and, unlike
     * a runtime comparison, cannot rot if the field type ever widens -- it
     * would become a build failure instead of a silently dead check.
     */
    static_assert(sizeof(attributes->LampCount) == 2,
                  "LampCount is assumed to be 16-bit; widen the bounds check "
                  "in ClaLampArrayInit if it changes");
    static_assert(CLA_MAX_LAMP_COUNT == 0xFFFFu,
                  "CLA_MAX_LAMP_COUNT must match the width of the LampId field");

    if (attributes->LampCount == 0)
    {
        return ClaStatusInvalidArgument;
    }

    ClaZeroMemory(state, (cla_u32)sizeof(*state));

    state->Attributes = *attributes;
    state->Lamps = lamps;
    state->Colors = colors;
    state->NextLampId = 0;

    /* HUT 1.4 s26.10.1: default autonomous mode is enabled/true. */
    state->AutonomousMode = 1;
    state->StrictDuplicateLampIdCheck = 1;

    /* HUT 1.4 s26.11: default state for all Lamps is off (RGBI = 0,0,0,0). */
    ClaZeroMemory(colors, (cla_u32)sizeof(ClaLampColor) * attributes->LampCount);

    return ClaStatusOk;
}

void ClaLampArrayBlank(ClaLampArrayState *state)
{
    if (state == 0 || state->Colors == 0)
    {
        return;
    }
    ClaZeroMemory(state->Colors,
                  (cla_u32)sizeof(ClaLampColor) * state->Attributes.LampCount);
}

/* ------------------------------------------------------------------ */
/* GET_FEATURE                                                         */
/* ------------------------------------------------------------------ */

static void ClaBuildAttributesReport(const ClaLampArrayState *state, cla_u8 *buffer)
{
    /* Offsets fixed by the report descriptor:
     *   0  report id
     *   1  LampCount                        u16
     *   3  BoundingBoxWidthInMicrometers    u32
     *   7  BoundingBoxHeightInMicrometers   u32
     *  11  BoundingBoxDepthInMicrometers    u32
     *  15  LampArrayKind                    u32
     *  19  MinUpdateIntervalInMicroseconds  u32
     *  == 23 bytes total */
    ClaWriteU8(buffer, 0, CLA_REPORT_ID_LAMP_ARRAY_ATTRIBUTES);
    ClaWriteU16(buffer, 1, state->Attributes.LampCount);
    ClaWriteU32(buffer, 3, state->Attributes.BoundingBoxWidthInMicrometers);
    ClaWriteU32(buffer, 7, state->Attributes.BoundingBoxHeightInMicrometers);
    ClaWriteU32(buffer, 11, state->Attributes.BoundingBoxDepthInMicrometers);
    ClaWriteU32(buffer, 15, state->Attributes.LampArrayKind);
    ClaWriteU32(buffer, 19, state->Attributes.MinUpdateIntervalInMicroseconds);
}

static void ClaBuildLampAttributesResponse(ClaLampArrayState *state, cla_u8 *buffer)
{
    /* Offsets fixed by the report descriptor:
     *   0  report id
     *   1  LampId                          u16
     *   3  PositionXInMicrometers          u32
     *   7  PositionYInMicrometers          u32
     *  11  PositionZInMicrometers          u32
     *  15  UpdateLatencyInMicroseconds     u32
     *  19  LampPurposes                    u32
     *  23  RedLevelCount                   u8
     *  24  GreenLevelCount                 u8
     *  25  BlueLevelCount                  u8
     *  26  IntensityLevelCount             u8
     *  27  IsProgrammable                  u8
     *  28  InputBinding                    u8
     *  == 29 bytes total
     *
     * Note the descriptor orders UpdateLatency BEFORE LampPurposes, even
     * though the usage ids run the other way (0x26 LampPurposes, 0x27
     * UpdateLatency).  The descriptor's declaration order is what defines the
     * wire layout, so this follows the descriptor. */
    const cla_u16 lampId = state->NextLampId;
    const ClaLampInfo *lamp = &state->Lamps[lampId];

    ClaWriteU8(buffer, 0, CLA_REPORT_ID_LAMP_ATTRIBUTES_RESPONSE);
    ClaWriteU16(buffer, 1, lampId);
    ClaWriteU32(buffer, 3, lamp->PositionXInMicrometers);
    ClaWriteU32(buffer, 7, lamp->PositionYInMicrometers);
    ClaWriteU32(buffer, 11, lamp->PositionZInMicrometers);
    ClaWriteU32(buffer, 15, lamp->UpdateLatencyInMicroseconds);
    ClaWriteU32(buffer, 19, lamp->LampPurposes);
    ClaWriteU8(buffer, 23, lamp->RedLevelCount);
    ClaWriteU8(buffer, 24, lamp->GreenLevelCount);
    ClaWriteU8(buffer, 25, lamp->BlueLevelCount);
    ClaWriteU8(buffer, 26, lamp->IntensityLevelCount);
    ClaWriteU8(buffer, 27, lamp->IsProgrammable);
    ClaWriteU8(buffer, 28, lamp->InputBinding);

    /* HUT 1.4 s26.8.2: after a successful response the device increments the
     * internal LampId, wrapping to 0 after LampCount-1. */
    if ((cla_u32)lampId + 1u >= (cla_u32)state->Attributes.LampCount)
    {
        state->NextLampId = 0;
    }
    else
    {
        state->NextLampId = (cla_u16)(lampId + 1);
    }
}

ClaStatus ClaLampArrayGetFeature(
    ClaLampArrayState *state,
    cla_u8 reportId,
    cla_u8 *buffer,
    cla_u32 bufferLength,
    cla_u32 *bytesWritten)
{
    if (state == 0 || buffer == 0 || state->Lamps == 0 || state->Colors == 0)
    {
        return ClaStatusInvalidArgument;
    }
    if (bytesWritten != 0)
    {
        *bytesWritten = 0;
    }

    state->Counters.GetFeatureCalls++;

    const cla_u32 expected = ClaGetReportBufferSize(reportId);
    if (expected == 0)
    {
        state->Counters.RejectedUnknownReportId++;
        return ClaStatusUnknownReportId;
    }

    /* Only the two attribute reports are readable. */
    if (reportId != CLA_REPORT_ID_LAMP_ARRAY_ATTRIBUTES &&
        reportId != CLA_REPORT_ID_LAMP_ATTRIBUTES_RESPONSE)
    {
        return ClaStatusInvalidOperation;
    }

    /* The buffer must be at least the declared report size.  HIDCLASS sizes
     * the buffer from the descriptor, so a short buffer means something is
     * wrong; refuse rather than write past the end. */
    if (bufferLength < expected)
    {
        state->Counters.RejectedBadBufferSize++;
        return ClaStatusBadBufferSize;
    }

    if (reportId == CLA_REPORT_ID_LAMP_ARRAY_ATTRIBUTES)
    {
        ClaBuildAttributesReport(state, buffer);
    }
    else
    {
        /* NextLampId is kept in range by every path that assigns it, but this
         * report indexes the lamp table with it, so re-check before use. */
        if (state->NextLampId >= state->Attributes.LampCount)
        {
            state->NextLampId = 0;
        }
        ClaBuildLampAttributesResponse(state, buffer);
    }

    if (bytesWritten != 0)
    {
        *bytesWritten = expected;
    }
    return ClaStatusOk;
}

/* ------------------------------------------------------------------ */
/* SET_FEATURE                                                         */
/* ------------------------------------------------------------------ */

/* HUT 1.4 s26.11.1: "*Channel > *LevelCount described by Lamps' attributes"
 * voids the report.  A LevelCount of N means values 0..N are legal (0 = off,
 * N = maximum), per s26.9. */
static int ClaColorFitsLamp(const ClaLampInfo *lamp, const ClaLampColor *color)
{
    if (color->Red > lamp->RedLevelCount) return 0;
    if (color->Green > lamp->GreenLevelCount) return 0;
    if (color->Blue > lamp->BlueLevelCount) return 0;
    if (color->Intensity > lamp->IntensityLevelCount) return 0;
    return 1;
}

/* Apply a colour, returning 1 if it actually changed. */
static int ClaApplyColor(ClaLampArrayState *state, cla_u16 lampId, const ClaLampColor *color)
{
    ClaLampColor *dst = &state->Colors[lampId];

    /* HUT 1.4 s26.11.1: for FixedColor lamps (IsProgrammable == 0) only the
     * intensity channel is examined; RGB is always ignored. */
    ClaLampColor effective = *color;
    if (state->Lamps[lampId].IsProgrammable == 0)
    {
        effective.Red = dst->Red;
        effective.Green = dst->Green;
        effective.Blue = dst->Blue;
    }

    if (dst->Red == effective.Red && dst->Green == effective.Green &&
        dst->Blue == effective.Blue && dst->Intensity == effective.Intensity)
    {
        return 0;
    }

    *dst = effective;
    return 1;
}

static ClaStatus ClaHandleMultiUpdate(
    ClaLampArrayState *state,
    const cla_u8 *buffer,
    ClaUpdateOutcome *outcome)
{
    /* Offsets fixed by the report descriptor:
     *   0  report id
     *   1  LampCount        u8   (number of populated slots)
     *   2  LampUpdateFlags  u8
     *   3  LampIds[8]       u16 each  -> 3 .. 18
     *  19  Colors[8]        4 x u8    -> 19 .. 50
     *  == 51 bytes total */
    const cla_u8 slotCount = ClaReadU8(buffer, 1);
    const cla_u8 flags = ClaReadU8(buffer, 2);

    if (slotCount > CLA_LAMP_MULTI_UPDATE_SLOT_COUNT)
    {
        state->Counters.RejectedInvalidReport++;
        return ClaStatusInvalidReport;
    }

    /* Validate everything BEFORE mutating any lamp: the spec requires that a
     * report containing any error be ignored in its entirety, so partial
     * application is not permitted. */
    cla_u16 lampIds[CLA_LAMP_MULTI_UPDATE_SLOT_COUNT];
    ClaLampColor colors[CLA_LAMP_MULTI_UPDATE_SLOT_COUNT];

    for (cla_u8 i = 0; i < slotCount; ++i)
    {
        const cla_u16 lampId = ClaReadU16(buffer, 3u + (cla_u32)i * 2u);
        if (lampId >= state->Attributes.LampCount)
        {
            state->Counters.RejectedLampIdOutOfRange++;
            state->Counters.RejectedInvalidReport++;
            return ClaStatusInvalidReport;
        }

        const cla_u32 colorOffset = 19u + (cla_u32)i * 4u;
        ClaLampColor color;
        color.Red = ClaReadU8(buffer, colorOffset + 0);
        color.Green = ClaReadU8(buffer, colorOffset + 1);
        color.Blue = ClaReadU8(buffer, colorOffset + 2);
        color.Intensity = ClaReadU8(buffer, colorOffset + 3);

        if (!ClaColorFitsLamp(&state->Lamps[lampId], &color))
        {
            state->Counters.RejectedChannelOverLevelCount++;
            state->Counters.RejectedInvalidReport++;
            return ClaStatusInvalidReport;
        }

        if (state->StrictDuplicateLampIdCheck)
        {
            for (cla_u8 j = 0; j < i; ++j)
            {
                if (lampIds[j] == lampId)
                {
                    state->Counters.RejectedDuplicateLampId++;
                    state->Counters.RejectedInvalidReport++;
                    return ClaStatusInvalidReport;
                }
            }
        }

        lampIds[i] = lampId;
        colors[i] = color;
    }

    cla_u32 changed = 0;
    for (cla_u8 i = 0; i < slotCount; ++i)
    {
        changed += (cla_u32)ClaApplyColor(state, lampIds[i], &colors[i]);
    }

    if (outcome != 0)
    {
        outcome->LampsChanged = changed;
        outcome->FrameComplete = (cla_u8)((flags & CLA_LAMP_UPDATE_COMPLETE) ? 1 : 0);
    }
    if (flags & CLA_LAMP_UPDATE_COMPLETE)
    {
        state->Counters.FramesCompleted++;
    }
    return ClaStatusOk;
}

static ClaStatus ClaHandleRangeUpdate(
    ClaLampArrayState *state,
    const cla_u8 *buffer,
    ClaUpdateOutcome *outcome)
{
    /* Offsets fixed by the report descriptor:
     *   0  report id
     *   1  LampUpdateFlags  u8
     *   2  LampIdStart      u16
     *   4  LampIdEnd        u16
     *   6  Color            4 x u8
     *  == 10 bytes total */
    const cla_u8 flags = ClaReadU8(buffer, 1);
    const cla_u16 lampIdStart = ClaReadU16(buffer, 2);
    const cla_u16 lampIdEnd = ClaReadU16(buffer, 4);

    ClaLampColor color;
    color.Red = ClaReadU8(buffer, 6);
    color.Green = ClaReadU8(buffer, 7);
    color.Blue = ClaReadU8(buffer, 8);
    color.Intensity = ClaReadU8(buffer, 9);

    /* HUT 1.4 s26.11.2 error cases. */
    if (lampIdStart > lampIdEnd)
    {
        state->Counters.RejectedInvalidReport++;
        return ClaStatusInvalidReport;
    }
    if (lampIdStart >= state->Attributes.LampCount ||
        lampIdEnd >= state->Attributes.LampCount)
    {
        state->Counters.RejectedLampIdOutOfRange++;
        state->Counters.RejectedInvalidReport++;
        return ClaStatusInvalidReport;
    }

    /* "The Host must ensure all Lamps in the described range support the
     * desired channel intensities" -- so validate the whole range first. */
    for (cla_u32 id = lampIdStart; id <= (cla_u32)lampIdEnd; ++id)
    {
        if (!ClaColorFitsLamp(&state->Lamps[id], &color))
        {
            state->Counters.RejectedChannelOverLevelCount++;
            state->Counters.RejectedInvalidReport++;
            return ClaStatusInvalidReport;
        }
    }

    cla_u32 changed = 0;
    for (cla_u32 id = lampIdStart; id <= (cla_u32)lampIdEnd; ++id)
    {
        changed += (cla_u32)ClaApplyColor(state, (cla_u16)id, &color);
    }

    if (outcome != 0)
    {
        outcome->LampsChanged = changed;
        outcome->FrameComplete = (cla_u8)((flags & CLA_LAMP_UPDATE_COMPLETE) ? 1 : 0);
    }
    if (flags & CLA_LAMP_UPDATE_COMPLETE)
    {
        state->Counters.FramesCompleted++;
    }
    return ClaStatusOk;
}

ClaStatus ClaLampArraySetFeature(
    ClaLampArrayState *state,
    cla_u8 reportId,
    const cla_u8 *buffer,
    cla_u32 bufferLength,
    ClaUpdateOutcome *outcome)
{
    if (state == 0 || buffer == 0 || state->Lamps == 0 || state->Colors == 0)
    {
        return ClaStatusInvalidArgument;
    }

    if (outcome != 0)
    {
        ClaZeroMemory(outcome, (cla_u32)sizeof(*outcome));
        outcome->AutonomousMode = state->AutonomousMode;
    }

    state->Counters.SetFeatureCalls++;

    const cla_u32 expected = ClaGetReportBufferSize(reportId);
    if (expected == 0)
    {
        state->Counters.RejectedUnknownReportId++;
        return ClaStatusUnknownReportId;
    }

    /* Only the four settable reports. */
    if (reportId == CLA_REPORT_ID_LAMP_ARRAY_ATTRIBUTES ||
        reportId == CLA_REPORT_ID_LAMP_ATTRIBUTES_RESPONSE)
    {
        return ClaStatusInvalidOperation;
    }

    /* A SET must carry exactly the declared report size.  Anything else is
     * malformed; refuse before reading a single payload byte. */
    if (bufferLength != expected)
    {
        state->Counters.RejectedBadBufferSize++;
        return ClaStatusBadBufferSize;
    }

    switch (reportId)
    {
    case CLA_REPORT_ID_LAMP_ATTRIBUTES_REQUEST:
    {
        const cla_u16 lampId = ClaReadU16(buffer, 1);
        /* HUT 1.4 s26.8.1: "An invalid LampId must be treated by the device as
         * LampId == 0." */
        state->NextLampId = (lampId < state->Attributes.LampCount) ? lampId : 0;
        return ClaStatusOk;
    }

    case CLA_REPORT_ID_LAMP_MULTI_UPDATE:
        return ClaHandleMultiUpdate(state, buffer, outcome);

    case CLA_REPORT_ID_LAMP_RANGE_UPDATE:
        return ClaHandleRangeUpdate(state, buffer, outcome);

    case CLA_REPORT_ID_LAMP_ARRAY_CONTROL:
    {
        const cla_u8 raw = ClaReadU8(buffer, 1);
        /* LogicalMaximum(1) in the descriptor: only 0 and 1 are legal. */
        if (raw > 1)
        {
            state->Counters.RejectedInvalidReport++;
            return ClaStatusInvalidReport;
        }
        const cla_u8 previous = state->AutonomousMode;
        state->AutonomousMode = raw;
        if (outcome != 0)
        {
            outcome->AutonomousMode = raw;
            outcome->AutonomousModeChanged = (cla_u8)(previous != raw ? 1 : 0);
        }
        return ClaStatusOk;
    }

    default:
        state->Counters.RejectedUnknownReportId++;
        return ClaStatusUnknownReportId;
    }
}
