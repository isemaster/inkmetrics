# agent.ps1 - host monitoring agent: collects host metrics and sends them to the device
#
# Settings (edit here; the device itself needs no setup):
$DeviceIP   = "192.168.7.1"      # device IP (the ADDR line on its screen)
$Interval   = 60                 # seconds between sends
$PingTarget = "8.8.8.8"          # internet check target: ICMP, then TCP 443 (ONLINE/OFFLINE)
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
    # Internet check for the ONLINE/OFFLINE frame on the device.
    # ICMP first (Test-Connection): it also gives the latency. If ICMP does not answer, the
    # internet may still be there (providers and firewalls drop echo requests), so the answer
    # is double-checked with a TCP connect to the same host on 443. Reporting "no internet"
    # for a working line is worse than a slower check.
    try {
        $r = Test-Connection -ComputerName $PingTarget -Count 1 -Quiet
        if ($r) {
            # latency: time one more echo request
            $t0 = [DateTime]::Now
            $null = Test-Connection -ComputerName $PingTarget -Count 1 -Quiet
            $ms = [math]::Round(([DateTime]::Now - $t0).TotalMilliseconds)
            return @{ ok = $true; ms = [int]$ms }
        }
    } catch { }

    try {
        $t0 = [DateTime]::Now
        $c = New-Object System.Net.Sockets.TcpClient
        $iar = $c.BeginConnect($PingTarget, 443, $null, $null)
        if ($iar.AsyncWaitHandle.WaitOne(3000, $false) -and $c.Connected) {
            $ms = [math]::Round(([DateTime]::Now - $t0).TotalMilliseconds)
            $c.Close()
            return @{ ok = $true; ms = [int]$ms }
        }
        $c.Close()
    } catch { }

    return @{ ok = $false; ms = 0 }
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
    # Returns one entry per card, in the order nvidia-smi lists them. No nvidia-smi,
    # a non-NVIDIA card or a dead driver: the list is empty and the device shows dashes
    # in both slots (an empty list is not zero load - it is "unknown").
    try {
        $smi = Get-Command nvidia-smi -ErrorAction Stop
    } catch {
        return @()
    }
    try {
        $lines = & $smi.Source --query-gpu=index,utilization.gpu,temperature.gpu,memory.used,memory.total `
                               --format=csv,noheader,nounits 2>$null
        if (-not $lines) { return @() }
        $out = @()
        foreach ($line in $lines) {
            $p = $line -split ','
            if ($p.Count -lt 5) { continue }
            $used  = [double]$p[3].Trim()
            $total = [double]$p[4].Trim()
            $memPct = if ($total -gt 0) { [math]::Round(($used / $total) * 100, 1) } else { $null }
            $out += @{
                util = [double]$p[1].Trim()
                temp = [int]$p[2].Trim()
                mem  = $memPct
            }
        }
        return $out
    } catch { }
    return @()
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
    $gpus = @(Get-Gpu)
    $g0 = if ($gpus.Count -ge 1) { $gpus[0] } else { $null }
    $g1 = if ($gpus.Count -ge 2) { $gpus[1] } else { $null }

    $obj = @{
        hostname      = $HostName
        timestamp     = (Get-Date -Format "yyyy-MM-ddTHH:mm:ss")
        cpu_percent   = $cpu
        mem_percent   = $mem
        disk_percent  = $disk
        gpu_count     = $gpus.Count
        gpu0_percent  = if ($g0) { $g0.util } else { $null }
        gpu0_temp     = if ($g0) { $g0.temp } else { $null }
        gpu0_mem_percent = if ($g0) { $g0.mem } else { $null }
        gpu1_percent  = if ($g1) { $g1.util } else { $null }
        gpu1_temp     = if ($g1) { $g1.temp } else { $null }
        gpu1_mem_percent = if ($g1) { $g1.mem } else { $null }
        ping_ok       = $ping.ok
        ping_ms       = $ping.ms
        ping_target   = $PingTarget
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
