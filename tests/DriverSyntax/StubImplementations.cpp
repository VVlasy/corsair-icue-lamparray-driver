/*
 * Copyright (c) Corsair Dynamic Lighting Bridge contributors.
 * SPDX-License-Identifier: MIT
 *
 * Bodies for the stub kernel APIs, so the driver sources can be LINKED as well
 * as compiled.  Linking is what catches duplicate symbols across translation
 * units and functions that were declared but never defined -- neither of which
 * a compile-only check would find.
 *
 * Nothing here emulates the WDK.  See stubs/README.md.
 */

#include "stubs/ntddk.h"
#include "stubs/wdf.h"
#include "stubs/vhf.h"
#include "stubs/ntstrsafe.h"

#include <stdlib.h>

extern "C" {

PVOID ExAllocatePool2(unsigned long long, SIZE_T byteCount, ULONG)
{
    return calloc(1, byteCount ? byteCount : 1);
}

void ExFreePoolWithTag(PVOID p, ULONG) { free(p); }

NTSTATUS RtlStringCchLengthW(PCWSTR source, SIZE_T maxLength, SIZE_T *length)
{
    if (source == NULL || length == NULL) return STATUS_INVALID_PARAMETER;
    SIZE_T i = 0;
    while (i < maxLength && source[i] != 0) ++i;
    if (i >= maxLength) return STATUS_INVALID_PARAMETER;
    *length = i;
    return STATUS_SUCCESS;
}

NTSTATUS WdfDriverCreate(PDRIVER_OBJECT, PCUNICODE_STRING, PWDF_OBJECT_ATTRIBUTES,
                         PWDF_DRIVER_CONFIG, WDFDRIVER *) { return STATUS_SUCCESS; }
NTSTATUS WdfDeviceCreate(PWDFDEVICE_INIT *, PWDF_OBJECT_ATTRIBUTES, WDFDEVICE *)
{ return STATUS_SUCCESS; }
void     WdfDeviceInitSetIoType(PWDFDEVICE_INIT, WDF_DEVICE_IO_TYPE) {}
NTSTATUS WdfDeviceInitAssignSDDLString(PWDFDEVICE_INIT, const UNICODE_STRING *)
{ return STATUS_SUCCESS; }
NTSTATUS WdfDeviceCreateDeviceInterface(WDFDEVICE, const GUID *, PCUNICODE_STRING)
{ return STATUS_SUCCESS; }
PDEVICE_OBJECT WdfDeviceWdmGetDeviceObject(WDFDEVICE) { return NULL; }

NTSTATUS WdfIoQueueCreate(WDFDEVICE, PWDF_IO_QUEUE_CONFIG, PWDF_OBJECT_ATTRIBUTES, WDFQUEUE *)
{ return STATUS_SUCCESS; }
WDFDEVICE WdfIoQueueGetDevice(WDFQUEUE) { return NULL; }
NTSTATUS WdfIoQueueRetrieveNextRequest(WDFQUEUE, WDFREQUEST *) { return STATUS_NOT_FOUND; }

NTSTATUS WdfWaitLockCreate(PWDF_OBJECT_ATTRIBUTES, WDFWAITLOCK *) { return STATUS_SUCCESS; }
NTSTATUS WdfWaitLockAcquire(WDFWAITLOCK, LONGLONG *) { return STATUS_SUCCESS; }
void     WdfWaitLockRelease(WDFWAITLOCK) {}

NTSTATUS WdfSpinLockCreate(PWDF_OBJECT_ATTRIBUTES, WDFSPINLOCK *) { return STATUS_SUCCESS; }
void     WdfSpinLockAcquire(WDFSPINLOCK) {}
void     WdfSpinLockRelease(WDFSPINLOCK) {}

NTSTATUS WdfRequestRetrieveInputBuffer(WDFREQUEST, size_t, PVOID *, size_t *)
{ return STATUS_INVALID_PARAMETER; }
NTSTATUS WdfRequestRetrieveOutputBuffer(WDFREQUEST, size_t, PVOID *, size_t *)
{ return STATUS_INVALID_PARAMETER; }
void     WdfRequestComplete(WDFREQUEST, NTSTATUS) {}
void     WdfRequestCompleteWithInformation(WDFREQUEST, NTSTATUS, size_t) {}
void     WdfRequestSetInformation(WDFREQUEST, size_t) {}
NTSTATUS WdfRequestForwardToIoQueue(WDFREQUEST, WDFQUEUE) { return STATUS_SUCCESS; }

void     WdfObjectDelete(WDFOBJECT) {}

NTSTATUS VhfCreate(PVHF_CONFIG, VHFHANDLE *) { return STATUS_SUCCESS; }
NTSTATUS VhfStart(VHFHANDLE) { return STATUS_SUCCESS; }
void     VhfDelete(VHFHANDLE, BOOLEAN) {}
void     VhfAsyncOperationComplete(VHFOPERATIONHANDLE, NTSTATUS) {}
NTSTATUS VhfReadReportSubmit(VHFHANDLE, PVOID) { return STATUS_SUCCESS; }

} // extern "C"
