# Gives the Wi-Fi card back to the MediaTek driver. Safe to run at any time, any number of times.
#
#   1. removes every mtdrop proxy driver package from the driver store and from the device,
#   2. rescans devices so Windows binds the vendor driver again,
#   3. if that does not happen, reinstalls the vendor driver from the backup folder,
#   4. re-adds saved Wi-Fi networks if the adapter came back without them.
#
# Must run elevated. Usage:  .\scripts\wifi-giveback.ps1 [-RemoveCertificate]
# Last resort if this fails: Device Manager > the MT7921 device > Update driver > Browse >
#   %USERPROFILE%\mtdrop-private\vendor-driver-backup ; or System Restore point "mtdrop: before driver work".

param(
    [switch]$RemoveCertificate,
    [string]$PrivateRoot = (Join-Path $env:USERPROFILE 'mtdrop-private'),
    [int]$WaitSeconds = 40
)

$ErrorActionPreference = 'Stop'
$HardwareMatch = 'PCI\VEN_14C3&DEV_7961*'
$OurInf = 'mtdrop_proxy.inf'
$CertSubject = 'CN=mtdrop local driver signing'
$log = Join-Path $PrivateRoot 'driver-swap.log'
New-Item -ItemType Directory -Force -Path $PrivateRoot | Out-Null
function Say([string]$m) { $line = "{0:yyyy-MM-dd HH:mm:ss} [giveback] {1}" -f (Get-Date), $m; Add-Content -Path $log -Value $line; Write-Output $line }

function Get-CardDriver {
    Get-CimInstance Win32_PnPSignedDriver | Where-Object { $_.DeviceID -like $HardwareMatch } | Select-Object -First 1
}
function Get-OurPackages {
    # pnputil can print XML, which does not depend on the display language.
    $xml = [xml]((& pnputil.exe /enum-drivers /format xml) -join "`n")
    @($xml.SelectNodes('//Driver') | Where-Object { $_.OriginalName -eq $OurInf } | ForEach-Object { $_.DriverName })
}
function Test-VendorBound($d) { $d -and $d.DriverProviderName -like 'MediaTek*' -and $d.InfName -notin (Get-OurPackages) }

$failed = $false
try {
    $identity = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
    if (-not $identity.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Not elevated.' }

    $d = Get-CardDriver
    Say ("card driver now: {0} / {1} / {2}" -f $d.DriverProviderName, $d.InfName, $d.DriverVersion)

    foreach ($package in Get-OurPackages) {
        $out = & pnputil.exe /delete-driver $package /uninstall /force 2>&1
        Say "pnputil /delete-driver $package /uninstall /force -> exit $LASTEXITCODE"
        if ($LASTEXITCODE -eq 3010) { Say 'Windows asks for a restart to finish removing the proxy driver' }
        elseif ($LASTEXITCODE -ne 0) { $out | ForEach-Object { Say "  $_" } }
    }
    & pnputil.exe /scan-devices | Out-Null
    Say 'pnputil /scan-devices done'

    $deadline = (Get-Date).AddSeconds($WaitSeconds)
    do { Start-Sleep -Milliseconds 700; $d = Get-CardDriver } until ((Test-VendorBound $d) -or (Get-Date) -gt $deadline)

    if (-not (Test-VendorBound $d)) {
        Say 'vendor driver did not come back by itself; installing it from the backup'
        $inf = Get-ChildItem (Join-Path $PrivateRoot 'vendor-driver-backup') -Recurse -Filter 'mtkwl6ex.inf' | Select-Object -First 1
        if (-not $inf) { throw 'backup of the vendor driver not found' }
        $out = & pnputil.exe /add-driver $inf.FullName /install 2>&1
        Say "pnputil /add-driver (backup) /install -> exit $LASTEXITCODE"
        & pnputil.exe /scan-devices | Out-Null
        $deadline = (Get-Date).AddSeconds($WaitSeconds)
        do { Start-Sleep -Milliseconds 700; $d = Get-CardDriver } until ((Test-VendorBound $d) -or (Get-Date) -gt $deadline)
    }
    if (-not (Test-VendorBound $d)) { throw 'the MediaTek driver is still not bound to the card' }
    Say ("card driver back: {0} / {1} / {2}" -f $d.DriverProviderName, $d.InfName, $d.DriverVersion)

    # Saved networks are stored per adapter instance; a re-created adapter can come back without them.
    $profiles = Join-Path $PrivateRoot 'wifi-profiles'
    Start-Sleep -Seconds 3
    if (Test-Path $profiles) {
        $known = (& netsh.exe wlan show profiles 2>$null) -join "`n"
        $missing = 0
        foreach ($f in Get-ChildItem $profiles -Filter '*.xml') {
            $name = ([xml](Get-Content $f.FullName -Raw)).WLANProfile.name
            if ($name -and $known.Contains($name)) { continue }
            $missing++
            & netsh.exe wlan add profile "filename=$($f.FullName)" user=all 2>&1 | Out-Null
            Say "re-added one saved network (netsh exit $LASTEXITCODE)"
        }
        if ($missing -eq 0) { Say 'saved Wi-Fi networks are all present' }
    }
    $deadline = (Get-Date).AddSeconds(30)
    do {
        Start-Sleep -Seconds 2
        $up = Get-NetAdapter -Physical -ErrorAction SilentlyContinue | Where-Object { $_.InterfaceDescription -like '*MT7921*' -and $_.Status -eq 'Up' }
    } until ($up -or (Get-Date) -gt $deadline)
    if ($up) { Say "Wi-Fi adapter '$($up.Name)' is connected" } else { Say 'Wi-Fi adapter is present but not connected yet (connect manually if it does not reconnect)' }
}
catch {
    $failed = $true
    Say "FAILED: $($_.Exception.Message)"
    Say 'Manual recovery: Device Manager > MT7921 device > Update driver > Browse > mtdrop-private\vendor-driver-backup'
}

if ($RemoveCertificate) {
    foreach ($store in 'Root', 'TrustedPublisher') {
        Get-ChildItem "Cert:\LocalMachine\$store" | Where-Object { $_.Subject -eq $CertSubject } | ForEach-Object { Remove-Item $_.PSPath; Say "removed signing certificate from LocalMachine\$store" }
    }
}
if ($failed) { exit 1 }
Say 'RESULT: OK'
