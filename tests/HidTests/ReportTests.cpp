/*
 * Copyright (c) Corsair Dynamic Lighting Bridge contributors.
 * SPDX-License-Identifier: MIT
 *
 * LampArray protocol state-machine tests.
 *
 * This exercises the exact code that runs in the kernel driver, against the
 * behaviours specified in HID Usage Tables v1.4 section 26.
 */

#include "ClaTest.h"

#include "cla/LampArrayDevice.h"
#include "cla/LampArrayProtocol.h"

#include <vector>
#include <array>

namespace {

/* A test device: 10 lamps in a row, full 8-bit RGBI, all programmable. */
struct TestDevice
{
    static constexpr cla_u16 kLampCount = 10;

    ClaLampArrayState State{};
    std::vector<ClaLampInfo> Lamps;
    std::vector<ClaLampColor> Colors;

    explicit TestDevice(cla_u16 lampCount = kLampCount)
        : Lamps(lampCount), Colors(lampCount)
    {
        for (cla_u16 i = 0; i < lampCount; ++i)
        {
            Lamps[i] = ClaLampInfo{};
            Lamps[i].PositionXInMicrometers = static_cast<cla_u32>(i) * 19000u;
            Lamps[i].PositionYInMicrometers = 5000u;
            Lamps[i].PositionZInMicrometers = 0u;
            Lamps[i].UpdateLatencyInMicroseconds = 4000u;
            Lamps[i].LampPurposes = CLA_LAMP_PURPOSE_CONTROL;
            /* Declaring the maximum level count on every channel means every
             * 0..255 value the host can send is in range, so no legitimate
             * host update is ever rejected.  See docs/lamp-array.md. */
            Lamps[i].RedLevelCount = 255;
            Lamps[i].GreenLevelCount = 255;
            Lamps[i].BlueLevelCount = 255;
            Lamps[i].IntensityLevelCount = 255;
            Lamps[i].IsProgrammable = 1;
            Lamps[i].InputBinding = 0;
        }

        ClaLampArrayAttributes attributes{};
        attributes.LampCount = lampCount;
        attributes.BoundingBoxWidthInMicrometers = 200000;
        attributes.BoundingBoxHeightInMicrometers = 10000;
        attributes.BoundingBoxDepthInMicrometers = 1000;
        attributes.LampArrayKind = ClaLampArrayKindKeyboard;
        attributes.MinUpdateIntervalInMicroseconds = 16667;

        const ClaStatus status =
            ClaLampArrayInit(&State, &attributes, Lamps.data(), Colors.data());
        CLA_CHECK_EQ(static_cast<int>(status), static_cast<int>(ClaStatusOk));
    }
};

cla_u16 ReadU16(const cla_u8 *p, size_t off)
{
    return static_cast<cla_u16>(p[off] | (p[off + 1] << 8));
}

cla_u32 ReadU32(const cla_u8 *p, size_t off)
{
    return static_cast<cla_u32>(p[off]) |
           (static_cast<cla_u32>(p[off + 1]) << 8) |
           (static_cast<cla_u32>(p[off + 2]) << 16) |
           (static_cast<cla_u32>(p[off + 3]) << 24);
}

void WriteU16(cla_u8 *p, size_t off, cla_u16 v)
{
    p[off] = static_cast<cla_u8>(v & 0xFF);
    p[off + 1] = static_cast<cla_u8>((v >> 8) & 0xFF);
}

/* Build a LampMultiUpdateReport buffer. */
std::array<cla_u8, CLA_BUFFER_SIZE_LAMP_MULTI_UPDATE> MakeMultiUpdate(
    const std::vector<std::pair<cla_u16, ClaLampColor>> &updates, cla_u8 flags)
{
    std::array<cla_u8, CLA_BUFFER_SIZE_LAMP_MULTI_UPDATE> buffer{};
    buffer[0] = CLA_REPORT_ID_LAMP_MULTI_UPDATE;
    buffer[1] = static_cast<cla_u8>(updates.size());
    buffer[2] = flags;
    for (size_t i = 0; i < updates.size(); ++i)
    {
        WriteU16(buffer.data(), 3 + i * 2, updates[i].first);
        const size_t c = 19 + i * 4;
        buffer[c + 0] = updates[i].second.Red;
        buffer[c + 1] = updates[i].second.Green;
        buffer[c + 2] = updates[i].second.Blue;
        buffer[c + 3] = updates[i].second.Intensity;
    }
    return buffer;
}

std::array<cla_u8, CLA_BUFFER_SIZE_LAMP_RANGE_UPDATE> MakeRangeUpdate(
    cla_u16 start, cla_u16 end, ClaLampColor color, cla_u8 flags)
{
    std::array<cla_u8, CLA_BUFFER_SIZE_LAMP_RANGE_UPDATE> buffer{};
    buffer[0] = CLA_REPORT_ID_LAMP_RANGE_UPDATE;
    buffer[1] = flags;
    WriteU16(buffer.data(), 2, start);
    WriteU16(buffer.data(), 4, end);
    buffer[6] = color.Red;
    buffer[7] = color.Green;
    buffer[8] = color.Blue;
    buffer[9] = color.Intensity;
    return buffer;
}

constexpr ClaLampColor kRed{255, 0, 0, 255};
constexpr ClaLampColor kGreen{0, 255, 0, 255};

} // namespace

/* ---------------------------------------------------------------- */
/* LampArrayAttributesReport (id 1)                                  */
/* ---------------------------------------------------------------- */

CLA_TEST(Attributes_RoundTripsDeviceAttributes)
{
    TestDevice device;
    cla_u8 buffer[CLA_BUFFER_SIZE_LAMP_ARRAY_ATTRIBUTES]{};
    cla_u32 written = 0;

    CLA_CHECK_EQ(static_cast<int>(ClaLampArrayGetFeature(
                     &device.State, CLA_REPORT_ID_LAMP_ARRAY_ATTRIBUTES,
                     buffer, sizeof(buffer), &written)),
                 static_cast<int>(ClaStatusOk));

    CLA_CHECK_EQ(written, static_cast<cla_u32>(CLA_BUFFER_SIZE_LAMP_ARRAY_ATTRIBUTES));
    CLA_CHECK_EQ(buffer[0], CLA_REPORT_ID_LAMP_ARRAY_ATTRIBUTES);
    CLA_CHECK_EQ(ReadU16(buffer, 1), TestDevice::kLampCount);
    CLA_CHECK_EQ(ReadU32(buffer, 3), 200000u);
    CLA_CHECK_EQ(ReadU32(buffer, 7), 10000u);
    CLA_CHECK_EQ(ReadU32(buffer, 11), 1000u);
    CLA_CHECK_EQ(ReadU32(buffer, 15), static_cast<cla_u32>(ClaLampArrayKindKeyboard));
    CLA_CHECK_EQ(ReadU32(buffer, 19), 16667u);
}

CLA_TEST(Attributes_RejectsShortBuffer)
{
    TestDevice device;
    cla_u8 buffer[CLA_BUFFER_SIZE_LAMP_ARRAY_ATTRIBUTES]{};
    cla_u32 written = 0;
    CLA_CHECK_EQ(static_cast<int>(ClaLampArrayGetFeature(
                     &device.State, CLA_REPORT_ID_LAMP_ARRAY_ATTRIBUTES,
                     buffer, sizeof(buffer) - 1, &written)),
                 static_cast<int>(ClaStatusBadBufferSize));
    CLA_CHECK_EQ(written, 0u);
    CLA_CHECK_EQ(device.State.Counters.RejectedBadBufferSize, 1u);
}

/* ---------------------------------------------------------------- */
/* Lamp attribute interrogation (ids 2 and 3), HUT 1.4 s26.8         */
/* ---------------------------------------------------------------- */

CLA_TEST(LampAttributes_ResponseCarriesLampData)
{
    TestDevice device;
    cla_u8 buffer[CLA_BUFFER_SIZE_LAMP_ATTRIBUTES_RESPONSE]{};
    cla_u32 written = 0;

    CLA_CHECK_EQ(static_cast<int>(ClaLampArrayGetFeature(
                     &device.State, CLA_REPORT_ID_LAMP_ATTRIBUTES_RESPONSE,
                     buffer, sizeof(buffer), &written)),
                 static_cast<int>(ClaStatusOk));

    CLA_CHECK_EQ(written, static_cast<cla_u32>(CLA_BUFFER_SIZE_LAMP_ATTRIBUTES_RESPONSE));
    CLA_CHECK_EQ(buffer[0], CLA_REPORT_ID_LAMP_ATTRIBUTES_RESPONSE);
    CLA_CHECK_EQ(ReadU16(buffer, 1), 0u);            /* first lamp */
    CLA_CHECK_EQ(ReadU32(buffer, 3), 0u);            /* PositionX  */
    CLA_CHECK_EQ(ReadU32(buffer, 7), 5000u);         /* PositionY  */
    CLA_CHECK_EQ(ReadU32(buffer, 11), 0u);           /* PositionZ  */
    CLA_CHECK_EQ(ReadU32(buffer, 15), 4000u);        /* UpdateLatency */
    CLA_CHECK_EQ(ReadU32(buffer, 19), static_cast<cla_u32>(CLA_LAMP_PURPOSE_CONTROL));
    CLA_CHECK_EQ(buffer[23], 255);                   /* RedLevelCount */
    CLA_CHECK_EQ(buffer[24], 255);
    CLA_CHECK_EQ(buffer[25], 255);
    CLA_CHECK_EQ(buffer[26], 255);                   /* IntensityLevelCount */
    CLA_CHECK_EQ(buffer[27], 1);                     /* IsProgrammable */
    CLA_CHECK_EQ(buffer[28], 0);                     /* InputBinding */
}

/* HUT 1.4 s26.8.2: consecutive responses auto-increment, then wrap to 0. */
CLA_TEST(LampAttributes_AutoIncrementsAndWraps)
{
    TestDevice device;
    cla_u8 buffer[CLA_BUFFER_SIZE_LAMP_ATTRIBUTES_RESPONSE]{};

    for (cla_u16 expected = 0; expected < TestDevice::kLampCount; ++expected)
    {
        CLA_CHECK_EQ(static_cast<int>(ClaLampArrayGetFeature(
                         &device.State, CLA_REPORT_ID_LAMP_ATTRIBUTES_RESPONSE,
                         buffer, sizeof(buffer), nullptr)),
                     static_cast<int>(ClaStatusOk));
        CLA_CHECK_EQ(ReadU16(buffer, 1), expected);
    }

    /* After LampCount-1 the cursor resets to 0. */
    CLA_CHECK_EQ(static_cast<int>(ClaLampArrayGetFeature(
                     &device.State, CLA_REPORT_ID_LAMP_ATTRIBUTES_RESPONSE,
                     buffer, sizeof(buffer), nullptr)),
                 static_cast<int>(ClaStatusOk));
    CLA_CHECK_EQ(ReadU16(buffer, 1), 0u);
}

CLA_TEST(LampAttributes_RequestSetsCursor)
{
    TestDevice device;
    cla_u8 request[CLA_BUFFER_SIZE_LAMP_ATTRIBUTES_REQUEST]{};
    request[0] = CLA_REPORT_ID_LAMP_ATTRIBUTES_REQUEST;
    WriteU16(request, 1, 4);

    CLA_CHECK_EQ(static_cast<int>(ClaLampArraySetFeature(
                     &device.State, CLA_REPORT_ID_LAMP_ATTRIBUTES_REQUEST,
                     request, sizeof(request), nullptr)),
                 static_cast<int>(ClaStatusOk));

    cla_u8 buffer[CLA_BUFFER_SIZE_LAMP_ATTRIBUTES_RESPONSE]{};
    ClaLampArrayGetFeature(&device.State, CLA_REPORT_ID_LAMP_ATTRIBUTES_RESPONSE,
                           buffer, sizeof(buffer), nullptr);
    CLA_CHECK_EQ(ReadU16(buffer, 1), 4u);
}

/* HUT 1.4 s26.8.1: "An invalid LampId must be treated by the device as
 * LampId == 0." */
CLA_TEST(LampAttributes_OutOfRangeRequestBecomesLampZero)
{
    TestDevice device;
    cla_u8 request[CLA_BUFFER_SIZE_LAMP_ATTRIBUTES_REQUEST]{};
    request[0] = CLA_REPORT_ID_LAMP_ATTRIBUTES_REQUEST;
    WriteU16(request, 1, 9999);

    CLA_CHECK_EQ(static_cast<int>(ClaLampArraySetFeature(
                     &device.State, CLA_REPORT_ID_LAMP_ATTRIBUTES_REQUEST,
                     request, sizeof(request), nullptr)),
                 static_cast<int>(ClaStatusOk));

    cla_u8 buffer[CLA_BUFFER_SIZE_LAMP_ATTRIBUTES_RESPONSE]{};
    ClaLampArrayGetFeature(&device.State, CLA_REPORT_ID_LAMP_ATTRIBUTES_RESPONSE,
                           buffer, sizeof(buffer), nullptr);
    CLA_CHECK_EQ(ReadU16(buffer, 1), 0u);
}

/* ---------------------------------------------------------------- */
/* LampMultiUpdateReport (id 4), HUT 1.4 s26.11.1                    */
/* ---------------------------------------------------------------- */

CLA_TEST(MultiUpdate_AppliesColors)
{
    TestDevice device;
    const auto buffer = MakeMultiUpdate({{0, kRed}, {3, kGreen}}, CLA_LAMP_UPDATE_COMPLETE);

    ClaUpdateOutcome outcome{};
    CLA_CHECK_EQ(static_cast<int>(ClaLampArraySetFeature(
                     &device.State, CLA_REPORT_ID_LAMP_MULTI_UPDATE,
                     buffer.data(), static_cast<cla_u32>(buffer.size()), &outcome)),
                 static_cast<int>(ClaStatusOk));

    CLA_CHECK_EQ(outcome.FrameComplete, 1);
    CLA_CHECK_EQ(outcome.LampsChanged, 2u);
    CLA_CHECK_EQ(device.Colors[0].Red, 255);
    CLA_CHECK_EQ(device.Colors[0].Green, 0);
    CLA_CHECK_EQ(device.Colors[3].Green, 255);
    CLA_CHECK_EQ(device.Colors[1].Red, 0);   /* untouched */
}

/* Batching: only the report carrying LampUpdateComplete ends the frame. */
CLA_TEST(MultiUpdate_FrameCompleteOnlyOnFlaggedReport)
{
    TestDevice device;

    const auto first = MakeMultiUpdate({{0, kRed}}, 0);
    ClaUpdateOutcome outcome{};
    ClaLampArraySetFeature(&device.State, CLA_REPORT_ID_LAMP_MULTI_UPDATE,
                           first.data(), static_cast<cla_u32>(first.size()), &outcome);
    CLA_CHECK_EQ(outcome.FrameComplete, 0);
    CLA_CHECK_EQ(device.State.Counters.FramesCompleted, 0u);

    const auto second = MakeMultiUpdate({{1, kGreen}}, CLA_LAMP_UPDATE_COMPLETE);
    ClaLampArraySetFeature(&device.State, CLA_REPORT_ID_LAMP_MULTI_UPDATE,
                           second.data(), static_cast<cla_u32>(second.size()), &outcome);
    CLA_CHECK_EQ(outcome.FrameComplete, 1);
    CLA_CHECK_EQ(device.State.Counters.FramesCompleted, 1u);

    /* Both updates are present in the completed frame. */
    CLA_CHECK_EQ(device.Colors[0].Red, 255);
    CLA_CHECK_EQ(device.Colors[1].Green, 255);
}

/* HUT 1.4 s26.11.1: "Any unused slot must be ignored by the device." */
CLA_TEST(MultiUpdate_IgnoresSlotsBeyondLampCount)
{
    TestDevice device;
    auto buffer = MakeMultiUpdate({{0, kRed}}, CLA_LAMP_UPDATE_COMPLETE);
    /* Populate slot 1 with a colour but leave LampCount at 1. */
    WriteU16(buffer.data(), 3 + 2, 5);
    buffer[19 + 4 + 0] = 111;
    buffer[19 + 4 + 1] = 222;

    ClaLampArraySetFeature(&device.State, CLA_REPORT_ID_LAMP_MULTI_UPDATE,
                           buffer.data(), static_cast<cla_u32>(buffer.size()), nullptr);

    CLA_CHECK_EQ(device.Colors[0].Red, 255);
    CLA_CHECK_EQ(device.Colors[5].Red, 0);      /* slot 1 ignored */
    CLA_CHECK_EQ(device.Colors[5].Green, 0);
}

/* HUT 1.4 s26.11.1 error: "Any LampId >= Device LampCount" -> ignore the
 * ENTIRE report, including the slots that were valid. */
CLA_TEST(MultiUpdate_OutOfRangeLampIdVoidsWholeReport)
{
    TestDevice device;
    const auto buffer = MakeMultiUpdate(
        {{0, kRed}, {TestDevice::kLampCount, kGreen}}, CLA_LAMP_UPDATE_COMPLETE);

    CLA_CHECK_EQ(static_cast<int>(ClaLampArraySetFeature(
                     &device.State, CLA_REPORT_ID_LAMP_MULTI_UPDATE,
                     buffer.data(), static_cast<cla_u32>(buffer.size()), nullptr)),
                 static_cast<int>(ClaStatusInvalidReport));

    /* Lamp 0 must NOT have been written. */
    CLA_CHECK_EQ(device.Colors[0].Red, 0);
    CLA_CHECK_EQ(device.State.Counters.RejectedLampIdOutOfRange, 1u);
}

/* HUT 1.4 s26.11.1 error: "Identical LampId in multiple slots". */
CLA_TEST(MultiUpdate_DuplicateLampIdVoidsWholeReport)
{
    TestDevice device;
    const auto buffer = MakeMultiUpdate({{2, kRed}, {2, kGreen}}, CLA_LAMP_UPDATE_COMPLETE);

    CLA_CHECK_EQ(static_cast<int>(ClaLampArraySetFeature(
                     &device.State, CLA_REPORT_ID_LAMP_MULTI_UPDATE,
                     buffer.data(), static_cast<cla_u32>(buffer.size()), nullptr)),
                 static_cast<int>(ClaStatusInvalidReport));
    CLA_CHECK_EQ(device.Colors[2].Red, 0);
    CLA_CHECK_EQ(device.State.Counters.RejectedDuplicateLampId, 1u);
}

/* The documented escape hatch: with strict checking off, duplicates apply
 * last-writer-wins instead of voiding the frame. */
CLA_TEST(MultiUpdate_DuplicateAllowedWhenStrictCheckDisabled)
{
    TestDevice device;
    device.State.StrictDuplicateLampIdCheck = 0;
    const auto buffer = MakeMultiUpdate({{2, kRed}, {2, kGreen}}, CLA_LAMP_UPDATE_COMPLETE);

    CLA_CHECK_EQ(static_cast<int>(ClaLampArraySetFeature(
                     &device.State, CLA_REPORT_ID_LAMP_MULTI_UPDATE,
                     buffer.data(), static_cast<cla_u32>(buffer.size()), nullptr)),
                 static_cast<int>(ClaStatusOk));
    CLA_CHECK_EQ(device.Colors[2].Green, 255);
}

CLA_TEST(MultiUpdate_SlotCountAboveEightIsRejected)
{
    TestDevice device;
    auto buffer = MakeMultiUpdate({{0, kRed}}, CLA_LAMP_UPDATE_COMPLETE);
    buffer[1] = 9;   /* more slots than the descriptor declares */

    CLA_CHECK_EQ(static_cast<int>(ClaLampArraySetFeature(
                     &device.State, CLA_REPORT_ID_LAMP_MULTI_UPDATE,
                     buffer.data(), static_cast<cla_u32>(buffer.size()), nullptr)),
                 static_cast<int>(ClaStatusInvalidReport));
    CLA_CHECK_EQ(device.Colors[0].Red, 0);
}

/* HUT 1.4 s26.11.1 error: "*Channel > *LevelCount described by Lamps'
 * attributes". A lamp with RedLevelCount 0 cannot take any red at all. */
CLA_TEST(MultiUpdate_ChannelAboveLevelCountVoidsWholeReport)
{
    TestDevice device;
    device.Lamps[1].RedLevelCount = 0;

    const auto buffer = MakeMultiUpdate({{1, kRed}}, CLA_LAMP_UPDATE_COMPLETE);
    CLA_CHECK_EQ(static_cast<int>(ClaLampArraySetFeature(
                     &device.State, CLA_REPORT_ID_LAMP_MULTI_UPDATE,
                     buffer.data(), static_cast<cla_u32>(buffer.size()), nullptr)),
                 static_cast<int>(ClaStatusInvalidReport));
    CLA_CHECK_EQ(device.State.Counters.RejectedChannelOverLevelCount, 1u);
}

/* HUT 1.4 s26.11.1: "For FixedColor Lamps, only the Intensity channel is
 * examined by the device (i.e. Red/Green/Blue channels are always ignored)." */
CLA_TEST(MultiUpdate_FixedColorLampTakesIntensityOnly)
{
    TestDevice device;
    device.Lamps[0].IsProgrammable = 0;

    const ClaLampColor color{200, 100, 50, 128};
    const auto buffer = MakeMultiUpdate({{0, color}}, CLA_LAMP_UPDATE_COMPLETE);
    CLA_CHECK_EQ(static_cast<int>(ClaLampArraySetFeature(
                     &device.State, CLA_REPORT_ID_LAMP_MULTI_UPDATE,
                     buffer.data(), static_cast<cla_u32>(buffer.size()), nullptr)),
                 static_cast<int>(ClaStatusOk));

    CLA_CHECK_EQ(device.Colors[0].Red, 0);          /* RGB ignored */
    CLA_CHECK_EQ(device.Colors[0].Green, 0);
    CLA_CHECK_EQ(device.Colors[0].Blue, 0);
    CLA_CHECK_EQ(device.Colors[0].Intensity, 128);  /* intensity applied */
}

CLA_TEST(MultiUpdate_ZeroSlotsIsValidNoOp)
{
    TestDevice device;
    const auto buffer = MakeMultiUpdate({}, CLA_LAMP_UPDATE_COMPLETE);
    ClaUpdateOutcome outcome{};
    CLA_CHECK_EQ(static_cast<int>(ClaLampArraySetFeature(
                     &device.State, CLA_REPORT_ID_LAMP_MULTI_UPDATE,
                     buffer.data(), static_cast<cla_u32>(buffer.size()), &outcome)),
                 static_cast<int>(ClaStatusOk));
    CLA_CHECK_EQ(outcome.LampsChanged, 0u);
    CLA_CHECK_EQ(outcome.FrameComplete, 1);
}

/* ---------------------------------------------------------------- */
/* LampRangeUpdateReport (id 5), HUT 1.4 s26.11.2                    */
/* ---------------------------------------------------------------- */

CLA_TEST(RangeUpdate_AppliesToInclusiveRange)
{
    TestDevice device;
    const auto buffer = MakeRangeUpdate(2, 4, kRed, CLA_LAMP_UPDATE_COMPLETE);

    ClaUpdateOutcome outcome{};
    CLA_CHECK_EQ(static_cast<int>(ClaLampArraySetFeature(
                     &device.State, CLA_REPORT_ID_LAMP_RANGE_UPDATE,
                     buffer.data(), static_cast<cla_u32>(buffer.size()), &outcome)),
                 static_cast<int>(ClaStatusOk));

    CLA_CHECK_EQ(outcome.LampsChanged, 3u);   /* 2, 3 and 4 inclusive */
    CLA_CHECK_EQ(device.Colors[1].Red, 0);
    CLA_CHECK_EQ(device.Colors[2].Red, 255);
    CLA_CHECK_EQ(device.Colors[3].Red, 255);
    CLA_CHECK_EQ(device.Colors[4].Red, 255);
    CLA_CHECK_EQ(device.Colors[5].Red, 0);
}

/* The common "turn everything off" case from HUT 1.4 s26.11.2. */
CLA_TEST(RangeUpdate_AllLampsOff)
{
    TestDevice device;
    const auto on = MakeRangeUpdate(0, TestDevice::kLampCount - 1, kRed, CLA_LAMP_UPDATE_COMPLETE);
    ClaLampArraySetFeature(&device.State, CLA_REPORT_ID_LAMP_RANGE_UPDATE,
                           on.data(), static_cast<cla_u32>(on.size()), nullptr);
    CLA_CHECK_EQ(device.Colors[0].Red, 255);

    const ClaLampColor off{0, 0, 0, 0};
    const auto buffer = MakeRangeUpdate(0, TestDevice::kLampCount - 1, off, CLA_LAMP_UPDATE_COMPLETE);
    ClaLampArraySetFeature(&device.State, CLA_REPORT_ID_LAMP_RANGE_UPDATE,
                           buffer.data(), static_cast<cla_u32>(buffer.size()), nullptr);

    for (cla_u16 i = 0; i < TestDevice::kLampCount; ++i)
    {
        CLA_CHECK_EQ(device.Colors[i].Red, 0);
        CLA_CHECK_EQ(device.Colors[i].Intensity, 0);
    }
}

CLA_TEST(RangeUpdate_StartAfterEndIsRejected)
{
    TestDevice device;
    const auto buffer = MakeRangeUpdate(5, 2, kRed, CLA_LAMP_UPDATE_COMPLETE);
    CLA_CHECK_EQ(static_cast<int>(ClaLampArraySetFeature(
                     &device.State, CLA_REPORT_ID_LAMP_RANGE_UPDATE,
                     buffer.data(), static_cast<cla_u32>(buffer.size()), nullptr)),
                 static_cast<int>(ClaStatusInvalidReport));
    CLA_CHECK_EQ(device.Colors[2].Red, 0);
}

CLA_TEST(RangeUpdate_EndBeyondLampCountIsRejected)
{
    TestDevice device;
    const auto buffer = MakeRangeUpdate(0, TestDevice::kLampCount, kRed, CLA_LAMP_UPDATE_COMPLETE);
    CLA_CHECK_EQ(static_cast<int>(ClaLampArraySetFeature(
                     &device.State, CLA_REPORT_ID_LAMP_RANGE_UPDATE,
                     buffer.data(), static_cast<cla_u32>(buffer.size()), nullptr)),
                 static_cast<int>(ClaStatusInvalidReport));
    CLA_CHECK_EQ(device.Colors[0].Red, 0);
}

CLA_TEST(RangeUpdate_ChannelOverLevelCountAnywhereInRangeVoidsReport)
{
    TestDevice device;
    device.Lamps[3].RedLevelCount = 10;   /* mid-range lamp cannot take 255 */

    const auto buffer = MakeRangeUpdate(0, 5, kRed, CLA_LAMP_UPDATE_COMPLETE);
    CLA_CHECK_EQ(static_cast<int>(ClaLampArraySetFeature(
                     &device.State, CLA_REPORT_ID_LAMP_RANGE_UPDATE,
                     buffer.data(), static_cast<cla_u32>(buffer.size()), nullptr)),
                 static_cast<int>(ClaStatusInvalidReport));
    /* Nothing applied, not even the lamps before the offending one. */
    CLA_CHECK_EQ(device.Colors[0].Red, 0);
    CLA_CHECK_EQ(device.Colors[1].Red, 0);
}

CLA_TEST(RangeUpdate_SingleLampRange)
{
    TestDevice device;
    const auto buffer = MakeRangeUpdate(7, 7, kGreen, CLA_LAMP_UPDATE_COMPLETE);
    ClaUpdateOutcome outcome{};
    CLA_CHECK_EQ(static_cast<int>(ClaLampArraySetFeature(
                     &device.State, CLA_REPORT_ID_LAMP_RANGE_UPDATE,
                     buffer.data(), static_cast<cla_u32>(buffer.size()), &outcome)),
                 static_cast<int>(ClaStatusOk));
    CLA_CHECK_EQ(outcome.LampsChanged, 1u);
    CLA_CHECK_EQ(device.Colors[7].Green, 255);
}

/* ---------------------------------------------------------------- */
/* LampArrayControlReport (id 6), HUT 1.4 s26.10.1                   */
/* ---------------------------------------------------------------- */

CLA_TEST(Control_AutonomousModeDefaultsToEnabled)
{
    TestDevice device;
    CLA_CHECK_EQ(device.State.AutonomousMode, 1);
}

CLA_TEST(Control_DisablingAndReenablingAutonomousMode)
{
    TestDevice device;
    cla_u8 buffer[CLA_BUFFER_SIZE_LAMP_ARRAY_CONTROL]{};
    buffer[0] = CLA_REPORT_ID_LAMP_ARRAY_CONTROL;

    buffer[1] = 0;
    ClaUpdateOutcome outcome{};
    CLA_CHECK_EQ(static_cast<int>(ClaLampArraySetFeature(
                     &device.State, CLA_REPORT_ID_LAMP_ARRAY_CONTROL,
                     buffer, sizeof(buffer), &outcome)),
                 static_cast<int>(ClaStatusOk));
    CLA_CHECK_EQ(device.State.AutonomousMode, 0);
    CLA_CHECK_EQ(outcome.AutonomousModeChanged, 1);
    CLA_CHECK_EQ(outcome.AutonomousMode, 0);

    /* "disabling when already disabled is a no-op" */
    ClaLampArraySetFeature(&device.State, CLA_REPORT_ID_LAMP_ARRAY_CONTROL,
                           buffer, sizeof(buffer), &outcome);
    CLA_CHECK_EQ(outcome.AutonomousModeChanged, 0);

    buffer[1] = 1;
    ClaLampArraySetFeature(&device.State, CLA_REPORT_ID_LAMP_ARRAY_CONTROL,
                           buffer, sizeof(buffer), &outcome);
    CLA_CHECK_EQ(device.State.AutonomousMode, 1);
    CLA_CHECK_EQ(outcome.AutonomousModeChanged, 1);
}

/* The descriptor declares LogicalMaximum(1), so 2 is out of range. */
CLA_TEST(Control_RejectsOutOfRangeAutonomousModeValue)
{
    TestDevice device;
    cla_u8 buffer[CLA_BUFFER_SIZE_LAMP_ARRAY_CONTROL]{};
    buffer[0] = CLA_REPORT_ID_LAMP_ARRAY_CONTROL;
    buffer[1] = 2;
    CLA_CHECK_EQ(static_cast<int>(ClaLampArraySetFeature(
                     &device.State, CLA_REPORT_ID_LAMP_ARRAY_CONTROL,
                     buffer, sizeof(buffer), nullptr)),
                 static_cast<int>(ClaStatusInvalidReport));
    CLA_CHECK_EQ(device.State.AutonomousMode, 1);   /* unchanged */
}

/* HUT 1.4 s26.10.1: attribute reports must keep working regardless of
 * autonomous-mode state. */
CLA_TEST(Control_AttributeReportsWorkInAutonomousMode)
{
    TestDevice device;
    CLA_CHECK_EQ(device.State.AutonomousMode, 1);

    cla_u8 buffer[CLA_BUFFER_SIZE_LAMP_ARRAY_ATTRIBUTES]{};
    CLA_CHECK_EQ(static_cast<int>(ClaLampArrayGetFeature(
                     &device.State, CLA_REPORT_ID_LAMP_ARRAY_ATTRIBUTES,
                     buffer, sizeof(buffer), nullptr)),
                 static_cast<int>(ClaStatusOk));
}

/* ---------------------------------------------------------------- */
/* Direction and buffer-size enforcement                             */
/* ---------------------------------------------------------------- */

CLA_TEST(Direction_GetOnUpdateReportIsRejected)
{
    TestDevice device;
    cla_u8 buffer[CLA_MAX_REPORT_BUFFER_SIZE]{};
    for (cla_u8 id : {cla_u8{CLA_REPORT_ID_LAMP_ATTRIBUTES_REQUEST},
                      cla_u8{CLA_REPORT_ID_LAMP_MULTI_UPDATE},
                      cla_u8{CLA_REPORT_ID_LAMP_RANGE_UPDATE},
                      cla_u8{CLA_REPORT_ID_LAMP_ARRAY_CONTROL}})
    {
        CLA_CHECK_EQ(static_cast<int>(ClaLampArrayGetFeature(
                         &device.State, id, buffer, sizeof(buffer), nullptr)),
                     static_cast<int>(ClaStatusInvalidOperation));
    }
}

CLA_TEST(Direction_SetOnAttributeReportIsRejected)
{
    TestDevice device;
    cla_u8 buffer[CLA_MAX_REPORT_BUFFER_SIZE]{};
    for (cla_u8 id : {cla_u8{CLA_REPORT_ID_LAMP_ARRAY_ATTRIBUTES},
                      cla_u8{CLA_REPORT_ID_LAMP_ATTRIBUTES_RESPONSE}})
    {
        CLA_CHECK_EQ(static_cast<int>(ClaLampArraySetFeature(
                         &device.State, id, buffer, sizeof(buffer), nullptr)),
                     static_cast<int>(ClaStatusInvalidOperation));
    }
}

CLA_TEST(ReportId_UnknownIdsAreRejectedBothWays)
{
    TestDevice device;
    cla_u8 buffer[CLA_MAX_REPORT_BUFFER_SIZE]{};
    for (cla_u8 id : {static_cast<cla_u8>(0), static_cast<cla_u8>(7),
                      static_cast<cla_u8>(200), static_cast<cla_u8>(255)})
    {
        CLA_CHECK_EQ(static_cast<int>(ClaLampArrayGetFeature(
                         &device.State, id, buffer, sizeof(buffer), nullptr)),
                     static_cast<int>(ClaStatusUnknownReportId));
        CLA_CHECK_EQ(static_cast<int>(ClaLampArraySetFeature(
                         &device.State, id, buffer, sizeof(buffer), nullptr)),
                     static_cast<int>(ClaStatusUnknownReportId));
    }
}

/* A SET must be exactly the declared size -- too long is as wrong as too
 * short, because it means the host and device disagree about the descriptor. */
CLA_TEST(BufferSize_SetFeatureRequiresExactLength)
{
    TestDevice device;
    cla_u8 buffer[CLA_BUFFER_SIZE_LAMP_MULTI_UPDATE + 4]{};
    buffer[0] = CLA_REPORT_ID_LAMP_MULTI_UPDATE;

    for (cla_u32 length : {0u, 1u, 10u,
                           static_cast<cla_u32>(CLA_BUFFER_SIZE_LAMP_MULTI_UPDATE) - 1u,
                           static_cast<cla_u32>(CLA_BUFFER_SIZE_LAMP_MULTI_UPDATE) + 1u})
    {
        CLA_CHECK_EQ(static_cast<int>(ClaLampArraySetFeature(
                         &device.State, CLA_REPORT_ID_LAMP_MULTI_UPDATE,
                         buffer, length, nullptr)),
                     static_cast<int>(ClaStatusBadBufferSize));
    }
}

CLA_TEST(NullArguments_AreRejectedNotDereferenced)
{
    TestDevice device;
    cla_u8 buffer[CLA_MAX_REPORT_BUFFER_SIZE]{};

    CLA_CHECK_EQ(static_cast<int>(ClaLampArrayGetFeature(
                     nullptr, 1, buffer, sizeof(buffer), nullptr)),
                 static_cast<int>(ClaStatusInvalidArgument));
    CLA_CHECK_EQ(static_cast<int>(ClaLampArrayGetFeature(
                     &device.State, 1, nullptr, sizeof(buffer), nullptr)),
                 static_cast<int>(ClaStatusInvalidArgument));
    CLA_CHECK_EQ(static_cast<int>(ClaLampArraySetFeature(
                     nullptr, 4, buffer, sizeof(buffer), nullptr)),
                 static_cast<int>(ClaStatusInvalidArgument));
    CLA_CHECK_EQ(static_cast<int>(ClaLampArraySetFeature(
                     &device.State, 4, nullptr, sizeof(buffer), nullptr)),
                 static_cast<int>(ClaStatusInvalidArgument));
}

/* ---------------------------------------------------------------- */
/* Init and lifecycle                                                */
/* ---------------------------------------------------------------- */

CLA_TEST(Init_RejectsZeroLampCount)
{
    ClaLampArrayState state{};
    ClaLampArrayAttributes attributes{};
    attributes.LampCount = 0;
    ClaLampInfo lamp{};
    ClaLampColor color{};
    CLA_CHECK_EQ(static_cast<int>(ClaLampArrayInit(&state, &attributes, &lamp, &color)),
                 static_cast<int>(ClaStatusInvalidArgument));
}

CLA_TEST(Init_RejectsNullPointers)
{
    ClaLampArrayState state{};
    ClaLampArrayAttributes attributes{};
    attributes.LampCount = 1;
    ClaLampInfo lamp{};
    ClaLampColor color{};
    CLA_CHECK_EQ(static_cast<int>(ClaLampArrayInit(nullptr, &attributes, &lamp, &color)),
                 static_cast<int>(ClaStatusInvalidArgument));
    CLA_CHECK_EQ(static_cast<int>(ClaLampArrayInit(&state, nullptr, &lamp, &color)),
                 static_cast<int>(ClaStatusInvalidArgument));
    CLA_CHECK_EQ(static_cast<int>(ClaLampArrayInit(&state, &attributes, nullptr, &color)),
                 static_cast<int>(ClaStatusInvalidArgument));
    CLA_CHECK_EQ(static_cast<int>(ClaLampArrayInit(&state, &attributes, &lamp, nullptr)),
                 static_cast<int>(ClaStatusInvalidArgument));
}

/* HUT 1.4 s26.11: "Default state for all Lamps is off (RGBI=0,0,0,0)." */
CLA_TEST(Init_ClearsLampsToOff)
{
    TestDevice device;
    for (cla_u16 i = 0; i < TestDevice::kLampCount; ++i)
    {
        CLA_CHECK_EQ(device.Colors[i].Red, 0);
        CLA_CHECK_EQ(device.Colors[i].Green, 0);
        CLA_CHECK_EQ(device.Colors[i].Blue, 0);
        CLA_CHECK_EQ(device.Colors[i].Intensity, 0);
    }
}

CLA_TEST(Blank_TurnsEverythingOffWithoutDisturbingModeOrCursor)
{
    TestDevice device;
    const auto on = MakeRangeUpdate(0, TestDevice::kLampCount - 1, kRed, CLA_LAMP_UPDATE_COMPLETE);
    ClaLampArraySetFeature(&device.State, CLA_REPORT_ID_LAMP_RANGE_UPDATE,
                           on.data(), static_cast<cla_u32>(on.size()), nullptr);

    cla_u8 control[CLA_BUFFER_SIZE_LAMP_ARRAY_CONTROL]{};
    control[0] = CLA_REPORT_ID_LAMP_ARRAY_CONTROL;
    control[1] = 0;
    ClaLampArraySetFeature(&device.State, CLA_REPORT_ID_LAMP_ARRAY_CONTROL,
                           control, sizeof(control), nullptr);

    ClaLampArrayBlank(&device.State);

    for (cla_u16 i = 0; i < TestDevice::kLampCount; ++i)
    {
        CLA_CHECK_EQ(device.Colors[i].Red, 0);
    }
    CLA_CHECK_EQ(device.State.AutonomousMode, 0);   /* preserved */
}

/* A single-lamp device is a legal degenerate case: the attribute cursor must
 * stay pinned at 0 rather than running off the end of a 1-entry table. */
CLA_TEST(SingleLampDevice_AttributeCursorStaysInRange)
{
    TestDevice device(1);
    cla_u8 buffer[CLA_BUFFER_SIZE_LAMP_ATTRIBUTES_RESPONSE]{};
    for (int i = 0; i < 5; ++i)
    {
        CLA_CHECK_EQ(static_cast<int>(ClaLampArrayGetFeature(
                         &device.State, CLA_REPORT_ID_LAMP_ATTRIBUTES_RESPONSE,
                         buffer, sizeof(buffer), nullptr)),
                     static_cast<int>(ClaStatusOk));
        CLA_CHECK_EQ(ReadU16(buffer, 1), 0u);
    }
}
