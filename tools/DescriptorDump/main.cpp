/*
 * Copyright (c) Corsair Dynamic Lighting Bridge contributors.
 * SPDX-License-Identifier: MIT
 *
 * DescriptorDump -- print and validate the LampArray HID report descriptor.
 *
 * Project Phase 3 asks for "a descriptor dump/test tool that prints the final
 * descriptor in a human-readable format".  This does that, and also re-derives
 * the report sizes from the bytes and checks them against the constants the
 * driver and service compile against, so running it is a real check rather
 * than just a listing.
 *
 * It is portable on purpose: the descriptor can be reviewed without a Windows
 * machine, and CI runs it as a test.
 *
 * Usage:
 *   DescriptorDump              print the descriptor and validate it
 *   DescriptorDump --check      validate only; exit non-zero on failure
 *   DescriptorDump --c-array    emit the descriptor as a C array
 *   DescriptorDump --bytes      emit the raw bytes as hex
 */

#include "cla/LampArrayDescriptor.h"
#include "cla/LampArrayProtocol.h"
#include "cla/HidDescriptorParser.h"
#include "cla/LampArrayDevice.h"

#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <string>
#include <vector>

namespace {

const char *MainTagName(cla_u8 tag)
{
    switch (tag)
    {
    case CLA_HID_MAIN_INPUT:          return "Input";
    case CLA_HID_MAIN_OUTPUT:         return "Output";
    case CLA_HID_MAIN_COLLECTION:     return "Collection";
    case CLA_HID_MAIN_FEATURE:        return "Feature";
    case CLA_HID_MAIN_END_COLLECTION: return "EndCollection";
    default:                          return "Main(reserved)";
    }
}

const char *GlobalTagName(cla_u8 tag)
{
    switch (tag)
    {
    case CLA_HID_GLOBAL_USAGE_PAGE:    return "UsagePage";
    case CLA_HID_GLOBAL_LOGICAL_MIN:   return "LogicalMinimum";
    case CLA_HID_GLOBAL_LOGICAL_MAX:   return "LogicalMaximum";
    case CLA_HID_GLOBAL_PHYSICAL_MIN:  return "PhysicalMinimum";
    case CLA_HID_GLOBAL_PHYSICAL_MAX:  return "PhysicalMaximum";
    case CLA_HID_GLOBAL_UNIT_EXPONENT: return "UnitExponent";
    case CLA_HID_GLOBAL_UNIT:          return "Unit";
    case CLA_HID_GLOBAL_REPORT_SIZE:   return "ReportSize";
    case CLA_HID_GLOBAL_REPORT_ID:     return "ReportId";
    case CLA_HID_GLOBAL_REPORT_COUNT:  return "ReportCount";
    case CLA_HID_GLOBAL_PUSH:          return "Push";
    case CLA_HID_GLOBAL_POP:           return "Pop";
    default:                           return "Global(reserved)";
    }
}

const char *LocalTagName(cla_u8 tag)
{
    switch (tag)
    {
    case CLA_HID_LOCAL_USAGE:     return "Usage";
    case CLA_HID_LOCAL_USAGE_MIN: return "UsageMinimum";
    case CLA_HID_LOCAL_USAGE_MAX: return "UsageMaximum";
    default:                      return "Local";
    }
}

/* Usage names from HID Usage Tables 1.4 section 26. */
const char *LightingUsageName(cla_u32 usage)
{
    switch (usage)
    {
    case CLA_USAGE_LAMP_ARRAY:                      return "LampArray";
    case CLA_USAGE_LAMP_ARRAY_ATTRIBUTES_REPORT:    return "LampArrayAttributesReport";
    case CLA_USAGE_LAMP_COUNT:                      return "LampCount";
    case CLA_USAGE_BOUNDING_BOX_WIDTH_UM:           return "BoundingBoxWidthInMicrometers";
    case CLA_USAGE_BOUNDING_BOX_HEIGHT_UM:          return "BoundingBoxHeightInMicrometers";
    case CLA_USAGE_BOUNDING_BOX_DEPTH_UM:           return "BoundingBoxDepthInMicrometers";
    case CLA_USAGE_LAMP_ARRAY_KIND:                 return "LampArrayKind";
    case CLA_USAGE_MIN_UPDATE_INTERVAL_US:          return "MinUpdateIntervalInMicroseconds";
    case CLA_USAGE_LAMP_ATTRIBUTES_REQUEST_REPORT:  return "LampAttributesRequestReport";
    case CLA_USAGE_LAMP_ID:                         return "LampId";
    case CLA_USAGE_LAMP_ATTRIBUTES_RESPONSE_REPORT: return "LampAttributesResponseReport";
    case CLA_USAGE_POSITION_X_UM:                   return "PositionXInMicrometers";
    case CLA_USAGE_POSITION_Y_UM:                   return "PositionYInMicrometers";
    case CLA_USAGE_POSITION_Z_UM:                   return "PositionZInMicrometers";
    case CLA_USAGE_LAMP_PURPOSES:                   return "LampPurposes";
    case CLA_USAGE_UPDATE_LATENCY_US:               return "UpdateLatencyInMicroseconds";
    case CLA_USAGE_RED_LEVEL_COUNT:                 return "RedLevelCount";
    case CLA_USAGE_GREEN_LEVEL_COUNT:               return "GreenLevelCount";
    case CLA_USAGE_BLUE_LEVEL_COUNT:                return "BlueLevelCount";
    case CLA_USAGE_INTENSITY_LEVEL_COUNT:           return "IntensityLevelCount";
    case CLA_USAGE_IS_PROGRAMMABLE:                 return "IsProgrammable";
    case CLA_USAGE_INPUT_BINDING:                   return "InputBinding";
    case CLA_USAGE_LAMP_MULTI_UPDATE_REPORT:        return "LampMultiUpdateReport";
    case CLA_USAGE_RED_UPDATE_CHANNEL:              return "RedUpdateChannel";
    case CLA_USAGE_GREEN_UPDATE_CHANNEL:            return "GreenUpdateChannel";
    case CLA_USAGE_BLUE_UPDATE_CHANNEL:             return "BlueUpdateChannel";
    case CLA_USAGE_INTENSITY_UPDATE_CHANNEL:        return "IntensityUpdateChannel";
    case CLA_USAGE_LAMP_UPDATE_FLAGS:               return "LampUpdateFlags";
    case CLA_USAGE_LAMP_RANGE_UPDATE_REPORT:        return "LampRangeUpdateReport";
    case CLA_USAGE_LAMP_ID_START:                   return "LampIdStart";
    case CLA_USAGE_LAMP_ID_END:                     return "LampIdEnd";
    case CLA_USAGE_LAMP_ARRAY_CONTROL_REPORT:       return "LampArrayControlReport";
    case CLA_USAGE_AUTONOMOUS_MODE:                 return "AutonomousMode";
    default:                                        return nullptr;
    }
}

const char *CollectionName(cla_u32 value)
{
    switch (value)
    {
    case 0x00: return "Physical";
    case 0x01: return "Application";
    case 0x02: return "Logical";
    case 0x03: return "Report";
    case 0x04: return "NamedArray";
    case 0x05: return "UsageSwitch";
    case 0x06: return "UsageModifier";
    default:   return "VendorDefined";
    }
}

struct DumpContext
{
    int Indent = 0;
    cla_u32 CurrentUsagePage = 0;
};

int PrintItem(const ClaHidItem *item, void *context)
{
    DumpContext *dump = static_cast<DumpContext *>(context);

    if (item->Type == CLA_HID_ITEM_TYPE_MAIN && item->Tag == CLA_HID_MAIN_END_COLLECTION)
    {
        if (dump->Indent > 0) dump->Indent--;
    }

    /* Raw bytes column. */
    std::string bytes;
    {
        char buffer[16];
        std::snprintf(buffer, sizeof(buffer), "%02X ",
                      ClaLampArrayReportDescriptor[item->ByteOffset]);
        bytes += buffer;
        for (cla_u8 i = 0; i < item->DataLength; ++i)
        {
            std::snprintf(buffer, sizeof(buffer), "%02X ",
                          ClaLampArrayReportDescriptor[item->ByteOffset + 1 + i]);
            bytes += buffer;
        }
    }

    /* Decoded text. */
    std::string text;
    char scratch[192];

    switch (item->Type)
    {
    case CLA_HID_ITEM_TYPE_MAIN:
        if (item->Tag == CLA_HID_MAIN_COLLECTION)
        {
            std::snprintf(scratch, sizeof(scratch), "Collection(%s)",
                          CollectionName(item->Data));
        }
        else if (item->Tag == CLA_HID_MAIN_END_COLLECTION)
        {
            std::snprintf(scratch, sizeof(scratch), "EndCollection()");
        }
        else
        {
            /* Main item data bits, HID 1.11 s6.2.2.5. */
            std::snprintf(scratch, sizeof(scratch), "%s(%s, %s, %s)",
                          MainTagName(item->Tag),
                          (item->Data & 0x01) ? "Constant" : "Data",
                          (item->Data & 0x02) ? "Variable" : "Array",
                          (item->Data & 0x04) ? "Relative" : "Absolute");
        }
        text = scratch;
        break;

    case CLA_HID_ITEM_TYPE_GLOBAL:
        if (item->Tag == CLA_HID_GLOBAL_USAGE_PAGE)
        {
            dump->CurrentUsagePage = item->Data;
            std::snprintf(scratch, sizeof(scratch), "UsagePage(%s [0x%04X])",
                          item->Data == CLA_USAGE_PAGE_LIGHTING_AND_ILLUMINATION
                              ? "Lighting And Illumination" : "?",
                          item->Data);
        }
        else
        {
            std::snprintf(scratch, sizeof(scratch), "%s(%u)",
                          GlobalTagName(item->Tag), item->Data);
        }
        text = scratch;
        break;

    case CLA_HID_ITEM_TYPE_LOCAL:
    {
        const char *name =
            (dump->CurrentUsagePage == CLA_USAGE_PAGE_LIGHTING_AND_ILLUMINATION)
                ? LightingUsageName(item->Data) : nullptr;
        if (name != nullptr)
        {
            std::snprintf(scratch, sizeof(scratch), "%s(%s [0x%04X])",
                          LocalTagName(item->Tag), name, item->Data);
        }
        else
        {
            std::snprintf(scratch, sizeof(scratch), "%s(0x%04X)",
                          LocalTagName(item->Tag), item->Data);
        }
        text = scratch;
        break;
    }

    default:
        text = "Reserved";
        break;
    }

    std::printf("  %3u  %-17s %*s%s\n",
                item->ByteOffset, bytes.c_str(), dump->Indent * 4, "", text.c_str());

    if (item->Type == CLA_HID_ITEM_TYPE_MAIN && item->Tag == CLA_HID_MAIN_COLLECTION)
    {
        dump->Indent++;
    }
    return 1;
}

struct ExpectedReport
{
    cla_u32 ReportId;
    cla_u32 PayloadBytes;
    const char *Name;
};

const ExpectedReport kExpectedReports[] = {
    { CLA_REPORT_ID_LAMP_ARRAY_ATTRIBUTES,    CLA_PAYLOAD_SIZE_LAMP_ARRAY_ATTRIBUTES,    "LampArrayAttributes" },
    { CLA_REPORT_ID_LAMP_ATTRIBUTES_REQUEST,  CLA_PAYLOAD_SIZE_LAMP_ATTRIBUTES_REQUEST,  "LampAttributesRequest" },
    { CLA_REPORT_ID_LAMP_ATTRIBUTES_RESPONSE, CLA_PAYLOAD_SIZE_LAMP_ATTRIBUTES_RESPONSE, "LampAttributesResponse" },
    { CLA_REPORT_ID_LAMP_MULTI_UPDATE,        CLA_PAYLOAD_SIZE_LAMP_MULTI_UPDATE,        "LampMultiUpdate" },
    { CLA_REPORT_ID_LAMP_RANGE_UPDATE,        CLA_PAYLOAD_SIZE_LAMP_RANGE_UPDATE,        "LampRangeUpdate" },
    { CLA_REPORT_ID_LAMP_ARRAY_CONTROL,       CLA_PAYLOAD_SIZE_LAMP_ARRAY_CONTROL,       "LampArrayControl" },
};

/* Returns the number of problems found. */
int Validate(const ClaHidDescriptorInfo &info, bool quiet)
{
    int problems = 0;
    auto fail = [&](const char *format, ...) {
        problems++;
        std::fputs("  FAIL  ", stdout);
        va_list args;
        va_start(args, format);
        std::vfprintf(stdout, format, args);
        va_end(args);
        std::fputc('\n', stdout);
    };
    auto pass = [&](const char *format, ...) {
        if (quiet) return;
        std::fputs("  ok    ", stdout);
        va_list args;
        va_start(args, format);
        std::vfprintf(stdout, format, args);
        va_end(args);
        std::fputc('\n', stdout);
    };

    if (info.TopLevelUsagePage == CLA_USAGE_PAGE_LIGHTING_AND_ILLUMINATION)
        pass("top-level usage page is Lighting And Illumination (0x59)");
    else
        fail("top-level usage page is 0x%04X, expected 0x59", info.TopLevelUsagePage);

    if (info.TopLevelUsage == CLA_USAGE_LAMP_ARRAY)
        pass("top-level usage is LampArray (0x01)");
    else
        fail("top-level usage is 0x%04X, expected 0x01", info.TopLevelUsage);

    if (info.ReportCount == 6)
        pass("declares 6 reports");
    else
        fail("declares %u reports, expected 6", info.ReportCount);

    for (const ExpectedReport &expected : kExpectedReports)
    {
        const cla_u32 actual = ClaGetFeatureReportSizeBytes(&info, expected.ReportId);
        if (actual == expected.PayloadBytes)
        {
            pass("report %u (%s): %u byte payload, %u byte transfer",
                 expected.ReportId, expected.Name, actual,
                 ClaGetReportBufferSize(static_cast<cla_u8>(expected.ReportId)));
        }
        else
        {
            fail("report %u (%s): descriptor says %u bytes, code expects %u",
                 expected.ReportId, expected.Name, actual, expected.PayloadBytes);
        }
    }

    /* Every report must be Feature-only: the driver has no read/write path. */
    bool featureOnly = true;
    for (cla_u32 i = 0; i < info.ReportCount; ++i)
    {
        if (info.Reports[i].InputBits != 0 || info.Reports[i].OutputBits != 0)
        {
            featureOnly = false;
            fail("report %u declares input/output bits; LampArray is feature-only",
                 info.Reports[i].ReportId);
        }
    }
    if (featureOnly) pass("all reports are Feature reports (no input/output)");

    return problems;
}

} // namespace

int main(int argc, char **argv)
{
    std::string mode = (argc > 1) ? argv[1] : "";

    if (mode == "--bytes")
    {
        for (cla_u32 i = 0; i < CLA_LAMP_ARRAY_DESCRIPTOR_SIZE; ++i)
        {
            std::printf("%02X%c", ClaLampArrayReportDescriptor[i],
                        ((i + 1) % 16 == 0) ? '\n' : ' ');
        }
        std::printf("\n");
        return 0;
    }

    if (mode == "--c-array")
    {
        std::printf("static const unsigned char LampArrayReportDescriptor[%u] =\n{\n",
                    CLA_LAMP_ARRAY_DESCRIPTOR_SIZE);
        for (cla_u32 i = 0; i < CLA_LAMP_ARRAY_DESCRIPTOR_SIZE; ++i)
        {
            if (i % 12 == 0) std::printf("   ");
            std::printf(" 0x%02X,", ClaLampArrayReportDescriptor[i]);
            if ((i + 1) % 12 == 0) std::printf("\n");
        }
        std::printf("\n};\n");
        return 0;
    }

    const bool quiet = (mode == "--check");

    ClaHidDescriptorInfo info{};
    DumpContext dump;

    if (!quiet)
    {
        std::printf("HID LampArray report descriptor\n");
        std::printf("===============================\n\n");
        std::printf("Size: %u bytes\n", CLA_LAMP_ARRAY_DESCRIPTOR_SIZE);
        std::printf("Spec: USB HID Usage Tables 1.4, section 26 "
                    "\"Lighting And Illumination Page (0x59)\"\n\n");
        std::printf("  off  bytes             item\n");
        std::printf("  ---  ----------------  ----\n");
    }

    const ClaStatus status = ClaParseHidDescriptor(
        ClaLampArrayReportDescriptor, CLA_LAMP_ARRAY_DESCRIPTOR_SIZE,
        &info, quiet ? nullptr : PrintItem, &dump);

    if (status != ClaStatusOk)
    {
        std::fprintf(stderr, "\nERROR: descriptor failed to parse (status %d)\n",
                     static_cast<int>(status));
        return 1;
    }

    if (!quiet)
    {
        std::printf("\nParsed %u items, max collection depth %u\n\n",
                    info.ItemCount, info.MaxCollectionDepth);

        std::printf("Reports\n-------\n");
        std::printf("  id  kind     payload  transfer  name\n");
        std::printf("  --  -------  -------  --------  ----\n");
        for (const ExpectedReport &expected : kExpectedReports)
        {
            std::printf("  %2u  Feature  %5u B  %6u B  %s\n",
                        expected.ReportId,
                        ClaGetFeatureReportSizeBytes(&info, expected.ReportId),
                        ClaGetReportBufferSize(static_cast<cla_u8>(expected.ReportId)),
                        expected.Name);
        }
        std::printf("\n  \"transfer\" includes the leading report id byte, which HIDCLASS\n"
                    "  places at buffer[0] and counts in reportBufferLen.\n");
        std::printf("\nValidation\n----------\n");
    }

    const int problems = Validate(info, quiet);

    if (problems == 0)
    {
        if (!quiet) std::printf("\nDescriptor is valid.\n");
        return 0;
    }

    std::printf("\n%d problem(s) found.\n", problems);
    return 1;
}
