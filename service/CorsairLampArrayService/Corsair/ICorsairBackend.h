/*
 * Copyright (c) Corsair Dynamic Lighting Bridge contributors.
 * SPDX-License-Identifier: MIT
 *
 * The lighting backend abstraction (project Phase 6).
 *
 * Nothing in this header mentions the iCUE SDK, includes its headers, or
 * assumes Windows.  That is what makes the rest of the service testable
 * without iCUE, without Corsair hardware and without a Windows machine, and it
 * is also what keeps the door open for a second backend later.
 *
 * The driver is deliberately NOT coupled to this interface: the driver never
 * sees a backend, only a validated lamp table and a stream of frames.
 */
#ifndef CLA_ICORSAIR_BACKEND_H
#define CLA_ICORSAIR_BACKEND_H

#include "cla/ClaTypes.h"
#include "cla/CorsairMapping.h"

#include <string>
#include <vector>
#include <functional>
#include <memory>

namespace cla {

enum class BackendStatus
{
    Ok = 0,
    /* The backend service (iCUE) is not running, or third-party control is
     * disabled in its settings. */
    NotConnected,
    /* Another client holds exclusive control. */
    NoControl,
    DeviceNotFound,
    InvalidArgument,
    NotSupported,
    Failed
};

const char *ToString(BackendStatus status);

/* What the backend knows about one physical device. */
struct BackendDeviceInfo
{
    /* Stable, backend-assigned identifier.  Used to derive the virtual
     * device's serial and container id, so it must be stable across restarts
     * for the user's Dynamic Lighting settings to persist. */
    std::string Id;
    std::string Model;
    std::string Serial;

    /* Mapped from the backend's own device taxonomy. */
    cla_u32 LampArrayKind = 0;
    /* Whether this device's LED coordinates are millimetres or logical units;
     * the iCUE SDK differs by device type. */
    cla_u32 PositionUnits = 0;
    /* Whether the LEDs belong to one component or span the case
     * (ClaSourceScope); sizes logical-unit devices. */
    cla_u32 Scope = 0;

    cla_u32 LedCount = 0;
    cla_u32 ChannelCount = 0;
};

struct BackendLedColor
{
    cla_u32 LedId = 0;
    cla_u8 Red = 0;
    cla_u8 Green = 0;
    cla_u8 Blue = 0;
    cla_u8 Alpha = 255;
};

enum class BackendEventKind
{
    SessionStateChanged,
    DeviceConnected,
    DeviceDisconnected
};

struct BackendEvent
{
    BackendEventKind Kind = BackendEventKind::SessionStateChanged;
    std::string DeviceId;
    /* Meaningful for SessionStateChanged. */
    bool SessionConnected = false;
};

using BackendEventHandler = std::function<void(const BackendEvent &)>;

/*
 * A lighting backend.
 *
 * Implementations must be safe to call from more than one thread, and must
 * never throw: the service is long-running and a backend fault (iCUE exiting
 * mid-call, a device vanishing) is an ordinary event, not an exception.  Every
 * operation reports failure by returning a BackendStatus.
 */
class ICorsairBackend
{
public:
    virtual ~ICorsairBackend() = default;

    /* Connecting is asynchronous in the real SDK: Initialize() returning Ok
     * means the attempt started, and IsConnected() reports the outcome.  A
     * backend that cannot connect yet is not an error -- iCUE may simply not
     * be running, and the service must keep retrying. */
    virtual BackendStatus Initialize() = 0;
    virtual void Shutdown() = 0;
    virtual bool IsConnected() const = 0;

    virtual BackendStatus EnumerateDevices(std::vector<BackendDeviceInfo> &devices) = 0;
    virtual BackendStatus GetDeviceInfo(const std::string &deviceId,
                                        BackendDeviceInfo &info) = 0;

    /* The device's LEDs with their positions.  Named "lamps" to match the
     * project's interface spec; these are backend LEDs, which become
     * LampArray lamps only after CorsairMapping has ordered and normalised
     * them. */
    virtual BackendStatus GetLamps(const std::string &deviceId,
                                   std::vector<ClaSourceLed> &leds) = 0;

    virtual BackendStatus SetLampColors(const std::string &deviceId,
                                        const std::vector<BackendLedColor> &colors) = 0;

    /* Output scale, 0..255.  Applied by the service, not the hardware. */
    virtual BackendStatus SetBrightness(const std::string &deviceId, cla_u8 brightness) = 0;

    /*
     * Exclusive control is NOT taken by default.
     *
     * The iCUE SDK's shared mode lets iCUE and this service coexist, layered
     * by priority.  Taking exclusive control would evict the user's own iCUE
     * profiles for as long as the service runs, which is not a decision a
     * background bridge should make on their behalf.  See docs/corsair-sdk.md.
     */
    virtual BackendStatus RequestControl(const std::string &deviceId, bool exclusive) = 0;
    virtual BackendStatus ReleaseControl(const std::string &deviceId) = 0;

    virtual void SubscribeEvents(BackendEventHandler handler) = 0;
};

} // namespace cla

#endif /* CLA_ICORSAIR_BACKEND_H */
