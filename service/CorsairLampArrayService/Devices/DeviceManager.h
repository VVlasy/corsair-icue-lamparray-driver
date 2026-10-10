/*
 * Copyright (c) Corsair Dynamic Lighting Bridge contributors.
 * SPDX-License-Identifier: MIT
 *
 * Ties the backend and the driver together.  Windows only.
 *
 * Responsibilities:
 *   - discover Corsair devices and give each one a virtual LampArray;
 *   - route frames from the driver through FramePipeline to the backend;
 *   - keep the virtual devices consistent as Corsair devices and iCUE itself
 *     come and go (project Phases 10 and 12).
 *
 * Lifecycle policy
 * ----------------
 * A virtual LampArray is created once per Corsair device and is NOT destroyed
 * when the device or iCUE goes away.  It is marked unavailable instead.  The
 * reasons matter:
 *
 *   - Windows keys per-device Dynamic Lighting settings off the device
 *     identity.  Tearing the device down and recreating it on every iCUE
 *     restart would churn those settings and make the device flicker in and
 *     out of the Settings list.
 *   - The project requires that iCUE can be restarted without a reboot and
 *     that hotplug does not create duplicate virtual devices.  Keeping a
 *     stable virtual device keyed by the Corsair device id is what delivers
 *     both.
 *
 * The virtual device is destroyed only when the service stops.
 */
#ifndef CLA_DEVICE_MANAGER_H
#define CLA_DEVICE_MANAGER_H

#include "Corsair/ICorsairBackend.h"
#include "Devices/FramePipeline.h"
#include "Ipc/DriverLink.h"

#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace cla {

struct DeviceManagerOptions
{
    /* Take exclusive lighting control from iCUE.  Off by default; see
     * ICorsairBackend::RequestControl. */
    bool ExclusiveControl = false;

    /* Layer priority for the SDK client.  iCUE is 127 and shared clients
     * default to 128. */
    unsigned int LayerPriority = 128;

    /* Advertised to Windows as MinUpdateIntervalInMicroseconds and used as the
     * pipeline's throttle floor.  16667us = 60Hz. */
    cla_u32 MinUpdateIntervalMicroseconds = 16667;

    /* How often to retry while iCUE is unavailable. */
    unsigned int ReconnectIntervalMilliseconds = 5000;

    /* Placeholder VID/PID for the virtual devices.  Not USB-IF allocated --
     * see docs/architecture.md. */
    cla_u16 VendorId = 0x1209;
    cla_u16 ProductId = 0x4C41;
};

class DeviceManager
{
public:
    DeviceManager(ICorsairBackend &backend, DriverLink &driver, DeviceManagerOptions options);
    ~DeviceManager();

    /* Opens the driver link, connects the backend and starts the update pump.
     * Returns false only for problems that cannot be retried away (no driver);
     * iCUE being absent is not a failure. */
    bool Start(std::string &error);
    void Stop();

    /* Called periodically by the service loop: reconnects the backend if
     * needed and picks up devices that appeared. */
    void Poll();

    size_t DeviceCount() const;

private:
    struct ManagedDevice
    {
        std::string BackendId;
        cla_u32 LampArrayId = 0;
        std::unique_ptr<FramePipeline> Pipeline;
        bool BackendAvailable = false;
        /* Windows has taken control (autonomous mode disabled). */
        bool HostControlled = false;
        cla_u64 FramesReceived = 0;
        cla_u64 WriteFailures = 0;
    };

    void OnBackendEvent(const BackendEvent &event);
    void OnDriverUpdate(const LampArrayUpdate &update);

    void SyncDevices();
    bool AddDevice(const BackendDeviceInfo &info);
    void SetDeviceAvailability(const std::string &backendId, bool available);

    /* Caller must hold m_mutex. */
    ManagedDevice *FindByLampArrayIdLocked(cla_u32 lampArrayId);

    static cla_u64 NowMicroseconds();

    ICorsairBackend &m_backend;
    DriverLink &m_driver;
    DeviceManagerOptions m_options;

    mutable std::mutex m_mutex;
    std::map<std::string, ManagedDevice> m_devices;

    bool m_started = false;
    bool m_backendConnected = false;
    cla_u64 m_lastReconnectAttemptMs = 0;
};

} // namespace cla

#endif /* CLA_DEVICE_MANAGER_H */
