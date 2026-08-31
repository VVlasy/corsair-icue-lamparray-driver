/*
 * Copyright (c) Corsair Dynamic Lighting Bridge contributors.
 * SPDX-License-Identifier: MIT
 *
 * Windows-specific half of the driver <-> service interface: the device
 * interface GUID and the IOCTL codes.
 *
 * The message structures and, more importantly, their validation live in
 * core/include/cla/IpcContract.h so that the driver's trust boundary can be
 * unit tested on any platform.  This header is included by the driver, the
 * service and the diagnostic tools.
 *
 * Threat model
 * ------------
 * The service is an ordinary user-mode process.  The driver treats every byte
 * it sends as untrusted and validates it with ClaValidate* before use.  The
 * device interface is additionally restricted by an SDDL string in the INF so
 * that only SYSTEM and Administrators can open it -- see
 * CorsairLampArray.inf and docs/architecture.md.
 */
#ifndef CORSAIR_LAMP_ARRAY_IPC_H
#define CORSAIR_LAMP_ARRAY_IPC_H

#include "cla/IpcContract.h"

#if defined(_KERNEL_MODE)
#  include <ntddk.h>
#  include <devioctl.h>
#else
#  include <windows.h>
#  include <winioctl.h>
#endif

#include <initguid.h>

/*
 * Device interface exposed by the driver and opened by the service.
 * {6B4F1F0C-2C1E-4A47-9E63-2F7B9C51A7D4}
 */
DEFINE_GUID(GUID_DEVINTERFACE_CORSAIR_LAMP_ARRAY,
    0x6b4f1f0c, 0x2c1e, 0x4a47, 0x9e, 0x63, 0x2f, 0x7b, 0x9c, 0x51, 0xa7, 0xd4);

/*
 * Hardware id of the root-enumerated software device the INF installs.
 * Matches CorsairLampArray.inf.
 */
#define CLA_DEVICE_HARDWARE_ID  L"ROOT\\CorsairLampArray"

/*
 * IOCTL codes.
 *
 * METHOD_BUFFERED throughout: the framework copies the caller's data into a
 * system buffer, so the driver never touches a user-mode address and a
 * hostile caller cannot swap the buffer out from under it mid-request.  This
 * is what the project requirement "Do not use arbitrary user-provided kernel
 * pointers" comes down to in practice.
 *
 * Function codes start at 0x800: codes below that are reserved by Microsoft.
 */
#define CLA_IOCTL_BASE 0x800

#define CLA_MAKE_IOCTL(function, access) \
    CTL_CODE(FILE_DEVICE_UNKNOWN, (CLA_IOCTL_BASE + (function)), METHOD_BUFFERED, (access))

/* GET_DEVICE_INFO: protocol version and driver limits.  The service calls
 * this first and refuses to continue on a version mismatch. */
#define IOCTL_CLA_GET_DRIVER_INFO      CLA_MAKE_IOCTL(0x00, FILE_READ_ACCESS)

/* Create a virtual LampArray.  Input is a ClaLampArrayCreateRequest with the
 * full lamp table appended; output is a ClaLampArrayCreateResponse carrying
 * the assigned id. */
#define IOCTL_CLA_CREATE_LAMP_ARRAY    CLA_MAKE_IOCTL(0x01, FILE_READ_ACCESS | FILE_WRITE_ACCESS)

/* Destroy one virtual LampArray.  Input is a ClaLampArrayIdRequest. */
#define IOCTL_CLA_DESTROY_LAMP_ARRAY   CLA_MAKE_IOCTL(0x02, FILE_WRITE_ACCESS)

/*
 * Inverted call: the service parks these on the driver and the driver
 * completes one whenever Windows finishes a lighting frame.  Output is a
 * ClaUpdateMessage; the buffer must be large enough for a full frame.
 *
 * The service should keep two or three of these outstanding so there is
 * always one available the instant a frame completes.
 */
#define IOCTL_CLA_GET_NEXT_UPDATE      CLA_MAKE_IOCTL(0x03, FILE_READ_ACCESS)

/* DEVICE_STATE_CHANGED: the service reports backend availability, e.g. iCUE
 * exited or the Corsair device was unplugged. */
#define IOCTL_CLA_SET_DEVICE_STATE     CLA_MAKE_IOCTL(0x04, FILE_WRITE_ACCESS)

/* SET_BRIGHTNESS: a service-side output scale.  See IpcContract.h for why
 * this is not a HID-level control. */
#define IOCTL_CLA_SET_BRIGHTNESS       CLA_MAKE_IOCTL(0x05, FILE_WRITE_ACCESS)

/* Diagnostics, used by tools/HidTestClient. */
#define IOCTL_CLA_GET_LAMP_COUNT       CLA_MAKE_IOCTL(0x06, FILE_READ_ACCESS)
#define IOCTL_CLA_GET_LAMP_INFO        CLA_MAKE_IOCTL(0x07, FILE_READ_ACCESS)
#define IOCTL_CLA_GET_STATISTICS       CLA_MAKE_IOCTL(0x08, FILE_READ_ACCESS)

#endif /* CORSAIR_LAMP_ARRAY_IPC_H */
