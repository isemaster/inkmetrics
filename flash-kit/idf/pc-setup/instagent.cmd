@echo off
rem ============================================================================
rem  instagent.cmd - install the inkmetrics agent on this PC.
rem
rem  The agent code travels INSIDE this file, so the file is self-contained:
rem  copy it anywhere and run it from any folder (double-click works too).
rem  Nothing else is needed - no Python, no ESP-IDF, no other scripts.
rem
rem  What it installs into C:\ProgramData\inkmetrics (change the folder with the
rem  environment variable EINK_INSTALL_DIR if you need to):
rem      agent.ps1  the metrics agent: CPU / memory / disk / ping -> the device, every minute
rem      net.ps1    the fixed address on the device link (192.168.7.2, no gateway)
rem      setup.ps1  registers the task and starts everything
rem  Task: "inkmetrics agent" - as SYSTEM, at startup, at logon and every minute.
rem
rem  The device is a MONITOR only: no internet sharing. The addresses are fixed -
rem  the device is always 192.168.7.1, this PC gets 192.168.7.2 on the device adapter
rem  (without a gateway, so the host internet never goes into the device).
rem
rem  Usage:   instagent.cmd              install (asks for administrator rights)
rem           instagent.cmd --keep-net   do not touch the adapter address
rem           instagent.cmd --dry-run    unpack into TEMP only, change nothing
rem  Removal: deinstall.cmd
rem
rem  ASCII only on purpose: cmd.exe renders Russian text from a UTF-8 .cmd as garbage.
rem  Below the batch part there are payload sections, each starting with a rem marker
rem  line; from the first marker on the file is DATA, not batch code, so the batch part
rem  must always end with exit /b and never fall through into the payload.
rem ============================================================================
setlocal EnableExtensions
set "SELF=%~f0"
set "DRY="
set "KEEPNET="
set "DIR="
for %%A in (%*) do (
    if /i "%%A"=="--dry-run"  set "DRY=1"
    if /i "%%A"=="--keep-net" set "KEEPNET=1"
)

if defined EINK_INSTALL_DIR set "DIR=%EINK_INSTALL_DIR%"
if not defined DIR if defined DRY set "DIR=%TEMP%\inkmetrics-dry-run"
if not defined DIR set "DIR=%ProgramData%\inkmetrics"

echo.
echo   inkmetrics: agent installer (device as a monitor)
echo   install folder : %DIR%
if defined DRY echo   mode           : DRY RUN - files go to TEMP, nothing is registered
if defined KEEPNET echo   address        : left as is - the adapter is NOT touched
echo.

if not defined DRY (
    net session >nul 2>&1
    if errorlevel 1 (
        echo   Administrator rights are required - asking for them now.
        powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Process -FilePath $env:SELF -Verb RunAs"
        exit /b 0
    )
)

if not exist "%DIR%" mkdir "%DIR%" 2>nul
if not exist "%DIR%" (
    echo   FAILED: cannot create "%DIR%".
    pause
    exit /b 1
)

echo   unpacking the agent code from this file...
set "EINK_DIR=%DIR%"
powershell -NoProfile -ExecutionPolicy Bypass -Command "$t=[IO.File]::ReadAllText($env:SELF); $p=[regex]::Split($t,'rem =====PAYLOAD:([A-Za-z0-9._-]+)====='); for($i=1;$i -lt $p.Count;$i+=2){ $n=$p[$i]; $b=$p[$i+1].Replace([string][char]13,''); [IO.File]::WriteAllText((Join-Path $env:EINK_DIR $n),$b); Write-Host ('    '+$n+': '+$b.Length+' bytes') }"
if errorlevel 1 (
    echo   FAILED: cannot unpack the payload from this file.
    pause
    exit /b 1
)
if not exist "%DIR%\agent.ps1" (
    echo   FAILED: agent.ps1 was not unpacked.
    pause
    exit /b 1
)
for %%A in ("%DIR%\agent.ps1") do if %%~zA LSS 2000 (
    echo   FAILED: agent.ps1 is empty - the unpack step did not work.
    pause
    exit /b 1
)

if defined DRY (
    echo.
    echo   DRY RUN: would set the fixed address on the device adapter, stop an old agent,
    echo   register the metrics task and start it. Nothing was changed.
    echo   Unpacked files are in "%DIR%" - delete that folder by hand.
    exit /b 0
)

set "SETUPARGS="
if defined KEEPNET set "SETUPARGS=-KeepNet"

echo.
powershell -NoProfile -ExecutionPolicy Bypass -File "%DIR%\setup.ps1" %SETUPARGS%
if errorlevel 1 (
    echo.
    echo   The installer reported a problem - see the lines above.
    pause
    exit /b 1
)

echo.
echo   Agent installed. The device is used as a monitor: this PC is 192.168.7.2 on the
echo   device link, the device is always 192.168.7.1.
echo   On the device press PWR until page 5/5 HOST SYS: the CPU and memory numbers
echo   there must match this PC.
echo   Check this PC:   schtasks /query /tn "inkmetrics agent" /v /fo LIST
echo                    type "%DIR%\agent.log"
echo   Remove it again: deinstall.cmd
echo.
pause
exit /b 0
rem =====PAYLOAD:agent.ps1=====# agent.ps1 - host monitoring agent: collects host metrics and sends them to the device
#
# Settings (edit here; the device itself needs no setup):
$DeviceIP   = "192.168.7.1"      # device IP (the ADDR line on its screen)
$Interval   = 60                 # seconds between sends
$PingTarget = "8.8.8.8"          # ping target
$HostName   = $env:COMPUTERNAME  # host name shown on the device
$Port       = 80                 # ingest port on the device
$LogPath    = "C:\ProgramData\inkmetrics\agent.log"
$PidFile    = "C:\ProgramData\inkmetrics\agent.pid"
$CpuStateFile = "C:\ProgramData\inkmetrics\cpu.state"  # previous CPU sample

# ------------------------------------------------------------- single instance guard
# The task scheduler starts this script every minute; if the agent is already alive, exit.
if (Test-Path $PidFile) {
    try {
        $oldPid = [int](Get-Content $PidFile -Raw)
        if (Get-Process -Id $oldPid -ErrorAction SilentlyContinue) {
            exit 0   # agent already running
        }
    } catch { }
}
$dir = Split-Path $PidFile
if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
Set-Content -Path $PidFile -Value $PID -NoNewline

# ------------------------------------------------------------- logging
function Log($msg) {
    $line = (Get-Date -Format "yyyy-MM-dd HH:mm:ss") + " " + $msg
    try {
        Add-Content -Path $LogPath -Value $line -Encoding UTF8 -ErrorAction SilentlyContinue
    } catch { }
    Write-Host $line
}

Log "agent started (PID $PID), device=${DeviceIP}:${Port}, interval=${Interval}s"

# ------------------------------------------------------------- metrics
function Get-CpuPercent {
    # Performance counters on this machine are broken (Win32_PerfFormattedData_* -> invalid
    # class, Get-Counter -> counter missing, Win32_Processor.LoadPercentage -> empty), so the
    # load is derived from idle time: load = 100 * (1 - idle_growth / (elapsed * cores)).
    # Idle is taken from Win32_Process "System Idle Process" (kernel time, 100 ns units).
    # Idle time only ever grows, so a process exiting between two sends cannot break the
    # delta (summing all processes did exactly that and produced an empty reading).
    try {
        $idle = $null
        $ip = Get-CimInstance Win32_Process -Filter "Name='System Idle Process'" -ErrorAction Stop
        if ($ip) { $idle = [double]($ip.KernelModeTime + $ip.UserModeTime) }
        if ($idle -eq $null) { return $null }

        $cores = [Environment]::ProcessorCount
        if ($cores -lt 1) { $cores = 1 }
        $sec = [double]((Get-Date) - ([datetime]'1970-01-01')).TotalSeconds
        $inv = [Globalization.CultureInfo]::InvariantCulture

        $pct = $null
        if (Test-Path $CpuStateFile) {
            # ';' and invariant culture: on this Russian-locale host a formatted number turns
            # "85.6" into "85,6", and a comma separator would tear the record apart.
            $parts = (Get-Content $CpuStateFile -Raw) -split ';'
            if ($parts.Count -ge 2) {
                $prevIdle = [double]::Parse($parts[0].Trim(), $inv)
                $prevSec  = [double]::Parse($parts[1].Trim(), $inv)
                $dSec  = $sec - $prevSec
                $dIdle = ($idle - $prevIdle) / 10000000.0     # to seconds
                if ($dSec -gt 1 -and $dIdle -ge 0) {
                    $pct = [math]::Round(100.0 * (1.0 - $dIdle / ($dSec * $cores)), 1)
                    if ($pct -gt 100) { $pct = 100 }
                    if ($pct -lt 0) { $pct = 0 }
                }
            }
        }
        Set-Content -Path $CpuStateFile -Value ($idle.ToString($inv) + ';' + $sec.ToString($inv)) -NoNewline -ErrorAction SilentlyContinue

        if ($pct -ne $null) { return $pct }

        # No previous sample (first send after start): take a short in-process sample, so the
        # very first reading already carries a number instead of N/A. Costs ~1 s once.
        $ip2 = Get-CimInstance Win32_Process -Filter "Name='System Idle Process'" -ErrorAction SilentlyContinue
        if ($ip2) {
            Start-Sleep -Milliseconds 1000
            $ip3 = Get-CimInstance Win32_Process -Filter "Name='System Idle Process'" -ErrorAction SilentlyContinue
            if ($ip3) {
                $idle3 = [double]($ip3.KernelModeTime + $ip3.UserModeTime)
                $sec3  = [double]((Get-Date) - ([datetime]'1970-01-01')).TotalSeconds
                $dSec2 = $sec3 - $sec
                $dIdle2 = ($idle3 - $idle) / 10000000.0
                if ($dSec2 -gt 0.2 -and $dIdle2 -ge 0) {
                    $pct2 = [math]::Round(100.0 * (1.0 - $dIdle2 / ($dSec2 * $cores)), 1)
                    if ($pct2 -gt 100) { $pct2 = 100 }
                    if ($pct2 -lt 0) { $pct2 = 0 }
                    Set-Content -Path $CpuStateFile -Value ($idle3.ToString($inv) + ';' + $sec3.ToString($inv)) -NoNewline -ErrorAction SilentlyContinue
                    return $pct2
                }
            }
        }
        # Last resort: the plain counter, if it answers at all
        $lp = (Get-CimInstance Win32_Processor -ErrorAction SilentlyContinue).LoadPercentage
        if ($lp -ne $null -and "$lp" -ne "") { return [math]::Round([double]$lp, 1) }
        return $null
    } catch { return $null }
}

function Get-MemPercent {
    try {
        $os = Get-CimInstance Win32_OperatingSystem
        $total = [math]::Round($os.TotalVisibleMemorySize / 1MB, 2)
        $free  = [math]::Round($os.FreePhysicalMemory / 1MB, 2)
        if ($total -gt 0) {
            return [math]::Round((($total - $free) / $total) * 100, 1)
        }
    } catch { }
    return $null
}

function Get-DiskPercent {
    try {
        $d = Get-CimInstance Win32_LogicalDisk -Filter "DeviceID='C:'"
        if ($d.Size -gt 0) {
            return [math]::Round((($d.Size - $d.FreeSpace) / $d.Size) * 100, 1)
        }
    } catch { }
    return $null
}

function Get-Ping {
    try {
        $r = Test-Connection -ComputerName $PingTarget -Count 1 -Quiet
        if ($r) {
            # latency: time one more echo request
            $t0 = [DateTime]::Now
            $null = Test-Connection -ComputerName $PingTarget -Count 1 -Quiet
            $ms = [math]::Round(([DateTime]::Now - $t0).TotalMilliseconds)
            return @{ ok = $true; ms = [int]$ms }
        } else {
            return @{ ok = $false; ms = 0 }
        }
    } catch {
        return @{ ok = $false; ms = 0 }
    }
}

function Get-UptimeHours {
    try {
        $os = Get-CimInstance Win32_OperatingSystem
        $up = $os.LastBootUpTime
        if ($up) {
            return [math]::Round(((Get-Date) - $up).TotalHours, 1)
        }
    } catch { }
    return $null
}

function Get-TcpEstablished {
    try {
        $conns = Get-NetTCPConnection -State Established -ErrorAction SilentlyContinue
        if ($conns) { return $conns.Count }
    } catch { }
    return $null
}

function Get-CpuTemp {
    # CPU temperature is only available through LibreHardwareMonitor (if it runs)
    try {
        $sensors = Get-CimInstance -Namespace "root/LibreHardwareMonitor" -ClassName Sensor -ErrorAction Stop
        foreach ($s in $sensors) {
            if ($s.Name -like "*CPU Package*" -or $s.Name -like "*CPU Total*") {
                return [math]::Round($s.Value, 0)
            }
        }
    } catch { }
    return $null
}

function Get-Gpu {
    # GPU load / temperature / memory through nvidia-smi (if present and the driver is alive).
    # No nvidia-smi or a non-NVIDIA card: the fields are omitted and the device shows GPU N/A.
    try {
        $smi = Get-Command nvidia-smi -ErrorAction Stop
    } catch {
        return $null
    }
    try {
        $line = & $smi.Source --query-gpu=utilization.gpu,temperature.gpu,memory.used,memory.total `
                              --format=csv,noheader,nounits 2>$null | Select-Object -First 1
        if (-not $line) { return $null }
        $p = $line -split ','
        if ($p.Count -lt 4) { return $null }
        $used  = [double]$p[2].Trim()
        $total = [double]$p[3].Trim()
        $memPct = if ($total -gt 0) { [math]::Round(($used / $total) * 100, 1) } else { $null }
        return @{
            util = [double]$p[0].Trim()
            temp = [int]$p[1].Trim()
            mem  = $memPct
        }
    } catch { }
    return $null
}

function Get-SmartStatus {
    # SMART status through Get-PhysicalDisk (Windows 10+)
    try {
        $disks = Get-PhysicalDisk -ErrorAction Stop
        $worst = "OK"
        foreach ($d in $disks) {
            if ($d.HealthStatus -eq "Unhealthy") { $worst = "UNHEALTHY" }
            elseif ($d.HealthStatus -eq "Warning" -and $worst -eq "OK") { $worst = "WARNING" }
        }
        return $worst
    } catch {
        return $null
    }
}

# ------------------------------------------------------------- JSON payload
function Build-Json {
    $cpu  = Get-CpuPercent
    $mem  = Get-MemPercent
    $disk = Get-DiskPercent
    $ping = Get-Ping
    $up   = Get-UptimeHours
    $tcp  = Get-TcpEstablished
    $temp = Get-CpuTemp
    $smart = Get-SmartStatus
    $gpu  = Get-Gpu

    $obj = @{
        hostname      = $HostName
        timestamp     = (Get-Date -Format "yyyy-MM-ddTHH:mm:ss")
        cpu_percent   = $cpu
        mem_percent   = $mem
        disk_percent  = $disk
        gpu_percent   = if ($gpu) { $gpu.util } else { $null }
        gpu_temp      = if ($gpu) { $gpu.temp } else { $null }
        gpu_mem_percent = if ($gpu) { $gpu.mem } else { $null }
        ping_ok       = $ping.ok
        ping_ms       = $ping.ms
        uptime_hours  = $up
        tcp_established = $tcp
        cpu_temp      = $temp
        smart_status  = $smart
    }

    # Drop null values: the device must not fall over on missing fields
    $clean = @{}
    foreach ($k in $obj.Keys) {
        if ($obj[$k] -ne $null) { $clean[$k] = $obj[$k] }
    }
    return ($clean | ConvertTo-Json -Compress)
}

# ------------------------------------------------------------- send
function Send-To-Device {
    param([string]$json)
    $url = "http://${DeviceIP}:${Port}/ingest"
    try {
        $r = Invoke-WebRequest -Uri $url -Method POST -Body $json `
            -ContentType "application/json" -TimeoutSec 5 -UseBasicParsing
        return ($r.StatusCode -eq 200)
    } catch {
        Log ("send error: " + $_.Exception.Message)
        return $false
    }
}

# ------------------------------------------------------------- main loop
$sendCount = 0
$failCount = 0

while ($true) {
    $json = Build-Json
    $ok = Send-To-Device $json

    if ($ok) {
        $sendCount++
        $failCount = 0
        Log ("sent ok (cpu=$($json | ConvertFrom-Json | Select-Object -Expand cpu_percent)%)")
    } else {
        $failCount++
        if ($failCount -eq 1 -or $failCount % 10 -eq 0) {
            Log ("send failed ($failCount consecutive)")
        }
    }

    Start-Sleep -Seconds $Interval
}
rem =====PAYLOAD:net.ps1=====# net.ps1 - part of instagent.cmd: the address on the device link.
#
# The device works as a monitor only and always keeps 192.168.7.1 (emergency mode: its own
# DHCP server hands out addresses). If Windows takes such a lease, the lease comes WITH the
# gateway 192.168.7.1 and Windows routes the host internet into the device - the host loses
# its internet. The fix is a fixed address without a gateway, exactly what is needed here:
#
#     192.168.7.2/24, no gateway, metric 9000, DHCP off, DNS cleared
#
# The host address is then always the same (192.168.7.2) and the device always at
# 192.168.7.1, so nothing has to be looked up or negotiated.
#
# Keys:
#   (none)      set the fixed address
#   -Restore    give the address back to DHCP (undo)
#   -DryRun     only show what is there now
#
# ASCII ONLY: PowerShell 5.1 reads .ps1 in the system codepage, Russian text breaks parsing.
param([switch]$Restore, [switch]$DryRun, [switch]$Quiet)
$ErrorActionPreference = 'Continue'

$Wanted = '192.168.7.2'

function Say($m) { if (-not $Quiet) { Write-Host ('  ' + $m) } }

function Get-DeviceAdapters {
    @(Get-NetAdapter -IncludeHidden -ErrorAction SilentlyContinue | Where-Object {
        $_.InterfaceDescription -like '*NDIS*' -or $_.InterfaceDescription -like '*RNDIS*' })
}

$dev = Get-DeviceAdapters
if ($dev.Count -eq 0) {
    Say 'device adapter (RNDIS) is not enumerated - plug the device in first'
    exit 1
}

$changed = 0
foreach ($a in $dev) {
    Say ('adapter: ' + $a.Name + '  |  ' + $a.InterfaceDescription + '  |  ' + $a.Status)
    $idx = $a.ifIndex
    $ips = @((Get-NetIPAddress -InterfaceIndex $idx -AddressFamily IPv4 -ErrorAction SilentlyContinue).IPAddress)
    $got = if ($ips.Count) { $ips -join ', ' } else { 'none' }
    $dhcp = (Get-NetIPInterface -InterfaceIndex $idx -AddressFamily IPv4 -ErrorAction SilentlyContinue).Dhcp
    $met = (Get-NetIPInterface -InterfaceIndex $idx -AddressFamily IPv4 -ErrorAction SilentlyContinue).InterfaceMetric
    Say ('  now    : address ' + $got + ', dhcp ' + $dhcp + ', metric ' + $met)

    if ($DryRun) { continue }

    if ($Restore) {
        try {
            Remove-NetRoute   -InterfaceIndex $idx -Confirm:$false -ErrorAction SilentlyContinue
            Remove-NetIPAddress -InterfaceIndex $idx -Confirm:$false -ErrorAction SilentlyContinue
            Set-NetIPInterface -InterfaceIndex $idx -Dhcp Enabled -ErrorAction Stop
            Say '  result : DHCP enabled again'
            $changed++
        } catch { Say ('  dhcp on: ' + $_.Exception.Message) }
        continue
    }

    try { Set-NetIPInterface -InterfaceIndex $idx -Dhcp Disabled -ErrorAction Stop }
    catch { Say ('  dhcp off: ' + $_.Exception.Message) }
    try { Remove-NetRoute   -InterfaceIndex $idx -Confirm:$false -ErrorAction SilentlyContinue } catch { }
    try { Remove-NetIPAddress -InterfaceIndex $idx -Confirm:$false -ErrorAction SilentlyContinue } catch { }
    try {
        New-NetIPAddress -InterfaceIndex $idx -IPAddress $Wanted -PrefixLength 24 -ErrorAction Stop | Out-Null
        Say ('  set    : ' + $Wanted + '/24, no gateway')
    } catch { Say ('  set ' + $Wanted + ': ' + $_.Exception.Message) }
    try {
        Set-NetIPInterface -InterfaceIndex $idx -InterfaceMetric 9000 -ErrorAction Stop
        Say '  set    : metric 9000 (the host internet keeps priority)'
    } catch { }
    try { Set-DnsClientServerAddress -InterfaceIndex $idx -ResetServerAddresses -ErrorAction SilentlyContinue } catch { }
    $changed++
}

if ($DryRun) {
    Say 'DRY RUN: the address was not touched'
    exit 0
}

# ---- verify
foreach ($a in Get-DeviceAdapters) {
    $ips = @((Get-NetIPAddress -InterfaceIndex $a.ifIndex -AddressFamily IPv4 -ErrorAction SilentlyContinue).IPAddress)
    $gw  = @((Get-NetRoute -InterfaceIndex $a.ifIndex -AddressFamily IPv4 -DestinationPrefix '0.0.0.0/0' -ErrorAction SilentlyContinue))
    $got = if ($ips.Count) { $ips -join ', ' } else { 'none' }
    Say ('check  : ' + $a.Name + ' -> ' + $got + ', default routes on it: ' + $gw.Count +
         ' (must be 0 - otherwise the host internet can go into the device)')
    if (-not $Restore -and $ips -notcontains $Wanted) {
        Say ('WARN   : the adapter does not have ' + $Wanted + ' - set it by hand if metrics do not arrive')
    }
}
Say ('adapters changed: ' + $changed)
exit 0
rem =====PAYLOAD:setup.ps1=====# setup.ps1 - part of instagent.cmd: the fixed address and the metrics task.
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
Say 'done. On the device press PWR until page 5/5 HOST SYS - the numbers must match this PC.'
exit 0
