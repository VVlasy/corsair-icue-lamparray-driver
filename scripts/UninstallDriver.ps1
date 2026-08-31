<#
.SYNOPSIS
    Removes the CorsairLampArray device node and driver package.

.DESCRIPTION
    Reverses InstallDriver.ps1: removes the root-enumerated device (which
    unloads the driver and destroys any virtual LampArrays it was hosting) and
    then deletes the driver package from the driver store.

    Stop the service first, or it will be holding a handle to the device.

.EXAMPLE
    .\UninstallDriver.ps1
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

$service = Get-Service -Name 'CorsairLampArrayService' -ErrorAction SilentlyContinue
if ($service -and $service.Status -ne 'Stopped') {
    Write-Host 'Stopping CorsairLampArrayService...' -ForegroundColor Cyan
    Stop-Service -Name 'CorsairLampArrayService' -Force
}

$hardwareId = 'ROOT\CorsairLampArray'
$devices = Get-PnpDevice -ErrorAction SilentlyContinue |
    Where-Object { $_.HardwareID -contains $hardwareId }

foreach ($device in $devices) {
    if ($PSCmdlet.ShouldProcess($device.InstanceId, 'Remove device')) {
        Write-Host "Removing device $($device.InstanceId)..." -ForegroundColor Cyan
        try {
            & pnputil.exe /remove-device $device.InstanceId
        } catch {
            Write-Host "  pnputil /remove-device failed: $_" -ForegroundColor Yellow
        }
    }
}

# Find our staged package(s) in the driver store and delete them.
$published = & pnputil.exe /enum-drivers | Out-String
$blocks = $published -split "(`r?`n){2,}"
$removed = 0

foreach ($block in $blocks) {
    if ($block -match 'CorsairLampArray\.inf' -and
        $block -match 'Published Name:\s*(oem\d+\.inf)') {
        $oemName = $Matches[1]
        if ($PSCmdlet.ShouldProcess($oemName, 'Delete driver package')) {
            Write-Host "Removing driver package $oemName..." -ForegroundColor Cyan
            & pnputil.exe /delete-driver $oemName /uninstall /force
            if ($LASTEXITCODE -eq 0) { $removed++ }
        }
    }
}

if ($removed -eq 0) {
    Write-Host 'No CorsairLampArray driver package was found in the driver store.' -ForegroundColor Yellow
} else {
    Write-Host ''
    Write-Host "Removed $removed driver package(s)." -ForegroundColor Green
}
