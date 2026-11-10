# Runs one PowerShell script as the SYSTEM account through a temporary scheduled task,
# waits for it to finish and removes the task. Used for measurements that need privileges
# only SYSTEM holds by default (e.g. SeLockMemoryPrivilege). Must run elevated.
#
# Usage:  .\scripts\run-as-system.ps1 -Script <path.ps1> [-Arguments '<args>'] [-TimeoutSeconds 120]

param(
    [Parameter(Mandatory)][string]$Script,
    [string]$Arguments = '',
    [int]$TimeoutSeconds = 120
)

$ErrorActionPreference = 'Stop'
$name = 'mtdrop-temp-' + [guid]::NewGuid().ToString('N').Substring(0, 8)
$action = New-ScheduledTaskAction -Execute 'powershell.exe' -Argument "-NoProfile -ExecutionPolicy Bypass -File `"$Script`" $Arguments"
$principal = New-ScheduledTaskPrincipal -UserId 'SYSTEM' -LogonType ServiceAccount -RunLevel Highest
$settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit (New-TimeSpan -Seconds ($TimeoutSeconds + 30))
Register-ScheduledTask -TaskName $name -Action $action -Principal $principal -Settings $settings | Out-Null
try {
    Start-ScheduledTask -TaskName $name
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    do { Start-Sleep -Milliseconds 500; $state = (Get-ScheduledTask -TaskName $name).State } until ($state -ne 'Running' -or (Get-Date) -gt $deadline)
    if ($state -eq 'Running') { Stop-ScheduledTask -TaskName $name; throw 'timed out' }
    $result = (Get-ScheduledTaskInfo -TaskName $name).LastTaskResult
    exit $result
}
finally {
    Unregister-ScheduledTask -TaskName $name -Confirm:$false
}
