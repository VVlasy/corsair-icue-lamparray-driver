/*
 * Copyright (c) Corsair Dynamic Lighting Bridge contributors.
 * SPDX-License-Identifier: MIT
 *
 * ICorsairBackend implemented over the Corsair iCUE SDK.  Windows only.
 *
 * The SDK is loaded at run time from the user's own iCUE installation; this
 * project ships none of it.  See docs/corsair-sdk.md.
 */
#ifndef CLA_ICUE_SDK_BACKEND_H
#define CLA_ICUE_SDK_BACKEND_H

#include "Corsair/ICorsairBackend.h"
#include "Corsair/IcueSdkApi.h"

#include <string>
#include <vector>
#include <mutex>
#include <atomic>

namespace cla {

class IcueSdkBackend final : public ICorsairBackend
{
public:
    IcueSdkBackend();
    ~IcueSdkBackend() override;

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

    /* Where the SDK was found, for diagnostics. Empty if not loaded. */
    std::string SdkPath() const;
    /* Version reported by iCUE, or empty when not connected. */
    std::string ServerVersion() const;

    /*
     * Layer priority for this client.
     *
     * iCUE itself runs at 127 and shared SDK clients default to 128, so by
     * default this bridge already draws above iCUE's own effects without
     * taking exclusive control away from anything.  Raising it further is
     * available but not the default.
     */
    void SetLayerPriority(unsigned int priority);

    /* Candidate SDK locations, in search order.  Exposed for the probe tool
     * and so the search can be tested/reported rather than being a black box. */
    static std::vector<std::wstring> CandidateSdkPaths();

    /* Map an iCUE device type onto a LampArray kind. */
    static cla_u32 LampArrayKindForType(int corsairDeviceType);
    /* Whether a device type reports positions in mm or logical units. */
    static cla_u32 PositionUnitsForType(int corsairDeviceType);

private:
    static void OnSessionStateChangedThunk(void *context,
                                           const icue::SessionStateChanged *eventData);
    static void OnEventThunk(void *context, const icue::Event *event);

    void OnSessionStateChanged(const icue::SessionStateChanged *eventData);
    void OnEvent(const icue::Event *event);

    bool LoadSdk(std::string &error);
    void UnloadSdk();
    BackendStatus MapError(icue::Error error) const;

    mutable std::mutex m_mutex;

    void *m_module = nullptr;             /* HMODULE */
    std::string m_sdkPath;
    icue::Api m_api;

    std::atomic<bool> m_connected{false};
    std::atomic<int> m_sessionState{icue::CSS_Closed};
    std::string m_serverVersion;

    BackendEventHandler m_handler;
    unsigned int m_layerPriority = 128;

    /* Reused across writes so a 60Hz update path does not allocate. */
    std::vector<icue::LedColor> m_colorScratch;
};

} // namespace cla

#endif /* CLA_ICUE_SDK_BACKEND_H */
