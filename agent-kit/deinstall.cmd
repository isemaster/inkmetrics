@echo off
rem ============================================================================
rem  deinstall.cmd - remove the inkmetrics agent from this PC.
rem
rem  Self-contained as well: the removal script travels inside this file, so it
rem  works from any folder. It stops the agent, removes the tasks "inkmetrics
rem  agent" and "inkmetrics ICS", turns internet sharing off and deletes the
rem  install folder (C:\ProgramData\inkmetrics, or EINK_INSTALL_DIR if that is
rem  set). Nothing else on this PC is touched.
rem
rem  Usage:   deinstall.cmd              remove everything (asks for admin rights)
rem           deinstall.cmd --dry-run    only report what is installed
rem
rem  ASCII only on purpose: cmd.exe renders Russian text from a UTF-8 .cmd as garbage.
rem  Below the batch part there are payload sections, each starting with a rem marker
rem  line; from the first marker on the file is DATA, not batch code.
rem ============================================================================
setlocal EnableExtensions
set "SELF=%~f0"
set "DRY="
set "DIR="
if /i "%~1"=="--dry-run" set "DRY=1"
if defined EINK_INSTALL_DIR set "DIR=%EINK_INSTALL_DIR%"
if not defined DIR set "DIR=%ProgramData%\inkmetrics"

echo.
echo   inkmetrics: agent removal
echo   install folder : %DIR%
echo.

set "EINK_DIR=%TEMP%\inkmetrics-remove"
if not defined DRY (
    net session >nul 2>&1
    if errorlevel 1 (
        echo   Administrator rights are required - asking for them now.
        powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Process -FilePath $env:SELF -Verb RunAs"
        exit /b 0
    )
)

if not exist "%EINK_DIR%" mkdir "%EINK_DIR%" 2>nul
echo   unpacking the removal script...
powershell -NoProfile -ExecutionPolicy Bypass -Command "$t=[IO.File]::ReadAllText($env:SELF); $p=[regex]::Split($t,'rem =====PAYLOAD:([A-Za-z0-9._-]+)====='); for($i=1;$i -lt $p.Count;$i+=2){ $n=$p[$i]; $b=$p[$i+1].Replace([string][char]13,''); [IO.File]::WriteAllText((Join-Path $env:EINK_DIR $n),$b); Write-Host ('    '+$n+': '+$b.Length+' bytes') }"
if errorlevel 1 (
    echo   FAILED: cannot unpack the payload from this file.
    pause
    exit /b 1
)

if defined DRY (
    powershell -NoProfile -ExecutionPolicy Bypass -File "%EINK_DIR%\remove.ps1" -DryRun -InstallDir "%DIR%"
    echo.
    echo   DRY RUN: nothing was changed.
    exit /b 0
)

powershell -NoProfile -ExecutionPolicy Bypass -File "%EINK_DIR%\remove.ps1" -InstallDir "%DIR%"

echo.
if exist "%DIR%" (
    echo   deleting "%DIR%" ...
    rd /s /q "%DIR%" 2>nul
)
if exist "%DIR%" (
    echo   WARN: could not delete "%DIR%" - look at it by hand
) else (
    echo   install folder removed.
)
rd /s /q "%EINK_DIR%" 2>nul

echo.
echo   Agent removed: tasks gone, sharing off, files deleted.
echo.
pause
exit /b 0
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
rem =====PAYLOAD:remove.ps1=====# remove.ps1 - part of deinstall.cmd: remove the agent from this PC.
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
