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
rem      ics.ps1    keeps internet sharing (ICS) on for the device adapter
rem      setup.ps1  registers both tasks and starts everything
rem  Tasks: "inkmetrics agent" and "inkmetrics ICS" - as SYSTEM, at startup, at
rem  logon and every minute.
rem
rem  Usage:   instagent.cmd              install (asks for administrator rights)
rem           instagent.cmd --no-ics     install WITHOUT internet sharing: only the
rem                                      metrics agent. Use this when you want the
rem                                      numbers on the device: with sharing on the
rem                                      device moves to 192.168.137.x and the agent,
rem                                      which knows only 192.168.7.1, cannot reach it.
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
set "NOICS="
set "DIR="
for %%A in (%*) do (
    if /i "%%A"=="--dry-run" set "DRY=1"
    if /i "%%A"=="--no-ics"  set "NOICS=1"
)

if defined EINK_INSTALL_DIR set "DIR=%EINK_INSTALL_DIR%"
if not defined DIR if defined DRY set "DIR=%TEMP%\inkmetrics-dry-run"
if not defined DIR set "DIR=%ProgramData%\inkmetrics"

echo.
echo   inkmetrics: agent installer
echo   install folder : %DIR%
if defined DRY echo   mode           : DRY RUN - files go to TEMP, nothing is registered
if defined NOICS echo   sharing (ICS)  : NOT installed - metrics only
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
    echo   DRY RUN: would stop an old agent, register the tasks, turn the sharing on
    echo   and start the agent. Nothing was changed.
    echo   Unpacked files are in "%DIR%" - delete that folder by hand.
    exit /b 0
)

set "SETUPARGS="
if defined NOICS set "SETUPARGS=-NoIcs"

echo.
powershell -NoProfile -ExecutionPolicy Bypass -File "%DIR%\setup.ps1" %SETUPARGS%
if errorlevel 1 (
    echo.
    echo   The installer reported a problem - see the lines above.
    pause
    exit /b 1
)

echo.
echo   Agent installed. On the device press PWR until page 5/5 HOST SYS: the CPU and
echo   memory numbers there must match this PC.
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
rem =====PAYLOAD:ics.ps1=====# ics_enable.ps1 - turn Internet Connection Sharing (ICS) on/off for the device adapter.
#
# ASCII ONLY: PowerShell 5.1 reads .ps1 in the system codepage, Russian text breaks parsing.
#
# What it does:
#   * device adapter  = the one whose description matches "Remote NDIS" (the inkmetrics board)
#   * internet adapter = the adapter that holds the default route, skipping virtual/VPN ones
#     (on the wired PC this is the Ethernet cable, as the user confirmed ICS works on wired)
#   * enables sharing: public = internet adapter, private = device adapter
#
# Usage:
#   powershell -NoProfile -ExecutionPolicy Bypass -File ics_enable.ps1 -DryRun
#   powershell -NoProfile -ExecutionPolicy Bypass -File ics_enable.ps1
#   powershell -NoProfile -ExecutionPolicy Bypass -File ics_enable.ps1 -Off
#   (enabling/disabling needs administrator rights)
param(
    [switch]$Off,
    [switch]$DryRun,
    [switch]$Quiet,
    [switch]$NoPause,
    [string]$Public = "",
    [string]$Private = ""
)

$ErrorActionPreference = "Continue"

function Say($msg) { if (-not $Quiet) { Write-Host $msg } }
function Fail($msg) { Write-Host "ERROR: $msg"; exit 1 }

# ---------------------------------------------------------------- who is who

# device adapter: Remote NDIS (the board). Fall back to a name match.
$device = Get-NetAdapter -ErrorAction SilentlyContinue | Where-Object {
    $_.InterfaceDescription -match "Remote NDIS" -or $_.Name -match "Remote NDIS" -or
    $_.InterfaceDescription -match "RNDIS"
} | Select-Object -First 1
if (-not $device) {
    Say "device adapter (Remote NDIS) not found - device is not plugged in; nothing to do"
    exit 0
}
if (-not $Off -and $device.Status -ne "Up") {
    Fail "device adapter '$($device.Name)' is not Up (status: $($device.Status))"
}

# internet adapter: default route, skipping virtual adapters (VPN, WSL, Hyper-V, ...)
$skip = "Virtual|VPN|WSL|Hyper-V|Loopback|Bluetooth|TAP|Tunnel|Radmin|Npcap|vEthernet"
$pub = $null
if ($Public -ne "") {
    $pub = Get-NetAdapter -Name $Public -ErrorAction SilentlyContinue
    if (-not $pub) { Fail "adapter '$Public' not found (parameter -Public)" }
} else {
    $routes = Get-NetRoute -DestinationPrefix "0.0.0.0/0" -ErrorAction SilentlyContinue |
        Sort-Object RouteMetric
    foreach ($r in $routes) {
        $cand = Get-NetAdapter -Name $r.InterfaceAlias -ErrorAction SilentlyContinue
        if (-not $cand) { continue }
        if ($cand.InterfaceDescription -match $skip) { continue }
        if ($cand.Status -ne "Up") { continue }
        $pub = $cand
        break
    }
}
if (-not $pub) { Fail "no internet adapter found (no default route on a physical adapter)" }

Say ("device adapter  : " + $device.Name + " | " + $device.InterfaceDescription + " | " + $device.Status)
Say ("internet adapter: " + $pub.Name + " | " + $pub.InterfaceDescription + " | " + $pub.Status)

# ---------------------------------------------------------------- ICS via COM

$elevated = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $elevated) {
    if ($DryRun) {
        Say "NOTE: not elevated - only showing what would be done (-DryRun)"
    } else {
        Fail "administrator rights are required to enable/disable ICS"
    }
}

try {
    $mgr = New-Object -ComObject HNetCfg.HNetShare
} catch {
    Fail ("COM HNetCfg.HNetShare is not available: " + $_.Exception.Message)
}

$conns = @{}
foreach ($c in $mgr.EnumEveryConnection) {
    $p = $mgr.NetConnectionProps($c)
    $conns[$p.Name] = @{ conn = $c; name = $p.Name; device = $p.DeviceName }
}

$pubConn = $null
$privConn = $null
foreach ($k in $conns.Keys) {
    $e = $conns[$k]
    if ($e.name -eq $pub.Name -or ($e.device -ne "" -and $e.device -eq $pub.InterfaceDescription)) { $pubConn = $e }
    if ($Private -ne "" -and $e.name -eq $Private) { $privConn = $e }
    if ($e.name -eq $device.Name -or ($e.device -ne "" -and $e.device -eq $device.InterfaceDescription)) { $privConn = $e }
}
if (-not $pubConn)  { Fail ("internet adapter '" + $pub.Name + "' is not visible to ICS (COM)") }
if (-not $privConn) { Fail ("device adapter '" + $device.Name + "' is not visible to ICS (COM)") }

$cfgPub = $mgr.INetSharingConfigurationForINetConnection($pubConn.conn)
$cfgPriv = $mgr.INetSharingConfigurationForINetConnection($privConn.conn)

Say ("sharing now     : public=" + $pubConn.name + " enabled=" + $cfgPub.SharingEnabled +
     " type=" + $cfgPub.SharingConnectionType + " ; private=" + $privConn.name +
     " enabled=" + $cfgPriv.SharingEnabled + " type=" + $cfgPriv.SharingConnectionType)

if ($Off) {
    if ($DryRun) { Say "DRY RUN: would disable sharing on both adapters"; exit 0 }
    if ($cfgPriv.SharingEnabled) { $cfgPriv.DisableSharing() }
    if ($cfgPub.SharingEnabled)  { $cfgPub.DisableSharing() }
    Say "sharing disabled"
    exit 0
}

# already configured the right way? (public = internet, private = device)
if ($cfgPub.SharingEnabled -and $cfgPub.SharingConnectionType -eq 0 -and
    $cfgPriv.SharingEnabled -and $cfgPriv.SharingConnectionType -eq 1) {
    Say "already enabled correctly - nothing to do"
} else {
    if ($DryRun) {
        Say "DRY RUN: would enable sharing: public=$($pubConn.name) (type 0), private=$($privConn.name) (type 1)"
        exit 0
    }
    # order matters: clear the old configuration first, then set public, then private
    if ($cfgPub.SharingEnabled)  { $cfgPub.DisableSharing(); Start-Sleep -Seconds 2 }
    if ($cfgPriv.SharingEnabled) { $cfgPriv.DisableSharing(); Start-Sleep -Seconds 2 }
    $cfgPub.EnableSharing(0)
    Start-Sleep -Seconds 3
    $cfgPriv.EnableSharing(1)
    Say "sharing enabled: public=$($pubConn.name), private=$($privConn.name)"
}

# ---------------------------------------------------------------- wait for the lease

$addr = $null
for ($i = 0; $i -lt 25; $i++) {
    Start-Sleep -Seconds 1
    $ip = Get-NetIPAddress -InterfaceIndex $device.ifIndex -AddressFamily IPv4 -ErrorAction SilentlyContinue |
        Where-Object { $_.IPAddress -notlike "169.254.*" } | Select-Object -First 1
    if ($ip) { $addr = $ip.IPAddress; break }
}
if ($addr) {
    Say ("device address  : " + $addr)
    try {
        $r = Invoke-WebRequest -Uri ("http://" + $addr + "/api/state") -TimeoutSec 5 -UseBasicParsing
        Say ("device answers  : HTTP " + $r.StatusCode)
        Say $r.Content
    } catch {
        Say ("device answers  : no HTTP yet (" + $_.Exception.Message + ")")
    }
} else {
    Say "device address  : not received yet (check the ADDR line on the device screen)"
}

Say "shared adapter on this PC is now 192.168.137.1; device address is shown on the device screen"
exit 0

# Pause at the end so the window does not vanish when the script is started by double
# click. The scheduled task and the agent installer call it with -NoPause (nobody there
# to press Enter).
if (-not $NoPause) {
    $interactive = $true
    try { if ([Console]::IsInputRedirected) { $interactive = $false } } catch { }
    if ($interactive) {
        Write-Host ""
        try { [void](Read-Host "Press Enter to close"); } catch { }
    }
}
rem =====PAYLOAD:setup.ps1=====# setup.ps1 - part of instagent.cmd: register the agent tasks and start everything.
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
