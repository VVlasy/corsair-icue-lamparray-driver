/*
 * Copyright (c) Corsair Dynamic Lighting Bridge contributors.
 * SPDX-License-Identifier: MIT
 *
 * iCUE device type classification.  Each of these decisions changes what
 * Windows believes a device looks like without ever producing an error, so
 * they are pinned here rather than discovered on someone's PC.
 */

#include "ClaTest.h"

#include "Corsair/IcueDeviceClass.h"
#include "Corsair/IcueSdkApi.h"
#include "cla/CorsairMapping.h"
#include "cla/LampArrayProtocol.h"

using namespace cla;

/* Fan and LED hubs drive fans and strips across the whole case. */
CLA_TEST(DeviceClass_ControllersSpanTheCase)
{
    for (int type : {static_cast<int>(icue::CDT_FanLedController),
                     static_cast<int>(icue::CDT_LedController)})
    {
        CLA_CHECK_EQ(icue::SourceScopeForType(type),
                     static_cast<cla_u32>(ClaSourceScopeWholeCase));
        CLA_CHECK_EQ(icue::LampArrayKindForType(type),
                     static_cast<cla_u32>(ClaLampArrayKindChassis));
        /* Logical units: without the whole-case scope they would be scaled
         * onto one fan. */
        CLA_CHECK_EQ(icue::PositionUnitsForType(type),
                     static_cast<cla_u32>(ClaSourceUnitsLogical));
    }
}

/* Single components keep the component envelope, Chassis or not. */
CLA_TEST(DeviceClass_SingleComponentsAreComponents)
{
    for (int type : {static_cast<int>(icue::CDT_MemoryModule),
                     static_cast<int>(icue::CDT_Cooler),
                     static_cast<int>(icue::CDT_Motherboard),
                     static_cast<int>(icue::CDT_GraphicsCard),
                     static_cast<int>(icue::CDT_Keyboard),
                     static_cast<int>(icue::CDT_Mouse),
                     static_cast<int>(icue::CDT_Mousemat),
                     static_cast<int>(icue::CDT_Headset),
                     static_cast<int>(icue::CDT_HeadsetStand),
                     static_cast<int>(icue::CDT_Touchbar),
                     static_cast<int>(icue::CDT_GameController),
                     static_cast<int>(icue::CDT_Unknown)})
    {
        CLA_CHECK_EQ(icue::SourceScopeForType(type),
                     static_cast<cla_u32>(ClaSourceScopeComponent));
    }
}

CLA_TEST(DeviceClass_KindsAndUnitsUnchanged)
{
    CLA_CHECK_EQ(icue::LampArrayKindForType(icue::CDT_Keyboard),
                 static_cast<cla_u32>(ClaLampArrayKindKeyboard));
    CLA_CHECK_EQ(icue::LampArrayKindForType(icue::CDT_Headset),
                 static_cast<cla_u32>(ClaLampArrayKindWearable));
    CLA_CHECK_EQ(icue::LampArrayKindForType(icue::CDT_Mousemat),
                 static_cast<cla_u32>(ClaLampArrayKindPeripheral));
    CLA_CHECK_EQ(icue::LampArrayKindForType(icue::CDT_Unknown),
                 static_cast<cla_u32>(ClaLampArrayKindUndefined));
    CLA_CHECK_EQ(icue::PositionUnitsForType(icue::CDT_MemoryModule),
                 static_cast<cla_u32>(ClaSourceUnitsMillimeters));
    CLA_CHECK_EQ(icue::PositionUnitsForType(icue::CDT_Cooler),
                 static_cast<cla_u32>(ClaSourceUnitsLogical));
}
