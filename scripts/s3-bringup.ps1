# Spike S3 (docs/adr/0002-driver-strategy.md, docs/MT7921-BRINGUP.md section 6): bind the mtdrop UMDF
# register proxy to the MT7921, run `radiod bringup` for the requested stages one after another
# (stopping at the first failure), and give the card back to the MediaTek driver.
#
# Stages (driver/mt792x): 1 ownership handshake + chip id | 2 + WFSYS reset, DMA disabled |
#   3 + rings programmed, DMA enabled (no traffic) | 4 + first MCU commands (first real DMA) |
#   5 + patch and RAM firmware download, firmware ready.
#
# WHAT THIS CHANGES ON THE MACHINE
#   - Same as s1-chipid.ps1: local signing certificate trusted machine-wide, proxy forced onto the
#     Wi-Fi card, Wi-Fi DOWN until the card is given back.
#   - From stage 3 on, the chip is told to use one locked 2 MiB block of RAM for DMA.
#
# AFTER A SYSTEM HANG (docs/FINDINGS.md F-011)
#   - radiod's own lines are in <PrivateRoot>\radiod-live.log, written through to disk one by one.
#
# HOW IT IS UNDONE
#   - radiod always runs the orderly chip shutdown; if radiod dies, the proxy replays the stop list.
#   - The card is given back at the end of this script whatever happens (finally block).
#   - A watchdog process gives the card back after -WatchdogSeconds even if this script is killed.
#   - By hand:  .\scripts\wifi-giveback.ps1   (elevated)
#
# Must run as SYSTEM (radiod needs SeLockMemoryPrivilege): start it through run-as-system.ps1.
# Refuses to run without -IUnderstandWifiGoesDown.

param(
    [switch]$IUnderstandWifiGoesDown,
    [string]$Stages = '',
    # Alternative to -Stages: radiod command lines separated by ';'. The token @fw is replaced by the
    # patch and RAM firmware paths, e.g.  -Commands 'bringup 6 @fw;sniff 1 10 @fw'  or, without spaces,
    # -Commands bringup:6:@fw;sniff:1:10:@fw
    [string]$Commands = '',
    # Second firmware set for comparisons, addressed by the token @alt (same two file names).
    [string]$AltFirmwareDir = '',
    # radioctl arguments (ADR-0003 M5.1): radioctl is started in the background before the radiod commands
    # and connects to `radiod serve` when it is up. ':' may stand for a space, e.g. --seconds:40:--tx-test
    [string]$Client = '',
    # awdld arguments (ADR-0003 M5.4): awdld (with wintun.dll beside it) is started in the background before the
    # radiod commands, connects to `radiod serve`, and ends when the server does. The first peer it reports
    # reachable is pinged over awdl0. ':' may stand for a space and '+' for '-' (a value starting with '-' would be
    # taken for a PowerShell parameter), e.g. +c:44:+S:intersect:+v
    [string]$Awdl = '',
    # M6: run the AirDrop receiver (airdropd console host) on awdl0 next to awdld, auto-accepting, saving into
    # <PrivateRoot>\received. Extra airdropd arguments as for -Awdl ('+' for '-', ':' for ' '), may be empty: use
    # -Airdrop on. Needs -Awdl. Adds a temporary inbound firewall rule for airdropd (TCP 8771, UDP 5353), removed in
    # the finally block whatever happens.
    # -Airdrop tray: the tray app (<ToolsRoot>\build\app\mtdrop.exe) receives instead. It must already run in the
    # maintainer's desktop session (this script runs as SYSTEM, where no prompt or toast can be seen) with
    # "Start receiving" clicked; it waits for awdl0. Only the temporary firewall rules are added, for mtdrop.exe.
    [string]$Airdrop = '',
    [switch]$KeepGoing,
    [string]$PrivateRoot = 'C:\Users\desktop\mtdrop-private',
    [string]$ToolsRoot = 'D:\mtdrop-tools',
    [int]$WatchdogSeconds = 240
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$HardwareMatch = 'PCI\VEN_14C3&DEV_7961*'
$HardwareId = 'PCI\VEN_14C3&DEV_7961&CC_0280'
$CertSubject = 'CN=mtdrop local driver signing'
$log = Join-Path $PrivateRoot 'driver-swap.log'
# radiod --live-log: every line on disk before radiod continues; after a hang, its last line is the last step.
$liveLog = Join-Path $PrivateRoot 'radiod-live.log'
$giveback = Join-Path $PSScriptRoot 'wifi-giveback.ps1'
New-Item -ItemType Directory -Force -Path $PrivateRoot | Out-Null
function Say([string]$m) { $line = "{0:yyyy-MM-dd HH:mm:ss} [s3] {1}" -f (Get-Date), $m; Add-Content -Path $log -Value $line; Write-Output $line }
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
    Say "=== S3: radiod session [$Stages] [$Commands], as $([Security.Principal.WindowsIdentity]::GetCurrent().Name) ==="
    if (-not $IUnderstandWifiGoesDown) { throw 'Refusing to run without -IUnderstandWifiGoesDown.' }
    $identity = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
    if (-not $identity.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Not elevated.' }

    # ---- preconditions (nothing is changed before all of them hold) ----------------------------
    $built = Join-Path $repo 'driver\proxy\x64\Release\mtdrop_proxy'
    $regtool = Join-Path $ToolsRoot 'build\radiod\radiod.exe'      # the program under test
    $fwDir = Get-ChildItem (Join-Path $PrivateRoot 'vendor-driver-backup') -Directory | Sort-Object Name | Select-Object -Last 1
    $patch = Join-Path $fwDir.FullName 'WIFI_MT7961_patch_mcu_1_2_hdr.bin'
    $ram = Join-Path $fwDir.FullName 'WIFI_RAM_CODE_MT7961_1.bin'
    if (-not $AltFirmwareDir) { $AltFirmwareDir = Join-Path $repo 'third_party\openwrt_mt76\firmware' }
    $altPatch = Join-Path $AltFirmwareDir 'WIFI_MT7961_patch_mcu_1_2_hdr.bin'
    $altRam = Join-Path $AltFirmwareDir 'WIFI_RAM_CODE_MT7961_1.bin'
    $commandList = @()
    if ($Stages) {
        foreach ($n in ($Stages -split ',')) {
            $v = [int]$n.Trim()
            if ($v -lt 1 -or $v -gt 6) { throw "bad -Stages '$Stages'" }
            $commandList += "bringup $v @fw"
        }
    }
    # ':' may stand for a space so the whole value survives being passed through a scheduled task unquoted.
    $Commands = $Commands -replace ':', ' '
    if ($Commands) { $commandList += @($Commands -split ';' | ForEach-Object { $_.Trim() } | Where-Object { $_ }) }
    if ($commandList.Count -eq 0) { throw 'nothing to run: give -Stages or -Commands' }
    if (($commandList -join ' ') -match '@alt') { foreach ($need in $altPatch, $altRam) { if (-not (Test-Path $need)) { throw "missing alternative firmware file: $need" } } }
    foreach ($c in $commandList) { if ($c -notmatch '^(bringup|sniff|txtest|acktest|serve) [A-Za-z0-9 ,@=_.-]+$') { throw "refusing radiod command '$c'" } }
    $Client = ($Client -replace ':', ' ').Trim()
    $clientTool = Join-Path $ToolsRoot 'build\radiod\radioctl.exe'
    $Awdl = ($Awdl -replace ':', ' ' -replace '\+', '-').Trim()
    $awdlTool = Join-Path $ToolsRoot 'build\awdld\awdld.exe'
    if ($Awdl) {
        if ($Awdl -notmatch '^[A-Za-z0-9 ._-]+$') { throw "refusing awdld arguments '$Awdl'" }
        foreach ($need in $awdlTool, (Join-Path (Split-Path $awdlTool) 'wintun.dll')) { if (-not (Test-Path $need)) { throw "missing $need" } }
    }
    $airdropTray = $Airdrop -eq 'tray'
    $airdropTool = if ($airdropTray) { Join-Path $ToolsRoot 'build\app\mtdrop.exe' } else { Join-Path $ToolsRoot 'build\airdropd\Mtdrop.Airdropd.exe' }
    if ($airdropTray) { $Airdrop = 'on' }
    $airdropOn = [bool]$Airdrop
    $Airdrop = if ($Airdrop -eq 'on') { '' } else { ($Airdrop -replace ':', ' ' -replace '\+', '-').Trim() }
    if ($airdropOn) {
        if (-not $Awdl) { throw '-Airdrop needs -Awdl (awdl0 exists only while awdld runs)' }
        if ($Airdrop -and $Airdrop -notmatch '^[A-Za-z0-9 ._-]+$') { throw "refusing airdropd arguments '$Airdrop'" }
        if (-not (Test-Path $airdropTool)) { throw "missing $airdropTool" }
    }
    if ($Client) {
        if ($Client -notmatch '^[A-Za-z0-9 ._-]+$') { throw "refusing radioctl arguments '$Client'" }
        if (-not (Test-Path $clientTool)) { throw "missing $clientTool" }
    }
    foreach ($need in $patch, $ram) { if (-not (Test-Path $need)) { throw "missing firmware file: $need" } }
    $inf2cat = Get-ChildItem (Join-Path $ToolsRoot 'packages') -Recurse -Filter Inf2Cat.exe -ErrorAction SilentlyContinue | Select-Object -First 1
    $builtInf = Join-Path $built 'mtdrop_proxy.inf'                                   # stamped by the build
    $builtDll = Join-Path $repo 'driver\proxy\x64\Release\mtdrop_proxy.dll'
    foreach ($need in $builtInf, $builtDll, $regtool, $giveback) {
        if (-not (Test-Path $need)) { throw "missing: $need (build the proxy and the tool first)" }
    }
    if (-not $inf2cat) { throw 'Inf2Cat.exe not found under the WDK packages' }
    if (-not (Get-ChildItem (Join-Path $PrivateRoot 'vendor-driver-backup') -Recurse -Filter mtkwl6ex.inf -ErrorAction SilentlyContinue)) { throw 'vendor driver backup not found' }
    if (@(Get-ComputerRestorePoint -ErrorAction SilentlyContinue).Count -eq 0) {
        # Windows deletes restore points (and cannot create one) when C: runs out of space.
        $freeGb = [math]::Round((Get-PSDrive C).Free / 1GB, 1)
        throw "no System Restore point (C: has $freeGb GB free; free some space, then: Checkpoint-Computer -Description 'mtdrop' -RestorePointType MODIFY_SETTINGS)"
    }
    $before = Get-CardDriver
    if (-not $before) { throw 'MT7921 not found' }
    if ($before.DriverProviderName -notlike 'MediaTek*') { throw "card is not on the MediaTek driver now ($($before.DriverProviderName) / $($before.InfName)); run wifi-giveback.ps1 first" }
    Say ("card driver before: {0} / {1} / {2}" -f $before.DriverProviderName, $before.InfName, $before.DriverVersion)

    # Pre-flight without touching the device: the DMA block must be obtainable now, before Wi-Fi goes down.
    $probe = & $regtool dma-probe 2>&1
    $probe | ForEach-Object { Say "  preflight: $_" }
    if ($LASTEXITCODE -ne 0) { throw 'pre-flight failed: no usable DMA block (close large programs to free RAM, then retry)' }

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
    $exit = 0
    $clientProc = $null
    $clientLog = Join-Path $PrivateRoot 'radioctl.log'
    if ($Client) {
        Remove-Item $clientLog, "$clientLog.err" -ErrorAction SilentlyContinue
        $clientProc = Start-Process -FilePath $clientTool -ArgumentList ($Client -split ' ' | Where-Object { $_ }) `
            -RedirectStandardOutput $clientLog -RedirectStandardError "$clientLog.err" -NoNewWindow -PassThru
        $null = $clientProc.Handle   # keeps the process handle, without which ExitCode stays empty
        Say "radioctl $Client started (pid $($clientProc.Id)); it connects once radiod serve is up"
    }
    $awdlProc = $null
    $pingJob = $null
    $awdlLog = Join-Path $PrivateRoot 'awdld.log'
    if ($Awdl) {
        Remove-Item $awdlLog, "$awdlLog.err", (Join-Path $PrivateRoot 'awdl-ping.log') -ErrorAction SilentlyContinue
        $awdlProc = Start-Process -FilePath $awdlTool -ArgumentList ($Awdl -split ' ' | Where-Object { $_ }) `
            -WorkingDirectory (Split-Path $awdlTool) -RedirectStandardOutput $awdlLog -RedirectStandardError "$awdlLog.err" -NoNewWindow -PassThru
        $null = $awdlProc.Handle
        Say "awdld $Awdl started (pid $($awdlProc.Id)); it connects once radiod serve is up"
        # Pings the first peer awdld reports reachable ("peer ... is reachable at fe80::...%N"), 20 times.
        $pingJob = Start-Job -ArgumentList $awdlLog, "$awdlLog.err", (Join-Path $PrivateRoot 'awdl-ping.log') -ScriptBlock {
            param($log, $err, $out)
            $deadline = (Get-Date).AddSeconds(600)
            while ((Get-Date) -lt $deadline) {
                $hit = Get-Content $log, $err -ErrorAction SilentlyContinue | Select-String 'reachable at (fe80::[0-9a-f:]+%\d+)' | Select-Object -First 1
                if ($hit) {
                    $addr = $hit.Matches[0].Groups[1].Value
                    "pinging $addr" | Out-File $out -Encoding utf8
                    & ping.exe -6 -n 20 -w 1000 $addr 2>&1 | Out-File $out -Append -Encoding utf8
                    return
                }
                Start-Sleep -Milliseconds 500
            }
        }
    }
    $airdropProc = $null
    $airdropLog = Join-Path $PrivateRoot 'airdropd.log'
    $received = Join-Path $PrivateRoot 'received'
    if ($airdropOn) {
        New-Item -ItemType Directory -Force -Path $received | Out-Null
        $before = @(Get-ChildItem $received -File -ErrorAction SilentlyContinue | ForEach-Object FullName)
        # Temporary: awdl0 is a new, unidentified (public) network, where inbound connections are blocked by default.
        Remove-NetFirewallRule -Name 'mtdrop-airdropd-test-*' -ErrorAction SilentlyContinue   # leftovers of a killed session
        New-NetFirewallRule -Name 'mtdrop-airdropd-test-tcp' -DisplayName 'mtdrop airdropd (test session, TCP 8771)' -Direction Inbound `
            -Program $airdropTool -Protocol TCP -LocalPort 8771 -Action Allow -Profile Any | Out-Null
        New-NetFirewallRule -Name 'mtdrop-airdropd-test-mdns' -DisplayName 'mtdrop airdropd (test session, mDNS)' -Direction Inbound `
            -Program $airdropTool -Protocol UDP -LocalPort 5353 -Action Allow -Profile Any | Out-Null
        Say "temporary firewall rules mtdrop-airdropd-test-* added for $(Split-Path -Leaf $airdropTool) (removed at the end)"
    }
    if ($airdropTray) {
        $tray = @(Get-Process -Name 'mtdrop' -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $airdropTool })
        if ($tray.Count) { Say "tray app running (pid $($tray[0].Id)); it receives on awdl0 once awdld creates it" }
        else { Say "WARNING: $airdropTool is not running - start it in your session and click Start receiving" }
    }
    elseif ($airdropOn) {
        Remove-Item $airdropLog, "$airdropLog.err" -ErrorAction SilentlyContinue
        $airdropArgs = @('--interface', 'awdl0', '--wait-interface', '90', '--yes', '--dir', $received) + @($Airdrop -split ' ' | Where-Object { $_ })
        $airdropProc = Start-Process -FilePath $airdropTool -ArgumentList $airdropArgs -WorkingDirectory (Split-Path $airdropTool) `
            -RedirectStandardOutput $airdropLog -RedirectStandardError "$airdropLog.err" -NoNewWindow -PassThru
        $null = $airdropProc.Handle
        Say "airdropd started (pid $($airdropProc.Id)), auto-accept, saving to $received; it waits for awdl0"
    }
    foreach ($c in $commandList) {
        Say "--- radiod $c"
        $argv = @($c -split ' ' | Where-Object { $_ } | ForEach-Object { if ($_ -eq '@fw') { $patch; $ram } elseif ($_ -eq '@alt') { $altPatch; $altRam } else { $_ } })
        # Streamed line by line, and radiod itself writes every line through to $liveLog before it goes
        # on (F-011: when the system hung, the collected output of the running command was lost).
        & $regtool --live-log $liveLog @argv 2>&1 | ForEach-Object { Say "  radiod: $_" }
        $code = $LASTEXITCODE
        Say "radiod $c -> exit code: $code"
        if ($code -ne 0) { $exit = 1; if ($KeepGoing) { Say "'$c' failed; continuing (-KeepGoing)" } else { Say "stopping: '$c' failed"; break } }
        Start-Sleep -Seconds 1
    }
    if ($awdlProc) {
        if (-not $awdlProc.WaitForExit(15000)) { Stop-Process -Id $awdlProc.Id -Force -ErrorAction SilentlyContinue; Say 'awdld did not end within 15 s after radiod; stopped' }
        foreach ($f in $awdlLog, "$awdlLog.err") { if (Test-Path $f) { Get-Content $f | ForEach-Object { Say "  awdld: $_" } } }
        $awdlProc.Refresh()
        Say "awdld -> exit code: $($awdlProc.ExitCode)"
        if ($pingJob) {
            $null = Wait-Job $pingJob -Timeout 30
            Stop-Job $pingJob -ErrorAction SilentlyContinue; Remove-Job $pingJob -Force -ErrorAction SilentlyContinue
            $pingLog = Join-Path $PrivateRoot 'awdl-ping.log'
            if (Test-Path $pingLog) { Get-Content $pingLog | ForEach-Object { Say "  ping: $_" } } else { Say '  ping: no peer was reported reachable' }
        }
    }
    if ($airdropProc) {
        if (-not $airdropProc.HasExited) { Stop-Process -Id $airdropProc.Id -Force -ErrorAction SilentlyContinue }
        foreach ($f in $airdropLog, "$airdropLog.err") { if (Test-Path $f) { Get-Content $f | ForEach-Object { Say "  airdropd: $_" } } }
        $new = @(Get-ChildItem $received -File -ErrorAction SilentlyContinue | Where-Object { $before -notcontains $_.FullName })
        Say "received files: $($new.Count)"
        foreach ($f in $new) { Say ("  received: {0} ({1:N0} bytes)" -f $f.Name, $f.Length) }
    }
    if ($clientProc) {
        if (-not $clientProc.WaitForExit(30000)) { Stop-Process -Id $clientProc.Id -Force -ErrorAction SilentlyContinue; Say 'radioctl did not finish within 30 s after radiod; stopped' }
        foreach ($f in $clientLog, "$clientLog.err") { if (Test-Path $f) { Get-Content $f | ForEach-Object { Say "  radioctl: $_" } } }
        $clientProc.Refresh()
        Say "radioctl -> exit code: $($clientProc.ExitCode)"
        if ($clientProc.ExitCode -ne 0) { $exit = 1 }
    }
}
catch {
    Say "FAILED: $($_.Exception.Message)"
}
finally {
    if (Get-NetFirewallRule -Name 'mtdrop-airdropd-test-*' -ErrorAction SilentlyContinue) {
        Remove-NetFirewallRule -Name 'mtdrop-airdropd-test-*' -ErrorAction SilentlyContinue
        Say 'temporary firewall rules removed'
    }
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
    Say ("S3 RESULT: {0}" -f $(if ($exit -eq 0) { 'PASS' } else { 'FAIL' }))
}
exit $exit
