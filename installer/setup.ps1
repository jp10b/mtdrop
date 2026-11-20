# Machine-side steps of the mtdrop setup program (installer\mtdrop.iss runs this script; it is not meant to be run
# by hand, but it is safe to read). Everything it changes is undone by -Uninstall. Log: %TEMP%\mtdrop-setup.log
#
#   -Check       read-only: the hardware check of -Prepare (the setup runs it when it starts).
#   -Prepare     before files are copied: checks that the PC has an MT7921 on the MediaTek driver (or a backup of
#                it), stops a running mtdrop (the service gives the Wi-Fi card back first) and the tray app.
#   -Install     after files are copied to -InstallDir: data folder (SYSTEM + Administrators only), backup of the
#                MediaTek driver package, local signature of the proxy driver package, the Windows service.
#   -Uninstall   stops and deletes the service (Wi-Fi card back to MediaTek first), removes session leftovers,
#                the signing certificate and %ProgramData%\mtdrop.
#
# Exit codes: 0 ok; 10 no MT7921 Wi-Fi card; 11 card not on the MediaTek driver and no backup of it;
#             12 MediaTek driver package not the one mtdrop knows; 1 other failure (see the log).

param(
    [switch]$Check,
    [switch]$Prepare,
    [switch]$Install,
    [switch]$Uninstall,
    [string]$InstallDir = (Join-Path $env:ProgramFiles 'mtdrop')
)

$ErrorActionPreference = 'Stop'
$ServiceName = 'mtdrop'
$CertSubject = 'CN=mtdrop local driver signing (service)'
$DataDir = Join-Path $env:ProgramData 'mtdrop'
$BackupDir = Join-Path $DataDir 'vendor-driver-backup'
$ServiceExe = Join-Path $InstallDir 'mtdrop-service.exe'
$HardwareMatch = 'PCI\VEN_14C3&DEV_7961*'
$VendorInfName = 'mtkwl6ex.inf'
$Log = Join-Path $env:TEMP 'mtdrop-setup.log'

function Say([string]$m) {
    $line = "{0:yyyy-MM-dd HH:mm:ss} {1}" -f (Get-Date), $m
    Add-Content -Path $Log -Value $line -Encoding utf8
    Write-Output $line
}
function Fail([int]$code, [string]$m) { Say "FAILED: $m"; exit $code }

function Get-Card { Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -like $HardwareMatch } | Select-Object -First 1 }
function Get-CardProperty($card, [string]$key) { (Get-PnpDeviceProperty -InstanceId $card.InstanceId -KeyName $key -ErrorAction SilentlyContinue).Data }
function Test-VendorBound($card) {
    $provider = Get-CardProperty $card 'DEVPKEY_Device_DriverProvider'
    $inf = Get-CardProperty $card 'DEVPKEY_Device_DriverInfPath'
    ($provider -like 'MediaTek*') -and $inf -and -not (Get-ProxyPackages | Where-Object { $_ -eq $inf })
}
function Get-ProxyPackages {
    $xml = [xml]((& pnputil.exe /enum-drivers /format xml) -join "`n")
    @($xml.SelectNodes('//Driver') | Where-Object { $_.OriginalName -eq 'mtdrop_proxy.inf' } | ForEach-Object { $_.DriverName })
}
function Test-Backup { [bool](Get-ChildItem $BackupDir -Recurse -Filter $VendorInfName -ErrorAction SilentlyContinue) }
function Stop-Mtdrop {
    Get-Process -Name 'mtdrop' -ErrorAction SilentlyContinue | ForEach-Object {
        try { Stop-Process -Id $_.Id -Force; [void]$_.WaitForExit(10000); Say "stopped the tray app (pid $($_.Id))" } catch { }
    }
    $s = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
    if ($s -and $s.Status -ne 'Stopped') {
        Say 'stopping the mtdrop service (a running session ends and the Wi-Fi card goes back first; up to 2 min)'
        Stop-Service -Name $ServiceName -Force
        (Get-Service -Name $ServiceName).WaitForStatus('Stopped', [TimeSpan]::FromMinutes(3))
    }
    foreach ($name in 'radiod', 'awdld', 'mtdrop-service') {
        Get-Process -Name $name -ErrorAction SilentlyContinue | Where-Object { try { $_.Path -like "$InstallDir\*" } catch { $false } } |
            ForEach-Object { if (-not $_.WaitForExit(15000)) { Fail 1 "$name (pid $($_.Id)) still runs from $InstallDir" } }
    }
}
function Remove-Certificates {
    foreach ($store in 'Root', 'TrustedPublisher') {
        Get-ChildItem "Cert:\LocalMachine\$store" | Where-Object { $_.Subject -eq $CertSubject } |
            ForEach-Object { Remove-Item $_.PSPath; Say "removed the signing certificate from LocalMachine\$store" }
    }
}

try {
    Say "---- setup.ps1 $(if ($Check) { '-Check' } elseif ($Prepare) { '-Prepare' } elseif ($Install) { '-Install' } else { '-Uninstall' }) ($InstallDir)"

    if ($Prepare -or $Check) {
        $card = Get-Card
        if (-not $card) { Fail 10 'no MediaTek MT7921 Wi-Fi card (PCI 14C3:7961) found' }
        $provider = Get-CardProperty $card 'DEVPKEY_Device_DriverProvider'
        Say "Wi-Fi card found, driver provider: $provider"
        if (-not (Test-VendorBound $card) -and -not (Test-Backup)) { Fail 11 "the Wi-Fi card is not on the MediaTek driver ($provider) and no backup of that driver exists" }
        if (Test-VendorBound $card) {
            $inf = Get-CardProperty $card 'DEVPKEY_Device_DriverInfPath'
            $xml = [xml]((& pnputil.exe /enum-drivers /format xml) -join "`n")
            $orig = ($xml.SelectNodes('//Driver') | Where-Object { $_.DriverName -eq $inf } | Select-Object -First 1).OriginalName
            Say "MediaTek driver package: $inf ($orig)"
            if ($orig -ne $VendorInfName) { Fail 12 "the MediaTek driver package is $orig; mtdrop knows only $VendorInfName (please open an issue)" }
        }
        if ($Check) { Say 'check: ok'; exit 0 }
        Stop-Mtdrop
        Say 'prepare: ok'
        exit 0
    }

    if ($Install) {
        # 1. data folder, SYSTEM + Administrators only (the service runs as SYSTEM and keeps Wi-Fi profiles there)
        $sec = New-Object System.Security.AccessControl.DirectorySecurity
        $sec.SetAccessRuleProtection($true, $false)
        $sec.SetOwner((New-Object System.Security.Principal.SecurityIdentifier 'S-1-5-32-544'))
        foreach ($sid in 'S-1-5-18', 'S-1-5-32-544') {
            $sec.AddAccessRule((New-Object System.Security.AccessControl.FileSystemAccessRule((New-Object System.Security.Principal.SecurityIdentifier $sid),
                'FullControl', 'ContainerInherit,ObjectInherit', 'None', 'Allow')))
        }
        New-Item -ItemType Directory -Force -Path $DataDir | Out-Null
        (Get-Item $DataDir).SetAccessControl($sec)
        if (Get-ChildItem $DataDir -Force) { & icacls.exe "$DataDir\*" /reset /T /C /Q | Out-Null }
        Say "$DataDir created (SYSTEM and Administrators only)"

        # 2. backup of the MediaTek driver package: the service reinstalls it if the card does not come back by itself
        $card = Get-Card
        if ($card -and (Test-VendorBound $card)) {
            $inf = Get-CardProperty $card 'DEVPKEY_Device_DriverInfPath'
            $tmp = "$BackupDir.new"
            if (Test-Path $tmp) { Remove-Item $tmp -Recurse -Force }
            New-Item -ItemType Directory -Force -Path $tmp | Out-Null
            $out = & pnputil.exe /export-driver $inf $tmp 2>&1
            if ($LASTEXITCODE -ne 0) { Fail 1 "pnputil /export-driver $inf failed: $($out -join ' ')" }
            if (-not (Get-ChildItem $tmp -Recurse -Filter $VendorInfName)) { Fail 12 "the exported package has no $VendorInfName" }
            if (Test-Path $BackupDir) { Remove-Item $BackupDir -Recurse -Force }
            Rename-Item $tmp (Split-Path $BackupDir -Leaf)
            Say "MediaTek driver package $inf backed up to $BackupDir ($(@(Get-ChildItem $BackupDir -Recurse -File).Count) files)"
        } elseif (Test-Backup) {
            Say 'the card is not on the MediaTek driver right now; keeping the existing backup'
        } else { Fail 11 'no MediaTek driver to back up' }

        # 3. sign the proxy driver package with a certificate made here; its private key is deleted right after
        $driver = Join-Path $InstallDir 'driver'
        Remove-Certificates
        $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject $CertSubject -CertStoreLocation Cert:\CurrentUser\My `
            -KeyExportPolicy NonExportable -HashAlgorithm SHA256 -NotAfter (Get-Date).AddYears(10)
        try {
            $cer = Join-Path $driver 'mtdrop-signing.cer'
            Export-Certificate -Cert $cert -FilePath $cer | Out-Null
            foreach ($store in 'Root', 'TrustedPublisher') { Import-Certificate -FilePath $cer -CertStoreLocation "Cert:\LocalMachine\$store" | Out-Null }
            foreach ($file in 'mtdrop_proxy.cat', 'mtdrop_proxy.dll') {
                $r = Set-AuthenticodeSignature -FilePath (Join-Path $driver $file) -Certificate $cert -HashAlgorithm SHA256
                if ($r.Status -ne 'Valid') { Fail 1 "$file could not be signed: $($r.StatusMessage)" }
            }
        } finally {
            Remove-Item "Cert:\CurrentUser\My\$($cert.Thumbprint)" -DeleteKey -ErrorAction SilentlyContinue
        }
        Say "driver package signed ($($cert.Thumbprint)); private key deleted"

        # 4. the service (LocalSystem, delayed automatic start, restarts on failure)
        if (Get-Service -Name $ServiceName -ErrorAction SilentlyContinue) {
            $wmi = Get-CimInstance Win32_Service -Filter "Name='$ServiceName'"
            if ($wmi.PathName -ne "`"$ServiceExe`"") {
                $r = Invoke-CimMethod -InputObject $wmi -MethodName Change -Arguments @{ PathName = "`"$ServiceExe`"" }
                if ($r.ReturnValue -ne 0) { Fail 1 "could not update the service path ($($r.ReturnValue))" }
            }
            Say 'service registration kept'
        } else {
            New-Service -Name $ServiceName -BinaryPathName "`"$ServiceExe`"" -DisplayName 'mtdrop' -StartupType Automatic `
                -Description 'Runs AirDrop-compatible sessions for the mtdrop tray app and always gives the Wi-Fi card back to the MediaTek driver.' | Out-Null
            Say 'service registered'
        }
        & sc.exe config $ServiceName start= delayed-auto | Out-Null
        & sc.exe failure $ServiceName reset= 86400 actions= restart/5000/restart/10000/restart/60000 | Out-Null
        Start-Service -Name $ServiceName
        (Get-Service -Name $ServiceName).WaitForStatus('Running', [TimeSpan]::FromSeconds(30))
        & $ServiceExe --check 2>&1 | ForEach-Object { Say "check: $_" }
        Say 'install: ok'
        exit 0
    }

    if ($Uninstall) {
        Stop-Mtdrop
        if (Get-Service -Name $ServiceName -ErrorAction SilentlyContinue) {
            & sc.exe delete $ServiceName | Out-Null
            Say 'service deleted'
        }
        & schtasks.exe /Query /TN 'mtdrop\watchdog' 2>$null | Out-Null
        if ($LASTEXITCODE -eq 0) { & schtasks.exe /Delete /TN 'mtdrop\watchdog' /F | Out-Null; Say 'watchdog task removed' }
        try { $ts = New-Object -ComObject Schedule.Service; $ts.Connect(); $ts.GetFolder('\').DeleteFolder('mtdrop', 0) } catch { }
        Get-NetFirewallRule -DisplayName 'mtdrop-receiver-*' -ErrorAction SilentlyContinue | Remove-NetFirewallRule
        $card = Get-Card
        if ((Get-ProxyPackages).Count -gt 0 -or ($card -and -not (Test-VendorBound $card))) {
            Say 'the card is not on the MediaTek driver or a proxy package is left; giving it back'
            if (Test-Path $ServiceExe) { & $ServiceExe --give-back 2>&1 | ForEach-Object { Say "  $_" } }
            $card = Get-Card
            if ((Get-ProxyPackages).Count -gt 0 -or ($card -and -not (Test-VendorBound $card))) {
                Say "WARNING: the Wi-Fi card is still not on the MediaTek driver; $DataDir (with the driver backup) is kept"
                Remove-Certificates
                exit 1
            }
        }
        Remove-Certificates
        if (Test-Path $DataDir) { Remove-Item $DataDir -Recurse -Force; Say "$DataDir removed" }
        Say 'uninstall: ok'
        exit 0
    }

    Fail 1 'no mode given (-Check, -Prepare, -Install or -Uninstall)'
} catch {
    Fail 1 $_.Exception.Message
}
