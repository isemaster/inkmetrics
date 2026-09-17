# MINSTALL.PS1 - install the host metrics agent on this PC.
#
# Entry point of the disk the device hands out: SETUP.CMD launches this file elevated.
# What it does:
#   1. stops an already running agent (it keeps its old code in memory otherwise);
#   2. copies METRICS.PS1 (the agent from this disk) to C:\ProgramData\inkmetrics\agent.ps1;
#   3. registers the scheduled task "inkmetrics agent": every 1 minute, as SYSTEM,
#      highest rights. The agent guards itself with a PID file, so a repeated start does
#      nothing while the previous process is alive;
#   4. starts the task once, so the device gets data immediately;
#   5. hands over to AGENT.PS1 (the internet-sharing setup) if it lies next to this file.
#
# Check afterwards:  schtasks /query /tn "inkmetrics agent" /v /fo LIST
#                    Get-Content C:\ProgramData\inkmetrics\agent.log -Tail 10
# On the device press PWR until page 5/5 "HOST SYS" - the numbers must match this PC.
#
# Remove the agent:  MINSTALL.PS1 -Remove   (drops the task and C:\ProgramData\inkmetrics)
#
# ASCII only: PowerShell 5.1 reads .ps1 in the system codepage and Russian text breaks it.

param([switch]$Remove)

$ErrorActionPreference = 'Continue'

$here       = Split-Path -Parent $MyInvocation.MyCommand.Path
$InstallDir = Join-Path $env:ProgramData 'inkmetrics'
$AgentSrc   = Join-Path $here 'METRICS.PS1'
$AgentDst   = Join-Path $InstallDir 'agent.ps1'
$TaskName   = 'inkmetrics agent'

function Say($m) { Write-Host $m }

$elevated = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $elevated) {
    Say 'This script needs administrator rights. Run SETUP.CMD from the device disk.'
    exit 1
}

# The kit must NOT sit inside its own install folder: the metrics agent is installed here as
# agent.ps1, while the sharing setup is a kit file named AGENT.PS1 - and on Windows those two
# names are the same file, so one silently overwrites the other (seen on a real PC 17.09).
$hereFull = ''
$dirFull  = ''
try { $hereFull = (Resolve-Path $here).Path.TrimEnd('\') } catch { $hereFull = $here.TrimEnd('\') }
try { $dirFull  = (Resolve-Path $InstallDir).Path.TrimEnd('\') } catch { $dirFull = $InstallDir.TrimEnd('\') }
if ($hereFull -ieq $dirFull) {
    Say 'ERROR: this kit lies inside its own install folder - refusing to install.'
    Say ('  folder: ' + $hereFull)
    Say '  agent.ps1 (the metrics agent) and AGENT.PS1 (internet sharing) are the same file name on'
    Say '  Windows, so installing here would overwrite the sharing setup with the agent or the other'
    Say '  way round. Copy the kit to a folder of its own (or run SETUP.CMD from the device disk)'
    Say '  and start SETUP.CMD there.'
    exit 1
}

if ($Remove) {
    Say 'inkmetrics: removing the metrics agent'
    & schtasks.exe /delete /tn $TaskName /f 2>&1 | ForEach-Object { Say ('  schtasks: ' + $_) }
    $alive = @(Get-CimInstance Win32_Process -Filter "Name='powershell.exe'" -ErrorAction SilentlyContinue |
               Where-Object { $_.CommandLine -like '*agent.ps1*' })
    foreach ($r in $alive) {
        try {
            Stop-Process -Id $r.ProcessId -Force -ErrorAction Stop
            Say ('  stopped agent: PID ' + $r.ProcessId)
        } catch { }
    }
    Remove-Item -Recurse -Force $InstallDir -ErrorAction SilentlyContinue
    Say ('  removed task "' + $TaskName + '" and folder ' + $InstallDir)
    exit 0
}

Say 'inkmetrics: installing the metrics agent'
Say ("  source : " + $AgentSrc)
Say ("  target : " + $AgentDst)
Say ("  task   : " + $TaskName)

if (-not (Test-Path $AgentSrc)) {
    Say ('ERROR: METRICS.PS1 not found next to this script (' + $here + ')')
    exit 1
}

# ---- 1. stop a running agent, otherwise it keeps the old code
$running = @(Get-CimInstance Win32_Process -Filter "Name='powershell.exe'" -ErrorAction SilentlyContinue |
             Where-Object { $_.CommandLine -like '*agent.ps1*' })
foreach ($r in $running) {
    try {
        Stop-Process -Id $r.ProcessId -Force -ErrorAction Stop
        Say ('  stopped running agent: PID ' + $r.ProcessId)
    } catch { Say ('  cannot stop PID ' + $r.ProcessId + ': ' + $_.Exception.Message) }
}
Remove-Item (Join-Path $InstallDir 'agent.pid') -Force -ErrorAction SilentlyContinue
Remove-Item (Join-Path $InstallDir 'cpu.state') -Force -ErrorAction SilentlyContinue

# ---- 2. copy the agent
New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
Copy-Item -Force $AgentSrc $AgentDst
Say ('  copied : ' + $AgentDst + ' (' + (Get-Item $AgentDst).Length + ' bytes)')

# ---- 3. scheduled task: schtasks does "every N minutes" without the trigger cmdlet quirks
& schtasks.exe /delete /tn $TaskName /f 2>&1 | Out-Null
$tr = 'powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "' + $AgentDst + '"'
$schargs = @('/create', '/tn', $TaskName, '/tr', $tr, '/sc', 'minute', '/mo', '1',
             '/ru', 'SYSTEM', '/rl', 'HIGHEST', '/f')
& schtasks.exe @schargs 2>&1 | ForEach-Object { Say ('  schtasks: ' + $_) }

$q = & schtasks.exe /query /tn $TaskName /fo LIST 2>&1
if ($LASTEXITCODE -eq 0) {
    Say '  task registered:'
    $q | ForEach-Object { Say ('    ' + $_) }
} else {
    Say '  WARN: task not found after creation'
}

# ---- 4. first run: the device starts showing metrics within a minute
& schtasks.exe /run /tn $TaskName 2>&1 | ForEach-Object { Say ('  schtasks: ' + $_) }

# ---- 5. the rest of the setup (internet sharing for the device) lives in AGENT.PS1
$ics = Join-Path $here 'AGENT.PS1'
if (Test-Path $ics) {
    Say ''
    Say 'inkmetrics: now the network part (internet sharing) - AGENT.PS1'
    & $ics
} else {
    Say ''
    Say ('note: ' + $ics + ' not found - skipped the internet-sharing part')
}

Say 'Done. Device page: the address from the ADDR line on its screen.'
