/*
 * Copyright (c) Corsair Dynamic Lighting Bridge contributors.
 * SPDX-License-Identifier: MIT
 *
 * Declarations for the parts of the Corsair iCUE SDK v4 ABI this project
 * calls, so that the SDK can be loaded at run time without vendoring or
 * redistributing anything of Corsair's.
 *
 * WHY THESE ARE DECLARED HERE RATHER THAN INCLUDED
 * ------------------------------------------------
 * The iCUE SDK is published by Corsair under its own terms and this project
 * does not redistribute it: no headers, no import library, no DLL (see
 * NOTICE and docs/corsair-sdk.md).  The service therefore locates the user's
 * own iCUE installation, LoadLibrary's the SDK from there, and resolves the
 * handful of entry points it needs with GetProcAddress.
 *
 * Declaring the ABI locally has a second benefit: a missing or older SDK
 * becomes a resolved-to-null function pointer that the service can report
 * cleanly, rather than a load-time failure that would stop the service
 * starting at all.  Project Phase 14 requires the service to survive "iCUE SDK
 * DLL is missing", and this is how.
 *
 * The layouts below are transcribed from the SDK's public documentation and
 * headers (https://github.com/CorsairOfficial/cue-sdk,
 * https://corsairofficial.github.io/cue-sdk/).  Because a layout mistake here
 * would be memory corruption rather than a compile error, every structure's
 * size is asserted at the bottom of this file.
 */
#ifndef CLA_ICUE_SDK_API_H
#define CLA_ICUE_SDK_API_H

#include <cstdint>
#include <cstddef>

namespace cla {
namespace icue {

/* Sizes and limits, from iCUESDK.h. */
constexpr unsigned int kStringSizeS = 64;
constexpr unsigned int kStringSizeM = 128;
constexpr unsigned int kDeviceCountMax = 64;
constexpr unsigned int kDeviceLedCountMax = 512;
constexpr unsigned int kLayerPriorityMax = 255;

using DeviceId = char[kStringSizeM];
using LedLuid = unsigned int;

enum Error : int
{
    CE_Success = 0,
    CE_NotConnected = 1,
    CE_NoControl = 2,
    CE_IncompatibleProtocol = 3,
    CE_InvalidArguments = 4,
    CE_InvalidOperation = 5,
    CE_DeviceNotFound = 6,
    CE_NotAllowed = 7
};

enum SessionState : int
{
    CSS_Invalid = 0,
    CSS_Closed = 1,
    CSS_Connecting = 2,
    CSS_Timeout = 3,
    CSS_ConnectionRefused = 4,
    CSS_ConnectionLost = 5,
    CSS_Connected = 6
};

enum DeviceType : int
{
    CDT_Unknown = 0x0000,
    CDT_Keyboard = 0x0001,
    CDT_Mouse = 0x0002,
    CDT_Mousemat = 0x0004,
    CDT_Headset = 0x0008,
    CDT_HeadsetStand = 0x0010,
    CDT_FanLedController = 0x0020,
    CDT_LedController = 0x0040,
    CDT_MemoryModule = 0x0080,
    CDT_Cooler = 0x0100,
    CDT_Motherboard = 0x0200,
    CDT_GraphicsCard = 0x0400,
    CDT_Touchbar = 0x0800,
    CDT_GameController = 0x1000,
    CDT_All = static_cast<int>(0xFFFFFFFF)
};

enum EventId : int
{
    CEI_Invalid = 0,
    CEI_DeviceConnectionStatusChangedEvent = 1,
    CEI_KeyEvent = 2
};

enum AccessLevel : int
{
    CAL_Shared = 0,
    CAL_ExclusiveLightingControl = 1,
    CAL_ExclusiveKeyEventsListening = 2,
    CAL_ExclusiveLightingControlAndKeyEventsListening = 3
};

struct Version
{
    int major;
    int minor;
    int patch;
};

struct SessionDetails
{
    Version clientVersion;
    Version serverVersion;
    Version serverHostVersion;
};

struct SessionStateChanged
{
    SessionState state;
    SessionDetails details;
};

struct DeviceInfo
{
    DeviceType type;
    DeviceId id;
    char serial[kStringSizeM];
    char model[kStringSizeM];
    int ledCount;
    int channelCount;
};

struct LedPosition
{
    LedLuid id;
    /* Millimetres for keyboards, mice, mousemats, headset stands and memory
     * modules; logical units for DIY devices, headsets and coolers.  See
     * IcueSdkBackend::PositionUnitsForType. */
    double cx;
    double cy;
};

struct DeviceFilter
{
    int deviceTypeMask;
};

struct LedColor
{
    LedLuid id;
    unsigned char r;
    unsigned char g;
    unsigned char b;
    unsigned char a;
};

struct DeviceConnectionStatusChangedEvent
{
    DeviceId deviceId;
    bool isConnected;
};

struct KeyEvent
{
    DeviceId deviceId;
    int keyId;
    bool isPressed;
};

struct Event
{
    EventId id;
    union
    {
        const DeviceConnectionStatusChangedEvent *deviceConnectionStatusChangedEvent;
        const KeyEvent *keyEvent;
    };
};

using SessionStateChangedHandler = void (*)(void *context, const SessionStateChanged *eventData);
using AsyncCallback = void (*)(void *context, Error error);
using EventHandler = void (*)(void *context, const Event *event);

/*
 * The entry points the service resolves.  Anything left null means the
 * installed SDK is older than expected; the service reports which symbol was
 * missing rather than crashing on a null call.
 */
struct Api
{
    Error (*Connect)(SessionStateChangedHandler onStateChanged, void *context) = nullptr;
    Error (*GetSessionDetails)(SessionDetails *details) = nullptr;
    Error (*Disconnect)() = nullptr;
    Error (*GetDevices)(const DeviceFilter *filter, int sizeMax, DeviceInfo *devices, int *size) = nullptr;
    Error (*GetDeviceInfo)(const char *deviceId, DeviceInfo *deviceInfo) = nullptr;
    Error (*GetLedPositions)(const char *deviceId, int sizeMax, LedPosition *positions, int *size) = nullptr;
    Error (*SubscribeForEvents)(EventHandler onEvent, void *context) = nullptr;
    Error (*UnsubscribeFromEvents)() = nullptr;
    Error (*SetLedColors)(const char *deviceId, int size, const LedColor *colors) = nullptr;
    Error (*SetLedColorsBuffer)(const char *deviceId, int size, const LedColor *colors) = nullptr;
    Error (*SetLedColorsFlushBufferAsync)(AsyncCallback callback, void *context) = nullptr;
    Error (*SetLayerPriority)(unsigned int priority) = nullptr;
    Error (*RequestControl)(const char *deviceId, AccessLevel accessLevel) = nullptr;
    Error (*ReleaseControl)(const char *deviceId) = nullptr;
};

const char *ToString(Error error);
const char *ToString(SessionState state);

/*
 * Layout assertions.
 *
 * These structures cross a DLL boundary into code compiled by someone else.
 * A padding or ordering mistake would be silent memory corruption at run time
 * on a user's machine, so it is turned into a compile error here instead.
 */
static_assert(sizeof(Version) == 12, "Version layout");
static_assert(sizeof(SessionDetails) == 36, "SessionDetails layout");
static_assert(sizeof(SessionStateChanged) == 40, "SessionStateChanged layout");
static_assert(sizeof(DeviceInfo) == 4 + 128 + 128 + 128 + 4 + 4, "DeviceInfo layout");
static_assert(offsetof(DeviceInfo, id) == 4, "DeviceInfo::id offset");
static_assert(offsetof(DeviceInfo, serial) == 132, "DeviceInfo::serial offset");
static_assert(offsetof(DeviceInfo, model) == 260, "DeviceInfo::model offset");
static_assert(offsetof(DeviceInfo, ledCount) == 388, "DeviceInfo::ledCount offset");
static_assert(sizeof(LedPosition) == 24, "LedPosition layout");
static_assert(offsetof(LedPosition, cx) == 8, "LedPosition::cx offset");
static_assert(sizeof(LedColor) == 8, "LedColor layout");
static_assert(sizeof(DeviceFilter) == 4, "DeviceFilter layout");

} // namespace icue
} // namespace cla

#endif /* CLA_ICUE_SDK_API_H */
