# Installer

The MVP ships as scripts rather than a packaged installer, because the
installation is mostly PnP driver staging and a service registration, and
doing that from PowerShell keeps every step visible and reversible while the
project is still being brought up on real hardware.

## What a full install does

1. **Stage the driver package** into the driver store (`pnputil /add-driver`).
2. **Create the root-enumerated device node** (`ROOT\CorsairLampArray`). This
   is the step that actually causes the driver to load; staging alone creates
   nothing, which is the most common reason a first install appears to do
   nothing at all.
3. **Register the service** (`CorsairLampArrayService.exe --install`), which
   installs it as LocalSystem with restart-on-failure actions.
4. **Start the service**, which connects to iCUE, enumerates devices and asks
   the driver to create one virtual LampArray per Corsair device.

`scripts/InstallDriver.ps1` does steps 1 and 2; the service binary does 3.
`scripts/UninstallDriver.ps1` reverses 1 and 2, and
`CorsairLampArrayService.exe --uninstall` reverses 3.

## Signing

The driver will not load unless it is signed. During development that means
test signing (`scripts/EnableTestSigning.ps1`), which lowers a real security
boundary and is why it is not something to ask users to do.

A release build must instead be submitted for **Microsoft attestation
signing**, after which the package installs on a normal machine with Secure
Boot on and no test mode. See `docs/signing.md`.

## Packaging this later

If this grows into a packaged installer, the pieces to be aware of:

- The driver package must remain a **single directory** containing the `.sys`,
  `.inf` and `.cat` produced together. Mixing a re-signed catalogue with a
  changed INF invalidates the signature.
- The device node creation is not idempotent in the obvious way: re-running it
  can create a *second* root device, which would present duplicate LampArrays
  to Windows. `InstallDriver.ps1` checks for an existing node first, and any
  installer must do the same.
- Uninstall order matters: stop the service, then remove the device node, then
  delete the driver package. Removing the package while the device is live
  leaves an orphaned node that Windows will keep trying to start.
