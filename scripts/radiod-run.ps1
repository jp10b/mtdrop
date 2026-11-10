# Runs radiod with one command and appends its output to a log file.
# Meant to be started through run-as-system.ps1 (radiod needs SeLockMemoryPrivilege).
# Only device-free commands may be run this way without the maintainer's confirmation.

param(
    [Parameter(Mandatory)][string]$Command,
    [string]$Radiod = 'D:\mtdrop-tools\build\radiod\radiod.exe',
    [string]$Log = 'C:\Users\desktop\mtdrop-private\radiod.log'
)

"=== radiod $Command  $(Get-Date -Format s)  as $([Security.Principal.WindowsIdentity]::GetCurrent().Name)" | Add-Content -Path $Log -Encoding utf8
& $Radiod $Command 2>&1 | ForEach-Object { "$_" } | Add-Content -Path $Log -Encoding utf8
"exit code: $LASTEXITCODE" | Add-Content -Path $Log -Encoding utf8
exit $LASTEXITCODE
