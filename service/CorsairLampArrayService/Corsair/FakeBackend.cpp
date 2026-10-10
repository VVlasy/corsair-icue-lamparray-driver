/*
 * Copyright (c) Corsair Dynamic Lighting Bridge contributors.
 * SPDX-License-Identifier: MIT
 */

#include "Corsair/FakeBackend.h"

namespace cla {

FakeBackend::FakeBackend() = default;

FakeBackend::~FakeBackend()
{
    Shutdown();
}

BackendStatus FakeBackend::Initialize()
{
    bool connected;
    {
        std::lock_guard<std::mutex> guard(m_mutex);
        m_initialized = true;
        m_connected = m_connectOnInitialize;
        connected = m_connected;
    }

    /* Notify outside the lock, via Emit, for the same reason every other
     * notification here does: handlers routinely call straight back into the
     * backend, and doing that under our own mutex would deadlock. */
    BackendEvent event;
    event.Kind = BackendEventKind::SessionStateChanged;
    event.SessionConnected = connected;
    Emit(event);

    return BackendStatus::Ok;
}

void FakeBackend::Shutdown()
{
    std::lock_guard<std::mutex> guard(m_mutex);
    m_initialized = false;
    m_connected = false;
}

bool FakeBackend::IsConnected() const
{
    std::lock_guard<std::mutex> guard(m_mutex);
    return m_initialized && m_connected;
}

void FakeBackend::Emit(const BackendEvent &event)
{
    /* Copy the handler under the lock, then call it outside: handlers commonly
     * turn around and query the backend, which would deadlock. */
    BackendEventHandler handler;
    {
        std::lock_guard<std::mutex> guard(m_mutex);
        handler = m_handler;
    }
    if (handler)
    {
        handler(event);
    }
}

BackendStatus FakeBackend::EnumerateDevices(std::vector<BackendDeviceInfo> &devices)
{
    devices.clear();

    std::lock_guard<std::mutex> guard(m_mutex);
    if (!m_initialized || !m_connected)
    {
        return BackendStatus::NotConnected;
    }
    for (const auto &entry : m_devices)
    {
        if (entry.second.Connected)
        {
            devices.push_back(entry.second.Info);
        }
    }
    return BackendStatus::Ok;
}

BackendStatus FakeBackend::GetDeviceInfo(const std::string &deviceId, BackendDeviceInfo &info)
{
    std::lock_guard<std::mutex> guard(m_mutex);
    if (!m_initialized || !m_connected)
    {
        return BackendStatus::NotConnected;
    }
    auto it = m_devices.find(deviceId);
    if (it == m_devices.end() || !it->second.Connected)
    {
        return BackendStatus::DeviceNotFound;
    }
    info = it->second.Info;
    return BackendStatus::Ok;
}

BackendStatus FakeBackend::GetLamps(const std::string &deviceId, std::vector<ClaSourceLed> &leds)
{
    leds.clear();

    std::lock_guard<std::mutex> guard(m_mutex);
    if (!m_initialized || !m_connected)
    {
        return BackendStatus::NotConnected;
    }
    auto it = m_devices.find(deviceId);
    if (it == m_devices.end() || !it->second.Connected)
    {
        return BackendStatus::DeviceNotFound;
    }
    leds = it->second.Leds;
    return BackendStatus::Ok;
}

BackendStatus FakeBackend::SetLampColors(const std::string &deviceId,
                                         const std::vector<BackendLedColor> &colors)
{
    std::lock_guard<std::mutex> guard(m_mutex);
    if (!m_initialized || !m_connected)
    {
        return BackendStatus::NotConnected;
    }
    if (m_failWriteCount > 0)
    {
        m_failWriteCount--;
        return m_failWriteStatus;
    }
    auto it = m_devices.find(deviceId);
    if (it == m_devices.end() || !it->second.Connected)
    {
        return BackendStatus::DeviceNotFound;
    }

    /* Merge, rather than replace: real writes are partial updates and the
     * device retains anything not mentioned.  Tests depend on that. */
    for (const BackendLedColor &color : colors)
    {
        bool replaced = false;
        for (BackendLedColor &existing : it->second.LastColors)
        {
            if (existing.LedId == color.LedId)
            {
                existing = color;
                replaced = true;
                break;
            }
        }
        if (!replaced)
        {
            it->second.LastColors.push_back(color);
        }
    }
    it->second.WriteCount++;
    return BackendStatus::Ok;
}

BackendStatus FakeBackend::SetBrightness(const std::string &deviceId, cla_u8 brightness)
{
    (void)brightness;
    std::lock_guard<std::mutex> guard(m_mutex);
    if (!m_initialized || !m_connected)
    {
        return BackendStatus::NotConnected;
    }
    return m_devices.count(deviceId) ? BackendStatus::Ok : BackendStatus::DeviceNotFound;
}

BackendStatus FakeBackend::RequestControl(const std::string &deviceId, bool exclusive)
{
    (void)exclusive;
    std::lock_guard<std::mutex> guard(m_mutex);
    if (!m_initialized || !m_connected)
    {
        return BackendStatus::NotConnected;
    }
    return m_devices.count(deviceId) ? BackendStatus::Ok : BackendStatus::DeviceNotFound;
}

BackendStatus FakeBackend::ReleaseControl(const std::string &deviceId)
{
    std::lock_guard<std::mutex> guard(m_mutex);
    if (!m_initialized || !m_connected)
    {
        return BackendStatus::NotConnected;
    }
    return m_devices.count(deviceId) ? BackendStatus::Ok : BackendStatus::DeviceNotFound;
}

void FakeBackend::SubscribeEvents(BackendEventHandler handler)
{
    std::lock_guard<std::mutex> guard(m_mutex);
    m_handler = std::move(handler);
}

void FakeBackend::AddDevice(const std::string &id,
                            const std::string &model,
                            cla_u32 lampArrayKind,
                            const std::vector<ClaSourceLed> &leds,
                            cla_u32 positionUnits,
                            cla_u32 scope)
{
    {
        std::lock_guard<std::mutex> guard(m_mutex);
        FakeDevice device;
        device.Info.Id = id;
        device.Info.Model = model;
        device.Info.Serial = id;
        device.Info.LampArrayKind = lampArrayKind;
        device.Info.PositionUnits = positionUnits;
        device.Info.Scope = scope;
        device.Info.LedCount = static_cast<cla_u32>(leds.size());
        device.Info.ChannelCount = 0;
        device.Leds = leds;
        device.Connected = true;
        m_devices[id] = std::move(device);
    }

    BackendEvent event;
    event.Kind = BackendEventKind::DeviceConnected;
    event.DeviceId = id;
    Emit(event);
}

void FakeBackend::AddKeyboard(const std::string &id, int rows, int columns)
{
    std::vector<ClaSourceLed> leds;
    cla_u32 ledId = 1;
    for (int row = 0; row < rows; ++row)
    {
        for (int column = 0; column < columns; ++column)
        {
            ClaSourceLed led{};
            led.LedId = ledId++;
            /* 19.05mm key pitch, the standard full-size spacing. */
            led.X = 10.0 + column * 19.05;
            led.Y = 10.0 + row * 19.05;
            leds.push_back(led);
        }
    }
    AddDevice(id, "Fake K95 RGB", ClaLampArrayKindKeyboard, leds);
}

void FakeBackend::RemoveDevice(const std::string &id)
{
    {
        std::lock_guard<std::mutex> guard(m_mutex);
        m_devices.erase(id);
    }
    BackendEvent event;
    event.Kind = BackendEventKind::DeviceDisconnected;
    event.DeviceId = id;
    Emit(event);
}

void FakeBackend::SimulateSessionLost()
{
    {
        std::lock_guard<std::mutex> guard(m_mutex);
        m_connected = false;
    }
    BackendEvent event;
    event.Kind = BackendEventKind::SessionStateChanged;
    event.SessionConnected = false;
    Emit(event);
}

void FakeBackend::SimulateSessionRestored()
{
    {
        std::lock_guard<std::mutex> guard(m_mutex);
        m_connected = true;
    }
    BackendEvent event;
    event.Kind = BackendEventKind::SessionStateChanged;
    event.SessionConnected = true;
    Emit(event);
}

void FakeBackend::SimulateDeviceDisconnected(const std::string &id)
{
    {
        std::lock_guard<std::mutex> guard(m_mutex);
        auto it = m_devices.find(id);
        if (it == m_devices.end())
        {
            return;
        }
        it->second.Connected = false;
    }
    BackendEvent event;
    event.Kind = BackendEventKind::DeviceDisconnected;
    event.DeviceId = id;
    Emit(event);
}

void FakeBackend::SimulateDeviceConnected(const std::string &id)
{
    {
        std::lock_guard<std::mutex> guard(m_mutex);
        auto it = m_devices.find(id);
        if (it == m_devices.end())
        {
            return;
        }
        it->second.Connected = true;
    }
    BackendEvent event;
    event.Kind = BackendEventKind::DeviceConnected;
    event.DeviceId = id;
    Emit(event);
}

void FakeBackend::FailNextWrites(int count, BackendStatus status)
{
    std::lock_guard<std::mutex> guard(m_mutex);
    m_failWriteCount = count;
    m_failWriteStatus = status;
}

const FakeBackend::FakeDevice *FakeBackend::Device(const std::string &id) const
{
    std::lock_guard<std::mutex> guard(m_mutex);
    auto it = m_devices.find(id);
    return (it == m_devices.end()) ? nullptr : &it->second;
}

cla_u64 FakeBackend::TotalWrites() const
{
    std::lock_guard<std::mutex> guard(m_mutex);
    cla_u64 total = 0;
    for (const auto &entry : m_devices)
    {
        total += entry.second.WriteCount;
    }
    return total;
}

} // namespace cla
