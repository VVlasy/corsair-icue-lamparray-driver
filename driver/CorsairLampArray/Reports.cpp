/*
 * Copyright (c) Corsair Dynamic Lighting Bridge contributors.
 * SPDX-License-Identifier: MIT
 *
 * VHF feature-report callbacks.
 *
 * These are the only entry points Windows has into the driver.  The LampArray
 * report descriptor declares Feature reports and nothing else -- no Input, no
 * Output main items -- so GetFeature and SetFeature are the whole protocol.
 *
 * All parsing is delegated to the core state machine
 * (core/src/LampArrayDevice.cpp), which is compiled identically into the
 * user-mode test suite and exercised there against truncated, oversized and
 * randomised buffers.  This file's job is the plumbing: unwrap the transfer
 * packet, hold the right lock, and complete the operation.
 *
 * IRQL: EVT_VHF_ASYNC_OPERATION is documented as <= DISPATCH_LEVEL, so
 * everything here uses a spinlock and touches only non-paged memory.
 */

#include "Driver.h"

/*
 * Map a core status onto an NTSTATUS.
 *
 * ClaStatusInvalidReport means the report was well-formed but semantically
 * invalid, and HUT 1.4 s26.11 requires the device to ignore such a report in
 * its entirety.  It is reported as success: from the host's point of view the
 * device accepted the report and chose to do nothing, which is exactly what
 * the specification describes.  Failing the request instead would make Windows
 * treat the device as broken.  The rejection is still visible -- the core
 * bumps a counter that IOCTL_CLA_GET_STATISTICS exposes.
 */
static NTSTATUS
ClaMapStatus(ClaStatus status)
{
    switch (status)
    {
    case ClaStatusOk:                return STATUS_SUCCESS;
    case ClaStatusInvalidReport:     return STATUS_SUCCESS;
    case ClaStatusUnknownReportId:   return STATUS_INVALID_PARAMETER;
    case ClaStatusInvalidOperation:  return STATUS_NOT_SUPPORTED;
    case ClaStatusBadBufferSize:     return STATUS_INVALID_BUFFER_SIZE;
    case ClaStatusInvalidArgument:   return STATUS_INVALID_PARAMETER;
    default:                         return STATUS_UNSUCCESSFUL;
    }
}

/*
 * Recover the report id and validate the transfer buffer.
 *
 * HIDCLASS places the report id at reportBuffer[0] and counts it in
 * reportBufferLen when the descriptor uses report ids -- ours does.  This is
 * the documented behaviour of the HID class driver and is what Microsoft's own
 * KMDF minidriver sample relies on (it computes
 * `sizeof(payload) + sizeof(packet.reportId)` and reads the payload from
 * `reportBuffer + sizeof(reportId)`).
 *
 * packet->reportId is also supplied separately.  Where both are available they
 * must agree; a disagreement means something upstream is confused, and the
 * safe response is to refuse the operation rather than guess which to believe.
 */
static NTSTATUS
ClaResolveReportId(
    _In_ PHID_XFER_PACKET Packet,
    _Out_ UCHAR *ReportId)
{
    *ReportId = 0;

    if (Packet == NULL || Packet->reportBuffer == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }
    if (Packet->reportBufferLen < CLA_REPORT_ID_SIZE)
    {
        return STATUS_INVALID_BUFFER_SIZE;
    }

    const UCHAR fromBuffer = Packet->reportBuffer[0];
    const UCHAR fromPacket = Packet->reportId;

    if (fromPacket != 0 && fromBuffer != 0 && fromPacket != fromBuffer)
    {
        KdPrint(("CorsairLampArray: report id mismatch, packet %u buffer %u\n",
                 fromPacket, fromBuffer));
        return STATUS_INVALID_PARAMETER;
    }

    *ReportId = (fromPacket != 0) ? fromPacket : fromBuffer;
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
VOID
ClaEvtVhfGetFeature(
    PVOID VhfClientContext,
    VHFOPERATIONHANDLE VhfOperationHandle,
    PVOID VhfOperationContext,
    PHID_XFER_PACKET HidTransferPacket)
{
    UNREFERENCED_PARAMETER(VhfOperationContext);

    PCLA_LAMP_ARRAY_CONTEXT lampArray = (PCLA_LAMP_ARRAY_CONTEXT)VhfClientContext;
    UCHAR reportId = 0;

    NTSTATUS status = ClaResolveReportId(HidTransferPacket, &reportId);
    if (!NT_SUCCESS(status) || lampArray == NULL)
    {
        VhfAsyncOperationComplete(VhfOperationHandle,
                                  NT_SUCCESS(status) ? STATUS_INVALID_PARAMETER : status);
        return;
    }

    cla_u32 bytesWritten = 0;
    ClaStatus coreStatus;

    WdfSpinLockAcquire(lampArray->StateLock);

    if (lampArray->Removing)
    {
        coreStatus = ClaStatusInvalidOperation;
    }
    else
    {
        /*
         * The core writes the report id into buffer[0] itself and treats the
         * length as the whole transfer buffer, matching what HIDCLASS hands
         * us.  It refuses to write anything if the buffer is short.
         */
        coreStatus = ClaLampArrayGetFeature(&lampArray->State,
                                            reportId,
                                            HidTransferPacket->reportBuffer,
                                            HidTransferPacket->reportBufferLen,
                                            &bytesWritten);
    }

    WdfSpinLockRelease(lampArray->StateLock);

    VhfAsyncOperationComplete(VhfOperationHandle, ClaMapStatus(coreStatus));
}

_Use_decl_annotations_
VOID
ClaEvtVhfSetFeature(
    PVOID VhfClientContext,
    VHFOPERATIONHANDLE VhfOperationHandle,
    PVOID VhfOperationContext,
    PHID_XFER_PACKET HidTransferPacket)
{
    UNREFERENCED_PARAMETER(VhfOperationContext);

    PCLA_LAMP_ARRAY_CONTEXT lampArray = (PCLA_LAMP_ARRAY_CONTEXT)VhfClientContext;
    UCHAR reportId = 0;

    NTSTATUS status = ClaResolveReportId(HidTransferPacket, &reportId);
    if (!NT_SUCCESS(status) || lampArray == NULL)
    {
        VhfAsyncOperationComplete(VhfOperationHandle,
                                  NT_SUCCESS(status) ? STATUS_INVALID_PARAMETER : status);
        return;
    }

    ClaUpdateOutcome outcome;
    RtlZeroMemory(&outcome, sizeof(outcome));
    ClaStatus coreStatus;
    BOOLEAN shouldDeliver = FALSE;

    WdfSpinLockAcquire(lampArray->StateLock);

    if (lampArray->Removing)
    {
        coreStatus = ClaStatusInvalidOperation;
    }
    else
    {
        coreStatus = ClaLampArraySetFeature(&lampArray->State,
                                            reportId,
                                            HidTransferPacket->reportBuffer,
                                            HidTransferPacket->reportBufferLen,
                                            &outcome);

        if (coreStatus == ClaStatusOk)
        {
            if (outcome.FrameComplete)
            {
                /*
                 * A batch finished (the host set LampUpdateComplete), so the
                 * accumulated lamp state is now a coherent frame.  Snapshot it
                 * into the single pending slot.
                 *
                 * If a previous frame is still sitting there uncollected it is
                 * overwritten and counted as coalesced.  That is deliberate:
                 * the service only ever wants the newest frame, and queueing
                 * would add latency and unbounded memory for output nobody
                 * will see.  (Project Phase 13.)
                 */
                if (lampArray->PendingFrameValid)
                {
                    lampArray->FramesCoalesced++;
                }

                RtlCopyMemory(lampArray->PendingFrame,
                              lampArray->Colors,
                              (SIZE_T)lampArray->LampCount * sizeof(ClaLampColor));

                lampArray->FrameSequence++;
                lampArray->PendingFrameValid = TRUE;
                shouldDeliver = TRUE;
            }

            if (outcome.AutonomousModeChanged)
            {
                /*
                 * Windows took control, or handed it back.  The service needs
                 * to know so it can release the Corsair device back to iCUE
                 * when autonomous mode is re-enabled (HUT 1.4 s26.10.1).
                 */
                lampArray->PendingAutonomousModeChange = TRUE;
                lampArray->PendingAutonomousMode = outcome.AutonomousMode;
                shouldDeliver = TRUE;
            }
        }
    }

    WdfSpinLockRelease(lampArray->StateLock);

    /* Outside the lock: this completes a WDFREQUEST. */
    if (shouldDeliver)
    {
        ClaTryDeliverPendingUpdate(lampArray->Device, lampArray);
    }

    VhfAsyncOperationComplete(VhfOperationHandle, ClaMapStatus(coreStatus));
}

_Use_decl_annotations_
VOID
ClaEvtVhfCleanup(
    PVOID VhfClientContext)
{
    /*
     * Called by VHF once the virtual device is gone.  The context itself is
     * freed by ClaLampArrayDestroy after VhfDelete(..., TRUE) returns, so
     * there is nothing to release here; the callback is registered because
     * VHF requires it for asynchronous deletion and to make the ordering
     * explicit.
     */
    UNREFERENCED_PARAMETER(VhfClientContext);
}
