# Installs or removes the mtdrop service (docs/adr/0004-service-and-watchdog.md, milestone M7.2).
#
# WITHOUT -Apply NOTHING IS CHANGED: the script checks its inputs and prints every change it would make, each with
# the way it is undone. Run it again with -Apply (elevated) to make them.
#
#   .\scripts\install-service.ps1                       plan of the install (read-only)
#   .\scripts\install-service.ps1 -Apply                install, or update an existing install
#   .\scripts\install-service.ps1 -Uninstall            plan of the removal (read-only)
#   .\scripts\install-service.ps1 -Uninstall -Apply     remove; add -RemoveData to delete %ProgramData%\mtdrop too
#
# WHAT AN INSTALL CHANGES ON THE MACHINE
#   - C:\Program Files\mtdrop\: mtdrop-service.exe, radiod.exe, awdld.exe + wintun.dll, app\ (tray app),
#     driver\ (proxy package, signed here once), firmware\ (linux-firmware MT7961 images, F-007).
#   - A local code-signing certificate "CN=mtdrop local driver signing (service)" trusted machine-wide
#     (LocalMachine\Root and \TrustedPublisher); its private key is deleted right after signing.
#   - C:\ProgramData\mtdrop\ restricted to SYSTEM and Administrators, with a copy of the MediaTek driver backup.
#   - Windows service "mtdrop" (LocalSystem, automatic delayed start, restarts on failure), started at the end.
#     It does NOT touch the Wi-Fi card until the tray app asks for a receive session.
#   - Start menu shortcut "mtdrop" and (unless -NoAutostart) a per-user autostart entry for the tray app.
#   The Wi-Fi card, its driver and the network adapters are not touched by the install.
#
# INPUTS (built beforehand, unelevated):
#   dotnet publish service\Mtdrop.Service -c Release -o <ToolsRoot>\build\service
#   dotnet publish app\Mtdrop.App -c Release -o <ToolsRoot>\build\app
#   radiod (<ToolsRoot>\build\radiod), awdld (<ToolsRoot>\build\awdld), the proxy (driver\proxy\x64\Release),
#   firmware (third_party\openwrt_mt76\firmware), vendor driver backup (<PrivateRoot>\vendor-driver-backup).

param(
    [switch]$Apply,
    [switch]$Uninstall,
    [switch]$RemoveData,
    [switch]$NoAutostart,
    [string]$ToolsRoot = 'D:\mtdrop-tools',
    [string]$PrivateRoot = (Join-Path $env:USERPROFILE 'mtdrop-private'),
    [string]$FirmwareDir = '',
    [string]$InstallDir = (Join-Path $env:ProgramFiles 'mtdrop')
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$ServiceName = 'mtdrop'
$CertSubject = 'CN=mtdrop local driver signing (service)'
$DataDir = Join-Path $env:ProgramData 'mtdrop'
$ServiceExe = Join-Path $InstallDir 'mtdrop-service.exe'
$TrayExe = Join-Path $InstallDir 'app\mtdrop.exe'
$Shortcut = Join-Path $env:ProgramData 'Microsoft\Windows\Start Menu\Programs\mtdrop.lnk'
$RunKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
$HardwareMatch = 'PCI\VEN_14C3&DEV_7961*'
if (-not $FirmwareDir) { $FirmwareDir = Join-Path $repo 'third_party\openwrt_mt76\firmware' }

function Say([string]$m) { Write-Output ("{0:HH:mm:ss} {1}" -f (Get-Date), $m) }
function Test-Elevated {
    ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

# The service runs these binaries as SYSTEM: they must live where only administrators can write.
$programFiles = [IO.Path]::GetFullPath($env:ProgramFiles).TrimEnd('\') + '\'
if (-not ([IO.Path]::GetFullPath($InstallDir) + '\').StartsWith($programFiles, [StringComparison]::OrdinalIgnoreCase)) {
    throw "-InstallDir must be under $programFiles (only administrators may write there; the service runs its files as SYSTEM)"
}

$steps = New-Object System.Collections.Generic.List[object]
function Step([string]$Change, [string]$Undo, [scriptblock]$Do) { $steps.Add([pscustomobject]@{ Change = $Change; Undo = $Undo; Do = $Do }) }

$service = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue

# ---- read-only helpers --------------------------------------------------------------------------------
function Get-CardProvider {
    $dev = Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -like $HardwareMatch } | Select-Object -First 1
    if (-not $dev) { return $null }
    (Get-PnpDeviceProperty -InstanceId $dev.InstanceId -KeyName 'DEVPKEY_Device_DriverProvider' -ErrorAction SilentlyContinue).Data
}
function Get-ProxyPackages {
    $xml = [xml]((& pnputil.exe /enum-drivers /format xml) -join "`n")
    @($xml.SelectNodes('//Driver') | Where-Object { $_.OriginalName -eq 'mtdrop_proxy.inf' } | ForEach-Object { $_.DriverName })
}
function Stop-TrayApps {
    Get-Process -Name 'mtdrop' -ErrorAction SilentlyContinue |
        Where-Object { $_.Path -and $_.Path.StartsWith($InstallDir, [StringComparison]::OrdinalIgnoreCase) } |
        ForEach-Object { Stop-Process -Id $_.Id -Force; Say "  stopped the tray app (pid $($_.Id))" }
}
function Stop-MtdropService {
    $s = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
    if ($s -and $s.Status -ne 'Stopped') {
        Say '  stopping the service (it ends a running session and gives the Wi-Fi card back first; up to 2 min)'
        Stop-Service -Name $ServiceName -Force
        (Get-Service -Name $ServiceName).WaitForStatus('Stopped', [TimeSpan]::FromMinutes(3))
    }
}
function Remove-Certificates {
    foreach ($store in 'Root', 'TrustedPublisher') {
        Get-ChildItem "Cert:\LocalMachine\$store" | Where-Object { $_.Subject -eq $CertSubject } |
            ForEach-Object { Remove-Item $_.PSPath; Say "  removed the signing certificate from LocalMachine\$store" }
    }
}
function Send-Status {
    # The control pipe's status command, as the tray app sends it (service/Mtdrop.Control).
    $pipe = New-Object System.IO.Pipes.NamedPipeClientStream('.', 'mtdrop-control', [IO.Pipes.PipeDirection]::InOut)
    try {
        $pipe.Connect(10000)
        $pipe.ReadMode = [IO.Pipes.PipeTransmissionMode]::Message
        $b = [Text.Encoding]::UTF8.GetBytes('{"command":"status"}')
        $pipe.Write($b, 0, $b.Length); $pipe.Flush()
        $buf = New-Object byte[] 4097
        $n = $pipe.Read($buf, 0, $buf.Length)
        [Text.Encoding]::UTF8.GetString($buf, 0, $n)
    } finally { $pipe.Dispose() }
}

if (-not $Uninstall) {
    # ---- inputs (checked in plan mode too) ---------------------------------------------------------------
    $src = [ordered]@{
        service  = Join-Path $ToolsRoot 'build\service'
        app      = Join-Path $ToolsRoot 'build\app'
        radiod   = Join-Path $ToolsRoot 'build\radiod\radiod.exe'
        awdld    = Join-Path $ToolsRoot 'build\awdld\awdld.exe'
        wintun   = Join-Path $ToolsRoot 'build\awdld\wintun.dll'
        proxyInf = Join-Path $repo 'driver\proxy\x64\Release\mtdrop_proxy\mtdrop_proxy.inf'
        proxyDll = Join-Path $repo 'driver\proxy\x64\Release\mtdrop_proxy.dll'
        patch    = Join-Path $FirmwareDir 'WIFI_MT7961_patch_mcu_1_2_hdr.bin'
        ram      = Join-Path $FirmwareDir 'WIFI_RAM_CODE_MT7961_1.bin'
        backup   = Join-Path $PrivateRoot 'vendor-driver-backup'
    }
    $missing = @($src.GetEnumerator() | Where-Object { -not (Test-Path $_.Value) } | ForEach-Object { "$($_.Key): $($_.Value)" })
    if (-not (Test-Path (Join-Path $src.service 'mtdrop-service.exe'))) { $missing += "service: $($src.service)\mtdrop-service.exe (dotnet publish service\Mtdrop.Service -c Release -o $($src.service))" }
    if (-not (Test-Path (Join-Path $src.app 'mtdrop.exe'))) { $missing += "app: $($src.app)\mtdrop.exe (dotnet publish app\Mtdrop.App -c Release -o $($src.app))" }
    if ((Test-Path $src.backup) -and -not (Get-ChildItem $src.backup -Recurse -Filter 'mtkwl6ex.inf' -ErrorAction SilentlyContinue)) { $missing += "mtkwl6ex.inf under $($src.backup)" }
    if (-not (Test-Path (Join-Path $env:SystemRoot 'System32\vcruntime140.dll'))) { $missing += 'vcruntime140.dll (Visual C++ runtime, needed by radiod)' }
    $inf2cat = Get-ChildItem (Join-Path $ToolsRoot 'packages') -Recurse -Filter Inf2Cat.exe -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $inf2cat) { $missing += "Inf2Cat.exe under $ToolsRoot\packages (WDK)" }
    $runtimes = (& dotnet --list-runtimes 2>$null) -join "`n"
    if ($runtimes -notmatch 'Microsoft\.NETCore\.App 10\.' -or $runtimes -notmatch 'Microsoft\.WindowsDesktop\.App 10\.') { $missing += '.NET 10 runtime and Windows Desktop runtime (machine-wide)' }
    if ($missing.Count) { throw ("missing inputs:`n  " + ($missing -join "`n  ")) }

    # ---- the install plan --------------------------------------------------------------------------------
    if ($service) {
        Step 'Stop the installed mtdrop service and the tray app (update of an existing install)' `
             'none needed: the last step starts the service again' {
            Stop-MtdropService
            Stop-TrayApps
        }
    }
    Step "Copy mtdrop-service.exe, radiod.exe, awdld.exe, wintun.dll, the tray app (app\) and the MT7961 firmware (firmware\) to $InstallDir (replacing an older copy)" `
         'install-service.ps1 -Uninstall -Apply deletes the folder' {
        if (Test-Path $InstallDir) { Remove-Item $InstallDir -Recurse -Force }
        New-Item -ItemType Directory -Force -Path $InstallDir, "$InstallDir\app", "$InstallDir\driver", "$InstallDir\firmware" | Out-Null
        Copy-Item (Join-Path $src.service '*') $InstallDir -Recurse
        Copy-Item (Join-Path $src.app '*') "$InstallDir\app" -Recurse
        Copy-Item $src.radiod, $src.awdld, $src.wintun $InstallDir
        Copy-Item $src.patch, $src.ram "$InstallDir\firmware"
        $lic = Join-Path $FirmwareDir 'LICENSE'
        if (Test-Path $lic) { Copy-Item $lic "$InstallDir\firmware\LICENSE" }
        Copy-Item $src.proxyInf, $src.proxyDll "$InstallDir\driver"
        Say "  copied $(@(Get-ChildItem $InstallDir -Recurse -File).Count) files"
    }
    Step "Create a catalog for the proxy driver package and sign it with a new local certificate '$CertSubject', trusted in LocalMachine\Root and \TrustedPublisher; the private key is deleted right after (the package is only put on the card during a receive session)" `
         'install-service.ps1 -Uninstall -Apply removes the certificate from both stores' {
        $out = & $inf2cat.FullName "/driver:$InstallDir\driver" /os:10_X64 /uselocaltime 2>&1
        if ($LASTEXITCODE -ne 0 -or -not (Test-Path "$InstallDir\driver\mtdrop_proxy.cat")) { $out | ForEach-Object { Say "  inf2cat: $_" }; throw "Inf2Cat failed ($LASTEXITCODE)" }
        Remove-Certificates
        $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject $CertSubject -CertStoreLocation Cert:\CurrentUser\My `
            -KeyExportPolicy NonExportable -HashAlgorithm SHA256 -NotAfter (Get-Date).AddYears(5)
        try {
            $cer = "$InstallDir\driver\mtdrop-signing.cer"
            Export-Certificate -Cert $cert -FilePath $cer | Out-Null
            foreach ($store in 'Root', 'TrustedPublisher') { Import-Certificate -FilePath $cer -CertStoreLocation "Cert:\LocalMachine\$store" | Out-Null }
            foreach ($file in 'mtdrop_proxy.cat', 'mtdrop_proxy.dll') {
                $r = Set-AuthenticodeSignature -FilePath "$InstallDir\driver\$file" -Certificate $cert -HashAlgorithm SHA256
                if ($r.Status -ne 'Valid') { throw "$file could not be signed: $($r.StatusMessage)" }
            }
        } finally {
            Remove-Item "Cert:\CurrentUser\My\$($cert.Thumbprint)" -DeleteKey -ErrorAction SilentlyContinue
        }
        Say "  driver package signed ($($cert.Thumbprint)); private key deleted"
    }
    Step "Create $DataDir (logs, saved Wi-Fi networks during a session, state) readable only by SYSTEM and Administrators, and copy the MediaTek driver backup into it" `
         "kept by -Uninstall (logs); -Uninstall -RemoveData -Apply deletes it" {
        $sec = New-Object System.Security.AccessControl.DirectorySecurity
        $sec.SetAccessRuleProtection($true, $false)
        $sec.SetOwner((New-Object System.Security.Principal.SecurityIdentifier 'S-1-5-32-544'))
        foreach ($sid in 'S-1-5-18', 'S-1-5-32-544') {
            $rule = New-Object System.Security.AccessControl.FileSystemAccessRule((New-Object System.Security.Principal.SecurityIdentifier $sid),
                'FullControl', 'ContainerInherit,ObjectInherit', 'None', 'Allow')
            $sec.AddAccessRule($rule)
        }
        New-Item -ItemType Directory -Force -Path $DataDir | Out-Null
        (Get-Item $DataDir).SetAccessControl($sec)
        if (Get-ChildItem $DataDir -Force) { & icacls.exe "$DataDir\*" /reset /T /C /Q | Out-Null }
        $dest = Join-Path $DataDir 'vendor-driver-backup'
        if (Test-Path $dest) { Remove-Item $dest -Recurse -Force }
        Copy-Item $src.backup $dest -Recurse
        Say "  $DataDir secured; vendor driver backup copied"
    }
    if ($service) {
        Step "Keep the registration of the service '$ServiceName' (automatic delayed start, LocalSystem, restart on failure), pointing at $ServiceExe" `
             'install-service.ps1 -Uninstall -Apply stops and deletes the service' {
            $wmi = Get-CimInstance Win32_Service -Filter "Name='$ServiceName'"
            if ($wmi.PathName -ne "`"$ServiceExe`"") {
                $r = Invoke-CimMethod -InputObject $wmi -MethodName Change -Arguments @{ PathName = "`"$ServiceExe`"" }
                if ($r.ReturnValue -ne 0) { throw "could not update the service path ($($r.ReturnValue))" }
            }
            & sc.exe config $ServiceName start= delayed-auto | Out-Null
            & sc.exe failure $ServiceName reset= 86400 actions= restart/5000/restart/10000/restart/60000 | Out-Null
        }
    } else {
        Step "Register the Windows service '$ServiceName' ($ServiceExe, LocalSystem, automatic delayed start, restart on failure)" `
             'install-service.ps1 -Uninstall -Apply stops and deletes the service (or: sc.exe stop mtdrop; sc.exe delete mtdrop)' {
            New-Service -Name $ServiceName -BinaryPathName "`"$ServiceExe`"" -DisplayName 'mtdrop receive service' -StartupType Automatic `
                -Description 'Runs AirDrop-compatible receive sessions for the mtdrop tray app and always gives the Wi-Fi card back to the MediaTek driver.' | Out-Null
            & sc.exe config $ServiceName start= delayed-auto | Out-Null
            if ($LASTEXITCODE -ne 0) { throw "sc.exe config failed ($LASTEXITCODE)" }
            & sc.exe failure $ServiceName reset= 86400 actions= restart/5000/restart/10000/restart/60000 | Out-Null
            if ($LASTEXITCODE -ne 0) { throw "sc.exe failure failed ($LASTEXITCODE)" }
        }
    }
    Step "Start menu shortcut 'mtdrop' for the tray app$(if (-not $NoAutostart) { ", and start the tray app at sign-in for $env:USERNAME (HKCU Run value 'mtdrop')" })" `
         '-Uninstall -Apply removes both' {
        $shell = New-Object -ComObject WScript.Shell
        $lnk = $shell.CreateShortcut($Shortcut)
        $lnk.TargetPath = $TrayExe
        $lnk.WorkingDirectory = Split-Path $TrayExe
        $lnk.Description = 'mtdrop: receive photos from an iPhone (AirDrop-compatible)'
        $lnk.Save()
        if (-not $NoAutostart) { Set-ItemProperty -Path $RunKey -Name 'mtdrop' -Value "`"$TrayExe`"" }
    }
    Step "Start the service and check it: on start it only verifies the Wi-Fi card is on the MediaTek driver, then waits for the tray app" `
         'sc.exe stop mtdrop' {
        Start-Service -Name $ServiceName
        (Get-Service -Name $ServiceName).WaitForStatus('Running', [TimeSpan]::FromSeconds(30))
        Say "  control pipe answers: $(Send-Status)"
        & $ServiceExe --check | ForEach-Object { Say "  check: $_" }
    }
}
else {
    # ---- the removal plan ----------------------------------------------------------------------------------
    if ($service) {
        Step "Stop the service '$ServiceName' (a running session ends and the Wi-Fi card is given back first) and delete it" `
             'install-service.ps1 -Apply registers it again' {
            Stop-MtdropService
            & sc.exe delete $ServiceName | Out-Null
            if ($LASTEXITCODE -ne 0) { throw "sc.exe delete failed ($LASTEXITCODE)" }
        }
    }
    Step 'Remove leftovers of a session if there are any: the watchdog task mtdrop\watchdog, the firewall rules mtdrop-receiver-*, and the proxy driver (the card goes back to the MediaTek driver)' `
         'none needed (session state only)' {
        & schtasks.exe /Query /TN 'mtdrop\watchdog' 2>$null | Out-Null
        if ($LASTEXITCODE -eq 0) { & schtasks.exe /Delete /TN 'mtdrop\watchdog' /F | Out-Null; Say '  watchdog task removed' }
        try { $ts = New-Object -ComObject Schedule.Service; $ts.Connect(); $ts.GetFolder('\').DeleteFolder('mtdrop', 0) } catch { }   # empty task folder; absent is fine
        Get-NetFirewallRule -DisplayName 'mtdrop-receiver-*' -ErrorAction SilentlyContinue | Remove-NetFirewallRule
        $provider = Get-CardProvider
        if ((Get-ProxyPackages).Count -gt 0 -or ($provider -and $provider -notlike 'MediaTek*')) {
            Say "  the card is not on the MediaTek driver or a proxy package is left ($provider); giving it back"
            if (Test-Path $ServiceExe) { & $ServiceExe --give-back | ForEach-Object { Say "  $_" } }
            else { & (Join-Path $PSScriptRoot 'wifi-giveback.ps1') | ForEach-Object { Say "  $_" } }
            if ($LASTEXITCODE -ne 0) { throw 'give-back failed: run scripts\wifi-giveback.ps1 as administrator before going on' }
        }
    }
    Step "Remove the Start menu shortcut, the autostart entry, the signing certificate '$CertSubject' and $InstallDir" `
         'install-service.ps1 -Apply installs again' {
        Stop-TrayApps
        if (Test-Path $Shortcut) { Remove-Item $Shortcut }
        Remove-ItemProperty -Path $RunKey -Name 'mtdrop' -ErrorAction SilentlyContinue
        Remove-Certificates
        if (Test-Path $InstallDir) { Remove-Item $InstallDir -Recurse -Force; Say "  $InstallDir removed" }
    }
    if ($RemoveData) {
        Step "Delete $DataDir (service logs, the copy of the MediaTek driver backup, saved Wi-Fi networks of the last session)" `
             'cannot be undone (the original driver backup in your private folder is not touched)' {
            if (Test-Path $DataDir) { Remove-Item $DataDir -Recurse -Force; Say "  $DataDir removed" }
        }
    }
}

# ---- print the plan, then (only with -Apply) carry it out ---------------------------------------------------
$what = if ($Uninstall) { 'removal' } else { 'install' }
Write-Output ''
Write-Output "mtdrop $what plan$(if (-not $Apply) { ' - NOTHING HAS BEEN CHANGED' }):"
$i = 0
foreach ($s in $steps) {
    $i++
    Write-Output ("  {0}. {1}" -f $i, $s.Change)
    Write-Output ("     undo: {0}" -f $s.Undo)
}
if ($Uninstall -and -not $RemoveData) { Write-Output "  ($DataDir is kept; add -RemoveData to delete it)" }
Write-Output ''
if (-not $Apply) {
    Write-Output "To make these changes run (elevated):  .\scripts\install-service.ps1$(if ($Uninstall) { ' -Uninstall' })$(if ($RemoveData) { ' -RemoveData' }) -Apply"
    exit 0
}
if (-not (Test-Elevated)) { throw 'Not elevated: run PowerShell as administrator for -Apply.' }

$i = 0
foreach ($s in $steps) {
    $i++
    Say "step $i/$($steps.Count): $($s.Change)"
    try { & $s.Do }
    catch {
        Say "FAILED at step ${i}: $($_.Exception.Message)"
        Say "Steps 1..$($i - 1) were done. Undo: .\scripts\install-service.ps1 -Uninstall -Apply ; Wi-Fi by hand: .\scripts\wifi-giveback.ps1"
        exit 1
    }
}
Say "mtdrop $what finished"
if (-not $Uninstall) { Say "Start the tray app from the Start menu (mtdrop) and click 'Start receiving'." }
exit 0
