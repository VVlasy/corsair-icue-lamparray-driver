/*
 * Copyright (c) Corsair Dynamic Lighting Bridge contributors.
 * SPDX-License-Identifier: MIT
 *
 * CorsairLampArray.sys -- KMDF HID source driver that presents Corsair RGB
 * devices to Windows as HID LampArray devices via the Virtual HID Framework.
 *
 * Architecture
 * ------------
 *   Windows Dynamic Lighting
 *          | HID feature reports (page 0x59, report ids 1..6)
 *   Hidclass.sys / Mshidkmdf.sys
 *          |
 *   Vhf.sys                      (lower filter, installed by our INF)
 *          |
 *   CorsairLampArray.sys         (this driver, root-enumerated)
 *          | IOCTLs on GUID_DEVINTERFACE_CORSAIR_LAMP_ARRAY
 *   CorsairLampArrayService.exe
 *          |
 *   iCUESDK.dll -> iCUE -> Corsair hardware
 *
 * This driver deliberately knows nothing about Corsair.  It never links the
 * iCUE SDK, never talks to hardware, and contains no vendor protocol.  It is a
 * HID LampArray transport with an IPC channel, and everything Corsair-specific
 * lives in the user-mode service.
 *
 * Division of responsibility with the service
 * -------------------------------------------
 * The service owns the Corsair topology, so it pushes a complete, immutable
 * lamp table down at creation time.  The driver then answers every
 * LampArrayAttributes / LampAttributes request from its own cached copy.  Only
 * lamp *updates* travel up to user mode.  That means:
 *
 *   - the HID path never blocks on a user-mode round trip;
 *   - Windows can still interrogate the device while the service is
 *     restarting;
 *   - a slow or wedged service degrades lighting, not enumeration.
 *
 * Concurrency
 * -----------
 * EVT_VHF_ASYNC_OPERATION callbacks are documented as IRQL <= DISPATCH_LEVEL,
 * so per-LampArray state is guarded by a WDFSPINLOCK, never a wait lock, and
 * nothing that can block (pool allocation, VhfCreate/VhfDelete, request
 * completion) happens while it is held.  The list of LampArrays is separately
 * guarded by a WDFWAITLOCK because it is only touched from PASSIVE_LEVEL IOCTL
 * paths, where allocation is needed.
 */
#ifndef CORSAIR_LAMP_ARRAY_DRIVER_H
#define CORSAIR_LAMP_ARRAY_DRIVER_H

#include <ntddk.h>
#include <wdf.h>
#include <vhf.h>
#include <ntstrsafe.h>

#include "CorsairLampArrayIpc.h"
#include "cla/LampArrayDevice.h"
#include "cla/LampArrayDescriptor.h"
#include "cla/IpcContract.h"

EXTERN_C_START

/* Pool tags read as "aLC" / "lLC" / "cLC" in !poolused. */
#define CLA_POOL_TAG_CONTEXT  'cALC'
#define CLA_POOL_TAG_LAMPS    'lALC'
#define CLA_POOL_TAG_COLORS   'kALC'

#define CLA_DRIVER_VERSION_MAJOR 0
#define CLA_DRIVER_VERSION_MINOR 1
#define CLA_DRIVER_VERSION_PATCH 0

/*
 * Per virtual LampArray state.
 *
 * Allocated from NonPagedPoolNx: the VHF callbacks may run at DISPATCH_LEVEL,
 * so nothing they touch may be pageable.
 */
typedef struct _CLA_LAMP_ARRAY_CONTEXT
{
    /* Back pointer to the owning device. */
    WDFDEVICE Device;

    /* Assigned by the driver, returned to the service, used to address this
     * LampArray in later IOCTLs. */
    ULONG LampArrayId;

    /* The virtual HID device.  NULL until VhfCreate succeeds. */
    VHFHANDLE VhfHandle;
    BOOLEAN VhfStarted;

    /*
     * Guards State, Colors, PendingFrame and the counters.  A spinlock, not a
     * wait lock: VHF async callbacks are IRQL <= DISPATCH_LEVEL.
     */
    WDFSPINLOCK StateLock;

    /* Protocol state machine (core/). Lamps and Colors below are the caller-
     * owned arrays it points at. */
    ClaLampArrayState State;

    /* Immutable lamp table, pushed down by the service at creation. */
    ClaLampInfo *Lamps;
    ULONG LampCount;

    /* Live lamp colours the update reports accumulate into. */
    ClaLampColor *Colors;

    /*
     * The most recently completed frame, waiting to be handed to the service.
     *
     * Exactly one slot: if Windows completes another frame before the service
     * collects this one, the newer frame overwrites it and FramesCoalesced is
     * bumped.  Queueing frames would only add latency -- for RGB output a
     * superseded frame has no value, and dropping it is the correct behaviour,
     * not a compromise.  (Project Phase 13: colour-change coalescing and
     * cancellation of stale frames.)
     */
    ClaLampColor *PendingFrame;
    BOOLEAN PendingFrameValid;
    BOOLEAN PendingAutonomousModeChange;
    UCHAR PendingAutonomousMode;

    ULONG64 FrameSequence;
    ULONG64 FramesPublished;
    ULONG64 FramesCoalesced;

    /* Set once the service asks for teardown, so in-flight callbacks stop
     * publishing. */
    BOOLEAN Removing;

    /* Backend availability as last reported by the service. */
    BOOLEAN BackendAvailable;

} CLA_LAMP_ARRAY_CONTEXT, *PCLA_LAMP_ARRAY_CONTEXT;

/*
 * Per-WDFDEVICE context (the single root-enumerated software device).
 */
typedef struct _CLA_DEVICE_CONTEXT
{
    /* Guards the LampArrays table. PASSIVE_LEVEL only. */
    WDFWAITLOCK LampArrayLock;

    PCLA_LAMP_ARRAY_CONTEXT LampArrays[CLA_MAX_LAMP_ARRAYS];
    ULONG ActiveLampArrayCount;
    ULONG NextLampArrayId;

    /*
     * Manual queue holding the service's parked IOCTL_CLA_GET_NEXT_UPDATE
     * requests (the inverted call).  Manual dispatch because requests sit here
     * indefinitely until a frame completes.
     */
    WDFQUEUE PendingUpdateQueue;

} CLA_DEVICE_CONTEXT, *PCLA_DEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(CLA_DEVICE_CONTEXT, ClaGetDeviceContext)

/* Driver.cpp */
DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD ClaEvtDeviceAdd;
EVT_WDF_OBJECT_CONTEXT_CLEANUP ClaEvtDriverContextCleanup;

/* Device.cpp */
NTSTATUS ClaCreateDevice(_Inout_ PWDFDEVICE_INIT DeviceInit);
EVT_WDF_DEVICE_CONTEXT_CLEANUP ClaEvtDeviceCleanup;

/* Ioctl.cpp */
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL ClaEvtIoDeviceControl;

/* VhfDevice.cpp */
NTSTATUS ClaLampArrayCreate(
    _In_ WDFDEVICE Device,
    _In_reads_bytes_(RequestLength) const ClaLampArrayCreateRequest *Request,
    _In_ ULONG RequestLength,
    _Out_ PULONG LampArrayId);

NTSTATUS ClaLampArrayDestroy(_In_ WDFDEVICE Device, _In_ ULONG LampArrayId);
VOID ClaLampArrayDestroyAll(_In_ WDFDEVICE Device);

/* Caller must hold the device's LampArrayLock. */
PCLA_LAMP_ARRAY_CONTEXT ClaFindLampArrayLocked(_In_ PCLA_DEVICE_CONTEXT DeviceContext,
                                               _In_ ULONG LampArrayId);

/* Reports.cpp -- VHF feature report callbacks. */
EVT_VHF_ASYNC_OPERATION ClaEvtVhfGetFeature;
EVT_VHF_ASYNC_OPERATION ClaEvtVhfSetFeature;
EVT_VHF_CLEANUP ClaEvtVhfCleanup;

/*
 * Try to hand a pending frame to the service.
 *
 * Called after a completed frame or an autonomous-mode change.  Safe at
 * DISPATCH_LEVEL and safe to call when no request is parked; in that case the
 * frame stays in PendingFrame and is picked up by the next
 * IOCTL_CLA_GET_NEXT_UPDATE.  Must NOT be called with StateLock held.
 */
VOID ClaTryDeliverPendingUpdate(_In_ WDFDEVICE Device,
                                _In_ PCLA_LAMP_ARRAY_CONTEXT LampArray);

EXTERN_C_END

#endif /* CORSAIR_LAMP_ARRAY_DRIVER_H */
