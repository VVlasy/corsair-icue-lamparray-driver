/* SPDX-License-Identifier: MIT */
#ifndef CLA_STUB_NTSTRSAFE_H
#define CLA_STUB_NTSTRSAFE_H
#include "ntddk.h"
EXTERN_C_START
NTSTATUS RtlStringCchLengthW(PCWSTR source, SIZE_T maxLength, SIZE_T *length);
EXTERN_C_END
#endif
