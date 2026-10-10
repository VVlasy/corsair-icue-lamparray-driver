/*
 * Copyright (c) Corsair Dynamic Lighting Bridge contributors.
 * SPDX-License-Identifier: MIT
 *
 * Built on every platform, not just Windows, so that the ABI layout
 * assertions in IcueSdkApi.h are checked by every CI run.  A struct-layout
 * mistake against a DLL compiled by someone else would otherwise only show up
 * as memory corruption on a user's machine.
 */

#include "Corsair/IcueSdkApi.h"

namespace cla {
namespace icue {

const char *ToString(Error error)
{
    switch (error)
    {
    case CE_Success:             return "Success";
    case CE_NotConnected:        return "NotConnected (iCUE not running, or third-party control disabled in iCUE settings)";
    case CE_NoControl:           return "NoControl (another client holds exclusive control)";
    case CE_IncompatibleProtocol:return "IncompatibleProtocol (iCUE is older than this SDK client expects)";
    case CE_InvalidArguments:    return "InvalidArguments";
    case CE_InvalidOperation:    return "InvalidOperation";
    case CE_DeviceNotFound:      return "DeviceNotFound";
    case CE_NotAllowed:          return "NotAllowed (disabled in iCUE settings)";
    default:                     return "Unknown";
    }
}

const char *ToString(SessionState state)
{
    switch (state)
    {
    case CSS_Invalid:           return "Invalid";
    case CSS_Closed:            return "Closed";
    case CSS_Connecting:        return "Connecting";
    case CSS_Timeout:           return "Timeout";
    case CSS_ConnectionRefused: return "ConnectionRefused";
    case CSS_ConnectionLost:    return "ConnectionLost";
    case CSS_Connected:         return "Connected";
    default:                    return "Unknown";
    }
}

} // namespace icue
} // namespace cla
