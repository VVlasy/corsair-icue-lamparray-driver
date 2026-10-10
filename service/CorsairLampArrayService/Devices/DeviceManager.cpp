/*
 * Copyright (c) Corsair Dynamic Lighting Bridge contributors.
 * SPDX-License-Identifier: MIT
 */

#include "Devices/DeviceManager.h"
#include "Logging/Logger.h"
#include "cla/CorsairMapping.h"

#include <windows.h>

#include <algorithm>

namespace cla {

namespace {

std::wstring Widen(const std::string &narrow)
{
    if (narrow.empty())
    {
        return std::wstring();
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, narrow.c_str(),
                                         static_cast<int>(narrow.size()), nullptr, 0);
    std::wstring wide(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, narrow.c_str(), static_cast<int>(narrow.size()),
                        wide.data(), size);
    return wide;
}

} // namespace

DeviceManager::DeviceManager(ICorsairBackend &backend, DriverLink &driver,
                             DeviceManagerOptions options)
    : m_backend(backend), m_driver(driver), m_options(options)
{
}

DeviceManager::~DeviceManager()
{
    Stop();
}

cla_u64 DeviceManager::NowMicroseconds()
{
    /* QueryPerformanceCounter, not GetTickCount: the pipeline throttles on
     * microsecond intervals and the tick counter's ~15ms granularity would
     * make a 60Hz cadence unmeasurable. */
    static LARGE_INTEGER frequency = [] {
        LARGE_INTEGER value{};
        QueryPerformanceFrequency(&value);
        return value;
    }();

    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    if (frequency.QuadPart == 0)
    {
        return 0;
    }
    return static_cast<cla_u64>((counter.QuadPart * 1000000LL) / frequency.QuadPart);
}

bool DeviceManager::Start(std::string &error)
{
    if (!m_driver.Open(error))
    {
        return false;   /* no driver: nothing can work, and retrying will not help */
    }

    ClaDriverInfo info{};
    if (!m_driver.GetDriverInfo(info, error))
    {
        return false;
    }
    if (info.ProtocolVersion != CLA_IPC_PROTOCOL_VERSION)
    {
        error = "driver speaks IPC protocol version " +
                std::to_string(info.ProtocolVersion) + " but this service expects " +
                std::to_string(CLA_IPC_PROTOCOL_VERSION) +
                "; reinstall so the driver and service match";
        return false;
    }

    CLA_LOG_INFO("Driver %u.%u.%u, protocol %u, capacity %u arrays x %u lamps",
                 info.DriverVersionMajor, info.DriverVersionMinor, info.DriverVersionPatch,
                 info.ProtocolVersion, info.MaxLampArrayCount, info.MaxLampsPerArray);

    if (!m_driver.StartUpdatePump([this](const LampArrayUpdate &update) {
            OnDriverUpdate(update);
        }, error))
    {
        return false;
    }

    m_backend.SubscribeEvents([this](const BackendEvent &event) { OnBackendEvent(event); });

    /* iCUE not running is expected, not fatal: Poll() keeps retrying. */
    const BackendStatus status = m_backend.Initialize();
    if (status != BackendStatus::Ok)
    {
        CLA_LOG_WARN("Backend initialise reported %s; will keep retrying",
                     ToString(status));
    }

    m_started = true;
    Poll();
    return true;
}

void DeviceManager::Stop()
{
    if (!m_started)
    {
        return;
    }
    m_started = false;

    m_driver.StopUpdatePump();

    std::map<std::string, ManagedDevice> devices;
    {
        std::lock_guard<std::mutex> guard(m_mutex);
        devices.swap(m_devices);
    }

    for (auto &entry : devices)
    {
        /* Hand the device back to iCUE before disappearing, so the user's own
         * profile resumes rather than the last frame we drew persisting. */
        m_backend.ReleaseControl(entry.first);

        std::string error;
        if (!m_driver.DestroyLampArray(entry.second.LampArrayId, error))
        {
            CLA_LOG_WARN("Destroying LampArray %u failed: %s",
                         entry.second.LampArrayId, error.c_str());
        }
    }

    m_backend.Shutdown();
    m_driver.Close();
}

void DeviceManager::Poll()
{
    if (!m_started)
    {
        return;
    }

    const bool connected = m_backend.IsConnected();

    if (!connected)
    {
        /* Retry on a timer rather than every poll: a reconnect attempt against
         * an absent iCUE is not free. */
        const cla_u64 nowMs = GetTickCount64();
        if (nowMs - m_lastReconnectAttemptMs >= m_options.ReconnectIntervalMilliseconds)
        {
            m_lastReconnectAttemptMs = nowMs;
            CLA_LOG_DEBUG("Backend not connected; retrying");
            m_backend.Initialize();
        }
        return;
    }

    if (!m_backendConnected)
    {
        CLA_LOG_INFO("Backend connected");
        m_backendConnected = true;
    }

    SyncDevices();
}

void DeviceManager::SyncDevices()
{
    std::vector<BackendDeviceInfo> devices;
    const BackendStatus status = m_backend.EnumerateDevices(devices);
    if (status != BackendStatus::Ok)
    {
        CLA_LOG_DEBUG("EnumerateDevices reported %s", ToString(status));
        return;
    }

    /* Devices that are present now. */
    for (const BackendDeviceInfo &info : devices)
    {
        bool known;
        {
            std::lock_guard<std::mutex> guard(m_mutex);
            known = m_devices.count(info.Id) != 0;
        }

        if (!known)
        {
            if (info.LedCount == 0)
            {
                /* Nothing to light. Not an error -- some Corsair devices are
                 * enumerated without controllable LEDs. */
                CLA_LOG_INFO("Skipping '%s' (%s): no controllable LEDs",
                             info.Model.c_str(), info.Id.c_str());
                continue;
            }
            AddDevice(info);
        }
        else
        {
            SetDeviceAvailability(info.Id, true);
        }
    }

    /* Devices we know about that are no longer present. */
    std::vector<std::string> missing;
    {
        std::lock_guard<std::mutex> guard(m_mutex);
        for (const auto &entry : m_devices)
        {
            const bool present = std::any_of(devices.begin(), devices.end(),
                                             [&](const BackendDeviceInfo &info) {
                                                 return info.Id == entry.first;
                                             });
            if (!present && entry.second.BackendAvailable)
            {
                missing.push_back(entry.first);
            }
        }
    }
    for (const std::string &id : missing)
    {
        CLA_LOG_INFO("Corsair device %s is no longer present", id.c_str());
        SetDeviceAvailability(id, false);
    }
}

bool DeviceManager::AddDevice(const BackendDeviceInfo &info)
{
    std::vector<ClaSourceLed> leds;
    const BackendStatus status = m_backend.GetLamps(info.Id, leds);
    if (status != BackendStatus::Ok || leds.empty())
    {
        CLA_LOG_WARN("Could not read LED positions for '%s': %s",
                     info.Model.c_str(), ToString(status));
        return false;
    }

    if (leds.size() > CLA_MAX_LAMPS_PER_ARRAY)
    {
        CLA_LOG_WARN("'%s' reports %zu LEDs, above the %u supported; truncating",
                     info.Model.c_str(), leds.size(), CLA_MAX_LAMPS_PER_ARRAY);
        leds.resize(CLA_MAX_LAMPS_PER_ARRAY);
    }

    ClaSourceDevice source{};
    source.LampArrayKind = info.LampArrayKind;
    source.Units = info.PositionUnits;
    source.Scope = info.Scope;
    source.LedCount = static_cast<cla_u32>(leds.size());
    source.Leds = leds.data();

    ClaMappingOptions mappingOptions{};
    ClaMappingOptionsInit(&mappingOptions);
    mappingOptions.MinUpdateIntervalMicroseconds = m_options.MinUpdateIntervalMicroseconds;

    std::vector<ClaLampInfo> lamps(leds.size());
    std::vector<cla_u32> lampIdToLedId(leds.size());
    ClaMappingResult mapping{};

    if (ClaBuildLampTable(&source, &mappingOptions, lamps.data(),
                          lampIdToLedId.data(), &mapping) != ClaStatusOk)
    {
        CLA_LOG_WARN("Could not build a lamp table for '%s'", info.Model.c_str());
        return false;
    }

    LampArrayCreateParameters parameters;
    parameters.DeviceName = Widen(info.Model.empty() ? info.Id : info.Model);
    /* The serial drives the virtual device's InstanceID and ContainerID, so it
     * must be stable for this physical device.  The Corsair device id is
     * stable; the serial field can be empty on some devices. */
    parameters.SerialNumber = Widen(info.Id);
    parameters.VendorId = m_options.VendorId;
    parameters.ProductId = m_options.ProductId;
    parameters.VersionNumber = 0x0100;
    parameters.Attributes = mapping.Attributes;
    parameters.Lamps = std::move(lamps);

    cla_u32 lampArrayId = 0;
    std::string error;
    if (!m_driver.CreateLampArray(parameters, lampArrayId, error))
    {
        CLA_LOG_ERROR("Creating a LampArray for '%s' failed: %s",
                      info.Model.c_str(), error.c_str());
        return false;
    }

    /* Shared control by default; see ICorsairBackend::RequestControl. */
    m_backend.RequestControl(info.Id, m_options.ExclusiveControl);

    ManagedDevice managed;
    managed.BackendId = info.Id;
    managed.LampArrayId = lampArrayId;
    managed.Pipeline = std::make_unique<FramePipeline>(
        std::move(lampIdToLedId), m_options.MinUpdateIntervalMicroseconds);
    managed.BackendAvailable = true;

    {
        std::lock_guard<std::mutex> guard(m_mutex);
        m_devices[info.Id] = std::move(managed);
    }

    CLA_LOG_INFO("Exposed '%s' (%s) as LampArray %u with %u lamps, kind %u",
                 info.Model.c_str(), info.Id.c_str(), lampArrayId,
                 mapping.Attributes.LampCount, mapping.Attributes.LampArrayKind);
    return true;
}

void DeviceManager::SetDeviceAvailability(const std::string &backendId, bool available)
{
    cla_u32 lampArrayId = 0;
    bool changed = false;

    {
        std::lock_guard<std::mutex> guard(m_mutex);
        auto it = m_devices.find(backendId);
        if (it == m_devices.end() || it->second.BackendAvailable == available)
        {
            return;
        }
        it->second.BackendAvailable = available;
        lampArrayId = it->second.LampArrayId;
        changed = true;

        if (it->second.Pipeline)
        {
            /* Whatever the device is showing now is not what we last sent --
             * iCUE has had it back, or it has been power-cycled.  Discard the
             * shadow so the next frame is written in full. */
            it->second.Pipeline->InvalidateShadow();
        }
    }

    if (!changed)
    {
        return;
    }

    std::string error;
    if (!m_driver.SetDeviceState(lampArrayId,
                                 available ? ClaDeviceStateAvailable
                                           : ClaDeviceStateUnavailable,
                                 error))
    {
        CLA_LOG_WARN("SetDeviceState for LampArray %u failed: %s",
                     lampArrayId, error.c_str());
    }

    CLA_LOG_INFO("LampArray %u is now %s", lampArrayId,
                 available ? "available" : "unavailable");
}

void DeviceManager::OnBackendEvent(const BackendEvent &event)
{
    switch (event.Kind)
    {
    case BackendEventKind::SessionStateChanged:
        if (!event.SessionConnected)
        {
            CLA_LOG_WARN("Backend session lost; devices marked unavailable");
            m_backendConnected = false;

            std::vector<std::string> ids;
            {
                std::lock_guard<std::mutex> guard(m_mutex);
                for (const auto &entry : m_devices)
                {
                    ids.push_back(entry.first);
                }
            }
            for (const std::string &id : ids)
            {
                SetDeviceAvailability(id, false);
            }
        }
        else
        {
            CLA_LOG_INFO("Backend session established");
            /* SyncDevices runs on the next Poll and re-arms everything. */
        }
        break;

    case BackendEventKind::DeviceConnected:
        CLA_LOG_INFO("Corsair device connected: %s", event.DeviceId.c_str());
        /* Picked up by the next Poll, which also handles the first-sighting
         * case where we have no record of the device at all. */
        break;

    case BackendEventKind::DeviceDisconnected:
        CLA_LOG_INFO("Corsair device disconnected: %s", event.DeviceId.c_str());
        SetDeviceAvailability(event.DeviceId, false);
        break;
    }
}

DeviceManager::ManagedDevice *DeviceManager::FindByLampArrayIdLocked(cla_u32 lampArrayId)
{
    for (auto &entry : m_devices)
    {
        if (entry.second.LampArrayId == lampArrayId)
        {
            return &entry.second;
        }
    }
    return nullptr;
}

void DeviceManager::OnDriverUpdate(const LampArrayUpdate &update)
{
    /*
     * Runs on the DriverLink pump thread.  The backend call happens outside
     * m_mutex: an SDK write can block on IPC to iCUE, and holding the lock
     * across it would stall Poll() and every other device's frames.
     */
    std::string backendId;
    std::vector<BackendLedColor> writes;
    bool shouldWrite = false;

    {
        std::lock_guard<std::mutex> guard(m_mutex);
        ManagedDevice *device = FindByLampArrayIdLocked(update.LampArrayId);
        if (device == nullptr)
        {
            return;
        }
        backendId = device->BackendId;

        switch (update.Kind)
        {
        case ClaUpdateKindLampColors:
        {
            device->FramesReceived++;
            if (!device->BackendAvailable || device->Pipeline == nullptr)
            {
                return;   /* nothing to write to */
            }
            shouldWrite = device->Pipeline->Submit(
                update.Colors.data(),
                static_cast<cla_u32>(update.Colors.size()),
                NowMicroseconds(),
                writes);
            break;
        }

        case ClaUpdateKindAutonomousMode:
        {
            /*
             * HUT 1.4 s26.10.1: autonomous mode disabled means the host has
             * taken exclusive charge of lamp state; enabled means the device
             * may drive itself again.  Translated here into handing the
             * Corsair device back to iCUE, which is the closest analogue to
             * "the device resumes its own effect".
             */
            device->HostControlled = !update.AutonomousMode;
            if (device->Pipeline)
            {
                device->Pipeline->InvalidateShadow();
            }
            CLA_LOG_INFO("LampArray %u: Windows %s control",
                         update.LampArrayId,
                         update.AutonomousMode ? "released" : "took");
            break;
        }

        case ClaUpdateKindRemoved:
            CLA_LOG_INFO("LampArray %u removed", update.LampArrayId);
            return;

        case ClaUpdateKindNone:
        default:
            return;
        }
    }

    if (update.Kind == ClaUpdateKindAutonomousMode)
    {
        if (update.AutonomousMode)
        {
            m_backend.ReleaseControl(backendId);
        }
        else
        {
            m_backend.RequestControl(backendId, m_options.ExclusiveControl);
        }
        return;
    }

    if (!shouldWrite || writes.empty())
    {
        return;
    }

    const BackendStatus status = m_backend.SetLampColors(backendId, writes);
    if (status == BackendStatus::Ok)
    {
        return;
    }

    std::lock_guard<std::mutex> guard(m_mutex);
    auto it = m_devices.find(backendId);
    if (it == m_devices.end())
    {
        return;
    }
    it->second.WriteFailures++;

    /* The write did not land, so the pipeline's shadow no longer reflects the
     * device.  Without this, dirty-lamp tracking would suppress the retry and
     * the device would stay wrong until something else changed. */
    if (it->second.Pipeline)
    {
        it->second.Pipeline->InvalidateShadow();
    }

    /* Log the first failure and then every 600th (about ten seconds at 60Hz):
     * a disconnected device would otherwise fill the log at frame rate. */
    if (it->second.WriteFailures == 1 || (it->second.WriteFailures % 600) == 0)
    {
        CLA_LOG_WARN("Writing colours to %s failed: %s (%llu failures)",
                     backendId.c_str(), ToString(status),
                     static_cast<unsigned long long>(it->second.WriteFailures));
    }
}

size_t DeviceManager::DeviceCount() const
{
    std::lock_guard<std::mutex> guard(m_mutex);
    return m_devices.size();
}

} // namespace cla
