/*
 * Copyright (c) Corsair Dynamic Lighting Bridge contributors.
 * SPDX-License-Identifier: MIT
 *
 * Fixed-width integer types usable from both kernel mode (WDK/MSVC) and
 * user mode (MSVC / GCC / Clang).  The protocol core is compiled into the
 * kernel driver as well as into user-mode tests, so it may not depend on
 * the C++ standard library, exceptions, RTTI or dynamic allocation.
 */
#ifndef CLA_TYPES_H
#define CLA_TYPES_H

#if defined(_KERNEL_MODE)

typedef unsigned __int8  cla_u8;
typedef unsigned __int16 cla_u16;
typedef unsigned __int32 cla_u32;
typedef signed   __int32 cla_s32;
typedef unsigned __int64 cla_u64;

#else /* user mode */

#include <stdint.h>
typedef uint8_t  cla_u8;
typedef uint16_t cla_u16;
typedef uint32_t cla_u32;
typedef int32_t  cla_s32;
typedef uint64_t cla_u64;

#endif

/* Result codes returned by the protocol core. Deliberately not NTSTATUS so
 * that the core stays free of Windows headers; the driver maps these onto
 * NTSTATUS in one place (see driver/CorsairLampArray/Reports.cpp). */
typedef enum _ClaStatus
{
    ClaStatusOk = 0,
    /* The report id is not one this device implements. */
    ClaStatusUnknownReportId = 1,
    /* Caller-supplied buffer is not exactly the size the descriptor declares. */
    ClaStatusBadBufferSize = 2,
    /* The report is well-formed but semantically invalid; per HUT 1.4 s26.11
     * the whole report must be ignored. */
    ClaStatusInvalidReport = 3,
    /* The operation is not valid for this report id (e.g. GET on an update
     * report, or SET on an attributes report). */
    ClaStatusInvalidOperation = 4,
    /* A required pointer argument was null. */
    ClaStatusInvalidArgument = 5
} ClaStatus;

#endif /* CLA_TYPES_H */
