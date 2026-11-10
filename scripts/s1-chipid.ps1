# Spike S1 (docs/adr/0002-driver-strategy.md): bind the mtdrop UMDF register proxy to the MT7921,
# read the chip ID, and give the card back to the MediaTek driver.
#
# WHAT THIS CHANGES ON THE MACHINE
#   - Creates a self-signed code-signing certificate and trusts it machine-wide
#     (LocalMachine\Root and LocalMachine\TrustedPublisher). Its private key is deleted right after signing.
#   - Adds the proxy driver package to the driver store and forces it onto the Wi-Fi card.
#     Wi-Fi is DOWN from that moment until the card is given back (expected: under a minute).
#   - Performs two register writes on the chip, both to the host-side remap window register (see regtool).
#
# HOW IT IS UNDONE
#   - Automatically at the end of this script, whatever happens (finally block).
#   - By a watchdog process started before the swap, which runs wifi-giveback.ps1 after -WatchdogSeconds
#     even if this script or its console is killed.
#   - By hand:  .\scripts\wifi-giveback.ps1 -RemoveCertificate   (elevated)
#   - Last resort: Device Manager > MT7921 > Update driver > Browse > mtdrop-private\vendor-driver-backup,
#     or the System Restore point "mtdrop: before driver work".
#
# Must run elevated. Refuses to run without -IUnderstandWifiGoesDown.

param(
    [switch]$IUnderstandWifiGoesDown,
    [string]$PrivateRoot = (Join-Path $env:USERPROFILE 'mtdrop-private'),
    [string]$ToolsRoot = 'D:\mtdrop-tools',
    [int]$WatchdogSeconds = 150
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$HardwareMatch = 'PCI\VEN_14C3&DEV_7961*'
$HardwareId = 'PCI\VEN_14C3&DEV_7961&CC_0280'
$CertSubject = 'CN=mtdrop local driver signing'
$log = Join-Path $PrivateRoot 'driver-swap.log'
$giveback = Join-Path $PSScriptRoot 'wifi-giveback.ps1'
New-Item -ItemType Directory -Force -Path $PrivateRoot | Out-Null
function Say([string]$m) { $line = "{0:yyyy-MM-dd HH:mm:ss} [s1] {1}" -f (Get-Date), $m; Add-Content -Path $log -Value $line; Write-Output $line }
function Get-CardDriver {
    # Live PnP properties. Win32_PnPSignedDriver (CIM) lags behind a driver swap by several seconds (seen in F-004).
    $dev = Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -like $HardwareMatch } | Select-Object -First 1
    if (-not $dev) { return $null }
    $get = { param($k) (Get-PnpDeviceProperty -InstanceId $dev.InstanceId -KeyName $k -ErrorAction SilentlyContinue).Data }
    [pscustomobject]@{
        DeviceID           = $dev.InstanceId
        DriverProviderName = (& $get 'DEVPKEY_Device_DriverProvider')
        InfName            = (& $get 'DEVPKEY_Device_DriverInfPath')
        DriverVersion      = (& $get 'DEVPKEY_Device_DriverVersion')
        Status             = $dev.Status
    }
}

$taken = $false
$watchdog = $null
$exit = 1
try {
    Say '=== S1: chip ID through the UMDF register proxy ==='
    if (-not $IUnderstandWifiGoesDown) { throw 'Refusing to run without -IUnderstandWifiGoesDown.' }
    $identity = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
    if (-not $identity.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Not elevated.' }

    # ---- preconditions (nothing is changed before all of them hold) ----------------------------
    $built = Join-Path $repo 'driver\proxy\x64\Release\mtdrop_proxy'
    $regtool = Join-Path $repo 'driver\tools\Mtdrop.RegTool\bin\Debug\net10.0\regtool.exe'
    $inf2cat = Get-ChildItem (Join-Path $ToolsRoot 'packages') -Recurse -Filter Inf2Cat.exe -ErrorAction SilentlyContinue | Select-Object -First 1
    $builtInf = Join-Path $built 'mtdrop_proxy.inf'                                   # stamped by the build
    $builtDll = Join-Path $repo 'driver\proxy\x64\Release\mtdrop_proxy.dll'
    foreach ($need in $builtInf, $builtDll, $regtool, $giveback) {
        if (-not (Test-Path $need)) { throw "missing: $need (build the proxy and the tool first)" }
    }
    if (-not $inf2cat) { throw 'Inf2Cat.exe not found under the WDK packages' }
    if (-not (Get-ChildItem (Join-Path $PrivateRoot 'vendor-driver-backup') -Recurse -Filter mtkwl6ex.inf -ErrorAction SilentlyContinue)) { throw 'vendor driver backup not found' }
    if (@(Get-ComputerRestorePoint -ErrorAction SilentlyContinue).Count -eq 0) { throw 'no System Restore point' }
    $before = Get-CardDriver
    if (-not $before) { throw 'MT7921 not found' }
    if ($before.DriverProviderName -notlike 'MediaTek*') { throw "card is not on the MediaTek driver now ($($before.DriverProviderName) / $($before.InfName)); run wifi-giveback.ps1 first" }
    Say ("card driver before: {0} / {1} / {2}" -f $before.DriverProviderName, $before.InfName, $before.DriverVersion)

    # ---- stage, catalog, sign ------------------------------------------------------------------
    $stage = Join-Path $ToolsRoot 'stage\proxy'
    if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $stage | Out-Null
    Copy-Item $builtInf, $builtDll $stage
    $out = & $inf2cat.FullName "/driver:$stage" /os:10_X64 /uselocaltime 2>&1
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path (Join-Path $stage 'mtdrop_proxy.cat'))) { $out | ForEach-Object { Say "  inf2cat: $_" }; throw "Inf2Cat failed ($LASTEXITCODE)" }
    Say 'catalog created'

    # Saved Wi-Fi networks (keys stay encrypted for this machine) in case the adapter comes back without them.
    $profiles = Join-Path $PrivateRoot 'wifi-profiles'
    New-Item -ItemType Directory -Force -Path $profiles | Out-Null
    & netsh.exe wlan export profile "folder=$profiles" | Out-Null
    Say ("saved Wi-Fi networks exported: {0}" -f @(Get-ChildItem $profiles -Filter *.xml).Count)

    foreach ($store in 'Root', 'TrustedPublisher') {
        Get-ChildItem "Cert:\LocalMachine\$store" | Where-Object { $_.Subject -eq $CertSubject } | Remove-Item
    }
    $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject $CertSubject -CertStoreLocation Cert:\CurrentUser\My `
        -KeyExportPolicy NonExportable -HashAlgorithm SHA256 -NotAfter (Get-Date).AddYears(2)
    try {
        $cer = Join-Path $stage 'mtdrop-signing.cer'
        Export-Certificate -Cert $cert -FilePath $cer | Out-Null
        foreach ($store in 'Root', 'TrustedPublisher') { Import-Certificate -FilePath $cer -CertStoreLocation "Cert:\LocalMachine\$store" | Out-Null }
        foreach ($file in 'mtdrop_proxy.cat', 'mtdrop_proxy.dll') {
            $r = Set-AuthenticodeSignature -FilePath (Join-Path $stage $file) -Certificate $cert -HashAlgorithm SHA256
            if ($r.Status -ne 'Valid') { throw "$file could not be signed: $($r.StatusMessage)" }
        }
    } finally {
        Remove-Item "Cert:\CurrentUser\My\$($cert.Thumbprint)" -DeleteKey -ErrorAction SilentlyContinue
    }
    Say "driver package signed with a local certificate ($($cert.Thumbprint)); private key deleted"

    # ---- watchdog first, then the swap ---------------------------------------------------------
    $wd = "Start-Sleep -Seconds $WatchdogSeconds; & '$giveback' -PrivateRoot '$PrivateRoot' *> `$null"
    $watchdog = Start-Process powershell.exe -WindowStyle Hidden -PassThru -ArgumentList '-NoProfile', '-ExecutionPolicy', 'Bypass', '-Command', $wd
    Say "watchdog started (pid $($watchdog.Id)): gives the card back in $WatchdogSeconds s no matter what"

    $out = & pnputil.exe /add-driver (Join-Path $stage 'mtdrop_proxy.inf') 2>&1
    if ($LASTEXITCODE -ne 0) { $out | ForEach-Object { Say "  pnputil: $_" }; throw "pnputil /add-driver failed ($LASTEXITCODE)" }
    Say 'proxy package added to the driver store'

    Add-Type -Namespace Mtdrop -Name NewDev -MemberDefinition @'
[System.Runtime.InteropServices.DllImport("newdev.dll", CharSet = System.Runtime.InteropServices.CharSet.Unicode, SetLastError = true)]
public static extern bool UpdateDriverForPlugAndPlayDevicesW(System.IntPtr parent, string hardwareId, string fullInfPath, uint flags, out bool rebootRequired);
'@
    $taken = $true
    $reboot = $false
    $t0 = Get-Date
    $ok = [Mtdrop.NewDev]::UpdateDriverForPlugAndPlayDevicesW([IntPtr]::Zero, $HardwareId, (Join-Path $stage 'mtdrop_proxy.inf'), 1, [ref]$reboot)
    $err = [System.Runtime.InteropServices.Marshal]::GetLastWin32Error()
    if (-not $ok) { throw ("Windows refused to put the proxy on the card (error 0x{0:X8})" -f $err) }
    if ($reboot) { throw 'Windows asks for a restart to bind the proxy; not continuing' }
    $now = Get-CardDriver
    Say ("card driver during test: {0} / {1} (bound in {2:N1} s)" -f $now.DriverProviderName, $now.InfName, ((Get-Date) - $t0).TotalSeconds)
    $dev = Get-PnpDevice -InstanceId $now.DeviceID -ErrorAction SilentlyContinue
    Say "device status: $($dev.Status), problem code: $($dev.Problem)"

    # ---- the actual experiment -----------------------------------------------------------------
    Start-Sleep -Seconds 2
    $result = & $regtool chipid 2>&1
    $code = $LASTEXITCODE
    $result | ForEach-Object { Say "  regtool: $_" }
    Say "regtool exit code: $code"
    if ($code -eq 0) { $exit = 0 }
}
catch {
    Say "FAILED: $($_.Exception.Message)"
}
finally {
    if ($taken) {
        try { & $giveback -PrivateRoot $PrivateRoot | Out-Null; Say "give-back finished (exit $LASTEXITCODE)" }
        catch { Say "give-back threw: $($_.Exception.Message)" }
    }
    $after = Get-CardDriver
    $vendorBack = $after -and $after.DriverProviderName -like 'MediaTek*'
    if ($watchdog -and -not $watchdog.HasExited) {
        if ($vendorBack) { Stop-Process -Id $watchdog.Id -Force -ErrorAction SilentlyContinue; Say 'watchdog stopped (not needed)' }
        else { Say 'watchdog left running because the MediaTek driver is not back yet' }
    }
    Say ("card driver after: {0} / {1} / {2}" -f $after.DriverProviderName, $after.InfName, $after.DriverVersion)
    Say ("S1 RESULT: {0}" -f $(if ($exit -eq 0) { 'PASS' } else { 'FAIL' }))
}
exit $exit
