# Corsair Dynamic Lighting Bridge

Exposes Corsair RGB devices to **Windows 11 Dynamic Lighting** by presenting
them as standard HID LampArray devices.

Corsair hardware is reached through the official iCUE SDK, so no USB protocol
reverse engineering is involved and iCUE remains responsible for talking to the
hardware.

```
Windows Dynamic Lighting  ->  HID LampArray  ->  VHF  ->  CorsairLampArray.sys
                                                              |
                                                              v
                                       CorsairLampArrayService.exe  ->  iCUE SDK
                                                                            |
                                                                            v
                                                                  Corsair hardware
```

Once installed, Corsair devices appear in **Settings > Personalization >
Dynamic Lighting** alongside natively supported hardware, and any Dynamic
Lighting-aware application can drive them.

## Status

The protocol, mapping and IPC layers are implemented and tested; the driver and
the Windows-only service components are implemented but have **not yet been
run on real hardware**. See [Current state](#current-state) below for exactly
what has and has not been verified. Contributions from anyone with a Corsair
device and a Windows 11 machine are the most useful thing right now.

## Requirements

- Windows 11 **23H2 or later** (Dynamic Lighting does not exist before that)
- **Corsair iCUE** installed, with SDK access enabled (the default)
- A Corsair RGB device supported by iCUE

## Installing

There is no signed release yet, so installation currently means building it and
enabling test signing -- which lowers a real security boundary and is not
something to do on a machine you care about. See
[docs/signing.md](docs/signing.md) and
[docs/development.md](docs/development.md).

```powershell
.\scripts\EnableTestSigning.ps1        # reboot
.\scripts\InstallDriver.ps1
.\build\service\Release\CorsairLampArrayService.exe --install
sc start CorsairLampArrayService
```

To try the Windows integration with no Corsair hardware at all, run the service
with `--fake`: it exposes a synthetic keyboard through the real driver.

## Design in one page

Two components, split along the line the licence and the failure modes both
demand:

**`CorsairLampArray.sys`** -- a KMDF HID source driver using Microsoft's Virtual
HID Framework. It owns the HID report descriptor, parses and validates
everything Windows sends, holds lamp state, and assembles frames. It contains
no Corsair code and never links the iCUE SDK.

**`CorsairLampArrayService.exe`** -- a LocalSystem service. It talks to iCUE,
enumerates devices and LED positions, maps them onto LampArray lamps, and
forwards colours. Everything that changes often lives here, because the driver
is the part that needs signing.

Three decisions worth knowing about:

- **The lamp table flows down once.** Lamp attributes are static per the HID
  spec, so the service pushes the whole table into the driver at creation and
  the driver answers Windows' interrogation from its own copy. The HID path
  never waits on user mode, and enumeration keeps working while the service
  restarts.
- **Frames coalesce, newest wins.** The driver holds exactly one pending frame
  per device. A superseded RGB frame has no value, so dropping it is correct
  rather than a compromise -- queueing would just make the lighting lag.
- **Shared control, not exclusive.** iCUE keeps running the user's own profiles
  underneath and our output layers above them by priority. Taking exclusive
  control would silently disable their profiles for as long as the service
  runs.

Full detail in [docs/architecture.md](docs/architecture.md).

## The HID descriptor

Windows recognises a Dynamic Lighting device by its top-level collection: usage
page **0x59** (Lighting and Illumination), usage **0x01** (LampArray).

The 292-byte descriptor is reproduced **verbatim** from Microsoft's LampArray
firmware sample, which generated it with WaratahCmd from HID Usage Tables 1.4.
It was not written from memory. Every behavioural rule in the implementation
cites the specification section it comes from.

The report sizes are derived twice -- by hand in the constants the driver
compiles against, and mechanically by walking the descriptor bytes -- and the
tests assert the two agree, so they cannot drift apart unnoticed.

```
$ DescriptorDump --check
  ok    top-level usage page is Lighting And Illumination (0x59)
  ok    top-level usage is LampArray (0x01)
  ok    declares 6 reports
  ok    report 4 (LampMultiUpdate): 50 byte payload, 51 byte transfer
  ok    all reports are Feature reports (no input/output)
```

See [docs/hid-descriptor.md](docs/hid-descriptor.md) and
[docs/lamp-array.md](docs/lamp-array.md).

## Building and testing

Most of the project builds and tests on any platform, deliberately. The
protocol core is compiled into the kernel driver, so it avoids the CRT, the
STL, exceptions and allocation -- and building it as an ordinary library
elsewhere is what keeps that honest, as well as letting the parsing code be
fuzzed under AddressSanitizer where a bug is a test failure rather than a
bugcheck.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

The driver needs Visual Studio and the WDK to build for real. CI does that on
every push, against a pinned NuGet WDK, with warnings and Code Analysis
findings as errors. Its sources are also compiled and linked against stub WDK
headers as part of the normal test run, so typos, signature mismatches and
duplicate symbols are caught on any machine, without a WDK. See
[docs/development.md](docs/development.md).

## Diagnosing problems

Each tool answers one question, and they are most useful in this order:

| Question | Command |
|---|---|
| Does the iCUE SDK work here? | `CorsairProbe` |
| Does it light the hardware? | `CorsairProbe --test-colors` |
| Is the descriptor correct? | `DescriptorDump --check` |
| Did the driver load? What does it host? | `HidTestClient --driver` |
| Does the device work over raw HID? | `HidTestClient --test-colors` |
| Does *Windows* see a LampArray? | `LampArrayTest` |
| Is the physical layout mapped right? | `LampArrayTest --rainbow` |

The pair to reach for when lighting is not working is `HidTestClient
--test-colors` and `LampArrayTest --test-colors`. If the first works and the
second does not, the driver and device are fine and the problem is in how
Windows sees the device.

Note that the LampArray spec requires invalid reports to be **ignored
silently**, so a device can be dropping every frame with no error anywhere.
`HidTestClient --driver` prints the driver's per-cause rejection counters,
which is the only way to see this.

## Current state

Implemented and **verified by automated tests** on every push:

- the HID LampArray protocol state machine, against HUT 1.4 section 26
- the report descriptor, validated against the specification and re-derived
  independently
- the driver's IOCTL validation -- its entire trust boundary -- against
  truncation, lying length fields, integer-overflow counts and randomised input
  under AddressSanitizer
- the Corsair-to-LampArray mapping, including degenerate layouts
- the frame pipeline: coalescing, dirty-lamp tracking, throttling, brightness
- the service's device lifecycle against a fake backend that simulates iCUE
  being absent, exiting mid-session, and devices being unplugged

Implemented but **not yet run on real hardware**:

- the kernel driver (it builds in CI, but has not been loaded on a machine)
- the iCUE SDK backend against real iCUE
- the Windows service host and driver link
- end-to-end colour changes reaching physical Corsair hardware

If you have a Corsair keyboard and a Windows 11 machine, running
`CorsairProbe`, then `HidTestClient`, then `LampArrayTest` and reporting what
happens is the single most valuable contribution.

## Licence

MIT -- see [LICENSE](LICENSE).

**This project redistributes no part of the Corsair iCUE SDK** -- no headers, no
import library, no DLL. The service loads the SDK at run time from the user's
own iCUE installation. See [NOTICE](NOTICE) and
[docs/corsair-sdk.md](docs/corsair-sdk.md).

The HID report descriptor is reproduced from Microsoft's MIT-licensed
LampArray sample, with attribution in [NOTICE](NOTICE).

This project is not affiliated with or endorsed by Corsair or Microsoft.
