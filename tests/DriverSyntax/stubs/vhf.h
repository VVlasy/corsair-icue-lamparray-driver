/* SPDX-License-Identifier: MIT
 * Stub <vhf.h>. Declarations transcribed from the VHF_CONFIG and
 * EVT_VHF_ASYNC_OPERATION documentation; see README.md in this directory. */
#ifndef CLA_STUB_VHF_H
#define CLA_STUB_VHF_H

#include "ntddk.h"

typedef PVOID VHFHANDLE;
typedef PVOID VHFOPERATIONHANDLE;

typedef struct _HID_XFER_PACKET
{
    PUCHAR reportBuffer;
    ULONG  reportBufferLen;
    UCHAR  reportId;
} HID_XFER_PACKET, *PHID_XFER_PACKET;

typedef VOID EVT_VHF_ASYNC_OPERATION(PVOID VhfClientContext,
                                     VHFOPERATIONHANDLE VhfOperationHandle,
                                     PVOID VhfOperationContext,
                                     PHID_XFER_PACKET HidTransferPacket);
typedef EVT_VHF_ASYNC_OPERATION *PEVT_VHF_ASYNC_OPERATION;

typedef VOID EVT_VHF_CLEANUP(PVOID VhfClientContext);
typedef EVT_VHF_CLEANUP *PEVT_VHF_CLEANUP;

typedef VOID EVT_VHF_READY_FOR_NEXT_READ_REPORT(PVOID VhfClientContext);
typedef EVT_VHF_READY_FOR_NEXT_READ_REPORT *PEVT_VHF_READY_FOR_NEXT_READ_REPORT;

typedef struct _VHF_CONFIG
{
    ULONG          Size;
    PVOID          VhfClientContext;
    ULONG          OperationContextSize;
    PDEVICE_OBJECT DeviceObject;
    USHORT         VendorID;
    USHORT         ProductID;
    USHORT         VersionNumber;
    GUID           ContainerID;
    USHORT         InstanceIDLength;
    PWSTR          InstanceID;
    USHORT         ReportDescriptorLength;
    PUCHAR         ReportDescriptor;
    PEVT_VHF_READY_FOR_NEXT_READ_REPORT EvtVhfReadyForNextReadReport;
    PEVT_VHF_ASYNC_OPERATION            EvtVhfAsyncOperationGetFeature;
    PEVT_VHF_ASYNC_OPERATION            EvtVhfAsyncOperationSetFeature;
    PEVT_VHF_ASYNC_OPERATION            EvtVhfAsyncOperationWriteReport;
    PEVT_VHF_ASYNC_OPERATION            EvtVhfAsyncOperationGetInputReport;
    PEVT_VHF_CLEANUP                    EvtVhfCleanup;
    USHORT         HardwareIDsLength;
    PWSTR          HardwareIDs;
} VHF_CONFIG, *PVHF_CONFIG;

#define VHF_CONFIG_INIT(config, deviceObject, descriptorLength, descriptor) \
    do {                                                                    \
        RtlZeroMemory((config), sizeof(VHF_CONFIG));                        \
        (config)->Size = sizeof(VHF_CONFIG);                                \
        (config)->DeviceObject = (deviceObject);                            \
        (config)->ReportDescriptorLength = (USHORT)(descriptorLength);      \
        (config)->ReportDescriptor = (descriptor);                          \
    } while (0)

EXTERN_C_START
NTSTATUS VhfCreate(PVHF_CONFIG config, VHFHANDLE *vhfHandle);
NTSTATUS VhfStart(VHFHANDLE vhfHandle);
void     VhfDelete(VHFHANDLE vhfHandle, BOOLEAN wait);
void     VhfAsyncOperationComplete(VHFOPERATIONHANDLE operationHandle,
                                   NTSTATUS completionStatus);
NTSTATUS VhfReadReportSubmit(VHFHANDLE vhfHandle, PVOID report);
EXTERN_C_END

#endif
