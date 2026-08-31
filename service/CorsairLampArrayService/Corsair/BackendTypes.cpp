/*
 * Copyright (c) Corsair Dynamic Lighting Bridge contributors.
 * SPDX-License-Identifier: MIT
 */

#include "Corsair/ICorsairBackend.h"

namespace cla {

const char *ToString(BackendStatus status)
{
    switch (status)
    {
    case BackendStatus::Ok:             return "Ok";
    case BackendStatus::NotConnected:   return "NotConnected";
    case BackendStatus::NoControl:      return "NoControl";
    case BackendStatus::DeviceNotFound: return "DeviceNotFound";
    case BackendStatus::InvalidArgument:return "InvalidArgument";
    case BackendStatus::NotSupported:   return "NotSupported";
    case BackendStatus::Failed:         return "Failed";
    default:                            return "Unknown";
    }
}

} // namespace cla
