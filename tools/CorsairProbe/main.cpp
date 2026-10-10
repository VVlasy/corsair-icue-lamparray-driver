/*
 * Copyright (c) Corsair Dynamic Lighting Bridge contributors.
 * SPDX-License-Identifier: MIT
 *
 * CorsairProbe -- prove the Corsair SDK backend works (project Phase 1).
 *
 * This is the gate the project puts before any virtual-device work: if this
 * tool cannot reliably light a Corsair keyboard through iCUE, nothing built on
 * top of it can either.  It exists to answer, on a real machine, questions the
 * automated tests cannot:
 *
 *   - is the SDK installed, and where;
 *   - does iCUE accept a connection, and which version;
 *   - what devices, LEDs and LED positions does it report;
 *   - do colour writes actually reach the hardware;
 *   - what happens when iCUE is closed and restarted while we are running.
 *
 * Windows only: it drives the real SDK.  Run it before filing any bug about
 * lighting not working, and paste its output into the report.
 *
 * Usage:
 *   CorsairProbe                 enumerate and report; no lighting changes
 *   CorsairProbe --test-colors   also run the colour test sequence
 *   CorsairProbe --watch         stay running and report session/device events
 *   CorsairProbe --device <id>   restrict to one device
 */

#include "Corsair/IcueSdkBackend.h"
#include "Corsair/FakeBackend.h"
#include "Logging/Logger.h"
#include "cla/CorsairMapping.h"

#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>
#include <atomic>

using namespace cla;

namespace {

std::atomic<bool> g_stop{false};

BOOL WINAPI ControlHandler(DWORD type)
{
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT)
    {
        g_stop.store(true);
        return TRUE;
    }
    return FALSE;
}

const char *KindName(cla_u32 kind)
{
    switch (kind)
    {
    case ClaLampArrayKindKeyboard:       return "Keyboard";
    case ClaLampArrayKindMouse:          return "Mouse";
    case ClaLampArrayKindGameController: return "GameController";
    case ClaLampArrayKindPeripheral:     return "Peripheral";
    case ClaLampArrayKindScene:          return "Scene";
    case ClaLampArrayKindNotification:   return "Notification";
    case ClaLampArrayKindChassis:        return "Chassis";
    case ClaLampArrayKindWearable:       return "Wearable";
    case ClaLampArrayKindFurniture:      return "Furniture";
    case ClaLampArrayKindArt:            return "Art";
    default:                             return "Undefined";
    }
}

void ReportSdkSearch()
{
    std::printf("iCUE SDK search order\n");
    std::printf("---------------------\n");
    for (const std::wstring &candidate : IcueSdkBackend::CandidateSdkPaths())
    {
        const DWORD attributes = GetFileAttributesW(candidate.c_str());
        const bool exists = (attributes != INVALID_FILE_ATTRIBUTES);
        std::wprintf(L"  [%hs] %ls\n", exists ? "found  " : "missing", candidate.c_str());
    }
    std::printf("\n");
}

bool WaitForConnection(ICorsairBackend &backend, int timeoutMilliseconds)
{
    /* CorsairConnect is asynchronous: it returns immediately and the session
     * state arrives on a callback, so a probe that checked once would usually
     * report "not connected" on a perfectly healthy system. */
    const DWORD deadline = GetTickCount() + static_cast<DWORD>(timeoutMilliseconds);
    while (GetTickCount() < deadline)
    {
        if (backend.IsConnected())
        {
            return true;
        }
        Sleep(100);
    }
    return backend.IsConnected();
}

void PrintDevice(ICorsairBackend &backend, const BackendDeviceInfo &info, bool verbose)
{
    std::printf("Device %s\n", info.Id.c_str());
    std::printf("  model        : %s\n", info.Model.c_str());
    std::printf("  serial       : %s\n", info.Serial.empty() ? "(none)" : info.Serial.c_str());
    std::printf("  LED count    : %u\n", info.LedCount);
    std::printf("  channels     : %u\n", info.ChannelCount);
    std::printf("  LampArrayKind: %s (%u)\n", KindName(info.LampArrayKind), info.LampArrayKind);
    std::printf("  position units: %s\n",
                info.PositionUnits == ClaSourceUnitsMillimeters ? "millimetres"
                                                                : "logical");
    std::printf("  scope        : %s\n",
                info.Scope == ClaSourceScopeWholeCase ? "whole case" : "component");

    std::vector<ClaSourceLed> leds;
    const BackendStatus status = backend.GetLamps(info.Id, leds);
    if (status != BackendStatus::Ok)
    {
        std::printf("  LED positions: FAILED (%s)\n\n", ToString(status));
        return;
    }

    std::printf("  LED positions: %zu reported\n", leds.size());

    if (verbose && !leds.empty())
    {
        std::printf("\n      %-10s %12s %12s\n", "LED id", "X", "Y");
        std::printf("      %-10s %12s %12s\n", "------", "-", "-");
        for (const ClaSourceLed &led : leds)
        {
            std::printf("      %-10u %12.2f %12.2f\n", led.LedId, led.X, led.Y);
        }
        std::printf("\n");
    }

    /* Show what the mapping layer would make of it -- this is what Windows
     * would be told the device looks like. */
    if (!leds.empty())
    {
        ClaSourceDevice source{};
        source.LampArrayKind = info.LampArrayKind;
        source.Units = info.PositionUnits;
        source.Scope = info.Scope;
        source.LedCount = static_cast<cla_u32>(leds.size());
        source.Leds = leds.data();

        std::vector<ClaLampInfo> lamps(leds.size());
        std::vector<cla_u32> map(leds.size());
        ClaMappingResult mapping{};

        if (ClaBuildLampTable(&source, nullptr, lamps.data(), map.data(), &mapping) ==
            ClaStatusOk)
        {
            std::printf("  mapped bounding box: %u x %u x %u um\n",
                        mapping.Attributes.BoundingBoxWidthInMicrometers,
                        mapping.Attributes.BoundingBoxHeightInMicrometers,
                        mapping.Attributes.BoundingBoxDepthInMicrometers);
            std::printf("  mapped lamp count  : %u\n", mapping.Attributes.LampCount);
        }
        else
        {
            std::printf("  mapping: FAILED -- this device would be skipped\n");
        }
    }
    std::printf("\n");
}

/* Set every LED on a device to one colour. */
BackendStatus Fill(ICorsairBackend &backend, const BackendDeviceInfo &info,
                   cla_u8 r, cla_u8 g, cla_u8 b)
{
    std::vector<ClaSourceLed> leds;
    const BackendStatus status = backend.GetLamps(info.Id, leds);
    if (status != BackendStatus::Ok)
    {
        return status;
    }

    std::vector<BackendLedColor> colors;
    colors.reserve(leds.size());
    for (const ClaSourceLed &led : leds)
    {
        BackendLedColor color;
        color.LedId = led.LedId;
        color.Red = r;
        color.Green = g;
        color.Blue = b;
        color.Alpha = 255;
        colors.push_back(color);
    }
    return backend.SetLampColors(info.Id, colors);
}

int RunColorTest(ICorsairBackend &backend, const std::vector<BackendDeviceInfo> &devices)
{
    struct Step { const char *Name; cla_u8 R, G, B; };
    const Step steps[] = {
        { "red",     255,   0,   0 },
        { "green",     0, 255,   0 },
        { "blue",      0,   0, 255 },
        { "white",   255, 255, 255 },
        { "arbitrary (200,64,180)", 200, 64, 180 },
        { "half brightness white", 128, 128, 128 },
        { "off",       0,   0,   0 },
    };

    std::printf("Colour test\n");
    std::printf("-----------\n");
    std::printf("Watch the hardware: each step should be visible for one second.\n\n");

    int failures = 0;
    for (const Step &step : steps)
    {
        std::printf("  %-26s ", step.Name);
        std::fflush(stdout);

        for (const BackendDeviceInfo &info : devices)
        {
            const BackendStatus status = Fill(backend, info, step.R, step.G, step.B);
            if (status != BackendStatus::Ok)
            {
                std::printf("FAILED on %s: %s\n", info.Model.c_str(), ToString(status));
                failures++;
                goto nextStep;
            }
        }
        std::printf("ok\n");
    nextStep:
        Sleep(1000);
        if (g_stop.load())
        {
            break;
        }
    }

    /*
     * Restore by releasing control rather than by writing a colour: handing
     * the device back to iCUE lets the user's own profile resume, which is
     * closer to "the previous state" than anything this tool could paint.
     */
    std::printf("\n  releasing control back to iCUE ... ");
    for (const BackendDeviceInfo &info : devices)
    {
        backend.ReleaseControl(info.Id);
    }
    std::printf("done\n\n");

    return failures;
}

int RunWatch(ICorsairBackend &backend)
{
    std::printf("Watching for session and device events. Ctrl+C to stop.\n");
    std::printf("Try closing and reopening iCUE, and unplugging a device.\n\n");

    backend.SubscribeEvents([](const BackendEvent &event) {
        switch (event.Kind)
        {
        case BackendEventKind::SessionStateChanged:
            std::printf("  [event] session %s\n",
                        event.SessionConnected ? "CONNECTED" : "DISCONNECTED");
            break;
        case BackendEventKind::DeviceConnected:
            std::printf("  [event] device connected: %s\n", event.DeviceId.c_str());
            break;
        case BackendEventKind::DeviceDisconnected:
            std::printf("  [event] device disconnected: %s\n", event.DeviceId.c_str());
            break;
        }
        std::fflush(stdout);
    });

    bool wasConnected = backend.IsConnected();
    while (!g_stop.load())
    {
        Sleep(500);

        const bool connected = backend.IsConnected();
        if (connected != wasConnected)
        {
            std::printf("  [poll ] IsConnected() is now %s\n", connected ? "true" : "false");
            if (connected)
            {
                /* Re-enumerating after a reconnect is exactly what the service
                 * does, so exercise it here too. */
                std::vector<BackendDeviceInfo> devices;
                if (backend.EnumerateDevices(devices) == BackendStatus::Ok)
                {
                    std::printf("  [poll ] %zu device(s) visible after reconnect\n",
                                devices.size());
                }
            }
            wasConnected = connected;
        }
    }
    std::printf("\nStopped.\n");
    return 0;
}

} // namespace

int main(int argc, char **argv)
{
    bool testColors = false;
    bool watch = false;
    bool verbose = false;
    bool useFake = false;
    std::string only;

    for (int i = 1; i < argc; ++i)
    {
        const std::string argument = argv[i];
        if (argument == "--test-colors" || argument == "--test-colours") testColors = true;
        else if (argument == "--watch")   watch = true;
        else if (argument == "--verbose") verbose = true;
        else if (argument == "--fake")    useFake = true;
        else if (argument == "--device" && i + 1 < argc) only = argv[++i];
        else if (argument == "--help" || argument == "-h")
        {
            std::printf(
                "CorsairProbe -- verify the Corsair iCUE SDK backend\n\n"
                "  --test-colors   run the colour sequence on every device\n"
                "  --watch         report session and device events until Ctrl+C\n"
                "  --verbose       print every LED id and position\n"
                "  --device <id>   restrict to one device id\n"
                "  --fake          use the built-in fake backend (no iCUE needed)\n");
            return 0;
        }
        else
        {
            std::fprintf(stderr, "Unknown option: %s\n", argument.c_str());
            return 2;
        }
    }

    SetConsoleCtrlHandler(ControlHandler, TRUE);
    Logger::Instance().SetLevel(verbose ? LogLevel::Debug : LogLevel::Warning);

    std::printf("CorsairProbe\n");
    std::printf("============\n\n");

    std::unique_ptr<ICorsairBackend> backend;
    if (useFake)
    {
        auto fake = std::make_unique<FakeBackend>();
        fake->AddKeyboard("fake-keyboard-1", 6, 18);
        backend = std::move(fake);
        std::printf("Using the fake backend.\n\n");
    }
    else
    {
        ReportSdkSearch();
        backend = std::make_unique<IcueSdkBackend>();
    }

    if (backend->Initialize() != BackendStatus::Ok && !useFake)
    {
        std::printf("Could not initialise the iCUE SDK.\n\n");
        std::printf("Check that:\n");
        std::printf("  - Corsair iCUE is installed and running;\n");
        std::printf("  - 'Enable SDK' is on in iCUE (Settings -> software and games);\n");
        std::printf("  - or set CORSAIR_ICUE_SDK_PATH to the folder holding the SDK DLL.\n");
        return 1;
    }

    if (!WaitForConnection(*backend, 5000))
    {
        std::printf("SDK loaded but iCUE did not accept a connection within 5s.\n");
        std::printf("iCUE is probably not running, or third-party control is disabled.\n");
        if (!watch)
        {
            return 1;
        }
    }

    if (auto *sdk = dynamic_cast<IcueSdkBackend *>(backend.get()))
    {
        std::printf("Connected. SDK: %s\n", sdk->SdkPath().c_str());
        const std::string version = sdk->ServerVersion();
        if (!version.empty())
        {
            std::printf("iCUE version: %s\n", version.c_str());
        }
        std::printf("\n");
    }

    std::vector<BackendDeviceInfo> devices;
    const BackendStatus status = backend->EnumerateDevices(devices);
    if (status != BackendStatus::Ok)
    {
        std::printf("EnumerateDevices failed: %s\n", ToString(status));
        return 1;
    }

    if (!only.empty())
    {
        std::vector<BackendDeviceInfo> filtered;
        for (const BackendDeviceInfo &info : devices)
        {
            if (info.Id == only)
            {
                filtered.push_back(info);
            }
        }
        if (filtered.empty())
        {
            std::printf("No device with id '%s'.\n", only.c_str());
            return 1;
        }
        devices.swap(filtered);
    }

    std::printf("%zu Corsair device(s)\n", devices.size());
    std::printf("=====================\n\n");
    for (const BackendDeviceInfo &info : devices)
    {
        PrintDevice(*backend, info, verbose);
    }

    if (devices.empty())
    {
        std::printf("No devices reported. If iCUE shows devices but this does not,\n"
                    "check that SDK access is enabled in iCUE's settings.\n");
        return 1;
    }

    int result = 0;
    if (testColors)
    {
        for (const BackendDeviceInfo &info : devices)
        {
            backend->RequestControl(info.Id, false);
        }
        result = RunColorTest(*backend, devices);
    }

    if (watch)
    {
        RunWatch(*backend);
    }

    backend->Shutdown();

    if (result == 0)
    {
        std::printf("Probe completed successfully.\n");
    }
    else
    {
        std::printf("Probe completed with %d failure(s).\n", result);
    }
    return result;
}
