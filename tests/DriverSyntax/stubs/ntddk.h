/* SPDX-License-Identifier: MIT
 * Stub <ntddk.h>. See README.md in this directory. */
#ifndef CLA_STUB_NTDDK_H
#define CLA_STUB_NTDDK_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

#include "sal.h"

#ifdef __cplusplus
#define EXTERN_C_START extern "C" {
#define EXTERN_C_END   }
#define EXTERN_C       extern "C"
#else
#define EXTERN_C_START
#define EXTERN_C_END
#define EXTERN_C
#endif

typedef int             BOOL;
typedef unsigned char   BOOLEAN;
typedef unsigned char   UCHAR;
typedef unsigned char  *PUCHAR;
typedef char            CHAR;
typedef unsigned short  USHORT;
/* wchar_t rather than unsigned short, so L"..." literals compile.  On Windows
 * wchar_t is 2 bytes as WCHAR is; under GCC it is 4, so string SIZES differ
 * here from the real thing.  That is acceptable for a syntax check and is the
 * one place these stubs knowingly diverge -- see README.md. */
typedef wchar_t         WCHAR;
typedef WCHAR          *PWSTR;
typedef const WCHAR    *PCWSTR;
typedef unsigned int    ULONG;
typedef unsigned int   *PULONG;
typedef unsigned long long ULONG64;
typedef long long       LONGLONG;
typedef int             LONG;
typedef void           *PVOID;
typedef void            VOID;
typedef size_t          SIZE_T;
typedef long            NTSTATUS;

#define MAXULONG    0xFFFFFFFFu
#define TRUE        1
#define FALSE       0
#ifndef NULL
#define NULL        0
#endif

#define NT_SUCCESS(status) (((NTSTATUS)(status)) >= 0)

#define STATUS_SUCCESS                  ((NTSTATUS)0x00000000L)
#define STATUS_UNSUCCESSFUL             ((NTSTATUS)0xC0000001L)
#define STATUS_NOT_SUPPORTED            ((NTSTATUS)0xC00000BBL)
#define STATUS_INVALID_PARAMETER        ((NTSTATUS)0xC000000DL)
#define STATUS_INVALID_DEVICE_REQUEST   ((NTSTATUS)0xC0000010L)
#define STATUS_INSUFFICIENT_RESOURCES   ((NTSTATUS)0xC000009AL)
#define STATUS_BUFFER_TOO_SMALL         ((NTSTATUS)0xC0000023L)
#define STATUS_INVALID_BUFFER_SIZE      ((NTSTATUS)0xC0000206L)
#define STATUS_NOT_FOUND                ((NTSTATUS)0xC0000225L)
#define STATUS_PENDING                  ((NTSTATUS)0x00000103L)

typedef struct _GUID
{
    ULONG  Data1;
    USHORT Data2;
    USHORT Data3;
    UCHAR  Data4[8];
} GUID;

typedef struct _UNICODE_STRING
{
    USHORT Length;
    USHORT MaximumLength;
    PWSTR  Buffer;
} UNICODE_STRING, *PUNICODE_STRING;

typedef const UNICODE_STRING *PCUNICODE_STRING;

typedef struct _DRIVER_OBJECT DRIVER_OBJECT, *PDRIVER_OBJECT;
typedef struct _DEVICE_OBJECT DEVICE_OBJECT, *PDEVICE_OBJECT;

typedef NTSTATUS DRIVER_INITIALIZE(PDRIVER_OBJECT, PUNICODE_STRING);

#define PAGED_CODE()            ((void)0)
#define UNREFERENCED_PARAMETER(x) ((void)(x))
#define FIELD_OFFSET(type, field) ((ULONG)(size_t)&(((type *)0)->field))

#define DECLARE_CONST_UNICODE_STRING(name, text) \
    static const WCHAR name##_buffer[] = text;   \
    static const UNICODE_STRING name = {         \
        (USHORT)(sizeof(text) / sizeof(WCHAR) - 1) * 2, \
        (USHORT)sizeof(text), (PWSTR)name##_buffer }

/* KdPrint takes a doubly-parenthesised printf argument list. */
#define KdPrint(args) ((void)0)

#define RtlZeroMemory(destination, length) memset((destination), 0, (length))
#define RtlCopyMemory(destination, source, length) memcpy((destination), (source), (length))

typedef ULONG POOL_FLAGS;
#define POOL_FLAG_NON_PAGED 0x0000000000000040ULL

EXTERN_C_START
PVOID ExAllocatePool2(unsigned long long flags, SIZE_T byteCount, ULONG tag);
void  ExFreePoolWithTag(PVOID p, ULONG tag);
EXTERN_C_END

/* CTL_CODE and friends, normally from devioctl.h. */
#define FILE_DEVICE_UNKNOWN 0x00000022
#define METHOD_BUFFERED     0
#define FILE_ANY_ACCESS     0
#define FILE_READ_ACCESS    0x0001
#define FILE_WRITE_ACCESS   0x0002

#define CTL_CODE(deviceType, function, method, access) \
    (((deviceType) << 16) | ((access) << 14) | ((function) << 2) | (method))

/* guiddef.h behaviour: a declaration unless INITGUID is defined. */
#ifdef INITGUID
#define DEFINE_GUID(name, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8) \
    EXTERN_C const GUID name = { l, w1, w2, { b1, b2, b3, b4, b5, b6, b7, b8 } }
#else
#define DEFINE_GUID(name, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8) \
    EXTERN_C const GUID name
#endif

#endif
