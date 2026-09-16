# agent_install.ps1 - install the inkmetrics helper ("agent") on this PC.
#
# ASCII ONLY: PowerShell 5.1 reads .ps1 in the system codepage, Russian text breaks parsing.
#
# What it does (run from the device's own flash drive, or from the folder in the kit):
#   1. copies the scripts to C:\ProgramData\inkmetrics\ (accessible to SYSTEM and users);
#   2. enables Internet Connection Sharing (ICS) on the device adapter - see ics_enable.ps1;
#   3. registers scheduled task "inkmetrics ICS": at startup, at logon and every minute,
#      running ics_enable.ps1 -Quiet. Windows drops ICS when networks change or after a
#      reboot, and this task brings it back without the user doing anything.
#
# Usage (administrator rights are required):
#   powershell -NoProfile -ExecutionPolicy Bypass -File agent_install.ps1
#   powershell -NoProfile -ExecutionPolicy Bypass -File agent_install.ps1 -DryRun
#   powershell -NoProfile -ExecutionPolicy Bypass -File agent_install.ps1 -Remove
param(
    [switch]$Remove,
    [switch]$DryRun
)

$ErrorActionPreference = "Continue"

$TaskName = "inkmetrics ICS"
$InstallDir = Join-Path $env:ProgramData "inkmetrics"
$SourceDir = Split-Path -Parent $MyInvocation.MyCommand.Path

function Say($m) { Write-Host $m }
function Fail($m) { Write-Host ("ERROR: " + $m); exit 1 }

$elevated = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)

Say "inkmetrics: installing the agent"
Say ("  source  : " + $SourceDir)
Say ("  target  : " + $InstallDir)
Say ("  task    : " + $TaskName)
Say ("  admin   : " + $elevated)

if ($Remove) {
    if (-not $elevated) { Fail "administrator rights are required to remove" }
    if ($DryRun) { Say "DRY RUN: would unregister task, disable ICS and remove files"; exit 0 }
    Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false -ErrorAction SilentlyContinue
    Say "task removed"
    $ics = Join-Path $SourceDir "ICS.PS1"
    if (-not (Test-Path $ics)) { $ics = Join-Path $InstallDir "ics_enable.ps1" }
    if (Test-Path $ics) { & powershell -NoProfile -ExecutionPolicy Bypass -File $ics -Off -Quiet }
    Remove-Item -Recurse -Force $InstallDir -ErrorAction SilentlyContinue
    Say "files removed"
    exit 0
}

if (-not $elevated -and -not $DryRun) {
    Fail "administrator rights are required (run SETUP.CMD, it asks for them)"
}

# ---------------------------------------------------------------- 1. copy files
$files = @("AGENT.PS1", "ICS.PS1", "NETCHECK.PS1")
if ($DryRun) {
    Say "DRY RUN: would copy to $InstallDir :"
    foreach ($f in $files) { Say ("    " + (Join-Path $SourceDir $f) + " -> " + $f) }
} else {
    New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
    foreach ($f in $files) {
        $src = Join-Path $SourceDir $f
        if (Test-Path $src) { Copy-Item -Force $src (Join-Path $InstallDir $f) }
    }
    Say ("copied: " + ((Get-ChildItem $InstallDir -File | ForEach-Object { $_.Name }) -join ", "))
}

# ---------------------------------------------------------------- 2. enable ICS now
$icsInstalled = Join-Path $InstallDir "ICS.PS1"
if (-not (Test-Path $icsInstalled)) { $icsInstalled = Join-Path $SourceDir "ICS.PS1" }
if ($DryRun) {
    Say ("DRY RUN: would run " + $icsInstalled + " -NoPause")
} else {
    if (Test-Path $icsInstalled) {
        & powershell -NoProfile -ExecutionPolicy Bypass -File $icsInstalled -NoPause
    } else {
        Say "WARN: ICS.PS1 not found next to the installer - sharing was not touched"
    }
}

# ---------------------------------------------------------------- 3. scheduled task
$action = New-ScheduledTaskAction -Execute "powershell.exe" `
    -Argument ("-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File `"" +
               (Join-Path $InstallDir "ICS.PS1") + "`" -NoPause -Quiet")

$t1 = New-ScheduledTaskTrigger -AtStartup
$t2 = New-ScheduledTaskTrigger -AtLogOn
$t3 = New-ScheduledTaskTrigger -Once -At (Get-Date).Date.AddMinutes(2) `
    -RepetitionInterval (New-TimeSpan -Minutes 1) -RepetitionDuration (New-TimeSpan -Days 365)

$principal = New-ScheduledTaskPrincipal -UserId "SYSTEM" -LogonType ServiceAccount -RunLevel Highest
$settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
    -StartWhenAvailable -MultipleInstances IgnoreNew -ExecutionTimeLimit (New-TimeSpan -Minutes 5)

if ($DryRun) {
    Say "DRY RUN: would register task '$TaskName' (startup + logon + every minute, as SYSTEM)"
    exit 0
}

Register-ScheduledTask -TaskName $TaskName -Action $action -Trigger @($t1, $t2, $t3) `
    -Principal $principal -Settings $settings -Force `
    -Description "inkmetrics: keeps Internet Connection Sharing enabled for the device adapter" | Out-Null

$task = Get-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue
if ($task) {
    Say ("task registered: " + $task.TaskName + " (" + $task.State + ")")
} else {
    Say "WARN: task was not registered"
}

Say ""
Say "Done. What next:"
Say "  * the device address is shown on its own screen, line ADDR (usually 192.168.137.x);"
Say "  * device page: http://<address>/ , settings: http://<address>/setup;"
Say "  * check from this PC: powershell -File C:\ProgramData\inkmetrics\NETCHECK.PS1 -Device <address>"
Say "  * remove: powershell -File agent_install.ps1 -Remove (from the install folder)"
exit 0
