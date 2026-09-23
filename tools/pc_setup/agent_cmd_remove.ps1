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
# our tasks, plus every task that starts an agent.ps1 from another folder: those belong to an
# older installation and are removed in one go (the name of that folder does not matter)
$taskNames = @('inkmetrics agent', 'inkmetrics ICS')
Get-ScheduledTask -ErrorAction SilentlyContinue | Where-Object {
    $a = (($_.Actions | ForEach-Object { $_.Execute + ' ' + $_.Arguments }) -join ' ')
    $a -like '*agent.ps1*' -and $a -notlike ('*' + $InstallDir + '*')
} | ForEach-Object { $taskNames += $_.TaskName }
foreach ($tn in ($taskNames | Select-Object -Unique)) {
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
foreach ($tn in ($taskNames | Select-Object -Unique)) {
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
# ------------------------------------- 4. folders left over by an older installation
$here = $InstallDir.TrimEnd('\')
foreach ($d in @(Get-ChildItem $env:ProgramData -Directory -ErrorAction SilentlyContinue | Where-Object {
        $_.FullName.TrimEnd('\') -ne $here -and (Test-Path (Join-Path $_.FullName 'agent.ps1')) })) {
    Remove-Item -Recurse -Force $d.FullName -ErrorAction SilentlyContinue
    Say ('stale folder      : removed ' + $d.FullName)
}
Say 'done.'
exit 0
