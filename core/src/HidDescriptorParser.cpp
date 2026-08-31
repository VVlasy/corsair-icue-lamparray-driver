/*
 * Copyright (c) Corsair Dynamic Lighting Bridge contributors.
 * SPDX-License-Identifier: MIT
 *
 * Minimal HID report-descriptor walker.  See HidDescriptorParser.h.
 */

#include "cla/HidDescriptorParser.h"

/* Global item state that Push/Pop save and restore (HID 1.11 s6.2.2.7). */
typedef struct _ClaGlobalState
{
    cla_u32 UsagePage;
    cla_u32 ReportSize;
    cla_u32 ReportCount;
    cla_u32 ReportId;
} ClaGlobalState;

static ClaHidReportSizes *ClaFindOrAddReport(ClaHidDescriptorInfo *info, cla_u32 reportId)
{
    for (cla_u32 i = 0; i < info->ReportCount; ++i)
    {
        if (info->Reports[i].ReportId == reportId)
        {
            return &info->Reports[i];
        }
    }
    if (info->ReportCount >= CLA_HID_MAX_REPORT_IDS)
    {
        return 0;
    }
    ClaHidReportSizes *slot = &info->Reports[info->ReportCount++];
    slot->ReportId = reportId;
    slot->InputBits = 0;
    slot->OutputBits = 0;
    slot->FeatureBits = 0;
    return slot;
}

ClaStatus ClaParseHidDescriptor(
    const cla_u8 *descriptor,
    cla_u32 descriptorLength,
    ClaHidDescriptorInfo *info,
    ClaHidItemCallback callback,
    void *callbackContext)
{
    if (descriptor == 0 || info == 0)
    {
        return ClaStatusInvalidArgument;
    }

    /* Zero the output up front so a failed parse cannot leave stale data. */
    {
        cla_u8 *p = (cla_u8 *)info;
        for (cla_u32 i = 0; i < (cla_u32)sizeof(*info); ++i) p[i] = 0;
    }

    ClaGlobalState global;
    global.UsagePage = 0;
    global.ReportSize = 0;
    global.ReportCount = 0;
    global.ReportId = 0;

    ClaGlobalState stack[CLA_HID_MAX_COLLECTION_DEPTH];
    cla_u32 stackDepth = 0;

    cla_u32 collectionDepth = 0;
    cla_u32 offset = 0;
    /* First Usage seen at top level, captured before any collection opens. */
    cla_u32 pendingUsage = 0;
    cla_u8 haveTopLevel = 0;

    while (offset < descriptorLength)
    {
        const cla_u32 itemOffset = offset;
        const cla_u8 prefix = descriptor[offset++];

        /* Long items (HID 1.11 s6.2.2.3) are not used by any descriptor we
         * ship; treat them as malformed rather than silently skipping. */
        if (prefix == 0xFE)
        {
            return ClaStatusInvalidReport;
        }

        const cla_u8 sizeCode = (cla_u8)(prefix & 0x03u);
        const cla_u8 type = (cla_u8)((prefix >> 2) & 0x03u);
        const cla_u8 tag = (cla_u8)((prefix >> 4) & 0x0Fu);
        const cla_u8 dataLength = (cla_u8)(sizeCode == 3 ? 4 : sizeCode);

        if (offset + dataLength > descriptorLength)
        {
            return ClaStatusInvalidReport;   /* truncated item */
        }

        cla_u32 data = 0;
        for (cla_u8 i = 0; i < dataLength; ++i)
        {
            data |= ((cla_u32)descriptor[offset + i]) << (8u * i);
        }
        offset += dataLength;

        cla_s32 signedData = (cla_s32)data;
        if (dataLength == 1 && (data & 0x80u))        signedData = (cla_s32)(data | 0xFFFFFF00u);
        else if (dataLength == 2 && (data & 0x8000u)) signedData = (cla_s32)(data | 0xFFFF0000u);

        info->ItemCount++;

        if (callback != 0)
        {
            ClaHidItem item;
            item.ByteOffset = itemOffset;
            item.Type = type;
            item.Tag = tag;
            item.DataLength = dataLength;
            item.Reserved = 0;
            item.Data = data;
            item.SignedData = signedData;
            if (callback(&item, callbackContext) == 0)
            {
                return ClaStatusOk;
            }
        }

        if (type == CLA_HID_ITEM_TYPE_GLOBAL)
        {
            switch (tag)
            {
            case CLA_HID_GLOBAL_USAGE_PAGE:   global.UsagePage = data; break;
            case CLA_HID_GLOBAL_REPORT_SIZE:  global.ReportSize = data; break;
            case CLA_HID_GLOBAL_REPORT_COUNT: global.ReportCount = data; break;
            case CLA_HID_GLOBAL_REPORT_ID:
                global.ReportId = data;
                info->UsesReportIds = 1;
                break;
            case CLA_HID_GLOBAL_PUSH:
                if (stackDepth >= CLA_HID_MAX_COLLECTION_DEPTH) return ClaStatusInvalidReport;
                stack[stackDepth++] = global;
                break;
            case CLA_HID_GLOBAL_POP:
                if (stackDepth == 0) return ClaStatusInvalidReport;
                global = stack[--stackDepth];
                break;
            default: break;
            }
        }
        else if (type == CLA_HID_ITEM_TYPE_LOCAL)
        {
            if (tag == CLA_HID_LOCAL_USAGE && pendingUsage == 0)
            {
                pendingUsage = data;
            }
        }
        else if (type == CLA_HID_ITEM_TYPE_MAIN)
        {
            switch (tag)
            {
            case CLA_HID_MAIN_COLLECTION:
                if (!haveTopLevel)
                {
                    info->TopLevelUsagePage = global.UsagePage;
                    info->TopLevelUsage = pendingUsage;
                    haveTopLevel = 1;
                }
                collectionDepth++;
                if (collectionDepth > CLA_HID_MAX_COLLECTION_DEPTH)
                {
                    return ClaStatusInvalidReport;
                }
                if (collectionDepth > info->MaxCollectionDepth)
                {
                    info->MaxCollectionDepth = collectionDepth;
                }
                break;

            case CLA_HID_MAIN_END_COLLECTION:
                if (collectionDepth == 0)
                {
                    return ClaStatusInvalidReport;   /* unbalanced */
                }
                collectionDepth--;
                break;

            case CLA_HID_MAIN_INPUT:
            case CLA_HID_MAIN_OUTPUT:
            case CLA_HID_MAIN_FEATURE:
            {
                ClaHidReportSizes *slot = ClaFindOrAddReport(info, global.ReportId);
                if (slot == 0)
                {
                    return ClaStatusInvalidReport;   /* too many report ids */
                }
                const cla_u32 bits = global.ReportSize * global.ReportCount;
                if (tag == CLA_HID_MAIN_INPUT)        slot->InputBits += bits;
                else if (tag == CLA_HID_MAIN_OUTPUT)  slot->OutputBits += bits;
                else                                  slot->FeatureBits += bits;
                break;
            }

            default:
                break;
            }

            /* Local item state is cleared after every main item
             * (HID 1.11 s6.2.2.8). */
            pendingUsage = 0;
        }
    }

    if (collectionDepth != 0)
    {
        return ClaStatusInvalidReport;   /* unterminated collection */
    }

    return ClaStatusOk;
}

cla_u32 ClaGetFeatureReportSizeBytes(const ClaHidDescriptorInfo *info, cla_u32 reportId)
{
    if (info == 0)
    {
        return 0;
    }
    for (cla_u32 i = 0; i < info->ReportCount; ++i)
    {
        if (info->Reports[i].ReportId == reportId)
        {
            return (info->Reports[i].FeatureBits + 7u) / 8u;
        }
    }
    return 0;
}
