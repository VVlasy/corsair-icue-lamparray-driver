/*
 * Copyright (c) Corsair Dynamic Lighting Bridge contributors.
 * SPDX-License-Identifier: MIT
 *
 * IOCTL dispatch -- the driver's boundary with the user-mode service.
 *
 * Everything arriving here is untrusted.  The pattern in every handler is the
 * same and is not varied:
 *
 *   1. WdfRequestRetrieveInput/OutputBuffer with a minimum length, so the
 *      framework itself rejects anything too small to contain a header.
 *   2. ClaValidate* from the core, which re-checks the version, the declared
 *      size against the actual length, and any embedded counts -- with the
 *      arithmetic done in 64 bits so a hostile count cannot overflow into a
 *      small "expected size".
 *   3. Only then read the body.
 *
 * All IOCTLs are METHOD_BUFFERED, so these are system buffers the framework
 * copied for us.  The driver never touches a user-mode address, and a caller
 * cannot alter the data after validation and before use.
 */

#include "Driver.h"

static NTSTATUS
ClaHandleGetDriverInfo(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _Out_ size_t *BytesReturned)
{
    PCLA_DEVICE_CONTEXT deviceContext = ClaGetDeviceContext(Device);
    ClaDriverInfo *info = NULL;
    size_t outputLength = 0;

    *BytesReturned = 0;

    NTSTATUS status = WdfRequestRetrieveOutputBuffer(
        Request, sizeof(ClaDriverInfo), (PVOID *)&info, &outputLength);
    if (!NT_SUCCESS(status))
    {
        return status;
    }
    if (outputLength < sizeof(ClaDriverInfo))
    {
        return STATUS_BUFFER_TOO_SMALL;
    }

    RtlZeroMemory(info, sizeof(*info));
    info->Header.Version = CLA_IPC_PROTOCOL_VERSION;
    info->Header.Size = sizeof(ClaDriverInfo);
    info->ProtocolVersion = CLA_IPC_PROTOCOL_VERSION;
    info->DriverVersionMajor = CLA_DRIVER_VERSION_MAJOR;
    info->DriverVersionMinor = CLA_DRIVER_VERSION_MINOR;
    info->DriverVersionPatch = CLA_DRIVER_VERSION_PATCH;
    info->MaxLampArrayCount = CLA_MAX_LAMP_ARRAYS;
    info->MaxLampsPerArray = CLA_MAX_LAMPS_PER_ARRAY;

    WdfWaitLockAcquire(deviceContext->LampArrayLock, NULL);
    info->ActiveLampArrayCount = deviceContext->ActiveLampArrayCount;
    WdfWaitLockRelease(deviceContext->LampArrayLock);

    *BytesReturned = sizeof(ClaDriverInfo);
    return STATUS_SUCCESS;
}

static NTSTATUS
ClaHandleCreateLampArray(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _Out_ size_t *BytesReturned)
{
    const ClaLampArrayCreateRequest *createRequest = NULL;
    ClaLampArrayCreateResponse *response = NULL;
    size_t actualInputLength = 0;
    size_t outputLength = 0;

    *BytesReturned = 0;

    NTSTATUS status = WdfRequestRetrieveInputBuffer(
        Request, sizeof(ClaMessageHeader), (PVOID *)&createRequest, &actualInputLength);
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    /* A length that does not fit in 32 bits cannot be a valid message, and
     * letting it reach the 32-bit validator would truncate it into one that
     * looks plausible. */
    if (actualInputLength > MAXULONG)
    {
        return STATUS_INVALID_BUFFER_SIZE;
    }

    status = WdfRequestRetrieveOutputBuffer(
        Request, sizeof(ClaLampArrayCreateResponse), (PVOID *)&response, &outputLength);
    if (!NT_SUCCESS(status))
    {
        return status;
    }
    if (outputLength < sizeof(ClaLampArrayCreateResponse))
    {
        return STATUS_BUFFER_TOO_SMALL;
    }

    /*
     * Validate against the length the framework actually gave us, never
     * against the length the caller claimed.
     */
    if (ClaValidateCreateRequest(createRequest, (cla_u32)actualInputLength) != ClaStatusOk)
    {
        return STATUS_INVALID_PARAMETER;
    }

    ULONG lampArrayId = 0;
    status = ClaLampArrayCreate(Device, createRequest, (ULONG)actualInputLength, &lampArrayId);
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    RtlZeroMemory(response, sizeof(*response));
    response->Header.Version = CLA_IPC_PROTOCOL_VERSION;
    response->Header.Size = sizeof(ClaLampArrayCreateResponse);
    response->LampArrayId = lampArrayId;

    *BytesReturned = sizeof(ClaLampArrayCreateResponse);
    return STATUS_SUCCESS;
}

/* Retrieve and validate a fixed-size input message of type T. */
#define CLA_RETRIEVE_FIXED_INPUT(Request, Type, Pointer)                       \
    do {                                                                       \
        size_t claLength_ = 0;                                                 \
        NTSTATUS claStatus_ = WdfRequestRetrieveInputBuffer(                   \
            (Request), sizeof(Type), (PVOID *)&(Pointer), &claLength_);        \
        if (!NT_SUCCESS(claStatus_)) return claStatus_;                        \
        if (claLength_ > MAXULONG) return STATUS_INVALID_BUFFER_SIZE;          \
        if (ClaValidateFixedMessage((Pointer), (cla_u32)claLength_,            \
                                    (cla_u32)sizeof(Type)) != ClaStatusOk)     \
            return STATUS_INVALID_PARAMETER;                                   \
    } while (0)

static NTSTATUS
ClaHandleDestroyLampArray(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request)
{
    const ClaLampArrayIdRequest *request = NULL;
    CLA_RETRIEVE_FIXED_INPUT(Request, ClaLampArrayIdRequest, request);

    return ClaLampArrayDestroy(Device, request->LampArrayId);
}

static NTSTATUS
ClaHandleSetDeviceState(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request)
{
    PCLA_DEVICE_CONTEXT deviceContext = ClaGetDeviceContext(Device);
    const ClaSetDeviceStateRequest *request = NULL;
    CLA_RETRIEVE_FIXED_INPUT(Request, ClaSetDeviceStateRequest, request);

    if (request->State > ClaDeviceStateAvailable)
    {
        return STATUS_INVALID_PARAMETER;
    }

    WdfWaitLockAcquire(deviceContext->LampArrayLock, NULL);
    PCLA_LAMP_ARRAY_CONTEXT lampArray =
        ClaFindLampArrayLocked(deviceContext, request->LampArrayId);

    NTSTATUS status = STATUS_NOT_FOUND;
    if (lampArray != NULL)
    {
        WdfSpinLockAcquire(lampArray->StateLock);
        lampArray->BackendAvailable = (request->State == ClaDeviceStateAvailable);
        if (!lampArray->BackendAvailable)
        {
            /*
             * The backend went away (iCUE closed, device unplugged).  Clear
             * the lamp state so that when it returns we do not resume from a
             * frame that is by then arbitrarily old.  Windows is not told the
             * device is gone -- the virtual device stays enumerated so that
             * settings and effects survive an iCUE restart, which is the whole
             * point of decoupling the two.
             */
            ClaLampArrayBlank(&lampArray->State);
            lampArray->PendingFrameValid = FALSE;
        }
        WdfSpinLockRelease(lampArray->StateLock);
        status = STATUS_SUCCESS;
    }
    WdfWaitLockRelease(deviceContext->LampArrayLock);

    return status;
}

static NTSTATUS
ClaHandleSetBrightness(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request)
{
    PCLA_DEVICE_CONTEXT deviceContext = ClaGetDeviceContext(Device);
    const ClaSetBrightnessRequest *request = NULL;
    CLA_RETRIEVE_FIXED_INPUT(Request, ClaSetBrightnessRequest, request);

    if (request->Brightness > 255)
    {
        return STATUS_INVALID_PARAMETER;
    }

    /*
     * Brightness is applied by the service when it translates a frame for the
     * Corsair SDK -- the HID LampArray protocol has no device-wide brightness
     * control, only the per-lamp intensity channel.  The driver's part is to
     * confirm the target exists and let the service read the value back.
     */
    WdfWaitLockAcquire(deviceContext->LampArrayLock, NULL);
    const BOOLEAN found =
        ClaFindLampArrayLocked(deviceContext, request->LampArrayId) != NULL;
    WdfWaitLockRelease(deviceContext->LampArrayLock);

    return found ? STATUS_SUCCESS : STATUS_NOT_FOUND;
}

static NTSTATUS
ClaHandleGetLampCount(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _Out_ size_t *BytesReturned)
{
    PCLA_DEVICE_CONTEXT deviceContext = ClaGetDeviceContext(Device);
    const ClaLampArrayIdRequest *request = NULL;
    ClaLampCountResponse *response = NULL;
    size_t outputLength = 0;

    *BytesReturned = 0;
    CLA_RETRIEVE_FIXED_INPUT(Request, ClaLampArrayIdRequest, request);

    NTSTATUS status = WdfRequestRetrieveOutputBuffer(
        Request, sizeof(ClaLampCountResponse), (PVOID *)&response, &outputLength);
    if (!NT_SUCCESS(status))
    {
        return status;
    }
    if (outputLength < sizeof(ClaLampCountResponse))
    {
        return STATUS_BUFFER_TOO_SMALL;
    }

    const ULONG lampArrayId = request->LampArrayId;

    WdfWaitLockAcquire(deviceContext->LampArrayLock, NULL);
    PCLA_LAMP_ARRAY_CONTEXT lampArray = ClaFindLampArrayLocked(deviceContext, lampArrayId);
    const ULONG lampCount = (lampArray != NULL) ? lampArray->LampCount : 0;
    const BOOLEAN found = (lampArray != NULL);
    WdfWaitLockRelease(deviceContext->LampArrayLock);

    if (!found)
    {
        return STATUS_NOT_FOUND;
    }

    RtlZeroMemory(response, sizeof(*response));
    response->Header.Version = CLA_IPC_PROTOCOL_VERSION;
    response->Header.Size = sizeof(ClaLampCountResponse);
    response->LampArrayId = lampArrayId;
    response->LampCount = lampCount;

    *BytesReturned = sizeof(ClaLampCountResponse);
    return STATUS_SUCCESS;
}

static NTSTATUS
ClaHandleGetLampInfo(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _Out_ size_t *BytesReturned)
{
    PCLA_DEVICE_CONTEXT deviceContext = ClaGetDeviceContext(Device);
    const ClaLampInfoRequest *request = NULL;
    ClaLampInfoResponse *response = NULL;
    size_t outputLength = 0;

    *BytesReturned = 0;
    CLA_RETRIEVE_FIXED_INPUT(Request, ClaLampInfoRequest, request);

    NTSTATUS status = WdfRequestRetrieveOutputBuffer(
        Request, sizeof(ClaLampInfoResponse), (PVOID *)&response, &outputLength);
    if (!NT_SUCCESS(status))
    {
        return status;
    }
    if (outputLength < sizeof(ClaLampInfoResponse))
    {
        return STATUS_BUFFER_TOO_SMALL;
    }

    const ULONG lampArrayId = request->LampArrayId;
    const ULONG lampId = request->LampId;

    RtlZeroMemory(response, sizeof(*response));

    WdfWaitLockAcquire(deviceContext->LampArrayLock, NULL);
    PCLA_LAMP_ARRAY_CONTEXT lampArray = ClaFindLampArrayLocked(deviceContext, lampArrayId);

    if (lampArray == NULL)
    {
        WdfWaitLockRelease(deviceContext->LampArrayLock);
        return STATUS_NOT_FOUND;
    }
    /* Bounds-check the caller's lamp id against this array's actual count
     * before indexing the table. */
    if (lampId >= lampArray->LampCount)
    {
        WdfWaitLockRelease(deviceContext->LampArrayLock);
        return STATUS_INVALID_PARAMETER;
    }

    response->Info = lampArray->Lamps[lampId];
    WdfWaitLockRelease(deviceContext->LampArrayLock);

    response->Header.Version = CLA_IPC_PROTOCOL_VERSION;
    response->Header.Size = sizeof(ClaLampInfoResponse);
    response->LampArrayId = lampArrayId;
    response->LampId = lampId;

    *BytesReturned = sizeof(ClaLampInfoResponse);
    return STATUS_SUCCESS;
}

static NTSTATUS
ClaHandleGetStatistics(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _Out_ size_t *BytesReturned)
{
    PCLA_DEVICE_CONTEXT deviceContext = ClaGetDeviceContext(Device);
    const ClaLampArrayIdRequest *request = NULL;
    ClaStatisticsResponse *response = NULL;
    size_t outputLength = 0;

    *BytesReturned = 0;
    CLA_RETRIEVE_FIXED_INPUT(Request, ClaLampArrayIdRequest, request);

    NTSTATUS status = WdfRequestRetrieveOutputBuffer(
        Request, sizeof(ClaStatisticsResponse), (PVOID *)&response, &outputLength);
    if (!NT_SUCCESS(status))
    {
        return status;
    }
    if (outputLength < sizeof(ClaStatisticsResponse))
    {
        return STATUS_BUFFER_TOO_SMALL;
    }

    const ULONG lampArrayId = request->LampArrayId;
    RtlZeroMemory(response, sizeof(*response));

    WdfWaitLockAcquire(deviceContext->LampArrayLock, NULL);
    PCLA_LAMP_ARRAY_CONTEXT lampArray = ClaFindLampArrayLocked(deviceContext, lampArrayId);

    if (lampArray == NULL)
    {
        WdfWaitLockRelease(deviceContext->LampArrayLock);
        return STATUS_NOT_FOUND;
    }

    WdfSpinLockAcquire(lampArray->StateLock);
    response->Counters = lampArray->State.Counters;
    response->FramesPublished = lampArray->FramesPublished;
    response->FramesCoalesced = lampArray->FramesCoalesced;
    WdfSpinLockRelease(lampArray->StateLock);

    WdfWaitLockRelease(deviceContext->LampArrayLock);

    response->Header.Version = CLA_IPC_PROTOCOL_VERSION;
    response->Header.Size = sizeof(ClaStatisticsResponse);
    response->LampArrayId = lampArrayId;

    *BytesReturned = sizeof(ClaStatisticsResponse);
    return STATUS_SUCCESS;
}

/*
 * The inverted call.
 *
 * The request is parked on the manual queue and completed later, when Windows
 * finishes a frame.  If a frame is already waiting it is delivered
 * immediately, so a service that was slow to re-arm does not miss one.
 */
static NTSTATUS
ClaHandleGetNextUpdate(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength)
{
    PCLA_DEVICE_CONTEXT deviceContext = ClaGetDeviceContext(Device);

    /*
     * Refuse a buffer that could not hold the largest frame we might produce.
     * Checking now, rather than at completion time, means the service learns
     * about its mistake straight away instead of silently never receiving
     * updates.
     */
    if (OutputBufferLength < sizeof(ClaUpdateMessage))
    {
        return STATUS_BUFFER_TOO_SMALL;
    }

    NTSTATUS status = WdfRequestForwardToIoQueue(Request, deviceContext->PendingUpdateQueue);
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    /* Now that a request is parked, flush anything already pending. */
    WdfWaitLockAcquire(deviceContext->LampArrayLock, NULL);
    for (ULONG i = 0; i < CLA_MAX_LAMP_ARRAYS; ++i)
    {
        PCLA_LAMP_ARRAY_CONTEXT lampArray = deviceContext->LampArrays[i];
        if (lampArray == NULL)
        {
            continue;
        }

        BOOLEAN pending;
        WdfSpinLockAcquire(lampArray->StateLock);
        pending = lampArray->PendingFrameValid || lampArray->PendingAutonomousModeChange;
        WdfSpinLockRelease(lampArray->StateLock);

        if (pending)
        {
            ClaTryDeliverPendingUpdate(Device, lampArray);
            break;
        }
    }
    WdfWaitLockRelease(deviceContext->LampArrayLock);

    /* STATUS_PENDING: the request now belongs to the manual queue and must not
     * be completed by this path. */
    return STATUS_PENDING;
}

_Use_decl_annotations_
VOID
ClaEvtIoDeviceControl(
    WDFQUEUE Queue,
    WDFREQUEST Request,
    size_t OutputBufferLength,
    size_t InputBufferLength,
    ULONG IoControlCode)
{
    WDFDEVICE device = WdfIoQueueGetDevice(Queue);
    size_t bytesReturned = 0;
    NTSTATUS status;

    /* Lengths are re-read from the request itself in each handler, since that
     * is the value the framework will actually honour. */
    UNREFERENCED_PARAMETER(InputBufferLength);

    switch (IoControlCode)
    {
    case IOCTL_CLA_GET_DRIVER_INFO:
        status = ClaHandleGetDriverInfo(device, Request, &bytesReturned);
        break;

    case IOCTL_CLA_CREATE_LAMP_ARRAY:
        status = ClaHandleCreateLampArray(device, Request, &bytesReturned);
        break;

    case IOCTL_CLA_DESTROY_LAMP_ARRAY:
        status = ClaHandleDestroyLampArray(device, Request);
        break;

    case IOCTL_CLA_GET_NEXT_UPDATE:
        status = ClaHandleGetNextUpdate(device, Request, OutputBufferLength);
        if (status == STATUS_PENDING)
        {
            /* Owned by the manual queue now; do not complete it here. */
            return;
        }
        break;

    case IOCTL_CLA_SET_DEVICE_STATE:
        status = ClaHandleSetDeviceState(device, Request);
        break;

    case IOCTL_CLA_SET_BRIGHTNESS:
        status = ClaHandleSetBrightness(device, Request);
        break;

    case IOCTL_CLA_GET_LAMP_COUNT:
        status = ClaHandleGetLampCount(device, Request, &bytesReturned);
        break;

    case IOCTL_CLA_GET_LAMP_INFO:
        status = ClaHandleGetLampInfo(device, Request, &bytesReturned);
        break;

    case IOCTL_CLA_GET_STATISTICS:
        status = ClaHandleGetStatistics(device, Request, &bytesReturned);
        break;

    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

    WdfRequestCompleteWithInformation(Request, status, bytesReturned);
}
