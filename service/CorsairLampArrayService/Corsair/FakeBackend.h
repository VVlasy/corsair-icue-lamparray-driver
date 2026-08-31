/*
 * Copyright (c) Corsair Dynamic Lighting Bridge contributors.
 * SPDX-License-Identifier: MIT
 *
 * An in-memory backend for tests and for bringing the service up without iCUE
 * or Corsair hardware (project Phase 6: "The service should be able to run a
 * fake backend for automated testing").
 *
 * It is not merely a stub.  The failures this project has to survive -- iCUE
 * not running, iCUE exiting mid-session, a device disappearing, a device with
 * zero LEDs, an SDK call failing -- are all things this backend can be told to
 * do on demand, so those paths get exercised in CI rather than only when a
 * user hits them.
 */
#ifndef CLA_FAKE_BACKEND_H
#define CLA_FAKE_BACKEND_H

#include "Corsair/ICorsairBackend.h"

#include <map>
#include <mutex>

namespace cla {

class FakeBackend final : public ICorsairBackend
{
public:
    struct FakeDevice
    {
        BackendDeviceInfo Info;
        std::vector<ClaSourceLed> Leds;
        /* Last colours written, so tests can assert what reached "hardware". */
        std::vector<BackendLedColor> LastColors;
        cla_u64 WriteCount = 0;
        bool Connected = true;
    };

    FakeBackend();
    ~FakeBackend() override;

    /* --- ICorsairBackend --- */
    BackendStatus Initialize() override;
    void Shutdown() override;
    bool IsConnected() const override;

    BackendStatus EnumerateDevices(std::vector<BackendDeviceInfo> &devices) override;
    BackendStatus GetDeviceInfo(const std::string &deviceId, BackendDeviceInfo &info) override;
    BackendStatus GetLamps(const std::string &deviceId, std::vector<ClaSourceLed> &leds) override;
    BackendStatus SetLampColors(const std::string &deviceId,
                                const std::vector<BackendLedColor> &colors) override;
    BackendStatus SetBrightness(const std::string &deviceId, cla_u8 brightness) override;
    BackendStatus RequestControl(const std::string &deviceId, bool exclusive) override;
    BackendStatus ReleaseControl(const std::string &deviceId) override;
    void SubscribeEvents(BackendEventHandler handler) override;

    /* --- test control --- */

    /* Add a device.  Positions are millimetres unless stated otherwise. */
    void AddDevice(const std::string &id,
                   const std::string &model,
                   cla_u32 lampArrayKind,
                   const std::vector<ClaSourceLed> &leds,
                   cla_u32 positionUnits = ClaSourceUnitsMillimeters);

    /* Add a keyboard-shaped grid; convenient for most tests. */
    void AddKeyboard(const std::string &id, int rows = 6, int columns = 18);

    void RemoveDevice(const std::string &id);

    /* Simulate the SDK session dropping (iCUE exited or crashed) and coming
     * back.  Fires the same events the real backend would. */
    void SimulateSessionLost();
    void SimulateSessionRestored();

    /* Simulate a device being unplugged / plugged in. */
    void SimulateDeviceDisconnected(const std::string &id);
    void SimulateDeviceConnected(const std::string &id);

    /* Make the next N SetLampColors calls fail with this status. */
    void FailNextWrites(int count, BackendStatus status);

    /* When set, Initialize() reports success but never connects -- exactly
     * what happens when iCUE is not installed or not running. */
    void SetConnectOnInitialize(bool connect) { m_connectOnInitialize = connect; }

    const FakeDevice *Device(const std::string &id) const;
    cla_u64 TotalWrites() const;

private:
    void Emit(const BackendEvent &event);

    mutable std::mutex m_mutex;
    std::map<std::string, FakeDevice> m_devices;
    BackendEventHandler m_handler;
    bool m_initialized = false;
    bool m_connected = false;
    bool m_connectOnInitialize = true;
    int m_failWriteCount = 0;
    BackendStatus m_failWriteStatus = BackendStatus::Failed;
};

} // namespace cla

#endif /* CLA_FAKE_BACKEND_H */
