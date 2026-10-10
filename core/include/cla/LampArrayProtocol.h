/*
 * Copyright (c) Corsair Dynamic Lighting Bridge contributors.
 * SPDX-License-Identifier: MIT
 *
 * On-the-wire definitions for the HID "Lighting And Illumination" page.
 *
 * Normative source:
 *   USB HID Usage Tables v1.4, section 26 "Lighting And Illumination Page
 *   (0x59)".  https://www.usb.org/sites/default/files/hut1_4.pdf
 *
 * Every constant below is transcribed from that document; nothing here is
 * invented.  Section references are given inline so each value can be checked
 * against the specification.
 *
 * Wire format notes
 * -----------------
 * HID reports are little-endian byte streams.  This header deliberately does
 * NOT declare packed C structs overlaying the wire bytes: the protocol core is
 * compiled by both MSVC (kernel) and GCC/Clang (tests), and relying on struct
 * packing across those toolchains -- plus the unaligned/strict-aliasing issues
 * of casting a byte buffer to a struct pointer -- is avoidable risk.  Reports
 * are serialised field by field with the explicit little-endian helpers in
 * LampArrayDevice.cpp.  The byte offsets are fixed by the report descriptor and
 * are asserted by the unit tests.
 */
#ifndef CLA_LAMP_ARRAY_PROTOCOL_H
#define CLA_LAMP_ARRAY_PROTOCOL_H

#include "ClaTypes.h"

/* ------------------------------------------------------------------ */
/* Usage page / usages (HUT 1.4 table in s26)                          */
/* ------------------------------------------------------------------ */

#define CLA_USAGE_PAGE_LIGHTING_AND_ILLUMINATION   0x59

#define CLA_USAGE_LAMP_ARRAY                       0x01
#define CLA_USAGE_LAMP_ARRAY_ATTRIBUTES_REPORT     0x02
#define CLA_USAGE_LAMP_COUNT                       0x03
#define CLA_USAGE_BOUNDING_BOX_WIDTH_UM            0x04
#define CLA_USAGE_BOUNDING_BOX_HEIGHT_UM           0x05
#define CLA_USAGE_BOUNDING_BOX_DEPTH_UM            0x06
#define CLA_USAGE_LAMP_ARRAY_KIND                  0x07
#define CLA_USAGE_MIN_UPDATE_INTERVAL_US           0x08
#define CLA_USAGE_LAMP_ATTRIBUTES_REQUEST_REPORT   0x20
#define CLA_USAGE_LAMP_ID                          0x21
#define CLA_USAGE_LAMP_ATTRIBUTES_RESPONSE_REPORT  0x22
#define CLA_USAGE_POSITION_X_UM                    0x23
#define CLA_USAGE_POSITION_Y_UM                    0x24
#define CLA_USAGE_POSITION_Z_UM                    0x25
#define CLA_USAGE_LAMP_PURPOSES                    0x26
#define CLA_USAGE_UPDATE_LATENCY_US                0x27
#define CLA_USAGE_RED_LEVEL_COUNT                  0x28
#define CLA_USAGE_GREEN_LEVEL_COUNT                0x29
#define CLA_USAGE_BLUE_LEVEL_COUNT                 0x2A
#define CLA_USAGE_INTENSITY_LEVEL_COUNT            0x2B
#define CLA_USAGE_IS_PROGRAMMABLE                  0x2C
#define CLA_USAGE_INPUT_BINDING                    0x2D
#define CLA_USAGE_LAMP_MULTI_UPDATE_REPORT         0x50
#define CLA_USAGE_RED_UPDATE_CHANNEL               0x51
#define CLA_USAGE_GREEN_UPDATE_CHANNEL             0x52
#define CLA_USAGE_BLUE_UPDATE_CHANNEL              0x53
#define CLA_USAGE_INTENSITY_UPDATE_CHANNEL         0x54
#define CLA_USAGE_LAMP_UPDATE_FLAGS                0x55
#define CLA_USAGE_LAMP_RANGE_UPDATE_REPORT         0x60
#define CLA_USAGE_LAMP_ID_START                    0x61
#define CLA_USAGE_LAMP_ID_END                      0x62
#define CLA_USAGE_LAMP_ARRAY_CONTROL_REPORT        0x70
#define CLA_USAGE_AUTONOMOUS_MODE                  0x71

/* ------------------------------------------------------------------ */
/* Report ids.                                                         */
/*                                                                     */
/* All six reports are FEATURE reports -- the LampArray protocol has no */
/* input or output reports at all.  This matters for the driver: every  */
/* host interaction arrives as IOCTL_HID_GET_FEATURE or                 */
/* IOCTL_HID_SET_FEATURE, never as a read/write report.                 */
/* ------------------------------------------------------------------ */

#define CLA_REPORT_ID_LAMP_ARRAY_ATTRIBUTES        1  /* GET  */
#define CLA_REPORT_ID_LAMP_ATTRIBUTES_REQUEST      2  /* SET  */
#define CLA_REPORT_ID_LAMP_ATTRIBUTES_RESPONSE     3  /* GET  */
#define CLA_REPORT_ID_LAMP_MULTI_UPDATE            4  /* SET  */
#define CLA_REPORT_ID_LAMP_RANGE_UPDATE            5  /* SET  */
#define CLA_REPORT_ID_LAMP_ARRAY_CONTROL           6  /* SET  */

/* Payload sizes in bytes, EXCLUDING the leading report id byte.
 * These are dictated by the report descriptor; DescriptorDump recomputes them
 * from the descriptor itself and the unit tests assert the two agree. */
#define CLA_PAYLOAD_SIZE_LAMP_ARRAY_ATTRIBUTES     22
#define CLA_PAYLOAD_SIZE_LAMP_ATTRIBUTES_REQUEST    2
#define CLA_PAYLOAD_SIZE_LAMP_ATTRIBUTES_RESPONSE  28
#define CLA_PAYLOAD_SIZE_LAMP_MULTI_UPDATE         50
#define CLA_PAYLOAD_SIZE_LAMP_RANGE_UPDATE          9
#define CLA_PAYLOAD_SIZE_LAMP_ARRAY_CONTROL         1

/* HIDCLASS hands the minidriver the report id as byte 0 of the transfer
 * buffer and counts it in reportBufferLen (confirmed by the KMDF vhidmini2
 * sample, which computes `sizeof(payload) + sizeof(packet.reportId)`).
 * These are therefore the full HID_XFER_PACKET buffer lengths. */
#define CLA_REPORT_ID_SIZE                          1
#define CLA_BUFFER_SIZE_LAMP_ARRAY_ATTRIBUTES      (CLA_PAYLOAD_SIZE_LAMP_ARRAY_ATTRIBUTES    + CLA_REPORT_ID_SIZE) /* 23 */
#define CLA_BUFFER_SIZE_LAMP_ATTRIBUTES_REQUEST    (CLA_PAYLOAD_SIZE_LAMP_ATTRIBUTES_REQUEST  + CLA_REPORT_ID_SIZE) /*  3 */
#define CLA_BUFFER_SIZE_LAMP_ATTRIBUTES_RESPONSE   (CLA_PAYLOAD_SIZE_LAMP_ATTRIBUTES_RESPONSE + CLA_REPORT_ID_SIZE) /* 29 */
#define CLA_BUFFER_SIZE_LAMP_MULTI_UPDATE          (CLA_PAYLOAD_SIZE_LAMP_MULTI_UPDATE        + CLA_REPORT_ID_SIZE) /* 51 */
#define CLA_BUFFER_SIZE_LAMP_RANGE_UPDATE          (CLA_PAYLOAD_SIZE_LAMP_RANGE_UPDATE        + CLA_REPORT_ID_SIZE) /* 10 */
#define CLA_BUFFER_SIZE_LAMP_ARRAY_CONTROL         (CLA_PAYLOAD_SIZE_LAMP_ARRAY_CONTROL       + CLA_REPORT_ID_SIZE) /*  2 */

/* Largest buffer any report needs; used to size fixed stack buffers. */
#define CLA_MAX_REPORT_BUFFER_SIZE                 CLA_BUFFER_SIZE_LAMP_MULTI_UPDATE  /* 51 */

/* Number of update slots in a LampMultiUpdateReport.  Fixed by the descriptor
 * (ReportCount(8) over the LampId array).  HUT 1.4 s26.11.1: "The
 * MaxLogicalSize of the LampCount Usage in the descriptor defines the number
 * of available update-slots." */
#define CLA_LAMP_MULTI_UPDATE_SLOT_COUNT            8

/* Maximum addressable lamps: LampId is a 16-bit field. */
#define CLA_MAX_LAMP_COUNT                          65535u

/* ------------------------------------------------------------------ */
/* LampArrayKind values -- HUT 1.4 table 26.2.1                        */
/* ------------------------------------------------------------------ */
typedef enum _ClaLampArrayKind
{
    ClaLampArrayKindUndefined      = 0x00,
    ClaLampArrayKindKeyboard       = 0x01,
    ClaLampArrayKindMouse          = 0x02,
    ClaLampArrayKindGameController = 0x03,
    ClaLampArrayKindPeripheral     = 0x04,
    ClaLampArrayKindScene          = 0x05,
    ClaLampArrayKindNotification   = 0x06,
    ClaLampArrayKindChassis        = 0x07,
    ClaLampArrayKindWearable       = 0x08,
    ClaLampArrayKindFurniture      = 0x09,
    ClaLampArrayKindArt            = 0x0A
} ClaLampArrayKind;

/* ------------------------------------------------------------------ */
/* LampPurposes flags -- HUT 1.4 table 26.3.1 (combinable)             */
/* ------------------------------------------------------------------ */
#define CLA_LAMP_PURPOSE_CONTROL       0x01
#define CLA_LAMP_PURPOSE_ACCENT        0x02
#define CLA_LAMP_PURPOSE_BRANDING      0x04
#define CLA_LAMP_PURPOSE_STATUS        0x08
#define CLA_LAMP_PURPOSE_ILLUMINATION  0x10
#define CLA_LAMP_PURPOSE_PRESENTATION  0x20

/* ------------------------------------------------------------------ */
/* LampUpdate flags -- HUT 1.4 table 26.4.1                            */
/* ------------------------------------------------------------------ */
/* "Signals that this was the last update in a batch of updates. Device should
 * now process all preceding messages as a single update to Lamp state." */
#define CLA_LAMP_UPDATE_COMPLETE       0x01

#endif /* CLA_LAMP_ARRAY_PROTOCOL_H */
