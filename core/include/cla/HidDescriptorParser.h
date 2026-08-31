/*
 * Copyright (c) Corsair Dynamic Lighting Bridge contributors.
 * SPDX-License-Identifier: MIT
 *
 * A minimal HID report-descriptor walker.
 *
 * Its job is verification, not general-purpose HID parsing: it re-derives the
 * report sizes from the descriptor bytes so that the descriptor and the
 * hand-maintained CLA_PAYLOAD_SIZE_* constants can be checked against each
 * other by tools/DescriptorDump and by the unit tests.  Deriving the sizes
 * twice, independently, is what stops the two drifting apart.
 *
 * Kernel-safe: no CRT, no STL, no allocation.
 */
#ifndef CLA_HID_DESCRIPTOR_PARSER_H
#define CLA_HID_DESCRIPTOR_PARSER_H

#include "ClaTypes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* HID item types (HID 1.11 s6.2.2.2). */
#define CLA_HID_ITEM_TYPE_MAIN     0
#define CLA_HID_ITEM_TYPE_GLOBAL   1
#define CLA_HID_ITEM_TYPE_LOCAL    2
#define CLA_HID_ITEM_TYPE_RESERVED 3

/* Main item tags (HID 1.11 s6.2.2.4). */
#define CLA_HID_MAIN_INPUT           0x8
#define CLA_HID_MAIN_OUTPUT          0x9
#define CLA_HID_MAIN_COLLECTION      0xA
#define CLA_HID_MAIN_FEATURE         0xB
#define CLA_HID_MAIN_END_COLLECTION  0xC

/* Global item tags (HID 1.11 s6.2.2.7). */
#define CLA_HID_GLOBAL_USAGE_PAGE    0x0
#define CLA_HID_GLOBAL_LOGICAL_MIN   0x1
#define CLA_HID_GLOBAL_LOGICAL_MAX   0x2
#define CLA_HID_GLOBAL_PHYSICAL_MIN  0x3
#define CLA_HID_GLOBAL_PHYSICAL_MAX  0x4
#define CLA_HID_GLOBAL_UNIT_EXPONENT 0x5
#define CLA_HID_GLOBAL_UNIT          0x6
#define CLA_HID_GLOBAL_REPORT_SIZE   0x7
#define CLA_HID_GLOBAL_REPORT_ID     0x8
#define CLA_HID_GLOBAL_REPORT_COUNT  0x9
#define CLA_HID_GLOBAL_PUSH          0xA
#define CLA_HID_GLOBAL_POP           0xB

/* Local item tags (HID 1.11 s6.2.2.8). */
#define CLA_HID_LOCAL_USAGE          0x0
#define CLA_HID_LOCAL_USAGE_MIN      0x1
#define CLA_HID_LOCAL_USAGE_MAX      0x2

/* Most descriptors we care about use a handful of report ids. */
#define CLA_HID_MAX_REPORT_IDS 16
/* Guards against a malformed descriptor driving unbounded nesting. */
#define CLA_HID_MAX_COLLECTION_DEPTH 16

/* One decoded short item. */
typedef struct _ClaHidItem
{
    cla_u32 ByteOffset;   /* offset of the prefix byte within the descriptor */
    cla_u8  Type;         /* CLA_HID_ITEM_TYPE_* */
    cla_u8  Tag;          /* tag within that type */
    cla_u8  DataLength;   /* 0, 1, 2 or 4 */
    cla_u8  Reserved;
    cla_u32 Data;         /* raw little-endian data, zero-extended */
    cla_s32 SignedData;   /* same bytes, sign-extended */
} ClaHidItem;

/* Accumulated report sizes, in BITS, excluding the report id byte. */
typedef struct _ClaHidReportSizes
{
    cla_u32 ReportId;
    cla_u32 InputBits;
    cla_u32 OutputBits;
    cla_u32 FeatureBits;
} ClaHidReportSizes;

typedef struct _ClaHidDescriptorInfo
{
    ClaHidReportSizes Reports[CLA_HID_MAX_REPORT_IDS];
    cla_u32 ReportCount;
    cla_u32 ItemCount;
    cla_u32 TopLevelUsagePage;
    cla_u32 TopLevelUsage;
    cla_u32 MaxCollectionDepth;
    cla_u8  UsesReportIds;
} ClaHidDescriptorInfo;

/* Optional per-item callback; return 0 to stop the walk. */
typedef int (*ClaHidItemCallback)(const ClaHidItem *item, void *context);

/*
 * Walk a report descriptor, filling *info and invoking callback (may be null)
 * for each item.
 *
 * Returns ClaStatusInvalidReport if the descriptor is malformed: truncated
 * item, unbalanced collections, nesting past CLA_HID_MAX_COLLECTION_DEPTH,
 * long items (unsupported), or more than CLA_HID_MAX_REPORT_IDS report ids.
 */
ClaStatus ClaParseHidDescriptor(
    const cla_u8 *descriptor,
    cla_u32 descriptorLength,
    ClaHidDescriptorInfo *info,
    ClaHidItemCallback callback,
    void *callbackContext);

/* Feature-report payload size in BYTES for a report id, or 0 if absent.
 * Rounds up: a report is a whole number of bytes on the wire. */
cla_u32 ClaGetFeatureReportSizeBytes(const ClaHidDescriptorInfo *info, cla_u32 reportId);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CLA_HID_DESCRIPTOR_PARSER_H */
