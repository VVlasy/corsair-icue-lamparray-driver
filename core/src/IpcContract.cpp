/*
 * Copyright (c) Corsair Dynamic Lighting Bridge contributors.
 * SPDX-License-Identifier: MIT
 *
 * Validation of driver <-> service messages.  See IpcContract.h.
 *
 * This is the driver's trust boundary.  Everything here assumes the caller is
 * hostile: arithmetic is done in 64-bit to make overflow impossible rather
 * than merely unlikely, and no field is read until the length that covers it
 * has been proved.
 */

#include "cla/IpcContract.h"

/* Offset of the trailing Lamps array within a create request. */
static cla_u32 ClaCreateRequestFixedSize(void)
{
    /* offsetof without <stddef.h>, which kernel-mode C++ may not pull in. */
    return (cla_u32)((cla_u8 *)&((ClaLampArrayCreateRequest *)0)->Lamps - (cla_u8 *)0);
}

static cla_u32 ClaUpdateMessageFixedSizeInternal(void)
{
    return (cla_u32)((cla_u8 *)&((ClaUpdateMessage *)0)->Colors - (cla_u8 *)0);
}

cla_u32 ClaCreateRequestSizeForLampCount(cla_u32 lampCount)
{
    if (lampCount == 0 || lampCount > CLA_MAX_LAMPS_PER_ARRAY)
    {
        return 0;
    }
    /* 64-bit: lampCount is bounded above, but computing in 64-bit means the
     * bound is what limits the result rather than the arithmetic wrapping. */
    const cla_u64 total = (cla_u64)ClaCreateRequestFixedSize() +
                          (cla_u64)lampCount * (cla_u64)sizeof(ClaLampInfo);
    if (total > 0xFFFFFFFFull)
    {
        return 0;
    }
    return (cla_u32)total;
}

cla_u32 ClaUpdateMessageFixedSize(void)
{
    return ClaUpdateMessageFixedSizeInternal();
}

cla_u32 ClaUpdateMessageSizeForLampCount(cla_u32 lampCount)
{
    if (lampCount == 0 || lampCount > CLA_MAX_LAMPS_PER_ARRAY)
    {
        return 0;
    }
    const cla_u64 total = (cla_u64)ClaUpdateMessageFixedSizeInternal() +
                          (cla_u64)lampCount * (cla_u64)sizeof(ClaLampColor);
    if (total > 0xFFFFFFFFull)
    {
        return 0;
    }
    return (cla_u32)total;
}

/* A UTF-16 string field must contain a NUL within its bounds, otherwise
 * anything that later treats it as a C string reads past the end. */
static int ClaStringIsTerminated(const cla_u16 *chars, cla_u32 capacity)
{
    for (cla_u32 i = 0; i < capacity; ++i)
    {
        if (chars[i] == 0)
        {
            return 1;
        }
    }
    return 0;
}

ClaStatus ClaValidateFixedMessage(const void *buffer, cla_u32 bufferLength, cla_u32 expectedSize)
{
    if (buffer == 0)
    {
        return ClaStatusInvalidArgument;
    }
    if (expectedSize < (cla_u32)sizeof(ClaMessageHeader))
    {
        return ClaStatusInvalidArgument;
    }
    /* Exact match: a longer buffer means the sender disagrees with us about
     * the layout, which is not something to paper over. */
    if (bufferLength != expectedSize)
    {
        return ClaStatusBadBufferSize;
    }

    const ClaMessageHeader *header = (const ClaMessageHeader *)buffer;
    if (header->Version != CLA_IPC_PROTOCOL_VERSION)
    {
        return ClaStatusInvalidReport;
    }
    if (header->Size != bufferLength)
    {
        return ClaStatusBadBufferSize;
    }
    return ClaStatusOk;
}

ClaStatus ClaValidateCreateRequest(const void *buffer, cla_u32 bufferLength)
{
    if (buffer == 0)
    {
        return ClaStatusInvalidArgument;
    }

    /* 1. Enough bytes to read the header and the fixed fields at all.  Until
     *    this passes, not one field below may be touched. */
    const cla_u32 fixedSize = ClaCreateRequestFixedSize();
    if (bufferLength < fixedSize)
    {
        return ClaStatusBadBufferSize;
    }

    const ClaLampArrayCreateRequest *request = (const ClaLampArrayCreateRequest *)buffer;

    /* 2. Version, before trusting any other field's meaning. */
    if (request->Header.Version != CLA_IPC_PROTOCOL_VERSION)
    {
        return ClaStatusInvalidReport;
    }

    /* 3. The sender's declared size must match what actually arrived. */
    if (request->Header.Size != bufferLength)
    {
        return ClaStatusBadBufferSize;
    }

    /* 4. Lamp count in range.  This is what bounds the allocation. */
    if (request->LampCount == 0 || request->LampCount > CLA_MAX_LAMPS_PER_ARRAY)
    {
        return ClaStatusInvalidReport;
    }

    /* 5. The buffer must be exactly the size implied by the lamp count -- not
     *    at least, exactly.  Computed in 64-bit so a huge LampCount cannot
     *    wrap into a small expected size that a short buffer then satisfies. */
    const cla_u32 expected = ClaCreateRequestSizeForLampCount(request->LampCount);
    if (expected == 0 || expected != bufferLength)
    {
        return ClaStatusBadBufferSize;
    }

    /* 6. Strings must be NUL-terminated inside their fixed fields. */
    if (!ClaStringIsTerminated(request->DeviceName, CLA_MAX_NAME_CHARS) ||
        !ClaStringIsTerminated(request->SerialNumber, CLA_MAX_SERIAL_CHARS))
    {
        return ClaStatusInvalidReport;
    }

    /* 7. A bounding box of zero in any axis would make host-side effect
     *    positioning degenerate; the spec requires positive dimensions
     *    (HUT 1.4 s26.2: "Must be a positive offset from the origin"). */
    if (request->BoundingBoxWidthInMicrometers == 0 ||
        request->BoundingBoxHeightInMicrometers == 0 ||
        request->BoundingBoxDepthInMicrometers == 0)
    {
        return ClaStatusInvalidReport;
    }

    /* 8. Per-lamp sanity.  These values are handed straight back to Windows in
     *    LampAttributesResponseReport, and the parser indexes lamp state with
     *    ids derived from LampCount, so they must be coherent. */
    for (cla_u32 i = 0; i < request->LampCount; ++i)
    {
        const ClaLampInfo *lamp = &request->Lamps[i];

        /* IsProgrammable is a boolean field in the descriptor. */
        if (lamp->IsProgrammable > 1)
        {
            return ClaStatusInvalidReport;
        }

        /* A programmable lamp that can display nothing at all is a
         * contradiction and would make every colour update out of range. */
        if (lamp->IsProgrammable &&
            lamp->RedLevelCount == 0 && lamp->GreenLevelCount == 0 &&
            lamp->BlueLevelCount == 0 && lamp->IntensityLevelCount == 0)
        {
            return ClaStatusInvalidReport;
        }

        /* Lamp positions must sit inside the declared bounding box: Windows
         * uses them to place effects, and HUT 1.4 s26.7 defines them as
         * offsets from the box origin. */
        if (lamp->PositionXInMicrometers > request->BoundingBoxWidthInMicrometers ||
            lamp->PositionYInMicrometers > request->BoundingBoxHeightInMicrometers ||
            lamp->PositionZInMicrometers > request->BoundingBoxDepthInMicrometers)
        {
            return ClaStatusInvalidReport;
        }
    }

    return ClaStatusOk;
}

ClaStatus ClaValidateUpdateOutputBuffer(cla_u32 bufferLength, cla_u32 lampCount)
{
    const cla_u32 required = ClaUpdateMessageSizeForLampCount(lampCount);
    if (required == 0)
    {
        return ClaStatusInvalidArgument;
    }
    if (bufferLength < required)
    {
        return ClaStatusBadBufferSize;
    }
    return ClaStatusOk;
}
