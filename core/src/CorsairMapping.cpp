/*
 * Copyright (c) Corsair Dynamic Lighting Bridge contributors.
 * SPDX-License-Identifier: MIT
 *
 * Corsair LED topology -> HID LampArray lamp table.  See CorsairMapping.h.
 *
 * User-mode only (uses floating point and the STL).
 */

#include "cla/CorsairMapping.h"
#include "cla/IpcContract.h"

#include <algorithm>
#include <vector>
#include <cmath>
#include <cstddef>

namespace {

constexpr cla_u32 kDefaultUpdateLatencyUs = 4000;    /* 4ms, typical for LEDs */
constexpr cla_u32 kDefaultMinUpdateIntervalUs = 16667; /* 60Hz */
constexpr cla_u32 kMinRowToleranceUm = 1000;         /* 1mm */
/* Floor for any bounding-box axis.  A zero-width box is rejected by
 * ClaValidateCreateRequest and would break host-side effect positioning. */
constexpr cla_u32 kMinBoundingBoxUm = 1000;          /* 1mm */

struct SortableLed
{
    cla_u32 LedId;
    cla_u32 X;   /* micrometres, already origin-shifted */
    cla_u32 Y;
    cla_u32 SourceIndex;
};

} // namespace

void ClaMappingOptionsInit(ClaMappingOptions *options)
{
    if (options == nullptr)
    {
        return;
    }
    *options = ClaMappingOptions{};
    options->RowToleranceMicrometers = 0;      /* derive from bounding box */
    options->NominalDepthMicrometers = 0;      /* per-kind default */
    options->UpdateLatencyMicroseconds = kDefaultUpdateLatencyUs;
    options->MinUpdateIntervalMicroseconds = kDefaultMinUpdateIntervalUs;
    options->PreserveSourceOrder = 0;
}

cla_u32 ClaDefaultLampPurposeForKind(cla_u32 lampArrayKind)
{
    switch (lampArrayKind)
    {
    case ClaLampArrayKindKeyboard:
        /* Keys are control lamps: the host may bind effects to keypresses. */
        return CLA_LAMP_PURPOSE_CONTROL;
    case ClaLampArrayKindMouse:
    case ClaLampArrayKindGameController:
        return CLA_LAMP_PURPOSE_CONTROL | CLA_LAMP_PURPOSE_ACCENT;
    case ClaLampArrayKindChassis:
        /* Fans, RAM, coolers: decorative, not user-facing controls. */
        return CLA_LAMP_PURPOSE_ACCENT;
    case ClaLampArrayKindWearable:
    case ClaLampArrayKindPeripheral:
        return CLA_LAMP_PURPOSE_ACCENT;
    default:
        return CLA_LAMP_PURPOSE_ACCENT;
    }
}

void ClaNominalSizeForKind(cla_u32 lampArrayKind,
                           cla_u32 *widthMicrometers,
                           cla_u32 *heightMicrometers,
                           cla_u32 *depthMicrometers)
{
    /* Rough real-world envelopes, in micrometres.  Used to give logical-unit
     * devices a physical scale and to floor degenerate boxes.  Only the
     * relative proportions matter to host effects. */
    cla_u32 w = 100000, h = 100000, d = 30000;

    switch (lampArrayKind)
    {
    case ClaLampArrayKindKeyboard:       w = 440000; h = 150000; d = 35000;  break;
    case ClaLampArrayKindMouse:          w =  70000; h = 125000; d = 42000;  break;
    case ClaLampArrayKindGameController: w = 160000; h = 105000; d = 60000;  break;
    case ClaLampArrayKindPeripheral:     w = 350000; h = 300000; d = 5000;   break; /* mousepad */
    case ClaLampArrayKindWearable:       w = 180000; h = 200000; d = 90000;  break; /* headset */
    case ClaLampArrayKindChassis:        w = 120000; h = 120000; d = 25000;  break; /* fan / RAM */
    default: break;
    }

    if (widthMicrometers) *widthMicrometers = w;
    if (heightMicrometers) *heightMicrometers = h;
    if (depthMicrometers) *depthMicrometers = d;
}

ClaStatus ClaBuildLampTable(
    const ClaSourceDevice *device,
    const ClaMappingOptions *options,
    ClaLampInfo *lamps,
    cla_u32 *lampIdToLedId,
    ClaMappingResult *result)
{
    if (device == nullptr || lamps == nullptr || lampIdToLedId == nullptr ||
        result == nullptr || device->Leds == nullptr)
    {
        return ClaStatusInvalidArgument;
    }
    if (device->LedCount == 0 || device->LedCount > CLA_MAX_LAMPS_PER_ARRAY)
    {
        return ClaStatusInvalidArgument;
    }

    ClaMappingOptions effective;
    if (options != nullptr)
    {
        effective = *options;
    }
    else
    {
        ClaMappingOptionsInit(&effective);
    }
    if (effective.UpdateLatencyMicroseconds == 0)
        effective.UpdateLatencyMicroseconds = kDefaultUpdateLatencyUs;
    if (effective.MinUpdateIntervalMicroseconds == 0)
        effective.MinUpdateIntervalMicroseconds = kDefaultMinUpdateIntervalUs;

    const cla_u32 ledCount = device->LedCount;

    /* ---- 1. Find the extent of the source coordinates. ---------------- */
    /* Each coordinate is checked for finiteness as it is read, NOT by testing
     * the resulting min/max.  std::min/std::max propagate their first argument
     * when the second is NaN, so a NaN coordinate would vanish from the
     * aggregate and then silently clamp to position 0 later on -- a wrong
     * layout with no error anywhere. */
    for (cla_u32 i = 0; i < ledCount; ++i)
    {
        if (!std::isfinite(device->Leds[i].X) || !std::isfinite(device->Leds[i].Y))
        {
            return ClaStatusInvalidArgument;
        }
    }

    double minX = device->Leds[0].X, maxX = device->Leds[0].X;
    double minY = device->Leds[0].Y, maxY = device->Leds[0].Y;
    for (cla_u32 i = 1; i < ledCount; ++i)
    {
        minX = std::min(minX, device->Leds[i].X);
        maxX = std::max(maxX, device->Leds[i].X);
        minY = std::min(minY, device->Leds[i].Y);
        maxY = std::max(maxY, device->Leds[i].Y);
    }

    const double spanX = maxX - minX;
    const double spanY = maxY - minY;

    cla_u32 nominalW = 0, nominalH = 0, nominalD = 0;
    ClaNominalSizeForKind(device->LampArrayKind, &nominalW, &nominalH, &nominalD);

    /* ---- 2. Establish a scale from source units to micrometres. ------- */
    double scaleX, scaleY;
    if (device->Units == ClaSourceUnitsMillimeters)
    {
        scaleX = 1000.0;   /* mm -> um */
        scaleY = 1000.0;
    }
    else
    {
        /* Logical units: no physical meaning, so scale the logical extent onto
         * the nominal envelope for this device kind.  A single shared factor
         * is used for both axes so the layout is not distorted. */
        const double largestSpan = std::max(spanX, spanY);
        if (largestSpan > 0.0)
        {
            const double target = static_cast<double>(std::max(nominalW, nominalH));
            scaleX = scaleY = target / largestSpan;
        }
        else
        {
            scaleX = scaleY = 1000.0;   /* all LEDs coincident; scale is moot */
        }
    }

    /* ---- 3. Shift to a (0,0) origin and convert to micrometres. ------- */
    std::vector<SortableLed> sorted(ledCount);
    for (cla_u32 i = 0; i < ledCount; ++i)
    {
        const double x = (device->Leds[i].X - minX) * scaleX;
        const double y = (device->Leds[i].Y - minY) * scaleY;

        /* Clamp: positions must be non-negative offsets from the origin and
         * must fit in the u32 wire field. */
        const double clampedX = std::min(std::max(x, 0.0), 4000000000.0);
        const double clampedY = std::min(std::max(y, 0.0), 4000000000.0);

        sorted[i].LedId = device->Leds[i].LedId;
        sorted[i].X = static_cast<cla_u32>(std::llround(clampedX));
        sorted[i].Y = static_cast<cla_u32>(std::llround(clampedY));
        sorted[i].SourceIndex = i;
    }

    /* ---- 4. Bounding box. -------------------------------------------- */
    cla_u32 boxW = 0, boxH = 0;
    for (const SortableLed &led : sorted)
    {
        boxW = std::max(boxW, led.X);
        boxH = std::max(boxH, led.Y);
    }
    /* Every axis must be positive (HUT 1.4 s26.2).  A single-LED device, or a
     * strip whose LEDs share a Y, legitimately produces a zero span. */
    boxW = std::max(boxW, kMinBoundingBoxUm);
    boxH = std::max(boxH, kMinBoundingBoxUm);

    cla_u32 boxD = effective.NominalDepthMicrometers;
    if (boxD == 0) boxD = nominalD;
    boxD = std::max(boxD, kMinBoundingBoxUm);

    /* ---- 5. Order the lamps. ------------------------------------------ */
    if (!effective.PreserveSourceOrder)
    {
        cla_u32 rowTolerance = effective.RowToleranceMicrometers;
        if (rowTolerance == 0)
        {
            rowTolerance = std::max(boxH / 20u, kMinRowToleranceUm);
        }

        /* Sort by Y, then group into rows within the tolerance, then sort each
         * row by X.  This yields the top-left-first, row-major order that
         * makes LampRangeUpdateReport effective. */
        std::stable_sort(sorted.begin(), sorted.end(),
                         [](const SortableLed &a, const SortableLed &b) {
                             return a.Y < b.Y;
                         });

        size_t rowStart = 0;
        while (rowStart < sorted.size())
        {
            const cla_u32 rowY = sorted[rowStart].Y;
            size_t rowEnd = rowStart + 1;
            while (rowEnd < sorted.size() && (sorted[rowEnd].Y - rowY) <= rowTolerance)
            {
                ++rowEnd;
            }
            std::stable_sort(sorted.begin() + static_cast<std::ptrdiff_t>(rowStart),
                             sorted.begin() + static_cast<std::ptrdiff_t>(rowEnd),
                             [](const SortableLed &a, const SortableLed &b) {
                                 return a.X < b.X;
                             });
            rowStart = rowEnd;
        }
    }

    /* ---- 6. Emit the lamp table. -------------------------------------- */
    const cla_u32 purposes = ClaDefaultLampPurposeForKind(device->LampArrayKind);

    for (cla_u32 i = 0; i < ledCount; ++i)
    {
        ClaLampInfo &lamp = lamps[i];
        lamp = ClaLampInfo{};
        lamp.PositionXInMicrometers = std::min(sorted[i].X, boxW);
        lamp.PositionYInMicrometers = std::min(sorted[i].Y, boxH);
        /* Corsair reports 2D positions only; place lamps on the front face. */
        lamp.PositionZInMicrometers = 0;
        lamp.UpdateLatencyInMicroseconds = effective.UpdateLatencyMicroseconds;
        lamp.LampPurposes = purposes;

        /* Full 8-bit range on every channel.  This matters: HUT 1.4 s26.11
         * makes any channel value above its LevelCount an error that voids the
         * whole report, so declaring 255 everywhere means no colour Windows
         * can express is ever rejected.  Corsair LEDs are 8-bit RGB, so this
         * is also accurate. */
        lamp.RedLevelCount = 255;
        lamp.GreenLevelCount = 255;
        lamp.BlueLevelCount = 255;
        lamp.IntensityLevelCount = 255;
        lamp.IsProgrammable = 1;

        /* InputBinding would associate a lamp with a key or mouse button.  The
         * iCUE SDK exposes LED ids, not HID usages, and a wrong binding is
         * worse than none (HUT 1.4 s26.8.3: non-zero values are used by the
         * host), so it is left at 0 -- "not associated". */
        lamp.InputBinding = 0;

        lampIdToLedId[i] = sorted[i].LedId;
    }

    result->Attributes = ClaLampArrayAttributes{};
    result->Attributes.LampCount = static_cast<cla_u16>(ledCount);
    result->Attributes.BoundingBoxWidthInMicrometers = boxW;
    result->Attributes.BoundingBoxHeightInMicrometers = boxH;
    result->Attributes.BoundingBoxDepthInMicrometers = boxD;
    result->Attributes.LampArrayKind = device->LampArrayKind;
    result->Attributes.MinUpdateIntervalInMicroseconds = effective.MinUpdateIntervalMicroseconds;
    result->LampIdToLedId = lampIdToLedId;
    result->LampCount = ledCount;

    return ClaStatusOk;
}
