/*
 * Copyright (c) Corsair Dynamic Lighting Bridge contributors.
 * SPDX-License-Identifier: MIT
 *
 * Driver entry and PnP plumbing.
 */

#include "Driver.h"

#ifdef ALLOC_PRAGMA
#pragma alloc_text (INIT, DriverEntry)
#pragma alloc_text (PAGE, ClaEvtDeviceAdd)
#pragma alloc_text (PAGE, ClaEvtDriverContextCleanup)
#endif

_Use_decl_annotations_
NTSTATUS
DriverEntry(
    PDRIVER_OBJECT DriverObject,
    PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG config;
    WDF_OBJECT_ATTRIBUTES attributes;

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.EvtCleanupCallback = ClaEvtDriverContextCleanup;

    WDF_DRIVER_CONFIG_INIT(&config, ClaEvtDeviceAdd);

    const NTSTATUS status = WdfDriverCreate(DriverObject,
                                            RegistryPath,
                                            &attributes,
                                            &config,
                                            WDF_NO_HANDLE);
    if (!NT_SUCCESS(status))
    {
        KdPrint(("CorsairLampArray: WdfDriverCreate failed 0x%08X\n", status));
    }

    return status;
}

_Use_decl_annotations_
NTSTATUS
ClaEvtDeviceAdd(
    WDFDRIVER Driver,
    PWDFDEVICE_INIT DeviceInit)
{
    UNREFERENCED_PARAMETER(Driver);
    PAGED_CODE();

    return ClaCreateDevice(DeviceInit);
}

_Use_decl_annotations_
VOID
ClaEvtDriverContextCleanup(
    WDFOBJECT DriverObject)
{
    UNREFERENCED_PARAMETER(DriverObject);
    PAGED_CODE();
}
