# LampArray behaviour

Everything here is specified by [USB HID Usage Tables
1.4](https://www.usb.org/sites/default/files/hut1_4.pdf) section 26. Section
numbers are cited so each rule can be checked. The implementation is
`core/src/LampArrayDevice.cpp` and the tests are `tests/HidTests`.

## The interrogation dance

A host brings a LampArray up like this (s26.6):

```
Host                                                     Device
  |--- GET   LampArrayAttributesReport ------------------>|
  |<-- lamp count, bounding box, kind, min interval ------|
  |                                                       |
  |--- SET   LampAttributesRequestReport(LampId=0) ------>|
  |--- GET   LampAttributesResponseReport --------------->|
  |<-- attributes for lamp 0 -----------------------------|
  |--- GET   LampAttributesResponseReport --------------->|
  |<-- attributes for lamp 1  (auto-incremented) ---------|
  |    ... repeated for every lamp ...                    |
  |                                                       |
  |--- SET   LampArrayControlReport(AutonomousMode=0) --->|
  |--- SET   Lamp*UpdateReport(Complete=0) -------------->|
  |--- SET   Lamp*UpdateReport(Complete=1) -------------->|  <- frame applied
  |    ... lighting ...                                   |
  |--- SET   LampArrayControlReport(AutonomousMode=1) --->|
```

### The cursor auto-increments and wraps

s26.8.2: after a successful response the device increments its internal
`LampId`, and after `LampCount-1` it **resets to 0**. A host need only send one
request report and can then read responses in a loop.

Microsoft's RP2040 sample clamps at the last lamp instead of wrapping. The
specification says wrap; this project follows the specification, and
`LampAttributes_AutoIncrementsAndWraps` pins the behaviour.

The specification also tells the host to check the `LampId` in each response,
because an invalid request is silently treated as lamp 0 (s26.8.1). Both
`HidTestClient` and the test suite do check.

## Update reports are all-or-nothing

s26.11.1 and s26.11.2 list errors that make the device ignore **the entire
report**:

| Error | Applies to |
|---|---|
| `LampId >= LampCount` | multi-update |
| duplicate `LampId` in two slots | multi-update |
| channel value above that lamp's `*LevelCount` | both |
| `LampIdStart > LampIdEnd` | range update |
| `LampIdStart` or `LampIdEnd >= LampCount` | range update |

"Entire" is the important word. A multi-update whose eighth slot is bad must
not apply the first seven. `ClaHandleMultiUpdate` therefore validates every
slot into a local array *before* mutating any lamp state, and
`MultiUpdate_OutOfRangeLampIdVoidsWholeReport` asserts that lamp 0 is untouched
when slot 1 is invalid.

Because the specification requires this rejection to be **silent**, the only
evidence a report was dropped is a counter. The core keeps per-cause counters,
the driver exposes them through `IOCTL_CLA_GET_STATISTICS`, and
`HidTestClient --driver` prints them. If lighting is not working, that is the
first place to look.

### The duplicate-LampId rule has an escape hatch

Rejecting a whole frame over a duplicate id would produce a visible glitch if a
host ever violated this rule. `ClaLampArrayState::StrictDuplicateLampIdCheck`
defaults to on, per the specification, but can be turned off to make duplicates
last-writer-wins instead. It exists only in case a real host is found to
misbehave; nothing sets it today.

## Colour levels, and why everything is 255

s26.9: a `*LevelCount` of N means values 0..N are legal, where 0 is off and N
is maximum. `RedLevelCount = 255` therefore means the full 8-bit range.

Combined with the rule above, this has a consequence worth stating on its own,
because getting it wrong yields a device that looks correct and silently does
nothing:

> If a lamp declared `IntensityLevelCount = 1` and Windows sent intensity 255,
> s26.11.1 would require the device to **discard the entire report**.

Microsoft's RP2040 sample declares `IntensityLevelCount = 1` and gets away with
it only because it performs no validation at all. A spec-conformant device that
declared 1 would drop frames.

This project declares **255 on all four channels of every lamp**
(`CorsairMapping.cpp`). That is both safe -- no colour Windows can express is
ever out of range -- and accurate, since Corsair LEDs are 8-bit RGB.

## Fixed-colour lamps

s26.11.1: for a lamp with `IsProgrammable == 0`, only the intensity channel is
examined; R, G and B are always ignored. `ClaApplyColor` implements this by
preserving the existing RGB and applying only intensity, so a fixed-colour lamp
dims and brightens but never changes hue. Corsair devices are all programmable,
so this path is not exercised in practice, but the state machine is a general
LampArray implementation and honours it.

## Autonomous mode

s26.10.1. A boolean, **default enabled**.

- **Enabled** -- the device may drive its own lamps, and `Lamp*UpdateReports`
  may be ignored.
- **Disabled** -- only the host may change lamp state. The device pauses any
  effect it was running and holds the lamps at whatever was last displayed.

Attribute reports must keep working in either state.

This project maps it onto iCUE control: when Windows enables autonomous mode,
the service calls `ReleaseControl`, handing the device back so the user's own
iCUE profile resumes. When Windows disables it, the service takes control
again. That is the closest available analogue to "the device resumes its own
effect", since our "autonomous" behaviour is whatever iCUE was already doing.

## Frames and batching

s26.11: `LampUpdateComplete` marks the last report of a batch, and the device
applies the accumulated updates as one frame. The host will not send more than
one such report per `MinUpdateIntervalInMicroseconds`.

The driver accumulates into its shadow lamp state and only publishes a frame to
the service when it sees that flag, so a multi-report update never reaches the
hardware half-applied and never causes tearing.

## Geometry

s26.7. The bounding box origin (0,0,0) is the topmost, farthest, left-hand
corner, and every lamp position is a positive offset from it, in micrometres.
Every axis must be positive.

Corsair reports 2D positions only, so lamps are placed at Z = 0 and the box is
given a nominal per-device-kind depth. Degenerate layouts -- one LED, or an LED
strip whose LEDs share a Y -- would otherwise produce a zero-height box, so the
mapper floors each axis at 1mm. See `docs/corsair-sdk.md`.

## LampId ordering

s26.8.1 recommends assigning ids methodically, grid-wise from the top left,
"to take the most advantage of the `LampRangeUpdateReport`".

This is a real performance property, not a stylistic one. With row-major ids, a
host painting a contiguous region can use one range update; with arbitrary ids
it needs one multi-update per eight lamps. The mapper therefore sorts LEDs into
rows (within a tolerance, so slightly misaligned keys still group) and then
left to right within each row, and keeps a `LampId -> LED id` map so the
service can translate frames back.
