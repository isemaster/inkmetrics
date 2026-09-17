# setup.ps1 - part of instagent.cmd: register the agent tasks and start everything.
#
# Run by instagent.cmd with administrator rights, from the folder the agent was unpacked
# into (by default C:\ProgramData\inkmetrics). It:
#   1. stops an agent that is already running (it would keep its old code in memory);
#   2. registers "inkmetrics agent": at startup, at logon and every minute, as SYSTEM,
#      runs agent.ps1 (the metrics agent);
#   3. registers "inkmetrics ICS": same triggers, runs ics.ps1 -NoPause -Quiet (Windows
#      drops internet sharing on network changes and after a reboot);
#   4. turns the sharing on right now and starts the agent once, so metrics arrive at once.
#
# ASCII ONLY: PowerShell 5.1 reads .ps1 in the system codepage, Russian text breaks parsing.
param([switch]$DryRun, [switch]$NoIcs, [switch]$Quiet)
$ErrorActionPreference = 'Continue'

$Dir   = Split-Path -Parent $MyInvocation.MyCommand.Path
$Agent = Join-Path $Dir 'agent.ps1'
$Ics   = Join-Path $Dir 'ics.ps1'

function Say($m) { if (-not $Quiet) { Write-Host ('  ' + $m) } }

Say ('install folder : ' + $Dir)
Say ('metrics agent  : ' + $Agent)

if (-not (Test-Path $Agent)) {
    Say 'ERROR: agent.ps1 is not here - the unpack step failed'
    exit 1
}

if ($DryRun) {
    if ($NoIcs) {
        Say 'DRY RUN: would stop a running agent, register the metrics task and start it (sharing NOT touched)'
    } else {
        Say 'DRY RUN: would stop a running agent, register both tasks, turn the sharing on and start'
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
Remove-Item (Join-Path $Dir 'agent.pid')  -Force -ErrorAction SilentlyContinue
Remove-Item (Join-Path $Dir 'cpu.state')  -Force -ErrorAction SilentlyContinue
Say ('stopped running agents : ' + $stopped)

# ---------------------------------------------------------------- 2. tasks
$quote = [char]34

function Register-OurTask([string]$name, [string]$script, [string]$extra, [string]$desc) {
    $arg = '-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File ' + $quote + $script + $quote + $extra
    $action = New-ScheduledTaskAction -Execute 'powershell.exe' -Argument $arg
    $t1 = New-ScheduledTaskTrigger -AtStartup
    $t2 = New-ScheduledTaskTrigger -AtLogOn
    $t3 = New-ScheduledTaskTrigger -Once -At (Get-Date).Date.AddMinutes(2) `
          -RepetitionInterval (New-TimeSpan -Minutes 1) -RepetitionDuration (New-TimeSpan -Days 365)
    $principal = New-ScheduledTaskPrincipal -UserId 'SYSTEM' -LogonType ServiceAccount -RunLevel Highest
    $settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
                -StartWhenAvailable -MultipleInstances IgnoreNew -ExecutionTimeLimit (New-TimeSpan -Minutes 5)
    try {
        Register-ScheduledTask -TaskName $name -Action $action -Trigger @($t1, $t2, $t3) `
            -Principal $principal -Settings $settings -Force -Description $desc | Out-Null
        $t = Get-ScheduledTask -TaskName $name -ErrorAction SilentlyContinue
        if ($t) { Say ('task "' + $name + '" : registered (' + $t.State + ')') }
        else    { Say ('WARN: task "' + $name + '" was not registered') }
    } catch {
        Say ('ERROR registering "' + $name + '": ' + $_.Exception.Message)
    }
}

Register-OurTask 'inkmetrics agent' $Agent '' 'inkmetrics: sends host metrics to the device, every minute'
if (-not $NoIcs -and (Test-Path $Ics)) {
    Register-OurTask 'inkmetrics ICS' $Ics ' -NoPause -Quiet' 'inkmetrics: keeps internet sharing on for the device adapter'
}

# ---------------------------------------------------------------- 3. sharing on, first run
if (-not $NoIcs -and (Test-Path $Ics)) {
    Say 'turning internet sharing on:'
    & powershell -NoProfile -ExecutionPolicy Bypass -File $Ics -NoPause -Quiet
}

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
Say 'done. On the device press PWR until page 5/5 HOST SYS - the numbers must match this PC.'
exit 0
