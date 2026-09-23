# remove.ps1 - part of deinstall.cmd: remove the agent from this PC.
#
# The monitor-only design has no internet sharing, so removal is short:
#   1. stops the running agent;
#   2. unregisters the task "inkmetrics agent" (and the old "inkmetrics ICS", if any);
#   3. gives the device adapter address back to DHCP (unless -KeepNet);
#   4. reports what is left; the caller (deinstall.cmd) then deletes the install folder.
#
# ASCII ONLY: PowerShell 5.1 reads .ps1 in the system codepage, Russian text breaks parsing.
param([switch]$DryRun, [switch]$KeepNet, [string]$InstallDir = '')
$ErrorActionPreference = 'Continue'

if (-not $InstallDir) { $InstallDir = Join-Path $env:ProgramData 'inkmetrics' }
$Here = Split-Path -Parent $MyInvocation.MyCommand.Path
$Net  = Join-Path $Here 'net.ps1'
if (-not (Test-Path $Net)) { $Net = Join-Path $InstallDir 'net.ps1' }

function Say($m) { Write-Host ('  ' + $m) }

Say ('install folder : ' + $InstallDir + '  (present: ' + (Test-Path $InstallDir) + ')')
# the former names (the project was inkmetrics until 23.09.2026) are listed as well, so
# that a machine that still has the old installation is cleaned up in one go
foreach ($tn in @('inkmetrics agent', 'inkmetrics ICS', 'inkmetrics agent', 'inkmetrics ICS')) {
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
if (-not $KeepNet) { Say 'address        : would go back to DHCP' }

if ($DryRun) {
    Say 'DRY RUN: would stop the agent, remove the task and give the address back to DHCP'
    exit 0
}

# ---------------------------------------------------------------- 1. stop the agent
$stopped = 0
$running = @(Get-CimInstance Win32_Process -Filter "Name='powershell.exe'" -ErrorAction SilentlyContinue |
             Where-Object { $_.ProcessId -ne $PID -and $_.CommandLine -like ('*' + $InstallDir + '\agent.ps1*') })
foreach ($r in $running) {
    try { Stop-Process -Id $r.ProcessId -Force -ErrorAction Stop; $stopped++ } catch { }
}
Say ('stopped processes : ' + $stopped)

# ---------------------------------------------------------------- 2. tasks
foreach ($tn in @('inkmetrics agent', 'inkmetrics ICS', 'inkmetrics agent', 'inkmetrics ICS')) {
    try {
        Unregister-ScheduledTask -TaskName $tn -Confirm:$false -ErrorAction Stop
        Say ('task removed      : ' + $tn)
    } catch {
        Say ('task not present  : ' + $tn)
    }
}

# ---------------------------------------------------------------- 3. address back to DHCP
if ($KeepNet) {
    Say 'address           : left as is (-KeepNet)'
} elseif (Test-Path $Net) {
    & powershell -NoProfile -ExecutionPolicy Bypass -File $Net -Restore -Quiet
} else {
    Say 'net.ps1 not found - set the device adapter back to DHCP by hand if you need to'
}
# ---------------------------------------------------------------- 4. the former folder
$OldDir = Join-Path $env:ProgramData 'inkmetrics'
if (Test-Path $OldDir) {
    Remove-Item -Recurse -Force $OldDir -ErrorAction SilentlyContinue
    Say ('former folder     : removed ' + $OldDir)
}
Say 'done.'
exit 0
