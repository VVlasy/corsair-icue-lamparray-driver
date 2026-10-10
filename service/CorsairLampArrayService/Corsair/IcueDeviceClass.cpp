/*
 * Copyright (c) Corsair Dynamic Lighting Bridge contributors.
 * SPDX-License-Identifier: MIT
 *
 * iCUE device type classification.  See IcueDeviceClass.h.
 */

#include "Corsair/IcueDeviceClass.h"
#include "Corsair/IcueSdkApi.h"

#include "cla/CorsairMapping.h"
#include "cla/LampArrayProtocol.h"

namespace cla {
namespace icue {

cla_u32 LampArrayKindForType(int corsairDeviceType)
{
    /*
     * Map iCUE's device taxonomy onto the LampArrayKind values from HUT 1.4
     * table 26.2.1.  Windows uses the kind to decide which effects suit a
     * device, so a mouse must not claim to be a keyboard.
     */
    switch (corsairDeviceType)
    {
    case CDT_Keyboard:         return ClaLampArrayKindKeyboard;
    case CDT_Mouse:            return ClaLampArrayKindMouse;
    case CDT_GameController:   return ClaLampArrayKindGameController;

    /* Mousemats, headset stands and touchbars are accessories the user looks
     * at but does not wear: "Peripheral" per the spec's own examples
     * ("speakers, mousepad, microphone, webcam"). */
    case CDT_Mousemat:
    case CDT_HeadsetStand:
    case CDT_Touchbar:         return ClaLampArrayKindPeripheral;

    /* "LampArray is embedded in a wearable accessory (audio-headset, ...)" */
    case CDT_Headset:          return ClaLampArrayKindWearable;

    /* Internal PC components: the spec names RAM sticks, motherboards and fans
     * explicitly under Chassis. */
    case CDT_FanLedController:
    case CDT_LedController:
    case CDT_MemoryModule:
    case CDT_Cooler:
    case CDT_Motherboard:
    case CDT_GraphicsCard:     return ClaLampArrayKindChassis;

    default:                   return ClaLampArrayKindUndefined;
    }
}

cla_u32 PositionUnitsForType(int corsairDeviceType)
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
    case CDT_Keyboard:
    case CDT_Mouse:
    case CDT_Mousemat:
    case CDT_HeadsetStand:
    case CDT_MemoryModule:
        return ClaSourceUnitsMillimeters;
    default:
        return ClaSourceUnitsLogical;
    }
}

cla_u32 SourceScopeForType(int corsairDeviceType)
{
    /*
     * Fan and LED controllers (Commander, Lighting Node) drive fans and
     * strips mounted throughout the case, so their LED layout spans the case
     * rather than one 120mm fan.  Everything else is a single component.
     *
     * Coolers are left as components even though an AIO's fans sit on a
     * radiator: the SDK reports them in logical units with no indication of
     * radiator size, and a 120mm envelope is no worse a guess than 360mm.
     */
    switch (corsairDeviceType)
    {
    case CDT_FanLedController:
    case CDT_LedController:
        return ClaSourceScopeWholeCase;
    default:
        return ClaSourceScopeComponent;
    }
}

} // namespace icue
} // namespace cla
