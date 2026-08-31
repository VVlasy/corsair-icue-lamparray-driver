/*
 * Copyright (c) Corsair Dynamic Lighting Bridge contributors.
 * SPDX-License-Identifier: MIT
 */

#include "Ipc/DriverLink.h"
#include "Logging/Logger.h"

/* Instantiates GUID_DEVINTERFACE_CORSAIR_LAMP_ARRAY for every binary that
 * links this file (the service, HidTestClient).  Must precede the IPC header,
 * and appear in exactly one translation unit -- see inc/CorsairLampArrayIpc.h. */
#include <initguid.h>
#include "CorsairLampArrayIpc.h"

#include <setupapi.h>
#include <cfgmgr32.h>

#include <cstring>

namespace cla {

namespace {

/* Copy a std::wstring into a fixed cla_u16 array, always NUL-terminating.
 * The driver rejects an unterminated string, so truncation must still leave
 * room for the terminator. */
void CopyToFixedString(const std::wstring &source, cla_u16 *destination, size_t capacity)
{
    const size_t limit = (capacity == 0) ? 0 : capacity - 1;
    size_t i = 0;
    for (; i < limit && i < source.size(); ++i)
    {
        destination[i] = static_cast<cla_u16>(source[i]);
    }
    for (; i < capacity; ++i)
    {
        destination[i] = 0;
    }
}

} // namespace

DriverLink::DriverLink() = default;

DriverLink::~DriverLink()
{
    StopUpdatePump();
    Close();
}

std::string DriverLink::FormatLastError(const char *what, DWORD code)
{
    char *buffer = nullptr;
    FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                       FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, code, 0, reinterpret_cast<char *>(&buffer), 0, nullptr);

    std::string message = what;
    message += " failed (";
    message += std::to_string(code);
    message += ")";
    if (buffer != nullptr)
    {
        message += ": ";
        message += buffer;
        while (!message.empty() && (message.back() == '\n' || message.back() == '\r'))
        {
            message.pop_back();
        }
        LocalFree(buffer);
    }
    return message;
}

bool DriverLink::Open(std::string &error)
{
    error.clear();
    if (IsOpen())
    {
        return true;
    }

    HDEVINFO deviceInfo = SetupDiGetClassDevsW(&GUID_DEVINTERFACE_CORSAIR_LAMP_ARRAY,
                                               nullptr, nullptr,
                                               DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (deviceInfo == INVALID_HANDLE_VALUE)
    {
        error = FormatLastError("SetupDiGetClassDevs", GetLastError());
        return false;
    }

    bool opened = false;
    SP_DEVICE_INTERFACE_DATA interfaceData{};
    interfaceData.cbSize = sizeof(interfaceData);

    for (DWORD index = 0;
         SetupDiEnumDeviceInterfaces(deviceInfo, nullptr,
                                     &GUID_DEVINTERFACE_CORSAIR_LAMP_ARRAY,
                                     index, &interfaceData);
         ++index)
    {
        DWORD required = 0;
        SetupDiGetDeviceInterfaceDetailW(deviceInfo, &interfaceData, nullptr, 0,
                                         &required, nullptr);
        if (required == 0)
        {
            continue;
        }

        std::vector<unsigned char> storage(required);
        auto *detail = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(storage.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

        if (!SetupDiGetDeviceInterfaceDetailW(deviceInfo, &interfaceData, detail,
                                              required, nullptr, nullptr))
        {
            continue;
        }

        /* FILE_FLAG_OVERLAPPED: the update pump needs asynchronous I/O so the
         * parked inverted-call requests do not block the thread. */
        HANDLE handle = CreateFileW(detail->DevicePath,
                                    GENERIC_READ | GENERIC_WRITE,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
                                    nullptr);
        if (handle == INVALID_HANDLE_VALUE)
        {
            const DWORD code = GetLastError();
            if (code == ERROR_ACCESS_DENIED)
            {
                error = "Access denied opening the CorsairLampArray device. The "
                        "device interface is restricted to SYSTEM and Administrators; "
                        "run the service as LocalSystem or the tool elevated.";
            }
            else
            {
                error = FormatLastError("CreateFile on device interface", code);
            }
            continue;
        }

        m_device = handle;
        int length = WideCharToMultiByte(CP_UTF8, 0, detail->DevicePath, -1,
                                         nullptr, 0, nullptr, nullptr);
        if (length > 0)
        {
            m_interfacePath.resize(static_cast<size_t>(length) - 1);
            WideCharToMultiByte(CP_UTF8, 0, detail->DevicePath, -1,
                                m_interfacePath.data(), length, nullptr, nullptr);
        }
        opened = true;
        break;
    }

    SetupDiDestroyDeviceInfoList(deviceInfo);

    if (!opened && error.empty())
    {
        error = "CorsairLampArray driver not found. Install the driver package "
                "(scripts/InstallDriver.ps1) and confirm the device is present "
                "in Device Manager.";
    }
    return opened;
}

void DriverLink::Close()
{
    if (m_device != INVALID_HANDLE_VALUE)
    {
        CloseHandle(m_device);
        m_device = INVALID_HANDLE_VALUE;
    }
    m_interfacePath.clear();
}

bool DriverLink::Control(cla_u32 code,
                         const void *input, cla_u32 inputSize,
                         void *output, cla_u32 outputSize,
                         cla_u32 *bytesReturned,
                         std::string &error)
{
    error.clear();
    if (!IsOpen())
    {
        error = "driver not open";
        return false;
    }

    /* The handle is opened overlapped, so even these synchronous control calls
     * must supply an OVERLAPPED and wait for completion. */
    OVERLAPPED overlapped{};
    overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (overlapped.hEvent == nullptr)
    {
        error = FormatLastError("CreateEvent", GetLastError());
        return false;
    }

    DWORD returned = 0;
    BOOL ok = DeviceIoControl(m_device, code,
                              const_cast<void *>(input), inputSize,
                              output, outputSize, &returned, &overlapped);
    if (!ok && GetLastError() == ERROR_IO_PENDING)
    {
        ok = GetOverlappedResult(m_device, &overlapped, &returned, TRUE);
    }

    const DWORD lastError = ok ? ERROR_SUCCESS : GetLastError();
    CloseHandle(overlapped.hEvent);

    if (!ok)
    {
        error = FormatLastError("DeviceIoControl", lastError);
        return false;
    }
    if (bytesReturned != nullptr)
    {
        *bytesReturned = returned;
    }
    return true;
}

bool DriverLink::GetDriverInfo(ClaDriverInfo &info, std::string &error)
{
    std::memset(&info, 0, sizeof(info));
    return Control(IOCTL_CLA_GET_DRIVER_INFO, nullptr, 0,
                   &info, sizeof(info), nullptr, error);
}

bool DriverLink::CreateLampArray(const LampArrayCreateParameters &parameters,
                                 cla_u32 &lampArrayId,
                                 std::string &error)
{
    lampArrayId = 0;

    const cla_u32 lampCount = static_cast<cla_u32>(parameters.Lamps.size());
    const cla_u32 size = ClaCreateRequestSizeForLampCount(lampCount);
    if (size == 0)
    {
        error = "lamp count " + std::to_string(lampCount) + " is out of range (1.." +
                std::to_string(CLA_MAX_LAMPS_PER_ARRAY) + ")";
        return false;
    }

    std::vector<unsigned char> buffer(size, 0);
    auto *request = reinterpret_cast<ClaLampArrayCreateRequest *>(buffer.data());

    request->Header.Version = CLA_IPC_PROTOCOL_VERSION;
    request->Header.Size = size;
    request->LampCount = lampCount;
    request->BoundingBoxWidthInMicrometers = parameters.Attributes.BoundingBoxWidthInMicrometers;
    request->BoundingBoxHeightInMicrometers = parameters.Attributes.BoundingBoxHeightInMicrometers;
    request->BoundingBoxDepthInMicrometers = parameters.Attributes.BoundingBoxDepthInMicrometers;
    request->LampArrayKind = parameters.Attributes.LampArrayKind;
    request->MinUpdateIntervalInMicroseconds = parameters.Attributes.MinUpdateIntervalInMicroseconds;
    request->VendorId = parameters.VendorId;
    request->ProductId = parameters.ProductId;
    request->VersionNumber = parameters.VersionNumber;

    CopyToFixedString(parameters.DeviceName, request->DeviceName, CLA_MAX_NAME_CHARS);
    CopyToFixedString(parameters.SerialNumber, request->SerialNumber, CLA_MAX_SERIAL_CHARS);

    std::memcpy(request->Lamps, parameters.Lamps.data(),
                static_cast<size_t>(lampCount) * sizeof(ClaLampInfo));

    /* Validate locally before sending.  The driver will reject a malformed
     * request anyway, but failing here produces a precise message instead of a
     * bare STATUS_INVALID_PARAMETER. */
    if (ClaValidateCreateRequest(buffer.data(), size) != ClaStatusOk)
    {
        error = "create request failed local validation; the mapped lamp table "
                "is inconsistent (check positions against the bounding box)";
        return false;
    }

    ClaLampArrayCreateResponse response{};
    if (!Control(IOCTL_CLA_CREATE_LAMP_ARRAY, buffer.data(), size,
                 &response, sizeof(response), nullptr, error))
    {
        return false;
    }

    lampArrayId = response.LampArrayId;
    return true;
}

bool DriverLink::DestroyLampArray(cla_u32 lampArrayId, std::string &error)
{
    ClaLampArrayIdRequest request{};
    request.Header.Version = CLA_IPC_PROTOCOL_VERSION;
    request.Header.Size = sizeof(request);
    request.LampArrayId = lampArrayId;
    return Control(IOCTL_CLA_DESTROY_LAMP_ARRAY, &request, sizeof(request),
                   nullptr, 0, nullptr, error);
}

bool DriverLink::SetDeviceState(cla_u32 lampArrayId, ClaDeviceState state, std::string &error)
{
    ClaSetDeviceStateRequest request{};
    request.Header.Version = CLA_IPC_PROTOCOL_VERSION;
    request.Header.Size = sizeof(request);
    request.LampArrayId = lampArrayId;
    request.State = static_cast<cla_u32>(state);
    return Control(IOCTL_CLA_SET_DEVICE_STATE, &request, sizeof(request),
                   nullptr, 0, nullptr, error);
}

bool DriverLink::SetBrightness(cla_u32 lampArrayId, cla_u8 brightness, std::string &error)
{
    ClaSetBrightnessRequest request{};
    request.Header.Version = CLA_IPC_PROTOCOL_VERSION;
    request.Header.Size = sizeof(request);
    request.LampArrayId = lampArrayId;
    request.Brightness = brightness;
    return Control(IOCTL_CLA_SET_BRIGHTNESS, &request, sizeof(request),
                   nullptr, 0, nullptr, error);
}

bool DriverLink::GetStatistics(cla_u32 lampArrayId,
                               ClaStatisticsResponse &statistics,
                               std::string &error)
{
    ClaLampArrayIdRequest request{};
    request.Header.Version = CLA_IPC_PROTOCOL_VERSION;
    request.Header.Size = sizeof(request);
    request.LampArrayId = lampArrayId;

    std::memset(&statistics, 0, sizeof(statistics));
    return Control(IOCTL_CLA_GET_STATISTICS, &request, sizeof(request),
                   &statistics, sizeof(statistics), nullptr, error);
}

bool DriverLink::GetLampCount(cla_u32 lampArrayId, cla_u32 &lampCount, std::string &error)
{
    ClaLampArrayIdRequest request{};
    request.Header.Version = CLA_IPC_PROTOCOL_VERSION;
    request.Header.Size = sizeof(request);
    request.LampArrayId = lampArrayId;

    ClaLampCountResponse response{};
    if (!Control(IOCTL_CLA_GET_LAMP_COUNT, &request, sizeof(request),
                 &response, sizeof(response), nullptr, error))
    {
        return false;
    }
    lampCount = response.LampCount;
    return true;
}

bool DriverLink::GetLampInfo(cla_u32 lampArrayId, cla_u32 lampId,
                             ClaLampInfo &info, std::string &error)
{
    ClaLampInfoRequest request{};
    request.Header.Version = CLA_IPC_PROTOCOL_VERSION;
    request.Header.Size = sizeof(request);
    request.LampArrayId = lampArrayId;
    request.LampId = lampId;

    ClaLampInfoResponse response{};
    if (!Control(IOCTL_CLA_GET_LAMP_INFO, &request, sizeof(request),
                 &response, sizeof(response), nullptr, error))
    {
        return false;
    }
    info = response.Info;
    return true;
}

bool DriverLink::IssueRequest(PendingRequest &request, std::string &error)
{
    ResetEvent(request.Overlapped.hEvent);

    DWORD returned = 0;
    const BOOL ok = DeviceIoControl(m_device, IOCTL_CLA_GET_NEXT_UPDATE,
                                    nullptr, 0,
                                    request.Buffer.data(),
                                    static_cast<DWORD>(request.Buffer.size()),
                                    &returned, &request.Overlapped);
    if (!ok)
    {
        const DWORD code = GetLastError();
        if (code != ERROR_IO_PENDING)
        {
            error = FormatLastError("DeviceIoControl(GET_NEXT_UPDATE)", code);
            request.InFlight = false;
            return false;
        }
    }
    request.InFlight = true;
    return true;
}

void DriverLink::PumpThread(UpdateHandler handler)
{
    PendingRequest requests[kOutstandingRequests];
    HANDLE waitHandles[kOutstandingRequests + 1];

    for (int i = 0; i < kOutstandingRequests; ++i)
    {
        requests[i].Buffer.resize(kUpdateBufferBytes);
        requests[i].Overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        waitHandles[i] = requests[i].Overlapped.hEvent;
    }
    waitHandles[kOutstandingRequests] = m_stopEvent;

    std::string error;
    for (int i = 0; i < kOutstandingRequests; ++i)
    {
        if (!IssueRequest(requests[i], error))
        {
            CLA_LOG_ERROR("Update pump could not arm request %d: %s", i, error.c_str());
        }
    }

    while (m_pumpRunning.load())
    {
        const DWORD result = WaitForMultipleObjects(kOutstandingRequests + 1,
                                                    waitHandles, FALSE, INFINITE);
        if (result == WAIT_OBJECT_0 + kOutstandingRequests || !m_pumpRunning.load())
        {
            break;   /* stop requested */
        }
        if (result < WAIT_OBJECT_0 || result >= WAIT_OBJECT_0 + kOutstandingRequests)
        {
            CLA_LOG_ERROR("Update pump wait failed (%lu)", GetLastError());
            break;
        }

        const int index = static_cast<int>(result - WAIT_OBJECT_0);
        PendingRequest &request = requests[index];

        DWORD returned = 0;
        if (!GetOverlappedResult(m_device, &request.Overlapped, &returned, FALSE))
        {
            const DWORD code = GetLastError();
            request.InFlight = false;

            if (code == ERROR_OPERATION_ABORTED)
            {
                break;   /* handle closing */
            }
            CLA_LOG_WARN("GET_NEXT_UPDATE completed with error %lu", code);
            /* Re-arm: a single failed request should not silence the pump. */
            if (m_pumpRunning.load() && !IssueRequest(request, error))
            {
                CLA_LOG_ERROR("Update pump could not re-arm: %s", error.c_str());
                break;
            }
            continue;
        }

        request.InFlight = false;

        /* Validate what the driver sent before trusting any of it. The driver
         * is more privileged than we are, but a length check costs nothing and
         * keeps this loop honest about its own buffer. */
        if (returned >= sizeof(ClaMessageHeader))
        {
            const auto *message =
                reinterpret_cast<const ClaUpdateMessage *>(request.Buffer.data());

            if (message->Header.Version == CLA_IPC_PROTOCOL_VERSION &&
                message->Header.Size <= returned)
            {
                LampArrayUpdate update;
                update.LampArrayId = message->LampArrayId;
                update.Kind = static_cast<ClaUpdateKind>(message->Kind);
                update.FrameSequence = message->FrameSequence;
                update.AutonomousMode = (message->AutonomousMode != 0);

                const cla_u32 lampCount = message->LampCount;
                const cla_u32 required = ClaUpdateMessageSizeForLampCount(lampCount);
                if (lampCount > 0 && required != 0 && required <= returned)
                {
                    update.Colors.assign(message->Colors, message->Colors + lampCount);
                }

                if (handler)
                {
                    handler(update);
                }
            }
            else
            {
                CLA_LOG_WARN("Discarding malformed update (version %u, size %u, returned %lu)",
                             message->Header.Version, message->Header.Size, returned);
            }
        }

        if (m_pumpRunning.load() && !IssueRequest(request, error))
        {
            CLA_LOG_ERROR("Update pump could not re-arm: %s", error.c_str());
            break;
        }
    }

    /* Cancel anything still parked, then wait for it, so the buffers are not
     * freed while the driver could still be writing into them. */
    if (m_device != INVALID_HANDLE_VALUE)
    {
        for (int i = 0; i < kOutstandingRequests; ++i)
        {
            if (requests[i].InFlight)
            {
                CancelIoEx(m_device, &requests[i].Overlapped);
            }
        }
        for (int i = 0; i < kOutstandingRequests; ++i)
        {
            if (requests[i].InFlight)
            {
                DWORD returned = 0;
                GetOverlappedResult(m_device, &requests[i].Overlapped, &returned, TRUE);
            }
        }
    }

    for (int i = 0; i < kOutstandingRequests; ++i)
    {
        if (requests[i].Overlapped.hEvent != nullptr)
        {
            CloseHandle(requests[i].Overlapped.hEvent);
        }
    }
}

bool DriverLink::StartUpdatePump(UpdateHandler handler, std::string &error)
{
    error.clear();
    if (!IsOpen())
    {
        error = "driver not open";
        return false;
    }
    if (m_pumpRunning.load())
    {
        return true;
    }

    m_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (m_stopEvent == nullptr)
    {
        error = FormatLastError("CreateEvent", GetLastError());
        return false;
    }

    m_pumpRunning.store(true);
    m_pumpThread = std::thread(&DriverLink::PumpThread, this, std::move(handler));
    return true;
}

void DriverLink::StopUpdatePump()
{
    if (!m_pumpRunning.exchange(false))
    {
        return;
    }
    if (m_stopEvent != nullptr)
    {
        SetEvent(m_stopEvent);
    }
    if (m_pumpThread.joinable())
    {
        m_pumpThread.join();
    }
    if (m_stopEvent != nullptr)
    {
        CloseHandle(m_stopEvent);
        m_stopEvent = nullptr;
    }
}

} // namespace cla
