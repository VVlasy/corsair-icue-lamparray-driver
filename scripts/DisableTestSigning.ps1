<#
.SYNOPSIS
    Disables Windows test signing, restoring the normal driver signing policy.

.DESCRIPTION
    Undoes EnableTestSigning.ps1.  Run this when you have finished development
    work: leaving test signing on permanently weakens the machine's driver
    signing guarantees.

    A reboot is required.  Any test-signed driver still installed will stop
    loading after the reboot -- uninstall the development driver first with
    UninstallDriver.ps1 if you want a clean state.

.EXAMPLE
    .\DisableTestSigning.ps1
#>
[CmdletBinding(SupportsShouldProcess = $true)]
param()

$ErrorActionPreference = 'Stop'

function Test-Administrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

if (-not (Test-Administrator)) {
    throw 'This script must be run from an elevated PowerShell session.'
}

if (-not $PSCmdlet.ShouldProcess($env:COMPUTERNAME, 'Disable test signing (requires reboot)')) {
    Write-Host 'Cancelled.'
    return
}

& bcdedit.exe /set testsigning off
if ($LASTEXITCODE -ne 0) {
    throw "bcdedit failed with exit code $LASTEXITCODE"
}

Write-Host ''
Write-Host 'Test signing disabled. Reboot for it to take effect.' -ForegroundColor Green
Write-Host 'Any test-signed driver will stop loading after the reboot.' -ForegroundColor Yellow
