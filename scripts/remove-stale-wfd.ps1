# Removes the stale "Microsoft Wi-Fi Direct Virtual Adapter" device nodes that driver swaps leave behind (F-037).
#
# Every give-back reinstalls the MT7921 as a new network interface; the MediaTek driver then creates a new Wi-Fi
# Direct virtual adapter and the previous one stays in the device tree as a non-present ("phantom") node. After ~100
# sessions there were 103 of them. ASSUMPTION under test (F-037): the MediaTek driver's start slows down with them.
#
# WITHOUT -Apply NOTHING IS CHANGED: the script lists what it would remove.
#
#   .\scripts\remove-stale-wfd.ps1                 list (read-only, any user)
#   .\scripts\remove-stale-wfd.ps1 -Apply          remove them (elevated)
#
# Only nodes that are NOT present (no device behind them), of the Wi-Fi Direct virtual adapter enumerator, first
# installed on or after -Since, are touched; the present adapter and anything older are left alone. Removal is what
# Device Manager's "View > Show hidden devices > Uninstall device" does for a greyed-out entry. There is nothing to
# undo: Windows creates the adapter it needs again when the Wi-Fi driver starts.

param(
    [switch]$Apply,
    [datetime]$Since = '2026-11-10',
    [string]$PrivateRoot = (Join-Path $env:USERPROFILE 'mtdrop-private')
)

$ErrorActionPreference = 'Stop'
$Enumerator = '{5D624F94-8850-40C3-A3FA-A4FD2080BAF3}\VWIFIMP_WFD\*'

if ($Apply) {
    $admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    if (-not $admin) { Write-Output 'RESULT: FAIL - -Apply needs an elevated PowerShell'; exit 2 }
}

$all = @(Get-PnpDevice -Class Net -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -like $Enumerator })
$stale = @()
foreach ($d in $all) {
    if ($d.Present) { continue }
    $first = (Get-PnpDeviceProperty -InstanceId $d.InstanceId -KeyName 'DEVPKEY_Device_FirstInstallDate' -ErrorAction SilentlyContinue).Data
    if (-not $first -or [datetime]$first -lt $Since) { continue }
    $stale += [pscustomobject]@{ InstanceId = $d.InstanceId; FirstInstall = [datetime]$first }
}
Write-Output ("Wi-Fi Direct virtual adapters: {0} in total, {1} present, {2} stale since {3:yyyy-MM-dd}" -f `
    $all.Count, @($all | Where-Object Present).Count, $stale.Count, $Since)

if (-not $Apply) {
    $stale | Sort-Object FirstInstall | Group-Object { $_.FirstInstall.ToString('yyyy-MM-dd') } |
        ForEach-Object { Write-Output ("  {0}: {1}" -f $_.Name, $_.Count) }
    Write-Output 'Nothing changed. Run again with -Apply (elevated) to remove the stale ones.'
    exit 0
}

New-Item -ItemType Directory -Force -Path $PrivateRoot | Out-Null
$log = Join-Path $PrivateRoot 'driver-swap.log'
$removed = 0; $failed = 0
foreach ($s in $stale) {
    $out = & pnputil.exe /remove-device $s.InstanceId 2>&1
    if ($LASTEXITCODE -eq 0) { $removed++ } else { $failed++; Write-Output "  failed ($LASTEXITCODE): $($s.InstanceId)" }
}
$line = "{0:yyyy-MM-dd HH:mm:ss} [wfd-cleanup] removed {1} stale Wi-Fi Direct adapter node(s), {2} failed" -f (Get-Date), $removed, $failed
Add-Content -Path $log -Value $line
Write-Output $line
Write-Output ("RESULT: {0}" -f $(if ($failed -eq 0) { 'PASS' } else { 'PARTIAL' }))
