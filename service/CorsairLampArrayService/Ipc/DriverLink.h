/*
 * Copyright (c) Corsair Dynamic Lighting Bridge contributors.
 * SPDX-License-Identifier: MIT
 *
 * The user-mode side of the driver interface.  Windows only.
 *
 * Locates CorsairLampArray.sys's device interface, issues the control IOCTLs,
 * and runs the inverted-call pump that receives lighting frames.
 *
 * The pump keeps several overlapped IOCTL_CLA_GET_NEXT_UPDATE requests parked
 * on the driver at once.  With only one, there would be a window after each
 * completion during which the driver has nowhere to deliver a frame and has to
 * hold it until we re-arm -- which at 60Hz is a frame's worth of latency on
 * every single update.
 */
#ifndef CLA_DRIVER_LINK_H
#define CLA_DRIVER_LINK_H

#include "cla/IpcContract.h"

#include <windows.h>

#include <string>
#include <vector>
#include <functional>
#include <thread>
#include <atomic>

namespace cla {

struct LampArrayCreateParameters
{
    std::wstring DeviceName;
    std::wstring SerialNumber;
    cla_u16 VendorId = 0;
    cla_u16 ProductId = 0;
    cla_u16 VersionNumber = 0;
    ClaLampArrayAttributes Attributes{};
    std::vector<ClaLampInfo> Lamps;
};

/* Delivered for each frame or state change the driver publishes. */
struct LampArrayUpdate
{
    cla_u32 LampArrayId = 0;
    ClaUpdateKind Kind = ClaUpdateKindNone;
    cla_u64 FrameSequence = 0;
    bool AutonomousMode = true;
    std::vector<ClaLampColor> Colors;
};

using UpdateHandler = std::function<void(const LampArrayUpdate &)>;

class DriverLink
{
public:
    DriverLink();
    ~DriverLink();

    /* Opens the driver's device interface.  Returns false with a reason if the
     * driver is not installed or not started. */
    bool Open(std::string &error);
    void Close();
    bool IsOpen() const { return m_device != INVALID_HANDLE_VALUE; }

    bool GetDriverInfo(ClaDriverInfo &info, std::string &error);

    /* Creates a virtual LampArray and returns its id. */
    bool CreateLampArray(const LampArrayCreateParameters &parameters,
                         cla_u32 &lampArrayId,
                         std::string &error);

    bool DestroyLampArray(cla_u32 lampArrayId, std::string &error);
    bool SetDeviceState(cla_u32 lampArrayId, ClaDeviceState state, std::string &error);
    bool SetBrightness(cla_u32 lampArrayId, cla_u8 brightness, std::string &error);
    bool GetStatistics(cla_u32 lampArrayId, ClaStatisticsResponse &statistics, std::string &error);
    bool GetLampCount(cla_u32 lampArrayId, cla_u32 &lampCount, std::string &error);
    bool GetLampInfo(cla_u32 lampArrayId, cla_u32 lampId, ClaLampInfo &info, std::string &error);

    /* Starts the pump.  The handler is invoked on the pump thread. */
    bool StartUpdatePump(UpdateHandler handler, std::string &error);
    void StopUpdatePump();

    /* Human-readable path of the opened interface, for diagnostics. */
    const std::string &InterfacePath() const { return m_interfacePath; }

private:
    static constexpr int kOutstandingRequests = 4;
    /* Enough for a maximum-size frame. */
    static constexpr size_t kUpdateBufferBytes =
        sizeof(ClaUpdateMessage) + CLA_MAX_LAMPS_PER_ARRAY * sizeof(ClaLampColor);

    struct PendingRequest
    {
        OVERLAPPED Overlapped{};
        std::vector<unsigned char> Buffer;
        bool InFlight = false;
    };

    bool Control(cla_u32 code,
                 const void *input, cla_u32 inputSize,
                 void *output, cla_u32 outputSize,
                 cla_u32 *bytesReturned,
                 std::string &error);

    bool IssueRequest(PendingRequest &request, std::string &error);
    void PumpThread(UpdateHandler handler);

    static std::string FormatLastError(const char *what, DWORD code);

    HANDLE m_device = INVALID_HANDLE_VALUE;
    std::string m_interfacePath;

    std::thread m_pumpThread;
    std::atomic<bool> m_pumpRunning{false};
    HANDLE m_stopEvent = nullptr;
};

} // namespace cla

#endif /* CLA_DRIVER_LINK_H */
