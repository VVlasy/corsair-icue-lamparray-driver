# Driver signing

Windows will not load a kernel driver unless it is signed by a certificate
chaining to a Microsoft-trusted root. There is no way around this on a normal
64-bit Windows installation, and there should not be.

## Development: test signing

For development the driver is signed with a self-generated test certificate,
which requires the machine to be in test-signing mode.

```powershell
.\scripts\EnableTestSigning.ps1     # reboot afterwards
# ... develop ...
.\scripts\DisableTestSigning.ps1    # reboot afterwards
```

**Understand what this does before running it.** Test signing lowers a real
security boundary: while it is on, *any* driver signed by *any* test
certificate in the machine's store can load. That is acceptable on a
development VM and not acceptable on a machine you care about.

Windows shows a "Test Mode" watermark on the desktop while it is active, which
is a feature rather than a nuisance -- it is hard to leave a machine in this
state by accident.

On a machine with **Secure Boot** enabled, test signing has no effect until
Secure Boot is disabled in firmware, because Secure Boot enforces the signing
policy independently of the boot configuration.

This is exactly why test signing must not be part of the release story. Asking
users to disable Secure Boot and enable test mode to get RGB lighting would be
asking them to trade a meaningful security guarantee for a cosmetic feature.

## Creating a test certificate

The WDK does this automatically for a Debug build. To do it by hand:

```powershell
$cert = New-SelfSignedCertificate `
    -Type CodeSigningCert `
    -Subject "CN=Corsair Dynamic Lighting Bridge Test" `
    -CertStoreLocation Cert:\CurrentUser\My `
    -KeyUsage DigitalSignature `
    -TextExtension @("2.5.29.37={text}1.3.6.1.5.5.7.3.3")

# Trust it locally, for both code signing and driver package verification.
Export-Certificate -Cert $cert -FilePath test.cer
Import-Certificate -FilePath test.cer -CertStoreLocation Cert:\LocalMachine\Root
Import-Certificate -FilePath test.cer -CertStoreLocation Cert:\LocalMachine\TrustedPublisher
```

Sign the built package:

```powershell
signtool sign /v /fd sha256 /s My /n "Corsair Dynamic Lighting Bridge Test" `
    /t http://timestamp.digicert.com CorsairLampArray.sys

# The catalogue must be signed too, or the INF install will be rejected.
inf2cat /driver:. /os:10_X64
signtool sign /v /fd sha256 /s My /n "Corsair Dynamic Lighting Bridge Test" `
    CorsairLampArray.cat
```

Both the `.sys` and the `.cat` need signing. Signing only the binary produces
an install that fails with a signature error that does not obviously point at
the catalogue.

## Release: Microsoft attestation signing

For a release, the package is submitted to the **Microsoft Hardware
Developer Program** for attestation signing. The result installs on a normal
machine with Secure Boot on and no test mode, which is the only acceptable
outcome for something end users install.

What is required:

1. **A Partner Center account** with hardware developer access.
2. **An EV code signing certificate**, which is what the account is validated
   against. This is the part with real cost and lead time: EV certificates
   require organisational identity validation and are typically issued on a
   hardware token.
3. **A signed `.cab`** containing the driver package, submitted through the
   Hardware Dashboard.
4. Microsoft returns the attestation-signed package.

Attestation signing covers Windows 10 and 11 on x64 and ARM64. It does not
require passing the HLK test suite -- that is only needed for a WHQL logo,
which this project does not need.

### Things worth knowing before submitting

- The submitted package must contain the `.sys`, `.inf` and `.cat` **produced
  together**. Re-signing a catalogue against a changed INF invalidates it.
- `PnpLockdown=1` is set in the INF (it is required for universal packages) and
  makes the driver's files immutable in the driver store once installed.
- The INF must pass `InfVerif` in universal mode. The project's INF is written
  to that standard.
- Attestation signing is per-package. Every driver change means a new
  submission, so the driver is deliberately the part of this project that
  changes least: the protocol logic it contains is shared with the service and
  the tests and can be modified without a resubmission only insofar as the
  driver's own copy is rebuilt with it.

## Why the driver is small

Attestation is the reason the driver contains no Corsair code, no SDK, and
nothing that changes often. The parts of this project most likely to need
updating -- device mapping, iCUE quirks, reconnect behaviour, performance
tuning -- all live in the service, which is an ordinary user-mode binary that
can be updated without a signing round trip.
