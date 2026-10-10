/*
 * Copyright (c) Corsair Dynamic Lighting Bridge contributors.
 * SPDX-License-Identifier: MIT
 *
 * Creation of the root-enumerated software device the service talks to.
 */

/* Instantiates GUID_DEVINTERFACE_CORSAIR_LAMP_ARRAY for the driver binary.
 * Must appear before Driver.h, and in exactly one translation unit -- see the
 * note in inc/CorsairLampArrayIpc.h. */
#include <initguid.h>

#include "Driver.h"

#ifdef ALLOC_PRAGMA
#pragma alloc_text (PAGE, ClaCreateDevice)
#pragma alloc_text (PAGE, ClaEvtDeviceCleanup)
#endif

_Use_decl_annotations_
NTSTATUS
ClaCreateDevice(
    PWDFDEVICE_INIT DeviceInit)
{
    WDF_OBJECT_ATTRIBUTES deviceAttributes;
    WDF_IO_QUEUE_CONFIG queueConfig;
    WDFDEVICE device = NULL;
    PCLA_DEVICE_CONTEXT deviceContext = NULL;
    NTSTATUS status;

    PAGED_CODE();

    /*
     * The service issues IOCTLs, and the inverted-call requests sit parked in
     * a manual queue for as long as Windows takes to send a frame.  Buffered
     * I/O means the framework copies caller data into a system buffer, so the
     * driver never dereferences a user-mode address.
     */
    WdfDeviceInitSetIoType(DeviceInit, WdfDeviceIoBuffered);

    /*
     * Only SYSTEM and Administrators may open the device.  The service runs as
     * LocalSystem; nothing else has any business creating virtual HID devices.
     * This is defence in depth -- the driver still validates every message --
     * but it keeps the attack surface off unprivileged code entirely.
     *
     * D:P(A;;GA;;;SY)(A;;GA;;;BA)
     *   protected DACL, generic-all for LocalSystem and Builtin Administrators.
     */
    DECLARE_CONST_UNICODE_STRING(sddl, L"D:P(A;;GA;;;SY)(A;;GA;;;BA)");
    status = WdfDeviceInitAssignSDDLString(DeviceInit, &sddl);
    if (!NT_SUCCESS(status))
    {
        KdPrint(("CorsairLampArray: WdfDeviceInitAssignSDDLString failed 0x%08X\n", status));
        return status;
    }

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&deviceAttributes, CLA_DEVICE_CONTEXT);
    deviceAttributes.EvtCleanupCallback = ClaEvtDeviceCleanup;

    status = WdfDeviceCreate(&DeviceInit, &deviceAttributes, &device);
    if (!NT_SUCCESS(status))
    {
        KdPrint(("CorsairLampArray: WdfDeviceCreate failed 0x%08X\n", status));
        return status;
    }

    deviceContext = ClaGetDeviceContext(device);
    RtlZeroMemory(deviceContext, sizeof(*deviceContext));
    deviceContext->NextLampArrayId = 1;

    WDF_OBJECT_ATTRIBUTES lockAttributes;
    WDF_OBJECT_ATTRIBUTES_INIT(&lockAttributes);
    lockAttributes.ParentObject = device;
    status = WdfWaitLockCreate(&lockAttributes, &deviceContext->LampArrayLock);
    if (!NT_SUCCESS(status))
    {
        KdPrint(("CorsairLampArray: WdfWaitLockCreate failed 0x%08X\n", status));
        return status;
    }

    /* Default queue: IOCTL dispatch.  Sequential so the LampArray table is
     * mutated by one request at a time without extra locking in the IOCTL
     * paths. */
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchSequential);
    queueConfig.EvtIoDeviceControl = ClaEvtIoDeviceControl;

    WDFQUEUE defaultQueue;
    status = WdfIoQueueCreate(device, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, &defaultQueue);
    if (!NT_SUCCESS(status))
    {
        KdPrint(("CorsairLampArray: default WdfIoQueueCreate failed 0x%08X\n", status));
        return status;
    }

    /*
     * Manual queue for the inverted call.  Requests park here until a frame is
     * ready; the driver retrieves and completes them from
     * ClaTryDeliverPendingUpdate, which can run at DISPATCH_LEVEL.
     */
    WDF_IO_QUEUE_CONFIG_INIT(&queueConfig, WdfIoQueueDispatchManual);
    status = WdfIoQueueCreate(device, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES,
                              &deviceContext->PendingUpdateQueue);
    if (!NT_SUCCESS(status))
    {
        KdPrint(("CorsairLampArray: pending WdfIoQueueCreate failed 0x%08X\n", status));
        return status;
    }

    status = WdfDeviceCreateDeviceInterface(device,
                                            &GUID_DEVINTERFACE_CORSAIR_LAMP_ARRAY,
                                            NULL);
    if (!NT_SUCCESS(status))
    {
        KdPrint(("CorsairLampArray: WdfDeviceCreateDeviceInterface failed 0x%08X\n", status));
        return status;
    }

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
VOID
ClaEvtDeviceCleanup(
    WDFOBJECT Object)
{
    PAGED_CODE();

    /*
     * Tear down every virtual HID device we created.  VhfDelete must be called
     * at PASSIVE_LEVEL when waiting, which is satisfied here.
     */
    ClaLampArrayDestroyAll((WDFDEVICE)Object);
}
