# setup.ps1 - part of instagent.cmd: the fixed address and the metrics task.
#
# The device is a MONITOR only in this design: there is no internet sharing (ICS) anywhere
# in this chain. The addresses are fixed, which removes all the guessing:
#     host (this PC, device adapter) : 192.168.7.2/24, no gateway, metric 9000
#     device                         : 192.168.7.1, always (emergency mode)
#
# What it does:
#   1. stops an agent that is already running (it would keep its old code in memory);
#   2. sets the fixed address on the device adapter (net.ps1) unless -KeepNet is given;
#   3. registers the task "inkmetrics agent": at startup, at logon and every minute, as
#      SYSTEM, runs agent.ps1 (the metrics agent);
#   4. removes the task "inkmetrics ICS" if an older kit left it behind - sharing is not
#      used any more and it would move the device into another subnet;
#   5. starts the agent once and shows the journal tail.
#
# ASCII ONLY: PowerShell 5.1 reads .ps1 in the system codepage, Russian text breaks parsing.
param([switch]$DryRun, [switch]$KeepNet, [switch]$Quiet)
$ErrorActionPreference = 'Continue'

$Dir    = Split-Path -Parent $MyInvocation.MyCommand.Path
$Agent  = Join-Path $Dir 'agent.ps1'
$Net    = Join-Path $Dir 'net.ps1'
$Target = '192.168.7.2'

function Say($m) { if (-not $Quiet) { Write-Host ('  ' + $m) } }

Say ('install folder : ' + $Dir)
Say ('metrics agent  : ' + $Agent)
Say ('host address   : ' + $Target + ' (the device is always 192.168.7.1)')

if (-not (Test-Path $Agent)) {
    Say 'ERROR: agent.ps1 is not here - the unpack step failed'
    exit 1
}

if ($DryRun) {
    if ($KeepNet) {
        Say 'DRY RUN: would stop a running agent, register the metrics task and start it (address NOT touched)'
    } else {
        Say ('DRY RUN: would set ' + $Target + ' on the device adapter, stop a running agent,')
        Say '         register the metrics task and start it'
    }
    exit 0
}

# ---------------------------------------------------------------- 1. stop a running agent
$stopped = 0
$running = @(Get-CimInstance Win32_Process -Filter "Name='powershell.exe'" -ErrorAction SilentlyContinue |
             Where-Object { $_.ProcessId -ne $PID -and $_.CommandLine -like ('*' + $Dir + '\agent.ps1*') })
foreach ($r in $running) {
    try { Stop-Process -Id $r.ProcessId -Force -ErrorAction Stop; $stopped++ } catch { }
}
Remove-Item (Join-Path $Dir 'agent.pid') -Force -ErrorAction SilentlyContinue
Remove-Item (Join-Path $Dir 'cpu.state') -Force -ErrorAction SilentlyContinue
Say ('stopped running agents : ' + $stopped)

# the project carries the name inkmetrics since 23.09.2026 (it was inkmetrics until then).
# An agent installed under the former name is a long-running process with its own loop:
# killing its task is not enough, it has to be stopped by command line - otherwise every
# metric goes to the device twice.
$OldDir = Join-Path $env:ProgramData 'inkmetrics'
$old = @(Get-CimInstance Win32_Process -Filter "Name='powershell.exe'" -ErrorAction SilentlyContinue |
         Where-Object { $_.ProcessId -ne $PID -and $_.CommandLine -like ('*' + $OldDir + '\agent.ps1*') })
foreach ($r in $old) {
    try { Stop-Process -Id $r.ProcessId -Force -ErrorAction Stop; $stopped++ } catch { }
}
& schtasks.exe /delete /tn 'inkmetrics agent' /f 2>&1 | Out-Null
if (Test-Path $OldDir) {
    Remove-Item -Recurse -Force $OldDir -ErrorAction SilentlyContinue
    Say ('removed former agent    : ' + $OldDir + ' (stopped: ' + $old.Count + ')')
}

# ---------------------------------------------------------------- 2. the fixed address
if ($KeepNet) {
    Say 'address        : left as is (-KeepNet)'
} elseif (Test-Path $Net) {
    Say 'setting the address on the device link:'
    & powershell -NoProfile -ExecutionPolicy Bypass -File $Net -Quiet
    if ($LASTEXITCODE -ne 0) { Say 'WARN: the device adapter was not found (is the device plugged in?)' }
} else {
    Say 'net.ps1 is missing - set the adapter address by hand: 192.168.7.2/24, no gateway'
}

# ---------------------------------------------------------------- 3. the metrics task
$arg = '-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File ' + [char]34 + $Agent + [char]34
$action = New-ScheduledTaskAction -Execute 'powershell.exe' -Argument $arg
$t1 = New-ScheduledTaskTrigger -AtStartup
$t2 = New-ScheduledTaskTrigger -AtLogOn
$t3 = New-ScheduledTaskTrigger -Once -At (Get-Date).Date.AddMinutes(2) `
      -RepetitionInterval (New-TimeSpan -Minutes 1) -RepetitionDuration (New-TimeSpan -Days 365)
$principal = New-ScheduledTaskPrincipal -UserId 'SYSTEM' -LogonType ServiceAccount -RunLevel Highest
$settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
            -StartWhenAvailable -MultipleInstances IgnoreNew -ExecutionTimeLimit (New-TimeSpan -Minutes 5)
try {
    Register-ScheduledTask -TaskName 'inkmetrics agent' -Action $action -Trigger @($t1, $t2, $t3) `
        -Principal $principal -Settings $settings -Force `
        -Description 'inkmetrics: sends host metrics to the device, every minute' | Out-Null
    $t = Get-ScheduledTask -TaskName 'inkmetrics agent' -ErrorAction SilentlyContinue
    if ($t) { Say ('task "inkmetrics agent" : registered (' + $t.State + ')') }
    else    { Say 'WARN: task "inkmetrics agent" was not registered' }
} catch {
    Say ('ERROR registering "inkmetrics agent": ' + $_.Exception.Message)
}

# ---------------------------------------------------------------- 4. drop the old ICS task
$old = Get-ScheduledTask -TaskName 'inkmetrics ICS' -ErrorAction SilentlyContinue
if ($old) {
    try {
        Unregister-ScheduledTask -TaskName 'inkmetrics ICS' -Confirm:$false -ErrorAction Stop
        Say 'removed the old task "inkmetrics ICS" (sharing is not used any more)'
    } catch { Say 'WARN: the old task "inkmetrics ICS" is still there' }
}
$sa = Get-Service SharedAccess -ErrorAction SilentlyContinue
if ($sa -and $sa.Status -eq 'Running') {
    Say 'NOTE: the ICS service (SharedAccess) is running. If metrics do not arrive, switch sharing'
    Say '      off: device adapter - Properties - Sharing - clear "Allow other users..."'
}

# ---------------------------------------------------------------- 5. first run + journal
Say 'starting the agent once:'
& schtasks.exe /run /tn 'inkmetrics agent' 2>&1 | ForEach-Object { Say ('  schtasks: ' + $_) }
Start-Sleep -Seconds 3
$log = Join-Path $Dir 'agent.log'
if (Test-Path $log) {
    Say 'agent.log (last lines):'
    Get-Content $log -Tail 5 -Encoding UTF8 | ForEach-Object { Say ('  | ' + $_) }
} else {
    Say 'agent.log is not there yet - the first send can take up to a minute'
}
Say 'done. The top line of the device must read PING and the numbers must match this PC.'
exit 0
