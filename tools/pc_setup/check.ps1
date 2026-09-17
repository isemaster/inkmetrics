# CHECK.PS1 - diagnostics for the inkmetrics device on this PC.
#
# ASCII ONLY: PowerShell 5.1 reads .ps1 in the system codepage, Russian text breaks parsing.
# Run it by double-clicking CHECK.CMD (it pauses at the end and writes a report file).
#
# What is checked, in this order:
#   1. device on USB (VID_303A = Espressif)
#   2. the device disk: volume label INKMETRICS, its files, the required five
#   3. installed agent: C:\ProgramData\inkmetrics - files, agent.log, log totals
#   4. scheduled tasks: "inkmetrics agent" (metrics) and "inkmetrics ICS" (sharing)
#   5. the agent process itself
#   6. device address: ARP by the device MAC, then 192.168.7.1 and 192.168.137.1
#   7. the metrics path: HTTP /api/state and whether the device really receives samples.
#      By default the check only READS: it looks at ingest.count and ingest.age. With -Probe it
#      also sends one synthetic sample (CPU/RAM 0) to prove the POST path - note that the device
#      shows that probe on its screen until the real agent overwrites it, so do not read those
#      numbers as the metrics of this PC.
#
# Verdicts: [OK] works, [FAIL] broken, [WARN] cannot tell / needs a look.
# By default nothing is written anywhere: the check only reads. With -Probe it sends ONE
# synthetic sample (CPU/RAM 0) to the device to test POST /ingest - those numbers then show
# on the device screen until the real agent overwrites them, so do not read them as metrics.

param([switch]$Probe)
$ErrorActionPreference = 'Continue'

$Here   = Split-Path -Parent $MyInvocation.MyCommand.Path
$Report = Join-Path $env:TEMP 'INKMETRICS-CHECK-REPORT.TXT'
$script:Lines = New-Object System.Collections.Generic.List[string]
$script:Ok = 0
$script:Fail = 0
$script:Warn = 0
$script:Hints = New-Object System.Collections.Generic.List[string]

function Out-Line([string]$Text) {
    Write-Host $Text
    $script:Lines.Add($Text) | Out-Null
}

function Result([string]$Item, [string]$State, [string]$Note) {
    switch ($State) {
        'OK'   { $script:Ok++ }
        'FAIL' { $script:Fail++ }
        default { $script:Warn++ }
    }
    Out-Line ("[{0,-4}] {1,-36} {2}" -f $State, $Item, $Note)
}

function Hint([string]$Text) {
    $script:Hints.Add($Text) | Out-Null
}

# ---------------------------------------------------------------- header
Out-Line 'inkmetrics - check on this PC'
Out-Line ('computer : ' + $env:COMPUTERNAME + '   time: ' + (Get-Date).ToString('yyyy-MM-dd HH:mm:ss'))
$elev = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
Out-Line ('admin    : ' + $elev)
Out-Line ''

# ---------------------------------------------------------------- 1. device on USB
$pnp = @()
try {
    $pnp = @(Get-PnpDevice -PresentOnly -ErrorAction Stop |
             Where-Object { $_.InstanceId -like '*VID_303A*' })
} catch { }
if ($pnp.Count -gt 0) {
    $names = ($pnp | ForEach-Object { $_.FriendlyName }) -join '; '
    Result 'device on USB (VID_303A)' 'OK' $names
} else {
    Result 'device on USB (VID_303A)' 'FAIL' 'not present: check cable and port (USB 2.0 port on the back is best)'
    Hint 'The device is not on USB. Plug it in directly, without a hub, and replug the cable.'
}

# ---------------------------------------------------------------- 2. the device disk
# The label alone is not enough: Windows can bind a stale "floppy" node and show no volume,
# or mount the volume without a label. So we look for a drive that really carries the kit.
$disk = ''
$required = @('SETUP.CMD', 'MINSTALL.PS1', 'METRICS.PS1', 'AGENT.PS1', 'ICS.PS1')
$vols = @()
try { $vols = @(Get-Volume -ErrorAction SilentlyContinue | Where-Object { $_.DriveLetter }) } catch { }
foreach ($v in $vols) {
    $letter = [string]$v.DriveLetter + ':'
    $names = @()
    try {
        $names = @(Get-ChildItem ($letter + '\') -File -ErrorAction SilentlyContinue |
                   ForEach-Object { $_.Name })
    } catch { }
    if (($names -contains 'SETUP.CMD') -and ($names -contains 'METRICS.PS1')) { $disk = $letter; break }
}
if (-not $disk) {
    $labelled = @($vols | Where-Object { $_.FileSystemLabel -eq 'INKMETRICS' })
    if ($labelled.Count -gt 0) { $disk = [string]$labelled[0].DriveLetter + ':' }
}

if ($disk) {
    $files = @(Get-ChildItem ($disk + '\') -File -ErrorAction SilentlyContinue)
    $volinfo = @($vols | Where-Object { ([string]$_.DriveLetter + ':') -eq $disk })
    $label = ''
    if ($volinfo.Count -gt 0) { $label = 'volume ' + $volinfo[0].FileSystemLabel + ', ' }
    Result ('device disk ' + $disk) 'OK' ($label + $files.Count + ' files')
    Out-Line ('       files: ' + (($files | ForEach-Object { $_.Name + ' (' + $_.Length + ')' }) -join ', '))
    $names = @($files | ForEach-Object { $_.Name })
    $missing = @($required | Where-Object { $names -notcontains $_ })
    if ($missing.Count -eq 0) {
        Result 'disk carries the required files' 'OK' 'SETUP.CMD, MINSTALL.PS1, METRICS.PS1, AGENT.PS1, ICS.PS1'
    } else {
        Result 'disk carries the required files' 'FAIL' ('missing: ' + ($missing -join ', '))
        Hint 'The disk holds an OLD image: flash the device with the current build (flash-kit\idf\flash.bat COMx).'
    }
} else {
    Out-Line '       volumes on this PC:'
    foreach ($v in $vols) {
        Out-Line ('       ' + $v.DriveLetter + ':  ' + $v.FileSystemLabel + '  ' + $v.FileSystem +
                  '  ' + [int]($v.Size / 1MB) + ' MB  ' + $v.DriveType)
    }
    $stor = @()
    try {
        $stor = @(Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue |
                  Where-Object { ($_.InstanceId -like '*INKMETRICS*') -or ($_.FriendlyName -like '*INKMETRICS*') })
    } catch { }
    if ($stor.Count -gt 0) {
        Out-Line '       storage nodes with INKMETRICS in the name:'
        foreach ($n in $stor) { Out-Line ('       ' + $n.Status + '  ' + $n.Class + '  ' + $n.FriendlyName) }
    } else {
        Out-Line '       no node with INKMETRICS in the name (Windows sees the USB device but no storage node)'
    }
    if ($pnp.Count -gt 0) {
        Result 'device disk' 'FAIL' 'the device is on USB, but no drive with the kit files is mounted'
        Hint 'Run FIXDISK.PS1 (in this folder, as administrator): it removes the stale storage node of the device and rescans the bus. Then replug the device. If the disk still does not appear, look in Disk Management for a disk without a letter.'
    } else {
        Result 'device disk' 'WARN' 'not found (the device is not on USB either)'
    }
}

# ---------------------------------------------------------------- 3. installed agent
$Dir = 'C:\ProgramData\inkmetrics'
$log = Join-Path $Dir 'agent.log'
if (Test-Path $Dir) {
    $have = @(Get-ChildItem $Dir -File -ErrorAction SilentlyContinue | ForEach-Object { $_.Name })
    Result 'agent installed' 'OK' ('C:\ProgramData\inkmetrics: ' + ($have -join ', '))

    # The kit must NOT live inside the install folder: the metrics agent is installed AS
    # agent.ps1, and on Windows agent.ps1 and AGENT.PS1 (the sharing setup) are one and the
    # same file - unpacking the kit there makes them overwrite each other.
    $kitInside = (Test-Path (Join-Path $Dir 'MINSTALL.PS1')) -or (Test-Path (Join-Path $Dir 'SETUP.CMD'))
    if ($kitInside) {
        Result 'kit kept outside the install folder' 'FAIL' 'the kit itself lies in C:\ProgramData\inkmetrics'
        Hint 'Move the kit out of C:\ProgramData\inkmetrics (a folder of its own, or the device disk). Inside it, agent.ps1 (metrics) and AGENT.PS1 (sharing) are the same file.'
    }

    $AgentFile = Join-Path $Dir 'agent.ps1'
    if (Test-Path $AgentFile) {
        $agentHead = ''
        try { $agentHead = ((Get-Content $AgentFile -TotalCount 2 -Encoding UTF8) -join ' ') } catch { }
        $sz = 0
        try { $sz = (Get-Item $AgentFile).Length } catch { }
        if ($agentHead -match 'host monitoring agent') {
            Result 'metrics agent file (agent.ps1)' 'OK' ($sz.ToString() + ' bytes, the metrics agent')
        } else {
            Result 'metrics agent file (agent.ps1)' 'FAIL' ('holds another script (' + $sz + ' bytes) - the metrics agent was overwritten')
            Hint 'On Windows agent.ps1 (the metrics agent, written by MINSTALL.PS1) and AGENT.PS1 (the sharing setup, a kit file) are the SAME name. An older kit overwrote one with the other: run SETUP.CMD from the device disk again with the current kit.'
        }
    } else {
        Result 'metrics agent file (agent.ps1)' 'FAIL' 'missing: the task "inkmetrics agent" has nothing to start'
        Hint 'agent.ps1 is written by MINSTALL.PS1 (copied from METRICS.PS1): run SETUP.CMD from the device disk again, as administrator.'
    }
    if (-not (Test-Path (Join-Path $Dir 'ICS.PS1'))) {
        Result 'sharing script (ICS.PS1)' 'WARN' 'not in C:\ProgramData\inkmetrics: the task "inkmetrics ICS" has nothing to run'
    }

    if (Test-Path $log) {
        $age = [int]((Get-Date) - (Get-Item $log).LastWriteTime).TotalSeconds
        if ($age -lt 180) {
            Result 'agent.log is fresh' 'OK' ('last write ' + $age + ' s ago')
        } else {
            Result 'agent.log is fresh' 'FAIL' ('last write ' + $age + ' s ago - the agent is not running or cannot send')
            Hint 'agent.log is stale: look at the task check below and at the last log lines.'
        }
        Out-Line '       --- last 10 log lines ---'
        # -Encoding UTF8: the agent writes the log in UTF-8, and PowerShell 5.1 reads files
        # as ANSI by default, which turns the Russian error text into garbage
        Get-Content $log -Tail 10 -Encoding UTF8 | ForEach-Object { Out-Line ('       | ' + $_) }
        $sent = @(Select-String -Path $log -Pattern 'sent ok' -SimpleMatch -ErrorAction SilentlyContinue)
        $err  = @(Select-String -Path $log -Pattern 'send error' -SimpleMatch -ErrorAction SilentlyContinue)
        Out-Line ('       log totals: sent ok = ' + $sent.Count + ', send error = ' + $err.Count)
        if ($sent.Count -eq 0 -and $err.Count -gt 0) {
            Hint 'The agent runs but every send fails: the device is not reachable - check the network part below.'
        }
    } else {
        Result 'agent.log' 'FAIL' 'no log file: the agent has never started'
        Hint 'No agent.log means METRICS.PS1 never ran: run SETUP.CMD from the device disk again (as administrator).'
    }
} else {
    Result 'agent installed' 'FAIL' 'no C:\ProgramData\inkmetrics: SETUP.CMD / MINSTALL.PS1 did not finish'
    if ($disk) {
        $names2 = @(Get-ChildItem ($disk + '\') -File -ErrorAction SilentlyContinue | ForEach-Object { $_.Name })
        if ($names2 -notcontains 'MINSTALL.PS1') {
            Hint 'The disk has no MINSTALL.PS1 at all: it is an old image, the metrics agent is not on it.'
        } else {
            Hint 'The disk looks current: run SETUP.CMD from it again and press "Yes" on the UAC prompt.'
        }
    }
}

# ---------------------------------------------------------------- 4. scheduled tasks
foreach ($tn in @('inkmetrics agent', 'inkmetrics ICS')) {
    $t = $null
    try { $t = Get-ScheduledTask -TaskName $tn -ErrorAction Stop } catch { }
    if ($t) {
        $note = 'state=' + $t.State
        try {
            $i = Get-ScheduledTaskInfo -TaskName $tn -ErrorAction Stop
            $note = $note + ', lastRun=' + $i.LastRunTime + ', lastResult=' + $i.LastTaskResult
        } catch { }
        Result ('task "' + $tn + '"') 'OK' $note
    } else {
        Result ('task "' + $tn + '"') 'FAIL' 'not registered'
        Hint ('Task "' + $tn + '" is missing: run SETUP.CMD from the device disk again (administrator).')
    }
}

# ---------------------------------------------------------------- 5. agent process
# The agent runs as SYSTEM (scheduled task "inkmetrics agent"), and Windows hides the
# command line of another user's process: querying by command line finds nothing even
# while the agent works. So the pid file is the primary source, the command line is a backup.
$pidFile = Join-Path $Dir 'agent.pid'
$alive = $false
$aliveNote = ''
if (Test-Path $pidFile) {
    $pidText = ''
    try { $pidText = (Get-Content $pidFile -Encoding UTF8 | Select-Object -First 1).Trim() } catch { }
    $pidNum = 0
    if ([int]::TryParse($pidText, [ref]$pidNum)) {
        $p = Get-Process -Id $pidNum -ErrorAction SilentlyContinue
        if ($p) {
            $alive = $true
            $started = '(start time not readable)'
            try { $started = $p.StartTime.ToString('yyyy-MM-dd HH:mm:ss') } catch { }
            $aliveNote = 'pid ' + $pidNum + ' (' + $p.ProcessName + ', started ' + $started + ') from agent.pid'
        } else {
            $aliveNote = 'agent.pid says ' + $pidNum + ', but no such process'
        }
    } else {
        $aliveNote = 'agent.pid is not a number: ' + $pidText
    }
}
if (-not $alive) {
    $proc = @()
    try {
        # $PID is skipped on purpose: this very script lives in C:\ProgramData\inkmetrics\CHECK.PS1,
        # so its own command line contains "inkmetrics" and used to look like a running agent.
        $proc = @(Get-CimInstance Win32_Process -Filter "Name='powershell.exe'" -ErrorAction Stop |
                  Where-Object { $_.ProcessId -ne $PID -and $_.CommandLine -like '*inkmetrics\agent.ps1*' })
    } catch { }
    if ($proc.Count -gt 0) {
        $alive = $true
        $aliveNote = (($proc | ForEach-Object { 'pid ' + $_.ProcessId }) -join ', ') + ' (found by command line)'
    } elseif (-not $aliveNote) {
        $aliveNote = 'no agent.pid and no powershell process with inkmetrics in the command line'
    }
}
if ($alive) {
    Result 'agent process running' 'OK' $aliveNote
} else {
    Result 'agent process running' 'FAIL' $aliveNote
    Hint 'The agent is not running: the task "inkmetrics agent" starts it once a minute, so look at the task check.'
}

# ---------------------------------------------------------------- 6. device address
$cands = New-Object System.Collections.Generic.List[string]
$arpText = ''
try { $arpText = (& arp.exe -a) 2>&1 | Out-String } catch { }
foreach ($line in ($arpText -split "`r?`n")) {
    if ($line -match '^\s*(\d+\.\d+\.\d+\.\d+)\s+([0-9a-fA-F]{2}(-[0-9a-fA-F]{2}){5})') {
        $mac = $matches[2].ToUpper()
        if ($mac.StartsWith('70-04-1D') -or $mac.StartsWith('72-04-1D')) { $cands.Add($matches[1]) }
    }
}
foreach ($ip in @('192.168.7.1', '192.168.137.1')) { $cands.Add($ip) }

$found = ''
$foundState = $null
foreach ($ip in ($cands | Select-Object -Unique)) {
    try {
        $r = Invoke-WebRequest -Uri ('http://' + $ip + '/api/state') -TimeoutSec 3 -UseBasicParsing -ErrorAction Stop
        $found = $ip
        $foundState = $r.Content
        break
    } catch { }
}
if ($found) {
    Result ('device answers at ' + $found) 'OK' 'HTTP /api/state'
    if ($found -ne '192.168.7.1') {
        Result 'agent target address' 'WARN' ('agent.ps1 posts to 192.168.7.1, the device answers at ' + $found)
        Hint ('The agent sends to 192.168.7.1 while the device is at ' + $found +
              ': with sharing (ICS) on, the device moves into the ICS subnet and the metrics never arrive (known gap, ISSUES Z-40).')
    }
} else {
    Result 'device answers by HTTP' 'FAIL' ('no answer from: ' + (($cands | Select-Object -Unique) -join ', '))
    Hint 'Read the ADDR line on the device screen - that is its address; if the screen shows NET EMERGENCY, sharing (ICS) is off.'
}

# ---------------------------------------------------------------- 7. metrics path
if ($found) {
    $state = $null
    try { $state = $foundState | ConvertFrom-Json } catch { }
    if ($state) {
        $fw = ''
        if ($state.PSObject.Properties.Name -contains 'version') { $fw = [string]$state.version }
        Result 'device /api/state parsed' 'OK' ('firmware ' + $fw)
        if ($state.PSObject.Properties.Name -contains 'ingest') {
            $ing = $state.ingest
            $cnt = -1; $age = -1
            if ($ing.PSObject.Properties.Name -contains 'count') { $cnt = [int]$ing.count }
            if ($ing.PSObject.Properties.Name -contains 'age')   { $age = [int]$ing.age }
            if ($cnt -le 0) {
                Result 'device receives metrics' 'FAIL' 'ingest.count=0: the device has not received a single sample'
                Hint 'No sample has ever arrived: the agent on this PC is not delivering (see agent.log and the task check above).'
            } elseif ($age -le 180) {
                Result 'device receives metrics' 'OK' ('ingest.count=' + $cnt + ', age=' + $age + ' s (fresh)')
            } else {
                Result 'device receives metrics' 'FAIL' ('last sample ' + $age + ' s ago (count=' + $cnt + '): nobody is sending')
                Hint 'The device holds a stale sample. If it is the one this check sent with -Probe, it means the agent is still not sending: look at agent.log on this PC.'
            }
        } else {
            Result 'device receives metrics' 'FAIL' '/api/state has no ingest block: the firmware is older than 0.4.0'
            Hint 'Flash the current build: only firmware 0.4.0+ has POST /ingest and the HOST SYS page.'
        }

        # The probe is opt-in on purpose: it writes to the device, and its numbers (CPU/RAM 0)
        # then sit on the device screen until the real agent overwrites them.
        if ($Probe) {
            $before = $null
            try { $before = ($state.ingest.count) } catch { }
            $body = '{"cpu_percent":0.0,"mem_percent":0.0,"ping_ok":true,"ping_ms":4}'
            $post = $null
            try {
                $post = Invoke-WebRequest -Uri ('http://' + $found + '/ingest') -Method POST `
                        -ContentType 'application/json' -Body $body -TimeoutSec 4 -UseBasicParsing -ErrorAction Stop
            } catch { }
            if ($post -and [int]$post.StatusCode -eq 200) {
                Result 'POST /ingest accepted' 'OK' 'HTTP 200 (probe sample sent, CPU/RAM 0)'
            } elseif ($post) {
                Result 'POST /ingest accepted' 'FAIL' ('HTTP ' + $post.StatusCode)
            } else {
                Result 'POST /ingest accepted' 'FAIL' 'no answer (404 means firmware without /ingest)'
            }
            Start-Sleep -Seconds 1
            try {
                $r2 = Invoke-WebRequest -Uri ('http://' + $found + '/api/state') -TimeoutSec 3 -UseBasicParsing -ErrorAction Stop
                $s2 = $r2.Content | ConvertFrom-Json
                $after = $s2.ingest.count
                if ($before -ne $null -and [int]$after -gt [int]$before) {
                    Result 'counter grew after the probe' 'OK' ('ingest.count ' + $before + ' -> ' + $after)
                } else {
                    Result 'counter grew after the probe' 'WARN' ('ingest.count ' + $before + ' -> ' + $after)
                }
                Out-Line '       note: the device now shows CPU/RAM 0 on its screen - that is this probe,'
                Out-Line '       not the metrics of this PC. The agent overwrites it within a minute.'
            } catch { Result 'counter grew after the probe' 'WARN' 'cannot read /api/state again' }
        } else {
            Out-Line '       (no probe sent: the check only reads. Add -Probe to test POST /ingest.)'
        }
    } else {
        Result 'device /api/state parsed' 'FAIL' 'answer is not JSON'
    }
}

# ---------------------------------------------------------------- summary
Out-Line ''
Out-Line ('summary: OK = ' + $script:Ok + ', FAIL = ' + $script:Fail + ', WARN = ' + $script:Warn)
if ($script:Hints.Count -gt 0) {
    Out-Line 'next steps:'
    $n = 1
    foreach ($h in $script:Hints) { Out-Line ('  ' + $n + ') ' + $h); $n++ }
} else {
    Out-Line 'next steps: nothing to fix - device, agent, tasks and metrics path all look good.'
}
Out-Line ''
Out-Line 'The last proof is the device itself: press PWR until page 5/5 HOST SYS.'
Out-Line 'The CPU and memory numbers there must match this PC - those numbers come from'
Out-Line 'the agent on this machine, so a match means the whole chain works.'

$script:Lines | Set-Content -Path $Report -Encoding UTF8
Write-Host ''
Write-Host ('report file: ' + $Report)
