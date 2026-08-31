<#
.SYNOPSIS
    Installs the CorsairLampArray driver package and creates its root device.

.DESCRIPTION
    Two steps are needed, and only doing the first is the usual mistake:

      1. Stage the driver package into the driver store (pnputil /add-driver).
         This makes the driver available but creates nothing.
      2. Create the root-enumerated device node that the driver binds to.
         Without this there is no device, so the driver never loads and the
         service finds no interface to open.

    Step 2 uses pnputil /add-driver with /install where available (Windows 10
    2004 and later), falling back to devcon if present.

    Requires an elevated session.  For a development build the machine must
    also be in test-signing mode -- see EnableTestSigning.ps1 and
    docs/signing.md.

.PARAMETER InfPath
    Path to CorsairLampArray.inf.  Defaults to the build output.

.EXAMPLE
    .\InstallDriver.ps1
    .\InstallDriver.ps1 -InfPath ..\build\driver\x64\Debug\CorsairLampArray\CorsairLampArray.inf
#>
[CmdletBinding()]
param(
    [string]$InfPath
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

$repositoryRoot = Split-Path -Parent $PSScriptRoot

if (-not $InfPath) {
    $candidates = @(
        "$repositoryRoot\build\driver\x64\Release\CorsairLampArray\CorsairLampArray.inf",
        "$repositoryRoot\build\driver\x64\Debug\CorsairLampArray\CorsairLampArray.inf",
        "$repositoryRoot\build\driver\x64\Release\CorsairLampArray.inf",
        "$repositoryRoot\build\driver\x64\Debug\CorsairLampArray.inf"
    )
    $InfPath = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
}

if (-not $InfPath -or -not (Test-Path $InfPath)) {
    Write-Host 'Could not find CorsairLampArray.inf.' -ForegroundColor Red
    Write-Host 'Build the driver first (see docs/development.md), or pass -InfPath.'
    throw 'driver package not found'
}

$InfPath = (Resolve-Path $InfPath).Path
Write-Host "Driver package: $InfPath" -ForegroundColor Cyan

$signing = (& bcdedit.exe /enum '{current}' | Select-String -Pattern 'testsigning\s+Yes' -Quiet)
if (-not $signing) {
    Write-Host ''
    Write-Host 'Test signing does not appear to be enabled.' -ForegroundColor Yellow
    Write-Host 'A development build will fail to load without it. If this is a' -ForegroundColor Yellow
    Write-Host 'Microsoft-attested release package, ignore this warning.' -ForegroundColor Yellow
    Write-Host ''
}

Write-Host 'Staging the driver package...' -ForegroundColor Cyan
& pnputil.exe /add-driver $InfPath /install
if ($LASTEXITCODE -ne 0) {
    throw "pnputil /add-driver failed with exit code $LASTEXITCODE"
}

# Creating the root device node. pnputil gained /add-device in Windows 10 2004;
# devcon is the fallback on older builds and in WDK-equipped environments.
$hardwareId = 'ROOT\CorsairLampArray'
$existing = Get-PnpDevice -ErrorAction SilentlyContinue |
    Where-Object { $_.HardwareID -contains $hardwareId }

if ($existing) {
    Write-Host "Device node already present ($($existing.Status))." -ForegroundColor Green
} else {
    Write-Host 'Creating the root device node...' -ForegroundColor Cyan

    $created = $false
    $pnputilHelp = & pnputil.exe /? 2>&1 | Out-String
    if ($pnputilHelp -match '/add-device') {
        & pnputil.exe /add-device $hardwareId
        if ($LASTEXITCODE -eq 0) { $created = $true }
    }

    if (-not $created) {
        $devcon = Get-Command devcon.exe -ErrorAction SilentlyContinue
        if ($devcon) {
            & $devcon.Source install $InfPath $hardwareId
            if ($LASTEXITCODE -eq 0) { $created = $true }
        }
    }

    if (-not $created) {
        Write-Host ''
        Write-Host 'Could not create the device node automatically.' -ForegroundColor Red
        Write-Host 'Create it by hand in Device Manager:' -ForegroundColor Yellow
        Write-Host '  Action > Add legacy hardware > Install manually >' -ForegroundColor Yellow
        Write-Host '  Show All Devices > Have Disk > select CorsairLampArray.inf' -ForegroundColor Yellow
        throw 'device node not created'
    }
}

Write-Host ''
Write-Host 'Driver installed.' -ForegroundColor Green
Write-Host 'Next: install and start the service, then check with HidTestClient.'
Write-Host '  CorsairLampArrayService.exe --install'
Write-Host '  sc start CorsairLampArrayService'
