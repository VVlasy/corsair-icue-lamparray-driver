/*
 * Copyright (c) Corsair Dynamic Lighting Bridge contributors.
 * SPDX-License-Identifier: MIT
 *
 * Validates the LampArray report descriptor against USB HID Usage Tables v1.4
 * section 26, and cross-checks it against the CLA_PAYLOAD_SIZE_* constants.
 *
 * The point of these tests is that the report sizes are derived twice by
 * independent means -- once by hand in LampArrayProtocol.h and once by walking
 * the descriptor bytes -- so the two cannot drift apart unnoticed.
 */

#include "ClaTest.h"

#include "cla/LampArrayDescriptor.h"
#include "cla/LampArrayProtocol.h"
#include "cla/HidDescriptorParser.h"
#include "cla/LampArrayDevice.h"

#include <vector>

namespace {

ClaHidDescriptorInfo ParseTheDescriptor()
{
    ClaHidDescriptorInfo info{};
    const ClaStatus status = ClaParseHidDescriptor(
        ClaLampArrayReportDescriptor,
        CLA_LAMP_ARRAY_DESCRIPTOR_SIZE,
        &info, nullptr, nullptr);
    CLA_CHECK_EQ(static_cast<int>(status), static_cast<int>(ClaStatusOk));
    return info;
}

} // namespace

CLA_TEST(Descriptor_ParsesCleanly)
{
    ClaHidDescriptorInfo info{};
    CLA_CHECK_EQ(static_cast<int>(ClaParseHidDescriptor(
                     ClaLampArrayReportDescriptor,
                     CLA_LAMP_ARRAY_DESCRIPTOR_SIZE,
                     &info, nullptr, nullptr)),
                 static_cast<int>(ClaStatusOk));
}

/* Windows classifies a device as a Dynamic Lighting LampArray by its top-level
 * collection: usage page 0x59 (Lighting And Illumination), usage 0x01
 * (LampArray).  If this ever changes the device silently stops appearing in
 * Settings, so it is asserted explicitly. */
CLA_TEST(Descriptor_TopLevelCollectionIsLampArray)
{
    const ClaHidDescriptorInfo info = ParseTheDescriptor();
    CLA_CHECK_EQ(info.TopLevelUsagePage, CLA_USAGE_PAGE_LIGHTING_AND_ILLUMINATION);
    CLA_CHECK_EQ(info.TopLevelUsage, CLA_USAGE_LAMP_ARRAY);
}

CLA_TEST(Descriptor_DeclaresExactlySixReports)
{
    const ClaHidDescriptorInfo info = ParseTheDescriptor();
    CLA_CHECK_EQ(info.ReportCount, 6u);
    CLA_CHECK_EQ(info.UsesReportIds, 1);
}

/* Every LampArray report is a FEATURE report.  This is the single most
 * consequential fact for the driver: it means the whole protocol arrives as
 * IOCTL_HID_GET_FEATURE / IOCTL_HID_SET_FEATURE and there is no input or
 * output report path at all. */
CLA_TEST(Descriptor_AllReportsAreFeatureOnly)
{
    const ClaHidDescriptorInfo info = ParseTheDescriptor();
    for (cla_u32 i = 0; i < info.ReportCount; ++i)
    {
        CLA_CHECK_EQ(info.Reports[i].InputBits, 0u);
        CLA_CHECK_EQ(info.Reports[i].OutputBits, 0u);
        CLA_CHECK(info.Reports[i].FeatureBits > 0u);
    }
}

/* The descriptor-derived payload sizes must equal the hand-maintained
 * constants the driver and service rely on. */
CLA_TEST(Descriptor_ReportSizesMatchProtocolConstants)
{
    const ClaHidDescriptorInfo info = ParseTheDescriptor();

    struct Expected { cla_u32 ReportId; cla_u32 PayloadBytes; };
    const Expected expected[] = {
        { CLA_REPORT_ID_LAMP_ARRAY_ATTRIBUTES,    CLA_PAYLOAD_SIZE_LAMP_ARRAY_ATTRIBUTES },
        { CLA_REPORT_ID_LAMP_ATTRIBUTES_REQUEST,  CLA_PAYLOAD_SIZE_LAMP_ATTRIBUTES_REQUEST },
        { CLA_REPORT_ID_LAMP_ATTRIBUTES_RESPONSE, CLA_PAYLOAD_SIZE_LAMP_ATTRIBUTES_RESPONSE },
        { CLA_REPORT_ID_LAMP_MULTI_UPDATE,        CLA_PAYLOAD_SIZE_LAMP_MULTI_UPDATE },
        { CLA_REPORT_ID_LAMP_RANGE_UPDATE,        CLA_PAYLOAD_SIZE_LAMP_RANGE_UPDATE },
        { CLA_REPORT_ID_LAMP_ARRAY_CONTROL,       CLA_PAYLOAD_SIZE_LAMP_ARRAY_CONTROL },
    };

    for (const Expected &e : expected)
    {
        CLA_CHECK_EQ(ClaGetFeatureReportSizeBytes(&info, e.ReportId), e.PayloadBytes);
        /* And the transfer-buffer size the driver validates against. */
        CLA_CHECK_EQ(ClaGetReportBufferSize(static_cast<cla_u8>(e.ReportId)),
                     e.PayloadBytes + CLA_REPORT_ID_SIZE);
    }
}

/* Documented sizes from the Waratah-generated header the descriptor came from:
 * 22 / 2 / 28 / 50 / 9 / 1 bytes. Restated as literals so a change to the
 * constants alone cannot make the previous test vacuously pass. */
CLA_TEST(Descriptor_ReportSizesMatchMicrosoftReferenceLiterals)
{
    const ClaHidDescriptorInfo info = ParseTheDescriptor();
    CLA_CHECK_EQ(ClaGetFeatureReportSizeBytes(&info, 1), 22u);
    CLA_CHECK_EQ(ClaGetFeatureReportSizeBytes(&info, 2), 2u);
    CLA_CHECK_EQ(ClaGetFeatureReportSizeBytes(&info, 3), 28u);
    CLA_CHECK_EQ(ClaGetFeatureReportSizeBytes(&info, 4), 50u);
    CLA_CHECK_EQ(ClaGetFeatureReportSizeBytes(&info, 5), 9u);
    CLA_CHECK_EQ(ClaGetFeatureReportSizeBytes(&info, 6), 1u);
}

CLA_TEST(Descriptor_HasExpectedSize)
{
    CLA_CHECK_EQ(sizeof(ClaLampArrayReportDescriptor), 292u);
    CLA_CHECK_EQ(static_cast<cla_u32>(CLA_LAMP_ARRAY_DESCRIPTOR_SIZE), 292u);
}

CLA_TEST(Descriptor_StartsWithLightingUsagePageAndLampArrayUsage)
{
    /* 0x05 0x59 = UsagePage(Lighting And Illumination)
     * 0x09 0x01 = UsageId(LampArray)
     * 0xA1 0x01 = Collection(Application) */
    CLA_CHECK_EQ(ClaLampArrayReportDescriptor[0], 0x05);
    CLA_CHECK_EQ(ClaLampArrayReportDescriptor[1], 0x59);
    CLA_CHECK_EQ(ClaLampArrayReportDescriptor[2], 0x09);
    CLA_CHECK_EQ(ClaLampArrayReportDescriptor[3], 0x01);
    CLA_CHECK_EQ(ClaLampArrayReportDescriptor[4], 0xA1);
    CLA_CHECK_EQ(ClaLampArrayReportDescriptor[5], 0x01);
    /* and ends with two EndCollection items */
    CLA_CHECK_EQ(ClaLampArrayReportDescriptor[290], 0xC0);
    CLA_CHECK_EQ(ClaLampArrayReportDescriptor[291], 0xC0);
}

/* The multi-update report exposes exactly 8 update slots; the driver sizes
 * fixed stack arrays from this. */
CLA_TEST(Descriptor_MultiUpdateHasEightSlots)
{
    /* 50-byte payload = 1 (LampCount) + 1 (flags) + 8*2 (ids) + 8*4 (colors) */
    CLA_CHECK_EQ(1u + 1u + CLA_LAMP_MULTI_UPDATE_SLOT_COUNT * 2u +
                     CLA_LAMP_MULTI_UPDATE_SLOT_COUNT * 4u,
                 static_cast<unsigned>(CLA_PAYLOAD_SIZE_LAMP_MULTI_UPDATE));
    CLA_CHECK_EQ(static_cast<unsigned>(CLA_LAMP_MULTI_UPDATE_SLOT_COUNT), 8u);
}

/* --- parser robustness: it must reject malformed descriptors, not crash --- */

CLA_TEST(Parser_RejectsTruncatedItem)
{
    /* 0x05 declares a 1-byte payload that is not present. */
    const cla_u8 bad[] = { 0x05 };
    ClaHidDescriptorInfo info{};
    CLA_CHECK_EQ(static_cast<int>(ClaParseHidDescriptor(bad, sizeof(bad), &info, nullptr, nullptr)),
                 static_cast<int>(ClaStatusInvalidReport));
}

CLA_TEST(Parser_RejectsUnbalancedEndCollection)
{
    const cla_u8 bad[] = { 0xC0 };
    ClaHidDescriptorInfo info{};
    CLA_CHECK_EQ(static_cast<int>(ClaParseHidDescriptor(bad, sizeof(bad), &info, nullptr, nullptr)),
                 static_cast<int>(ClaStatusInvalidReport));
}

CLA_TEST(Parser_RejectsUnterminatedCollection)
{
    const cla_u8 bad[] = { 0x05, 0x59, 0x09, 0x01, 0xA1, 0x01 };
    ClaHidDescriptorInfo info{};
    CLA_CHECK_EQ(static_cast<int>(ClaParseHidDescriptor(bad, sizeof(bad), &info, nullptr, nullptr)),
                 static_cast<int>(ClaStatusInvalidReport));
}

CLA_TEST(Parser_RejectsLongItems)
{
    const cla_u8 bad[] = { 0xFE, 0x00, 0x00 };
    ClaHidDescriptorInfo info{};
    CLA_CHECK_EQ(static_cast<int>(ClaParseHidDescriptor(bad, sizeof(bad), &info, nullptr, nullptr)),
                 static_cast<int>(ClaStatusInvalidReport));
}

CLA_TEST(Parser_RejectsExcessiveCollectionNesting)
{
    std::vector<cla_u8> bad;
    for (int i = 0; i < CLA_HID_MAX_COLLECTION_DEPTH + 4; ++i)
    {
        bad.push_back(0xA1); bad.push_back(0x01);   /* Collection(Application) */
    }
    ClaHidDescriptorInfo info{};
    CLA_CHECK_EQ(static_cast<int>(ClaParseHidDescriptor(
                     bad.data(), static_cast<cla_u32>(bad.size()), &info, nullptr, nullptr)),
                 static_cast<int>(ClaStatusInvalidReport));
}

CLA_TEST(Parser_RejectsPopWithoutPush)
{
    const cla_u8 bad[] = { 0xB4 };   /* Global tag 0xB (Pop), size 0 */
    ClaHidDescriptorInfo info{};
    CLA_CHECK_EQ(static_cast<int>(ClaParseHidDescriptor(bad, sizeof(bad), &info, nullptr, nullptr)),
                 static_cast<int>(ClaStatusInvalidReport));
}

CLA_TEST(Parser_HandlesEmptyDescriptor)
{
    ClaHidDescriptorInfo info{};
    CLA_CHECK_EQ(static_cast<int>(ClaParseHidDescriptor(
                     ClaLampArrayReportDescriptor, 0, &info, nullptr, nullptr)),
                 static_cast<int>(ClaStatusOk));
    CLA_CHECK_EQ(info.ReportCount, 0u);
}

CLA_TEST(Parser_RejectsNullArguments)
{
    ClaHidDescriptorInfo info{};
    CLA_CHECK_EQ(static_cast<int>(ClaParseHidDescriptor(nullptr, 4, &info, nullptr, nullptr)),
                 static_cast<int>(ClaStatusInvalidArgument));
    CLA_CHECK_EQ(static_cast<int>(ClaParseHidDescriptor(
                     ClaLampArrayReportDescriptor, 4, nullptr, nullptr, nullptr)),
                 static_cast<int>(ClaStatusInvalidArgument));
}
