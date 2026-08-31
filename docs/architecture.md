# Architecture

## The stack

```
  Windows Settings > Personalization > Dynamic Lighting
  Windows.Devices.Lights, and any Dynamic Lighting-aware app
                    |
                    |  HID feature reports, usage page 0x59, report ids 1-6
                    v
  Hidclass.sys / Mshidkmdf.sys
                    |
                    v
  Vhf.sys                          (Microsoft, in-box; our INF adds it as a
                    |               lower filter)
                    v
  CorsairLampArray.sys             (this project; root-enumerated KMDF driver)
                    |
                    |  IOCTLs on GUID_DEVINTERFACE_CORSAIR_LAMP_ARRAY
                    v
  CorsairLampArrayService.exe      (this project; LocalSystem)
                    |
                    |  LoadLibrary / GetProcAddress at run time
                    v
  iCUESDK.x64_2019.dll             (Corsair; from the user's iCUE install)
                    |
                    v
  iCUE  ->  Corsair hardware
```

## The split, and why it is where it is

The brief required the kernel component to know nothing about Corsair, and
that constraint turns out to be the right one for reasons beyond tidiness:

- **The iCUE SDK is user-mode, closed-source and not redistributable.** It
  could not be linked into a driver even if that were desirable.
- **A crash in the lighting backend must not be a bugcheck.** iCUE
  disappearing, an SDK call hanging, a device vanishing mid-write are all
  ordinary events. In user mode they are a logged warning; in kernel mode the
  cheapest of them is a machine reboot.
- **The driver is the part that must be signed.** Keeping it small and free of
  third-party code keeps the attested package small and its dependencies
  auditable.

So the driver is a HID LampArray transport with an IPC channel, and everything
Corsair-specific is in the service.

## Who owns what

| Concern | Driver | Service |
|---|---|---|
| HID report descriptor | yes | - |
| Parsing and validating host reports | yes | - |
| Lamp state and frame assembly | yes | - |
| Frame coalescing (newest wins) | yes | - |
| Corsair topology and LED positions | - | yes |
| Mapping LEDs to lamps | - | yes |
| Dirty-lamp tracking, throttling, brightness | - | yes |
| Talking to iCUE | - | yes |
| Reconnect and hotplug policy | - | yes |

## The lamp table flows down once

The service is the authority on what a Corsair device looks like, so at
creation it pushes the **complete, immutable lamp table** into the driver in a
single `IOCTL_CLA_CREATE_LAMP_ARRAY`. The driver then answers every
`LampArrayAttributesReport` and `LampAttributesResponseReport` from its own
copy.

This is allowed because HUT 1.4 §26.8.3 says lamp attributes are static and can
never change. It buys three things:

1. The HID path never blocks on a user-mode round trip.
2. Windows can still interrogate the device while the service is restarting.
3. A slow or wedged service degrades lighting, not enumeration.

## Frames flow up on an inverted call

Lamp *updates* are the only thing that travels the other way. The service
parks several `IOCTL_CLA_GET_NEXT_UPDATE` requests on the driver; the driver
completes one whenever Windows finishes a frame.

The driver keeps **exactly one pending frame per device**. If Windows completes
another before the service collects the first, the newer one overwrites it and
a counter is bumped. This is not a compromise forced by buffer size: for RGB
output a superseded frame has no value, and delivering it would only add
latency to a picture nobody will see. Queueing frames would make the device
laggy under load, which is precisely the failure mode users notice.

The service keeps **four** requests outstanding rather than one. With a single
request there is a window after every completion during which the driver has
nowhere to deliver a frame and has to hold it until the service re-arms --
which at 60Hz is a frame of latency on every single update.

## Threading and locking

| Context | IRQL / thread | Lock |
|---|---|---|
| VHF `GetFeature` / `SetFeature` | <= DISPATCH_LEVEL | per-array `WDFSPINLOCK` |
| IOCTL handlers | PASSIVE_LEVEL | device `WDFWAITLOCK`, then the spinlock |
| Service update pump | dedicated thread | `DeviceManager::m_mutex` |
| Service poll loop | main thread | same |

Rules that follow, and are not varied anywhere:

- Nothing that can block -- pool allocation, `VhfCreate`/`VhfDelete`, request
  completion -- happens while the spinlock is held.
- The wait lock is never taken while holding the spinlock, so there is no
  inversion.
- Every buffer the VHF callbacks touch is `NonPagedPoolNx`: a page fault at
  DISPATCH_LEVEL is a bugcheck.
- The service never calls the backend while holding its own mutex. An SDK write
  can block on IPC to iCUE, and holding the lock across it would stall every
  other device's frames as well as device discovery.

## Trust boundary

The service is an ordinary user-mode process, so the driver treats everything
it sends as hostile. Every IOCTL handler follows the same shape:

1. `WdfRequestRetrieveInput/OutputBuffer` with a minimum length, so the
   framework rejects anything too small to hold a header.
2. `ClaValidate*` from the core: version, declared size against *actual*
   length, and any embedded counts -- with the arithmetic in 64 bits so a
   hostile count cannot overflow into a small "expected size" that a short
   buffer then satisfies.
3. Only then read the body.

All IOCTLs are `METHOD_BUFFERED`, so the driver works on a system buffer the
framework copied and never dereferences a user-mode address or races a caller
mutating data after validation.

The device interface is additionally restricted by SDDL in the INF to SYSTEM
and Administrators (`D:P(A;;GA;;;SY)(A;;GA;;;BA)`). That is defence in depth,
not the primary control: the validation above has to hold regardless.

Because this validation is the driver's whole attack surface, it lives in the
portable core and is unit tested -- against truncation at every length, lying
size fields, integer-overflow lamp counts, unterminated strings and randomised
buffers, under AddressSanitizer. See `tests/IpcTests`.

## Multiple devices

One virtual LampArray per Corsair device, each its own `VhfCreate` handle with
its own `InstanceID` and `ContainerID`.

They are deliberately not merged. A keyboard and a mouse are not one LampArray:
merging them would mean handing Windows a single bounding box describing no
physical object, and effects positioned within it would land nowhere real. The
HID model treats a LampArray as one physical device and Windows presents them
individually in Settings, so the mapping is one-to-one.

`ContainerID` is derived deterministically from the Corsair device id rather
than generated randomly, because Windows keys per-device Dynamic Lighting
settings off the container. A fresh GUID each run would make the user's choices
evaporate on every service restart.

## Device lifecycle

A virtual LampArray is created once per Corsair device and is **not** destroyed
when the device or iCUE goes away -- it is marked unavailable instead, and
destroyed only when the service stops. The requirement was that iCUE can be
restarted without a reboot and that hotplug must not create duplicates; keeping
a stable virtual device keyed by the Corsair device id delivers both, and
avoids the device flickering in and out of the Settings list every time iCUE
updates itself.

## VID/PID

The virtual devices report **0x1209 / 0x4C41** by default.

This is a placeholder and is not a USB-IF allocation. 0x1209 is the
[pid.codes](https://pid.codes) community vendor ID for open-source hardware;
0x4C41 is "LA" in ASCII. It is configurable via `DeviceManagerOptions`.

Using a real Corsair VID would be worse than arbitrary: it would make the
virtual device claim to be hardware it is not, to any software that inspects
it. The devices are virtual and never touch USB, so the identifiers are
informational.
