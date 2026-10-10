# The Corsair iCUE SDK dependency

## What this project ships, and what it does not

**Nothing of Corsair's is redistributed here.** No headers, no import library,
no DLL. The iCUE SDK is published by Corsair under its own terms, and this
repository stays clear of them entirely.

Instead:

- `service/CorsairLampArrayService/Corsair/IcueSdkApi.h` **declares** the parts
  of the SDK ABI the service calls -- the enums, structures and function
  signatures. These are interface declarations transcribed from Corsair's
  public documentation, not Corsair's code.
- `IcueSdkBackend` loads the DLL at run time from the user's own iCUE
  installation with `LoadLibrary`, and resolves entry points with
  `GetProcAddress`.

This has a practical benefit beyond licensing: a missing or older SDK becomes a
function pointer that resolved to null and can be reported cleanly, instead of
a load-time failure that would stop the service starting at all. The project
requirement to survive "iCUE SDK DLL is missing" falls out of the design rather
than needing a special case.

## What the user must install

**Corsair iCUE**, from <https://www.corsair.com/icue>. That is the whole
requirement -- iCUE ships the SDK DLL as part of a normal installation, so
there is no separate SDK download.

SDK access must also be enabled in iCUE's own settings (it is by default). If
it has been turned off, `CorsairConnect` fails with `CE_NotConnected` and
`CorsairProbe` will say so.

## Where the SDK is looked for

`IcueSdkBackend::CandidateSdkPaths()`, in order:

1. `%CORSAIR_ICUE_SDK_PATH%` -- either the folder holding the DLL, or the DLL
   itself. For administrators and developers pointing at a specific build.
2. Beside `CorsairLampArrayService.exe`.
3. `%ProgramFiles%\Corsair\CORSAIR iCUE 5 Software\`, then the iCUE 4 and
   legacy paths, and the same under `%ProgramFiles(x86)%`.
4. The bare filename, letting the loader use its normal search order.

The DLL is named for the architecture and the MSVC toolset it was built with,
not the OS: `iCUESDK.x64_2019.dll` on x64, `iCUESDK.arm64_2019.dll` on ARM64.

`CorsairProbe` prints this list with a found/missing marker for each entry,
which is the fastest way to diagnose a machine where iCUE is installed
somewhere unusual.

## The API that is actually used

From iCUE SDK v4:

| Function | Used for |
|---|---|
| `CorsairConnect` | connect, and register the session-state callback |
| `CorsairDisconnect` | shutdown |
| `CorsairGetDevices` | enumerate devices |
| `CorsairGetDeviceInfo` | per-device details |
| `CorsairGetLedPositions` | LED ids and physical positions |
| `CorsairSubscribeForEvents` | device connect/disconnect |
| `CorsairSetLedColorsBuffer` + `...FlushBufferAsync` | the frame path |
| `CorsairSetLedColors` | fallback when the buffered pair is unavailable |
| `CorsairSetLayerPriority` | layering above iCUE's own effects |
| `CorsairRequestControl` / `CorsairReleaseControl` | access level |

Only `CorsairConnect`, `CorsairGetDevices`, `CorsairGetLedPositions` and
`CorsairSetLedColors` are treated as required. If an older SDK is missing any
of the others, the service logs it and carries on with reduced behaviour rather
than refusing to run.

### The buffered write path

`CorsairSetLedColors` is synchronous: it blocks on IPC to iCUE. Putting that in
the frame path would serialise a 60Hz update loop behind the round trip. The
service therefore prefers `CorsairSetLedColorsBuffer` followed by
`CorsairSetLedColorsFlushBufferAsync`, which stages the write and returns
immediately.

No completion callback is registered for the flush. The next frame supersedes
this one regardless, so waiting for confirmation would reintroduce exactly the
latency the buffered path exists to remove.

## Shared control, not exclusive

`CorsairRequestControl` offers shared and exclusive access levels. **This
project uses shared by default.**

iCUE runs at layer priority 127 and shared SDK clients default to 128, so our
output already draws above iCUE's own effects without evicting them. Taking
`CAL_ExclusiveLightingControl` would silently disable the user's iCUE profiles
for as long as the service runs -- not a decision a background bridge should
make on their behalf. `--exclusive` is available for anyone who wants it.

When Windows re-enables autonomous mode, the service calls
`CorsairReleaseControl`, which lets the user's own iCUE profile resume. That is
the closest analogue to a device "returning to its own effect".

## LED position units -- the subtle one

The SDK documents `CorsairLedPosition::cx/cy` as:

> "for keyboards, mice, mousemats, headset stands and memory modules values are
> in mm, for DIY-devices, headsets and coolers values are in logical units"

Logical units have no physical scale. Treating them as millimetres does not
fail -- it silently produces a bounding box off by orders of magnitude, and
Windows then positions effects on a device it believes is either microscopic or
metres across. Nothing errors; the lighting is just wrong in a way that is hard
to attribute.

`IcueSdkBackend::PositionUnitsForType` therefore classifies each device type,
and the mapper scales logical-unit devices onto a nominal physical envelope for
their kind, using one factor for both axes so the layout is not distorted.

## Failure modes

| Situation | Behaviour |
|---|---|
| iCUE not installed | SDK not found; the service logs it and retries every 5s |
| iCUE installed, not running | `CorsairConnect` succeeds, session never connects; devices marked unavailable |
| SDK access disabled in iCUE | `CE_NotConnected`; same handling |
| iCUE exits while running | session-state callback fires; devices marked unavailable, virtual devices retained |
| iCUE restarts | callback fires again; the service re-enumerates and resumes. No reboot, no reinstall |
| device unplugged | device event; that LampArray is marked unavailable |
| device with zero LEDs | skipped with a log line -- not an error |
| SDK write fails | counted, the pipeline's shadow is invalidated so the next frame is resent in full, and logged on the first failure and every 600th thereafter |

That last detail matters: without invalidating the shadow, dirty-lamp tracking
would suppress the retry and the device would stay wrong until something else
happened to change.

## Testing without iCUE

`ICorsairBackend` names no Corsair type and includes no SDK header, so
`FakeBackend` can stand in for the real thing. It simulates devices, session
loss and restoration, hotplug and write failures, and `tests/CorsairTests` runs
the whole service-side path against it on any OS.

For the real SDK, `CorsairProbe` is the manual counterpart -- run it on a
machine with iCUE and Corsair hardware before trusting anything else.
