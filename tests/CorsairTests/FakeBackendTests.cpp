/*
 * Copyright (c) Corsair Dynamic Lighting Bridge contributors.
 * SPDX-License-Identifier: MIT
 *
 * Backend and frame-pipeline tests.
 *
 * These cover the behaviours the project calls out for the Corsair backend
 * (Phase 16): enumeration, LED enumeration, LED position retrieval, colour
 * writes, reconnect and iCUE restart -- plus the Phase 13 performance
 * behaviour (coalescing, dirty-lamp tracking, throttling) and the Phase 14
 * failure cases.
 *
 * They run against FakeBackend, so they exercise the service's own logic on
 * any machine.  The iCUE backend itself needs real iCUE; tools/CorsairProbe
 * is the manual counterpart for that.
 */

#include "ClaTest.h"

#include "Corsair/FakeBackend.h"
#include "Devices/FramePipeline.h"
#include "cla/CorsairMapping.h"

#include <vector>
#include <string>
#include <algorithm>
#include <memory>

using namespace cla;

namespace {

std::unique_ptr<FakeBackend> MakeBackendWithKeyboard(const std::string &id = "kb-1")
{
    auto backend = std::make_unique<FakeBackend>();
    backend->AddKeyboard(id, 6, 18);
    backend->Initialize();
    return backend;
}

/* Build the pipeline the service would build for a device. */
FramePipeline MakePipelineFor(FakeBackend &backend, const std::string &id,
                              cla_u64 minInterval = 0)
{
    std::vector<ClaSourceLed> leds;
    backend.GetLamps(id, leds);

    BackendDeviceInfo info{};
    backend.GetDeviceInfo(id, info);

    ClaSourceDevice device{};
    device.LampArrayKind = info.LampArrayKind;
    device.Units = info.PositionUnits;
    device.LedCount = static_cast<cla_u32>(leds.size());
    device.Leds = leds.data();

    std::vector<ClaLampInfo> lamps(leds.size());
    std::vector<cla_u32> map(leds.size());
    ClaMappingResult result{};
    ClaBuildLampTable(&device, nullptr, lamps.data(), map.data(), &result);

    return FramePipeline(map, minInterval);
}

std::vector<ClaLampColor> SolidFrame(size_t lampCount, cla_u8 r, cla_u8 g, cla_u8 b, cla_u8 i = 255)
{
    std::vector<ClaLampColor> frame(lampCount);
    for (auto &color : frame)
    {
        color = ClaLampColor{r, g, b, i};
    }
    return frame;
}

} // namespace

/* ---------------------------------------------------------------- */
/* Enumeration                                                       */
/* ---------------------------------------------------------------- */

CLA_TEST(Backend_EnumeratesConnectedDevices)
{
    auto backend = MakeBackendWithKeyboard();
    std::vector<BackendDeviceInfo> devices;
    CLA_CHECK(backend->EnumerateDevices(devices) == BackendStatus::Ok);
    CLA_CHECK_EQ(devices.size(), 1u);
    CLA_CHECK(devices[0].Id == "kb-1");
    CLA_CHECK_EQ(devices[0].LedCount, 6u * 18u);
    CLA_CHECK_EQ(devices[0].LampArrayKind, static_cast<cla_u32>(ClaLampArrayKindKeyboard));
}

CLA_TEST(Backend_LedPositionsAreRetrievable)
{
    auto backend = MakeBackendWithKeyboard();
    std::vector<ClaSourceLed> leds;
    CLA_CHECK(backend->GetLamps("kb-1", leds) == BackendStatus::Ok);
    CLA_CHECK_EQ(leds.size(), 6u * 18u);
    /* Positions must be distinct, or the mapping would collapse the layout. */
    CLA_CHECK(leds[0].X != leds[1].X || leds[0].Y != leds[1].Y);
}

CLA_TEST(Backend_UnknownDeviceIsReported)
{
    auto backend = MakeBackendWithKeyboard();
    BackendDeviceInfo info{};
    CLA_CHECK(backend->GetDeviceInfo("nope", info) == BackendStatus::DeviceNotFound);
    std::vector<ClaSourceLed> leds;
    CLA_CHECK(backend->GetLamps("nope", leds) == BackendStatus::DeviceNotFound);
}

/* ---------------------------------------------------------------- */
/* iCUE availability (project Phase 10)                              */
/* ---------------------------------------------------------------- */

/* iCUE not running: Initialize succeeds but the session never connects, and
 * every operation reports NotConnected rather than failing hard. */
CLA_TEST(Backend_IcueNotRunningIsNotAnError)
{
    FakeBackend backend;
    backend.SetConnectOnInitialize(false);
    backend.AddKeyboard("kb-1");

    CLA_CHECK(backend.Initialize() == BackendStatus::Ok);
    CLA_CHECK(!backend.IsConnected());

    std::vector<BackendDeviceInfo> devices;
    CLA_CHECK(backend.EnumerateDevices(devices) == BackendStatus::NotConnected);
    CLA_CHECK(backend.SetLampColors("kb-1", {}) == BackendStatus::NotConnected);
}

/* iCUE exits mid-session, then restarts. */
CLA_TEST(Backend_SessionLostAndRestored)
{
    auto backend = MakeBackendWithKeyboard();
    CLA_CHECK(backend->IsConnected());

    std::vector<BackendEvent> events;
    backend->SubscribeEvents([&](const BackendEvent &event) { events.push_back(event); });

    backend->SimulateSessionLost();
    CLA_CHECK(!backend->IsConnected());
    std::vector<BackendDeviceInfo> devices;
    CLA_CHECK(backend->EnumerateDevices(devices) == BackendStatus::NotConnected);

    backend->SimulateSessionRestored();
    CLA_CHECK(backend->IsConnected());
    CLA_CHECK(backend->EnumerateDevices(devices) == BackendStatus::Ok);
    CLA_CHECK_EQ(devices.size(), 1u);

    CLA_CHECK_EQ(events.size(), 2u);
    CLA_CHECK(events[0].Kind == BackendEventKind::SessionStateChanged);
    CLA_CHECK(!events[0].SessionConnected);
    CLA_CHECK(events[1].SessionConnected);
}

/* ---------------------------------------------------------------- */
/* Hotplug (project Phase 12)                                        */
/* ---------------------------------------------------------------- */

CLA_TEST(Backend_DeviceDisconnectAndReconnect)
{
    auto backend = MakeBackendWithKeyboard();
    std::vector<BackendEvent> events;
    backend->SubscribeEvents([&](const BackendEvent &event) { events.push_back(event); });

    backend->SimulateDeviceDisconnected("kb-1");

    std::vector<BackendDeviceInfo> devices;
    backend->EnumerateDevices(devices);
    CLA_CHECK_EQ(devices.size(), 0u);
    BackendDeviceInfo info{};
    CLA_CHECK(backend->GetDeviceInfo("kb-1", info) == BackendStatus::DeviceNotFound);

    backend->SimulateDeviceConnected("kb-1");
    backend->EnumerateDevices(devices);
    CLA_CHECK_EQ(devices.size(), 1u);

    CLA_CHECK_EQ(events.size(), 2u);
    CLA_CHECK(events[0].Kind == BackendEventKind::DeviceDisconnected);
    CLA_CHECK(events[1].Kind == BackendEventKind::DeviceConnected);
}

/* A device with no LEDs must not crash anything (project Phase 14). */
CLA_TEST(Backend_ZeroLedDeviceIsHandled)
{
    FakeBackend backend;
    backend.AddDevice("empty", "Nothing", ClaLampArrayKindChassis, {});
    backend.Initialize();

    std::vector<ClaSourceLed> leds;
    CLA_CHECK(backend.GetLamps("empty", leds) == BackendStatus::Ok);
    CLA_CHECK_EQ(leds.size(), 0u);

    /* And the mapper refuses it rather than producing a zero-lamp device,
     * which the driver would reject anyway. */
    ClaSourceDevice device{};
    device.LampArrayKind = ClaLampArrayKindChassis;
    device.LedCount = 0;
    ClaSourceLed dummy{};
    device.Leds = &dummy;
    ClaLampInfo lamp{};
    cla_u32 map = 0;
    ClaMappingResult result{};
    CLA_CHECK(ClaBuildLampTable(&device, nullptr, &lamp, &map, &result) ==
              ClaStatusInvalidArgument);
}

CLA_TEST(Backend_WriteFailuresAreReportedNotFatal)
{
    auto backend = MakeBackendWithKeyboard();
    backend->FailNextWrites(2, BackendStatus::NoControl);

    std::vector<BackendLedColor> colors = {{1, 255, 0, 0, 255}};
    CLA_CHECK(backend->SetLampColors("kb-1", colors) == BackendStatus::NoControl);
    CLA_CHECK(backend->SetLampColors("kb-1", colors) == BackendStatus::NoControl);
    /* Third call succeeds again. */
    CLA_CHECK(backend->SetLampColors("kb-1", colors) == BackendStatus::Ok);
}

/* ---------------------------------------------------------------- */
/* Frame pipeline: dirty-lamp tracking (project Phase 13)            */
/* ---------------------------------------------------------------- */

CLA_TEST(Pipeline_FirstFrameWritesEveryLamp)
{
    auto backend = MakeBackendWithKeyboard();
    FramePipeline pipeline = MakePipelineFor(*backend, "kb-1");

    const auto frame = SolidFrame(pipeline.LampCount(), 255, 0, 0);
    std::vector<BackendLedColor> out;
    CLA_CHECK(pipeline.Submit(frame.data(), pipeline.LampCount(), 0, out));
    CLA_CHECK_EQ(out.size(), pipeline.LampCount());
}

/* The central optimisation: an identical frame costs nothing. */
CLA_TEST(Pipeline_UnchangedFrameIsSkippedEntirely)
{
    auto backend = MakeBackendWithKeyboard();
    FramePipeline pipeline = MakePipelineFor(*backend, "kb-1");

    const auto frame = SolidFrame(pipeline.LampCount(), 0, 128, 255);
    std::vector<BackendLedColor> out;

    CLA_CHECK(pipeline.Submit(frame.data(), pipeline.LampCount(), 0, out));
    CLA_CHECK(!pipeline.Submit(frame.data(), pipeline.LampCount(), 1000000, out));
    CLA_CHECK_EQ(out.size(), 0u);
    CLA_CHECK_EQ(pipeline.Stats().FramesSkippedUnchanged, 1u);
    CLA_CHECK_EQ(pipeline.Stats().FramesForwarded, 1u);
}

/* Only the lamps that changed are sent, which is what keeps a moving effect
 * cheap: a wavefront touches a few lamps, not the whole device. */
CLA_TEST(Pipeline_OnlyChangedLampsAreForwarded)
{
    auto backend = MakeBackendWithKeyboard();
    FramePipeline pipeline = MakePipelineFor(*backend, "kb-1");

    auto frame = SolidFrame(pipeline.LampCount(), 10, 20, 30);
    std::vector<BackendLedColor> out;
    CLA_CHECK(pipeline.Submit(frame.data(), pipeline.LampCount(), 0, out));

    frame[5] = ClaLampColor{255, 255, 255, 255};
    frame[9] = ClaLampColor{1, 2, 3, 255};
    CLA_CHECK(pipeline.Submit(frame.data(), pipeline.LampCount(), 100000, out));
    CLA_CHECK_EQ(out.size(), 2u);
}

/* After the backend may have changed the lighting behind our back, the shadow
 * must be discarded or the next frame would be under-written. */
CLA_TEST(Pipeline_InvalidateShadowForcesFullResend)
{
    auto backend = MakeBackendWithKeyboard();
    FramePipeline pipeline = MakePipelineFor(*backend, "kb-1");

    const auto frame = SolidFrame(pipeline.LampCount(), 7, 7, 7);
    std::vector<BackendLedColor> out;
    CLA_CHECK(pipeline.Submit(frame.data(), pipeline.LampCount(), 0, out));
    CLA_CHECK(!pipeline.Submit(frame.data(), pipeline.LampCount(), 100000, out));

    pipeline.InvalidateShadow();
    CLA_CHECK(pipeline.Submit(frame.data(), pipeline.LampCount(), 200000, out));
    CLA_CHECK_EQ(out.size(), pipeline.LampCount());
}

/* ---------------------------------------------------------------- */
/* Frame pipeline: throttling                                        */
/* ---------------------------------------------------------------- */

CLA_TEST(Pipeline_ThrottlesFasterThanMinimumInterval)
{
    auto backend = MakeBackendWithKeyboard();
    /* 60Hz: 16667us between writes. */
    FramePipeline pipeline = MakePipelineFor(*backend, "kb-1", 16667);

    std::vector<BackendLedColor> out;
    auto frame = SolidFrame(pipeline.LampCount(), 0, 0, 0);

    frame[0] = ClaLampColor{1, 0, 0, 255};
    CLA_CHECK(pipeline.Submit(frame.data(), pipeline.LampCount(), 0, out));

    /* 1ms later with a genuinely different frame: too soon. */
    frame[0] = ClaLampColor{2, 0, 0, 255};
    CLA_CHECK(!pipeline.Submit(frame.data(), pipeline.LampCount(), 1000, out));
    CLA_CHECK_EQ(pipeline.Stats().FramesSkippedThrottled, 1u);

    /* Past the interval: accepted. */
    frame[0] = ClaLampColor{3, 0, 0, 255};
    CLA_CHECK(pipeline.Submit(frame.data(), pipeline.LampCount(), 17000, out));
    CLA_CHECK_EQ(pipeline.Stats().FramesForwarded, 2u);
}

/* An unchanged frame must not start the throttle window, or a device that is
 * static for a while would then delay the first real change. */
CLA_TEST(Pipeline_UnchangedFrameDoesNotArmTheThrottle)
{
    auto backend = MakeBackendWithKeyboard();
    FramePipeline pipeline = MakePipelineFor(*backend, "kb-1", 16667);

    std::vector<BackendLedColor> out;
    auto frame = SolidFrame(pipeline.LampCount(), 5, 5, 5);
    CLA_CHECK(pipeline.Submit(frame.data(), pipeline.LampCount(), 0, out));

    /* Same frame well past the interval: skipped as unchanged. */
    CLA_CHECK(!pipeline.Submit(frame.data(), pipeline.LampCount(), 100000, out));

    /* A change immediately after is accepted because the last *write* was at
     * t=0, over an interval ago. */
    frame[3] = ClaLampColor{9, 9, 9, 255};
    CLA_CHECK(pipeline.Submit(frame.data(), pipeline.LampCount(), 100001, out));
}

/* A clock that goes backwards must not be read as an enormous elapsed time. */
CLA_TEST(Pipeline_SurvivesNonMonotonicClock)
{
    auto backend = MakeBackendWithKeyboard();
    FramePipeline pipeline = MakePipelineFor(*backend, "kb-1", 16667);

    std::vector<BackendLedColor> out;
    auto frame = SolidFrame(pipeline.LampCount(), 1, 1, 1);
    CLA_CHECK(pipeline.Submit(frame.data(), pipeline.LampCount(), 1000000, out));

    /* Time jumps backwards; treated as zero elapsed, so throttled, not
     * accepted as though hours had passed. */
    frame[0] = ClaLampColor{2, 2, 2, 255};
    CLA_CHECK(!pipeline.Submit(frame.data(), pipeline.LampCount(), 5, out));
}

/* ---------------------------------------------------------------- */
/* Frame pipeline: brightness and intensity                          */
/* ---------------------------------------------------------------- */

CLA_TEST(Pipeline_FullBrightnessIsExactlyIdentity)
{
    /* Any loss here would be visible as a slightly-wrong white. */
    for (int channel = 0; channel <= 255; ++channel)
    {
        CLA_CHECK_EQ(ApplyBrightness(static_cast<cla_u8>(channel), 255),
                     static_cast<cla_u8>(channel));
    }
}

CLA_TEST(Pipeline_BrightnessScalesWithRounding)
{
    CLA_CHECK_EQ(ApplyBrightness(255, 128), 128);
    CLA_CHECK_EQ(ApplyBrightness(255, 0), 0);
    CLA_CHECK_EQ(ApplyBrightness(0, 128), 0);
    CLA_CHECK_EQ(ApplyBrightness(100, 255), 100);
}

/* The per-lamp intensity channel from Windows and the device brightness are
 * two independent gains; both must apply. */
CLA_TEST(Pipeline_IntensityChannelAndBrightnessBothApply)
{
    auto backend = MakeBackendWithKeyboard();
    FramePipeline pipeline = MakePipelineFor(*backend, "kb-1");

    std::vector<ClaLampColor> frame(pipeline.LampCount());
    for (auto &color : frame) color = ClaLampColor{255, 255, 255, 255};
    frame[0] = ClaLampColor{255, 0, 0, 128};   /* half intensity */

    std::vector<BackendLedColor> out;
    CLA_CHECK(pipeline.Submit(frame.data(), pipeline.LampCount(), 0, out));

    /* Lamp 0 is half-intensity red at full brightness. */
    CLA_CHECK_EQ(out[0].Red, ApplyBrightness(255, 128));
    CLA_CHECK_EQ(out[0].Green, 0);
}

CLA_TEST(Pipeline_ChangingBrightnessResendsEverything)
{
    auto backend = MakeBackendWithKeyboard();
    FramePipeline pipeline = MakePipelineFor(*backend, "kb-1");

    const auto frame = SolidFrame(pipeline.LampCount(), 200, 200, 200);
    std::vector<BackendLedColor> out;
    CLA_CHECK(pipeline.Submit(frame.data(), pipeline.LampCount(), 0, out));
    CLA_CHECK(!pipeline.Submit(frame.data(), pipeline.LampCount(), 100000, out));

    /* Every lamp's output value changed, even though the frame did not. */
    pipeline.SetBrightness(64);
    CLA_CHECK(pipeline.Submit(frame.data(), pipeline.LampCount(), 200000, out));
    CLA_CHECK_EQ(out.size(), pipeline.LampCount());
    CLA_CHECK_EQ(out[0].Red, ApplyBrightness(200, ApplyBrightness(64, 255)));
}

/* ---------------------------------------------------------------- */
/* Frame pipeline: malformed input                                   */
/* ---------------------------------------------------------------- */

CLA_TEST(Pipeline_RejectsMismatchedLampCount)
{
    auto backend = MakeBackendWithKeyboard();
    FramePipeline pipeline = MakePipelineFor(*backend, "kb-1");

    const auto frame = SolidFrame(pipeline.LampCount(), 1, 2, 3);
    std::vector<BackendLedColor> out;

    CLA_CHECK(!pipeline.Submit(frame.data(), pipeline.LampCount() - 1, 0, out));
    CLA_CHECK(!pipeline.Submit(frame.data(), pipeline.LampCount() + 1, 0, out));
    CLA_CHECK(!pipeline.Submit(nullptr, pipeline.LampCount(), 0, out));
    CLA_CHECK_EQ(out.size(), 0u);
}

/* ---------------------------------------------------------------- */
/* End to end through the fake backend                               */
/* ---------------------------------------------------------------- */

/* The whole service path with no Windows and no iCUE: a frame from Windows
 * lands on the right LEDs with the right colours. */
CLA_TEST(EndToEnd_FrameReachesTheCorrectLeds)
{
    auto backend = MakeBackendWithKeyboard();

    std::vector<ClaSourceLed> leds;
    backend->GetLamps("kb-1", leds);

    BackendDeviceInfo info{};
    backend->GetDeviceInfo("kb-1", info);

    ClaSourceDevice source{};
    source.LampArrayKind = info.LampArrayKind;
    source.Units = info.PositionUnits;
    source.LedCount = static_cast<cla_u32>(leds.size());
    source.Leds = leds.data();

    std::vector<ClaLampInfo> lamps(leds.size());
    std::vector<cla_u32> map(leds.size());
    ClaMappingResult mapping{};
    CLA_CHECK(ClaBuildLampTable(&source, nullptr, lamps.data(), map.data(), &mapping) ==
              ClaStatusOk);

    /* Drive the protocol state machine exactly as the driver would. */
    std::vector<ClaLampColor> colors(leds.size());
    ClaLampArrayState state{};
    CLA_CHECK(ClaLampArrayInit(&state, &mapping.Attributes, lamps.data(), colors.data()) ==
              ClaStatusOk);

    /* Windows sends a range update turning every lamp green. */
    std::vector<cla_u8> report(CLA_BUFFER_SIZE_LAMP_RANGE_UPDATE, 0);
    report[0] = CLA_REPORT_ID_LAMP_RANGE_UPDATE;
    report[1] = CLA_LAMP_UPDATE_COMPLETE;
    const cla_u16 last = static_cast<cla_u16>(leds.size() - 1);
    report[2] = 0; report[3] = 0;
    report[4] = static_cast<cla_u8>(last & 0xFF);
    report[5] = static_cast<cla_u8>(last >> 8);
    report[6] = 0; report[7] = 255; report[8] = 0; report[9] = 255;

    ClaUpdateOutcome outcome{};
    CLA_CHECK(ClaLampArraySetFeature(&state, CLA_REPORT_ID_LAMP_RANGE_UPDATE,
                                     report.data(),
                                     static_cast<cla_u32>(report.size()),
                                     &outcome) == ClaStatusOk);
    CLA_CHECK_EQ(outcome.FrameComplete, 1);

    /* Through the pipeline and out to the backend. */
    FramePipeline pipeline(map, 0);
    std::vector<BackendLedColor> writes;
    CLA_CHECK(pipeline.Submit(colors.data(), static_cast<cla_u32>(colors.size()), 0, writes));
    CLA_CHECK(backend->SetLampColors("kb-1", writes) == BackendStatus::Ok);

    const FakeBackend::FakeDevice *device = backend->Device("kb-1");
    CLA_CHECK(device != nullptr);
    CLA_CHECK_EQ(device->LastColors.size(), leds.size());
    for (const BackendLedColor &color : device->LastColors)
    {
        CLA_CHECK_EQ(color.Red, 0);
        CLA_CHECK_EQ(color.Green, 255);
        CLA_CHECK_EQ(color.Blue, 0);
    }

    /* Every backend LED must have been addressed exactly once. */
    std::vector<cla_u32> written;
    for (const auto &color : device->LastColors) written.push_back(color.LedId);
    std::sort(written.begin(), written.end());
    std::vector<cla_u32> expected;
    for (const auto &led : leds) expected.push_back(led.LedId);
    std::sort(expected.begin(), expected.end());
    CLA_CHECK(written == expected);
}

/* A sustained 60Hz animation must not turn into a flood of SDK calls. */
CLA_TEST(EndToEnd_SixtyHertzAnimationStaysBounded)
{
    auto backend = MakeBackendWithKeyboard();
    FramePipeline pipeline = MakePipelineFor(*backend, "kb-1", 16667);

    std::vector<ClaLampColor> frame(pipeline.LampCount());
    std::vector<BackendLedColor> out;

    /* One second of a wavefront sweeping across the device, offered at 240Hz
     * -- four times faster than the advertised minimum interval. */
    cla_u64 now = 0;
    int accepted = 0;
    for (int tick = 0; tick < 240; ++tick)
    {
        for (cla_u32 lamp = 0; lamp < pipeline.LampCount(); ++lamp)
        {
            const bool lit = (lamp % 64) == static_cast<cla_u32>(tick % 64);
            frame[lamp] = lit ? ClaLampColor{255, 255, 255, 255} : ClaLampColor{0, 0, 0, 0};
        }
        if (pipeline.Submit(frame.data(), pipeline.LampCount(), now, out))
        {
            accepted++;
            backend->SetLampColors("kb-1", out);
        }
        now += 1000000 / 240;
    }

    /*
     * Throttled to at most ~60 writes/sec rather than 240.
     *
     * The exact figure here is 48, not 60: offers arrive every 4166us and a
     * write is only allowed once a full 16667us has elapsed since the last
     * one, so every fifth offer passes (5 x 4166 = 20830us).  That
     * quantisation is inherent to a simple "never faster than the interval"
     * rule and is harmless -- Windows guarantees it will not send faster than
     * the MinUpdateInterval we advertise, so this over-driven case does not
     * arise in practice.  The property being asserted is the one that matters:
     * the SDK call rate is capped and does not track the offer rate.
     */
    CLA_CHECK(accepted <= 61);
    CLA_CHECK(accepted >= 40);
    CLA_CHECK_EQ(backend->TotalWrites(), static_cast<cla_u64>(accepted));

    /* And each write touched only the lamps that actually changed. */
    const cla_u64 averageLampsPerWrite =
        pipeline.Stats().LampWritesForwarded / pipeline.Stats().FramesForwarded;
    CLA_CHECK(averageLampsPerWrite < pipeline.LampCount());
}

/* The case that actually occurs: Windows honours the MinUpdateInterval we
 * advertise, so at exactly that rate no frame may be dropped.  A throttle that
 * shaved frames here would visibly stutter every effect. */
CLA_TEST(EndToEnd_AtAdvertisedRateNoFrameIsDropped)
{
    auto backend = MakeBackendWithKeyboard();
    constexpr cla_u64 kInterval = 16667;   /* 60Hz, as advertised */
    FramePipeline pipeline = MakePipelineFor(*backend, "kb-1", kInterval);

    std::vector<ClaLampColor> frame(pipeline.LampCount());
    std::vector<BackendLedColor> out;

    int accepted = 0;
    for (int tick = 0; tick < 60; ++tick)
    {
        for (cla_u32 lamp = 0; lamp < pipeline.LampCount(); ++lamp)
        {
            const bool lit = (lamp % 60) == static_cast<cla_u32>(tick);
            frame[lamp] = lit ? ClaLampColor{255, 255, 255, 255} : ClaLampColor{0, 0, 0, 0};
        }
        if (pipeline.Submit(frame.data(), pipeline.LampCount(),
                            static_cast<cla_u64>(tick) * kInterval, out))
        {
            accepted++;
        }
    }

    CLA_CHECK_EQ(accepted, 60);
    CLA_CHECK_EQ(pipeline.Stats().FramesSkippedThrottled, 0u);
}
