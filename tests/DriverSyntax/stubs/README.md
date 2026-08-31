# Stub WDK headers

## What this is

Just enough of `ntddk.h`, `wdf.h`, `vhf.h` and friends to **compile and link
the kernel driver sources with an ordinary C++ compiler**, on any platform.

## What it is for

The driver cannot be built without Visual Studio and the WDK, which means on
most machines -- and on most CI runners -- it is never compiled at all. Code
that is never compiled rots: a typo, a renamed function, a changed signature or
a duplicated symbol sits there until someone with a WDK happens to try.

This harness closes that gap cheaply. It catches:

- syntax errors and typos;
- undeclared identifiers and misspelled API names;
- wrong argument counts and mismatched types;
- functions declared but never defined;
- **duplicate symbols across translation units.**

That last one is not hypothetical: this harness was written after a
`#include <initguid.h>` in a shared header was found to make all five driver
translation units emit their own definition of the device interface GUID, which
would have failed the link on a real WDK build.

## What it is NOT

**This is a syntax and linkage check, not a substitute for a WDK build.** These
stubs approximate the WDK's *shape*, not its behaviour. They cannot tell you
that a WDF object is used correctly, that an IRQL rule is respected, that a
lock is held where it should be, or that the driver works. Only a real WDK
build plus testing on Windows can do that.

The stubs are also not a reimplementation to build against: every function body
is empty or returns a fixed value.

One known, deliberate divergence: `WCHAR` is `wchar_t` here so that `L"..."`
literals compile under GCC, where `wchar_t` is 4 bytes rather than the 2 it is
on Windows. Sizes computed from `WCHAR` therefore differ from the real build,
which is why this harness makes no assertions about them.

## Keeping it accurate

If a stub declaration drifts from the real WDK, this harness will happily
compile something the WDK rejects -- so a signature here should be corrected
against the real header whenever a real build disagrees. The declarations were
transcribed from the Microsoft documentation linked in `docs/research.md`.
