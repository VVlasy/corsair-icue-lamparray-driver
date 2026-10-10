/*
 * Copyright (c) Corsair Dynamic Lighting Bridge contributors.
 * SPDX-License-Identifier: MIT
 *
 * LampArrayTest -- inspect and drive the device through Windows itself
 * (project Phase 4).
 *
 * Where HidTestClient talks HID directly, this uses Windows.Devices.Lights --
 * the same API Windows Dynamic Lighting and every Dynamic Lighting-aware app
 * use.  That difference is the whole point: a device can be perfectly correct
 * at the HID level and still be invisible or misinterpreted here, and this is
 * where that shows up.
 *
 * It answers:
 *   - does Windows discover the device as a LampArray at all;
 *   - what lamp count, kind, bounding box and per-lamp info does Windows see;
 *   - does Windows accept colour writes, and do they reach the hardware.
 *
 * Windows only.  Needs the Windows SDK (C++/WinRT).
 *
 * Usage:
 *   LampArrayTest                list every LampArray Windows can see
 *   LampArrayTest --verbose      include every lamp's info
 *   LampArrayTest --test-colors  run a colour sequence through Windows
 *   LampArrayTest --rainbow      run a moving effect using lamp positions
 */

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Devices.Lights.h>
#include <winrt/Windows.UI.h>

#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>
#include <cmath>
#include <atomic>

using namespace winrt;
using namespace winrt::Windows::Devices::Enumeration;
using namespace winrt::Windows::Devices::Lights;
using namespace winrt::Windows::UI;

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

const char *KindName(LampArrayKind kind)
{
    switch (kind)
    {
    case LampArrayKind::Undefined:      return "Undefined";
    case LampArrayKind::Keyboard:       return "Keyboard";
    case LampArrayKind::Mouse:          return "Mouse";
    case LampArrayKind::GameController: return "GameController";
    case LampArrayKind::Peripheral:     return "Peripheral";
    case LampArrayKind::Scene:          return "Scene";
    case LampArrayKind::Notification:   return "Notification";
    case LampArrayKind::Chassis:        return "Chassis";
    case LampArrayKind::Wearable:       return "Wearable";
    case LampArrayKind::Furniture:      return "Furniture";
    case LampArrayKind::Art:            return "Art";
    default:                            return "(unknown)";
    }
}

std::string Narrow(const hstring &text)
{
    const std::wstring_view view = text;
    if (view.empty())
    {
        return std::string();
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, view.data(),
                                         static_cast<int>(view.size()),
                                         nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, view.data(), static_cast<int>(view.size()),
                        result.data(), size, nullptr, nullptr);
    return result;
}

struct FoundLampArray
{
    LampArray Array{nullptr};
    std::string Id;
    std::string Name;
};

std::vector<FoundLampArray> FindLampArrays()
{
    std::vector<FoundLampArray> found;

    const hstring selector = LampArray::GetDeviceSelector();
    const DeviceInformationCollection devices =
        DeviceInformation::FindAllAsync(selector).get();

    for (const DeviceInformation &information : devices)
    {
        try
        {
            LampArray array = LampArray::FromIdAsync(information.Id()).get();
            if (array == nullptr)
            {
                std::printf("  (could not open %s)\n", Narrow(information.Name()).c_str());
                continue;
            }
            found.push_back({array, Narrow(information.Id()), Narrow(information.Name())});
        }
        catch (const hresult_error &error)
        {
            std::printf("  (error opening %s: 0x%08X %s)\n",
                        Narrow(information.Name()).c_str(),
                        static_cast<unsigned>(error.code()),
                        Narrow(error.message()).c_str());
        }
    }
    return found;
}

void Describe(const FoundLampArray &found, bool verbose)
{
    const LampArray &array = found.Array;

    std::printf("LampArray: %s\n", found.Name.c_str());
    std::printf("  device id       : %s\n", found.Id.c_str());
    std::printf("  kind            : %s\n", KindName(array.LampArrayKind()));
    std::printf("  lamp count      : %d\n", array.LampCount());
    std::printf("  available       : %s\n", array.IsAvailable() ? "yes" : "no");
    std::printf("  hardware vendor : 0x%04X\n", array.HardwareVendorId());
    std::printf("  hardware product: 0x%04X\n", array.HardwareProductId());
    std::printf("  min update      : %lld us\n",
                static_cast<long long>(
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        array.MinUpdateInterval()).count()));

    const auto bounds = array.BoundingBox();
    std::printf("  bounding box    : %.1f x %.1f x %.1f mm\n",
                bounds.x * 1000.0, bounds.y * 1000.0, bounds.z * 1000.0);
    std::printf("  brightness      : %.2f\n", array.BrightnessLevel());

    if (!array.IsAvailable())
    {
        std::printf("\n  NOTE: Windows reports this device as unavailable. That usually\n"
                    "  means Dynamic Lighting is off, or another app has taken\n"
                    "  foreground control (Settings > Personalization > Dynamic Lighting).\n");
    }

    const int lampCount = array.LampCount();
    const int toShow = verbose ? lampCount : (lampCount < 8 ? lampCount : 8);

    if (toShow > 0)
    {
        std::printf("\n  %-8s %9s %9s %9s %7s %s\n",
                    "index", "X (mm)", "Y (mm)", "Z (mm)", "prog", "purposes");
        std::printf("  %-8s %9s %9s %9s %7s %s\n",
                    "-----", "------", "------", "------", "----", "--------");

        for (int i = 0; i < toShow; ++i)
        {
            const LampInfo info = array.GetLampInfo(i);
            const auto position = info.Position();
            std::printf("  %-8d %9.1f %9.1f %9.1f %7s 0x%02X\n",
                        info.Index(),
                        position.x * 1000.0, position.y * 1000.0, position.z * 1000.0,
                        /* WinRT has no IsProgrammable; a non-programmable lamp
                         * is one that reports a FixedColor. */
                        info.FixedColor() ? "no" : "yes",
                        static_cast<unsigned>(info.Purposes()));
        }
        if (!verbose && lampCount > toShow)
        {
            std::printf("  ... %d more (use --verbose)\n", lampCount - toShow);
        }
    }
    std::printf("\n");
}

int RunColorTest(const FoundLampArray &found)
{
    const LampArray &array = found.Array;

    std::printf("Colour test through Windows.Devices.Lights\n");
    std::printf("------------------------------------------\n");

    if (!array.IsAvailable())
    {
        std::printf("  device is unavailable; enable Dynamic Lighting and retry\n\n");
        return 1;
    }

    struct Step { const char *Name; uint8_t R, G, B; };
    const Step steps[] = {
        { "red",     255,   0,   0 },
        { "green",     0, 255,   0 },
        { "blue",      0,   0, 255 },
        { "white",   255, 255, 255 },
        { "arbitrary", 200, 64, 180 },
        { "off",       0,   0,   0 },
    };

    for (const Step &step : steps)
    {
        Color color{};
        color.A = 255;
        color.R = step.R;
        color.G = step.G;
        color.B = step.B;

        std::printf("  %-10s ... ", step.Name);
        std::fflush(stdout);
        array.SetColor(color);
        std::printf("sent\n");
        Sleep(1000);
        if (g_stop.load()) break;
    }

    /* Brightness is a Windows-side control; exercising it checks the whole
     * intensity path down to the hardware. */
    std::printf("\n  brightness sweep ... ");
    std::fflush(stdout);
    Color white{255, 255, 255, 255};
    array.SetColor(white);
    for (int step = 10; step >= 0 && !g_stop.load(); --step)
    {
        array.BrightnessLevel(static_cast<double>(step) / 10.0);
        Sleep(120);
    }
    array.BrightnessLevel(1.0);
    std::printf("done\n");

    Color off{255, 0, 0, 0};
    array.SetColor(off);
    std::printf("\n");
    return 0;
}

/*
 * A moving effect driven from the lamp positions Windows reports.
 *
 * This is the real test of the mapping layer: if positions are wrong, the
 * sweep will look scrambled, bunched, or will not move at all -- none of which
 * any amount of protocol testing would reveal.
 */
int RunRainbow(const FoundLampArray &found)
{
    const LampArray &array = found.Array;
    const int lampCount = array.LampCount();
    if (lampCount <= 0)
    {
        return 1;
    }

    std::printf("Position-driven sweep. Ctrl+C to stop.\n");
    std::printf("If the layout is mapped correctly this moves smoothly left to right.\n\n");

    std::vector<int> indices(static_cast<size_t>(lampCount));
    std::vector<Color> colors(static_cast<size_t>(lampCount));
    std::vector<double> normalisedX(static_cast<size_t>(lampCount));

    const auto bounds = array.BoundingBox();
    const double width = (bounds.x > 0.0) ? bounds.x : 1.0;

    for (int i = 0; i < lampCount; ++i)
    {
        indices[static_cast<size_t>(i)] = i;
        const LampInfo info = array.GetLampInfo(i);
        normalisedX[static_cast<size_t>(i)] = info.Position().x / width;
    }

    double phase = 0.0;
    while (!g_stop.load())
    {
        for (int i = 0; i < lampCount; ++i)
        {
            const double hue = std::fmod(normalisedX[static_cast<size_t>(i)] + phase, 1.0) * 6.0;
            const int sector = static_cast<int>(hue);
            const double fraction = hue - sector;
            const uint8_t high = 255;
            const auto rising = static_cast<uint8_t>(fraction * 255.0);
            const auto falling = static_cast<uint8_t>((1.0 - fraction) * 255.0);

            Color color{};
            color.A = 255;
            switch (sector)
            {
            case 0: color.R = high;    color.G = rising;  color.B = 0;       break;
            case 1: color.R = falling; color.G = high;    color.B = 0;       break;
            case 2: color.R = 0;       color.G = high;    color.B = rising;  break;
            case 3: color.R = 0;       color.G = falling; color.B = high;    break;
            case 4: color.R = rising;  color.G = 0;       color.B = high;    break;
            default:color.R = high;    color.G = 0;       color.B = falling; break;
            }
            colors[static_cast<size_t>(i)] = color;
        }

        array.SetColorsForIndices(colors, indices);

        phase += 0.01;
        if (phase >= 1.0) phase -= 1.0;
        Sleep(16);   /* ~60Hz */
    }

    Color off{255, 0, 0, 0};
    array.SetColor(off);
    std::printf("\nStopped.\n");
    return 0;
}

} // namespace

int main(int argc, char **argv)
{
    bool verbose = false;
    bool testColors = false;
    bool rainbow = false;

    for (int i = 1; i < argc; ++i)
    {
        const std::string argument = argv[i];
        if (argument == "--verbose") verbose = true;
        else if (argument == "--test-colors" || argument == "--test-colours") testColors = true;
        else if (argument == "--rainbow") rainbow = true;
        else if (argument == "--help" || argument == "-h")
        {
            std::printf(
                "LampArrayTest -- inspect LampArrays through Windows.Devices.Lights\n\n"
                "  --verbose       print every lamp's info\n"
                "  --test-colors   run a colour and brightness sequence\n"
                "  --rainbow       run a position-driven sweep until Ctrl+C\n");
            return 0;
        }
        else
        {
            std::fprintf(stderr, "Unknown option: %s\n", argument.c_str());
            return 2;
        }
    }

    SetConsoleCtrlHandler(ControlHandler, TRUE);
    init_apartment();

    std::printf("LampArrayTest\n");
    std::printf("=============\n\n");

    std::vector<FoundLampArray> found;
    try
    {
        found = FindLampArrays();
    }
    catch (const hresult_error &error)
    {
        std::printf("Enumeration failed: 0x%08X %s\n",
                    static_cast<unsigned>(error.code()), Narrow(error.message()).c_str());
        return 1;
    }

    std::printf("Windows reports %zu LampArray device(s)\n\n", found.size());

    if (found.empty())
    {
        std::printf("None found. Check that:\n");
        std::printf("  - the CorsairLampArray driver is installed and the service running;\n");
        std::printf("  - HidTestClient sees the HID device (if it does and this does not,\n");
        std::printf("    the problem is in how Windows reads the descriptor, not the driver);\n");
        std::printf("  - this is Windows 11 23H2 or later, which is where Dynamic\n");
        std::printf("    Lighting exists;\n");
        std::printf("  - Settings > Personalization > Dynamic Lighting is enabled.\n");
        return 1;
    }

    for (const FoundLampArray &array : found)
    {
        Describe(array, verbose);
    }

    int result = 0;
    if (testColors)
    {
        for (const FoundLampArray &array : found)
        {
            result += RunColorTest(array);
        }
    }
    if (rainbow && !found.empty())
    {
        result += RunRainbow(found.front());
    }

    return result;
}
