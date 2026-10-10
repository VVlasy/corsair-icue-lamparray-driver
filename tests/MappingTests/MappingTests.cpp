/*
 * Copyright (c) Corsair Dynamic Lighting Bridge contributors.
 * SPDX-License-Identifier: MIT
 *
 * Corsair topology -> LampArray mapping (project Phase 7).
 *
 * The mapping decides what Windows believes the device physically looks like,
 * so a mistake here shows up as effects sweeping the wrong way or bunching in
 * a corner -- and never as an error.  Hence the emphasis on the geometric
 * invariants and on degenerate topologies.
 */

#include "ClaTest.h"

#include "cla/CorsairMapping.h"
#include "cla/IpcContract.h"

#include <vector>
#include <algorithm>
#include <limits>

namespace {

struct Mapped
{
    std::vector<ClaLampInfo> Lamps;
    std::vector<cla_u32> LampIdToLedId;
    ClaMappingResult Result{};
    ClaStatus Status = ClaStatusOk;
};

Mapped Map(const std::vector<ClaSourceLed> &leds, cla_u32 kind,
           ClaSourceUnits units = ClaSourceUnitsMillimeters,
           const ClaMappingOptions *options = nullptr,
           cla_u32 scope = ClaSourceScopeComponent)
{
    Mapped mapped;
    mapped.Lamps.resize(leds.empty() ? 1 : leds.size());
    mapped.LampIdToLedId.resize(leds.empty() ? 1 : leds.size());

    ClaSourceDevice device{};
    device.LampArrayKind = kind;
    device.Units = units;
    device.Scope = scope;
    device.LedCount = static_cast<cla_u32>(leds.size());
    device.Leds = leds.data();

    mapped.Status = ClaBuildLampTable(&device, options, mapped.Lamps.data(),
                                      mapped.LampIdToLedId.data(), &mapped.Result);
    return mapped;
}

/* A small keyboard-like grid: 6 rows x 15 columns on a 19.05mm pitch, given
 * to the mapper in deliberately scrambled order. */
std::vector<ClaSourceLed> MakeKeyboardGrid(int rows = 6, int columns = 15)
{
    std::vector<ClaSourceLed> leds;
    for (int row = 0; row < rows; ++row)
    {
        for (int column = 0; column < columns; ++column)
        {
            ClaSourceLed led{};
            led.LedId = static_cast<cla_u32>(row * 100 + column);
            led.X = 10.0 + column * 19.05;
            led.Y = 10.0 + row * 19.05;
            leds.push_back(led);
        }
    }
    /* Scramble: the SDK does not promise geometric ordering. */
    for (size_t i = 0; i < leds.size(); ++i)
    {
        std::swap(leds[i], leds[(i * 7 + 3) % leds.size()]);
    }
    return leds;
}

} // namespace

/* ---------------------------------------------------------------- */
/* Basic geometry                                                    */
/* ---------------------------------------------------------------- */

CLA_TEST(Mapping_MillimetresBecomeMicrometres)
{
    /* Two LEDs 100mm apart -> 100000um apart. */
    const std::vector<ClaSourceLed> leds = {
        {1, 0.0, 0.0},
        {2, 100.0, 0.0},
    };
    const Mapped mapped = Map(leds, ClaLampArrayKindKeyboard);
    CLA_CHECK_EQ(static_cast<int>(mapped.Status), static_cast<int>(ClaStatusOk));
    CLA_CHECK_EQ(mapped.Lamps[0].PositionXInMicrometers, 0u);
    CLA_CHECK_EQ(mapped.Lamps[1].PositionXInMicrometers, 100000u);
    CLA_CHECK_EQ(mapped.Result.Attributes.BoundingBoxWidthInMicrometers, 100000u);
}

/* HUT 1.4 s26.7: positions are positive offsets from the box origin, so the
 * minimum coordinate must be shifted to zero. */
CLA_TEST(Mapping_ShiftsOriginToZero)
{
    const std::vector<ClaSourceLed> leds = {
        {1, 50.0, 30.0},
        {2, 150.0, 80.0},
    };
    const Mapped mapped = Map(leds, ClaLampArrayKindKeyboard);
    CLA_CHECK_EQ(mapped.Lamps[0].PositionXInMicrometers, 0u);
    CLA_CHECK_EQ(mapped.Lamps[0].PositionYInMicrometers, 0u);
    CLA_CHECK_EQ(mapped.Lamps[1].PositionXInMicrometers, 100000u);
    CLA_CHECK_EQ(mapped.Lamps[1].PositionYInMicrometers, 50000u);
}

/* Every lamp must sit inside the declared box, or ClaValidateCreateRequest
 * rejects the device outright. */
CLA_TEST(Mapping_AllLampsFallInsideBoundingBox)
{
    const Mapped mapped = Map(MakeKeyboardGrid(), ClaLampArrayKindKeyboard);
    const ClaLampArrayAttributes &attributes = mapped.Result.Attributes;

    for (const ClaLampInfo &lamp : mapped.Lamps)
    {
        CLA_CHECK(lamp.PositionXInMicrometers <= attributes.BoundingBoxWidthInMicrometers);
        CLA_CHECK(lamp.PositionYInMicrometers <= attributes.BoundingBoxHeightInMicrometers);
        CLA_CHECK(lamp.PositionZInMicrometers <= attributes.BoundingBoxDepthInMicrometers);
    }
}

CLA_TEST(Mapping_BoundingBoxIsPositiveInEveryAxis)
{
    for (cla_u32 kind : {ClaLampArrayKindKeyboard, ClaLampArrayKindMouse,
                         ClaLampArrayKindChassis, ClaLampArrayKindWearable,
                         ClaLampArrayKindPeripheral})
    {
        const Mapped mapped = Map(MakeKeyboardGrid(2, 3), kind);
        CLA_CHECK(mapped.Result.Attributes.BoundingBoxWidthInMicrometers > 0u);
        CLA_CHECK(mapped.Result.Attributes.BoundingBoxHeightInMicrometers > 0u);
        CLA_CHECK(mapped.Result.Attributes.BoundingBoxDepthInMicrometers > 0u);
    }
}

/* ---------------------------------------------------------------- */
/* Lamp ordering (HUT 1.4 s26.8.1)                                   */
/* ---------------------------------------------------------------- */

/* "It is recommended that LampIds are assigned to Lamps in a methodical
 * manner (e.g. grid, starting from top-left)".  Scrambled input must come out
 * row-major. */
CLA_TEST(Mapping_SortsScrambledLedsIntoRowMajorOrder)
{
    const Mapped mapped = Map(MakeKeyboardGrid(6, 15), ClaLampArrayKindKeyboard);
    CLA_CHECK_EQ(static_cast<int>(mapped.Status), static_cast<int>(ClaStatusOk));

    /* Y must be non-decreasing across the whole table... */
    for (size_t i = 1; i < mapped.Lamps.size(); ++i)
    {
        CLA_CHECK(mapped.Lamps[i].PositionYInMicrometers >=
                  mapped.Lamps[i - 1].PositionYInMicrometers);
    }

    /* ...and within each row of 15, X must be strictly increasing. */
    for (size_t row = 0; row < 6; ++row)
    {
        for (size_t column = 1; column < 15; ++column)
        {
            const size_t index = row * 15 + column;
            CLA_CHECK(mapped.Lamps[index].PositionXInMicrometers >
                      mapped.Lamps[index - 1].PositionXInMicrometers);
            CLA_CHECK_EQ(mapped.Lamps[index].PositionYInMicrometers,
                         mapped.Lamps[row * 15].PositionYInMicrometers);
        }
    }
}

CLA_TEST(Mapping_PreserveSourceOrderSkipsSorting)
{
    ClaMappingOptions options{};
    ClaMappingOptionsInit(&options);
    options.PreserveSourceOrder = 1;

    const std::vector<ClaSourceLed> leds = {
        {10, 100.0, 100.0},
        {11, 0.0, 0.0},
    };
    const Mapped mapped = Map(leds, ClaLampArrayKindKeyboard,
                              ClaSourceUnitsMillimeters, &options);
    CLA_CHECK_EQ(mapped.LampIdToLedId[0], 10u);
    CLA_CHECK_EQ(mapped.LampIdToLedId[1], 11u);
}

/* The LampId -> LED id map is what the service uses to turn a frame back into
 * Corsair colours; if sorting lost it, every lamp would drive the wrong LED. */
CLA_TEST(Mapping_LedIdMapFollowsSorting)
{
    const std::vector<ClaSourceLed> leds = {
        {77, 50.0, 0.0},   /* right */
        {88, 0.0, 0.0},    /* left  */
    };
    const Mapped mapped = Map(leds, ClaLampArrayKindKeyboard);
    /* Sorted left-to-right, so LampId 0 is the LED that was second. */
    CLA_CHECK_EQ(mapped.LampIdToLedId[0], 88u);
    CLA_CHECK_EQ(mapped.LampIdToLedId[1], 77u);
    CLA_CHECK_EQ(mapped.Lamps[0].PositionXInMicrometers, 0u);
}

CLA_TEST(Mapping_LedIdMapIsAPermutationOfTheInput)
{
    const auto leds = MakeKeyboardGrid(4, 8);
    const Mapped mapped = Map(leds, ClaLampArrayKindKeyboard);

    std::vector<cla_u32> input, output;
    for (const auto &led : leds) input.push_back(led.LedId);
    for (cla_u32 id : mapped.LampIdToLedId) output.push_back(id);
    std::sort(input.begin(), input.end());
    std::sort(output.begin(), output.end());
    CLA_CHECK(input == output);
}

/* Rows are grouped within a tolerance, so a keyboard whose keys are not
 * perfectly aligned still yields clean rows. */
CLA_TEST(Mapping_RowGroupingToleratesSlightYJitter)
{
    std::vector<ClaSourceLed> leds;
    for (int column = 0; column < 10; ++column)
    {
        ClaSourceLed led{};
        led.LedId = static_cast<cla_u32>(column);
        led.X = column * 19.05;
        /* Sub-millimetre jitter, far below the default tolerance. */
        led.Y = 10.0 + ((column % 2) ? 0.3 : 0.0);
        leds.push_back(led);
    }
    /* Reverse so sorting has work to do. */
    std::reverse(leds.begin(), leds.end());

    const Mapped mapped = Map(leds, ClaLampArrayKindKeyboard);
    for (size_t i = 1; i < mapped.Lamps.size(); ++i)
    {
        CLA_CHECK(mapped.Lamps[i].PositionXInMicrometers >
                  mapped.Lamps[i - 1].PositionXInMicrometers);
    }
}

/* ---------------------------------------------------------------- */
/* Logical units                                                     */
/* ---------------------------------------------------------------- */

/* The SDK reports DIY devices, headsets and coolers in logical units, which
 * have no millimetre meaning.  They are scaled onto the nominal envelope for
 * the device kind instead. */
CLA_TEST(Mapping_LogicalUnitsScaleToNominalEnvelope)
{
    const std::vector<ClaSourceLed> leds = {
        {1, 0.0, 0.0},
        {2, 1.0, 0.0},
        {3, 0.0, 1.0},
    };
    const Mapped mapped = Map(leds, ClaLampArrayKindChassis, ClaSourceUnitsLogical);
    CLA_CHECK_EQ(static_cast<int>(mapped.Status), static_cast<int>(ClaStatusOk));

    cla_u32 nominalW = 0, nominalH = 0, nominalD = 0;
    ClaNominalSizeForKind(ClaLampArrayKindChassis, &nominalW, &nominalH, &nominalD);

    /* A logical span of 1.0 maps onto the larger nominal dimension. */
    const cla_u32 expected = std::max(nominalW, nominalH);
    CLA_CHECK_EQ(mapped.Result.Attributes.BoundingBoxWidthInMicrometers, expected);
}

/* Scaling must be uniform across axes so the layout is not stretched. */
CLA_TEST(Mapping_LogicalScalingPreservesAspectRatio)
{
    const std::vector<ClaSourceLed> leds = {
        {1, 0.0, 0.0},
        {2, 2.0, 0.0},
        {3, 0.0, 1.0},
    };
    const Mapped mapped = Map(leds, ClaLampArrayKindChassis, ClaSourceUnitsLogical);
    const auto &attributes = mapped.Result.Attributes;
    /* Source aspect is 2:1, so the box must be too (within rounding). */
    const double ratio = static_cast<double>(attributes.BoundingBoxWidthInMicrometers) /
                         static_cast<double>(attributes.BoundingBoxHeightInMicrometers);
    CLA_CHECK(ratio > 1.99 && ratio < 2.01);
}

/* ---------------------------------------------------------------- */
/* Whole-case controllers                                            */
/* ---------------------------------------------------------------- */

/* Four fans in a column, in logical units: the shape a Commander or Lighting
 * Node reports for front-mounted fans. */
namespace {

std::vector<ClaSourceLed> MakeFanColumn()
{
    std::vector<ClaSourceLed> leds;
    for (int fan = 0; fan < 4; ++fan)
    {
        leds.push_back({static_cast<cla_u32>(fan), 0.5, fan * 1.0});
        leds.push_back({static_cast<cla_u32>(fan + 10), 1.5, fan * 1.0 + 0.5});
    }
    return leds;
}

} // namespace

/* The bug this guards: a controller driving every fan in the case was scaled
 * onto one 120mm fan, so Windows believed the whole case was 12cm tall. */
CLA_TEST(Mapping_WholeCaseControllerGetsCaseSizedEnvelope)
{
    const Mapped mapped = Map(MakeFanColumn(), ClaLampArrayKindChassis,
                              ClaSourceUnitsLogical, nullptr, ClaSourceScopeWholeCase);
    CLA_CHECK_EQ(static_cast<int>(mapped.Status), static_cast<int>(ClaStatusOk));

    cla_u32 caseW = 0, caseH = 0, caseD = 0;
    ClaNominalSize(ClaLampArrayKindChassis, ClaSourceScopeWholeCase, &caseW, &caseH, &caseD);

    /* The fan column is taller than wide, so its height takes the larger
     * case dimension. */
    CLA_CHECK_EQ(mapped.Result.Attributes.BoundingBoxHeightInMicrometers,
                 std::max(caseW, caseH));
    CLA_CHECK_EQ(mapped.Result.Attributes.BoundingBoxDepthInMicrometers, caseD);
    CLA_CHECK_EQ(mapped.Result.Attributes.LampArrayKind,
                 static_cast<cla_u32>(ClaLampArrayKindChassis));
}

CLA_TEST(Mapping_WholeCaseEnvelopeIsLargerThanOneComponent)
{
    const Mapped component = Map(MakeFanColumn(), ClaLampArrayKindChassis,
                                 ClaSourceUnitsLogical);
    const Mapped wholeCase = Map(MakeFanColumn(), ClaLampArrayKindChassis,
                                 ClaSourceUnitsLogical, nullptr, ClaSourceScopeWholeCase);

    /* Pin the real-world sizes: a mid-tower is roughly 0.47m tall, a fan
     * 0.12m.  A regression to the fan envelope would fail here. */
    CLA_CHECK_EQ(component.Result.Attributes.BoundingBoxHeightInMicrometers, 120000u);
    CLA_CHECK_EQ(wholeCase.Result.Attributes.BoundingBoxHeightInMicrometers, 466000u);
    CLA_CHECK_EQ(wholeCase.Result.Attributes.BoundingBoxDepthInMicrometers, 453000u);
}

/* Scope sizes the envelope only; the layout itself must be the same shape and
 * in the same LampId order. */
CLA_TEST(Mapping_WholeCaseKeepsLayoutAndOrder)
{
    const Mapped component = Map(MakeFanColumn(), ClaLampArrayKindChassis,
                                 ClaSourceUnitsLogical);
    const Mapped wholeCase = Map(MakeFanColumn(), ClaLampArrayKindChassis,
                                 ClaSourceUnitsLogical, nullptr, ClaSourceScopeWholeCase);

    CLA_CHECK_EQ(component.Result.LampCount, wholeCase.Result.LampCount);
    for (cla_u32 i = 0; i < component.Result.LampCount; ++i)
    {
        CLA_CHECK_EQ(component.LampIdToLedId[i], wholeCase.LampIdToLedId[i]);
    }

    const auto &a = wholeCase.Result.Attributes;
    const double ratio = static_cast<double>(a.BoundingBoxHeightInMicrometers) /
                         static_cast<double>(a.BoundingBoxWidthInMicrometers);
    /* Source span is 1.0 wide by 3.5 tall. */
    CLA_CHECK(ratio > 3.49 && ratio < 3.51);
}

/* Millimetre positions are already physical; scope must not rescale them. */
CLA_TEST(Mapping_WholeCaseDoesNotRescaleMillimetres)
{
    const std::vector<ClaSourceLed> leds = {
        {1, 0.0, 0.0},
        {2, 100.0, 0.0},
    };
    const Mapped mapped = Map(leds, ClaLampArrayKindChassis, ClaSourceUnitsMillimeters,
                              nullptr, ClaSourceScopeWholeCase);
    CLA_CHECK_EQ(mapped.Result.Attributes.BoundingBoxWidthInMicrometers, 100000u);
}

/* Whole-case only means something for Chassis.  A keyboard is a keyboard. */
CLA_TEST(Mapping_ScopeOnlyAffectsChassis)
{
    for (cla_u32 kind : {ClaLampArrayKindKeyboard, ClaLampArrayKindMouse,
                         ClaLampArrayKindPeripheral, ClaLampArrayKindWearable,
                         ClaLampArrayKindUndefined})
    {
        cla_u32 w1 = 0, h1 = 0, d1 = 0, w2 = 0, h2 = 0, d2 = 0;
        ClaNominalSize(kind, ClaSourceScopeComponent, &w1, &h1, &d1);
        ClaNominalSize(kind, ClaSourceScopeWholeCase, &w2, &h2, &d2);
        CLA_CHECK_EQ(w1, w2);
        CLA_CHECK_EQ(h1, h2);
        CLA_CHECK_EQ(d1, d2);
    }
}

/* ClaNominalSizeForKind is the component envelope, and a value outside the
 * enum falls back to it rather than to anything larger. */
CLA_TEST(Mapping_NominalSizeDefaultsToComponent)
{
    cla_u32 w1 = 0, h1 = 0, d1 = 0, w2 = 0, h2 = 0, d2 = 0, w3 = 0, h3 = 0, d3 = 0;
    ClaNominalSizeForKind(ClaLampArrayKindChassis, &w1, &h1, &d1);
    ClaNominalSize(ClaLampArrayKindChassis, ClaSourceScopeComponent, &w2, &h2, &d2);
    ClaNominalSize(ClaLampArrayKindChassis, 0xFFFFFFFFu, &w3, &h3, &d3);
    CLA_CHECK_EQ(w1, w2);
    CLA_CHECK_EQ(h1, h2);
    CLA_CHECK_EQ(d1, d2);
    CLA_CHECK_EQ(w3, w2);
    CLA_CHECK_EQ(h3, h2);
    CLA_CHECK_EQ(d3, d2);
}

/* ---------------------------------------------------------------- */
/* Degenerate topologies                                             */
/* ---------------------------------------------------------------- */

/* A single-LED device (some DIY nodes) must map, not fail. */
CLA_TEST(Mapping_SingleLedProducesValidDevice)
{
    const std::vector<ClaSourceLed> leds = {{42, 12.0, 34.0}};
    const Mapped mapped = Map(leds, ClaLampArrayKindChassis);

    CLA_CHECK_EQ(static_cast<int>(mapped.Status), static_cast<int>(ClaStatusOk));
    CLA_CHECK_EQ(mapped.Result.Attributes.LampCount, 1u);
    CLA_CHECK(mapped.Result.Attributes.BoundingBoxWidthInMicrometers > 0u);
    CLA_CHECK(mapped.Result.Attributes.BoundingBoxHeightInMicrometers > 0u);
    CLA_CHECK_EQ(mapped.Lamps[0].PositionXInMicrometers, 0u);
    CLA_CHECK_EQ(mapped.LampIdToLedId[0], 42u);
}

/* An LED strip: every LED shares a Y, so the height span is zero. */
CLA_TEST(Mapping_CollinearLedsStillGetPositiveHeight)
{
    std::vector<ClaSourceLed> leds;
    for (int i = 0; i < 30; ++i)
    {
        leds.push_back({static_cast<cla_u32>(i), i * 10.0, 5.0});
    }
    const Mapped mapped = Map(leds, ClaLampArrayKindChassis);
    CLA_CHECK_EQ(static_cast<int>(mapped.Status), static_cast<int>(ClaStatusOk));
    CLA_CHECK(mapped.Result.Attributes.BoundingBoxHeightInMicrometers > 0u);
    for (const ClaLampInfo &lamp : mapped.Lamps)
    {
        CLA_CHECK_EQ(lamp.PositionYInMicrometers, 0u);
    }
}

/* Every LED reported at the same point -- seen when a device does not publish
 * real geometry. */
CLA_TEST(Mapping_CoincidentLedsProduceValidDevice)
{
    std::vector<ClaSourceLed> leds;
    for (int i = 0; i < 16; ++i)
    {
        leds.push_back({static_cast<cla_u32>(i), 0.0, 0.0});
    }
    const Mapped mapped = Map(leds, ClaLampArrayKindChassis);
    CLA_CHECK_EQ(static_cast<int>(mapped.Status), static_cast<int>(ClaStatusOk));
    CLA_CHECK(mapped.Result.Attributes.BoundingBoxWidthInMicrometers > 0u);
    CLA_CHECK(mapped.Result.Attributes.BoundingBoxHeightInMicrometers > 0u);
    CLA_CHECK_EQ(mapped.Result.Attributes.LampCount, 16u);
}

/* ---------------------------------------------------------------- */
/* Rejections and hostile input                                      */
/* ---------------------------------------------------------------- */

CLA_TEST(Mapping_RejectsZeroLedCount)
{
    ClaSourceDevice device{};
    device.LampArrayKind = ClaLampArrayKindKeyboard;
    device.LedCount = 0;
    ClaSourceLed led{};
    device.Leds = &led;

    ClaLampInfo lamp{};
    cla_u32 map = 0;
    ClaMappingResult result{};
    CLA_CHECK_EQ(static_cast<int>(ClaBuildLampTable(&device, nullptr, &lamp, &map, &result)),
                 static_cast<int>(ClaStatusInvalidArgument));
}

CLA_TEST(Mapping_RejectsLedCountAboveLimit)
{
    ClaSourceDevice device{};
    device.LampArrayKind = ClaLampArrayKindKeyboard;
    device.LedCount = CLA_MAX_LAMPS_PER_ARRAY + 1;
    ClaSourceLed led{};
    device.Leds = &led;

    ClaLampInfo lamp{};
    cla_u32 map = 0;
    ClaMappingResult result{};
    CLA_CHECK_EQ(static_cast<int>(ClaBuildLampTable(&device, nullptr, &lamp, &map, &result)),
                 static_cast<int>(ClaStatusInvalidArgument));
}

CLA_TEST(Mapping_RejectsNullArguments)
{
    ClaLampInfo lamp{};
    cla_u32 map = 0;
    ClaMappingResult result{};
    ClaSourceLed led{};
    ClaSourceDevice device{};
    device.LedCount = 1;
    device.Leds = &led;

    CLA_CHECK_EQ(static_cast<int>(ClaBuildLampTable(nullptr, nullptr, &lamp, &map, &result)),
                 static_cast<int>(ClaStatusInvalidArgument));
    CLA_CHECK_EQ(static_cast<int>(ClaBuildLampTable(&device, nullptr, nullptr, &map, &result)),
                 static_cast<int>(ClaStatusInvalidArgument));
    CLA_CHECK_EQ(static_cast<int>(ClaBuildLampTable(&device, nullptr, &lamp, nullptr, &result)),
                 static_cast<int>(ClaStatusInvalidArgument));
    CLA_CHECK_EQ(static_cast<int>(ClaBuildLampTable(&device, nullptr, &lamp, &map, nullptr)),
                 static_cast<int>(ClaStatusInvalidArgument));

    device.Leds = nullptr;
    CLA_CHECK_EQ(static_cast<int>(ClaBuildLampTable(&device, nullptr, &lamp, &map, &result)),
                 static_cast<int>(ClaStatusInvalidArgument));
}

/* A backend returning NaN or infinity must not propagate garbage into the
 * bounding box reported to Windows. */
CLA_TEST(Mapping_RejectsNonFiniteCoordinates)
{
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();

    for (double bad : {nan, inf, -inf})
    {
        const std::vector<ClaSourceLed> leds = {{1, 0.0, 0.0}, {2, bad, 5.0}};
        const Mapped mapped = Map(leds, ClaLampArrayKindKeyboard);
        CLA_CHECK_EQ(static_cast<int>(mapped.Status),
                     static_cast<int>(ClaStatusInvalidArgument));
    }
}

/* ---------------------------------------------------------------- */
/* Attributes and integration with IPC validation                    */
/* ---------------------------------------------------------------- */

CLA_TEST(Mapping_DeclaresFullEightBitColourOnEveryLamp)
{
    const Mapped mapped = Map(MakeKeyboardGrid(3, 5), ClaLampArrayKindKeyboard);
    for (const ClaLampInfo &lamp : mapped.Lamps)
    {
        /* Anything less would make some colours Windows can express illegal
         * under HUT 1.4 s26.11, voiding whole frames. */
        CLA_CHECK_EQ(lamp.RedLevelCount, 255);
        CLA_CHECK_EQ(lamp.GreenLevelCount, 255);
        CLA_CHECK_EQ(lamp.BlueLevelCount, 255);
        CLA_CHECK_EQ(lamp.IntensityLevelCount, 255);
        CLA_CHECK_EQ(lamp.IsProgrammable, 1);
    }
}

CLA_TEST(Mapping_KeyboardLampsAreControlPurpose)
{
    const Mapped mapped = Map(MakeKeyboardGrid(2, 4), ClaLampArrayKindKeyboard);
    for (const ClaLampInfo &lamp : mapped.Lamps)
    {
        CLA_CHECK_EQ(lamp.LampPurposes, static_cast<cla_u32>(CLA_LAMP_PURPOSE_CONTROL));
    }
    CLA_CHECK_EQ(ClaDefaultLampPurposeForKind(ClaLampArrayKindChassis),
                 static_cast<cla_u32>(CLA_LAMP_PURPOSE_ACCENT));
}

CLA_TEST(Mapping_AttributesCarryKindAndInterval)
{
    ClaMappingOptions options{};
    ClaMappingOptionsInit(&options);
    options.MinUpdateIntervalMicroseconds = 33333;

    const Mapped mapped = Map(MakeKeyboardGrid(2, 2), ClaLampArrayKindMouse,
                              ClaSourceUnitsMillimeters, &options);
    CLA_CHECK_EQ(mapped.Result.Attributes.LampArrayKind,
                 static_cast<cla_u32>(ClaLampArrayKindMouse));
    CLA_CHECK_EQ(mapped.Result.Attributes.MinUpdateIntervalInMicroseconds, 33333u);
}

/* The end-to-end contract: whatever the mapper produces must survive the
 * driver's own validation, or the service could build a device the driver
 * refuses. */
CLA_TEST(Mapping_OutputPassesDriverSideValidation)
{
    for (cla_u32 kind : {ClaLampArrayKindKeyboard, ClaLampArrayKindMouse,
                         ClaLampArrayKindChassis, ClaLampArrayKindWearable})
    {
        for (ClaSourceUnits units : {ClaSourceUnitsMillimeters, ClaSourceUnitsLogical})
        {
            for (cla_u32 scope : {static_cast<cla_u32>(ClaSourceScopeComponent),
                                  static_cast<cla_u32>(ClaSourceScopeWholeCase)})
            {
                const auto leds = MakeKeyboardGrid(5, 9);
                const Mapped mapped = Map(leds, kind, units, nullptr, scope);
                CLA_CHECK_EQ(static_cast<int>(mapped.Status), static_cast<int>(ClaStatusOk));

                /* Assemble the create request the service would send. */
                const cla_u32 lampCount = mapped.Result.LampCount;
                const cla_u32 size = ClaCreateRequestSizeForLampCount(lampCount);
                std::vector<cla_u8> buffer(size, 0);
                auto *request = reinterpret_cast<ClaLampArrayCreateRequest *>(buffer.data());
                request->Header.Version = CLA_IPC_PROTOCOL_VERSION;
                request->Header.Size = size;
                request->LampCount = lampCount;
                request->BoundingBoxWidthInMicrometers =
                    mapped.Result.Attributes.BoundingBoxWidthInMicrometers;
                request->BoundingBoxHeightInMicrometers =
                    mapped.Result.Attributes.BoundingBoxHeightInMicrometers;
                request->BoundingBoxDepthInMicrometers =
                    mapped.Result.Attributes.BoundingBoxDepthInMicrometers;
                request->LampArrayKind = mapped.Result.Attributes.LampArrayKind;
                request->MinUpdateIntervalInMicroseconds =
                    mapped.Result.Attributes.MinUpdateIntervalInMicroseconds;
                request->DeviceName[0] = 0;
                request->SerialNumber[0] = 0;
                for (cla_u32 i = 0; i < lampCount; ++i)
                {
                    request->Lamps[i] = mapped.Lamps[i];
                }

                CLA_CHECK_EQ(static_cast<int>(ClaValidateCreateRequest(buffer.data(), size)),
                             static_cast<int>(ClaStatusOk));
            }
        }
    }
}

/* A full-size device must map and validate: 512 LEDs is the iCUE SDK's
 * documented per-device maximum (CORSAIR_DEVICE_LEDCOUNT_MAX). */
CLA_TEST(Mapping_HandlesMaximumRealisticLedCount)
{
    std::vector<ClaSourceLed> leds;
    for (int i = 0; i < 512; ++i)
    {
        leds.push_back({static_cast<cla_u32>(i),
                        (i % 32) * 12.0,
                        (i / 32) * 12.0});
    }
    const Mapped mapped = Map(leds, ClaLampArrayKindKeyboard);
    CLA_CHECK_EQ(static_cast<int>(mapped.Status), static_cast<int>(ClaStatusOk));
    CLA_CHECK_EQ(mapped.Result.Attributes.LampCount, 512u);
}
