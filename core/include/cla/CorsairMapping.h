/*
 * Copyright (c) Corsair Dynamic Lighting Bridge contributors.
 * SPDX-License-Identifier: MIT
 *
 * Corsair LED topology -> HID LampArray lamp table.
 *
 * NOT part of the kernel-safe core.  This code uses floating point, which in
 * kernel mode would require KeSaveFloatingPointState, so it is built into a
 * separate library (cla_mapping) that the driver does not link.  The driver
 * only ever consumes the *result* of this, as a validated lamp table pushed
 * down by the service.
 *
 * It also deliberately does not include the iCUE SDK header.  The input is a
 * neutral description of LEDs, so the mapping can be tested with no SDK, no
 * iCUE and no hardware -- and so a second backend could feed it later.
 */
#ifndef CLA_CORSAIR_MAPPING_H
#define CLA_CORSAIR_MAPPING_H

#include "ClaTypes.h"
#include "LampArrayDevice.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Units of the incoming LED coordinates.
 *
 * The iCUE SDK documents CorsairLedPosition::cx/cy as: "for keyboards, mice,
 * mousemats, headset stands and memory modules values are in mm, for
 * DIY-devices, headsets and coolers values are in logical units".  Treating a
 * logical-unit device as millimetres would produce a nonsense bounding box, so
 * the caller must say which it is and logical devices are scaled to a nominal
 * physical size for their kind.
 */
typedef enum _ClaSourceUnits
{
    ClaSourceUnitsMillimeters = 0,
    ClaSourceUnitsLogical = 1
} ClaSourceUnits;

typedef struct _ClaSourceLed
{
    /* Opaque backend LED identifier (a CorsairLedLuid for the iCUE backend).
     * Carried through so the service can map a LampId back to an LED. */
    cla_u32 LedId;
    double X;
    double Y;
} ClaSourceLed;

typedef struct _ClaSourceDevice
{
    cla_u32 LampArrayKind;          /* ClaLampArrayKind */
    cla_u32 Units;                  /* ClaSourceUnits */
    cla_u32 LedCount;
    const ClaSourceLed *Leds;
} ClaSourceDevice;

typedef struct _ClaMappingOptions
{
    /*
     * Two LEDs within this many micrometres of each other in Y are treated as
     * the same row.  HUT 1.4 s26.8.1 recommends assigning LampIds "in a
     * methodical manner (e.g. grid, starting from top-left) to take the most
     * advantage of the LampRangeUpdateReport", which is what row-major
     * ordering achieves: contiguous runs collapse into range updates instead
     * of one multi-update per eight lamps.
     *
     * 0 selects the default: 5% of the bounding-box height, floored at 1mm.
     */
    cla_u32 RowToleranceMicrometers;

    /* Nominal depth for the bounding box.  Corsair reports 2D positions only,
     * but HUT 1.4 s26.2 requires a positive depth.  0 selects a per-kind
     * default. */
    cla_u32 NominalDepthMicrometers;

    /* Reported in LampAttributesResponseReport. 0 selects a default. */
    cla_u32 UpdateLatencyMicroseconds;

    /* Reported in LampArrayAttributesReport.  Bounds how often Windows may
     * send updates. 0 selects a default of 16667us (60Hz). */
    cla_u32 MinUpdateIntervalMicroseconds;

    /* When non-zero, keep the backend's LED order instead of sorting into
     * row-major order.  Diagnostic escape hatch. */
    cla_u8 PreserveSourceOrder;
    cla_u8 Reserved[3];
} ClaMappingOptions;

/* Fills in the documented defaults. */
void ClaMappingOptionsInit(ClaMappingOptions *options);

typedef struct _ClaMappingResult
{
    ClaLampArrayAttributes Attributes;
    /* LampId -> source LED id.  Entry i is the backend LED that LampId i
     * drives, which is what the service needs to build its colour buffer. */
    cla_u32 *LampIdToLedId;
    cla_u32 LampCount;
} ClaMappingResult;

/*
 * Build a lamp table from a device's LED topology.
 *
 * lamps and lampIdToLedId are caller-owned output arrays of at least
 * device->LedCount entries.
 *
 * Returns ClaStatusInvalidArgument for null arguments, a zero LED count, or a
 * count above CLA_MAX_LAMPS_PER_ARRAY.  Degenerate topologies (every LED at
 * the same point, a single LED) are handled rather than rejected: the bounding
 * box is widened to a minimum so it stays positive in every axis, as HUT 1.4
 * s26.2 requires.
 */
ClaStatus ClaBuildLampTable(
    const ClaSourceDevice *device,
    const ClaMappingOptions *options,
    ClaLampInfo *lamps,
    cla_u32 *lampIdToLedId,
    ClaMappingResult *result);

/* Default LampPurposes for a device kind. */
cla_u32 ClaDefaultLampPurposeForKind(cla_u32 lampArrayKind);

/* Nominal physical size, in micrometres, used to scale logical-unit devices
 * and to floor degenerate bounding boxes. */
void ClaNominalSizeForKind(cla_u32 lampArrayKind,
                           cla_u32 *widthMicrometers,
                           cla_u32 *heightMicrometers,
                           cla_u32 *depthMicrometers);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CLA_CORSAIR_MAPPING_H */
