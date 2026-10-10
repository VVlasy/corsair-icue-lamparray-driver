/*
 * Copyright (c) Corsair Dynamic Lighting Bridge contributors.
 * SPDX-License-Identifier: MIT
 *
 * Entry point for the driver syntax and linkage check.
 *
 * Reaching this main() means every driver translation unit compiled and the
 * whole set linked: no typos, no missing declarations, no signature
 * mismatches, no duplicate symbols across translation units and no functions
 * declared but never defined.
 *
 * It does NOT mean the driver works. See stubs/README.md.
 */

#include "stubs/ntddk.h"
#include "stubs/wdf.h"
#include "stubs/vhf.h"

#include "cla/LampArrayDescriptor.h"

#include <cstdio>

/* Declared in the driver sources; referenced here so the linker must resolve
 * them, which is what turns this into a linkage check rather than a
 * compile-only one. */
extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT, PUNICODE_STRING);
extern "C" const GUID GUID_DEVINTERFACE_CORSAIR_LAMP_ARRAY;

int main()
{
    /* Take the address of DriverEntry rather than calling it: the stubs do not
     * emulate WDF, so calling in would prove nothing and could crash. */
    volatile void *entry = reinterpret_cast<void *>(&DriverEntry);
    (void)entry;

    /* Reference the GUID so a duplicate or missing definition fails the link. */
    const GUID &guid = GUID_DEVINTERFACE_CORSAIR_LAMP_ARRAY;

    std::printf("Driver sources compiled and linked.\n");
    std::printf("  device interface GUID: {%08X-%04X-%04X-...}\n",
                guid.Data1, guid.Data2, guid.Data3);
    std::printf("  report descriptor    : %u bytes\n",
                CLA_LAMP_ARRAY_DESCRIPTOR_SIZE);
    std::printf("\nThis is a syntax and linkage check only. It does not\n");
    std::printf("substitute for building the driver with the WDK.\n");
    return 0;
}
