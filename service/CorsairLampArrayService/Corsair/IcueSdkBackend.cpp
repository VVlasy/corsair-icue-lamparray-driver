/*
 * Copyright (c) Corsair Dynamic Lighting Bridge contributors.
 * SPDX-License-Identifier: MIT
 *
 * ICorsairBackend over the Corsair iCUE SDK.  Windows only.
 */

#include "Corsair/IcueSdkBackend.h"
#include "Logging/Logger.h"

#include <windows.h>

#include <cstring>

namespace cla {

namespace {

/*
 * The SDK DLL name is architecture- and toolset-stamped by Corsair.
 * "x64_2019" refers to the MSVC toolset it was built with, not the OS.
 */
#if defined(_M_ARM64)
const wchar_t *kSdkFileNames[] = { L"iCUESDK.arm64_2019.dll", L"iCUESDK.x64_2019.dll" };
#elif defined(_M_X64)
const wchar_t *kSdkFileNames[] = { L"iCUESDK.x64_2019.dll", L"iCUESDK_2019.dll" };
#else
const wchar_t *kSdkFileNames[] = { L"iCUESDK_2019.dll" };
#endif

std::wstring EnvironmentVariable(const wchar_t *name)
{
    wchar_t buffer[MAX_PATH];
    const DWORD length = GetEnvironmentVariableW(name, buffer, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
    {
        return std::wstring();
    }
    return std::wstring(buffer, length);
}

std::string Narrow(const std::wstring &wide)
{
    if (wide.empty())
    {
        return std::string();
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(),
                                         static_cast<int>(wide.size()),
                                         nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()),
                        result.data(), size, nullptr, nullptr);
    return result;
}

} // namespace

std::vector<std::wstring> IcueSdkBackend::CandidateSdkPaths()
{
    std::vector<std::wstring> candidates;

    /*
     * Search order, most specific first:
     *
     *  1. CORSAIR_ICUE_SDK_PATH, so an administrator or a developer can point
     *     the service at a specific SDK without touching the code.
     *  2. Beside our own executable, for a self-contained deployment where the
     *     user has placed the SDK there themselves.
     *  3. The standard iCUE installation directories.
     *  4. Bare filename, letting the loader use its normal search path.
     *
     * The SDK is never shipped by this project, so every one of these is a
     * location belonging to the user's own iCUE installation.
     */
    const std::wstring override = EnvironmentVariable(L"CORSAIR_ICUE_SDK_PATH");
    if (!override.empty())
    {
        for (const wchar_t *name : kSdkFileNames)
        {
            candidates.push_back(override + L"\\" + name);
        }
        /* Allow the variable to name the DLL itself, not just its folder. */
        candidates.push_back(override);
    }

    wchar_t modulePath[MAX_PATH];
    const DWORD moduleLength = GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
    if (moduleLength > 0 && moduleLength < MAX_PATH)
    {
        std::wstring directory(modulePath, moduleLength);
        const size_t slash = directory.find_last_of(L'\\');
        if (slash != std::wstring::npos)
        {
            directory.resize(slash);
            for (const wchar_t *name : kSdkFileNames)
            {
                candidates.push_back(directory + L"\\" + name);
            }
        }
    }

    const std::wstring programFiles = EnvironmentVariable(L"ProgramFiles");
    const std::wstring programFilesX86 = EnvironmentVariable(L"ProgramFiles(x86)");
    const wchar_t *kInstallSubdirectories[] = {
        L"\\Corsair\\CORSAIR iCUE 5 Software",
        L"\\Corsair\\CORSAIR iCUE 4 Software",
        L"\\Corsair\\CORSAIR iCUE Software",
    };

    for (const std::wstring &root : { programFiles, programFilesX86 })
    {
        if (root.empty())
        {
            continue;
        }
        for (const wchar_t *subdirectory : kInstallSubdirectories)
        {
            for (const wchar_t *name : kSdkFileNames)
            {
                candidates.push_back(root + subdirectory + L"\\" + name);
            }
        }
    }

    for (const wchar_t *name : kSdkFileNames)
    {
        candidates.push_back(name);
    }

    return candidates;
}

IcueSdkBackend::IcueSdkBackend() = default;

IcueSdkBackend::~IcueSdkBackend()
{
    Shutdown();
}

bool IcueSdkBackend::LoadSdk(std::string &error)
{
    error.clear();

    for (const std::wstring &candidate : CandidateSdkPaths())
    {
        HMODULE module = LoadLibraryExW(candidate.c_str(), nullptr,
                                        LOAD_LIBRARY_SEARCH_DEFAULT_DIRS |
                                        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR);
        if (module == nullptr)
        {
            /* Fall back to a plain load for the bare-filename candidates,
             * which the altered search path above cannot resolve. */
            module = LoadLibraryW(candidate.c_str());
        }
        if (module == nullptr)
        {
            continue;
        }

        m_module = module;
        m_sdkPath = Narrow(candidate);

        auto resolve = [module](const char *name) -> FARPROC {
            return GetProcAddress(module, name);
        };

        m_api.Connect = reinterpret_cast<decltype(m_api.Connect)>(resolve("CorsairConnect"));
        m_api.GetSessionDetails = reinterpret_cast<decltype(m_api.GetSessionDetails)>(resolve("CorsairGetSessionDetails"));
        m_api.Disconnect = reinterpret_cast<decltype(m_api.Disconnect)>(resolve("CorsairDisconnect"));
        m_api.GetDevices = reinterpret_cast<decltype(m_api.GetDevices)>(resolve("CorsairGetDevices"));
        m_api.GetDeviceInfo = reinterpret_cast<decltype(m_api.GetDeviceInfo)>(resolve("CorsairGetDeviceInfo"));
        m_api.GetLedPositions = reinterpret_cast<decltype(m_api.GetLedPositions)>(resolve("CorsairGetLedPositions"));
        m_api.SubscribeForEvents = reinterpret_cast<decltype(m_api.SubscribeForEvents)>(resolve("CorsairSubscribeForEvents"));
        m_api.UnsubscribeFromEvents = reinterpret_cast<decltype(m_api.UnsubscribeFromEvents)>(resolve("CorsairUnsubscribeFromEvents"));
        m_api.SetLedColors = reinterpret_cast<decltype(m_api.SetLedColors)>(resolve("CorsairSetLedColors"));
        m_api.SetLedColorsBuffer = reinterpret_cast<decltype(m_api.SetLedColorsBuffer)>(resolve("CorsairSetLedColorsBuffer"));
        m_api.SetLedColorsFlushBufferAsync = reinterpret_cast<decltype(m_api.SetLedColorsFlushBufferAsync)>(resolve("CorsairSetLedColorsFlushBufferAsync"));
        m_api.SetLayerPriority = reinterpret_cast<decltype(m_api.SetLayerPriority)>(resolve("CorsairSetLayerPriority"));
        m_api.RequestControl = reinterpret_cast<decltype(m_api.RequestControl)>(resolve("CorsairRequestControl"));
        m_api.ReleaseControl = reinterpret_cast<decltype(m_api.ReleaseControl)>(resolve("CorsairReleaseControl"));

        /* Only these are indispensable.  The rest are optional refinements and
         * a missing one degrades behaviour rather than preventing operation --
         * an older SDK should still light up a keyboard. */
        if (m_api.Connect == nullptr || m_api.GetDevices == nullptr ||
            m_api.GetLedPositions == nullptr || m_api.SetLedColors == nullptr)
        {
            CLA_LOG_WARN("iCUE SDK at %s is missing required exports; ignoring it",
                         m_sdkPath.c_str());
            UnloadSdk();
            continue;
        }

        CLA_LOG_INFO("Loaded iCUE SDK from %s", m_sdkPath.c_str());
        return true;
    }

    error = "iCUE SDK not found. Install Corsair iCUE, or set CORSAIR_ICUE_SDK_PATH "
            "to the folder containing iCUESDK.x64_2019.dll.";
    return false;
}

void IcueSdkBackend::UnloadSdk()
{
    if (m_module != nullptr)
    {
        FreeLibrary(static_cast<HMODULE>(m_module));
        m_module = nullptr;
    }
    m_api = icue::Api{};
    m_sdkPath.clear();
}

BackendStatus IcueSdkBackend::MapError(icue::Error error) const
{
    switch (error)
    {
    case icue::CE_Success:              return BackendStatus::Ok;
    case icue::CE_NotConnected:         return BackendStatus::NotConnected;
    case icue::CE_NoControl:            return BackendStatus::NoControl;
    case icue::CE_DeviceNotFound:       return BackendStatus::DeviceNotFound;
    case icue::CE_InvalidArguments:     return BackendStatus::InvalidArgument;
    case icue::CE_IncompatibleProtocol: return BackendStatus::NotSupported;
    case icue::CE_NotAllowed:           return BackendStatus::NotSupported;
    case icue::CE_InvalidOperation:     return BackendStatus::Failed;
    default:                            return BackendStatus::Failed;
    }
}

cla_u32 IcueSdkBackend::LampArrayKindForType(int corsairDeviceType)
{
    /*
     * Map iCUE's device taxonomy onto the LampArrayKind values from HUT 1.4
     * table 26.2.1.  Windows uses the kind to decide which effects suit a
     * device, so a mouse must not claim to be a keyboard.
     */
    switch (corsairDeviceType)
    {
    case icue::CDT_Keyboard:         return ClaLampArrayKindKeyboard;
    case icue::CDT_Mouse:            return ClaLampArrayKindMouse;
    case icue::CDT_GameController:   return ClaLampArrayKindGameController;

    /* Mousemats, headset stands and touchbars are accessories the user looks
     * at but does not wear: "Peripheral" per the spec's own examples
     * ("speakers, mousepad, microphone, webcam"). */
    case icue::CDT_Mousemat:
    case icue::CDT_HeadsetStand:
    case icue::CDT_Touchbar:         return ClaLampArrayKindPeripheral;

    /* "LampArray is embedded in a wearable accessory (audio-headset, ...)" */
    case icue::CDT_Headset:          return ClaLampArrayKindWearable;

    /* Internal PC components: the spec names RAM sticks, motherboards and fans
     * explicitly under Chassis. */
    case icue::CDT_FanLedController:
    case icue::CDT_LedController:
    case icue::CDT_MemoryModule:
    case icue::CDT_Cooler:
    case icue::CDT_Motherboard:
    case icue::CDT_GraphicsCard:     return ClaLampArrayKindChassis;

    default:                         return ClaLampArrayKindUndefined;
    }
}

cla_u32 IcueSdkBackend::PositionUnitsForType(int corsairDeviceType)
{
    /*
     * The SDK documents CorsairLedPosition::cx/cy as: "for keyboards, mice,
     * mousemats, headset stands and memory modules values are in mm, for
     * DIY-devices, headsets and coolers values are in logical units".
     *
     * Getting this wrong does not fail; it silently produces a bounding box
     * off by orders of magnitude, and Windows then places effects on a device
     * it believes is either microscopic or metres wide.
     */
    switch (corsairDeviceType)
    {
    case icue::CDT_Keyboard:
    case icue::CDT_Mouse:
    case icue::CDT_Mousemat:
    case icue::CDT_HeadsetStand:
    case icue::CDT_MemoryModule:
        return ClaSourceUnitsMillimeters;
    default:
        return ClaSourceUnitsLogical;
    }
}

void IcueSdkBackend::OnSessionStateChangedThunk(void *context,
                                                const icue::SessionStateChanged *eventData)
{
    if (context != nullptr)
    {
        static_cast<IcueSdkBackend *>(context)->OnSessionStateChanged(eventData);
    }
}

void IcueSdkBackend::OnEventThunk(void *context, const icue::Event *event)
{
    if (context != nullptr)
    {
        static_cast<IcueSdkBackend *>(context)->OnEvent(event);
    }
}

void IcueSdkBackend::OnSessionStateChanged(const icue::SessionStateChanged *eventData)
{
    if (eventData == nullptr)
    {
        return;
    }

    const bool connected = (eventData->state == icue::CSS_Connected);
    m_sessionState.store(eventData->state);
    m_connected.store(connected);

    BackendEventHandler handler;
    {
        std::lock_guard<std::mutex> guard(m_mutex);
        char version[64];
        std::snprintf(version, sizeof(version), "%d.%d.%d",
                      eventData->details.serverHostVersion.major,
                      eventData->details.serverHostVersion.minor,
                      eventData->details.serverHostVersion.patch);
        m_serverVersion = version;
        handler = m_handler;
    }

    CLA_LOG_INFO("iCUE session state: %s", icue::ToString(eventData->state));

    if (connected && m_api.SetLayerPriority != nullptr)
    {
        /* Re-apply on every (re)connect: a new session does not inherit the
         * priority from the previous one. */
        m_api.SetLayerPriority(m_layerPriority);
    }

    if (handler)
    {
        BackendEvent event;
        event.Kind = BackendEventKind::SessionStateChanged;
        event.SessionConnected = connected;
        handler(event);
    }
}

void IcueSdkBackend::OnEvent(const icue::Event *event)
{
    if (event == nullptr)
    {
        return;
    }
    if (event->id != icue::CEI_DeviceConnectionStatusChangedEvent ||
        event->deviceConnectionStatusChangedEvent == nullptr)
    {
        return;   /* key events are not our concern */
    }

    BackendEventHandler handler;
    {
        std::lock_guard<std::mutex> guard(m_mutex);
        handler = m_handler;
    }
    if (!handler)
    {
        return;
    }

    const auto *data = event->deviceConnectionStatusChangedEvent;

    BackendEvent outgoing;
    outgoing.Kind = data->isConnected ? BackendEventKind::DeviceConnected
                                      : BackendEventKind::DeviceDisconnected;
    /* The id field is a fixed 128-byte array; bound the read in case it is not
     * terminated. */
    outgoing.DeviceId.assign(data->deviceId,
                             strnlen(data->deviceId, icue::kStringSizeM));
    handler(outgoing);
}

BackendStatus IcueSdkBackend::Initialize()
{
    std::lock_guard<std::mutex> guard(m_mutex);

    if (m_module == nullptr)
    {
        std::string error;
        if (!LoadSdk(error))
        {
            CLA_LOG_WARN("%s", error.c_str());
            /*
             * Not an error the service should die on.  iCUE may simply not be
             * installed yet; the caller retries.  Project Phase 14 requires
             * surviving a missing SDK DLL.
             */
            return BackendStatus::NotConnected;
        }
    }

    const icue::Error error = m_api.Connect(&IcueSdkBackend::OnSessionStateChangedThunk, this);
    if (error != icue::CE_Success)
    {
        CLA_LOG_WARN("CorsairConnect failed: %s", icue::ToString(error));
        return MapError(error);
    }

    if (m_api.SubscribeForEvents != nullptr)
    {
        m_api.SubscribeForEvents(&IcueSdkBackend::OnEventThunk, this);
    }

    /* Connection completes asynchronously; the session-state callback reports
     * the outcome. */
    return BackendStatus::Ok;
}

void IcueSdkBackend::Shutdown()
{
    std::lock_guard<std::mutex> guard(m_mutex);

    if (m_module == nullptr)
    {
        return;
    }
    if (m_api.UnsubscribeFromEvents != nullptr)
    {
        m_api.UnsubscribeFromEvents();
    }
    if (m_api.Disconnect != nullptr)
    {
        m_api.Disconnect();
    }
    m_connected.store(false);

    /*
     * The module is deliberately NOT unloaded here.  The SDK may still have
     * threads running our callback thunks, and unloading underneath them would
     * be a use-after-free of code.  Leaving it mapped for the life of the
     * process costs a few hundred KB and removes the race entirely.
     */
}

bool IcueSdkBackend::IsConnected() const
{
    return m_connected.load();
}

std::string IcueSdkBackend::SdkPath() const
{
    std::lock_guard<std::mutex> guard(m_mutex);
    return m_sdkPath;
}

std::string IcueSdkBackend::ServerVersion() const
{
    std::lock_guard<std::mutex> guard(m_mutex);
    return m_serverVersion;
}

void IcueSdkBackend::SetLayerPriority(unsigned int priority)
{
    if (priority > icue::kLayerPriorityMax)
    {
        priority = icue::kLayerPriorityMax;
    }
    std::lock_guard<std::mutex> guard(m_mutex);
    m_layerPriority = priority;
    if (m_api.SetLayerPriority != nullptr && m_connected.load())
    {
        m_api.SetLayerPriority(priority);
    }
}

BackendStatus IcueSdkBackend::EnumerateDevices(std::vector<BackendDeviceInfo> &devices)
{
    devices.clear();

    std::lock_guard<std::mutex> guard(m_mutex);
    if (m_api.GetDevices == nullptr)
    {
        return BackendStatus::NotConnected;
    }

    icue::DeviceFilter filter{};
    filter.deviceTypeMask = icue::CDT_All;

    std::vector<icue::DeviceInfo> raw(icue::kDeviceCountMax);
    int count = 0;
    const icue::Error error = m_api.GetDevices(&filter,
                                               static_cast<int>(raw.size()),
                                               raw.data(), &count);
    if (error != icue::CE_Success)
    {
        return MapError(error);
    }

    /* Trust nothing the SDK reports about counts. */
    if (count < 0)
    {
        count = 0;
    }
    if (static_cast<size_t>(count) > raw.size())
    {
        count = static_cast<int>(raw.size());
    }

    for (int i = 0; i < count; ++i)
    {
        const icue::DeviceInfo &source = raw[static_cast<size_t>(i)];

        BackendDeviceInfo info;
        info.Id.assign(source.id, strnlen(source.id, icue::kStringSizeM));
        info.Model.assign(source.model, strnlen(source.model, icue::kStringSizeM));
        info.Serial.assign(source.serial, strnlen(source.serial, icue::kStringSizeM));
        info.LampArrayKind = LampArrayKindForType(source.type);
        info.PositionUnits = PositionUnitsForType(source.type);
        info.LedCount = (source.ledCount > 0) ? static_cast<cla_u32>(source.ledCount) : 0;
        info.ChannelCount = (source.channelCount > 0)
                                ? static_cast<cla_u32>(source.channelCount) : 0;

        if (info.Id.empty())
        {
            CLA_LOG_WARN("Skipping Corsair device with empty id (model '%s')",
                         info.Model.c_str());
            continue;
        }
        devices.push_back(std::move(info));
    }

    return BackendStatus::Ok;
}

BackendStatus IcueSdkBackend::GetDeviceInfo(const std::string &deviceId,
                                            BackendDeviceInfo &info)
{
    std::lock_guard<std::mutex> guard(m_mutex);
    if (m_api.GetDeviceInfo == nullptr)
    {
        return BackendStatus::NotConnected;
    }

    icue::DeviceInfo raw{};
    const icue::Error error = m_api.GetDeviceInfo(deviceId.c_str(), &raw);
    if (error != icue::CE_Success)
    {
        return MapError(error);
    }

    info.Id.assign(raw.id, strnlen(raw.id, icue::kStringSizeM));
    info.Model.assign(raw.model, strnlen(raw.model, icue::kStringSizeM));
    info.Serial.assign(raw.serial, strnlen(raw.serial, icue::kStringSizeM));
    info.LampArrayKind = LampArrayKindForType(raw.type);
    info.PositionUnits = PositionUnitsForType(raw.type);
    info.LedCount = (raw.ledCount > 0) ? static_cast<cla_u32>(raw.ledCount) : 0;
    info.ChannelCount = (raw.channelCount > 0) ? static_cast<cla_u32>(raw.channelCount) : 0;
    return BackendStatus::Ok;
}

BackendStatus IcueSdkBackend::GetLamps(const std::string &deviceId,
                                       std::vector<ClaSourceLed> &leds)
{
    leds.clear();

    std::lock_guard<std::mutex> guard(m_mutex);
    if (m_api.GetLedPositions == nullptr)
    {
        return BackendStatus::NotConnected;
    }

    std::vector<icue::LedPosition> raw(icue::kDeviceLedCountMax);
    int count = 0;
    const icue::Error error = m_api.GetLedPositions(deviceId.c_str(),
                                                    static_cast<int>(raw.size()),
                                                    raw.data(), &count);
    if (error != icue::CE_Success)
    {
        return MapError(error);
    }

    if (count < 0)
    {
        count = 0;
    }
    if (static_cast<size_t>(count) > raw.size())
    {
        count = static_cast<int>(raw.size());
    }

    leds.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i)
    {
        ClaSourceLed led{};
        led.LedId = raw[static_cast<size_t>(i)].id;
        led.X = raw[static_cast<size_t>(i)].cx;
        led.Y = raw[static_cast<size_t>(i)].cy;
        leds.push_back(led);
    }
    return BackendStatus::Ok;
}

BackendStatus IcueSdkBackend::SetLampColors(const std::string &deviceId,
                                            const std::vector<BackendLedColor> &colors)
{
    if (colors.empty())
    {
        return BackendStatus::Ok;
    }

    std::lock_guard<std::mutex> guard(m_mutex);
    if (m_api.SetLedColors == nullptr)
    {
        return BackendStatus::NotConnected;
    }

    m_colorScratch.clear();
    m_colorScratch.reserve(colors.size());
    for (const BackendLedColor &color : colors)
    {
        icue::LedColor entry{};
        entry.id = color.LedId;
        entry.r = color.Red;
        entry.g = color.Green;
        entry.b = color.Blue;
        entry.a = color.Alpha;
        m_colorScratch.push_back(entry);
    }

    /*
     * Prefer the buffered path: SetLedColorsBuffer stages the write and
     * FlushBufferAsync returns immediately, so a 60Hz update loop is not
     * serialised behind the SDK's IPC round trip.  CorsairSetLedColors is
     * synchronous and would put that latency directly in the frame path.
     *
     * If either buffered entry point is missing (older SDK), fall back to the
     * synchronous call rather than failing.
     */
    if (m_api.SetLedColorsBuffer != nullptr && m_api.SetLedColorsFlushBufferAsync != nullptr)
    {
        icue::Error error = m_api.SetLedColorsBuffer(deviceId.c_str(),
                                                     static_cast<int>(m_colorScratch.size()),
                                                     m_colorScratch.data());
        if (error != icue::CE_Success)
        {
            return MapError(error);
        }
        /* No completion callback: the next frame supersedes this one anyway,
         * and waiting would reintroduce the latency the buffered path exists
         * to avoid. */
        error = m_api.SetLedColorsFlushBufferAsync(nullptr, nullptr);
        return MapError(error);
    }

    const icue::Error error = m_api.SetLedColors(deviceId.c_str(),
                                                 static_cast<int>(m_colorScratch.size()),
                                                 m_colorScratch.data());
    return MapError(error);
}

BackendStatus IcueSdkBackend::SetBrightness(const std::string &deviceId, cla_u8 brightness)
{
    /*
     * The SDK has no device brightness control; brightness is applied by
     * FramePipeline when it scales colours.  Reported as supported so callers
     * do not treat it as a failure.
     */
    (void)deviceId;
    (void)brightness;
    return BackendStatus::Ok;
}

BackendStatus IcueSdkBackend::RequestControl(const std::string &deviceId, bool exclusive)
{
    std::lock_guard<std::mutex> guard(m_mutex);
    if (m_api.RequestControl == nullptr)
    {
        return BackendStatus::NotSupported;
    }

    /*
     * Shared is the default and the right default.  In shared mode iCUE keeps
     * running the user's own profiles and our output is layered above them by
     * priority; taking exclusive control would silently disable the user's
     * iCUE effects for as long as this service runs, which is not a decision
     * a background bridge should make for them.
     */
    const icue::AccessLevel level = exclusive ? icue::CAL_ExclusiveLightingControl
                                              : icue::CAL_Shared;
    const icue::Error error = m_api.RequestControl(deviceId.c_str(), level);
    if (error != icue::CE_Success)
    {
        CLA_LOG_WARN("RequestControl(%s, %s) failed: %s", deviceId.c_str(),
                     exclusive ? "exclusive" : "shared", icue::ToString(error));
    }
    return MapError(error);
}

BackendStatus IcueSdkBackend::ReleaseControl(const std::string &deviceId)
{
    std::lock_guard<std::mutex> guard(m_mutex);
    if (m_api.ReleaseControl == nullptr)
    {
        return BackendStatus::NotSupported;
    }
    return MapError(m_api.ReleaseControl(deviceId.c_str()));
}

void IcueSdkBackend::SubscribeEvents(BackendEventHandler handler)
{
    std::lock_guard<std::mutex> guard(m_mutex);
    m_handler = std::move(handler);
}

} // namespace cla
