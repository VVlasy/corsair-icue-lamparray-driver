/*
 * Copyright (c) Corsair Dynamic Lighting Bridge contributors.
 * SPDX-License-Identifier: MIT
 *
 * Lifecycle of the virtual HID LampArray devices.
 *
 * One CorsairLampArray.sys instance hosts up to CLA_MAX_LAMP_ARRAYS virtual
 * devices, one per Corsair device the service found.  Each is an independent
 * VhfCreate handle with its own InstanceID and ContainerID, so Windows
 * enumerates them as separate Dynamic Lighting devices -- which is what the
 * HID model calls for: a keyboard and a mouse are not one LampArray, and
 * merging them would give Windows a bounding box describing nothing physical.
 */

#include "Driver.h"

#ifdef ALLOC_PRAGMA
#pragma alloc_text (PAGE, ClaLampArrayCreate)
#pragma alloc_text (PAGE, ClaLampArrayDestroy)
#pragma alloc_text (PAGE, ClaLampArrayDestroyAll)
#endif

/*
 * Derive a stable ContainerID from the device's serial string.
 *
 * It must be deterministic: Windows keys per-device Dynamic Lighting settings
 * off the container, so a fresh GUID on every service restart would make the
 * user's choices evaporate each reboot.  A 128-bit FNV-1a-derived value in a
 * fixed namespace gives the same GUID for the same Corsair device forever,
 * without needing any persistent state in the driver.
 */
static VOID
ClaDeriveContainerId(
    _In_reads_(CharCount) const WCHAR *Serial,
    _In_ ULONG CharCount,
    _Out_ GUID *ContainerId)
{
    /* Two independent FNV-1a streams give us 128 bits. */
    ULONG64 hashA = 0xCBF29CE484222325ull;
    ULONG64 hashB = 0x9E3779B97F4A7C15ull;

    for (ULONG i = 0; i < CharCount && Serial[i] != L'\0'; ++i)
    {
        const ULONG64 value = (ULONG64)Serial[i];
        hashA = (hashA ^ value) * 0x100000001B3ull;
        hashB = (hashB ^ (value + i)) * 0x100000001B3ull;
    }

    /* Namespace salt, so these can never collide with a real vendor GUID. */
    hashA ^= 0x436F727361697221ull;   /* "Corsair!" */
    hashB ^= 0x4C616D7041727221ull;   /* "LampArr!" */

    ContainerId->Data1 = (ULONG)(hashA >> 32);
    ContainerId->Data2 = (USHORT)(hashA >> 16);
    ContainerId->Data3 = (USHORT)(hashA);
    for (int i = 0; i < 8; ++i)
    {
        ContainerId->Data4[i] = (UCHAR)(hashB >> (8 * i));
    }

    /* Mark as RFC 4122 version 4 variant so it is a well-formed GUID. */
    ContainerId->Data3 = (USHORT)((ContainerId->Data3 & 0x0FFF) | 0x4000);
    ContainerId->Data4[0] = (UCHAR)((ContainerId->Data4[0] & 0x3F) | 0x80);
}

_Use_decl_annotations_
PCLA_LAMP_ARRAY_CONTEXT
ClaFindLampArrayLocked(
    PCLA_DEVICE_CONTEXT DeviceContext,
    ULONG LampArrayId)
{
    for (ULONG i = 0; i < CLA_MAX_LAMP_ARRAYS; ++i)
    {
        PCLA_LAMP_ARRAY_CONTEXT candidate = DeviceContext->LampArrays[i];
        if (candidate != NULL && candidate->LampArrayId == LampArrayId)
        {
            return candidate;
        }
    }
    return NULL;
}

static VOID
ClaFreeLampArray(_In_ PCLA_LAMP_ARRAY_CONTEXT LampArray)
{
    if (LampArray == NULL)
    {
        return;
    }
    if (LampArray->Lamps != NULL)
    {
        ExFreePoolWithTag(LampArray->Lamps, CLA_POOL_TAG_LAMPS);
    }
    if (LampArray->Colors != NULL)
    {
        ExFreePoolWithTag(LampArray->Colors, CLA_POOL_TAG_COLORS);
    }
    if (LampArray->PendingFrame != NULL)
    {
        ExFreePoolWithTag(LampArray->PendingFrame, CLA_POOL_TAG_COLORS);
    }
    if (LampArray->StateLock != NULL)
    {
        WdfObjectDelete(LampArray->StateLock);
    }
    ExFreePoolWithTag(LampArray, CLA_POOL_TAG_CONTEXT);
}

_Use_decl_annotations_
NTSTATUS
ClaLampArrayCreate(
    WDFDEVICE Device,
    const ClaLampArrayCreateRequest *Request,
    ULONG RequestLength,
    PULONG LampArrayId)
{
    PCLA_DEVICE_CONTEXT deviceContext = ClaGetDeviceContext(Device);
    PCLA_LAMP_ARRAY_CONTEXT lampArray = NULL;
    NTSTATUS status;

    /*
     * Everything this function uses is declared here, before the first goto.
     * The cleanup paths below jump forward to Cleanup, and in C++ a goto may
     * not cross the initialisation of a variable that is still in scope at the
     * label -- MSVC rejects it as C2362.  Declaring up front is the usual
     * shape for goto-based cleanup in driver code and keeps that legal.
     */
    WDF_OBJECT_ATTRIBUTES lockAttributes;
    ClaLampArrayAttributes attributes;
    VHF_CONFIG vhfConfig;
    WCHAR instanceId[CLA_MAX_SERIAL_CHARS];
    size_t instanceIdChars = 0;
    ULONG slot = CLA_MAX_LAMP_ARRAYS;
    ULONG lampCount = 0;

    PAGED_CODE();

    *LampArrayId = 0;

    /*
     * Revalidate here even though the IOCTL path already did.  This function
     * is the one that sizes allocations from Request->LampCount, so the check
     * that bounds that value belongs immediately next to the use -- not one
     * call frame away where a later refactor could separate them.
     */
    if (ClaValidateCreateRequest(Request, RequestLength) != ClaStatusOk)
    {
        return STATUS_INVALID_PARAMETER;
    }

    lampCount = Request->LampCount;

    lampArray = (PCLA_LAMP_ARRAY_CONTEXT)ExAllocatePool2(
        POOL_FLAG_NON_PAGED, sizeof(CLA_LAMP_ARRAY_CONTEXT), CLA_POOL_TAG_CONTEXT);
    if (lampArray == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    lampArray->Device = Device;
    lampArray->BackendAvailable = TRUE;

    /* NonPagedPoolNx throughout: the VHF callbacks may run at DISPATCH_LEVEL,
     * where a page fault would bugcheck. */
    lampArray->Lamps = (ClaLampInfo *)ExAllocatePool2(
        POOL_FLAG_NON_PAGED, (SIZE_T)lampCount * sizeof(ClaLampInfo), CLA_POOL_TAG_LAMPS);
    lampArray->Colors = (ClaLampColor *)ExAllocatePool2(
        POOL_FLAG_NON_PAGED, (SIZE_T)lampCount * sizeof(ClaLampColor), CLA_POOL_TAG_COLORS);
    lampArray->PendingFrame = (ClaLampColor *)ExAllocatePool2(
        POOL_FLAG_NON_PAGED, (SIZE_T)lampCount * sizeof(ClaLampColor), CLA_POOL_TAG_COLORS);

    if (lampArray->Lamps == NULL || lampArray->Colors == NULL ||
        lampArray->PendingFrame == NULL)
    {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto Cleanup;
    }

    lampArray->LampCount = lampCount;
    RtlCopyMemory(lampArray->Lamps, Request->Lamps,
                  (SIZE_T)lampCount * sizeof(ClaLampInfo));

    WDF_OBJECT_ATTRIBUTES_INIT(&lockAttributes);
    lockAttributes.ParentObject = Device;
    status = WdfSpinLockCreate(&lockAttributes, &lampArray->StateLock);
    if (!NT_SUCCESS(status))
    {
        goto Cleanup;
    }

    RtlZeroMemory(&attributes, sizeof(attributes));
    attributes.LampCount = (cla_u16)lampCount;
    attributes.BoundingBoxWidthInMicrometers = Request->BoundingBoxWidthInMicrometers;
    attributes.BoundingBoxHeightInMicrometers = Request->BoundingBoxHeightInMicrometers;
    attributes.BoundingBoxDepthInMicrometers = Request->BoundingBoxDepthInMicrometers;
    attributes.LampArrayKind = Request->LampArrayKind;
    attributes.MinUpdateIntervalInMicroseconds = Request->MinUpdateIntervalInMicroseconds;

    if (ClaLampArrayInit(&lampArray->State, &attributes,
                         lampArray->Lamps, lampArray->Colors) != ClaStatusOk)
    {
        status = STATUS_INVALID_PARAMETER;
        goto Cleanup;
    }

    /* Take a slot before creating the VHF device, so a callback that fires the
     * instant VhfStart returns can already find us. */
    WdfWaitLockAcquire(deviceContext->LampArrayLock, NULL);

    for (ULONG i = 0; i < CLA_MAX_LAMP_ARRAYS; ++i)
    {
        if (deviceContext->LampArrays[i] == NULL)
        {
            slot = i;
            break;
        }
    }
    if (slot == CLA_MAX_LAMP_ARRAYS)
    {
        WdfWaitLockRelease(deviceContext->LampArrayLock);
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto Cleanup;
    }

    lampArray->LampArrayId = deviceContext->NextLampArrayId++;
    deviceContext->LampArrays[slot] = lampArray;
    deviceContext->ActiveLampArrayCount++;

    WdfWaitLockRelease(deviceContext->LampArrayLock);

    /*
     * Build the virtual HID device.
     *
     * The report descriptor is the same 292 bytes for every LampArray: it
     * carries no lamp count (that is runtime data in report 1) and every
     * LampId field is 16 bits, so one descriptor serves any device.
     */
    VHF_CONFIG_INIT(&vhfConfig,
                    WdfDeviceWdmGetDeviceObject(Device),
                    CLA_LAMP_ARRAY_DESCRIPTOR_SIZE,
                    (PUCHAR)ClaLampArrayReportDescriptor);

    vhfConfig.VhfClientContext = lampArray;
    vhfConfig.VendorID = Request->VendorId;
    vhfConfig.ProductID = Request->ProductId;
    vhfConfig.VersionNumber = Request->VersionNumber;

    /* LampArray is a feature-report-only protocol (see the descriptor: no
     * Input or Output main items at all), so these two callbacks are the
     * entire host-facing surface.  No read-report path is needed. */
    vhfConfig.EvtVhfAsyncOperationGetFeature = ClaEvtVhfGetFeature;
    vhfConfig.EvtVhfAsyncOperationSetFeature = ClaEvtVhfSetFeature;
    vhfConfig.EvtVhfCleanup = ClaEvtVhfCleanup;

    /* Serial identifies this LampArray among the others we host. */
    RtlZeroMemory(instanceId, sizeof(instanceId));
    for (ULONG i = 0; i < CLA_MAX_SERIAL_CHARS - 1; ++i)
    {
        instanceId[i] = (WCHAR)Request->SerialNumber[i];
        if (instanceId[i] == L'\0')
        {
            break;
        }
    }
    instanceId[CLA_MAX_SERIAL_CHARS - 1] = L'\0';

    status = RtlStringCchLengthW(instanceId, CLA_MAX_SERIAL_CHARS, &instanceIdChars);
    if (NT_SUCCESS(status) && instanceIdChars > 0)
    {
        vhfConfig.InstanceID = instanceId;
        /* InstanceIDLength is a byte count and includes the terminator. */
        vhfConfig.InstanceIDLength =
            (USHORT)((instanceIdChars + 1) * sizeof(WCHAR));

        ClaDeriveContainerId(instanceId, (ULONG)instanceIdChars, &vhfConfig.ContainerID);
    }

    status = VhfCreate(&vhfConfig, &lampArray->VhfHandle);
    if (!NT_SUCCESS(status))
    {
        KdPrint(("CorsairLampArray: VhfCreate failed 0x%08X\n", status));
        goto RemoveFromTable;
    }

    status = VhfStart(lampArray->VhfHandle);
    if (!NT_SUCCESS(status))
    {
        KdPrint(("CorsairLampArray: VhfStart failed 0x%08X\n", status));
        VhfDelete(lampArray->VhfHandle, TRUE);
        lampArray->VhfHandle = NULL;
        goto RemoveFromTable;
    }

    lampArray->VhfStarted = TRUE;
    *LampArrayId = lampArray->LampArrayId;
    return STATUS_SUCCESS;

RemoveFromTable:
    WdfWaitLockAcquire(deviceContext->LampArrayLock, NULL);
    for (ULONG i = 0; i < CLA_MAX_LAMP_ARRAYS; ++i)
    {
        if (deviceContext->LampArrays[i] == lampArray)
        {
            deviceContext->LampArrays[i] = NULL;
            deviceContext->ActiveLampArrayCount--;
            break;
        }
    }
    WdfWaitLockRelease(deviceContext->LampArrayLock);

Cleanup:
    ClaFreeLampArray(lampArray);
    return status;
}

_Use_decl_annotations_
NTSTATUS
ClaLampArrayDestroy(
    WDFDEVICE Device,
    ULONG LampArrayId)
{
    PCLA_DEVICE_CONTEXT deviceContext = ClaGetDeviceContext(Device);
    PCLA_LAMP_ARRAY_CONTEXT lampArray = NULL;

    PAGED_CODE();

    /* Unlink under the lock, then do the blocking teardown outside it. */
    WdfWaitLockAcquire(deviceContext->LampArrayLock, NULL);

    for (ULONG i = 0; i < CLA_MAX_LAMP_ARRAYS; ++i)
    {
        PCLA_LAMP_ARRAY_CONTEXT candidate = deviceContext->LampArrays[i];
        if (candidate != NULL && candidate->LampArrayId == LampArrayId)
        {
            lampArray = candidate;
            deviceContext->LampArrays[i] = NULL;
            deviceContext->ActiveLampArrayCount--;
            break;
        }
    }

    WdfWaitLockRelease(deviceContext->LampArrayLock);

    if (lampArray == NULL)
    {
        return STATUS_NOT_FOUND;
    }

    /* Stop in-flight callbacks from publishing anything further. */
    WdfSpinLockAcquire(lampArray->StateLock);
    lampArray->Removing = TRUE;
    lampArray->PendingFrameValid = FALSE;
    WdfSpinLockRelease(lampArray->StateLock);

    if (lampArray->VhfHandle != NULL)
    {
        /* Wait: after this returns no further VHF callback can reference the
         * context, which is what makes the free below safe. */
        VhfDelete(lampArray->VhfHandle, TRUE);
        lampArray->VhfHandle = NULL;
    }

    ClaFreeLampArray(lampArray);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
VOID
ClaLampArrayDestroyAll(
    WDFDEVICE Device)
{
    PCLA_DEVICE_CONTEXT deviceContext = ClaGetDeviceContext(Device);

    PAGED_CODE();

    if (deviceContext == NULL || deviceContext->LampArrayLock == NULL)
    {
        return;
    }

    for (;;)
    {
        ULONG id = 0;

        WdfWaitLockAcquire(deviceContext->LampArrayLock, NULL);
        for (ULONG i = 0; i < CLA_MAX_LAMP_ARRAYS; ++i)
        {
            if (deviceContext->LampArrays[i] != NULL)
            {
                id = deviceContext->LampArrays[i]->LampArrayId;
                break;
            }
        }
        WdfWaitLockRelease(deviceContext->LampArrayLock);

        if (id == 0)
        {
            break;
        }
        ClaLampArrayDestroy(Device, id);
    }
}

_Use_decl_annotations_
VOID
ClaTryDeliverPendingUpdate(
    WDFDEVICE Device,
    PCLA_LAMP_ARRAY_CONTEXT LampArray)
{
    PCLA_DEVICE_CONTEXT deviceContext = ClaGetDeviceContext(Device);
    WDFREQUEST request = NULL;

    /*
     * Take a parked inverted-call request, if the service has one waiting.  If
     * it does not, we simply return: the frame stays in PendingFrame and the
     * next IOCTL_CLA_GET_NEXT_UPDATE collects it.  Nothing is lost except
     * superseded frames, which is the intent.
     */
    NTSTATUS status = WdfIoQueueRetrieveNextRequest(deviceContext->PendingUpdateQueue,
                                                    &request);
    if (!NT_SUCCESS(status) || request == NULL)
    {
        return;
    }

    ClaUpdateMessage *message = NULL;
    size_t outputLength = 0;
    status = WdfRequestRetrieveOutputBuffer(request,
                                            sizeof(ClaMessageHeader),
                                            (PVOID *)&message,
                                            &outputLength);
    if (!NT_SUCCESS(status))
    {
        WdfRequestComplete(request, status);
        return;
    }

    /*
     * Check the buffer BEFORE consuming anything.
     *
     * The frame is taken out of the pending slot under the lock below, and
     * that is destructive: once PendingFrameValid is cleared the frame is
     * gone.  Discovering only afterwards that the caller's buffer is too small
     * would lose it silently, which is the worst possible failure here because
     * the lighting would simply stop with nothing logged.
     *
     * LampCount is fixed at creation and never changes, so the largest message
     * this array can produce is known without holding the lock.
     */
    const ULONG maximumMessageSize = ClaUpdateMessageSizeForLampCount(LampArray->LampCount);
    if (maximumMessageSize == 0 || outputLength < maximumMessageSize)
    {
        WdfRequestComplete(request, STATUS_BUFFER_TOO_SMALL);
        return;
    }

    /*
     * Everything below happens under StateLock, including the colour copy.
     * The copy cannot be deferred until after the lock is dropped: a
     * concurrent SetFeature would overwrite PendingFrame mid-copy and the
     * service would see half of one frame and half of another.  At the
     * maximum lamp count the copy is 4KB, a few hundred nanoseconds, which is
     * acceptable to hold a spinlock for.
     */
    ClaUpdateKind kind = ClaUpdateKindNone;
    ULONG lampCount = 0;
    ULONG64 sequence = 0;
    UCHAR autonomousMode = 1;
    BOOLEAN haveFrame = FALSE;

    WdfSpinLockAcquire(LampArray->StateLock);

    if (LampArray->Removing)
    {
        kind = ClaUpdateKindRemoved;
    }
    else if (LampArray->PendingAutonomousModeChange)
    {
        LampArray->PendingAutonomousModeChange = FALSE;
        kind = ClaUpdateKindAutonomousMode;
        autonomousMode = LampArray->PendingAutonomousMode;
    }
    else if (LampArray->PendingFrameValid)
    {
        LampArray->PendingFrameValid = FALSE;
        kind = ClaUpdateKindLampColors;
        lampCount = LampArray->LampCount;
        sequence = LampArray->FrameSequence;
        autonomousMode = LampArray->State.AutonomousMode;
        haveFrame = TRUE;
        LampArray->FramesPublished++;
    }

    if (haveFrame)
    {
        /* Guaranteed to fit: outputLength was checked against this array's
         * maximum message size above, and lampCount is that same LampCount. */
        RtlCopyMemory(message->Colors, LampArray->PendingFrame,
                      (SIZE_T)lampCount * sizeof(ClaLampColor));
    }

    WdfSpinLockRelease(LampArray->StateLock);

    if (kind == ClaUpdateKindNone)
    {
        /* Nothing to report after all -- re-park the request rather than
         * completing it empty, so the service is not forced to spin. */
        status = WdfRequestForwardToIoQueue(request, deviceContext->PendingUpdateQueue);
        if (!NT_SUCCESS(status))
        {
            WdfRequestComplete(request, status);
        }
        return;
    }

    /*
     * For a colour frame this is the exact wire size.  For the state-change
     * kinds no colours are written, so the fixed part is all there is --
     * and that is offsetof(Colors), NOT sizeof(ClaUpdateMessage), which
     * includes trailing padding and would over-report the byte count to the
     * service.  Both are within outputLength, checked above.
     */
    const ULONG messageSize =
        haveFrame ? maximumMessageSize : ClaUpdateMessageFixedSize();

    message->Header.Version = CLA_IPC_PROTOCOL_VERSION;
    message->Header.Size = messageSize;
    message->LampArrayId = LampArray->LampArrayId;
    message->Kind = (cla_u32)kind;
    message->FrameSequence = sequence;
    message->AutonomousMode = autonomousMode;
    message->LampCount = lampCount;

    WdfRequestCompleteWithInformation(request, STATUS_SUCCESS, messageSize);
}
