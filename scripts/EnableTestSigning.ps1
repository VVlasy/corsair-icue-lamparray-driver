<#
.SYNOPSIS
    Enables Windows test signing so an unsigned development build of
    CorsairLampArray.sys can load.

.DESCRIPTION
    Windows will not load a kernel driver that is not signed by a certificate
    chaining to a Microsoft-trusted root.  During development the driver is
    signed with a self-generated test certificate, which requires the machine
    to be in test-signing mode.

    THIS IS A DEVELOPMENT-ONLY STEP.  Test signing lowers a real security
    boundary: while it is on, any driver signed by any test certificate on the
    machine can load.  Release builds must be Microsoft-attested instead, so
    users never have to do this -- see docs/signing.md.

    A reboot is required.  Windows displays a "Test Mode" watermark on the
    desktop while this is active.

    On a machine with Secure Boot enabled this will not take effect until
    Secure Boot is disabled in firmware, because Secure Boot enforces the
    signing policy independently.

.EXAMPLE
    .\EnableTestSigning.ps1
#>
[CmdletBinding(SupportsShouldProcess = $true, ConfirmImpact = 'High')]
param(
    [switch]$Force
)

$ErrorActionPreference = 'Stop'

function Test-Administrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

if (-not (Test-Administrator)) {
    throw 'This script must be run from an elevated PowerShell session.'
}

Write-Host ''
Write-Host 'Enabling Windows test signing' -ForegroundColor Cyan
Write-Host '-----------------------------'
Write-Host ''
Write-Host 'This lowers a security boundary on this machine: while test signing' -ForegroundColor Yellow
Write-Host 'is enabled, any driver signed with any test certificate can load.' -ForegroundColor Yellow
Write-Host 'It is intended for development machines only.' -ForegroundColor Yellow
Write-Host ''

$secureBoot = $null
try {
    $secureBoot = Confirm-SecureBootUEFI
} catch {
    # Not a UEFI machine, or the cmdlet is unavailable; nothing to report.
}

if ($secureBoot -eq $true) {
    Write-Host 'Secure Boot is ENABLED on this machine.' -ForegroundColor Yellow
    Write-Host 'Test signing will not take effect until Secure Boot is turned off' -ForegroundColor Yellow
    Write-Host 'in the firmware settings.' -ForegroundColor Yellow
    Write-Host ''
}

if (-not $Force -and -not $PSCmdlet.ShouldProcess($env:COMPUTERNAME, 'Enable test signing (requires reboot)')) {
    Write-Host 'Cancelled.'
    return
}

& bcdedit.exe /set testsigning on
if ($LASTEXITCODE -ne 0) {
    throw "bcdedit failed with exit code $LASTEXITCODE"
}

Write-Host ''
Write-Host 'Test signing enabled. Reboot for it to take effect.' -ForegroundColor Green
Write-Host 'Run DisableTestSigning.ps1 when you are finished developing.'
