# Development

## What builds where

The repository is split so that most of it builds and tests anywhere:

| Component | Toolchain | Builds on |
|---|---|---|
| `core/` protocol, descriptor, IPC validation | CMake, any C++17 compiler | anywhere |
| `core/` mapping (`cla_mapping`) | CMake | anywhere |
| `service/` backend abstraction, fake backend, frame pipeline | CMake | anywhere |
| `tests/` | CMake + CTest | anywhere |
| `tools/DescriptorDump` | CMake | anywhere |
| `service/` iCUE backend, service host, driver link | CMake + MSVC | Windows |
| `tools/CorsairProbe`, `HidTestClient`, `LampArrayTest` | CMake + MSVC (+ Windows SDK) | Windows |
| `driver/` | MSBuild + WDK | Windows |

That the protocol core builds on Linux is not incidental. It is compiled into
the kernel driver, so it must avoid the CRT, the STL, exceptions, RTTI and
allocation -- and building it as an ordinary library on a different toolchain
is what keeps that discipline honest, as well as letting the parsing code be
fuzzed under AddressSanitizer where a bug is a test failure rather than a
bugcheck.

## Building the portable parts

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Under sanitizers, which is what CI does and what you should run before touching
anything in `core/`:

```sh
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DCLA_SANITIZE=ON
cmake --build build-asan --parallel
ctest --test-dir build-asan --output-on-failure
```

Validate the descriptor:

```sh
./build/tools/DescriptorDump/DescriptorDump          # decoded listing
./build/tools/DescriptorDump/DescriptorDump --check  # exit code only
```

Compile and link the **driver** sources without a WDK:

```sh
./build/tests/DriverSyntax/DriverSyntaxCheck
```

This builds the five driver translation units against stub WDK headers. It is
a syntax and linkage check, not a substitute for a WDK build -- the stubs
approximate the WDK's shape, not its behaviour, so they cannot tell you the
driver is correct. What they do catch, on any machine in seconds, is the class
of problem that otherwise sits undiscovered until someone with a WDK tries:
typos, misspelled APIs, wrong argument counts, functions declared but never
defined, and duplicate symbols across translation units.

That last one is not hypothetical. This harness was written after the driver
was complete, and immediately found two errors that would have failed the first
real build: a `#include <initguid.h>` in a shared header made all five
translation units emit their own definition of the device interface GUID, and a
`goto` in `ClaLampArrayCreate` crossed variable initialisations, which MSVC
rejects as C2362. See `tests/DriverSyntax/stubs/README.md`.

Source hygiene (non-ASCII, tabs, trailing whitespace, SPDX headers):

```sh
./scripts/check-source-hygiene.sh
```

## Building the driver

Needs **Visual Studio 2022** with the Desktop C++ workload, the **Windows 11
SDK**, and the **WDK** matching that SDK version. Install the SDK before the
WDK: the WDK installer adds the Visual Studio integration that provides the
`WindowsKernelModeDriver10.0` platform toolset, and it needs the SDK present.

```
msbuild driver\CorsairLampArray\CorsairLampArray.vcxproj ^
    /p:Configuration=Debug /p:Platform=x64
```

Output lands in `build\driver\x64\Debug\`.

Without an installed WDK, restore the pinned NuGet WDK first (this is what CI
does); `driver\Directory.Build.props` picks it up automatically, and ignores it
when it has not been restored:

```
nuget restore driver\packages.config -PackagesDirectory driver\packages
```

Build with the 64-bit MSBuild: the NuGet WDK ships InfVerif for x64 and ARM64
only, so the 32-bit MSBuild fails the InfVerif step.

The project has Code Analysis and PREfast on, and warnings as errors. This is
kernel code parsing untrusted input; the extra build time is worth it.

Note which core sources the `.vcxproj` lists: `LampArrayDescriptor.cpp`,
`LampArrayDevice.cpp`, `HidDescriptorParser.cpp` and `IpcContract.cpp`, and
deliberately **not** `CorsairMapping.cpp`, which uses floating point. Floating
point in kernel mode requires `KeSaveFloatingPointState`. The list is explicit
rather than a glob so that adding a non-kernel-safe file to `core/` cannot
silently end up in the driver.

## Installing a development build

1. Enable test signing and reboot:
   ```powershell
   .\scripts\EnableTestSigning.ps1
   ```
   This lowers a real security boundary -- see `docs/signing.md`. On a machine
   with Secure Boot on, it has no effect until Secure Boot is turned off in
   firmware.

2. Install the driver package and create the device node:
   ```powershell
   .\scripts\InstallDriver.ps1
   ```
   Two separate steps happen here. Staging the package alone creates nothing;
   the root device node is what causes the driver to load. Skipping the second
   is the usual reason a first install appears to do nothing.

3. Install and start the service:
   ```powershell
   .\build\service\Release\CorsairLampArrayService.exe --install
   sc start CorsairLampArrayService
   ```

To iterate on the service without reinstalling, stop it and run it in the
console:

```powershell
sc stop CorsairLampArrayService
.\CorsairLampArrayService.exe --console --verbose
```

## Bringing it up in stages

The project was designed to be verified in the order the brief lays out, and
each stage has a tool that answers one question:

| Question | Tool |
|---|---|
| Does the SDK work at all on this machine? | `CorsairProbe` |
| Does the SDK path light the hardware? | `CorsairProbe --test-colors` |
| Is the descriptor correct? | `DescriptorDump --check` |
| Did the driver load, and what does it think it hosts? | `HidTestClient --driver` |
| Did Windows enumerate a HID LampArray? | `HidTestClient` |
| Does the device work over HID, independent of Windows? | `HidTestClient --test-colors` |
| Does *Windows* see a LampArray? | `LampArrayTest` |
| Does Windows drive it correctly? | `LampArrayTest --test-colors` |
| Is the layout mapped correctly? | `LampArrayTest --rainbow` |

The pairing of `HidTestClient --test-colors` and `LampArrayTest --test-colors`
is the useful one when something is wrong: if the first works and the second
does not, the driver and device are fine and the problem is in how Windows sees
the device.

`LampArrayTest --rainbow` is the only check for the mapping layer that actually
means anything -- a wrong layout produces a sweep that looks scrambled or
bunched, which no amount of protocol testing would reveal.

To exercise the whole Windows path with no Corsair hardware at all, run the
service with `--fake`. It exposes a synthetic 6x18 keyboard through the real
driver, which is exactly what the brief's first milestone asks for.

## Debugging the driver

Attach WinDbg to the target and:

```
!wdfkd.wdfldr                          list KMDF drivers
!wdfkd.wdfdriverinfo CorsairLampArray  driver state
ed nt!Kd_IHVDRIVER_Mask 0xF            enable KdPrint output
```

If the device is not enumerating, check in this order:

1. Is the device node present in Device Manager, and what is its status?
2. Is `vhf` listed under the device's `LowerFilters` registry value? Without
   it, `VhfCreate` has nothing to talk to and no LampArray will ever appear.
3. Does `setupapi.dev.log` in `%windir%\inf` show the INF being applied?
4. Does `HidTestClient --driver` reach the control interface?

## Layout

```
core/          protocol, descriptor, IPC validation, mapping. Portable.
inc/           the Windows half of the driver/service contract.
driver/        the KMDF/VHF driver. MSBuild + WDK.
service/       the user-mode service, backends and frame pipeline.
tools/         diagnostics, one per bring-up question.
tests/         CTest suites, all runnable without Windows.
scripts/       signing and install helpers.
docs/          this.
```

## Conventions

- 4 spaces, no tabs. ASCII only. Every source file carries an SPDX identifier.
  `scripts/check-source-hygiene.sh` enforces all of it and CI runs it.
- `core/` is kernel-safe: no CRT, no STL, no exceptions, no RTTI, no
  allocation. Caller-provided storage throughout.
- Anything that parses untrusted bytes takes an explicit length and is total:
  every pointer/length pair, including null and zero, must be a legal call.
- New rules taken from the specification cite their section number, so the next
  reader can check them.
