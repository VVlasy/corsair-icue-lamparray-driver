/*
 * Copyright (c) Corsair Dynamic Lighting Bridge contributors.
 * SPDX-License-Identifier: MIT
 *
 * How the service classifies an iCUE device type: which LampArrayKind it is,
 * which units its LED positions use, and how much of the case it spans.
 *
 * Kept apart from IcueSdkBackend, which is Windows-only, so that these
 * decisions -- each of which silently changes what Windows believes the
 * device looks like -- are tested on every platform.
 */
#ifndef CLA_ICUE_DEVICE_CLASS_H
#define CLA_ICUE_DEVICE_CLASS_H

#include "cla/ClaTypes.h"

namespace cla {
namespace icue {

/* Map an iCUE device type onto a LampArray kind. */
cla_u32 LampArrayKindForType(int corsairDeviceType);

/* Whether a device type reports positions in mm or logical units
 * (ClaSourceUnits). */
cla_u32 PositionUnitsForType(int corsairDeviceType);

/* Whether a device type is one component or spans the case (ClaSourceScope). */
cla_u32 SourceScopeForType(int corsairDeviceType);

} // namespace icue
} // namespace cla

#endif /* CLA_ICUE_DEVICE_CLASS_H */
