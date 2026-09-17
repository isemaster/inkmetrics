# remove.ps1 - part of deinstall.cmd: remove the agent from this PC.
#
# Run by deinstall.cmd with administrator rights. It:
#   1. stops the running agent;
#   2. unregisters the tasks "inkmetrics agent" and "inkmetrics ICS";
#   3. turns internet sharing off for the device adapter (ics.ps1 -Off) if ics.ps1 is at hand;
#   4. reports what is left; the caller (deinstall.cmd) then deletes the install folder.
#
# ASCII ONLY: PowerShell 5.1 reads .ps1 in the system codepage, Russian text breaks parsing.
param([switch]$DryRun, [string]$InstallDir = '')
$ErrorActionPreference = 'Continue'

if (-not $InstallDir) { $InstallDir = Join-Path $env:ProgramData 'inkmetrics' }
$Here = Split-Path -Parent $MyInvocation.MyCommand.Path
$Ics  = Join-Path $Here 'ics.ps1'
if (-not (Test-Path $Ics)) { $Ics = Join-Path $InstallDir 'ics.ps1' }

function Say($m) { Write-Host ('  ' + $m) }

Say ('install folder : ' + $InstallDir + '  (' + (Test-Path $InstallDir) + ')')
foreach ($tn in @('inkmetrics agent', 'inkmetrics ICS')) {
    $t = Get-ScheduledTask -TaskName $tn -ErrorAction SilentlyContinue
    if ($t) { Say ('task "' + $tn + '" : present (' + $t.State + ')') }
    else    { Say ('task "' + $tn + '" : not registered') }
}
$pidFile = Join-Path $InstallDir 'agent.pid'
$agentPid = 0
if (Test-Path $pidFile) {
    $txt = ''
    try { $txt = (Get-Content $pidFile -Encoding UTF8 | Select-Object -First 1).Trim() } catch { }
    [void][int]::TryParse($txt, [ref]$agentPid)
    if ($agentPid -and (Get-Process -Id $agentPid -ErrorAction SilentlyContinue)) {
        Say ('agent process  : running, pid ' + $agentPid)
    } else {
        Say ('agent process  : not running (agent.pid says ' + $txt + ')')
    }
} else {
    Say 'agent process  : no agent.pid file'
}

if ($DryRun) {
    Say 'DRY RUN: would stop the agent, remove both tasks, turn the sharing off and delete the folder'
    exit 0
}

# ---------------------------------------------------------------- 1. stop the agent
$stopped = 0
$running = @(Get-CimInstance Win32_Process -Filter "Name='powershell.exe'" -ErrorAction SilentlyContinue |
             Where-Object { $_.ProcessId -ne $PID -and
                            ($_.CommandLine -like ('*' + $InstallDir + '\agent.ps1*') -or
                             $_.CommandLine -like ('*' + $InstallDir + '\ics.ps1*')) })
foreach ($r in $running) {
    try { Stop-Process -Id $r.ProcessId -Force -ErrorAction Stop; $stopped++ } catch { }
}
Say ('stopped processes : ' + $stopped)

# ---------------------------------------------------------------- 2. tasks
foreach ($tn in @('inkmetrics agent', 'inkmetrics ICS')) {
    try {
        Unregister-ScheduledTask -TaskName $tn -Confirm:$false -ErrorAction Stop
        Say ('task removed     : ' + $tn)
    } catch {
        Say ('task not present : ' + $tn)
    }
}

# ---------------------------------------------------------------- 3. sharing off
if (Test-Path $Ics) {
    Say 'turning internet sharing off:'
    & powershell -NoProfile -ExecutionPolicy Bypass -File $Ics -Off -Quiet -NoPause
} else {
    Say 'ics.ps1 not found - sharing was not touched (turn it off by hand if it is on)'
}
Say 'done.'
exit 0
