/*
 * Copyright (c) Corsair Dynamic Lighting Bridge contributors.
 * SPDX-License-Identifier: MIT
 *
 * The HID report descriptor that makes Windows classify this virtual device
 * as a Dynamic Lighting LampArray.
 */
#ifndef CLA_LAMP_ARRAY_DESCRIPTOR_H
#define CLA_LAMP_ARRAY_DESCRIPTOR_H

#include "ClaTypes.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CLA_LAMP_ARRAY_DESCRIPTOR_SIZE 292

/* The report descriptor.  Exactly CLA_LAMP_ARRAY_DESCRIPTOR_SIZE bytes. */
extern const cla_u8 ClaLampArrayReportDescriptor[CLA_LAMP_ARRAY_DESCRIPTOR_SIZE];

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CLA_LAMP_ARRAY_DESCRIPTOR_H */
