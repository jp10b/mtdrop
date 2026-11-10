# Spike S2 (docs/adr/0002-driver-strategy.md): can a user-mode process get locked physical
# memory below 4 GiB, which the MT7921 (32-bit DMA addresses) could use?
# Measurement only: the probe allocates, reports and frees. No driver or device is touched.
# Must run elevated (the probe needs SeLockMemoryPrivilege).

param(
    [string]$Probe = 'D:\mtdrop-tools\awe_probe\awe_probe.exe',
    [string]$Log = (Join-Path $env:USERPROFILE 'mtdrop-private\s2-awe-probe.log'),
    [int]$Runs = 3
)

$ErrorActionPreference = 'Continue'
$os = Get-CimInstance Win32_OperatingSystem
$lines = @(
    "=== S2 memory probe $(Get-Date -Format s) ===",
    ("uptime hours: {0:N1}" -f ((Get-Date) - $os.LastBootUpTime).TotalHours),
    ("RAM total GiB: {0:N1}, free GiB: {1:N1}" -f ($os.TotalVisibleMemorySize / 1MB), ($os.FreePhysicalMemory / 1MB)),
    ("holds SeLockMemoryPrivilege: {0}" -f [bool]((whoami /priv) -match 'SeLockMemoryPrivilege'))
)
for ($i = 1; $i -le $Runs; $i++) {
    $lines += "--- run $i"
    $lines += (& $Probe 2>&1 | ForEach-Object { "$_" })
    $lines += "exit code: $LASTEXITCODE"
    Start-Sleep -Seconds 2
}
$lines | Add-Content -Path $Log -Encoding utf8
$lines
