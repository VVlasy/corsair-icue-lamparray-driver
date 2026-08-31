/*
 * Copyright (c) Corsair Dynamic Lighting Bridge contributors.
 * SPDX-License-Identifier: MIT
 *
 * HidTestClient -- talk to the virtual LampArray as a raw HID device.
 *
 * Two jobs (project Phases 2 and 4):
 *
 *  1. Open the virtual HID device that VHF created and exercise the LampArray
 *     feature reports directly, without going through Windows Dynamic
 *     Lighting.  That separates "the device is wrong" from "Windows is not
 *     driving it", which is otherwise very hard to tell apart.
 *
 *  2. Talk to the driver's own control interface to inspect what it thinks it
 *     is hosting: lamp counts, lamp attributes, and the rejection counters
 *     that are invisible from the HID side (the LampArray spec requires
 *     invalid reports be ignored silently, so counters are the only evidence).
 *
 * Windows only.  Run elevated: the driver's control interface is restricted to
 * SYSTEM and Administrators.
 */

#include "Ipc/DriverLink.h"
#include "cla/LampArrayDevice.h"
#include "cla/LampArrayProtocol.h"
#include "Logging/Logger.h"

#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <hidclass.h>

#include <cstdio>
#include <string>
#include <vector>

using namespace cla;

namespace {

struct HidDevice
{
    HANDLE Handle = INVALID_HANDLE_VALUE;
    std::wstring Path;
    USHORT VendorId = 0;
    USHORT ProductId = 0;
    USHORT UsagePage = 0;
    USHORT Usage = 0;
    std::wstring Product;
};

/* Find every HID device whose top-level collection is a LampArray. */
std::vector<HidDevice> FindLampArrayDevices()
{
    std::vector<HidDevice> found;

    GUID hidGuid{};
    HidD_GetHidGuid(&hidGuid);

    HDEVINFO deviceInfo = SetupDiGetClassDevsW(&hidGuid, nullptr, nullptr,
                                               DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (deviceInfo == INVALID_HANDLE_VALUE)
    {
        return found;
    }

    SP_DEVICE_INTERFACE_DATA interfaceData{};
    interfaceData.cbSize = sizeof(interfaceData);

    for (DWORD index = 0;
         SetupDiEnumDeviceInterfaces(deviceInfo, nullptr, &hidGuid, index, &interfaceData);
         ++index)
    {
        DWORD required = 0;
        SetupDiGetDeviceInterfaceDetailW(deviceInfo, &interfaceData, nullptr, 0,
                                         &required, nullptr);
        if (required == 0)
        {
            continue;
        }

        std::vector<unsigned char> storage(required);
        auto *detail = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(storage.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(deviceInfo, &interfaceData, detail,
                                              required, nullptr, nullptr))
        {
            continue;
        }

        HANDLE handle = CreateFileW(detail->DevicePath,
                                    GENERIC_READ | GENERIC_WRITE,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    nullptr, OPEN_EXISTING, 0, nullptr);
        if (handle == INVALID_HANDLE_VALUE)
        {
            continue;
        }

        HIDD_ATTRIBUTES attributes{};
        attributes.Size = sizeof(attributes);
        PHIDP_PREPARSED_DATA preparsed = nullptr;
        HIDP_CAPS capabilities{};

        bool isLampArray = false;
        if (HidD_GetAttributes(handle, &attributes) &&
            HidD_GetPreparsedData(handle, &preparsed))
        {
            if (HidP_GetCaps(preparsed, &capabilities) == HIDP_STATUS_SUCCESS)
            {
                /* The definition of a Dynamic Lighting device: usage page 0x59
                 * (Lighting And Illumination), usage 0x01 (LampArray). */
                isLampArray =
                    (capabilities.UsagePage == CLA_USAGE_PAGE_LIGHTING_AND_ILLUMINATION &&
                     capabilities.Usage == CLA_USAGE_LAMP_ARRAY);
            }
            HidD_FreePreparsedData(preparsed);
        }

        if (!isLampArray)
        {
            CloseHandle(handle);
            continue;
        }

        HidDevice device;
        device.Handle = handle;
        device.Path = detail->DevicePath;
        device.VendorId = attributes.VendorID;
        device.ProductId = attributes.ProductID;
        device.UsagePage = capabilities.UsagePage;
        device.Usage = capabilities.Usage;

        wchar_t product[256] = {};
        if (HidD_GetProductString(handle, product, sizeof(product)))
        {
            device.Product = product;
        }
        found.push_back(std::move(device));
    }

    SetupDiDestroyDeviceInfoList(deviceInfo);
    return found;
}

bool GetFeature(HANDLE handle, cla_u8 reportId, std::vector<unsigned char> &buffer)
{
    buffer.assign(ClaGetReportBufferSize(reportId), 0);
    if (buffer.empty())
    {
        return false;
    }
    buffer[0] = reportId;
    return HidD_GetFeature(handle, buffer.data(), static_cast<ULONG>(buffer.size())) != FALSE;
}

bool SetFeature(HANDLE handle, std::vector<unsigned char> &buffer)
{
    return HidD_SetFeature(handle, buffer.data(), static_cast<ULONG>(buffer.size())) != FALSE;
}

cla_u16 ReadU16(const unsigned char *p, size_t offset)
{
    return static_cast<cla_u16>(p[offset] | (p[offset + 1] << 8));
}

cla_u32 ReadU32(const unsigned char *p, size_t offset)
{
    return static_cast<cla_u32>(p[offset]) |
           (static_cast<cla_u32>(p[offset + 1]) << 8) |
           (static_cast<cla_u32>(p[offset + 2]) << 16) |
           (static_cast<cla_u32>(p[offset + 3]) << 24);
}

void WriteU16(unsigned char *p, size_t offset, cla_u16 value)
{
    p[offset] = static_cast<unsigned char>(value & 0xFF);
    p[offset + 1] = static_cast<unsigned char>(value >> 8);
}

int InspectDevice(HidDevice &device, bool verbose)
{
    std::wprintf(L"Device %ls\n", device.Path.c_str());
    std::printf("  VID/PID   : %04X:%04X\n", device.VendorId, device.ProductId);
    std::printf("  usage     : page 0x%04X usage 0x%04X (LampArray)\n",
                device.UsagePage, device.Usage);
    if (!device.Product.empty())
    {
        std::wprintf(L"  product   : %ls\n", device.Product.c_str());
    }

    std::vector<unsigned char> buffer;
    if (!GetFeature(device.Handle, CLA_REPORT_ID_LAMP_ARRAY_ATTRIBUTES, buffer))
    {
        std::printf("  LampArrayAttributesReport: FAILED (%lu)\n\n", GetLastError());
        return 1;
    }

    const cla_u16 lampCount = ReadU16(buffer.data(), 1);
    std::printf("  lamp count: %u\n", lampCount);
    std::printf("  bounding  : %u x %u x %u um\n",
                ReadU32(buffer.data(), 3), ReadU32(buffer.data(), 7),
                ReadU32(buffer.data(), 11));
    std::printf("  kind      : %u\n", ReadU32(buffer.data(), 15));
    std::printf("  min update: %u us\n", ReadU32(buffer.data(), 19));

    if (lampCount == 0)
    {
        std::printf("  no lamps; nothing further to inspect\n\n");
        return 1;
    }

    /* Walk the lamp attributes exactly as a host would: request lamp 0, then
     * read responses and let the device auto-increment (HUT 1.4 s26.8.2). */
    std::vector<unsigned char> request(ClaGetReportBufferSize(CLA_REPORT_ID_LAMP_ATTRIBUTES_REQUEST), 0);
    request[0] = CLA_REPORT_ID_LAMP_ATTRIBUTES_REQUEST;
    WriteU16(request.data(), 1, 0);
    if (!SetFeature(device.Handle, request))
    {
        std::printf("  LampAttributesRequestReport: FAILED (%lu)\n\n", GetLastError());
        return 1;
    }

    const cla_u16 toShow = verbose ? lampCount : (lampCount < 8 ? lampCount : 8);
    std::printf("\n  %-8s %10s %10s %10s %8s %6s %s\n",
                "LampId", "X (um)", "Y (um)", "Z (um)", "purposes", "prog", "RGBI levels");
    std::printf("  %-8s %10s %10s %10s %8s %6s %s\n",
                "------", "------", "------", "------", "--------", "----", "-----------");

    int problems = 0;
    for (cla_u16 i = 0; i < toShow; ++i)
    {
        std::vector<unsigned char> response;
        if (!GetFeature(device.Handle, CLA_REPORT_ID_LAMP_ATTRIBUTES_RESPONSE, response))
        {
            std::printf("  lamp %u: response FAILED (%lu)\n", i, GetLastError());
            problems++;
            break;
        }

        const cla_u16 reportedId = ReadU16(response.data(), 1);
        if (reportedId != i)
        {
            /* The spec tells the host to check this, because an invalid
             * request is silently treated as lamp 0. */
            std::printf("  WARNING: expected LampId %u, device returned %u\n", i, reportedId);
            problems++;
        }

        std::printf("  %-8u %10u %10u %10u %8u %6u %u/%u/%u/%u\n",
                    reportedId,
                    ReadU32(response.data(), 3),
                    ReadU32(response.data(), 7),
                    ReadU32(response.data(), 11),
                    ReadU32(response.data(), 19),
                    response[27],
                    response[23], response[24], response[25], response[26]);
    }
    if (!verbose && lampCount > toShow)
    {
        std::printf("  ... %u more (use --verbose)\n", lampCount - toShow);
    }
    std::printf("\n");
    return problems;
}

int RunColorTest(HidDevice &device)
{
    std::vector<unsigned char> attributes;
    if (!GetFeature(device.Handle, CLA_REPORT_ID_LAMP_ARRAY_ATTRIBUTES, attributes))
    {
        std::printf("Could not read attributes.\n");
        return 1;
    }
    const cla_u16 lampCount = ReadU16(attributes.data(), 1);
    if (lampCount == 0)
    {
        return 1;
    }

    std::printf("Driving the device directly over HID (bypassing Dynamic Lighting)\n");
    std::printf("----------------------------------------------------------------\n");

    /* Take control: HUT 1.4 s26.10.1 says the host must disable autonomous
     * mode before its updates are honoured. */
    std::vector<unsigned char> control(ClaGetReportBufferSize(CLA_REPORT_ID_LAMP_ARRAY_CONTROL), 0);
    control[0] = CLA_REPORT_ID_LAMP_ARRAY_CONTROL;
    control[1] = 0;   /* autonomous mode off */
    if (!SetFeature(device.Handle, control))
    {
        std::printf("  disabling autonomous mode FAILED (%lu)\n", GetLastError());
        return 1;
    }
    std::printf("  autonomous mode disabled\n");

    struct Step { const char *Name; cla_u8 R, G, B; };
    const Step steps[] = {
        { "red",   255, 0, 0 }, { "green", 0, 255, 0 }, { "blue",  0, 0, 255 },
        { "white", 255, 255, 255 }, { "off", 0, 0, 0 },
    };

    for (const Step &step : steps)
    {
        std::vector<unsigned char> update(ClaGetReportBufferSize(CLA_REPORT_ID_LAMP_RANGE_UPDATE), 0);
        update[0] = CLA_REPORT_ID_LAMP_RANGE_UPDATE;
        update[1] = CLA_LAMP_UPDATE_COMPLETE;
        WriteU16(update.data(), 2, 0);
        WriteU16(update.data(), 4, static_cast<cla_u16>(lampCount - 1));
        update[6] = step.R;
        update[7] = step.G;
        update[8] = step.B;
        update[9] = 255;

        std::printf("  %-6s ... ", step.Name);
        std::fflush(stdout);
        if (!SetFeature(device.Handle, update))
        {
            std::printf("FAILED (%lu)\n", GetLastError());
            return 1;
        }
        std::printf("sent\n");
        Sleep(1000);
    }

    /* Hand the device back, as a well-behaved host must. */
    control[1] = 1;
    SetFeature(device.Handle, control);
    std::printf("  autonomous mode re-enabled\n\n");
    return 0;
}

int ReportDriverState()
{
    DriverLink link;
    std::string error;
    if (!link.Open(error))
    {
        std::printf("Driver control interface: NOT AVAILABLE\n  %s\n\n", error.c_str());
        return 1;
    }

    std::printf("Driver control interface\n");
    std::printf("------------------------\n");
    std::printf("  path: %s\n", link.InterfacePath().c_str());

    ClaDriverInfo info{};
    if (!link.GetDriverInfo(info, error))
    {
        std::printf("  GetDriverInfo failed: %s\n\n", error.c_str());
        return 1;
    }

    std::printf("  driver version : %u.%u.%u\n",
                info.DriverVersionMajor, info.DriverVersionMinor, info.DriverVersionPatch);
    std::printf("  IPC protocol   : %u (this tool speaks %u)\n",
                info.ProtocolVersion, CLA_IPC_PROTOCOL_VERSION);
    std::printf("  active arrays  : %u of %u\n",
                info.ActiveLampArrayCount, info.MaxLampArrayCount);
    std::printf("  max lamps      : %u\n\n", info.MaxLampsPerArray);

    /* Per-array counters.  Ids are assigned from 1 upwards; probe a small
     * range rather than requiring the caller to know them. */
    for (cla_u32 id = 1; id <= CLA_MAX_LAMP_ARRAYS; ++id)
    {
        ClaStatisticsResponse statistics{};
        if (!link.GetStatistics(id, statistics, error))
        {
            continue;
        }

        std::printf("  LampArray %u\n", id);
        std::printf("    GetFeature calls        : %llu\n",
                    (unsigned long long)statistics.Counters.GetFeatureCalls);
        std::printf("    SetFeature calls        : %llu\n",
                    (unsigned long long)statistics.Counters.SetFeatureCalls);
        std::printf("    frames completed        : %llu\n",
                    (unsigned long long)statistics.Counters.FramesCompleted);
        std::printf("    frames published        : %llu\n",
                    (unsigned long long)statistics.FramesPublished);
        std::printf("    frames coalesced        : %llu\n",
                    (unsigned long long)statistics.FramesCoalesced);
        std::printf("    rejected: bad size %llu, invalid %llu, unknown id %llu\n",
                    (unsigned long long)statistics.Counters.RejectedBadBufferSize,
                    (unsigned long long)statistics.Counters.RejectedInvalidReport,
                    (unsigned long long)statistics.Counters.RejectedUnknownReportId);
        std::printf("    rejected: dup lamp %llu, lamp range %llu, channel level %llu\n\n",
                    (unsigned long long)statistics.Counters.RejectedDuplicateLampId,
                    (unsigned long long)statistics.Counters.RejectedLampIdOutOfRange,
                    (unsigned long long)statistics.Counters.RejectedChannelOverLevelCount);
    }
    return 0;
}

} // namespace

int main(int argc, char **argv)
{
    bool testColors = false;
    bool verbose = false;
    bool driverOnly = false;

    for (int i = 1; i < argc; ++i)
    {
        const std::string argument = argv[i];
        if (argument == "--test-colors" || argument == "--test-colours") testColors = true;
        else if (argument == "--verbose") verbose = true;
        else if (argument == "--driver")  driverOnly = true;
        else if (argument == "--help" || argument == "-h")
        {
            std::printf(
                "HidTestClient -- inspect and drive the virtual HID LampArray\n\n"
                "  --test-colors   drive colours over HID, bypassing Dynamic Lighting\n"
                "  --verbose       print every lamp's attributes\n"
                "  --driver        only report the driver's control interface\n"
                "\nRun elevated: the driver interface is restricted to Administrators.\n");
            return 0;
        }
        else
        {
            std::fprintf(stderr, "Unknown option: %s\n", argument.c_str());
            return 2;
        }
    }

    Logger::Instance().SetLevel(LogLevel::Warning);

    std::printf("HidTestClient\n");
    std::printf("=============\n\n");

    ReportDriverState();

    if (driverOnly)
    {
        return 0;
    }

    std::vector<HidDevice> devices = FindLampArrayDevices();
    std::printf("%zu HID LampArray device(s) present\n", devices.size());
    std::printf("=================================\n\n");

    if (devices.empty())
    {
        std::printf("None found. Check that:\n");
        std::printf("  - the CorsairLampArray driver is installed and started;\n");
        std::printf("  - the service is running and has created at least one device;\n");
        std::printf("  - Device Manager shows a HID device under the driver.\n");
        return 1;
    }

    int problems = 0;
    for (HidDevice &device : devices)
    {
        problems += InspectDevice(device, verbose);
        if (testColors)
        {
            problems += RunColorTest(device);
        }
    }

    for (HidDevice &device : devices)
    {
        if (device.Handle != INVALID_HANDLE_VALUE)
        {
            CloseHandle(device.Handle);
        }
    }

    std::printf(problems == 0 ? "All checks passed.\n" : "%d problem(s) found.\n", problems);
    return problems == 0 ? 0 : 1;
}
