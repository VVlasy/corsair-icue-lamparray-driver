/*
 * Copyright (c) Corsair Dynamic Lighting Bridge contributors.
 * SPDX-License-Identifier: MIT
 *
 * Robustness tests for the report parser.
 *
 * Requirement (project Phase 14): "A malformed user-mode request must not be
 * able to crash the kernel driver."  Everything exercised here is the exact
 * code that runs in the driver, so these runs are meaningful evidence for that
 * claim.  Build with -DCLA_SANITIZE=ON to run them under ASan/UBSan, which is
 * what CI does; a buffer overrun in the parser then fails the build rather
 * than bugchecking a machine later.
 *
 * The invariants asserted are:
 *   - every entry point returns, for every input, without reading or writing
 *     outside the caller's buffer;
 *   - a rejected report leaves lamp state completely untouched;
 *   - lamp state stays internally consistent no matter what arrives.
 */

#include "ClaTest.h"

#include "cla/LampArrayDevice.h"
#include "cla/LampArrayProtocol.h"
#include "cla/HidDescriptorParser.h"
#include "cla/LampArrayDescriptor.h"

#include <vector>
#include <random>
#include <cstring>

namespace {

constexpr cla_u16 kLampCount = 64;

struct FuzzDevice
{
    ClaLampArrayState State{};
    std::vector<ClaLampInfo> Lamps;
    std::vector<ClaLampColor> Colors;

    FuzzDevice() : Lamps(kLampCount), Colors(kLampCount)
    {
        for (auto &lamp : Lamps)
        {
            lamp = ClaLampInfo{};
            lamp.RedLevelCount = 255;
            lamp.GreenLevelCount = 255;
            lamp.BlueLevelCount = 255;
            lamp.IntensityLevelCount = 255;
            lamp.IsProgrammable = 1;
        }
        ClaLampArrayAttributes attributes{};
        attributes.LampCount = kLampCount;
        attributes.BoundingBoxWidthInMicrometers = 400000;
        attributes.BoundingBoxHeightInMicrometers = 150000;
        attributes.BoundingBoxDepthInMicrometers = 30000;
        attributes.LampArrayKind = ClaLampArrayKindKeyboard;
        attributes.MinUpdateIntervalInMicroseconds = 16667;
        ClaLampArrayInit(&State, &attributes, Lamps.data(), Colors.data());
    }
};

/* Guard pages either side of the report buffer, so any overrun by the parser
 * is caught here rather than showing up as kernel memory corruption. */
struct GuardedBuffer
{
    static constexpr size_t kGuard = 64;
    static constexpr cla_u8 kFill = 0xCD;

    std::vector<cla_u8> Storage;
    size_t Size;

    explicit GuardedBuffer(size_t size)
        : Storage(size + 2 * kGuard, kFill), Size(size) {}

    cla_u8 *Data() { return Storage.data() + kGuard; }

    bool GuardsIntact() const
    {
        for (size_t i = 0; i < kGuard; ++i)
        {
            if (Storage[i] != kFill) return false;
            if (Storage[Storage.size() - 1 - i] != kFill) return false;
        }
        return true;
    }
};

} // namespace

/* Random bytes into SET_FEATURE across every report id and every plausible
 * length.  Nothing may crash, and no guard byte may be touched. */
CLA_TEST(Fuzz_SetFeatureSurvivesRandomInput)
{
    std::mt19937 rng(0xC0FFEE);   /* fixed seed: failures are reproducible */
    std::uniform_int_distribution<int> byteDist(0, 255);

    for (int iteration = 0; iteration < 20000; ++iteration)
    {
        FuzzDevice device;
        const cla_u8 reportId = static_cast<cla_u8>(rng() % 8);
        const size_t length = rng() % (CLA_MAX_REPORT_BUFFER_SIZE + 8);

        GuardedBuffer buffer(length);
        for (size_t i = 0; i < length; ++i)
        {
            buffer.Data()[i] = static_cast<cla_u8>(byteDist(rng));
        }

        ClaUpdateOutcome outcome{};
        const ClaStatus status = ClaLampArraySetFeature(
            &device.State, reportId, buffer.Data(),
            static_cast<cla_u32>(length), &outcome);

        CLA_CHECK(buffer.GuardsIntact());

        /* A rejected report must leave lamp state exactly as it was. */
        if (status != ClaStatusOk)
        {
            for (cla_u16 i = 0; i < kLampCount; ++i)
            {
                CLA_CHECK_EQ(device.Colors[i].Red, 0);
                CLA_CHECK_EQ(device.Colors[i].Green, 0);
                CLA_CHECK_EQ(device.Colors[i].Blue, 0);
                CLA_CHECK_EQ(device.Colors[i].Intensity, 0);
            }
        }

        /* Autonomous mode may only ever hold 0 or 1. */
        CLA_CHECK(device.State.AutonomousMode <= 1);
        /* The attribute cursor may never leave the lamp table. */
        CLA_CHECK(device.State.NextLampId < kLampCount);
    }
}

CLA_TEST(Fuzz_GetFeatureSurvivesRandomBufferSizes)
{
    std::mt19937 rng(0x1234);
    FuzzDevice device;

    for (int iteration = 0; iteration < 20000; ++iteration)
    {
        const cla_u8 reportId = static_cast<cla_u8>(rng() % 8);
        const size_t length = rng() % (CLA_MAX_REPORT_BUFFER_SIZE + 8);

        GuardedBuffer buffer(length);
        cla_u32 written = 0;
        const ClaStatus status = ClaLampArrayGetFeature(
            &device.State, reportId, buffer.Data(),
            static_cast<cla_u32>(length), &written);

        CLA_CHECK(buffer.GuardsIntact());
        /* Never claim to have written more than the caller offered. */
        CLA_CHECK(written <= length);
        if (status != ClaStatusOk)
        {
            CLA_CHECK_EQ(written, 0u);
        }
        CLA_CHECK(device.State.NextLampId < kLampCount);
    }
}

/* Valid-shaped multi-updates with hostile lamp ids: the parser must reject or
 * apply, never index out of the lamp table. */
CLA_TEST(Fuzz_MultiUpdateWithHostileLampIds)
{
    std::mt19937 rng(0xABCD);

    for (int iteration = 0; iteration < 20000; ++iteration)
    {
        FuzzDevice device;
        GuardedBuffer buffer(CLA_BUFFER_SIZE_LAMP_MULTI_UPDATE);
        std::memset(buffer.Data(), 0, CLA_BUFFER_SIZE_LAMP_MULTI_UPDATE);

        buffer.Data()[0] = CLA_REPORT_ID_LAMP_MULTI_UPDATE;
        buffer.Data()[1] = static_cast<cla_u8>(rng() % 20);   /* incl. > 8 */
        buffer.Data()[2] = static_cast<cla_u8>(rng() % 4);

        for (int slot = 0; slot < CLA_LAMP_MULTI_UPDATE_SLOT_COUNT; ++slot)
        {
            /* Mix of in-range, boundary and wildly out-of-range ids. */
            cla_u16 lampId;
            switch (rng() % 4)
            {
            case 0: lampId = static_cast<cla_u16>(rng() % kLampCount); break;
            case 1: lampId = kLampCount; break;
            case 2: lampId = 0xFFFF; break;
            default: lampId = static_cast<cla_u16>(rng()); break;
            }
            buffer.Data()[3 + slot * 2] = static_cast<cla_u8>(lampId & 0xFF);
            buffer.Data()[4 + slot * 2] = static_cast<cla_u8>(lampId >> 8);
        }

        ClaLampArraySetFeature(&device.State, CLA_REPORT_ID_LAMP_MULTI_UPDATE,
                               buffer.Data(), CLA_BUFFER_SIZE_LAMP_MULTI_UPDATE,
                               nullptr);
        CLA_CHECK(buffer.GuardsIntact());
    }
}

CLA_TEST(Fuzz_RangeUpdateWithHostileBounds)
{
    std::mt19937 rng(0x5EED);

    for (int iteration = 0; iteration < 20000; ++iteration)
    {
        FuzzDevice device;
        GuardedBuffer buffer(CLA_BUFFER_SIZE_LAMP_RANGE_UPDATE);

        buffer.Data()[0] = CLA_REPORT_ID_LAMP_RANGE_UPDATE;
        buffer.Data()[1] = static_cast<cla_u8>(rng() % 4);
        const cla_u16 start = static_cast<cla_u16>(rng());
        const cla_u16 end = static_cast<cla_u16>(rng());
        buffer.Data()[2] = static_cast<cla_u8>(start & 0xFF);
        buffer.Data()[3] = static_cast<cla_u8>(start >> 8);
        buffer.Data()[4] = static_cast<cla_u8>(end & 0xFF);
        buffer.Data()[5] = static_cast<cla_u8>(end >> 8);
        for (int i = 6; i < CLA_BUFFER_SIZE_LAMP_RANGE_UPDATE; ++i)
        {
            buffer.Data()[i] = static_cast<cla_u8>(rng() & 0xFF);
        }

        const ClaStatus status = ClaLampArraySetFeature(
            &device.State, CLA_REPORT_ID_LAMP_RANGE_UPDATE, buffer.Data(),
            CLA_BUFFER_SIZE_LAMP_RANGE_UPDATE, nullptr);

        CLA_CHECK(buffer.GuardsIntact());

        /* Only a genuinely in-range request may be accepted. */
        if (status == ClaStatusOk)
        {
            CLA_CHECK(start <= end);
            CLA_CHECK(end < kLampCount);
        }
    }
}

/* The descriptor parser is fed attacker-shaped bytes too: it runs over data
 * that, in other deployments, could come from outside. */
CLA_TEST(Fuzz_DescriptorParserSurvivesRandomBytes)
{
    std::mt19937 rng(0xFACE);

    for (int iteration = 0; iteration < 20000; ++iteration)
    {
        const size_t length = rng() % 300;
        GuardedBuffer buffer(length);
        for (size_t i = 0; i < length; ++i)
        {
            buffer.Data()[i] = static_cast<cla_u8>(rng() & 0xFF);
        }

        ClaHidDescriptorInfo info{};
        ClaParseHidDescriptor(buffer.Data(), static_cast<cla_u32>(length),
                              &info, nullptr, nullptr);

        CLA_CHECK(buffer.GuardsIntact());
        CLA_CHECK(info.ReportCount <= CLA_HID_MAX_REPORT_IDS);
        CLA_CHECK(info.MaxCollectionDepth <= CLA_HID_MAX_COLLECTION_DEPTH);
    }
}

/* Bit-flip the real descriptor: a single corrupted byte must be rejected or
 * parsed safely, never crash. */
CLA_TEST(Fuzz_DescriptorParserSurvivesBitFlips)
{
    for (cla_u32 byteIndex = 0; byteIndex < CLA_LAMP_ARRAY_DESCRIPTOR_SIZE; ++byteIndex)
    {
        for (int bit = 0; bit < 8; ++bit)
        {
            GuardedBuffer buffer(CLA_LAMP_ARRAY_DESCRIPTOR_SIZE);
            std::memcpy(buffer.Data(), ClaLampArrayReportDescriptor,
                        CLA_LAMP_ARRAY_DESCRIPTOR_SIZE);
            buffer.Data()[byteIndex] ^= static_cast<cla_u8>(1u << bit);

            ClaHidDescriptorInfo info{};
            ClaParseHidDescriptor(buffer.Data(), CLA_LAMP_ARRAY_DESCRIPTOR_SIZE,
                                  &info, nullptr, nullptr);
            CLA_CHECK(buffer.GuardsIntact());
        }
    }
}

/* Truncating the descriptor at every possible length must never over-read. */
CLA_TEST(Fuzz_DescriptorParserSurvivesTruncation)
{
    for (cla_u32 length = 0; length <= CLA_LAMP_ARRAY_DESCRIPTOR_SIZE; ++length)
    {
        GuardedBuffer buffer(length);
        std::memcpy(buffer.Data(), ClaLampArrayReportDescriptor, length);

        ClaHidDescriptorInfo info{};
        ClaParseHidDescriptor(buffer.Data(), length, &info, nullptr, nullptr);
        CLA_CHECK(buffer.GuardsIntact());
    }
}

/* Long random sequences of valid-shaped reports: state must stay consistent
 * across an extended session, as it would across hours of Dynamic Lighting. */
CLA_TEST(Fuzz_LongRunningSessionKeepsStateConsistent)
{
    std::mt19937 rng(0xD00D);
    FuzzDevice device;

    for (int iteration = 0; iteration < 50000; ++iteration)
    {
        cla_u8 buffer[CLA_MAX_REPORT_BUFFER_SIZE]{};
        const cla_u8 reportId = static_cast<cla_u8>(1 + (rng() % 6));
        const cla_u32 size = ClaGetReportBufferSize(reportId);
        buffer[0] = reportId;
        for (cla_u32 i = 1; i < size; ++i)
        {
            buffer[i] = static_cast<cla_u8>(rng() & 0xFF);
        }

        if (reportId == CLA_REPORT_ID_LAMP_ARRAY_ATTRIBUTES ||
            reportId == CLA_REPORT_ID_LAMP_ATTRIBUTES_RESPONSE)
        {
            cla_u32 written = 0;
            ClaLampArrayGetFeature(&device.State, reportId, buffer, size, &written);
        }
        else
        {
            ClaLampArraySetFeature(&device.State, reportId, buffer, size, nullptr);
        }

        CLA_CHECK(device.State.NextLampId < kLampCount);
        CLA_CHECK(device.State.AutonomousMode <= 1);
    }
}
