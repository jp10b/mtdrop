# Preparation before any driver experiment (docs/DEV-SETUP.md checklist, items 1 and 2).
# Makes NO change to drivers, boot settings or network adapters:
#   1. creates a System Restore point (turns System Protection on for C: if it is off),
#   2. exports a copy of the installed MediaTek Wi-Fi driver package to a folder outside the repository.
# Must run elevated. Undo: delete the backup folder; remove the restore point in
# "System Protection" > "Configure" > "Delete" if the disk space is needed.
#
# Usage (elevated PowerShell):  .\scripts\prepare-driver-work.ps1 [-BackupRoot <folder>]

param(
    [string]$BackupRoot = (Join-Path $env:USERPROFILE 'mtdrop-private\vendor-driver-backup')
)

$ErrorActionPreference = 'Stop'
$log = Join-Path (Split-Path $BackupRoot -Parent) 'prepare-driver-work.log'
New-Item -ItemType Directory -Force -Path $BackupRoot | Out-Null
function Say([string]$m) { $line = "{0:HH:mm:ss} {1}" -f (Get-Date), $m; $line | Tee-Object -FilePath $log -Append }

try {
    $identity = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
    if (-not $identity.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Not elevated.' }
    Say "=== prepare-driver-work $(Get-Date -Format s) ==="

    # --- 1. Restore point -------------------------------------------------
    $before = @(Get-ComputerRestorePoint -ErrorAction SilentlyContinue)
    Say "restore points before: $($before.Count)"
    try { Enable-ComputerRestore -Drive "$env:SystemDrive\" ; Say 'System Protection is on for the system drive' }
    catch { Say "Enable-ComputerRestore: $($_.Exception.Message)" }
    # Windows silently skips a new point if one was made in the last 24 h; lift that for this one call.
    $key = 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\SystemRestore'
    $old = (Get-ItemProperty -Path $key -Name SystemRestorePointCreationFrequency -ErrorAction SilentlyContinue).SystemRestorePointCreationFrequency
    Set-ItemProperty -Path $key -Name SystemRestorePointCreationFrequency -Value 0 -Type DWord
    try {
        Checkpoint-Computer -Description 'mtdrop: before driver work' -RestorePointType MODIFY_SETTINGS
    } finally {
        if ($null -eq $old) { Remove-ItemProperty -Path $key -Name SystemRestorePointCreationFrequency -ErrorAction SilentlyContinue }
        else { Set-ItemProperty -Path $key -Name SystemRestorePointCreationFrequency -Value $old -Type DWord }
    }
    $after = @(Get-ComputerRestorePoint -ErrorAction SilentlyContinue)
    Say "restore points after: $($after.Count)"
    $after | Select-Object -Last 3 | ForEach-Object { Say ("  #{0} {1} {2}" -f $_.SequenceNumber, $_.CreationTime, $_.Description) }
    if ($after.Count -le $before.Count) { Say 'WARNING: no new restore point appeared' }

    # --- 2. Vendor driver backup ------------------------------------------
    $drv = Get-CimInstance Win32_PnPSignedDriver | Where-Object { $_.DeviceID -like 'PCI\VEN_14C3&DEV_7961*' } | Select-Object -First 1
    if (-not $drv) { throw 'MT7921 device not found.' }
    Say "device: $($drv.DeviceName); package: $($drv.InfName); version: $($drv.DriverVersion)"
    $dest = Join-Path $BackupRoot ("{0}-{1}" -f $drv.InfName.Replace('.inf', ''), $drv.DriverVersion)
    New-Item -ItemType Directory -Force -Path $dest | Out-Null
    $out = & pnputil.exe /export-driver $drv.InfName $dest 2>&1
    $out | ForEach-Object { Say "  pnputil: $_" }
    if ($LASTEXITCODE -ne 0) { throw "pnputil exit code $LASTEXITCODE" }
    $files = @(Get-ChildItem -Recurse -File $dest)
    Say ("exported {0} files, {1:N1} MB to {2}" -f $files.Count, (($files | Measure-Object Length -Sum).Sum / 1MB), $dest)
    foreach ($need in '*.inf', '*.sys', '*.cat') {
        if (-not ($files | Where-Object Name -like $need)) { throw "backup is missing $need" }
    }
    Say 'RESULT: OK'
}
catch {
    Say "RESULT: FAILED - $($_.Exception.Message)"
    exit 1
}
