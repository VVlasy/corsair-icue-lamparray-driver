/*
 * Copyright (c) Corsair Dynamic Lighting Bridge contributors.
 * SPDX-License-Identifier: MIT
 *
 * Driver <-> service message validation.
 *
 * These messages arrive at the kernel from a user-mode process, so this is the
 * driver's trust boundary.  Project Phase 14: "The driver must validate all
 * IOCTL buffers ... A malformed user-mode request must not be able to crash
 * the kernel driver."  Each test below is a way a caller could try to make the
 * driver read or allocate out of bounds.
 */

#include "ClaTest.h"

#include "cla/IpcContract.h"

#include <vector>
#include <cstring>
#include <random>
#include <cstddef>

namespace {

/* Build a well-formed create request with `lampCount` lamps. */
std::vector<cla_u8> MakeCreateRequest(cla_u32 lampCount)
{
    const cla_u32 size = ClaCreateRequestSizeForLampCount(lampCount);
    std::vector<cla_u8> buffer(size ? size : sizeof(ClaLampArrayCreateRequest), 0);

    auto *request = reinterpret_cast<ClaLampArrayCreateRequest *>(buffer.data());
    request->Header.Version = CLA_IPC_PROTOCOL_VERSION;
    request->Header.Size = size;
    request->LampCount = lampCount;
    request->BoundingBoxWidthInMicrometers = 450000;
    request->BoundingBoxHeightInMicrometers = 160000;
    request->BoundingBoxDepthInMicrometers = 40000;
    request->LampArrayKind = ClaLampArrayKindKeyboard;
    request->MinUpdateIntervalInMicroseconds = 16667;
    request->VendorId = 0x1209;
    request->ProductId = 0x4C41;
    request->VersionNumber = 0x0100;
    request->DeviceName[0] = u'K';
    request->DeviceName[1] = 0;
    request->SerialNumber[0] = u'1';
    request->SerialNumber[1] = 0;

    for (cla_u32 i = 0; i < lampCount; ++i)
    {
        ClaLampInfo &lamp = request->Lamps[i];
        lamp = ClaLampInfo{};
        lamp.PositionXInMicrometers = i * 100;
        lamp.PositionYInMicrometers = 1000;
        lamp.PositionZInMicrometers = 0;
        lamp.UpdateLatencyInMicroseconds = 4000;
        lamp.LampPurposes = CLA_LAMP_PURPOSE_CONTROL;
        lamp.RedLevelCount = 255;
        lamp.GreenLevelCount = 255;
        lamp.BlueLevelCount = 255;
        lamp.IntensityLevelCount = 255;
        lamp.IsProgrammable = 1;
        lamp.InputBinding = 0;
    }
    return buffer;
}

ClaStatus Validate(std::vector<cla_u8> &buffer)
{
    return ClaValidateCreateRequest(buffer.data(), static_cast<cla_u32>(buffer.size()));
}

} // namespace

/* ---------------------------------------------------------------- */
/* Layout: the driver and the service must agree byte for byte.      */
/* ---------------------------------------------------------------- */

CLA_TEST(Layout_StructuresHaveStableSizes)
{
    /* If any of these change, CLA_IPC_PROTOCOL_VERSION must change too. */
    CLA_CHECK_EQ(sizeof(ClaMessageHeader), 8u);
    CLA_CHECK_EQ(sizeof(ClaLampInfo), 28u);
    CLA_CHECK_EQ(sizeof(ClaLampColor), 4u);
    CLA_CHECK_EQ(sizeof(ClaLampArrayIdRequest), 16u);
    CLA_CHECK_EQ(sizeof(ClaLampCountResponse), 16u);
    CLA_CHECK_EQ(sizeof(ClaSetBrightnessRequest), 16u);
    CLA_CHECK_EQ(sizeof(ClaSetDeviceStateRequest), 16u);
}

/* UTF-16 string fields must be 2 bytes per character on every platform.
 * wchar_t would be 4 bytes on Linux and silently change the layout. */
CLA_TEST(Layout_StringFieldsAreUtf16NotWcharT)
{
    CLA_CHECK_EQ(sizeof(cla_u16), 2u);
    ClaLampArrayCreateRequest request{};
    CLA_CHECK_EQ(sizeof(request.DeviceName), CLA_MAX_NAME_CHARS * 2u);
    CLA_CHECK_EQ(sizeof(request.SerialNumber), CLA_MAX_SERIAL_CHARS * 2u);
}

CLA_TEST(Layout_SizeHelpersAgreeWithStructures)
{
    /* One lamp: fixed part plus one ClaLampInfo. */
    const cla_u32 one = ClaCreateRequestSizeForLampCount(1);
    CLA_CHECK_EQ(one, static_cast<cla_u32>(sizeof(ClaLampArrayCreateRequest)));

    const cla_u32 two = ClaCreateRequestSizeForLampCount(2);
    CLA_CHECK_EQ(two - one, static_cast<cla_u32>(sizeof(ClaLampInfo)));

    /* ClaUpdateMessage carries a cla_u64, which gives the struct 8-byte
     * alignment and therefore 4 bytes of TRAILING PADDING after the 4-byte
     * Colors[1].  The wire size is offsetof(Colors) + n*sizeof(ClaLampColor),
     * which for n == 1 is smaller than sizeof(ClaUpdateMessage).  The helper
     * computes the wire size; that is the number the driver validates
     * against. */
    const cla_u32 update1 = ClaUpdateMessageSizeForLampCount(1);
    CLA_CHECK_EQ(update1, static_cast<cla_u32>(offsetof(ClaUpdateMessage, Colors) +
                                               sizeof(ClaLampColor)));
    CLA_CHECK_EQ(ClaUpdateMessageSizeForLampCount(2) - update1,
                 static_cast<cla_u32>(sizeof(ClaLampColor)));

    /* Sizing a buffer with sizeof() must always be safe, i.e. never smaller
     * than the wire size, so a caller that does the obvious thing is fine. */
    CLA_CHECK(sizeof(ClaUpdateMessage) >= update1);
}

/* ---------------------------------------------------------------- */
/* The happy path                                                    */
/* ---------------------------------------------------------------- */

CLA_TEST(Create_AcceptsWellFormedRequest)
{
    for (cla_u32 lampCount : {1u, 2u, 104u, CLA_MAX_LAMPS_PER_ARRAY})
    {
        auto buffer = MakeCreateRequest(lampCount);
        CLA_CHECK_EQ(static_cast<int>(Validate(buffer)), static_cast<int>(ClaStatusOk));
    }
}

/* ---------------------------------------------------------------- */
/* Rejection cases                                                   */
/* ---------------------------------------------------------------- */

CLA_TEST(Create_RejectsNullBuffer)
{
    CLA_CHECK_EQ(static_cast<int>(ClaValidateCreateRequest(nullptr, 1024)),
                 static_cast<int>(ClaStatusInvalidArgument));
}

/* Truncation at every length below a valid message must be refused, and must
 * not read past the buffer -- run under ASan this also proves the latter. */
CLA_TEST(Create_RejectsEveryTruncationLength)
{
    const auto full = MakeCreateRequest(16);
    for (cla_u32 length = 0; length < full.size(); ++length)
    {
        std::vector<cla_u8> truncated(full.begin(), full.begin() + length);
        const ClaStatus status = ClaValidateCreateRequest(
            truncated.empty() ? nullptr : truncated.data(), length);
        CLA_CHECK(status != ClaStatusOk);
    }
}

CLA_TEST(Create_RejectsWrongProtocolVersion)
{
    auto buffer = MakeCreateRequest(4);
    auto *request = reinterpret_cast<ClaLampArrayCreateRequest *>(buffer.data());
    request->Header.Version = CLA_IPC_PROTOCOL_VERSION + 1;
    CLA_CHECK_EQ(static_cast<int>(Validate(buffer)), static_cast<int>(ClaStatusInvalidReport));

    request->Header.Version = 0;
    CLA_CHECK_EQ(static_cast<int>(Validate(buffer)), static_cast<int>(ClaStatusInvalidReport));
}

/* The classic confused-deputy attempt: claim a small size in the header while
 * sending a different amount of data. */
CLA_TEST(Create_RejectsHeaderSizeDisagreeingWithBufferLength)
{
    auto buffer = MakeCreateRequest(8);
    auto *request = reinterpret_cast<ClaLampArrayCreateRequest *>(buffer.data());

    request->Header.Size = static_cast<cla_u32>(buffer.size()) - 4;
    CLA_CHECK_EQ(static_cast<int>(Validate(buffer)), static_cast<int>(ClaStatusBadBufferSize));

    request->Header.Size = static_cast<cla_u32>(buffer.size()) + 4;
    CLA_CHECK_EQ(static_cast<int>(Validate(buffer)), static_cast<int>(ClaStatusBadBufferSize));

    request->Header.Size = 0xFFFFFFFFu;
    CLA_CHECK_EQ(static_cast<int>(Validate(buffer)), static_cast<int>(ClaStatusBadBufferSize));
}

/* The important one: a LampCount far larger than the data actually sent.  If
 * the driver trusted LampCount it would read (and copy) far past the buffer. */
CLA_TEST(Create_RejectsLampCountLargerThanBuffer)
{
    auto buffer = MakeCreateRequest(4);
    auto *request = reinterpret_cast<ClaLampArrayCreateRequest *>(buffer.data());

    for (cla_u32 lying : {5u, 100u, CLA_MAX_LAMPS_PER_ARRAY, 0xFFFFu})
    {
        request->LampCount = lying;
        CLA_CHECK(Validate(buffer) != ClaStatusOk);
    }
}

CLA_TEST(Create_RejectsZeroLampCount)
{
    auto buffer = MakeCreateRequest(4);
    auto *request = reinterpret_cast<ClaLampArrayCreateRequest *>(buffer.data());
    request->LampCount = 0;
    CLA_CHECK_EQ(static_cast<int>(Validate(buffer)), static_cast<int>(ClaStatusInvalidReport));
}

CLA_TEST(Create_RejectsLampCountAboveLimit)
{
    auto buffer = MakeCreateRequest(4);
    auto *request = reinterpret_cast<ClaLampArrayCreateRequest *>(buffer.data());
    request->LampCount = CLA_MAX_LAMPS_PER_ARRAY + 1;
    CLA_CHECK(Validate(buffer) != ClaStatusOk);
}

/* A LampCount chosen so that fixedSize + count*sizeof(ClaLampInfo) would wrap
 * a 32-bit computation.  Doing the arithmetic in 64-bit is what stops this
 * turning into a tiny "expected size" that a small buffer satisfies. */
CLA_TEST(Create_RejectsLampCountCausingIntegerOverflow)
{
    auto buffer = MakeCreateRequest(4);
    auto *request = reinterpret_cast<ClaLampArrayCreateRequest *>(buffer.data());

    for (cla_u32 lying : {0xFFFFFFFFu, 0x80000000u, 0x0F000000u,
                          (0xFFFFFFFFu / static_cast<cla_u32>(sizeof(ClaLampInfo))) + 1u})
    {
        request->LampCount = lying;
        CLA_CHECK(Validate(buffer) != ClaStatusOk);
        /* And the size helper must refuse to produce a wrapped size. */
        CLA_CHECK_EQ(ClaCreateRequestSizeForLampCount(lying), 0u);
    }
}

CLA_TEST(Create_RejectsUnterminatedStrings)
{
    auto buffer = MakeCreateRequest(4);
    auto *request = reinterpret_cast<ClaLampArrayCreateRequest *>(buffer.data());

    for (cla_u32 i = 0; i < CLA_MAX_NAME_CHARS; ++i) request->DeviceName[i] = u'A';
    CLA_CHECK_EQ(static_cast<int>(Validate(buffer)), static_cast<int>(ClaStatusInvalidReport));

    request->DeviceName[CLA_MAX_NAME_CHARS - 1] = 0;
    CLA_CHECK_EQ(static_cast<int>(Validate(buffer)), static_cast<int>(ClaStatusOk));

    for (cla_u32 i = 0; i < CLA_MAX_SERIAL_CHARS; ++i) request->SerialNumber[i] = u'B';
    CLA_CHECK_EQ(static_cast<int>(Validate(buffer)), static_cast<int>(ClaStatusInvalidReport));
}

CLA_TEST(Create_RejectsZeroBoundingBox)
{
    for (int axis = 0; axis < 3; ++axis)
    {
        auto buffer = MakeCreateRequest(4);
        auto *request = reinterpret_cast<ClaLampArrayCreateRequest *>(buffer.data());
        if (axis == 0) request->BoundingBoxWidthInMicrometers = 0;
        if (axis == 1) request->BoundingBoxHeightInMicrometers = 0;
        if (axis == 2) request->BoundingBoxDepthInMicrometers = 0;
        CLA_CHECK_EQ(static_cast<int>(Validate(buffer)), static_cast<int>(ClaStatusInvalidReport));
    }
}

CLA_TEST(Create_RejectsLampOutsideBoundingBox)
{
    auto buffer = MakeCreateRequest(4);
    auto *request = reinterpret_cast<ClaLampArrayCreateRequest *>(buffer.data());
    request->Lamps[2].PositionXInMicrometers = request->BoundingBoxWidthInMicrometers + 1;
    CLA_CHECK_EQ(static_cast<int>(Validate(buffer)), static_cast<int>(ClaStatusInvalidReport));
}

CLA_TEST(Create_RejectsNonBooleanIsProgrammable)
{
    auto buffer = MakeCreateRequest(4);
    auto *request = reinterpret_cast<ClaLampArrayCreateRequest *>(buffer.data());
    request->Lamps[1].IsProgrammable = 2;
    CLA_CHECK_EQ(static_cast<int>(Validate(buffer)), static_cast<int>(ClaStatusInvalidReport));
}

/* A programmable lamp with every level count zero could never accept any
 * colour, so every host update to it would be voided. */
CLA_TEST(Create_RejectsProgrammableLampWithNoLevels)
{
    auto buffer = MakeCreateRequest(4);
    auto *request = reinterpret_cast<ClaLampArrayCreateRequest *>(buffer.data());
    request->Lamps[0].RedLevelCount = 0;
    request->Lamps[0].GreenLevelCount = 0;
    request->Lamps[0].BlueLevelCount = 0;
    request->Lamps[0].IntensityLevelCount = 0;
    CLA_CHECK_EQ(static_cast<int>(Validate(buffer)), static_cast<int>(ClaStatusInvalidReport));
}

/* ---------------------------------------------------------------- */
/* Fixed-size messages                                               */
/* ---------------------------------------------------------------- */

CLA_TEST(FixedMessage_AcceptsExactSizeOnly)
{
    ClaLampArrayIdRequest request{};
    request.Header.Version = CLA_IPC_PROTOCOL_VERSION;
    request.Header.Size = sizeof(request);
    request.LampArrayId = 3;

    CLA_CHECK_EQ(static_cast<int>(ClaValidateFixedMessage(
                     &request, sizeof(request), sizeof(request))),
                 static_cast<int>(ClaStatusOk));

    /* Too short and too long are both refused. */
    CLA_CHECK_EQ(static_cast<int>(ClaValidateFixedMessage(
                     &request, sizeof(request) - 1, sizeof(request))),
                 static_cast<int>(ClaStatusBadBufferSize));
    CLA_CHECK_EQ(static_cast<int>(ClaValidateFixedMessage(
                     &request, sizeof(request) + 1, sizeof(request))),
                 static_cast<int>(ClaStatusBadBufferSize));
    CLA_CHECK_EQ(static_cast<int>(ClaValidateFixedMessage(&request, 0, sizeof(request))),
                 static_cast<int>(ClaStatusBadBufferSize));
}

CLA_TEST(FixedMessage_RejectsNullAndBadVersion)
{
    CLA_CHECK_EQ(static_cast<int>(ClaValidateFixedMessage(nullptr, 16, 16)),
                 static_cast<int>(ClaStatusInvalidArgument));

    ClaLampArrayIdRequest request{};
    request.Header.Version = 999;
    request.Header.Size = sizeof(request);
    CLA_CHECK_EQ(static_cast<int>(ClaValidateFixedMessage(
                     &request, sizeof(request), sizeof(request))),
                 static_cast<int>(ClaStatusInvalidReport));
}

CLA_TEST(FixedMessage_RejectsHeaderSizeMismatch)
{
    ClaLampArrayIdRequest request{};
    request.Header.Version = CLA_IPC_PROTOCOL_VERSION;
    request.Header.Size = sizeof(request) + 8;
    CLA_CHECK_EQ(static_cast<int>(ClaValidateFixedMessage(
                     &request, sizeof(request), sizeof(request))),
                 static_cast<int>(ClaStatusBadBufferSize));
}

/* ---------------------------------------------------------------- */
/* Inverted-call output buffer                                       */
/* ---------------------------------------------------------------- */

CLA_TEST(UpdateBuffer_RequiresRoomForEveryLamp)
{
    const cla_u32 lampCount = 104;
    const cla_u32 required = ClaUpdateMessageSizeForLampCount(lampCount);
    CLA_CHECK(required > 0u);

    CLA_CHECK_EQ(static_cast<int>(ClaValidateUpdateOutputBuffer(required, lampCount)),
                 static_cast<int>(ClaStatusOk));
    /* A larger buffer is fine -- the service may reuse one big buffer. */
    CLA_CHECK_EQ(static_cast<int>(ClaValidateUpdateOutputBuffer(required + 4096, lampCount)),
                 static_cast<int>(ClaStatusOk));
    CLA_CHECK_EQ(static_cast<int>(ClaValidateUpdateOutputBuffer(required - 1, lampCount)),
                 static_cast<int>(ClaStatusBadBufferSize));
    CLA_CHECK_EQ(static_cast<int>(ClaValidateUpdateOutputBuffer(0, lampCount)),
                 static_cast<int>(ClaStatusBadBufferSize));
}

CLA_TEST(UpdateBuffer_RejectsOutOfRangeLampCounts)
{
    CLA_CHECK_EQ(static_cast<int>(ClaValidateUpdateOutputBuffer(1u << 20, 0)),
                 static_cast<int>(ClaStatusInvalidArgument));
    CLA_CHECK_EQ(static_cast<int>(ClaValidateUpdateOutputBuffer(
                     1u << 20, CLA_MAX_LAMPS_PER_ARRAY + 1)),
                 static_cast<int>(ClaStatusInvalidArgument));
    CLA_CHECK_EQ(static_cast<int>(ClaValidateUpdateOutputBuffer(1u << 20, 0xFFFFFFFFu)),
                 static_cast<int>(ClaStatusInvalidArgument));
}

/* ---------------------------------------------------------------- */
/* Fuzz                                                              */
/* ---------------------------------------------------------------- */

/* Random bytes as a create request: validation must always terminate, never
 * read out of bounds, and never accept something whose declared lamp count
 * disagrees with the buffer it arrived in. */
CLA_TEST(Fuzz_CreateValidationSurvivesRandomBuffers)
{
    std::mt19937 rng(0xBEEF);

    for (int iteration = 0; iteration < 20000; ++iteration)
    {
        const size_t length = rng() % 4096;
        std::vector<cla_u8> buffer(length);
        for (auto &byte : buffer) byte = static_cast<cla_u8>(rng() & 0xFF);

        const ClaStatus status = ClaValidateCreateRequest(
            buffer.empty() ? nullptr : buffer.data(), static_cast<cla_u32>(length));

        if (status == ClaStatusOk)
        {
            /* If it was accepted, the invariants the driver relies on must
             * actually hold. */
            const auto *request =
                reinterpret_cast<const ClaLampArrayCreateRequest *>(buffer.data());
            CLA_CHECK_EQ(request->Header.Version, CLA_IPC_PROTOCOL_VERSION);
            CLA_CHECK_EQ(request->Header.Size, static_cast<cla_u32>(length));
            CLA_CHECK(request->LampCount >= 1u);
            CLA_CHECK(request->LampCount <= CLA_MAX_LAMPS_PER_ARRAY);
            CLA_CHECK_EQ(ClaCreateRequestSizeForLampCount(request->LampCount),
                         static_cast<cla_u32>(length));
        }
    }
}

/* Start from a valid request and corrupt one byte at a time. */
CLA_TEST(Fuzz_CreateValidationSurvivesSingleByteCorruption)
{
    std::mt19937 rng(0x1357);
    const auto pristine = MakeCreateRequest(8);

    for (int iteration = 0; iteration < 20000; ++iteration)
    {
        auto buffer = pristine;
        const size_t index = rng() % buffer.size();
        buffer[index] = static_cast<cla_u8>(rng() & 0xFF);

        const ClaStatus status = ClaValidateCreateRequest(
            buffer.data(), static_cast<cla_u32>(buffer.size()));
        if (status == ClaStatusOk)
        {
            const auto *request =
                reinterpret_cast<const ClaLampArrayCreateRequest *>(buffer.data());
            CLA_CHECK_EQ(ClaCreateRequestSizeForLampCount(request->LampCount),
                         static_cast<cla_u32>(buffer.size()));
        }
    }
}
